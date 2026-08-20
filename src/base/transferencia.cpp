#include "transferencia.h"

#include "strutil.h"

namespace zfsmgr::base::transferencia {

const char* claveDe(Camino c) {
    switch (c) {
        case Camino::TrabajoAsincrono: return "trabajo";
        case Camino::DaemonADaemon:    return "daemon-a-daemon";
        case Camino::TuberiaSsh:       return "tuberia-ssh";
        case Camino::Ninguno:          return "ninguno";
    }
    return "ninguno";
}

const char* claveDe(Fallo f) {
    switch (f) {
        case Fallo::Ninguno:               return "";
        case Fallo::ElMismoObjeto:         return "mismo-objeto";
        case Fallo::OrigenNoEsInstantanea: return "origen-no-instantanea";
        case Fallo::DestinoNoEsDataset:    return "destino-no-dataset";
        case Fallo::ExtremoWindows:        return "extremo-windows";
        case Fallo::SinTrabajos:           return "sin-trabajos";
    }
    return "";
}

std::string etiquetaDe(Camino c) {
    switch (c) {
        case Camino::TrabajoAsincrono: return "como trabajo en el daemon";
        case Camino::DaemonADaemon:    return "de daemon a daemon";
        case Camino::TuberiaSsh:       return "por una tubería SSH";
        case Camino::Ninguno:          return "ninguno";
    }
    return {};
}

std::string etiquetaDe(Fallo f) {
    switch (f) {
        case Fallo::Ninguno:
            return {};
        case Fallo::ElMismoObjeto:
            return "el origen y el destino son el mismo";
        case Fallo::OrigenNoEsInstantanea:
            return "el origen tiene que ser una instantánea";
        case Fallo::DestinoNoEsDataset:
            return "el destino tiene que ser un dataset, no una instantánea";
        case Fallo::ExtremoWindows:
            return "no está disponible cuando algún extremo es Windows: hace falta "
                   "transmitir por una tubería, y el agente de Windows todavía no lo hace";
        case Fallo::SinTrabajos:
            return "hace falta que los dos daemons admitan trabajos en segundo plano, "
                   "porque quien lo pide no puede esperar a que termine";
    }
    return {};
}

Plan planea(const Extremo& origen, const Extremo& destino, bool exigeAsincrono) {
    Plan p;

    // Lo que no depende del camino va primero: no tiene sentido hablar de daemons cuando el
    // problema es que se está copiando algo sobre sí mismo.
    if (origen.conexion == destino.conexion && origen.objeto == destino.objeto) {
        p.fallo = Fallo::ElMismoObjeto;
        return p;
    }
    if (!origen.esInstantanea()) {
        p.fallo = Fallo::OrigenNoEsInstantanea;
        return p;
    }
    if (destino.esInstantanea()) {
        p.fallo = Fallo::DestinoNoEsDataset;
        return p;
    }

    // Windows corta TODO, no solo un camino.
    //
    // Los dos que quedan necesitan transmitir por una tubería, y el agente de Windows no lo
    // hace; el tercero es un guion de shell POSIX que allí no puede ejecutarse desde que se
    // retiró MSYS2. Encolarlo igualmente hacía que PowerShell devolviera su objeto de error
    // en XML y el usuario viera un «<Objs Version="1.1.0.1">…» que no guarda ninguna
    // relación aparente con la copia que había pedido.
    if (origen.esWindows || destino.esWindows) {
        p.fallo = Fallo::ExtremoWindows;
        return p;
    }

    const bool hayLosDosDaemons = origen.tieneDaemon && destino.tieneDaemon;
    const bool hayTrabajos = hayLosDosDaemons && origen.admiteTrabajos && destino.admiteTrabajos;

    if (hayTrabajos) {
        p.caminos.push_back(Camino::TrabajoAsincrono);
    }
    if (exigeAsincrono) {
        // Para quien no puede esperar, los otros dos no son un respaldo: son otra cosa que
        // no puede hacer. Mejor decir que no que empezar algo que se va a cortar.
        if (p.caminos.empty()) {
            p.fallo = Fallo::SinTrabajos;
        }
        return p;
    }
    if (hayLosDosDaemons) {
        p.caminos.push_back(Camino::DaemonADaemon);
    }
    // La tubería SSH no necesita daemon en ningún extremo: manda `zfs send` y `zfs recv`
    // por SSH. Es lo que queda cuando no hay daemon, y por eso siempre entra en la lista.
    p.caminos.push_back(Camino::TuberiaSsh);
    return p;
}

Reanudacion testigoDeReanudacion(const std::string& objetivo, const std::string& salidaTsv) {
    Reanudacion r;
    const std::string diana = trim(objetivo);
    std::vector<std::pair<std::string, std::string>> conTestigo;
    for (const std::string& linea : split(salidaTsv, "\n", true)) {
        const std::vector<std::string> col = split(linea, "\t", false);
        if (col.size() < 2) {
            continue;
        }
        const std::string ds = trim(col[0]);
        const std::string valor = trim(col[1]);
        // ZFS escribe «-» cuando no hay ninguno. Y que una línea falte no significa que no
        // haya nada a medias: puede que el dataset aún no exista, que es lo normal en una
        // copia nueva.
        if (valor.empty() || valor == "-") {
            continue;
        }
        conTestigo.push_back({ds, valor});
    }
    // El del propio objetivo manda sobre los de sus descendientes.
    for (const auto& kv : conTestigo) {
        if (kv.first == diana) {
            r.testigo = kv.second;
            r.quienLoTiene = kv.first;
            return r;
        }
    }
    if (!conTestigo.empty()) {
        r.testigo = conTestigo.front().second;
        r.quienLoTiene = conTestigo.front().first;
    }
    return r;
}

}  // namespace zfsmgr::base::transferencia
