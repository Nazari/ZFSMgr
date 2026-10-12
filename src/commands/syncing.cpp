#include "syncing.h"
#include "i18n.h"

#include "helpers.h"
#include "strutil.h"

namespace zfsmgr::base::syncing {

std::string labelOf(Failure f) {
    switch (f) {
        case Failure::None_:
            return {};
        case Failure::SameObject:
            return i18n::tr("t_cmd_syncing_origen_destino_son_mismo", "el origen y el destino son el mismo");
        case Failure::SourceIsNotDataset:
            return i18n::tr("t_cmd_syncing_origen_tiene_ser_dataset", "el origen tiene que ser un dataset, no un snapshot");
        case Failure::TargetIsNotDataset:
            return i18n::tr("t_cmd_syncing_destino_tiene_ser_dataset", "el destino tiene que ser un dataset, no un snapshot");
        case Failure::WindowsEndpoint:
            // Ya no lo devuelve nadie: se queda para no romper a quien lo lea de un dato
            // guardado, y porque el `switch` tiene que ser exhaustivo.
            return i18n::tr("t_cmd_syncing_extremo_windows_por_este", "un extremo Windows por este camino");
        case Failure::DifferentMachine:
            return i18n::tr("t_cmd_syncing_dos_extremos_tienen_estar", "los dos extremos tienen que estar en la misma máquina");
        case Failure::SourceNotMounted:
            return i18n::tr("t_cmd_syncing_origen_esta_montado_sincronizar", "el origen no está montado, y sincronizar compara ficheros");
        case Failure::TargetNotMounted:
            return i18n::tr("t_cmd_syncing_destino_esta_montado_sincronizar", "el destino no está montado, y sincronizar compara ficheros");
        case Failure::UnusablePath:
            return i18n::tr("t_cmd_syncing_alguno_dos_tiene_punto", "alguno de los dos no tiene un punto de montaje utilizable");
        case Failure::NoDaemon:
            return i18n::tr("t_cmd_syncing_hace_falta_daemon_esa", "hace falta el daemon en esa máquina");
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

std::string copyLocalPayload(const std::vector<std::pair<std::string, std::string>>& pares,
                             bool borrar, bool enSeco) {
    if (pares.empty()) {
        return {};
    }
    std::vector<std::string> campos = {borrar ? "1" : "0", enSeco ? "1" : "0"};
    for (const auto& par : pares) {
        // Absolutas las dos, como en la de rsync: el daemon rechaza lo que no empiece por
        // barra, y dejarlo pasar aquí solo cambia dónde falla.
        if (par.first.empty() || par.first.front() != '/' || par.second.empty()
            || par.second.front() != '/') {
            return {};
        }
        campos.push_back(par.first);
        campos.push_back(par.second);
    }
    return helpers::agentArgv(campos);
}

}  // namespace zfsmgr::base::syncing
