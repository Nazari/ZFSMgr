#include "fsmeta.h"

#include <cstring>
#include <sstream>
#include <vector>

#include <openssl/evp.h>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#elif defined(__FreeBSD__)
#include <sys/extattr.h>
#endif
#if defined(__FreeBSD__) || defined(__APPLE__)
#include <sys/acl.h>
#endif
#endif

namespace zfsmgr::fsmeta {

Family localFamily() {
#if defined(_WIN32)
    return Family::Windows;
#elif defined(__APPLE__)
    return Family::Darwin;
#elif defined(__FreeBSD__)
    return Family::FreeBSD;
#elif defined(__linux__)
    return Family::Linux;
#else
    return Family::Other;
#endif
}

const char* nameOf(Family f) {
    switch (f) {
        case Family::Linux: return "linux";
        case Family::FreeBSD: return "freebsd";
        case Family::Darwin: return "darwin";
        case Family::Windows: return "windows";
        case Family::Other: return "other";
    }
    return "other";
}

Family familyFromName(const std::string& n) {
    for (const Family f : {Family::Linux, Family::FreeBSD, Family::Darwin, Family::Windows}) {
        if (n == nameOf(f)) {
            return f;
        }
    }
    return Family::Other;
}

bool isLocalOnly(const std::string& name) {
    return name.rfind("security.selinux", 0) == 0 || name == "com.apple.provenance";
}

namespace {

bool startsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

#ifndef _WIN32
#if defined(__linux__) || defined(__APPLE__)
#if defined(__APPLE__)
ssize_t xList(const char* p, char* buf, std::size_t sz) {
    return ::listxattr(p, buf, sz, XATTR_NOFOLLOW);
}
ssize_t xGet(const char* p, const char* n, void* v, std::size_t sz) {
    return ::getxattr(p, n, v, sz, 0, XATTR_NOFOLLOW);
}
int xSet(const char* p, const char* n, const void* v, std::size_t sz) {
    return ::setxattr(p, n, v, sz, 0, XATTR_NOFOLLOW);
}
int xDel(const char* p, const char* n) {
    return ::removexattr(p, n, XATTR_NOFOLLOW);
}
#else
ssize_t xList(const char* p, char* buf, std::size_t sz) {
    return ::llistxattr(p, buf, sz);
}
ssize_t xGet(const char* p, const char* n, void* v, std::size_t sz) {
    return ::lgetxattr(p, n, v, sz);
}
int xSet(const char* p, const char* n, const void* v, std::size_t sz) {
    return ::lsetxattr(p, n, v, sz, 0);
}
int xDel(const char* p, const char* n) {
    return ::lremovexattr(p, n);
}
#endif

Xattrs readXattrs(const std::string& p) {
    Xattrs out;
    const ssize_t listLen = xList(p.c_str(), nullptr, 0);
    if (listLen <= 0) {
        return out;
    }
    std::vector<char> names(static_cast<std::size_t>(listLen));
    const ssize_t got = xList(p.c_str(), names.data(), names.size());
    std::size_t off = 0;
    while (got > 0 && off < static_cast<std::size_t>(got)) {
        const std::string name(names.data() + off);
        off += name.size() + 1;
        if (name.empty()) {
            break;
        }
        if (isLocalOnly(name)) {
            continue;
        }
        const ssize_t valLen = xGet(p.c_str(), name.c_str(), nullptr, 0);
        if (valLen < 0) {
            continue;
        }
        std::string value(static_cast<std::size_t>(valLen), '\0');
        if (valLen > 0 && xGet(p.c_str(), name.c_str(), value.data(), value.size()) != valLen) {
            continue;
        }
        out.emplace(name, std::move(value));
    }
    return out;
}

bool setXattr(const std::string& p, const std::string& name, const std::string& value) {
    return xSet(p.c_str(), name.c_str(), value.data(), value.size()) == 0;
}

bool removeXattr(const std::string& p, const std::string& name) {
    return xDel(p.c_str(), name.c_str()) == 0;
}

#elif defined(__FreeBSD__)
struct ExtattrNamespace {
    int ns;
    const char* prefix;
};
constexpr ExtattrNamespace kNamespaces[] = {{EXTATTR_NAMESPACE_USER, "user."},
                                           {EXTATTR_NAMESPACE_SYSTEM, "system."}};

Xattrs readXattrs(const std::string& p) {
    Xattrs out;
    for (const auto& ns : kNamespaces) {
        const ssize_t listLen = ::extattr_list_link(p.c_str(), ns.ns, nullptr, 0);
        if (listLen <= 0) {
            continue;
        }
        // Formato de extattr: cada nombre va precedido de un byte con su largo, sin '\0'.
        std::vector<unsigned char> buf(static_cast<std::size_t>(listLen));
        const ssize_t got = ::extattr_list_link(p.c_str(), ns.ns, buf.data(), buf.size());
        std::size_t off = 0;
        while (got > 0 && off < static_cast<std::size_t>(got)) {
            const std::size_t len = buf[off];
            const std::string name(reinterpret_cast<const char*>(buf.data()) + off + 1, len);
            off += len + 1;
            const ssize_t valLen = ::extattr_get_link(p.c_str(), ns.ns, name.c_str(), nullptr, 0);
            if (valLen < 0) {
                continue;
            }
            std::string value(static_cast<std::size_t>(valLen), '\0');
            if (valLen > 0
                && ::extattr_get_link(p.c_str(), ns.ns, name.c_str(), value.data(), value.size())
                       != valLen) {
                continue;
            }
            out.emplace(std::string(ns.prefix) + name, std::move(value));
        }
    }
    return out;
}

bool splitName(const std::string& full, int& ns, std::string& name) {
    for (const auto& n : kNamespaces) {
        if (startsWith(full, n.prefix)) {
            ns = n.ns;
            name = full.substr(std::strlen(n.prefix));
            return true;
        }
    }
    return false;
}

bool setXattr(const std::string& p, const std::string& full, const std::string& value) {
    int ns = 0;
    std::string name;
    return splitName(full, ns, name)
           && ::extattr_set_link(p.c_str(), ns, name.c_str(), value.data(), value.size())
                  == static_cast<ssize_t>(value.size());
}

bool removeXattr(const std::string& p, const std::string& full) {
    int ns = 0;
    std::string name;
    return splitName(full, ns, name) && ::extattr_delete_link(p.c_str(), ns, name.c_str()) == 0;
}
#else
Xattrs readXattrs(const std::string&) { return {}; }
bool setXattr(const std::string&, const std::string&, const std::string&) { return false; }
bool removeXattr(const std::string&, const std::string&) { return false; }
#endif
#else  // _WIN32
Xattrs readXattrs(const std::string&) { return {}; }
bool setXattr(const std::string&, const std::string&, const std::string&) { return false; }
bool removeXattr(const std::string&, const std::string&) { return false; }
#endif

// ── ACL que NO son atributos extendidos ─────────────────────────────────────────────────
#if !defined(_WIN32) && (defined(__FreeBSD__) || defined(__APPLE__))
#if defined(__FreeBSD__)
constexpr acl_type_t kAclType = ACL_TYPE_NFS4;
constexpr const char* kAclKind = "nfs4";
#else
constexpr acl_type_t kAclType = ACL_TYPE_EXTENDED;
constexpr const char* kAclKind = "darwin";
#endif

std::string textOf(acl_t a) {
    ssize_t len = 0;
    char* text = ::acl_to_text(a, &len);
    std::string out = text != nullptr ? std::string(text, static_cast<std::size_t>(len)) : "";
    if (text != nullptr) {
        ::acl_free(text);
    }
    return out;
}

Acl readAcl(const std::string& p) {
    Acl out;
    acl_t a = ::acl_get_link_np(p.c_str(), kAclType);
    if (a == nullptr) {
        return out;  // sin ACL (macOS), o un sistema de ficheros que no las tiene
    }
#if defined(__FreeBSD__)
    // La TRIVIAL —la que ZFS da a todo fichero— no es más que el modo: cuenta como ninguna.
    int trivial = 0;
    if (::acl_is_trivial_np(a, &trivial) == 0 && trivial != 0) {
        ::acl_free(a);
        return out;
    }
#endif
    out.kind = kAclKind;
    out.text = textOf(a);
    ::acl_free(a);
    return out;
}

bool setAclImpl(const std::string& p, const Acl& acl) {
    if (acl.kind.empty()) {
        // Quitar la que haya: en FreeBSD se reduce a la trivial, en macOS se pone una vacía.
        if (readAcl(p).kind.empty()) {
            return true;
        }
#if defined(__FreeBSD__)
        acl_t cur = ::acl_get_link_np(p.c_str(), kAclType);
        if (cur == nullptr) {
            return false;
        }
        acl_t strip = ::acl_strip_np(cur, 0);
        ::acl_free(cur);
        if (strip == nullptr) {
            return false;
        }
        const bool ok = ::acl_set_link_np(p.c_str(), kAclType, strip) == 0;
        ::acl_free(strip);
        return ok;
#else
        acl_t vacia = ::acl_init(0);
        if (vacia == nullptr) {
            return false;
        }
        const bool ok = ::acl_set_link_np(p.c_str(), kAclType, vacia) == 0;
        ::acl_free(vacia);
        return ok;
#endif
    }
    if (acl.kind != kAclKind) {
        return false;
    }
    acl_t a = ::acl_from_text(acl.text.c_str());
    if (a == nullptr) {
        return false;
    }
    const bool ok = ::acl_set_link_np(p.c_str(), kAclType, a) == 0;
    ::acl_free(a);
    return ok;
}
#else
Acl readAcl(const std::string&) { return {}; }
bool setAclImpl(const std::string&, const Acl& acl) { return acl.kind.empty(); }
#endif

}  // namespace

Metadata read(const std::string& path) {
    Metadata m;
    m.xattrs = readXattrs(path);
    m.acl = readAcl(path);
    return m;
}

int syncXattrs(const std::string& path, const Xattrs& want) {
    int failed = 0;
    const Xattrs have = readXattrs(path);
    for (const auto& kv : have) {
        if (want.find(kv.first) == want.end() && !removeXattr(path, kv.first)) {
            ++failed;
        }
    }
    for (const auto& kv : want) {
        if (isLocalOnly(kv.first)) {
            continue;
        }
        const auto it = have.find(kv.first);
        if ((it == have.end() || it->second != kv.second) && !setXattr(path, kv.first, kv.second)) {
            ++failed;
        }
    }
    return failed;
}

bool setAcl(const std::string& path, const Acl& acl) {
    return setAclImpl(path, acl);
}

bool xattrFits(const std::string& name, Family f) {
    if (isLocalOnly(name)) {
        return false;
    }
    switch (f) {
        case Family::Linux:
            // Linux exige espacio de nombres; de «system.» solo existen las ACL POSIX.
            return startsWith(name, "user.") || startsWith(name, "trusted.")
                   || startsWith(name, "security.") || startsWith(name, "system.posix_acl_");
        case Family::FreeBSD:
            return (startsWith(name, "user.") || startsWith(name, "system."))
                   && !startsWith(name, "system.posix_acl_");
        case Family::Darwin:
            // Espacio plano: cabe cualquier nombre. Solo la ACL POSIX de Linux, que aquí
            // sería un atributo con bytes sin sentido, se queda fuera.
            return !startsWith(name, "system.posix_acl_");
        case Family::Windows:
        case Family::Other:
            return false;
    }
    return false;
}

bool aclFits(const Acl& acl, Family f) {
    if (acl.kind.empty()) {
        return true;
    }
    return (acl.kind == "nfs4" && f == Family::FreeBSD)
           || (acl.kind == "darwin" && f == Family::Darwin);
}

Metadata fitFor(const Metadata& m, Family f, bool* lost) {
    if (f == localFamily()) {
        if (lost != nullptr) {
            *lost = false;
        }
        return m;
    }
    Metadata out;
    bool perdido = false;
    for (const auto& kv : m.xattrs) {
        if (xattrFits(kv.first, f)) {
            out.xattrs.insert(kv);
        } else {
            perdido = true;
        }
    }
    if (aclFits(m.acl, f)) {
        out.acl = m.acl;
    } else {
        perdido = true;
    }
    if (lost != nullptr) {
        *lost = perdido;
    }
    return out;
}

std::string digestOf(const Metadata& m) {
    if (m.empty()) {
        return {};
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (ctx != nullptr) {
            EVP_MD_CTX_free(ctx);
        }
        return "?";
    }
    const std::string s = serialize(m);
    EVP_DigestUpdate(ctx, s.data(), s.size());
    unsigned char h[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, h, &len);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < 8 && i < len; ++i) {
        out.push_back(hex[h[i] >> 4]);
        out.push_back(hex[h[i] & 0x0f]);
    }
    return out;
}

// `<n> <aclKindLen> <aclTextLen>\n` + kind + text, y por cada atributo
// `<nameLen> <valueLen>\n` + nombre + valor.
std::string serialize(const Metadata& m) {
    std::string out = std::to_string(m.xattrs.size()) + " " + std::to_string(m.acl.kind.size())
                      + " " + std::to_string(m.acl.text.size()) + "\n" + m.acl.kind + m.acl.text;
    for (const auto& kv : m.xattrs) {
        out += std::to_string(kv.first.size()) + " " + std::to_string(kv.second.size()) + "\n";
        out += kv.first;
        out += kv.second;
    }
    return out;
}

bool parse(const std::string& data, Metadata& out, std::string& error) {
    out = Metadata{};
    if (data.empty()) {
        return true;
    }
    std::size_t i = 0;
    const auto line = [&](std::string& l) {
        const std::size_t fin = data.find('\n', i);
        if (fin == std::string::npos) {
            return false;
        }
        l = data.substr(i, fin - i);
        i = fin + 1;
        return true;
    };
    const auto take = [&](std::size_t n, std::string& s) {
        if (i + n > data.size()) {
            return false;
        }
        s = data.substr(i, n);
        i += n;
        return true;
    };
    std::string l;
    std::size_t n = 0;
    std::size_t lk = 0;
    std::size_t lt = 0;
    if (!line(l) || !(std::istringstream(l) >> n >> lk >> lt) || !take(lk, out.acl.kind)
        || !take(lt, out.acl.text)) {
        error = "unreadable metadata";
        return false;
    }
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t ln = 0;
        std::size_t lv = 0;
        std::string name;
        std::string value;
        if (!line(l) || !(std::istringstream(l) >> ln >> lv) || !take(ln, name)
            || !take(lv, value)) {
            error = "unreadable metadata";
            return false;
        }
        out.xattrs.emplace(std::move(name), std::move(value));
    }
    if (i != data.size()) {
        error = "metadata with bytes left over";
        return false;
    }
    return true;
}

}  // namespace zfsmgr::fsmeta
