#include "tlsserver.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace zfsmgr::base::tlsserver {

bool writeSelfSignedPair(const std::string& rutaCert, const std::string& rutaClave,
                           const std::string& commonName, bool paraServidor,
                           const std::string& altNames, std::string& error) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) {
        error = "could not generate the RSA key";
        return false;
    }
    X509* x = X509_new();
    if (!x) {
        EVP_PKEY_free(pkey);
        error = "could not create the certificate";
        return false;
    }
    X509_set_version(x, 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 60L * 60 * 24 * 3650);
    X509_set_pubkey(x, pkey);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1, 0);
    X509_set_issuer_name(x, name);  // autofirmado: emisor = sujeto

    auto addExt = [&](int nid, const char* value) {
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
        X509_EXTENSION* ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
        if (ex) {
            X509_add_ext(x, ex, -1);
            X509_EXTENSION_free(ex);
        }
    };
    addExt(NID_basic_constraints, "critical,CA:TRUE");
    addExt(NID_subject_alt_name, altNames.c_str());
    addExt(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
    addExt(NID_ext_key_usage, paraServidor ? "serverAuth" : "clientAuth");

    if (!X509_sign(x, pkey, EVP_sha256())) {
        X509_free(x);
        EVP_PKEY_free(pkey);
        error = "could not sign the certificate";
        return false;
    }

    bool ok = false;
    if (FILE* cf = std::fopen(rutaCert.c_str(), "wb")) {
        ok = PEM_write_X509(cf, x) == 1;
        std::fclose(cf);
    }
    if (ok) {
        ok = false;
        if (FILE* kf = std::fopen(rutaClave.c_str(), "wb")) {
            ok = PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
            std::fclose(kf);
        }
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    if (!ok) {
        error = "could not write the PEM files";
        return false;
    }
#ifndef _WIN32
    ::chmod(rutaCert.c_str(), 0600);
    ::chmod(rutaClave.c_str(), 0600);
#endif
    return true;
}

}  // namespace zfsmgr::base::tlsserver
