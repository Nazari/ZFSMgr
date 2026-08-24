#include "endpoints.h"

namespace zfsmgr::base::endpoints {

const char* keyOf(Action a) {
    switch (a) {
        case Action::Diff:        return "diff";
        case Action::Clone:      return "clonar";
        case Action::Send:      return "copiar";
        case Action::Move:       return "mover";
        case Action::Sync: return "sincronizar";
        case Action::Level:     return "nivelar";
    }
    return "";
}

std::string labelOf(Action a) {
    switch (a) {
        case Action::Diff:        return "Comparar";
        case Action::Clone:      return "Clonar aquí";
        case Action::Send:      return "Copiar aquí";
        case Action::Move:       return "Mover aquí";
        case Action::Sync: return "Sincronizar aquí";
        case Action::Level:     return "Nivelar";
    }
    return {};
}

std::string labelOf(NotApplicable n) {
    switch (n) {
        case NotApplicable::None_:
            return {};
        case NotApplicable::NoSource:
            return "no hay ningún origen marcado";
        case NotApplicable::SameObject:
            return "el origen y el destino son el mismo";
        case NotApplicable::SourceIsNotSnapshot:
            return "el origen tiene que ser una instantánea";
        case NotApplicable::TargetIsNotDataset:
            return "el destino tiene que ser un dataset, no una instantánea";
        case NotApplicable::DifferentDataset:
            return "comparar es entre dos puntos del mismo dataset";
        case NotApplicable::DifferentMachine:
            return "los dos extremos tienen que estar en la misma máquina";
        case NotApplicable::DifferentPool:
            return "mover es dentro del mismo pool; entre pools se copia";
        case NotApplicable::SourceIsNotDataset:
            return "el origen tiene que ser un dataset, no una instantánea";
        case NotApplicable::TargetInsideSource:
            return "el destino cuelga del origen: no se puede meter dentro de sí mismo";
        case NotApplicable::NotInTheWebYet:
            return "todavía no está en la web: hágalo desde la interfaz o el intérprete";
    }
    return {};
}

NotApplicable check(Action a, const Endpoint& origen, const Endpoint& destino) {
    if (origen.empty()) {
        return NotApplicable::NoSource;
    }
    if (origen.connection == destino.connection && origen.object == destino.object) {
        return NotApplicable::SameObject;
    }
    switch (a) {
        case Action::Diff:
            // `zfs diff` compara dos puntos de la MISMA historia: dos instantáneas del
            // mismo dataset, o una instantánea contra el estado actual de su dataset. No
            // sirve para comparar dos datasets distintos, que es lo que la gente espera la
            // primera vez.
            if (origen.connection != destino.connection) {
                return NotApplicable::DifferentMachine;
            }
            if (!origen.isSnapshot()) {
                return NotApplicable::SourceIsNotSnapshot;
            }
            if (origen.dataset() != destino.dataset()) {
                return NotApplicable::DifferentDataset;
            }
            return NotApplicable::None_;
        case Action::Clone:
            // Un clon nace de una instantánea y aparece como un dataset nuevo. El destino
            // marca DÓNDE, así que tiene que ser un dataset: colgar un clon de una
            // instantánea no significa nada.
            if (origen.connection != destino.connection) {
                return NotApplicable::DifferentMachine;
            }
            if (!origen.isSnapshot()) {
                return NotApplicable::SourceIsNotSnapshot;
            }
            if (destino.isSnapshot()) {
                return NotApplicable::TargetIsNotDataset;
            }
            return NotApplicable::None_;
        case Action::Move:
            // **Mover NO es copiar y destruir.** Es un `zfs rename`, que ZFS solo deja
            // dentro del mismo pool: el dataset cambia de sitio en el árbol sin que se
            // muevan los datos, y por eso es instantáneo y no hay nada que destruir
            // después. La interfaz de Qt hace exactamente esto —lo encola como cambio
            // pendiente— y aquí se replican sus mismas condiciones.
            //
            // Este documento decía «Send + destruir el origen». Era falso, y se vio al
            // leer `executeConnectionTransferAction`. Se deja escrito porque la versión
            // equivocada es más plausible que la verdadera y volverá a proponerse.
            if (origen.connection != destino.connection) {
                return NotApplicable::DifferentMachine;
            }
            if (origen.isSnapshot()) {
                return NotApplicable::SourceIsNotDataset;
            }
            if (destino.isSnapshot()) {
                return NotApplicable::TargetIsNotDataset;
            }
            if (origen.pool() != destino.pool()) {
                return NotApplicable::DifferentPool;
            }
            // Meter un dataset bajo uno de sus propios descendientes no tiene sentido y
            // ZFS lo rechaza. Se compara con la barra puesta para que «tanque/datos» no
            // parezca padre de «tanque/datos2».
            if (destino.dataset() == origen.dataset()
                || destino.dataset().rfind(origen.dataset() + "/", 0) == 0) {
                return NotApplicable::TargetInsideSource;
            }
            return NotApplicable::None_;
        case Action::Send:
        case Action::Sync:
        case Action::Level:
            // Las cuatro necesitan la orquestación de transfer, que hoy vive dentro
            // de la interfaz —`mainwindow_transfer.cpp`— y no en esta capa. Se ofrecen
            // igual, en gris y con el motivo: esconderlas haría creer que no existen.
            return NotApplicable::NotInTheWebYet;
    }
    return NotApplicable::None_;
}

std::string moveDestination(const Endpoint& origen, const Endpoint& destino) {
    return destino.dataset() + "/" + origen.leaf();
}

}  // namespace zfsmgr::base::endpoints
