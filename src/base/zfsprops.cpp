#include "zfsprops.h"

namespace zfsmgr::base::zfsprops {

// Las propiedades de ZFS cuyo valor sale de una lista CERRADA, y esa lista.
//
// Vive en la capa base porque la usan los dos: la interfaz para ofrecer un desplegable
// al editar, y el intérprete para completar con el tabulador. Estaba escrita solo en la
// interfaz —dentro de una función de 400 líneas—, así que el CLI no podía ofrecerla y
// habría acabado con una segunda copia que se desincroniza.
//
// No están TODAS las propiedades: solo las de valor cerrado. Un `quota` o un
// `mountpoint` no tienen lista que ofrecer, y fingir que sí sería peor que no ofrecer
// nada.
const std::map<std::string, std::vector<std::string>>& propiedadesConValores() {
    static const std::map<std::string, std::vector<std::string>> kTabla = {
        {"atime", {"on", "off"}},
        {"relatime", {"on", "off"}},
        {"readonly", {"on", "off"}},
        {"compression", {"on", "off", "lz4", "zstd", "gzip", "zle", "lzjb"}},
        {"checksum", {"on", "off", "fletcher2", "fletcher4", "sha256", "sha512", "skein", "edonr", "blake3"}},
        {"sync", {"standard", "always", "disabled"}},
        {"logbias", {"latency", "throughput"}},
        {"primarycache", {"all", "none", "metadata"}},
        {"secondarycache", {"all", "none", "metadata"}},
        {"dedup", {"on", "off", "verify", "sha256", "sha512", "skein", "edonr", "blake3"}},
        {"copies", {"1", "2", "3"}},
        {"acltype", {"off", "posix", "nfsv4"}},
        {"aclinherit", {"discard", "noallow", "restricted", "passthrough", "passthrough-x"}},
        {"xattr", {"on", "off", "sa", "dir"}},
        {"normalization", {"none", "formC", "formD", "formKC", "formKD"}},
        {"casesensitivity", {"sensitive", "insensitive", "mixed"}},
        {"utf8only", {"on", "off"}},
        {"canmount", {"on", "off", "noauto"}},
        {"snapdir", {"hidden", "visible"}},
        {"exec", {"on", "off"}},
        {"setuid", {"on", "off"}},
        {"devices", {"on", "off"}},
        {"snapdev", {"hidden", "visible"}},
        {"volmode", {"default", "full", "dev", "none", "geom"}},
    };
    return kTabla;
}

const std::vector<std::string>& valoresDe(const std::string& propiedad) {
    static const std::vector<std::string> vacio;
    const auto& t = propiedadesConValores();
    const auto it = t.find(propiedad);
    return it == t.end() ? vacio : it->second;
}

}  // namespace zfsmgr::base::zfsprops
