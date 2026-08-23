#include "snapshots.h"

#include "strutil.h"

namespace zfsmgr::commands::snapshots {

namespace B = zfsmgr::base;

std::string scopeFlag(Scope a) {
    switch (a) {
        case Scope::Only:          return {};
        case Scope::Descendants: return "r";
        case Scope::Dependents:  return "R";
    }
    return {};
}

bool reachesBeyondTarget(Scope a) {
    return a != Scope::Only;
}

bool isSnapshot(const std::string& objeto) {
    return objeto.find('@') != std::string::npos;
}

std::string snapshotName(const std::string& dataset, const std::string& nombre) {
    const std::string n = B::trim(nombre);
    if (n.empty()) {
        return {};
    }
    if (n.find('@') != std::string::npos) {
        return n;
    }
    const std::string ds = B::trim(dataset);
    if (ds.empty()) {
        return {};
    }
    return ds + "@" + n;
}

std::vector<std::string> argvCreateSnapshot(const std::string& dataset,
                                              const std::string& nombre, bool recursiva) {
    const std::string completo = snapshotName(dataset, nombre);
    if (completo.empty()) {
        return {};
    }
    return {"--mutate-zfs-snapshot", completo, recursiva ? "1" : "0"};
}

std::vector<std::string> argvDestroy(const std::string& objeto, bool forzar, Scope alcance) {
    const std::string o = B::trim(objeto);
    if (o.empty()) {
        return {};
    }
    return {"--mutate-zfs-destroy", o, forzar ? "1" : "0", scopeFlag(alcance)};
}

std::vector<std::string> argvRollback(const std::string& instantanea, bool forzar,
                                      Scope alcance) {
    const std::string s = B::trim(instantanea);
    // Sin arroba no es una instantánea, y volver atrás a un dataset no significa nada.
    if (s.empty() || !isSnapshot(s)) {
        return {};
    }
    return {"--mutate-zfs-rollback", s, forzar ? "1" : "0", scopeFlag(alcance)};
}

std::vector<std::string> argvClone(const std::string& instantaneaOrigen,
                                    const std::string& datasetNuevo) {
    const std::string o = B::trim(instantaneaOrigen);
    const std::string n = B::trim(datasetNuevo);
    if (o.empty() || n.empty() || !isSnapshot(o)) {
        return {};
    }
    // Y el destino NO puede llevar arroba: sería clonar sobre una instantánea, que no existe.
    if (isSnapshot(n)) {
        return {};
    }
    return {"--mutate-zfs-clone", o, n};
}

std::vector<std::string> argvZfsClone(const std::string& instantaneaOrigen,
                                       const std::string& datasetNuevo,
                                       const std::vector<std::string>& banderas) {
    const std::string o = B::trim(instantaneaOrigen);
    const std::string n = B::trim(datasetNuevo);
    if (o.empty() || n.empty() || !isSnapshot(o) || isSnapshot(n)) {
        return {};
    }
    std::vector<std::string> out{"clone"};
    for (const std::string& b : banderas) {
        const std::string t = B::trim(b);
        if (!t.empty()) {
            out.push_back(t);
        }
    }
    out.push_back(o);
    out.push_back(n);
    return out;
}

bool isValidTag(const std::string& etiqueta) {
    const std::string t = B::trim(etiqueta);
    if (t.empty()) {
        return false;
    }
    return t.find(' ') == std::string::npos && t.find('@') == std::string::npos
           && t.find('/') == std::string::npos && t.find('\t') == std::string::npos;
}

namespace {

// Comprueba el par y devuelve false si no sirve. Se comparte entre las cuatro formas.
bool parValido(const std::string& etiqueta, const std::string& instantanea) {
    return isValidTag(etiqueta) && !B::trim(instantanea).empty()
           && isSnapshot(B::trim(instantanea));
}

}  // namespace

std::vector<std::string> argvHold(const std::string& etiqueta,
                                     const std::string& instantanea) {
    if (!parValido(etiqueta, instantanea)) {
        return {};
    }
    return {"--mutate-zfs-hold", B::trim(etiqueta), B::trim(instantanea)};
}

std::vector<std::string> argvRelease(const std::string& etiqueta,
                                    const std::string& instantanea) {
    if (!parValido(etiqueta, instantanea)) {
        return {};
    }
    return {"--mutate-zfs-release", B::trim(etiqueta), B::trim(instantanea)};
}

namespace {

std::vector<std::string> argvZfsRetencion(const char* sub, const std::string& etiqueta,
                                          const std::string& instantanea, bool recursivo) {
    if (!parValido(etiqueta, instantanea)) {
        return {};
    }
    std::vector<std::string> out{sub};
    if (recursivo) {
        out.push_back("-r");
    }
    out.push_back(B::trim(etiqueta));
    out.push_back(B::trim(instantanea));
    return out;
}

}  // namespace

std::vector<std::string> argvZfsHold(const std::string& etiqueta,
                                        const std::string& instantanea, bool recursivo) {
    return argvZfsRetencion("hold", etiqueta, instantanea, recursivo);
}

std::vector<std::string> argvZfsRelease(const std::string& etiqueta,
                                       const std::string& instantanea, bool recursivo) {
    return argvZfsRetencion("release", etiqueta, instantanea, recursivo);
}

}  // namespace zfsmgr::commands::snapshots
