#include "fidokey.h"

#include <fido.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <cstring>
#include <mutex>

namespace zfsmgr::cli::fido {
namespace {

std::once_flag g_init;
std::mutex g_mutex;
fido_dev_t* g_current = nullptr;  // the one being waited on, for cancel()
bool g_cancelled = false;

void init() {
    std::call_once(g_init, [] { fido_init(0); });
}

Result resultOf(int r, std::string& detail) {
    switch (r) {
        case FIDO_OK:
            return Result::Ok;
        case FIDO_ERR_PIN_REQUIRED:
        case FIDO_ERR_UV_INVALID:
            return Result::PinRequired;
        case FIDO_ERR_PIN_INVALID:
            return Result::PinInvalid;
        case FIDO_ERR_PIN_BLOCKED:
        case FIDO_ERR_PIN_AUTH_BLOCKED:
        case FIDO_ERR_UV_BLOCKED:
            return Result::PinBlocked;
        case FIDO_ERR_NO_CREDENTIALS:
            return Result::NotRegistered;
        case FIDO_ERR_UNSUPPORTED_EXTENSION:
        case FIDO_ERR_UNSUPPORTED_OPTION:
            return Result::Unsupported;
        // What a key answers when it was not touched in time: a YubiKey says «operation
        // denied» after ~30 s, other keys «action timeout».
        case FIDO_ERR_OPERATION_DENIED:
        case FIDO_ERR_USER_ACTION_TIMEOUT:
        case FIDO_ERR_ACTION_TIMEOUT:
            return Result::NotTouched;
        case FIDO_ERR_KEEPALIVE_CANCEL:
            return Result::Cancelled;
        default:
            detail = fido_strerr(r);
            return Result::Failed;
    }
}

// Opens the first key connected. Registered in g_current while it is in use.
fido_dev_t* openFirst(std::string& product, bool& hello) {
    init();
    fido_dev_info_t* list = fido_dev_info_new(16);
    size_t n = 0;
    fido_dev_t* dev = nullptr;
    if (list && fido_dev_info_manifest(list, 16, &n) == FIDO_OK) {
        for (size_t i = 0; i < n && !dev; ++i) {
            const fido_dev_info_t* di = fido_dev_info_ptr(list, i);
            fido_dev_t* d = fido_dev_new();
            if (d && fido_dev_open(d, fido_dev_info_path(di)) == FIDO_OK) {
                dev = d;
                product = fido_dev_info_product_string(di) ? fido_dev_info_product_string(di) : "";
                hello = fido_dev_is_winhello(d);
            } else if (d) {
                fido_dev_free(&d);
            }
        }
    }
    if (list) {
        fido_dev_info_free(&list, 16);
    }
    if (dev) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_current = dev;
        g_cancelled = false;
    }
    return dev;
}

void closeDev(fido_dev_t*& dev) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_current == dev) {
            g_current = nullptr;
        }
    }
    fido_dev_close(dev);
    fido_dev_free(&dev);
}

bool cancelledNow() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_cancelled;
}

// The client data hash is not checked by anyone here —there is no server—, but it has to be
// there: random every time.
void randomHash(unsigned char (&h)[32]) {
    RAND_bytes(h, sizeof h);
}

Result getSecret(fido_dev_t* dev, const std::vector<std::string>& ids, const std::string& salt,
                 const std::string& pin, std::string& used, std::string& out, std::string& detail) {
    fido_assert_t* a = fido_assert_new();
    unsigned char cdh[32];
    randomHash(cdh);
    fido_assert_set_clientdata_hash(a, cdh, sizeof cdh);
    fido_assert_set_rp(a, kRelyingParty);
    for (const std::string& id : ids) {
        fido_assert_allow_cred(a, reinterpret_cast<const unsigned char*>(id.data()), id.size());
    }
    fido_assert_set_extensions(a, FIDO_EXT_HMAC_SECRET);
    fido_assert_set_hmac_salt(a, reinterpret_cast<const unsigned char*>(salt.data()), salt.size());
    fido_assert_set_up(a, FIDO_OPT_TRUE);
    const int r = fido_dev_get_assert(dev, a, pin.empty() ? nullptr : pin.c_str());
    Result res = cancelledNow() ? Result::Cancelled : resultOf(r, detail);
    if (res == Result::Ok) {
        const size_t n = fido_assert_hmac_secret_len(a, 0);
        if (n != 32) {
            res = Result::Unsupported;
        } else {
            out.assign(reinterpret_cast<const char*>(fido_assert_hmac_secret_ptr(a, 0)), n);
            used.assign(reinterpret_cast<const char*>(fido_assert_id_ptr(a, 0)),
                        fido_assert_id_len(a, 0));
            // With one credential allowed a key may omit its id; it is that one, then.
            if (used.empty() && ids.size() == 1) {
                used = ids[0];
            }
        }
    }
    fido_assert_free(&a);
    return res;
}

}  // namespace

std::vector<Device> devices() {
    init();
    std::vector<Device> out;
    fido_dev_info_t* list = fido_dev_info_new(16);
    size_t n = 0;
    if (list && fido_dev_info_manifest(list, 16, &n) == FIDO_OK) {
        for (size_t i = 0; i < n; ++i) {
            const fido_dev_info_t* di = fido_dev_info_ptr(list, i);
            Device d;
            d.path = fido_dev_info_path(di);
            d.product = fido_dev_info_product_string(di) ? fido_dev_info_product_string(di) : "";
            d.windowsHello = d.path.rfind("windows://", 0) == 0;
            out.push_back(d);
        }
    }
    if (list) {
        fido_dev_info_free(&list, 16);
    }
    return out;
}

Result registerKey(const std::string& salt, const std::string& pin, std::string& credentialId,
                   bool& resident, std::string& secret, std::string& product, std::string& detail) {
    bool hello = false;
    fido_dev_t* dev = openFirst(product, hello);
    if (!dev) {
        return Result::NoDevice;
    }
    // Windows Hello ignores hmac-secret unless the credential is stored in the key.
    resident = hello;
    fido_cred_t* c = fido_cred_new();
    unsigned char cdh[32];
    randomHash(cdh);
    unsigned char user[16];
    RAND_bytes(user, sizeof user);
    fido_cred_set_type(c, COSE_ES256);
    fido_cred_set_clientdata_hash(c, cdh, sizeof cdh);
    fido_cred_set_rp(c, kRelyingParty, "ZFSMgr");
    fido_cred_set_user(c, user, sizeof user, "zfsmgr", "ZFSMgr", nullptr);
    fido_cred_set_extensions(c, FIDO_EXT_HMAC_SECRET);
    fido_cred_set_rk(c, resident ? FIDO_OPT_TRUE : FIDO_OPT_FALSE);
    const int r = fido_dev_make_cred(dev, c, pin.empty() ? nullptr : pin.c_str());
    Result res = cancelledNow() ? Result::Cancelled : resultOf(r, detail);
    if (res == Result::Ok) {
        credentialId.assign(reinterpret_cast<const char*>(fido_cred_id_ptr(c)),
                            fido_cred_id_len(c));
    }
    fido_cred_free(&c);
    if (res == Result::Ok) {
        std::string used;
        res = getSecret(dev, {credentialId}, salt, pin, used, secret, detail);
    }
    closeDev(dev);
    return res;
}

Result secret(const std::vector<std::string>& credentialIds, const std::string& salt,
              const std::string& pin, std::string& used, std::string& out, std::string& detail) {
    std::string product;
    bool hello = false;
    fido_dev_t* dev = openFirst(product, hello);
    if (!dev) {
        return Result::NoDevice;
    }
    const Result res = getSecret(dev, credentialIds, salt, pin, used, out, detail);
    closeDev(dev);
    return res;
}

void cancel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cancelled = true;
    if (g_current) {
        fido_dev_cancel(g_current);
    }
}

}  // namespace zfsmgr::cli::fido
