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


}  // namespace zfsmgr::base::transferencia

namespace zfsmgr::base::transferencia {

EscuchaDelReceptor leeEscucha(const std::string& salida) {
    EscuchaDelReceptor e;
    for (const std::string& linea : split(salida, "\n", true)) {
        const std::string l = trim(linea);
        if (startsWith(l, "PORT=")) {
            e.puerto = std::atoi(trim(l.substr(5)).c_str());
        } else if (startsWith(l, "TOKEN=")) {
            e.testigo = trim(l.substr(6));
        }
    }
    return e;
}

std::string leeIdentificadorDeTrabajo(const std::string& salida) {
    for (const std::string& linea : split(salida, "\n", true)) {
        const std::string l = trim(linea);
        if (startsWith(l, "JOB_ID=")) {
            return trim(l.substr(7));
        }
    }
    return {};
}

std::string etiquetaDe(FalloTrabajo f) {
    switch (f) {
        case FalloTrabajo::Ninguno:
            return {};
        case FalloTrabajo::ReceptorNoEscucha:
            return "el daemon del destino no pudo ponerse a escuchar";
        case FalloTrabajo::RespuestaDeEscuchaNoVale:
            return "el destino contestó algo que no es un puerto y un testigo";
        case FalloTrabajo::SinDireccionDeVuelta:
            return "no se pudo averiguar con qué dirección ve el origen a este equipo";
        case FalloTrabajo::EmisorNoArranco:
            return "el daemon del origen no arrancó el envío";
        case FalloTrabajo::SinIdentificador:
            return "el origen arrancó el envío pero no dijo con qué identificador seguirlo";
    }
    return {};
}

std::string haciaDondeConecta(TransportSession& ses, const ConnectionProfile& origen,
                              const ConnectionProfile& destino, bool mismaConexion,
                              bool verboso) {
    // El `host` del perfil del destino NO sirve cuando el destino es la conexión Local:
    // vale «localhost», que desde el origen apunta al propio origen. La transferencia se
    // quedaba intentando conectar consigo misma. Se le pregunta al origen con qué dirección
    // nos ve, que es la única que con seguridad le sirve para volver.
    //
    // Está aparte porque lo necesitan DOS cosas: el flujo de `zfs send` y el árbol de
    // ficheros. Copiarlo era garantizar que una de las dos se quedara sin el caso de Local
    // —que es justo lo que le había pasado al intérprete—.
    if (mismaConexion) {
        return "127.0.0.1";
    }
    if (transport::isLocalConnection(destino)) {
        return comoMeVeElOrigen(ses, origen, verboso);
    }
    return trim(destino.host);
}

Trabajo lanzaTrabajo(TransportSession& ses, const LlamadaAlAgente& llama,
                     const ConnectionProfile& origen, const ConnectionProfile& destino,
                     const std::string& instantanea, const std::string& destinoDelRecv,
                     const std::string& desdeInstantanea, const std::string& banderas,
                     const std::string& testigoReanudacion, bool mismaConexion, bool verboso) {
    Trabajo t;

    // 1. Que el destino se ponga a escuchar.
    std::string salida;
    std::string err;
    int rc = -1;
    if (!llama(destino, {"--zfs-recv-listen", destinoDelRecv, "1"}, 12000, salida, err, rc)
        || rc != 0) {
        t.fallo = FalloTrabajo::ReceptorNoEscucha;
        t.detalle = trim(err.empty() ? salida : err);
        return t;
    }
    const EscuchaDelReceptor escucha = leeEscucha(salida);
    if (!escucha.vale()) {
        t.fallo = FalloTrabajo::RespuestaDeEscuchaNoVale;
        t.detalle = trim(salida);
        return t;
    }

    // 2. Con qué dirección tiene que conectar el ORIGEN.
    const std::string haciaDonde = haciaDondeConecta(ses, origen, destino, mismaConexion,
                                                     verboso);
    if (haciaDonde.empty()) {
        t.fallo = FalloTrabajo::SinDireccionDeVuelta;
        return t;
    }

    // 3. Que el origen arranque el envío. Con el testigo puesto, los tres campos de en
    // medio van vacíos: `zfs send -t` lleva dentro qué continuar.
    const bool reanudando = !trim(testigoReanudacion).empty();
    const std::vector<std::string> args = {
        "--zfs-send-to-peer-async",
        reanudando ? std::string() : instantanea,
        haciaDonde,
        std::to_string(escucha.puerto),
        escucha.testigo,
        reanudando ? std::string() : trim(desdeInstantanea),
        reanudando ? std::string() : trim(banderas),
        trim(testigoReanudacion),
    };
    // Lo lanza el emisor, salvo en la misma conexión: allí las dos puntas son la misma
    // máquina y el que manda es el mismo daemon.
    const ConnectionProfile& quienEnvia = mismaConexion ? destino : origen;
    salida.clear();
    err.clear();
    rc = -1;
    if (!llama(quienEnvia, args, 10000, salida, err, rc) || rc != 0) {
        t.fallo = FalloTrabajo::EmisorNoArranco;
        t.detalle = trim(err.empty() ? salida : err);
        return t;
    }
    t.id = leeIdentificadorDeTrabajo(salida);
    if (t.id.empty()) {
        t.fallo = FalloTrabajo::SinIdentificador;
        t.detalle = trim(salida);
    }
    return t;
}

Trabajo lanzaTrabajoDeArbol(TransportSession& ses, const LlamadaAlAgente& llama,
                            const ConnectionProfile& origen, const ConnectionProfile& destino,
                            const std::string& directorioOrigen,
                            const std::string& directorioDestino, bool mismaConexion,
                            bool verboso, bool comoTrabajo, bool borrarEnDestino, bool enSeco,
                            std::string* salidaDelEnvio) {
    Trabajo t;
    if (trim(directorioOrigen).empty() || trim(directorioDestino).empty()) {
        t.fallo = FalloTrabajo::ReceptorNoEscucha;
        t.detalle = "falta el directorio de origen o el de destino";
        return t;
    }

    // 1. Que el destino se ponga a escuchar. Falla si el directorio no existe, y eso es a
    // propósito: crear directorios como root a petición de quien conecta no es cosa del
    // receptor.
    std::string salida;
    std::string err;
    int rc = -1;
    if (!llama(destino, {"--tree-recv-listen", directorioDestino}, 30000, salida, err, rc)
        || rc != 0) {
        t.fallo = FalloTrabajo::ReceptorNoEscucha;
        t.detalle = trim(err.empty() ? salida : err);
        return t;
    }
    const EscuchaDelReceptor escucha = leeEscucha(salida);
    if (!escucha.vale()) {
        t.fallo = FalloTrabajo::RespuestaDeEscuchaNoVale;
        t.detalle = trim(salida);
        return t;
    }

    // 2. Con qué dirección ve el ORIGEN al destino. Es la misma regla que el flujo de
    // instantáneas, y por eso se llama en vez de copiarse: el caso de la conexión Local
    // —donde «localhost» apuntaría al propio origen— es el que se pierde al copiarla.
    const std::string haciaDonde = haciaDondeConecta(ses, origen, destino, mismaConexion,
                                                     verboso);
    if (haciaDonde.empty()) {
        t.fallo = FalloTrabajo::SinDireccionDeVuelta;
        return t;
    }

    // 3. Que el origen envíe.
    //
    // `borrarEnDestino` es de SINCRONIZAR, no de traer: traer un directorio es AÑADIR, y con
    // el borrado puesto esto se llevaría por delante lo que ya hubiera en el destino. Por eso
    // es un parámetro y no algo que se decida aquí, y por eso «Desde Dir» lo deja en falso.
    std::vector<std::string> args;
    if (comoTrabajo) {
        args.push_back("--job-submit");
    }
    args.push_back("--tree-send-to-peer");
    args.push_back(directorioOrigen);
    args.push_back(haciaDonde);
    args.push_back(std::to_string(escucha.puerto));
    args.push_back(escucha.testigo);
    if (borrarEnDestino) {
        args.push_back("--delete");
    }
    if (enSeco) {
        args.push_back("--dry-run");
    }
    const ConnectionProfile& quienEnvia = mismaConexion ? destino : origen;
    salida.clear();
    err.clear();
    rc = -1;
    // Encolar es instantáneo; esperar puede tardar horas, y ahí el plazo lo pone quien
    // llama a base de no ponerlo. Un plazo de un minuto sobre una copia de verdad la
    // declararía fallida mientras sigue moviendo datos.
    const int plazo = comoTrabajo ? 60000 : 0;
    if (!llama(quienEnvia, args, plazo, salida, err, rc) || rc != 0) {
        t.fallo = FalloTrabajo::EmisorNoArranco;
        t.detalle = trim(err.empty() ? salida : err);
        return t;
    }
    if (salidaDelEnvio != nullptr) {
        *salidaDelEnvio = salida;
    }
    if (!comoTrabajo) {
        return t;  // terminó: no hay identificador que leer ni que seguir
    }
    t.id = leeIdentificadorDeTrabajo(salida);
    if (t.id.empty()) {
        t.fallo = FalloTrabajo::SinIdentificador;
        t.detalle = trim(salida);
    }
    return t;
}

std::string etiquetaDe(FalloNivelar f) {
    switch (f) {
        case FalloNivelar::Ninguno:
            return {};
        case FalloNivelar::ObjetivoNoEstaEnOrigen:
            return "la instantánea de origen ya no está en su dataset";
        case FalloNivelar::DestinoSinInstantaneas:
            return "el destino no tiene ninguna instantánea: no hay base común desde la que "
                   "seguir; para llevarlo entero, copie";
        case FalloNivelar::BaseNoEstaEnOrigen:
            return "la última instantánea del destino no existe en el origen: son historias "
                   "distintas y no hay incremental posible";
        case FalloNivelar::DestinoMasNuevo:
            return "el destino tiene una instantánea más moderna que la que se quiere enviar";
        case FalloNivelar::YaNivelado:
            return "el destino ya está nivelado en esa instantánea";
    }
    return {};
}

PlanNivelar planeaNivelar(const std::vector<Instantanea>& origen,
                          const std::vector<Instantanea>& destino,
                          const std::string& objetivo) {
    PlanNivelar plan;
    plan.objetivo = objetivo;

    std::size_t iObjetivo = origen.size();
    for (std::size_t i = 0; i < origen.size(); ++i) {
        if (origen[i].nombre == objetivo) {
            iObjetivo = i;
            break;
        }
    }
    if (iObjetivo == origen.size()) {
        plan.fallo = FalloNivelar::ObjetivoNoEstaEnOrigen;
        return plan;
    }
    if (destino.empty()) {
        plan.fallo = FalloNivelar::DestinoSinInstantaneas;
        return plan;
    }

    // La ÚLTIMA del destino marca hasta dónde llegó, y su GUID es lo que hay que reconocer
    // en el origen. Buscarla por nombre daría con la equivocada en cuanto las dos máquinas
    // tengan una instantánea llamada igual, que con nombres automáticos es lo normal y no
    // lo raro.
    const std::string guidUltima = destino.back().guid;
    std::size_t iBase = origen.size();
    if (!guidUltima.empty()) {
        for (std::size_t i = 0; i < origen.size(); ++i) {
            if (!origen[i].guid.empty() && origen[i].guid == guidUltima) {
                iBase = i;
                break;
            }
        }
    }
    if (iBase == origen.size()) {
        plan.fallo = FalloNivelar::BaseNoEstaEnOrigen;
        return plan;
    }
    if (iBase > iObjetivo) {
        plan.fallo = FalloNivelar::DestinoMasNuevo;
        return plan;
    }
    if (iBase == iObjetivo) {
        plan.fallo = FalloNivelar::YaNivelado;
        return plan;
    }
    plan.base = origen[iBase].nombre;
    return plan;
}

}  // namespace zfsmgr::base::transferencia
