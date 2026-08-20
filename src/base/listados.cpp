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
            p.origen = kv.second["source"]["data"].toString();
            if (p.origen.empty()) {
                p.origen = kv.second["source"]["type"].toString();
            }
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

}  // namespace zfsmgr::base::listados
