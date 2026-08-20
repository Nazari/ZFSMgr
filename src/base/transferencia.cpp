#include "transferencia.h"

#include "strutil.h"
#include "transportcmd.h"
#include "transportrpc.h"

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
        case Fallo::ZfsDemasiadoViejo:     return "zfs-viejo";
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
        case Fallo::ZfsDemasiadoViejo:
            return "alguno de los extremos usa un OpenZFS anterior al 2.3.3";
    }
    return {};
}

bool versionAdmiteTransferencia(const std::string& version) {
    const std::string v = trim(version);
    if (v.empty()) {
        return true;   // no saberla no es saber que es vieja
    }
    // «2.3.3», «2.2.99-1», «2.3»… Se leen los tres primeros números y se para en cuanto
    // deja de haberlos: el sufijo de distribución no dice nada del formato del flujo.
    int n[3] = {0, 0, 0};
    std::size_t i = 0;
    for (int parte = 0; parte < 3; ++parte) {
        if (i >= v.size() || v[i] < '0' || v[i] > '9') {
            if (parte == 0) {
                return true;   // no empieza por un número: no se entiende, no se bloquea
            }
            break;
        }
        int valor = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            valor = valor * 10 + (v[i] - '0');
            ++i;
        }
        n[parte] = valor;
        if (i < v.size() && v[i] == '.') {
            ++i;
        } else {
            break;
        }
    }
    if (n[0] != 2) {
        return true;   // el 1 y el 3 no son «viejos»: la regla es sobre la rama 2
    }
    if (n[1] < 3) {
        return false;
    }
    if (n[1] > 3) {
        return true;
    }
    return n[2] >= 3;
}

std::string banderasDeEnvio(const OpcionesDeEnvio& o) {
    std::string f = "-";
    if (o.w) { f += 'w'; }
    if (o.L) { f += 'L'; }
    if (o.e) { f += 'e'; }
    if (o.c) { f += 'c'; }
    if (o.R) { f += 'R'; }
    return f == "-" ? std::string() : f;
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

    // La versión de ZFS antes que el camino: da igual por dónde vayan los bytes si el
    // formato del flujo no se entiende en el otro lado.
    if (!versionAdmiteTransferencia(origen.versionZfs)
        || !versionAdmiteTransferencia(destino.versionZfs)) {
        p.fallo = Fallo::ZfsDemasiadoViejo;
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

std::string direccionDeSshClient(const std::string& salida) {
    // SSH_CLIENT = «<dirección> <puerto origen> <puerto destino>». Se coge la primera línea
    // y su primer campo.
    //
    // **El recorte se hace AQUÍ, en C++, y no con `${SSH_CLIENT%% *}` en la orden.** Esa
    // orden la lanza el cliente, que puede ser Windows, y allí «%» es el carácter de
    // expansión de variables de cmd: se comía parte del texto y devolvía una dirección con
    // una letra de más.
    std::string primera = trim(salida);
    const std::size_t salto = primera.find('\n');
    if (salto != std::string::npos) {
        primera = trim(primera.substr(0, salto));
    }
    const std::size_t espacio = primera.find(' ');
    const std::string dir = trim(espacio == std::string::npos ? primera
                                                              : primera.substr(0, espacio));
    if (dir.empty()) {
        return {};
    }
    // Lo que se admite incluye «%» y letras: una IPv6 con zona los lleva.
    for (const char c : dir) {
        const bool vale = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                          || (c >= 'a' && c <= 'z') || c == ':' || c == '.' || c == '%'
                          || c == '_' || c == '-';
        if (!vale) {
            return {};
        }
    }
    // Y tiene que parecerse a una dirección: sin dos puntos ni punto no lo es.
    if (dir.find(':') == std::string::npos && dir.find('.') == std::string::npos) {
        return {};
    }
    return dir;
}

std::string comoMeVeElOrigen(TransportSession& ses, const ConnectionProfile& origen,
                             bool verboso) {
    if (transport::isLocalConnection(origen)) {
        return "127.0.0.1";
    }
    std::string out;
    std::string err;
    int rc = -1;
    // `allowAgentRpc=false` a propósito: con el valor de siempre la orden se desviaba al
    // daemon por RPC, y allí `$SSH_CLIENT` no existe porque no es una sesión SSH.
    if (!transport::runSsh(ses, origen, "echo $SSH_CLIENT", 8000, out, err, rc, {}, {}, {}, {},
                           /*allowAgentRpc=*/false, verboso)
        || rc != 0) {
        return {};
    }
    return direccionDeSshClient(out);
}

Reanudacion buscaTestigo(TransportSession& ses, const ConnectionProfile& destino,
                         const std::string& objetivo, bool verboso) {
    const std::string diana = trim(objetivo);
    if (diana.empty()) {
        return {};
    }
    const auto testigoDe = [&](const std::string& ds) {
        std::string out;
        std::string err;
        int rc = -1;
        if (!transport::tryAgentRpcOverSsh(ses, destino,
                                           {"--dump-zfs-get-prop", "receive_resume_token", ds},
                                           15000, out, err, rc, {}, {}, verboso)
            || rc != 0) {
            // Que no se pueda leer NO significa que no haya nada a medias: puede que el
            // dataset aún no exista, que es el caso normal en una copia nueva.
            return std::string();
        }
        return trim(out);
    };

    // Se compone el mismo TSV que analiza la regla, para que la decisión esté escrita una
    // sola vez y probada aparte.
    std::string tsv = diana + "\t" + testigoDe(diana) + "\n";
    std::string hijos;
    std::string err;
    int rc = -1;
    if (transport::tryAgentRpcOverSsh(ses, destino, {"--dump-zfs-list-children", diana}, 15000,
                                      hijos, err, rc, {}, {}, verboso)
        && rc == 0) {
        for (const std::string& cruda : split(hijos, "\n", true)) {
            const std::string ds = trim(cruda);
            if (ds.empty() || ds == diana) {
                continue;
            }
            tsv += ds + "\t" + testigoDe(ds) + "\n";
        }
    }
    return testigoDeReanudacion(diana, tsv);
}

}  // namespace zfsmgr::base::transferencia

namespace zfsmgr::base::transferencia {

std::string destinoReal(const std::string& origenDataset, const std::string& destinoElegido) {
    const std::string origen = trim(origenDataset);
    const std::string destino = trim(destinoElegido);
    const std::size_t barra = origen.find_last_of('/');
    const std::string hoja = barra == std::string::npos ? origen : origen.substr(barra + 1);
    if (hoja.empty() || destino.empty()) {
        return destino;
    }
    const std::size_t barraDestino = destino.find_last_of('/');
    const std::string hojaDestino =
        barraDestino == std::string::npos ? destino : destino.substr(barraDestino + 1);
    // Ya acaba en el nombre del origen: se toma tal cual. Sin esto, copiar dos veces al
    // mismo sitio dejaría «respaldos/datos/datos».
    if (hojaDestino == hoja || endsWith(destino, "/" + hoja)) {
        return destino;
    }
    return destino + "/" + hoja;
}

std::string ordenDeEnvio(const std::string& instantanea, const std::string& banderas) {
    const std::string b = trim(banderas);
    return b.empty() ? "zfs send " + shSingleQuote(instantanea)
                     : "zfs send " + b + " " + shSingleQuote(instantanea);
}

std::string ordenDeRecepcion(const std::string& destino) {
    return "zfs recv -Fus " + shSingleQuote(destino);
}

Montaje montajeDe(const ConnectionProfile& origen, const ConnectionProfile& destino,
                  bool mismaConexion) {
    if (mismaConexion) {
        return Montaje::MismaConexion;
    }
    const bool losDosRemotosPorSsh = !transport::isLocalConnection(origen)
                                     && !transport::isLocalConnection(destino)
                                     && toLowerAscii(trim(origen.connType)) == "ssh"
                                     && toLowerAscii(trim(destino.connType)) == "ssh";
    return losDosRemotosPorSsh ? Montaje::RemotoARemotoDirecto : Montaje::PorElCliente;
}

}  // namespace zfsmgr::base::transferencia
