#include "peers.h"

#include "json.h"
#include "strutil.h"
#include "transportcmd.h"

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
        if (c.size() >= 3) {
            Peer p;
            p.id = trim(c[0]);
            p.host = trim(c[1]);
            try {
                p.puerto = std::stoi(trim(c[2]));
            } catch (...) {
                p.puerto = 0;
            }
            v.pares.push_back(p);
        }
    }
    return v;
}

std::string labelOf(Failure f) {
    switch (f) {
        case Failure::None_:
            return {};
        case Failure::NoOtherConnections:
            return "no hay ninguna otra conexión que entregar";
        case Failure::NoTlsMaterial:
            return "ninguna de las otras conexiones tiene material TLS: instale su daemon "
                   "primero, que es quien lo genera";
    }
    return {};
}

Handover composeHandover(const std::vector<ConnectionProfile>& perfiles,
                       const std::string& destino) {
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
        json::Value uno;
        uno.set("id", json::Value(id));
        // Una conexión local se le entrega como 127.0.0.1 y no por su nombre de red: desde
        // la máquina de destino, «ella misma» es el bucle local.
        uno.set("host",
                json::Value(transport::isLocalConnection(p) ? std::string("127.0.0.1") : p.host));
        uno.set("port", json::Value(static_cast<double>(p.daemonTlsPort)));
        uno.set("server_cert_pem", json::Value(p.daemonTlsServerCertPem));
        uno.set("client_cert_pem", json::Value(p.daemonTlsClientCertPem));
        uno.set("client_key_pem", json::Value(p.daemonTlsClientKeyPem));
        pares.push_back(uno);
        e.names.push_back(id);
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
    raiz.set("peers", json::Value(std::move(pares)));
    e.payloadB64 = base64Encode(json::toCompact(raiz));
    return e;
}

std::vector<std::string> bindAddresses() {
    return {"127.0.0.1", "0.0.0.0", "::"};
}

bool isValidBindAddress(const std::string& dir) {
    const std::string d = trim(dir);
    for (const std::string& v : bindAddresses()) {
        if (d == v) {
            return true;
        }
    }
    return false;
}

}  // namespace zfsmgr::base::peers
