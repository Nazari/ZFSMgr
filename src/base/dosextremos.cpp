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
        case Accion::Copiar:
        case Accion::Mover:
        case Accion::Sincronizar:
        case Accion::Nivelar:
            // Las cuatro necesitan la orquestación de transferencia, que hoy vive dentro
            // de la interfaz —`mainwindow_transfer.cpp`— y no en esta capa. Se ofrecen
            // igual, en gris y con el motivo: esconderlas haría creer que no existen.
            return NoAplica::TodaviaNoEstaEnLaWeb;
    }
    return NoAplica::Ninguna;
}

}  // namespace zfsmgr::base::dosextremos
