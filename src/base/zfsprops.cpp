#include "zfsprops.h"

#include "strutil.h"

#include <set>
#include <sstream>

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

// Tomada de `zfs send -?` de OpenZFS:
//     send [-DLPbcehnpsVvw] [-i|-I snapshot] [-R [-X dataset[,dataset]...]] <snapshot>
//
// Es la lista literal del mandato menos las tres que pone el programa. Copiarla entera y
// no «las que hacen falta» es lo que hace que valga como criterio: cualquier otra cosa se
// rechaza, y ahí entra tanto una bandera inventada como un nombre de dataset suelto.
const std::vector<BanderaSend>& banderasDeSend() {
    static const std::vector<BanderaSend> kTabla = {
        {"-D", false, "t_nat_send_D", "Deduplicado (obsoleto; el mandato aún lo acepta)."},
        {"-L", false, "t_nat_send_L", "Permite bloques grandes en el flujo."},
        {"-P", false, "t_nat_send_P", "Estadísticas en formato analizable."},
        {"-b", false, "t_nat_send_b", "Solo los bloques que ocupan espacio."},
        {"-c", false, "t_nat_send_c", "Comprimido tal cual está en disco."},
        {"-e", false, "t_nat_send_e", "Con los bloques embebidos, sin expandirlos."},
        {"-h", false, "t_nat_send_h", "Se lleva también las retenciones."},
        {"-n", false, "t_nat_send_n", "Ensayo: no manda nada, dice qué mandaría."},
        {"-p", false, "t_nat_send_p", "Con las propiedades del dataset."},
        {"-s", false, "t_nat_send_s", "Reanudable: deja testigo si se corta."},
        {"-V", false, "t_nat_send_V", "Fija la versión del flujo."},
        {"-v", false, "t_nat_send_v", "Cuenta lo que va mandando."},
        {"-w", false, "t_nat_send_w", "En crudo: lo cifrado viaja sin descifrar."},
        {"-R", false, "t_nat_send_R", "Replicación: con descendientes e instantáneas."},
        {"-X", true,  "t_nat_send_X", "Con -R, deja fuera ese dataset (o varios, con comas)."}
    };
    return kTabla;
}

// ¿Está declarada esta forma, tal cual?
const BanderaSend* buscaBanderaSend(const std::string& forma) {
    for (const BanderaSend& b : banderasDeSend()) {
        if (forma == b.forma) {
            return &b;
        }
    }
    return nullptr;
}

bool banderasDeSendValidas(const std::string& cadena, std::string& mala) {
    mala.clear();
    std::istringstream iss(cadena);
    std::string tok;
    while (iss >> tok) {
        const BanderaSend* encontrada = buscaBanderaSend(tok);
        // AGRUPADAS: `-wLec` son cuatro. Es como las escribe el manual de OpenZFS y como
        // las manda el planificador de instantáneas, así que rechazarlas aquí no protegía
        // de nada y sí cortaba una nivelación entera.
        //
        // Se acepta el grupo solo si TODAS las letras están declaradas y NINGUNA lleva
        // valor: con una que lo lleve no se sabe dónde empieza el valor sin inventárselo.
        if (!encontrada && tok.size() > 2 && tok[0] == '-' && tok[1] != '-') {
            bool todas = true;
            for (std::size_t i = 1; i < tok.size(); ++i) {
                const BanderaSend* una = buscaBanderaSend(std::string("-") + tok[i]);
                if (!una || una->valor) {
                    todas = false;
                    break;
                }
            }
            if (todas) {
                continue;
            }
        }
        if (!encontrada) {
            mala = tok;
            return false;
        }
        // El valor de `-X` se consume aquí: si no, se leería como un componente suelto y
        // se rechazaría el dataset que la propia bandera pide.
        if (encontrada->valor && !(iss >> tok)) {
            mala = encontrada->forma;
            return false;
        }
    }
    return true;
}

}  // namespace zfsmgr::base::zfsprops

namespace zfsmgr::base::zfsprops {
namespace {

std::string bajo(const std::string& s) { return toLowerAscii(trim(s)); }

bool esCierto(const std::string& v) {
    const std::string s = bajo(v);
    return s == "true" || s == "on" || s == "yes" || s == "1";
}

// Las que se pueden escribir en cualquier tipo de dataset.
const std::set<std::string>& comunes() {
    static const std::set<std::string> s = {
        "atime",      "relatime",       "readonly",   "compression", "checksum",
        "sync",       "logbias",        "primarycache", "secondarycache", "dedup",
        "copies",     "acltype",        "aclinherit", "xattr",       "normalization",
        "casesensitivity", "utf8only",  "keylocation", "comment",
    };
    return s;
}

const std::set<std::string>& deSistemaDeFicheros() {
    static const std::set<std::string> s = [] {
        std::set<std::string> t = comunes();
        for (const char* p : {"mountpoint", "canmount", "recordsize", "quota", "reservation",
                              "refquota", "refreservation", "snapdir", "exec", "setuid",
                              "devices", "driveletter", "sharesmb", "sharenfs", "nbmand",
                              "overlay", "jailed", "zoned"}) {
            t.insert(p);
        }
        return t;
    }();
    return s;
}

const std::set<std::string>& deVolumen() {
    static const std::set<std::string> s = [] {
        std::set<std::string> t = comunes();
        for (const char* p : {"volsize", "volblocksize", "reservation", "refreservation",
                              "snapdev", "volmode"}) {
            t.insert(p);
        }
        return t;
    }();
    return s;
}

}  // namespace

Plataforma plataformaDe(const std::string& osType, const std::string& osLine) {
    const std::string junto = bajo(osType + " " + osLine);
    if (contains(junto, "windows")) {
        return Plataforma::Windows;
    }
    if (contains(junto, "darwin") || contains(junto, "mac")) {
        return Plataforma::MacOs;
    }
    if (contains(junto, "freebsd")) {
        return Plataforma::FreeBsd;
    }
    if (contains(junto, "linux")) {
        return Plataforma::Linux;
    }
    return Plataforma::Otra;
}

bool esPropiedadDeUsuario(const std::string& prop) {
    return prop.find(':') != std::string::npos;
}

bool soportadaEn(const std::string& prop, Plataforma p) {
    const std::string n = bajo(prop);
    if (n.empty()) {
        return false;
    }
    if (n == "vscan") {
        return false;
    }
    if (n == "jailed") {
        return p == Plataforma::FreeBsd;
    }
    if (n == "zoned") {
        return p == Plataforma::Linux;
    }
    if (n == "sharesmb") {
        return p != Plataforma::MacOs;
    }
    if (n == "nbmand") {
        return p == Plataforma::Linux;
    }
    return true;
}

bool editableEnLinea(const std::string& prop, const std::string& tipoDataset,
                     const std::string& origen, const std::string& readonly, Plataforma p) {
    const std::string n = bajo(prop);
    const std::string tipo = bajo(tipoDataset);
    if (n.empty()) {
        return false;
    }
    if (!soportadaEn(n, p)) {
        return false;
    }
    // Lo que ZFS declara de solo lectura no se toca, diga lo que diga la lista de abajo.
    if (esCierto(readonly)) {
        return false;
    }
    // Origen «-» es una propiedad CALCULADA —`used`, `creation`—, no una que esté puesta.
    if (trim(origen) == "-") {
        return false;
    }
    if (esPropiedadDeUsuario(n)) {
        return true;
    }
    if (tipo == "filesystem") {
        return deSistemaDeFicheros().count(n) > 0;
    }
    if (tipo == "volume") {
        return deVolumen().count(n) > 0;
    }
    // A una instantánea no se le cambia nada: es de solo lectura por definición.
    if (tipo == "snapshot") {
        return false;
    }
    // Sin saber el tipo, se admite lo de cualquiera de los dos: es lo que hacía la interfaz.
    return deSistemaDeFicheros().count(n) > 0 || deVolumen().count(n) > 0;
}

}  // namespace zfsmgr::base::zfsprops
