#include "refreshparse.h"

#include "strutil.h"

#include <regex>

namespace zfsmgr::base::refresh {
namespace {


}  // namespace

std::vector<std::string> zfsmgrUnixCommandSet() {
    // `zstd` y `gzip` NO son necesarios para nada: se preguntan para poder ELEGIR códec al
    // transferir. Antes eso costaba cuatro sondas por SSH —dos herramientas por dos
    // extremos— cada vez que se abría el diálogo de sincronizar; aquí van con el resto y
    // salen gratis del refresco que ya se hace.
    return {"zfs", "zpool", "rsync", "tar", "ssh", "sh", "zstd", "gzip"};
}

std::string normalizeMachineUuid(std::string s) {
    // Un UUID es hexadecimal, así que la variante ASCII es la correcta y además evita
    // sorpresas de idioma justo donde se está comparando un identificador.
    s = toLowerAscii(trim(s));
    if (s.size() > 2 && s.front() == '{' && s.back() == '}') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

std::string extractMachineUuid(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) {
        return std::string();
    }
    static const std::regex rxDashed(
        "([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");
    std::smatch m;
    if (std::regex_search(t, m, rxDashed)) {
        return normalizeMachineUuid(m[1].str());
    }
    static const std::regex rxCompact("([0-9a-fA-F]{32})");
    if (std::regex_search(t, m, rxCompact)) {
        return normalizeMachineUuid(m[1].str());
    }
    // Sin nada reconocible: la primera línea, que es lo que suele imprimir el comando.
    const std::vector<std::string> lineas = split(t, "\n", false);
    return normalizeMachineUuid(trim(lineas.empty() ? std::string() : lineas.front()));
}

std::map<std::string, std::string> parseKeyValueOutput(const std::string& text) {
    std::map<std::string, std::string> out;
    for (const std::string& raw : split(text, "\n", true)) {
        const long long eq = indexOf(raw, "=");
        if (eq <= 0) {
            continue;
        }
        const std::string key = toUpperAscii(trim(raw.substr(0, static_cast<std::size_t>(eq))));
        const std::string value = trim(raw.substr(static_cast<std::size_t>(eq) + 1));
        if (!key.empty()) {
            out[key] = value;
        }
    }
    return out;
}



std::map<std::string, PoolGuidStatus> parsePoolGuidStatusBatch(const std::string& text) {
    std::map<std::string, PoolGuidStatus> out;
    std::string currentPool;
    std::string currentGuid;
    std::vector<std::string> statusLines;
    bool collectingStatus = false;

    auto flushCurrent = [&]() {
        const std::string pool = trim(currentPool);
        if (!pool.empty()) {
            PoolGuidStatus entry;
            entry.guid = trim(currentGuid);
            entry.status = trim(join(statusLines, "\n"));
            out[pool] = entry;
        }
        currentPool.clear();
        currentGuid.clear();
        statusLines.clear();
        collectingStatus = false;
    };

    for (const std::string& line : split(text, "\n", false)) {
        if (startsWith(line, "__ZFSMGR_POOL__:")) {
            flushCurrent();
            currentPool = trim(mid(line, 16));
            continue;
        }
        if (startsWith(line, "__ZFSMGR_GUID__:")) {
            currentGuid = trim(mid(line, 16));
            continue;
        }
        if (line == "__ZFSMGR_STATUS_BEGIN__") {
            collectingStatus = true;
            continue;
        }
        if (line == "__ZFSMGR_STATUS_END__") {
            collectingStatus = false;
            continue;
        }
        if (collectingStatus) {
            // Sin recortar: la sangría de `zpool status` es parte de lo que se muestra.
            statusLines.push_back(line);
        }
    }
    flushCurrent();
    return out;
}

}  // namespace zfsmgr::base::refresh
