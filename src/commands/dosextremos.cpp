#include "dosextremos.h"

namespace zfsmgr::base::dosextremos {

const char* claveDe(Accion a) {
    switch (a) {
        case Accion::Diff:        return "diff";
        case Accion::Clonar:      return "clonar";
        case Accion::Copiar:      return "copiar";
        case Accion::Mover:       return "mover";
        case Accion::Sincronizar: return "sincronizar";
        case Accion::Nivelar:     return "nivelar";
    }
    return "";
}

std::string etiquetaDe(Accion a) {
    switch (a) {
        case Accion::Diff:        return "Comparar";
        case Accion::Clonar:      return "Clonar aquí";
        case Accion::Copiar:      return "Copiar aquí";
        case Accion::Mover:       return "Mover aquí";
        case Accion::Sincronizar: return "Sincronizar aquí";
        case Accion::Nivelar:     return "Nivelar";
    }
    return {};
}

std::string etiquetaDe(NoAplica n) {
    switch (n) {
        case NoAplica::Ninguna:
            return {};
        case NoAplica::SinOrigen:
            return "no hay ningún origen marcado";
        case NoAplica::ElMismoObjeto:
            return "el origen y el destino son el mismo";
        case NoAplica::OrigenNoEsInstantanea:
            return "el origen tiene que ser una instantánea";
        case NoAplica::DestinoNoEsDataset:
            return "el destino tiene que ser un dataset, no una instantánea";
        case NoAplica::DistintoDataset:
            return "comparar es entre dos puntos del mismo dataset";
        case NoAplica::DistintaMaquina:
            return "los dos extremos tienen que estar en la misma máquina";
        case NoAplica::DistintoPool:
            return "mover es dentro del mismo pool; entre pools se copia";
        case NoAplica::OrigenNoEsDataset:
            return "el origen tiene que ser un dataset, no una instantánea";
        case NoAplica::DestinoDentroDelOrigen:
            return "el destino cuelga del origen: no se puede meter dentro de sí mismo";
        case NoAplica::TodaviaNoEstaEnLaWeb:
            return "todavía no está en la web: hágalo desde la interfaz o el intérprete";
    }
    return {};
}

NoAplica compruebo(Accion a, const Extremo& origen, const Extremo& destino) {
    if (origen.vacio()) {
        return NoAplica::SinOrigen;
    }
    if (origen.conexion == destino.conexion && origen.objeto == destino.objeto) {
        return NoAplica::ElMismoObjeto;
    }
    switch (a) {
        case Accion::Diff:
            // `zfs diff` compara dos puntos de la MISMA historia: dos instantáneas del
            // mismo dataset, o una instantánea contra el estado actual de su dataset. No
            // sirve para comparar dos datasets distintos, que es lo que la gente espera la
            // primera vez.
            if (origen.conexion != destino.conexion) {
                return NoAplica::DistintaMaquina;
            }
            if (!origen.esInstantanea()) {
                return NoAplica::OrigenNoEsInstantanea;
            }
            if (origen.dataset() != destino.dataset()) {
                return NoAplica::DistintoDataset;
            }
            return NoAplica::Ninguna;
        case Accion::Clonar:
            // Un clon nace de una instantánea y aparece como un dataset nuevo. El destino
            // marca DÓNDE, así que tiene que ser un dataset: colgar un clon de una
            // instantánea no significa nada.
            if (origen.conexion != destino.conexion) {
                return NoAplica::DistintaMaquina;
            }
            if (!origen.esInstantanea()) {
                return NoAplica::OrigenNoEsInstantanea;
            }
            if (destino.esInstantanea()) {
                return NoAplica::DestinoNoEsDataset;
            }
            return NoAplica::Ninguna;
        case Accion::Mover:
            // **Mover NO es copiar y destruir.** Es un `zfs rename`, que ZFS solo deja
            // dentro del mismo pool: el dataset cambia de sitio en el árbol sin que se
            // muevan los datos, y por eso es instantáneo y no hay nada que destruir
            // después. La interfaz de Qt hace exactamente esto —lo encola como cambio
            // pendiente— y aquí se replican sus mismas condiciones.
            //
            // Este documento decía «Copiar + destruir el origen». Era falso, y se vio al
            // leer `executeConnectionTransferAction`. Se deja escrito porque la versión
            // equivocada es más plausible que la verdadera y volverá a proponerse.
            if (origen.conexion != destino.conexion) {
                return NoAplica::DistintaMaquina;
            }
            if (origen.esInstantanea()) {
                return NoAplica::OrigenNoEsDataset;
            }
            if (destino.esInstantanea()) {
                return NoAplica::DestinoNoEsDataset;
            }
            if (origen.pool() != destino.pool()) {
                return NoAplica::DistintoPool;
            }
            // Meter un dataset bajo uno de sus propios descendientes no tiene sentido y
            // ZFS lo rechaza. Se compara con la barra puesta para que «tanque/datos» no
            // parezca padre de «tanque/datos2».
            if (destino.dataset() == origen.dataset()
                || destino.dataset().rfind(origen.dataset() + "/", 0) == 0) {
                return NoAplica::DestinoDentroDelOrigen;
            }
            return NoAplica::Ninguna;
        case Accion::Copiar:
        case Accion::Sincronizar:
        case Accion::Nivelar:
            // Las cuatro necesitan la orquestación de transferencia, que hoy vive dentro
            // de la interfaz —`mainwindow_transfer.cpp`— y no en esta capa. Se ofrecen
            // igual, en gris y con el motivo: esconderlas haría creer que no existen.
            return NoAplica::TodaviaNoEstaEnLaWeb;
    }
    return NoAplica::Ninguna;
}

std::string destinoDeMover(const Extremo& origen, const Extremo& destino) {
    return destino.dataset() + "/" + origen.hoja();
}

}  // namespace zfsmgr::base::dosextremos
