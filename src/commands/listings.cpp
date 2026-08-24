#include "listings.h"

#include "strutil.h"

#include <algorithm>

namespace zfsmgr::base::listings {
namespace {

std::string valorDe(const json::Value& props, const char* clave) {
    return props[clave]["value"].toString();
}

}  // namespace

bool pools(const std::string& salida, std::vector<Pool>& out, std::string& error) {
    out.clear();
    error.clear();
    json::Value raiz;
    if (trim(salida).empty()) {
        return true;   // no hay pools; ver la nota de la cabecera
    }
    if (!json::parse(salida, raiz, &error)) {
        return false;
    }
    for (const auto& kv : raiz["pools"].toObject()) {
        const json::Value& p = kv.second;
        const json::Value& props = p["properties"];
        Pool pool;
        pool.name = p["name"].toString(kv.first);
        pool.state = p["state"].toString();
        pool.guid = p["pool_guid"].toString();
        pool.health = valorDe(props, "health");
        pool.size = valorDe(props, "size");
        pool.free = valorDe(props, "free");
        pool.used = valorDe(props, "capacity");
        if (pool.name.empty()) {
            continue;   // sin nombre no se puede nombrar: no sirve para nada
        }
        out.push_back(pool);
    }
    std::sort(out.begin(), out.end(),
              [](const Pool& a, const Pool& b) { return a.name < b.name; });
    return true;
}

std::vector<Entry> entries(const std::string& salidaTsv) {
    std::vector<Entry> out;
    for (const std::string& linea : split(salidaTsv, "\n", true)) {
        const std::vector<std::string> c = split(linea, "\t", false);
        if (c.size() < 10) {
            continue;
        }
        Entry e;
        e.name = c[0];
        e.guid = c[1];
        e.used = c[2];
        e.compression = c[3];
        e.encryption = c[4];
        e.creation = c[5];
        e.referenced = c[6];
        e.mounted = c[7];
        e.mountpoint = c[8];
        e.canmount = c[9];
        if (trim(e.name).empty()) {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

namespace {

// El origen de una propiedad tal y como lo escribe `zfs get -H -o source`, a partir del
// par «type/data» del JSON. Comprobado contra la salida real de las dos formas, que es de
// donde salen estos nombres: no son una invención de este programa.
std::string origenLegible(const std::string& tipo, const std::string& dato) {
    const std::string t = toUpperAscii(trim(tipo));
    if (t == "NONE" || t.empty()) {
        return "-";   // calculada: `used`, `creation`. No se cambia.
    }
    if (t == "DEFAULT") {
        return "default";
    }
    if (t == "LOCAL") {
        return "local";
    }
    if (t == "RECEIVED") {
        return "received";
    }
    if (t == "TEMPORARY") {
        return "temporary";
    }
    if (t == "INHERITED") {
        const std::string d = trim(dato);
        return d.empty() || d == "-" ? "inherited" : "inherited from " + d;
    }
    return toLowerAscii(t);
}

// El cuerpo común de `zfs get -j` y `zpool get -j`: cambia la sección de la que cuelgan
// los objetos y nada más.
bool propiedadesDe(const std::string& salida, const char* seccion, std::vector<Property>& out,
                   std::string& error) {
    out.clear();
    error.clear();
    if (trim(salida).empty()) {
        return true;
    }
    json::Value raiz;
    if (!json::parse(salida, raiz, &error)) {
        return false;
    }
    // Hay una entrada por objeto aunque se haya preguntado por uno solo.
    for (const auto& obj : raiz[seccion].toObject()) {
        for (const auto& kv : obj.second["properties"].toObject()) {
            Property p;
            p.name = kv.first;
            p.value = kv.second["value"].toString();
            p.source = origenLegible(kv.second["source"]["type"].toString(),
                                     kv.second["source"]["data"].toString());
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Property& a, const Property& b) { return a.name < b.name; });
    return true;
}

}  // namespace

bool properties(const std::string& salida, std::vector<Property>& out, std::string& error) {
    return propiedadesDe(salida, "datasets", out, error);
}

bool poolProperties(const std::string& salida, std::vector<Property>& out,
                       std::string& error) {
    return propiedadesDe(salida, "pools", out, error);
}

bool directoryContents(const std::string& salida, std::vector<DirectoryEntry>& out,
                           std::string& error) {
    out.clear();
    error.clear();
    zfsmgr::base::json::Value raiz;
    if (!zfsmgr::base::json::parse(salida, raiz, &error)) {
        return false;
    }
    for (const zfsmgr::base::json::Value& e : raiz["entries"].toArray()) {
        DirectoryEntry ent;
        ent.name = e["name"].toString();
        if (ent.name.empty()) {
            continue;
        }
        ent.directory = e["type"].toString() == "d";
        // Un directorio no tiene tamaño que enseñar: el que trae el sistema de ficheros es
        // el del propio nodo, no el de lo que contiene, y enseñarlo invita a leerlo mal.
        ent.size = ent.directory ? 0 : static_cast<std::uint64_t>(e["size"].toInt());
        out.push_back(ent);
    }
    std::sort(out.begin(), out.end(),
              [](const DirectoryEntry& a, const DirectoryEntry& b) {
                  return a.name < b.name;
              });
    return true;
}

bool devices(const std::string& salidaJson, std::vector<Device>& out,
                  std::string& error) {
    out.clear();
    error.clear();
    zfsmgr::base::json::Value raiz;
    if (!zfsmgr::base::json::parse(salidaJson, raiz, &error)) {
        return false;
    }
    for (const zfsmgr::base::json::Value& d : raiz["devices"].toArray()) {
        Device x;
        x.path = d["path"].toString();
        if (x.path.empty()) {
            continue;
        }
        x.resolved = d["resolved"].toString();
        x.alias = d["alias"].toBool();
        x.type = d["type"].toString();
        x.fs = d["fstype"].toString();
        x.mountpoint = d["mountpoint"].toString();
        x.parent = d["parent"].toString();
        x.size = static_cast<std::uint64_t>(d["size"].toInt());
        x.inUse = d["inuse"].toBool();
        out.push_back(x);
    }
    return true;
}

bool mounted(const std::string& salidaJson,
              std::vector<std::pair<std::string, std::string>>& out, std::string& error) {
    out.clear();
    error.clear();
    zfsmgr::base::json::Value raiz;
    if (!zfsmgr::base::json::parse(salidaJson, raiz, &error)) {
        return false;
    }
    // None_ montado NO es un error: es una respuesta legítima.
    for (const auto& par : raiz["datasets"].toObject()) {
        const std::string punto = par.second["mountpoint"].toString();
        if (!par.first.empty()) {
            out.emplace_back(par.first, punto);
        }
    }
    std::sort(out.begin(), out.end());
    return true;
}

bool hasMountedDescendants(const std::string& salidaJson, const std::string& dataset) {
    const std::string ds = zfsmgr::base::trim(dataset);
    if (ds.empty()) {
        return false;
    }
    std::vector<std::pair<std::string, std::string>> lista;
    std::string err;
    if (!mounted(salidaJson, lista, err)) {
        return false;
    }
    // Con la barra: «tank/datos2» empieza por «tank/datos» y no está debajo de él.
    const std::string prefijo = ds + "/";
    for (const auto& par : lista) {
        if (par.first.rfind(prefijo, 0) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace zfsmgr::base::listings
