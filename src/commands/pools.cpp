#include "pools.h"

#include <cctype>

#include "strutil.h"

namespace zfsmgr::commands::pools {

const char* subcomando(Operacion op) {
    switch (op) {
        case Operacion::Scrub:      return "scrub";
        case Operacion::Trim:       return "trim";
        case Operacion::Initialize: return "initialize";
        case Operacion::Clear:      return "clear";
        case Operacion::Sync:       return "sync";
        case Operacion::Export:     return "export";
        case Operacion::Import:     return "import";
        case Operacion::Destroy:    return "destroy";
        case Operacion::Upgrade:    return "upgrade";
        case Operacion::Reguid:     return "reguid";
    }
    return "";
}

bool admiteFase(Operacion op) {
    switch (op) {
        case Operacion::Scrub:
        case Operacion::Trim:
        case Operacion::Initialize:
            return true;
        case Operacion::Clear:
        case Operacion::Sync:
        case Operacion::Export:
        case Operacion::Import:
        case Operacion::Destroy:
        case Operacion::Upgrade:
        case Operacion::Reguid:
            return false;
    }
    return false;
}

bool esIrreversible(Operacion op) {
    switch (op) {
        case Operacion::Destroy:   // se lleva el pool y sus datos
        case Operacion::Upgrade:   // no se puede bajar la versión
        case Operacion::Reguid:    // el identificador viejo no vuelve
            return true;
        case Operacion::Scrub:
        case Operacion::Trim:
        case Operacion::Initialize:
        case Operacion::Clear:
        case Operacion::Sync:
        case Operacion::Export:
        case Operacion::Import:
            return false;
    }
    return false;
}

bool pideConfirmacion(Operacion op) {
    if (esIrreversible(op)) {
        return true;
    }
    switch (op) {
        // No borra datos, pero borra la CUENTA DE ERRORES del pool, y se teclea queriendo
        // limpiar el terminal.
        case Operacion::Clear:
        // No borra nada, pero el pool DESAPARECE de esa máquina hasta que alguien lo
        // importe, y lo que estuviera usándolo se queda sin él.
        case Operacion::Export:
            return true;
        case Operacion::Scrub:
        case Operacion::Trim:
        case Operacion::Initialize:
        case Operacion::Sync:
        case Operacion::Import:
        case Operacion::Destroy:
        case Operacion::Upgrade:
        case Operacion::Reguid:
            return false;
    }
    return false;
}

namespace {

// La letra de la fase, que NO es la misma para las tres.
const char* letraDeFase(Operacion op, Fase fase) {
    if (fase == Fase::Arrancar) {
        return nullptr;
    }
    if (op == Operacion::Scrub) {
        return fase == Fase::Parar ? "-s" : "-p";
    }
    // trim e initialize: cancelar es `-c` y suspender es `-s`.
    return fase == Fase::Parar ? "-c" : "-s";
}

}  // namespace

std::vector<std::string> argv(Operacion op, const std::string& pool, Fase fase,
                              const std::vector<std::string>& banderas,
                              const std::vector<std::string>& discos) {
    const std::string p = zfsmgr::base::trim(pool);
    if (p.empty()) {
        return {};
    }
    if (fase != Fase::Arrancar && !admiteFase(op)) {
        return {};
    }
    std::vector<std::string> out{subcomando(op)};
    if (const char* letra = letraDeFase(op, fase)) {
        out.push_back(letra);
    }
    // Banderas ANTES del pool: detrás, zpool las ignora en silencio.
    for (const std::string& b : banderas) {
        const std::string t = zfsmgr::base::trim(b);
        if (!t.empty()) {
            out.push_back(t);
        }
    }
    out.push_back(p);
    // Y los discos al final: delante, zpool lee el primero como nombre de pool.
    for (const std::string& d : discos) {
        const std::string t = zfsmgr::base::trim(d);
        if (!t.empty()) {
            out.push_back(t);
        }
    }
    return out;
}

bool nombreDePoolValido(const std::string& nombre) {
    const std::string n = zfsmgr::base::trim(nombre);
    if (n.empty()) {
        return false;
    }
    if (std::isalpha(static_cast<unsigned char>(n[0])) == 0) {
        return false;
    }
    return n.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "0123456789_-.:") == std::string::npos;
}

std::vector<std::string> argvImportarComo(const std::string& pool,
                                          const std::string& nombreNuevo) {
    const std::string p = zfsmgr::base::trim(pool);
    const std::string n = zfsmgr::base::trim(nombreNuevo);
    if (p.empty() || !nombreDePoolValido(n)) {
        return {};
    }
    return {"import", p, n};
}

}  // namespace zfsmgr::commands::pools
