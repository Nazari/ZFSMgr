#include "pools.h"

#include <cctype>

#include "strutil.h"

namespace zfsmgr::commands::pools {

const char* subcommand(Operation op) {
    switch (op) {
        case Operation::Scrub:      return "scrub";
        case Operation::Trim:       return "trim";
        case Operation::Initialize: return "initialize";
        case Operation::Clear:      return "clear";
        case Operation::Sync:       return "sync";
        case Operation::Export:     return "export";
        case Operation::Import:     return "import";
        case Operation::Destroy:    return "destroy";
        case Operation::Upgrade:    return "upgrade";
        case Operation::Reguid:     return "reguid";
    }
    return "";
}

bool acceptsPhase(Operation op) {
    switch (op) {
        case Operation::Scrub:
        case Operation::Trim:
        case Operation::Initialize:
            return true;
        case Operation::Clear:
        case Operation::Sync:
        case Operation::Export:
        case Operation::Import:
        case Operation::Destroy:
        case Operation::Upgrade:
        case Operation::Reguid:
            return false;
    }
    return false;
}

bool isIrreversible(Operation op) {
    switch (op) {
        case Operation::Destroy:   // se lleva el pool y sus datos
        case Operation::Upgrade:   // no se puede bajar la versión
        case Operation::Reguid:    // el identificador viejo no vuelve
            return true;
        case Operation::Scrub:
        case Operation::Trim:
        case Operation::Initialize:
        case Operation::Clear:
        case Operation::Sync:
        case Operation::Export:
        case Operation::Import:
            return false;
    }
    return false;
}

bool needsConfirmation(Operation op) {
    if (isIrreversible(op)) {
        return true;
    }
    switch (op) {
        // No borra datos, pero borra la CUENTA DE ERRORES del pool, y se teclea queriendo
        // limpiar el terminal.
        case Operation::Clear:
        // No borra nada, pero el pool DESAPARECE de esa máquina hasta que alguien lo
        // importe, y lo que estuviera usándolo se queda sin él.
        case Operation::Export:
            return true;
        case Operation::Scrub:
        case Operation::Trim:
        case Operation::Initialize:
        case Operation::Sync:
        case Operation::Import:
        case Operation::Destroy:
        case Operation::Upgrade:
        case Operation::Reguid:
            return false;
    }
    return false;
}

namespace {

// La letra de la fase, que NO es la misma para las tres.
const char* letraDeFase(Operation op, Phase fase) {
    if (fase == Phase::Start) {
        return nullptr;
    }
    if (op == Operation::Scrub) {
        return fase == Phase::Stop ? "-s" : "-p";
    }
    // trim e initialize: cancelar es `-c` y suspender es `-s`.
    return fase == Phase::Stop ? "-c" : "-s";
}

}  // namespace

std::vector<std::string> argv(Operation op, const std::string& pool, Phase fase,
                              const std::vector<std::string>& banderas,
                              const std::vector<std::string>& discos) {
    const std::string p = zfsmgr::base::trim(pool);
    if (p.empty()) {
        return {};
    }
    if (fase != Phase::Start && !acceptsPhase(op)) {
        return {};
    }
    std::vector<std::string> out{subcommand(op)};
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

bool isValidPoolName(const std::string& nombre) {
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

std::vector<std::string> argvImportAs(const std::string& pool,
                                          const std::string& nombreNuevo) {
    const std::string p = zfsmgr::base::trim(pool);
    const std::string n = zfsmgr::base::trim(nombreNuevo);
    if (p.empty() || !isValidPoolName(n)) {
        return {};
    }
    return {"import", p, n};
}

}  // namespace zfsmgr::commands::pools
