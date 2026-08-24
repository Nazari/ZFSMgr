#include "requests.h"

#include "strutil.h"

namespace zfsmgr::commands::requests {

namespace {

std::string limpia(const std::string& s) { return zfsmgr::base::trim(s); }

// Un verbo con un argumento obligatorio. Si el argumento viene vacío se devuelve la lista
// vacía en vez de mandar el verbo pelado: el daemon contestaría con su línea de uso y un
// rc=2, que es un error mucho peor de leer que no haber preguntado.
std::vector<std::string> withOne(const char* verbo, const std::string& arg) {
    const std::string a = limpia(arg);
    if (a.empty()) {
        return {};
    }
    return {verbo, a};
}

}  // namespace

std::vector<std::string> poolList() { return {"--dump-zpool-list"}; }

std::vector<std::string> poolStatus(const std::string& pool) {
    return withOne("--dump-zpool-status", pool);
}

std::vector<std::string> poolStatusRaw(const std::string& pool) {
    return withOne("--dump-zpool-status-p", pool);
}

std::vector<std::string> poolHistory(const std::string& pool) {
    return withOne("--dump-zpool-history", pool);
}

std::vector<std::string> poolProperties(const std::string& pool) {
    return withOne("--dump-zpool-get-all", pool);
}

std::vector<std::string> poolGuid(const std::string& pool) {
    return withOne("--dump-zpool-guid", pool);
}

std::vector<std::string> importableProbe() { return {"--dump-zpool-import-probe"}; }

std::vector<std::string> datasetList(const std::string& objeto) {
    return withOne("--dump-zfs-list-all", objeto);
}

std::vector<std::string> descendantNames(const std::string& objeto) {
    return withOne("--dump-zfs-list-children", objeto);
}

std::vector<std::string> breakdownList(const std::string& dataset) {
    return withOne("--dump-advanced-breakdown-list", dataset);
}

std::vector<std::string> datasetProperties(const std::string& objeto) {
    return withOne("--dump-zfs-get-all", objeto);
}

std::vector<std::string> datasetProperty(const std::string& propiedad,
                                            const std::string& objeto) {
    const std::string p = limpia(propiedad);
    const std::string o = limpia(objeto);
    if (p.empty() || o.empty()) {
        return {};
    }
    // El orden es propiedad y luego objeto, que es al revés de como se dice en voz alta
    // —«el mountpoint de tank/datos»—. Por eso la función lo pide en ese mismo orden: para
    // que no haya que acordarse.
    return {"--dump-zfs-get-prop", p, o};
}

std::vector<std::string> datasetExists(const std::string& objeto) {
    return withOne("--dump-zfs-exists", objeto);
}

std::vector<std::string> guidMap(const std::string& objeto) {
    return withOne("--dump-zfs-guid-map", objeto);
}

std::vector<std::string> specificProperties(const std::vector<std::string>& propiedades,
                                              const std::string& objeto) {
    const std::string o = limpia(objeto);
    if (o.empty()) {
        return {};
    }
    std::string lista;
    for (const std::string& pr : propiedades) {
        const std::string t = limpia(pr);
        if (t.empty()) {
            continue;
        }
        if (!lista.empty()) {
            lista += ",";
        }
        lista += t;
    }
    if (lista.empty()) {
        return {};
    }
    return {"--dump-zfs-get-json", lista, o};
}

std::vector<std::string> permissionsOfMany(const std::vector<std::string>& datasets) {
    std::vector<std::string> argv{"--dump-zfs-allow-batch"};
    for (const std::string& d : datasets) {
        const std::string t = limpia(d);
        if (!t.empty()) {
            argv.push_back(t);
        }
    }
    return argv.size() < 2 ? std::vector<std::string>{} : argv;
}

std::vector<std::string> poolGuidsAndStates() { return {"--dump-zpool-guid-status-batch"}; }

std::vector<std::string> mounts() { return {"--dump-zfs-mount"}; }

std::vector<std::string> driveLetters(const std::string& pool) {
    return withOne("--dump-zfs-driveletters", pool);
}

std::vector<std::string> permissionsOf(const std::string& dataset) {
    return withOne("--dump-zfs-allow", dataset);
}

std::vector<std::string> holdsOf(const std::vector<std::string>& objetos) {
    std::vector<std::string> argv{"--dump-zfs-holds"};
    for (const std::string& o : objetos) {
        const std::string t = limpia(o);
        if (!t.empty()) {
            argv.push_back(t);
        }
    }
    if (argv.size() < 2) {
        return {};
    }
    return argv;
}

std::vector<std::string> diffBetween(const std::string& instantaneaA,
                                         const std::string& instantaneaB) {
    const std::string a = limpia(instantaneaA);
    const std::string b = limpia(instantaneaB);
    if (a.empty() || b.empty()) {
        return {};
    }
    return {"--dump-zfs-diff", a, b};
}

std::vector<std::string> gsaOfDataset(const std::string& dataset) {
    return withOne("--dump-zfs-get-gsa-raw-recursive", dataset);
}

std::vector<std::string> gsaOfAllPools() { return {"--dump-zfs-get-gsa-raw-all-pools"}; }

std::vector<std::string> directoryContents(const std::string& ruta) {
    return withOne("--dump-dir-list", ruta);
}

std::vector<std::string> fileContents(const std::string& ruta, unsigned long long desde,
                                            unsigned long long cuanto) {
    const std::string r = limpia(ruta);
    if (r.empty()) {
        return {};
    }
    return {"--dump-file", r, std::to_string(desde), std::to_string(cuanto)};
}

std::vector<std::string> health() { return {"--health"}; }

std::vector<std::string> daemonLog(unsigned long long desdeByte, unsigned long long cuantos) {
    if (desdeByte == 0 && cuantos == 0) {
        // Entero: el verbo trata los dos argumentos como opcionales, y mandar «0 0» no es lo
        // mismo que no mandar nada en todos los caminos.
        return {"--dump-daemon-log"};
    }
    return {"--dump-daemon-log", std::to_string(desdeByte), std::to_string(cuantos)};
}

std::vector<std::string> blockDevices() { return {"--dump-block-devices"}; }

std::vector<std::string> zfsVersion() { return {"--dump-zfs-version"}; }

std::vector<std::string> availableTools() { return {"--dump-tool-availability"}; }

std::vector<std::string> refreshBasics() { return {"--dump-refresh-basics"}; }

std::vector<std::string> peerList() { return {"--dump-peers"}; }

std::vector<std::string> zfsGeneric(const std::string& argvCodificado) {
    return withOne("--mutate-zfs-generic", argvCodificado);
}

std::vector<std::string> zpoolGeneric(const std::string& argvCodificado) {
    return withOne("--mutate-zpool-generic", argvCodificado);
}

std::vector<std::string> createDataset(const std::string& argvCodificado) {
    return withOne("--mutate-zfs-create", argvCodificado);
}

std::vector<std::string> loadKey(const std::string& dataset, const std::string& frase) {
    const std::string d = limpia(dataset);
    if (d.empty()) {
        return {};
    }
    // La frase NO se recorta: un espacio al final de una frase de paso es parte de la frase.
    return {"--mutate-zfs-load-key", zfsmgr::base::base64Encode(d),
            zfsmgr::base::base64Encode(frase)};
}

std::vector<std::string> changeKey(const std::string& dataset, const std::string& frase,
                                     const std::string& nueva) {
    const std::string d = limpia(dataset);
    if (d.empty()) {
        return {};
    }
    return {"--mutate-zfs-change-key", zfsmgr::base::base64Encode(d),
            zfsmgr::base::base64Encode(frase), zfsmgr::base::base64Encode(nueva)};
}

std::vector<std::string> repairAltMountpoints(const std::vector<std::string>& extras) {
    std::vector<std::string> argv{"--repair-alt-mountpoints"};
    for (const std::string& e : extras) {
        const std::string t = limpia(e);
        if (!t.empty()) {
            argv.push_back(t);
        }
    }
    return argv;
}

std::vector<std::string> setPeers(const std::string& cargaB64) {
    return withOne("--mutate-set-peers", cargaB64);
}

std::vector<std::string> setBindAddress(const std::string& direccion) {
    return withOne("--mutate-set-bind", direccion);
}

std::vector<std::string> rsyncCopy(const std::string& cargaB64) {
    return withOne("--mutate-rsync-local", cargaB64);
}

std::vector<std::string> permissionsBatch(const std::string& cargaB64) {
    return withOne("--mutate-zfs-allow-batch", cargaB64);
}

bool canEnqueue(const std::string& verbo) {
    const std::string v = limpia(verbo);
    // **Esta lista tiene un solo dueño, y es esta función.** El daemon la necesita para no
    // fiarse del cliente y los clientes para decidir antes de pedir nada —qué botón pintar,
    // qué camino tomar—, así que la tentación era escribirla dos veces. `daemon_main.cpp`
    // enlaza `zfsmgr_commands` y llama aquí: `isAsyncSubmittableCommand` es una línea.
    return v == "--mutate-advanced-breakdown" || v == "--mutate-advanced-assemble"
           || v == "--mutate-advanced-todir" || v == "--mutate-rsync-local"
           || v == "--tree-send-to-peer";
}

std::vector<std::string> enqueue(const std::vector<std::string>& orden) {
    if (orden.empty() || !canEnqueue(orden.front())) {
        return {};
    }
    std::vector<std::string> argv{"--job-submit"};
    argv.insert(argv.end(), orden.begin(), orden.end());
    return argv;
}

std::vector<std::string> jobList() { return {"--job-list"}; }

std::vector<std::string> jobStatus(const std::string& id) {
    return withOne("--job-status", id);
}

std::vector<std::string> cancelJob(const std::string& id) {
    return withOne("--job-cancel", id);
}

}  // namespace zfsmgr::commands::requests
