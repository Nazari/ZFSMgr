// libzfs_core fast paths (with the `zfs` subprocess as the fallback) and the generation of
// the agent's own TLS material.

#include "agent_internal.h"

namespace zfsmgr::agent {

int runMutateShellGeneric(const std::string& payloadB64) {
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        std::cerr << "invalid shell payload\n";
        return 2;
    }
    decoded = trim(decoded);
    if (decoded.empty()) {
        std::cerr << "empty shell payload\n";
        return 2;
    }
    return runExecStreaming("sh", {"-lc", decoded});
}

#ifdef HAVE_LIBZFS_CORE
// ── libzfs_core native helpers (subprocess fallback used on any lzc error) ──

bool s_lzcReady = false;

bool lzcEnsureInit() {
    if (!s_lzcReady) {
        s_lzcReady = (libzfs_core_init() == 0);
    }
    return s_lzcReady;
}

ExecResult lzcSnapshotOne(const std::string& snapName) {
    ExecResult r;
    if (!lzcEnsureInit()) {
        r.rc = -1;
        r.err = "libzfs_core_init failed\n";
        return r;
    }
    nvlist_t* snaps = nullptr;
    if (nvlist_alloc(&snaps, NV_UNIQUE_NAME, 0) != 0) {
        r.rc = ENOMEM;
        r.err = "nvlist_alloc failed\n";
        return r;
    }
    nvlist_add_boolean(snaps, snapName.c_str());
    nvlist_t* errlist = nullptr;
    r.rc = lzc_snapshot(snaps, nullptr, &errlist);
    nvlist_free(snaps);
    if (r.rc != 0) {
        r.err = std::string("cannot create snapshot '") + snapName + "': " + strerror(r.rc) + "\n";
        if (errlist)
            nvlist_free(errlist);
    }
    return r;
}

ExecResult lzcDestroyOneSnap(const std::string& snapName) {
    ExecResult r;
    if (!lzcEnsureInit()) {
        r.rc = -1;
        r.err = "libzfs_core_init failed\n";
        return r;
    }
    nvlist_t* snaps = nullptr;
    if (nvlist_alloc(&snaps, NV_UNIQUE_NAME, 0) != 0) {
        r.rc = ENOMEM;
        r.err = "nvlist_alloc failed\n";
        return r;
    }
    nvlist_add_boolean(snaps, snapName.c_str());
    nvlist_t* errlist = nullptr;
    r.rc = lzc_destroy_snaps(snaps, B_FALSE, &errlist);
    nvlist_free(snaps);
    if (r.rc != 0) {
        r.err = std::string("cannot destroy '") + snapName + "': " + strerror(r.rc) + "\n";
        if (errlist)
            nvlist_free(errlist);
    }
    return r;
}

ExecResult lzcRollbackTo(const std::string& dataset, const std::string& snapName) {
    ExecResult r;
    if (!lzcEnsureInit()) {
        r.rc = -1;
        r.err = "libzfs_core_init failed\n";
        return r;
    }
    r.rc = lzc_rollback_to(dataset.c_str(), snapName.c_str());
    if (r.rc != 0) {
        r.err = std::string("cannot rollback '") + dataset + "' to '" + snapName
                + "': " + strerror(r.rc) + "\n";
    }
    return r;
}

ExecResult lzcClone(const std::string& snapOrigin, const std::string& newDataset) {
    ExecResult r;
    if (!lzcEnsureInit()) {
        r.rc = -1;
        r.err = "libzfs_core_init failed\n";
        return r;
    }
    r.rc = lzc_clone(newDataset.c_str(), snapOrigin.c_str(), nullptr);
    if (r.rc != 0) {
        r.err = std::string("cannot clone '") + snapOrigin + "' to '" + newDataset
                + "': " + strerror(r.rc) + "\n";
    }
    return r;
}

bool lzcExists(const std::string& name) {
    return lzcEnsureInit() && lzc_exists(name.c_str()) != B_FALSE;
}
#endif  // HAVE_LIBZFS_CORE

// ── Generación del material TLS por el propio agente ─────────────────────────
// Antes lo hacía un openssl EXTERNO invocado por shell desde la GUI. En Windows eso
// no vale: openssl no está en el PATH (comprobado en un Windows 11 real; solo
// aparece si hay Git instalado, que no es garantía). El agente ya enlaza OpenSSL de
// forma estática, así que puede emitirlos él y el requisito desaparece en todas las
// plataformas.
//
// Los certificados llevan subjectAltName, keyUsage y extendedKeyUsage porque sin
// ellos el backend TLS de Apple los rechaza (ver el arreglo de la 0.90.3).
bool writeSelfSignedPair(const std::string& certPath, const std::string& keyPath,
                         const std::string& commonName, bool serverAuth, std::string& errMsg) {
    // La emisión vive en `base/tlsserver.cpp`. Aquí queda el nombre y los alt-names del
    // agente.
    std::string err;
    const bool ok = zfsmgr::base::tlsserver::writeSelfSignedPair(
        certPath, keyPath, commonName, serverAuth,
        "DNS:zfsmgr-agent-server,DNS:zfsmgr-agent,DNS:localhost,IP:127.0.0.1", err);
    if (!ok) {
        errMsg = err + "\n";
    }
    return ok;
}

// Genera el material que falte. No toca lo existente salvo que carezca de SAN, misma
// regla que el bootstrap por shell, para que los hosts ya aprovisionados se corrijan.
ExecResult runEnsureTlsCapture() {
    ExecResult r;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(kDefaultTlsDir, ec);

    const std::string serverCrt = kDefaultTlsCertPath;
    const std::string serverKey = kDefaultTlsKeyPath;
    const std::string clientCrt = kDefaultTlsClientCertPath;
    const std::string clientKey = std::string(kDefaultTlsDir) + kPathSep + "client.key";

    auto needsRegen = [](const std::string& certPath) {
        std::error_code e;
        if (!std::filesystem::exists(certPath, e) || std::filesystem::file_size(certPath, e) == 0) {
            return true;
        }
        FILE* f = std::fopen(certPath.c_str(), "rb");
        if (!f) {
            return true;
        }
        X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr);
        std::fclose(f);
        if (!x) {
            return true;
        }
        const int idx = X509_get_ext_by_NID(x, NID_subject_alt_name, -1);
        X509_free(x);
        return idx < 0;  // sin subjectAltName: se rehace
    };

    std::string err;
    if (needsRegen(serverCrt)
        && !writeSelfSignedPair(serverCrt, serverKey, "zfsmgr-agent-server", true, err)) {
        r.rc = 1;
        r.err = err;
        return r;
    }
    if (needsRegen(clientCrt)
        && !writeSelfSignedPair(clientCrt, clientKey, "zfsmgr-agent-client", false, err)) {
        r.rc = 1;
        r.err = err;
        return r;
    }
    r.rc = 0;
    r.out = "TLS_DIR=" + std::string(kDefaultTlsDir) + "\n";
    return r;
}

ExecResult runDumpRefreshBasicsCapture() {
    ExecResult r;
    const std::string osLine = compactSpaces(detectOsLine());
    const std::string machineUuid = compactSpaces(detectMachineUuid());
    const std::string zraw = compactSpaces(detectZfsVersionRaw());
    std::ostringstream ss;
    ss << "OS_LINE=" << osLine << "\n";
    ss << "MACHINE_UUID=" << machineUuid << "\n";
    ss << "ZFS_VERSION_RAW=" << zraw << "\n";
    if (zraw.empty()) {
        ss << "ZFS_DETAIL=" << detectZfsUnavailableReason() << "\n";
    }
    r.rc = 0;
    r.out = ss.str();
    return r;
}

#ifdef _WIN32
// En Windows no hay gestores de paquetes que sondear ni rutas tipo /usr/local/bin: el
// agente resuelve por PATH, que él mismo fija en kDefaultCommandPath. Se compila la
// sonda igual que en Unix porque la pregunta es la misma —¿puede el agente ejecutar
// esta herramienta?— y sin ella la aplicación no puede decir por qué falla una acción.
bool isExecutableFile(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool lookupExecutable(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos
        || name.find('\\') != std::string::npos) {
        return false;
    }
    const char* pathEnv = ::getenv("PATH");
    if (!pathEnv) {
        return false;
    }
    const std::string exts[] = {"", ".exe", ".cmd", ".bat"};
    std::string acc;
    std::istringstream ps(pathEnv);
    std::string dir;
    while (std::getline(ps, dir, ';')) {
        if (dir.empty()) {
            continue;
        }
        for (const std::string& ext : exts) {
            if (isExecutableFile(dir + "\\" + name + ext)) {
                return true;
            }
        }
    }
    return false;
}

ExecResult runDumpToolAvailabilityCapture(const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    std::vector<std::string> wanted;
    if (!decodeBase64(payloadB64, decoded) || !parseJsonStringArray(decoded, wanted)) {
        r.rc = 2;
        ponCodigo(r, "internal", {"invalid tool-availability payload"});
        return r;
    }
    std::ostringstream ss;
    for (const std::string& raw : wanted) {
        const std::string name = trim(raw);
        if (!name.empty()) {
            ss << (lookupExecutable(name) ? "OK:" : "KO:") << name << "\n";
        }
    }
    r.rc = 0;
    r.out = ss.str();
    return r;
}
#endif

#ifndef _WIN32

bool isExecutableFile(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

// Equivalent of `command -v <name>`, without a shell. Names containing a slash
// are rejected rather than resolved: the caller asks for tools by bare name, and
// accepting a path would turn this into an arbitrary-file existence oracle.
bool lookupExecutable(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) {
        return false;
    }
    if (const char* pathEnv = ::getenv("PATH")) {
        std::istringstream ps(pathEnv);
        std::string dir;
        while (std::getline(ps, dir, ':')) {
            if (!dir.empty() && isExecutableFile(dir + "/" + name)) {
                return true;
            }
        }
    }
    for (const char* dir : kToolSearchDirs) {
        if (isExecutableFile(std::string(dir) + "/" + name)) {
            return true;
        }
    }
    return false;
}

// Command availability probe. Payload is base64(JSON array) with the tool names
// to look for; output keeps the OK:/KO:/PM: line format the GUI already parses.
// This replaces a `for c in %1; do command -v ...` loop that interpolated the
// names straight into the remote shell.
ExecResult runDumpToolAvailabilityCapture(const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    std::vector<std::string> wanted;
    if (!decodeBase64(payloadB64, decoded) || !parseJsonStringArray(decoded, wanted)) {
        r.rc = 2;
        ponCodigo(r, "internal", {"invalid tool-availability payload"});
        return r;
    }
    std::ostringstream ss;
    for (const std::string& raw : wanted) {
        const std::string name = trim(raw);
        if (name.empty()) {
            continue;
        }
        ss << (lookupExecutable(name) ? "OK:" : "KO:") << name << "\n";
    }
    for (const char* pm : kProbedPackageManagers) {
        ss << "PM:" << pm << ":" << (lookupExecutable(pm) ? "1" : "0") << "\n";
    }
    r.rc = 0;
    r.out = ss.str();
    return r;
}
#endif

ExecResult runDumpZpoolGuidStatusBatchCapture() {
    ExecResult r;
    ExecResult pools = runExecCapture("zpool", {"list", "-H", "-o", "name"});
    if (pools.rc != 0) {
        return pools;
    }
    std::ostringstream out;
    std::ostringstream err;
    for (const std::string& raw : splitLines(pools.out)) {
        const std::string pool = trim(raw);
        if (pool.empty()) {
            continue;
        }
        ExecResult guid = runExecCapture("zpool", {"get", "-H", "-o", "value", "guid", pool});
        ExecResult status = runExecCapture("zpool", {"status", "-v", pool});
        out << "__ZFSMGR_POOL__:" << pool << "\n";
        const std::vector<std::string> guidLines = splitLines(guid.out);
        out << "__ZFSMGR_GUID__:" << trim(guidLines.empty() ? std::string() : guidLines.front())
            << "\n";
        out << "__ZFSMGR_STATUS_BEGIN__\n";
        if (!status.out.empty()) {
            out << status.out;
            if (status.out.back() != '\n') {
                out << '\n';
            }
        }
        if (!status.err.empty()) {
            out << status.err;
            if (status.err.back() != '\n') {
                out << '\n';
            }
        }
        out << "__ZFSMGR_STATUS_END__\n";
        if (guid.rc != 0 || status.rc != 0) {
            r.rc = status.rc != 0 ? status.rc : guid.rc;
        }
    }
    r.out = out.str();
    r.err = err.str();
    if (r.rc == 1 && r.out.empty() && r.err.empty()) {
        r.rc = 0;
    }
    return r;
}

}  // namespace zfsmgr::agent
