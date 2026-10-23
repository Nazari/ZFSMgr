#include "peers.h"

#include <set>
#include "i18n.h"

#include "json.h"
#include "strutil.h"
#include "transportcmd.h"

#ifdef _WIN32
#include <cstdlib>
#else
#include <unistd.h>
#endif

namespace zfsmgr::base::peers {

View parse(const std::string& salida) {
    View v;
    for (const std::string& linea : split(salida, "\n", true)) {
        const std::vector<std::string> c = split(linea, "\t", false);
        if (c.empty()) {
            continue;
        }
        if (trim(c[0]) == "SELF") {
            if (c.size() >= 2) {
                v.self = trim(c[1]);
            }
            continue;
        }
        if (trim(c[0]) == "SELF_UID") {
            if (c.size() >= 2) {
                v.selfUid = trim(c[1]);
            }
            continue;
        }
        if (trim(c[0]) == "REAL_UID") {
            if (c.size() >= 2) {
                v.realUid = trim(c[1]);
            }
            continue;
        }
        if (c.size() >= 4) {
            Peer p;
            p.id = trim(c[0]);
            p.host = trim(c[1]);
            try {
                p.puerto = std::stoi(trim(c[2]));
            } catch (...) {
                p.puerto = 0;
            }
            p.uid = trim(c[3]);
            v.pares.push_back(p);
        }
    }
    return v;
}

bool destinationIsSelf(const std::string& connId, const SelfMatch& m) {
    const std::string id = toLowerAscii(trim(connId));
    if (id.empty()) {
        return false;
    }
    const std::string mio = toLowerAscii(trim(m.realUid));

    // 1. El nombre es el de un par: manda SU uid.
    for (const Peer& p : m.peers) {
        if (toLowerAscii(trim(p.id)) != id) {
            continue;
        }
        const std::string suyo = toLowerAscii(trim(p.uid));
        return !suyo.empty() && !mio.empty() && suyo == mio;
    }

    // 2. El nombre es con el que me entregaron las credenciales.
    const std::string entregado = toLowerAscii(trim(m.selfUid));
    if (!entregado.empty() && !mio.empty() && toLowerAscii(trim(m.selfName)) == id) {
        return entregado == mio;
    }

    // Sin identidad no se afirma nada: mejor no nivelar que nivelar al sitio equivocado.
    return false;
}

std::string labelOf(Failure f) {
    switch (f) {
        case Failure::None_:
            return {};
        case Failure::NoOtherConnections:
            return i18n::tr("t_cmd_peers_hay_ninguna_otra_conexion", "no hay ninguna otra conexión que entregar");
        case Failure::NoTlsMaterial:
            return i18n::tr("t_cmd_peers_ninguna_otras_conexiones_tiene", "ninguna de las otras conexiones tiene material TLS: instale su daemon "
                   "primero, que es quien lo genera");
    }
    return {};
}

std::string thisMachineName() {
#ifdef _WIN32
    const char* n = std::getenv("COMPUTERNAME");
    return n ? trim(n) : std::string();
#else
    char n[256] = {};
    if (gethostname(n, sizeof(n) - 1) != 0) {
        return {};
    }
    return trim(n);
#endif
}

Handover composeHandover(
    const std::vector<ConnectionProfile>& perfiles, const std::string& destino,
    const std::string& uidDestino, const std::string& clientHost,
    const std::map<std::string, std::pair<std::string, std::string>>* credencialesTraer) {
    Handover e;
    json::Array pares;
    bool habiaOtras = false;
    for (const ConnectionProfile& p : perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        if (toLowerAscii(id) == toLowerAscii(destino)) {
            continue;
        }
        habiaOtras = true;
        if (p.daemonTlsServerCertPem.empty() || p.daemonTlsClientCertPem.empty()
            || p.daemonTlsClientKeyPem.empty()) {
            continue;  // sin material TLS no hay nada que entregar
        }
        // The Local connection is the CLIENT's machine. Its profile says «local» and
        // 127.0.0.1, and from the target both mean the target itself: it goes under the
        // client machine's name, as name and address, or not at all.
        const bool esLocal = transport::isLocalConnection(p);
        if (esLocal && trim(clientHost).empty()) {
            continue;
        }
        const std::string entregado = esLocal ? trim(clientHost) : id;
        json::Value uno;
        uno.set("id", json::Value(entregado));
        uno.set("host", json::Value(esLocal ? trim(clientHost) : p.host));

        uno.set("port", json::Value(static_cast<double>(p.daemonTlsPort)));
        uno.set("server_cert_pem", json::Value(p.daemonTlsServerCertPem));
        if (credencialesTraer) {
            const auto it = credencialesTraer->find(id);
            if (it == credencialesTraer->end() || it->second.first.empty()
                || it->second.second.empty()) {
                e.withoutPullCredential.push_back(entregado);
                continue;
            }
            uno.set("client_cert_pem", json::Value(it->second.first));
            uno.set("client_key_pem", json::Value(it->second.second));
            uno.set("role", json::Value("pull"));
        } else {
            uno.set("client_cert_pem", json::Value(p.daemonTlsClientCertPem));
            uno.set("client_key_pem", json::Value(p.daemonTlsClientKeyPem));
        }
        // El IDENTIFICADOR DE MÁQUINA, junto al nombre. El nombre es una etiqueta de quien
        // mira —`local` es una máquina distinta según quién pregunte, y la misma máquina
        // aparece como «unib» en una lista y «unibody» en otra—; el uid no. Es lo que
        // permite que el daemon conteste «¿este destino soy yo?» sin fiarse de una etiqueta
        // que le dijo un cliente.
        uno.set("uid", json::Value(trim(p.machineUid)));
        if (esLocal) {
            e.localName = id;
        }
        pares.push_back(uno);
        e.names.push_back(entregado);
    }
    if (!habiaOtras) {
        e.failure = Failure::NoOtherConnections;
        return e;
    }
    if (pares.empty()) {
        e.failure = Failure::NoTlsMaterial;
        return e;
    }
    json::Value raiz;
    // Con quién se identifica ESA máquina. Lo sabe el cliente —le está entregando las
    // credenciales— y allí no hay forma de averiguarlo. Sirve para que el daemon distinga
    // «nivela contra otra» de «nivela contra un dataset mío».
    raiz.set("self", json::Value(destino));
    // Y el suyo propio, para que pueda compararse. Se manda aunque el daemon sepa
    // averiguarlo él solo: si los dos no coinciden, es que esta configuración apunta a otra
    // máquina de la que cree —un perfil copiado de otro equipo, por ejemplo— y eso se quiere
    // poder detectar en vez de tragarlo.
    raiz.set("self_uid", json::Value(trim(uidDestino)));
    raiz.set("peers", json::Value(std::move(pares)));
    e.payloadB64 = base64Encode(json::toCompact(raiz));
    return e;
}

std::vector<std::string> bindAddresses() {
    return {"127.0.0.1", "0.0.0.0", "::"};
}

bool pullerMayCall(const std::string& verb, const std::vector<std::string>& args) {
    static const std::set<std::string> kLectura = {"--health", "--heartbeat", "--dump-zfs-list-all",
                                                   "--dump-zfs-holds", "--zfs-send-stream"};
    if (kLectura.count(verb) > 0) {
        return true;
    }
    // Hold and release: only its own tags. Releasing someone else's hold —another puller's
    // base, a hold put by hand— would let its prune take what that one needs.
    if (verb == "--mutate-zfs-hold" || verb == "--mutate-zfs-release") {
        return args.size() >= 2 && startsWith(trim(args[0]), "zfsmgr-pull:");
    }
    return false;
}

bool parsePullCredential(const std::string& texto, std::string& cert, std::string& clave) {
    cert.clear();
    clave.clear();
    static const std::string kBegin = "__ZFSMGR_TLS_BEGIN__:";
    static const std::string kEnd = "__ZFSMGR_TLS_END__:";
    std::string ruta;
    std::string contenido;
    for (const std::string& linea : split(texto, "\n", false)) {
        const std::string l = trim(linea);
        if (startsWith(l, kBegin)) {
            ruta = trim(l.substr(kBegin.size()));
            contenido.clear();
            continue;
        }
        if (startsWith(l, kEnd)) {
            if (!ruta.empty() && trim(l.substr(kEnd.size())) == ruta) {
                const std::string pem = trim(contenido) + "\n";
                if (endsWith(ruta, "/puller.crt")) {
                    cert = pem;
                } else if (endsWith(ruta, "/puller.key")) {
                    clave = pem;
                }
            }
            ruta.clear();
            continue;
        }
        if (!ruta.empty()) {
            contenido += linea + "\n";
        }
    }
    return cert.find("BEGIN CERTIFICATE") != std::string::npos
           && clave.find("PRIVATE KEY") != std::string::npos;
}

}  // namespace zfsmgr::base::peers
