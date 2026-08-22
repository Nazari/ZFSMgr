#include "datasets.h"

#include "strutil.h"

namespace zfsmgr::commands::datasets {

namespace B = zfsmgr::base;

bool nombreValido(const std::string& nombre) {
    const std::string n = B::trim(nombre);
    if (n.empty()) {
        return false;
    }
    // La arroba haría una instantánea y el espacio rompe cualquier troceo posterior.
    if (n.find('@') != std::string::npos || n.find(' ') != std::string::npos
        || n.find('\t') != std::string::npos) {
        return false;
    }
    // Ni empezar ni acabar en barra: `tank/` y `/tank` no nombran nada.
    if (n.front() == '/' || n.back() == '/') {
        return false;
    }
    return n.find("//") == std::string::npos;
}

std::string nombreDeRenombrado(const std::string& actual, const std::string& nuevo) {
    const std::string a = B::trim(actual);
    const std::string n = B::trim(nuevo);
    if (n.empty()) {
        return {};
    }
    if (n.find('/') != std::string::npos) {
        return n;  // ruta completa: se mueve donde diga
    }
    const std::size_t barra = a.rfind('/');
    if (barra == std::string::npos) {
        // El actual es un pool raíz: no hay padre que anteponer.
        return n;
    }
    return a.substr(0, barra + 1) + n;
}

std::vector<std::string> argvRenombrar(const std::string& actual, const std::string& nuevo) {
    const std::string a = B::trim(actual);
    const std::string destino = nombreDeRenombrado(a, nuevo);
    if (a.empty() || destino.empty() || !nombreValido(a) || !nombreValido(destino)) {
        return {};
    }
    if (a == destino) {
        return {};  // renombrar a lo mismo no es una orden, es un no-op
    }
    return {"rename", a, destino};
}

std::string nombreDeHijo(const std::string& padre, const std::string& hoja) {
    const std::string h = B::trim(hoja);
    if (h.empty()) {
        return {};
    }
    if (h.find('/') != std::string::npos) {
        return h;
    }
    const std::string p = B::trim(padre);
    if (p.empty()) {
        return h;
    }
    return p + "/" + h;
}

std::vector<std::string> argvCrear(const std::string& dataset,
                                   const std::vector<std::string>& propiedades, bool padres) {
    const std::string ds = B::trim(dataset);
    if (!nombreValido(ds)) {
        return {};
    }
    std::vector<std::string> out{"create"};
    if (padres) {
        out.push_back("-p");
    }
    for (const std::string& pv : propiedades) {
        const std::string t = B::trim(pv);
        // Sin «=» no es una propiedad: sería un argumento suelto que zfs leería como el
        // nombre del dataset.
        if (t.empty() || t.find('=') == std::string::npos) {
            continue;
        }
        out.push_back("-o");
        out.push_back(t);
    }
    out.push_back(ds);
    return out;
}

std::vector<std::string> argvPromover(const std::string& dataset) {
    const std::string ds = B::trim(dataset);
    if (!nombreValido(ds)) {
        return {};
    }
    return {"promote", ds};
}

namespace {

std::vector<std::string> argvMontaje(const char* sub, const std::string& dataset, bool forzar) {
    const std::string ds = B::trim(dataset);
    if (!nombreValido(ds)) {
        return {};
    }
    std::vector<std::string> out{sub};
    if (forzar) {
        out.push_back("-f");
    }
    out.push_back(ds);
    return out;
}

}  // namespace

std::vector<std::string> argvMontar(const std::string& dataset, bool forzar) {
    return argvMontaje("mount", dataset, forzar);
}

std::vector<std::string> argvDesmontar(const std::string& dataset, bool forzar) {
    return argvMontaje("unmount", dataset, forzar);
}

std::vector<std::string> argvPonerPropiedad(const std::string& dataset,
                                            const std::string& propiedad,
                                            const std::string& valor) {
    const std::string ds = B::trim(dataset);
    const std::string p = B::trim(propiedad);
    if (ds.empty() || p.empty() || p.find('=') != std::string::npos) {
        return {};
    }
    return {"set", p + "=" + valor, ds};
}

std::vector<std::string> argvHeredarPropiedad(const std::string& dataset,
                                              const std::string& propiedad) {
    const std::string ds = B::trim(dataset);
    const std::string p = B::trim(propiedad);
    if (ds.empty() || p.empty()) {
        return {};
    }
    return {"inherit", p, ds};
}

}  // namespace zfsmgr::commands::datasets
