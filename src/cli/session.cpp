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
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif
#include <mutex>

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

// ¿Se quedó algún secreto sin abrir? Es distinto de «no hay secreto»: con --no-secrets o
// con la maestra equivocada los campos quedan vacíos, y sin esta marca el listado diría
// que la conexión no tiene usuario cuando lo que pasa es que no se puede leer.
bool algunSecretoSinAbrir(const B::ConnectionProfile& crudo, const std::string& maestra) {
    for (const std::string* campo : {&crudo.username, &crudo.password,
                                     &crudo.daemonTlsServerCertPem, &crudo.daemonTlsClientCertPem,
                                     &crudo.daemonTlsClientKeyPem}) {
        if (!B::SecretCipher::isEncrypted(*campo)) {
            continue;
        }
        std::string claro;
        std::string err;
        if (maestra.empty() || !B::SecretCipher::decryptEncv1(*campo, maestra, claro, err)) {
            return true;
        }
    }
    return false;
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

bool Conexiones::tieneTls(const std::string& id) const {
    return conTls.count(B::toLowerAscii(B::trim(id))) > 0;
}
bool Conexiones::secretoSinAbrir(const std::string& id) const {
    return secretosSinAbrir.count(B::toLowerAscii(B::trim(id))) > 0;
}
bool Conexiones::desconectada(const std::string& id) const {
    return desconectadas.count(B::toLowerAscii(B::trim(id))) > 0;
}

Tabla tablaDeConexiones(const Conexiones& c) {
    Tabla t;
    t.nombreJson = "connections";
    t.cabecerasTexto = {"ID",     "NOMBRE", "TIPO", "SO",  "USUARIO",
                        "HOST",   "PUERTO", "SUDO", "TLS", "CONECTADA"};
    t.campos = {"id",   "name",   "type", "os",  "user",
                "host", "port",   "sudo", "tls", "connected"};
    t.tipos = {Tipo::Cadena,   Tipo::Cadena, Tipo::Cadena,   Tipo::Cadena,   Tipo::Cadena,
               Tipo::Cadena,   Tipo::Entero, Tipo::Booleano, Tipo::Booleano, Tipo::Booleano};
    for (const auto& p : c.perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        const bool local = CJ::isLocalProfile(p);
        // Un usuario que no se ha podido descifrar sale marcado y NUNCA vacío: vacío se
        // leería como «no tiene usuario», que es otra cosa. Sale igual en los tres
        // formatos porque es un dato, no una decoración: quien lo lea tiene que saber que
        // ahí falta la contraseña maestra.
        std::string usuario;
        if (!local) {
            usuario = p.username.empty() && c.secretoSinAbrir(id) ? "<cifrado>" : p.username;
        }
        t.filas.push_back({p.id,
                           p.name,
                           p.connType.empty() ? std::string("SSH") : p.connType,
                           p.osType,
                           usuario,
                           local ? std::string() : p.host,
                           local ? std::string() : std::to_string(p.port),
                           p.useSudo ? "true" : "false",
                           c.tieneTls(id) ? "true" : "false",
                           c.desconectada(id) ? "false" : "true"});
    }
    return t;
}

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
        if (t.id.empty()) {
            continue;
        }
        // Si TIENE material, se anota AHORA, mirando el valor crudo: descifrarlo puede
        // fallar y dejarlo vacío, y entonces parecería que no lo tiene.
        if (CJ::profileHasDaemonTls(t)) {
            c.conTls.insert(B::toLowerAscii(t.id));
        }
        if (algunSecretoSinAbrir(t, maestra)) {
            c.secretosSinAbrir.insert(B::toLowerAscii(t.id));
        }
        descifraPerfil(t, maestra);
        tlsPorId[B::toLowerAscii(t.id)] = t;
    }

    // Las apartadas, de la misma lectura del fichero.
    for (const auto& v : root["app"]["disconnected_connections"].toArray()) {
        c.desconectadas.insert(B::toLowerAscii(B::trim(v.toString())));
    }

    for (const auto& v : root["connections"].toArray()) {
        auto p = CJ::connectionFromJson(v, std::string());
        const std::string idBajo = B::toLowerAscii(p.id.empty() ? p.name : p.id);
        if (CJ::profileHasDaemonTls(p)) {
            c.conTls.insert(idBajo);
        }
        if (algunSecretoSinAbrir(p, maestra)) {
            c.secretosSinAbrir.insert(idBajo);
        }
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
                                    bool verboso,
                                    bool sinSecretos) {
    auto s = std::make_unique<Sesion>();
    s->dirConfig = dirConfig;
    s->maestra = maestra;
    s->verboso = verboso;
    s->sinSecretos = sinSecretos;

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

namespace {

// La clave con la que la interfaz recuerda una conexión en sus ajustes: el identificador
// en minúsculas, o el nombre si no hay identificador. Tiene que coincidir EXACTAMENTE con
// `MainWindow::connectionPersistKey`, o cada mitad marcaría una cosa distinta.
std::string clavePersistencia(const std::string& idONombre) {
    return B::toLowerAscii(B::trim(idONombre));
}

}  // namespace

namespace {

// El directorio del propio ejecutable. Sin Qt no hay `applicationDirPath()`.
std::string dirDelEjecutable() {
#ifdef _WIN32
    char buf[4096] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0) {
        return ".";
    }
    std::string ruta(buf, n);
#else
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    std::string ruta = ec ? std::string() : p.string();
    if (ruta.empty()) {
        // macOS y FreeBSD no tienen /proc/self/exe. Con el cwd basta para el caso que
        // importa: el árbol de compilación.
        return ".";
    }
#endif
    const std::size_t barra = ruta.find_last_of("/\\");
    return barra == std::string::npos ? std::string(".") : ruta.substr(0, barra);
}

// Los nombres con los que puede aparecer una arquitectura. `uname -m` dice «x86_64» en
// Linux y «amd64» en FreeBSD para lo mismo, y «arm64»/«aarch64» también son la misma.
std::vector<std::string> aliasDeArquitectura(const std::string& arqCruda) {
    const std::string a = B::toLowerAscii(B::trim(arqCruda));
    if (a == "amd64" || a == "x86_64" || a == "x64") {
        return {"x86_64", "amd64"};
    }
    if (a == "arm64" || a == "aarch64") {
        return {"arm64", "aarch64"};
    }
    if (!a.empty()) {
        return {a};
    }
    return {"x86_64", "amd64", "arm64", "aarch64"};
}

}  // namespace

std::string rutaDelAgente(const std::string& plataforma, const std::string& arquitectura) {
    const std::string ext = plataforma == "windows" ? ".exe" : "";
    const std::string dir = dirDelEjecutable();
    std::error_code ec;
    for (const std::string& arq : aliasDeArquitectura(arquitectura)) {
        const std::string rel = plataforma + "-" + arq + "/zfsmgr_agent" + ext;
        for (const std::string& base : {dir + "/agents/", dir + "/../agents/",
                                        dir + "/../Resources/agents/",
                                        dir + "/../share/zfsmgr/agents/",
                                        dir + "/builds/agents/", dir + "/../builds/agents/",
                                        dir + "/../../builds/agents/",
                                        std::string("builds/agents/")}) {
            const std::string cand = base + rel;
            if (std::filesystem::is_regular_file(cand, ec)) {
                return std::filesystem::absolute(cand, ec).string();
            }
        }
    }
    return {};
}

bool guardarConexion(Sesion& s, const B::ConnectionProfile& p, std::string& error) {
    error.clear();
    if (B::trim(p.id).empty()) {
        error = "la conexión necesita un identificador";
        return false;
    }
    // **Sin poder leer los secretos NO se escribe.** Un perfil cargado con --no-secrets
    // trae los campos cifrados VACÍOS, y guardarlo así los deja vacíos en el fichero: se
    // pierde la contraseña, sin aviso y sin vuelta atrás. Pasó de verdad con un `edit`.
    if (s.sinSecretos) {
        error = "con --no-secrets no se escribe la configuración: los campos cifrados no se "
                "han podido leer y guardarlos los borraría";
        return false;
    }
    B::ConnectionProfile guardado = p;
    guardado.port = CJ::ensurePort(guardado.connType, guardado.port);
    if (guardado.daemonTlsPort <= 0 || guardado.daemonTlsPort > 65535) {
        guardado.daemonTlsPort = 47653;
    }
    // La contraseña, cifrada. Sin contraseña maestra NO se guarda en claro: dejar una
    // contraseña de acceso legible en disco para ahorrarse un paso es un mal cambio, y es
    // la misma regla que aplica la interfaz.
    if (!guardado.password.empty() && !B::SecretCipher::isEncrypted(guardado.password)) {
        if (s.maestra.empty()) {
            error = "hace falta la contraseña maestra para cifrar la de la conexión";
            return false;
        }
        std::string cifrada;
        std::string err;
        if (!B::SecretCipher::encryptEncv1(guardado.password, s.maestra, cifrada, err)) {
            error = "no se pudo cifrar la contraseña: " + err;
            return false;
        }
        guardado.password = cifrada;
    }

    ST::Aviso aviso;
    auto root = ST::leerConfig(s.dirConfig, aviso);
    B::json::Array salida;
    bool sustituida = false;
    for (const auto& v : root["connections"].toArray()) {
        const auto existente = CJ::connectionFromJson(v, std::string());
        if (B::toLowerAscii(existente.id) == B::toLowerAscii(guardado.id)) {
            salida.push_back(CJ::connectionToJson(guardado, std::string()));
            sustituida = true;
        } else {
            salida.push_back(v);
        }
    }
    if (!sustituida) {
        salida.push_back(CJ::connectionToJson(guardado, std::string()));
    }
    root.set("connections", B::json::Value(std::move(salida)));
    ST::Aviso avisoEscritura;
    if (!ST::escribirConfig(s.dirConfig, root, avisoEscritura)) {
        error = "no se pudo escribir config.json";
        return false;
    }
    return true;
}

bool borrarConexion(Sesion& s, const std::string& id, std::string& error) {
    error.clear();
    ST::Aviso aviso;
    auto root = ST::leerConfig(s.dirConfig, aviso);
    B::json::Array salida;
    bool encontrada = false;
    for (const auto& v : root["connections"].toArray()) {
        const auto existente = CJ::connectionFromJson(v, std::string());
        if (B::toLowerAscii(existente.id) == B::toLowerAscii(B::trim(id))) {
            encontrada = true;
            continue;
        }
        salida.push_back(v);
    }
    if (!encontrada) {
        error = "no hay ninguna conexión con identificador «" + id + "»";
        return false;
    }
    root.set("connections", B::json::Value(std::move(salida)));
    ST::Aviso avisoEscritura;
    if (!ST::escribirConfig(s.dirConfig, root, avisoEscritura)) {
        error = "no se pudo escribir config.json";
        return false;
    }
    return true;
}


bool marcarDesconectada(Sesion& s, const std::string& id, bool desconectada, std::string& error) {
    error.clear();
    const std::string clave = clavePersistencia(id);
    if (clave.empty()) {
        error = "identificador vacío";
        return false;
    }
    ST::Aviso aviso;
    auto root = ST::leerConfig(s.dirConfig, aviso);
    auto app = root["app"];
    B::json::Array lista;
    for (const auto& v : app["disconnected_connections"].toArray()) {
        if (clavePersistencia(v.toString()) != clave) {
            lista.push_back(v);
        }
    }
    if (desconectada) {
        lista.push_back(B::json::Value(clave));
    }
    app.set("disconnected_connections", B::json::Value(std::move(lista)));
    root.set("app", app);
    ST::Aviso avisoEscritura;
    if (!ST::escribirConfig(s.dirConfig, root, avisoEscritura)) {
        error = "no se pudo escribir config.json";
        return false;
    }
    return true;
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
            // El POR QUÉ, no solo el qué. `tryAgentRpcOverSsh` no lo devuelve, pero lo deja
            // anotado en el mapa de castigos al decidir cuánto esperar antes de reintentar:
            // se lee de ahí. Sin esto el usuario recibía «no respondió por RPC», que no
            // dice si falta el material TLS, si la máquina está apagada o si el daemon no
            // está instalado — tres cosas con arreglos distintos.
            std::string razon;
            {
                std::lock_guard<std::mutex> lock(s.transporte.mutex);
                const auto it = s.transporte.retryReasonByConnKey.find(
                    T::remoteDaemonTlsCacheKey(p));
                if (it != s.transporte.retryReasonByConnKey.end()) {
                    razon = it->second;
                }
            }
            const std::string quien = p.name.empty() ? p.id : p.name;
            *motivo = razon.empty() ? "el daemon de " + quien + " no respondió por RPC"
                                    : "el daemon de " + quien + " no respondió: " + razon;
        }
        return false;
    }
    return true;
}

}  // namespace zfsmgr::cli
