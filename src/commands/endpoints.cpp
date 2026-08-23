#include "endpoints.h"

namespace zfsmgr::base::endpoints {

const char* keyOf(Action a) {
    switch (a) {
        case Action::Diff:        return "diff";
        case Action::Clonar:      return "clonar";
        case Action::Copiar:      return "copiar";
        case Action::Mover:       return "mover";
        case Action::Sincronizar: return "sincronizar";
        case Action::Nivelar:     return "nivelar";
    }
    return "";
}

std::string labelOf(Action a) {
    switch (a) {
        case Action::Diff:        return "Comparar";
        case Action::Clonar:      return "Clonar aquí";
        case Action::Copiar:      return "Copiar aquí";
        case Action::Mover:       return "Mover aquí";
        case Action::Sincronizar: return "Sincronizar aquí";
        case Action::Nivelar:     return "Nivelar";
    }
    return {};
}

std::string labelOf(NotApplicable n) {
    switch (n) {
        case NotApplicable::Ninguna:
            return {};
        case NotApplicable::SinOrigen:
            return "no hay ningún origen marcado";
        case NotApplicable::ElMismoObjeto:
            return "el origen y el destino son el mismo";
        case NotApplicable::OrigenNoEsInstantanea:
            return "el origen tiene que ser una instantánea";
        case NotApplicable::DestinoNoEsDataset:
            return "el destino tiene que ser un dataset, no una instantánea";
        case NotApplicable::DistintoDataset:
            return "comparar es entre dos puntos del mismo dataset";
        case NotApplicable::DistintaMaquina:
            return "los dos extremos tienen que estar en la misma máquina";
        case NotApplicable::DistintoPool:
            return "mover es dentro del mismo pool; entre pools se copia";
        case NotApplicable::OrigenNoEsDataset:
            return "el origen tiene que ser un dataset, no una instantánea";
        case NotApplicable::DestinoDentroDelOrigen:
            return "el destino cuelga del origen: no se puede meter dentro de sí mismo";
        case NotApplicable::TodaviaNoEstaEnLaWeb:
            return "todavía no está en la web: hágalo desde la interfaz o el intérprete";
    }
    return {};
}

NotApplicable check(Action a, const Endpoint& origen, const Endpoint& destino) {
    if (origen.vacio()) {
        return NotApplicable::SinOrigen;
    }
    if (origen.conexion == destino.conexion && origen.objeto == destino.objeto) {
        return NotApplicable::ElMismoObjeto;
    }
    switch (a) {
        case Action::Diff:
            // `zfs diff` compara dos puntos de la MISMA historia: dos instantáneas del
            // mismo dataset, o una instantánea contra el estado actual de su dataset. No
            // sirve para comparar dos datasets distintos, que es lo que la gente espera la
            // primera vez.
            if (origen.conexion != destino.conexion) {
                return NotApplicable::DistintaMaquina;
            }
            if (!origen.isSnapshot()) {
                return NotApplicable::OrigenNoEsInstantanea;
            }
            if (origen.dataset() != destino.dataset()) {
                return NotApplicable::DistintoDataset;
            }
            return NotApplicable::Ninguna;
        case Action::Clonar:
            // Un clon nace de una instantánea y aparece como un dataset nuevo. El destino
            // marca DÓNDE, así que tiene que ser un dataset: colgar un clon de una
            // instantánea no significa nada.
            if (origen.conexion != destino.conexion) {
                return NotApplicable::DistintaMaquina;
            }
            if (!origen.isSnapshot()) {
                return NotApplicable::OrigenNoEsInstantanea;
            }
            if (destino.isSnapshot()) {
                return NotApplicable::DestinoNoEsDataset;
            }
            return NotApplicable::Ninguna;
        case Action::Mover:
            // **Mover NO es copiar y destruir.** Es un `zfs rename`, que ZFS solo deja
            // dentro del mismo pool: el dataset cambia de sitio en el árbol sin que se
            // muevan los datos, y por eso es instantáneo y no hay nada que destruir
            // después. La interfaz de Qt hace exactamente esto —lo enqueue como cambio
            // pendiente— y aquí se replican sus mismas condiciones.
            //
            // Este documento decía «Copiar + destruir el origen». Era falso, y se vio al
            // leer `executeConnectionTransferAction`. Se deja escrito porque la versión
            // equivocada es más plausible que la verdadera y volverá a proponerse.
            if (origen.conexion != destino.conexion) {
                return NotApplicable::DistintaMaquina;
            }
            if (origen.isSnapshot()) {
                return NotApplicable::OrigenNoEsDataset;
            }
            if (destino.isSnapshot()) {
                return NotApplicable::DestinoNoEsDataset;
            }
            if (origen.pool() != destino.pool()) {
                return NotApplicable::DistintoPool;
            }
            // Meter un dataset bajo uno de sus propios descendientes no tiene sentido y
            // ZFS lo rechaza. Se compara con la barra puesta para que «tanque/datos» no
            // parezca padre de «tanque/datos2».
            if (destino.dataset() == origen.dataset()
                || destino.dataset().rfind(origen.dataset() + "/", 0) == 0) {
                return NotApplicable::DestinoDentroDelOrigen;
            }
            return NotApplicable::Ninguna;
        case Action::Copiar:
        case Action::Sincronizar:
        case Action::Nivelar:
            // Las cuatro necesitan la orquestación de transfer, que hoy vive dentro
            // de la interfaz —`mainwindow_transfer.cpp`— y no en esta capa. Se ofrecen
            // igual, en gris y con el motivo: esconderlas haría creer que no existen.
            return NotApplicable::TodaviaNoEstaEnLaWeb;
    }
    return NotApplicable::Ninguna;
}

std::string moveDestination(const Endpoint& origen, const Endpoint& destino) {
    return destino.dataset() + "/" + origen.hoja();
}

}  // namespace zfsmgr::base::endpoints
