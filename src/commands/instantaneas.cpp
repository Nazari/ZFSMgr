#include "instantaneas.h"

#include "strutil.h"

namespace zfsmgr::commands::instantaneas {

namespace B = zfsmgr::base;

std::string letraDeAlcance(Alcance a) {
    switch (a) {
        case Alcance::Solo:          return {};
        case Alcance::Descendientes: return "r";
        case Alcance::Dependientes:  return "R";
    }
    return {};
}

bool arrastraOtros(Alcance a) {
    return a != Alcance::Solo;
}

bool esInstantanea(const std::string& objeto) {
    return objeto.find('@') != std::string::npos;
}

std::string nombreDeInstantanea(const std::string& dataset, const std::string& nombre) {
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

std::vector<std::string> argvCrearInstantanea(const std::string& dataset,
                                              const std::string& nombre, bool recursiva) {
    const std::string completo = nombreDeInstantanea(dataset, nombre);
    if (completo.empty()) {
        return {};
    }
    return {"--mutate-zfs-snapshot", completo, recursiva ? "1" : "0"};
}

std::vector<std::string> argvDestruir(const std::string& objeto, bool forzar, Alcance alcance) {
    const std::string o = B::trim(objeto);
    if (o.empty()) {
        return {};
    }
    return {"--mutate-zfs-destroy", o, forzar ? "1" : "0", letraDeAlcance(alcance)};
}

std::vector<std::string> argvRollback(const std::string& instantanea, bool forzar,
                                      Alcance alcance) {
    const std::string s = B::trim(instantanea);
    // Sin arroba no es una instantánea, y volver atrás a un dataset no significa nada.
    if (s.empty() || !esInstantanea(s)) {
        return {};
    }
    return {"--mutate-zfs-rollback", s, forzar ? "1" : "0", letraDeAlcance(alcance)};
}

std::vector<std::string> argvClonar(const std::string& instantaneaOrigen,
                                    const std::string& datasetNuevo) {
    const std::string o = B::trim(instantaneaOrigen);
    const std::string n = B::trim(datasetNuevo);
    if (o.empty() || n.empty() || !esInstantanea(o)) {
        return {};
    }
    // Y el destino NO puede llevar arroba: sería clonar sobre una instantánea, que no existe.
    if (esInstantanea(n)) {
        return {};
    }
    return {"--mutate-zfs-clone", o, n};
}

std::vector<std::string> argvZfsClonar(const std::string& instantaneaOrigen,
                                       const std::string& datasetNuevo,
                                       const std::vector<std::string>& banderas) {
    const std::string o = B::trim(instantaneaOrigen);
    const std::string n = B::trim(datasetNuevo);
    if (o.empty() || n.empty() || !esInstantanea(o) || esInstantanea(n)) {
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

bool etiquetaValida(const std::string& etiqueta) {
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
    return etiquetaValida(etiqueta) && !B::trim(instantanea).empty()
           && esInstantanea(B::trim(instantanea));
}

}  // namespace

std::vector<std::string> argvRetener(const std::string& etiqueta,
                                     const std::string& instantanea) {
    if (!parValido(etiqueta, instantanea)) {
        return {};
    }
    return {"--mutate-zfs-hold", B::trim(etiqueta), B::trim(instantanea)};
}

std::vector<std::string> argvSoltar(const std::string& etiqueta,
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

std::vector<std::string> argvZfsRetener(const std::string& etiqueta,
                                        const std::string& instantanea, bool recursivo) {
    return argvZfsRetencion("hold", etiqueta, instantanea, recursivo);
}

std::vector<std::string> argvZfsSoltar(const std::string& etiqueta,
                                       const std::string& instantanea, bool recursivo) {
    return argvZfsRetencion("release", etiqueta, instantanea, recursivo);
}

}  // namespace zfsmgr::commands::instantaneas
