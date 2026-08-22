#include "peers.h"

#include "json.h"
#include "strutil.h"
#include "transportcmd.h"

namespace zfsmgr::base::peers {

Vista analiza(const std::string& salida) {
    Vista v;
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
            Par p;
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

std::string etiquetaDe(Fallo f) {
    switch (f) {
        case Fallo::Ninguno:
            return {};
        case Fallo::SinOtrasConexiones:
            return "no hay ninguna otra conexión que entregar";
        case Fallo::SinMaterialTls:
            return "ninguna de las otras conexiones tiene material TLS: instale su daemon "
                   "primero, que es quien lo genera";
    }
    return {};
}

Entrega componeEntrega(const std::vector<ConnectionProfile>& perfiles,
                       const std::string& destino) {
    Entrega e;
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
        e.nombres.push_back(id);
    }
    if (!habiaOtras) {
        e.fallo = Fallo::SinOtrasConexiones;
        return e;
    }
    if (pares.empty()) {
        e.fallo = Fallo::SinMaterialTls;
        return e;
    }
    json::Value raiz;
    // Con quién se identifica ESA máquina. Lo sabe el cliente —le está entregando las
    // credenciales— y allí no hay forma de averiguarlo. Sirve para que el daemon distinga
    // «nivela contra otra» de «nivela contra un dataset mío».
    raiz.set("self", json::Value(destino));
    raiz.set("peers", json::Value(std::move(pares)));
    e.cargaB64 = base64Encode(json::toCompact(raiz));
    return e;
}

std::vector<std::string> direccionesDeEscucha() {
    return {"127.0.0.1", "0.0.0.0", "::"};
}

bool direccionDeEscuchaValida(const std::string& dir) {
    const std::string d = trim(dir);
    for (const std::string& v : direccionesDeEscucha()) {
        if (d == v) {
            return true;
        }
    }
    return false;
}

}  // namespace zfsmgr::base::peers
