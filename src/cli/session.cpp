#include "session.h"

#include "connectionjson.h"
#include "secretcipher.h"
#include "secretinput.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "transportcmd.h"
#include "transportrpc.h"
#include "strutil.h"

#include <cstdio>
#include <map>

namespace zfsmgr::cli {
namespace {

namespace B = zfsmgr::base;
namespace ST = zfsmgr::base::store;
namespace CJ = zfsmgr::base::connjson;
using Nivel = B::TransportSession::Nivel;

// Descifra si hace falta. Si no se puede abrir devuelve VACÍO, nunca el texto cifrado: la
// interfaz lo dejaba tal cual y podía acabar enviándose como si fuera una contraseña.
std::string abrir(const std::string& valor, const std::string& maestra) {
    if (!B::SecretCipher::isEncrypted(valor)) {
        return valor;
    }
    if (maestra.empty()) {
        return {};
    }
    std::string claro;
    std::string err;
    return B::SecretCipher::decryptEncv1(valor, maestra, claro, err) ? claro : std::string();
}

void descifraPerfil(B::ConnectionProfile& p, const std::string& maestra) {
    p.username = abrir(p.username, maestra);
    p.password = abrir(p.password, maestra);
    p.daemonTlsServerCertPem = abrir(p.daemonTlsServerCertPem, maestra);
    p.daemonTlsClientCertPem = abrir(p.daemonTlsClientCertPem, maestra);
    p.daemonTlsClientKeyPem = abrir(p.daemonTlsClientKeyPem, maestra);
}

const char* etiqueta(Nivel n) {
    switch (n) {
        case Nivel::Warn: return "aviso";
        case Nivel::Error: return "error";
        case Nivel::Debug: return "depuración";
        case Nivel::Info: return "info";
        default: return "";
    }
}

}  // namespace

Conexiones cargarConexiones(const std::string& dirConfig, const std::string& maestra) {
    Conexiones c;
    ST::Aviso aviso;
    const auto root = ST::leerConfig(dirConfig, aviso);
    if (!aviso.vacio()) {
        c.aviso = "no se pudo leer config.json";
        return c;
    }
    // El material TLS del almacén de confianza, indexado por identificador para fundirlo
    // con el perfil. Sin esto el transporte lo pediría otra vez por SSH en cada arranque,
    // y en una máquina donde /etc/zfsmgr es solo de root eso significa pedir sudo.
    ST::Aviso avisoTrust;
    const auto trust = ST::leerTrustStore(dirConfig, avisoTrust);
    std::map<std::string, B::ConnectionProfile> tlsPorId;
    for (const auto& v : trust["connections"].toArray()) {
        auto t = CJ::connectionFromJson(v, std::string());
        if (!t.id.empty()) {
            descifraPerfil(t, maestra);
            tlsPorId[B::toLowerAscii(t.id)] = t;
        }
    }

    for (const auto& v : root["connections"].toArray()) {
        auto p = CJ::connectionFromJson(v, std::string());
        descifraPerfil(p, maestra);
        const auto it = tlsPorId.find(B::toLowerAscii(p.id));
        if (it != tlsPorId.end() && !CJ::profileHasDaemonTls(p)) {
            p.daemonTlsServerCertPem = it->second.daemonTlsServerCertPem;
            p.daemonTlsClientCertPem = it->second.daemonTlsClientCertPem;
            p.daemonTlsClientKeyPem = it->second.daemonTlsClientKeyPem;
            p.daemonTlsPort = it->second.daemonTlsPort;
        }
        p.port = CJ::ensurePort(p.connType, p.port);
        c.perfiles.push_back(std::move(p));
    }
    return c;
}

const zfsmgr::base::ConnectionProfile* buscarConexion(const Conexiones& c, const std::string& nombre) {
    const std::string n = B::toLowerAscii(B::trim(nombre));
    if (n.empty()) {
        return nullptr;
    }
    // El identificador manda sobre el nombre: es el que no cambia.
    for (const auto& p : c.perfiles) {
        if (B::toLowerAscii(p.id) == n) {
            return &p;
        }
    }
    for (const auto& p : c.perfiles) {
        if (B::toLowerAscii(p.name) == n) {
            return &p;
        }
    }
    return nullptr;
}

std::unique_ptr<Sesion> crearSesion(const std::string& dirConfig,
                                    const std::string& maestra,
                                    bool verboso) {
    auto s = std::make_unique<Sesion>();
    s->dirConfig = dirConfig;
    s->maestra = maestra;
    s->verboso = verboso;

    Sesion* raw = s.get();

    // A la salida de ERROR, para que la estándar quede limpia y se pueda redirigir. Sin
    // `-v` solo se cuentan los avisos y los errores: el detalle de cada orden es útil
    // depurando y ruido el resto del tiempo.
    s->transporte.sink = [raw](Nivel n, const std::string& connId, const std::string& msg) {
        const bool importante = (n == Nivel::Warn || n == Nivel::Error);
        if (!importante && !raw->verboso) {
            return;
        }
        const char* et = etiqueta(n);
        std::fprintf(stderr, "%s%s%s%s%s\n", *et ? "[" : "", et, *et ? "] " : "",
                     connId.empty() ? "" : (connId + ": ").c_str(), msg.c_str());
    };

    // Por el TERMINAL, nunca por argumento ni por variable de entorno: las dos cosas
    // quedan visibles en `ps` para cualquier usuario de la máquina.
    s->transporte.credentialProvider = [](const std::string& motivo, std::string& usuario,
                                          std::string& clave) {
        std::fprintf(stderr, "%s\n", motivo.c_str());
        std::string err;
        if (!preguntarPorTerminal("Usuario: ", usuario, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        if (!preguntarSecretoPorTerminal("Contraseña: ", clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        return true;
    };

    // El sudo de ESTA máquina. Se busca primero entre las conexiones conocidas —si ya hay
    // una local con contraseña guardada, no hay por qué volver a preguntar— y solo si no
    // aparece se pregunta por el terminal.
    s->transporte.localSudoResolver = [raw](B::ConnectionProfile& perfil) {
        if (raw->sudoResuelto) {
            perfil.username = raw->sudoUsuario;
            perfil.password = raw->sudoClave;
            perfil.useSudo = true;
            return true;
        }
        const Conexiones c = cargarConexiones(raw->dirConfig, raw->maestra);
        for (const auto& p : c.perfiles) {
            if (CJ::isLocalProfile(p) && !p.password.empty()) {
                perfil.username = p.username;
                perfil.password = p.password;
                perfil.useSudo = true;
                raw->sudoUsuario = p.username;
                raw->sudoClave = p.password;
                raw->sudoResuelto = true;
                return true;
            }
        }
        std::string err;
        std::string clave;
        std::fprintf(stderr, "Se necesita la contraseña de sudo de esta máquina.\n");
        if (!preguntarSecretoPorTerminal("Contraseña de sudo: ", clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        perfil.password = clave;
        perfil.useSudo = true;
        raw->sudoUsuario = perfil.username;
        raw->sudoClave = clave;
        raw->sudoResuelto = true;
        return true;
    };

    // Guardar el material TLS negociado, para no repetir el arranque cada vez. Va al
    // ALMACÉN DE CONFIANZA y no a config.json, que es donde lo pone la interfaz.
    s->transporte.tlsPersister = [raw](const B::ConnectionProfile& p, const std::string& srv,
                                       const std::string& cli, const std::string& key,
                                       std::uint16_t puerto, std::string* errorOut) {
        if (p.id.empty()) {
            if (errorOut) {
                *errorOut = "la conexión no tiene identificador";
            }
            return false;
        }
        ST::Aviso aviso;
        auto root = ST::leerTrustStore(raw->dirConfig, aviso);
        B::ConnectionProfile guardado = p;
        guardado.daemonTlsServerCertPem = srv;
        guardado.daemonTlsClientCertPem = cli;
        guardado.daemonTlsClientKeyPem = key;
        guardado.daemonTlsPort = puerto;
        // Cifrado con la misma contraseña maestra que usa la interfaz. Sin ella NO se
        // guarda en claro: dejar certificados y clave privada legibles en disco para
        // ahorrarse una lectura por SSH es un mal cambio.
        if (raw->maestra.empty()) {
            if (errorOut) {
                *errorOut = "sin contraseña maestra no se guarda el material TLS en claro";
            }
            return false;
        }
        std::string err;
        for (std::string* campo : {&guardado.daemonTlsServerCertPem, &guardado.daemonTlsClientCertPem,
                                   &guardado.daemonTlsClientKeyPem}) {
            std::string cifrado;
            if (!B::SecretCipher::encryptEncv1(*campo, raw->maestra, cifrado, err)) {
                if (errorOut) {
                    *errorOut = "no se pudo cifrar el material TLS: " + err;
                }
                return false;
            }
            *campo = cifrado;
        }
        auto arr = root["connections"].toArray();
        B::json::Array salida;
        bool sustituido = false;
        for (const auto& v : arr) {
            const auto existente = CJ::connectionFromJson(v, std::string());
            if (B::toLowerAscii(existente.id) == B::toLowerAscii(p.id)) {
                salida.push_back(CJ::connectionTrustToJson(guardado, std::string()));
                sustituido = true;
            } else {
                salida.push_back(v);
            }
        }
        if (!sustituido) {
            salida.push_back(CJ::connectionTrustToJson(guardado, std::string()));
        }
        root.set("connections", B::json::Value(std::move(salida)));
        ST::Aviso avisoEscritura;
        if (!ST::escribirTrustStore(raw->dirConfig, root, avisoEscritura)) {
            if (errorOut) {
                *errorOut = "no se pudo escribir trust-store.json";
            }
            return false;
        }
        return true;
    };

    // Sin ventana no hay nada que dejar respirar, y sin hilos no hay a dónde desviar el
    // montaje de túneles: los dos enganches se quedan sin poner a propósito.
    return s;
}

bool ejecutarAgente(Sesion& s,
                    const B::ConnectionProfile& p,
                    const std::vector<std::string>& args,
                    std::string& out,
                    std::string& err,
                    int& rc,
                    std::string* motivo,
                    int timeoutMs) {
    out.clear();
    err.clear();
    rc = -1;
    if (motivo) {
        motivo->clear();
    }
    if (args.empty()) {
        if (motivo) {
            *motivo = "sin verbo";
        }
        return false;
    }
    namespace T = zfsmgr::base::transport;

    if (T::isLocalConnection(p)) {
        std::string srv;
        std::string cli;
        std::string key;
        std::uint16_t puerto = 47653;
        if (!T::ensureLocalDaemonTlsMaterial(s.transporte, srv, cli, key, puerto)) {
            if (motivo) {
                *motivo = "no se pudo leer el material TLS del daemon local";
            }
            return false;
        }
        T::LocalRpcDiag diag;
        if (!T::runLocalAgentRpc(args, srv, cli, key, puerto, timeoutMs, out, err, rc, &diag)) {
            if (motivo) {
                *motivo = diag.failure.empty() ? "el daemon local no respondió" : diag.failure;
            }
            return false;
        }
        return true;
    }

    if (!T::tryAgentRpcOverSsh(s.transporte, p, args, timeoutMs, out, err, rc, {}, {},
                               /*echoOutputToLog=*/s.verboso)) {
        if (motivo) {
            *motivo = "el daemon de " + (p.name.empty() ? p.id : p.name) + " no respondió por RPC";
        }
        return false;
    }
    return true;
}

}  // namespace zfsmgr::cli
