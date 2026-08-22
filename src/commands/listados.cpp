#include "listados.h"

#include "strutil.h"

#include <algorithm>

namespace zfsmgr::base::listados {
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
        pool.nombre = p["name"].toString(kv.first);
        pool.estado = p["state"].toString();
        pool.guid = p["pool_guid"].toString();
        pool.salud = valorDe(props, "health");
        pool.tamano = valorDe(props, "size");
        pool.libre = valorDe(props, "free");
        pool.uso = valorDe(props, "capacity");
        if (pool.nombre.empty()) {
            continue;   // sin nombre no se puede nombrar: no sirve para nada
        }
        out.push_back(pool);
    }
    std::sort(out.begin(), out.end(),
              [](const Pool& a, const Pool& b) { return a.nombre < b.nombre; });
    return true;
}

std::vector<Entrada> entradas(const std::string& salidaTsv) {
    std::vector<Entrada> out;
    for (const std::string& linea : split(salidaTsv, "\n", true)) {
        const std::vector<std::string> c = split(linea, "\t", false);
        if (c.size() < 10) {
            continue;
        }
        Entrada e;
        e.nombre = c[0];
        e.guid = c[1];
        e.usado = c[2];
        e.compresion = c[3];
        e.cifrado = c[4];
        e.creacion = c[5];
        e.referenciado = c[6];
        e.montado = c[7];
        e.puntoMontaje = c[8];
        e.canmount = c[9];
        if (trim(e.nombre).empty()) {
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
bool propiedadesDe(const std::string& salida, const char* seccion, std::vector<Propiedad>& out,
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
            Propiedad p;
            p.nombre = kv.first;
            p.valor = kv.second["value"].toString();
            p.origen = origenLegible(kv.second["source"]["type"].toString(),
                                     kv.second["source"]["data"].toString());
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Propiedad& a, const Propiedad& b) { return a.nombre < b.nombre; });
    return true;
}

}  // namespace

bool propiedades(const std::string& salida, std::vector<Propiedad>& out, std::string& error) {
    return propiedadesDe(salida, "datasets", out, error);
}

bool propiedadesDePool(const std::string& salida, std::vector<Propiedad>& out,
                       std::string& error) {
    return propiedadesDe(salida, "pools", out, error);
}

bool contenidoDeDirectorio(const std::string& salida, std::vector<EntradaDeDirectorio>& out,
                           std::string& error) {
    out.clear();
    error.clear();
    zfsmgr::base::json::Value raiz;
    if (!zfsmgr::base::json::parse(salida, raiz, &error)) {
        return false;
    }
    for (const zfsmgr::base::json::Value& e : raiz["entries"].toArray()) {
        EntradaDeDirectorio ent;
        ent.nombre = e["name"].toString();
        if (ent.nombre.empty()) {
            continue;
        }
        ent.directorio = e["type"].toString() == "d";
        // Un directorio no tiene tamaño que enseñar: el que trae el sistema de ficheros es
        // el del propio nodo, no el de lo que contiene, y enseñarlo invita a leerlo mal.
        ent.tamano = ent.directorio ? 0 : static_cast<std::uint64_t>(e["size"].toInt());
        out.push_back(ent);
    }
    std::sort(out.begin(), out.end(),
              [](const EntradaDeDirectorio& a, const EntradaDeDirectorio& b) {
                  return a.nombre < b.nombre;
              });
    return true;
}

bool dispositivos(const std::string& salidaJson, std::vector<Dispositivo>& out,
                  std::string& error) {
    out.clear();
    error.clear();
    zfsmgr::base::json::Value raiz;
    if (!zfsmgr::base::json::parse(salidaJson, raiz, &error)) {
        return false;
    }
    for (const zfsmgr::base::json::Value& d : raiz["devices"].toArray()) {
        Dispositivo x;
        x.ruta = d["path"].toString();
        if (x.ruta.empty()) {
            continue;
        }
        x.resuelta = d["resolved"].toString();
        x.alias = d["alias"].toBool();
        x.tipo = d["type"].toString();
        x.fs = d["fstype"].toString();
        x.montaje = d["mountpoint"].toString();
        x.padre = d["parent"].toString();
        x.tamano = static_cast<std::uint64_t>(d["size"].toInt());
        x.enUso = d["inuse"].toBool();
        out.push_back(x);
    }
    return true;
}

bool montados(const std::string& salidaJson,
              std::vector<std::pair<std::string, std::string>>& out, std::string& error) {
    out.clear();
    error.clear();
    zfsmgr::base::json::Value raiz;
    if (!zfsmgr::base::json::parse(salidaJson, raiz, &error)) {
        return false;
    }
    // Ninguno montado NO es un error: es una respuesta legítima.
    for (const auto& par : raiz["datasets"].toObject()) {
        const std::string punto = par.second["mountpoint"].toString();
        if (!par.first.empty()) {
            out.emplace_back(par.first, punto);
        }
    }
    std::sort(out.begin(), out.end());
    return true;
}

bool tieneDescendientesMontados(const std::string& salidaJson, const std::string& dataset) {
    const std::string ds = zfsmgr::base::trim(dataset);
    if (ds.empty()) {
        return false;
    }
    std::vector<std::pair<std::string, std::string>> lista;
    std::string err;
    if (!montados(salidaJson, lista, err)) {
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

}  // namespace zfsmgr::base::listados
