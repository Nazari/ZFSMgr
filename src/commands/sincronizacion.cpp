#include "sincronizacion.h"

#include "helpers.h"
#include "strutil.h"

namespace zfsmgr::base::sincronizacion {

std::string etiquetaDe(Fallo f) {
    switch (f) {
        case Fallo::Ninguno:
            return {};
        case Fallo::ElMismoObjeto:
            return "el origen y el destino son el mismo";
        case Fallo::OrigenNoEsDataset:
            return "el origen tiene que ser un dataset, no una instantánea";
        case Fallo::DestinoNoEsDataset:
            return "el destino tiene que ser un dataset, no una instantánea";
        case Fallo::ExtremoWindows:
            // Ya no lo devuelve nadie: se queda para no romper a quien lo lea de un dato
            // guardado, y porque el `switch` tiene que ser exhaustivo.
            return "un extremo Windows por este camino";
        case Fallo::DistintaMaquina:
            return "los dos extremos tienen que estar en la misma máquina";
        case Fallo::OrigenNoMontado:
            return "el origen no está montado, y sincronizar compara ficheros";
        case Fallo::DestinoNoMontado:
            return "el destino no está montado, y sincronizar compara ficheros";
        case Fallo::RutaNoUsable:
            return "alguno de los dos no tiene un punto de montaje utilizable";
        case Fallo::SinDaemon:
            return "hace falta el daemon en esa máquina";
    }
    return {};
}

bool rutaUsable(const std::string& ruta, bool esWindows) {
    const std::string r = trim(ruta);
    // «none» y «-» son lo que responde ZFS cuando NO hay punto de montaje; tratarlos como
    // una ruta cualquiera acababa pasándoselos a rsync, que se quejaba de algo que no era
    // el problema.
    if (r.empty() || r == "none" || r == "-" || r == "legacy") {
        return false;
    }
    if (esWindows) {
        return r.find(':') != std::string::npos;
    }
    return r[0] == '/';
}

Fallo compruebo(const Extremo& origen, const Extremo& destino) {
    if (origen.conexion == destino.conexion && origen.objeto == destino.objeto) {
        return Fallo::ElMismoObjeto;
    }
    if (origen.objeto.find('@') != std::string::npos) {
        return Fallo::OrigenNoEsDataset;
    }
    if (destino.objeto.find('@') != std::string::npos) {
        return Fallo::DestinoNoEsDataset;
    }
    // Windows ya no estorba en ningún caso.
    //
    // Estorbaba dentro de una misma máquina, porque allí el camino era rsync y rsync no
    // existe en Windows. Ahora también ese caso va por el ÁRBOL, conectando el daemon
    // consigo mismo por el bucle local: el mismo mecanismo que entre máquinas, sin rsync
    // en ninguna parte. Tener dos caminos según la plataforma era tener uno de los dos sin
    // probar la mitad de las veces.
    // Entre máquinas ya se puede: va por el árbol por el socket entre daemons, que no
    // necesita rsync en ninguno de los dos lados. Por eso aquí ya no se rechaza.
    if (!origen.tieneDaemon || !destino.tieneDaemon) {
        return Fallo::SinDaemon;
    }
    return Fallo::Ninguno;
}

Plan planea(const Extremo& origen, const Extremo& destino) {
    Plan plan;
    plan.fallo = compruebo(origen, destino);
    if (plan.fallo != Fallo::Ninguno) {
        return plan;
    }
    if (!origen.montado) {
        plan.fallo = Fallo::OrigenNoMontado;
        return plan;
    }
    if (!destino.montado) {
        plan.fallo = Fallo::DestinoNoMontado;
        return plan;
    }
    if (!rutaUsable(origen.puntoMontaje, origen.esWindows)
        || !rutaUsable(destino.puntoMontaje, destino.esWindows)) {
        plan.fallo = Fallo::RutaNoUsable;
        return plan;
    }
    plan.rutaOrigen = trim(origen.puntoMontaje);
    plan.rutaDestino = trim(destino.puntoMontaje);
    return plan;
}

std::string cargaRsync(const std::vector<std::pair<std::string, std::string>>& pares,
                       bool borrar, bool enSeco,
                       const std::string& rsh, const std::string& hostDestino) {
    if (pares.empty()) {
        return {};
    }
    // La carga es una lista de cadenas, igual que la de cualquier verbo genérico: se
    // empaqueta con la misma función y no con un JSON escrito aquí. Lo único propio de
    // rsync es el ORDEN de los campos, que sí es cosa de este módulo.
    std::vector<std::string> campos = {borrar ? "1" : "0", enSeco ? "1" : "0", rsh, hostDestino};
    for (const auto& par : pares) {
        const std::string o = trim(par.first);
        const std::string d = trim(par.second);
        if (!rutaUsable(o) || !rutaUsable(d)) {
            return {};
        }
        campos.push_back(o);
        campos.push_back(d);
    }
    return helpers::argvParaAgente(campos);
}

}  // namespace zfsmgr::base::sincronizacion
