#include "syncing.h"

#include "helpers.h"
#include "strutil.h"

namespace zfsmgr::base::syncing {

std::string labelOf(Failure f) {
    switch (f) {
        case Failure::None_:
            return {};
        case Failure::SameObject:
            return "el origen y el destino son el mismo";
        case Failure::SourceIsNotDataset:
            return "el origen tiene que ser un dataset, no una instantánea";
        case Failure::TargetIsNotDataset:
            return "el destino tiene que ser un dataset, no una instantánea";
        case Failure::WindowsEndpoint:
            // Ya no lo devuelve nadie: se queda para no romper a quien lo lea de un dato
            // guardado, y porque el `switch` tiene que ser exhaustivo.
            return "un extremo Windows por este camino";
        case Failure::DifferentMachine:
            return "los dos extremos tienen que estar en la misma máquina";
        case Failure::SourceNotMounted:
            return "el origen no está montado, y sincronizar compara ficheros";
        case Failure::TargetNotMounted:
            return "el destino no está montado, y sincronizar compara ficheros";
        case Failure::UnusablePath:
            return "alguno de los dos no tiene un punto de montaje utilizable";
        case Failure::NoDaemon:
            return "hace falta el daemon en esa máquina";
    }
    return {};
}

bool isUsablePath(const std::string& ruta, bool esWindows) {
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

Failure check(const Endpoint& origen, const Endpoint& destino) {
    if (origen.connection == destino.connection && origen.object == destino.object) {
        return Failure::SameObject;
    }
    if (origen.object.find('@') != std::string::npos) {
        return Failure::SourceIsNotDataset;
    }
    if (destino.object.find('@') != std::string::npos) {
        return Failure::TargetIsNotDataset;
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
    if (!origen.hasDaemon || !destino.hasDaemon) {
        return Failure::NoDaemon;
    }
    return Failure::None_;
}

Plan makePlan(const Endpoint& origen, const Endpoint& destino) {
    Plan plan;
    plan.failure = check(origen, destino);
    if (plan.failure != Failure::None_) {
        return plan;
    }
    if (!origen.mounted) {
        plan.failure = Failure::SourceNotMounted;
        return plan;
    }
    if (!destino.mounted) {
        plan.failure = Failure::TargetNotMounted;
        return plan;
    }
    if (!isUsablePath(origen.mountpoint, origen.isWindows)
        || !isUsablePath(destino.mountpoint, destino.isWindows)) {
        plan.failure = Failure::UnusablePath;
        return plan;
    }
    plan.sourcePath = trim(origen.mountpoint);
    plan.targetPath = trim(destino.mountpoint);
    return plan;
}

std::string rsyncPayload(const std::vector<std::pair<std::string, std::string>>& pares,
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
        if (!isUsablePath(o) || !isUsablePath(d)) {
            return {};
        }
        campos.push_back(o);
        campos.push_back(d);
    }
    return helpers::agentArgv(campos);
}

}  // namespace zfsmgr::base::syncing
