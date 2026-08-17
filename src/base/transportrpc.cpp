#include "transportrpc.h"

#include "helpers.h"
#include "json.h"
#include "process.h"
#include "strutil.h"
#include "tlsclient.h"
#include "transportcmd.h"
#include "transporttunnel.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace zfsmgr::base::transport {
namespace {

namespace H = zfsmgr::base::helpers;

int acota(int v, int minimo, int maximo) { return std::max(minimo, std::min(v, maximo)); }

using Nivel = TransportSession::Nivel;
using Reloj = std::chrono::steady_clock;

long long msDesde(Reloj::time_point t) {
    if (t.time_since_epoch().count() == 0) {
        return -1;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(Reloj::now() - t).count();
}

std::string leeFichero(const std::string& ruta) {
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Cada línea completa: se recorta y las vacías se descartan. Va al destino de quien llamó
// y, si toca, al registro de la conexión.
void entregaLineas(const TransportSession& ses, const std::string& connId, const std::string& texto,
                   const std::function<void(const std::string&)>& cb, bool alRegistro) {
    for (const std::string& cruda : split(texto, "\n", true)) {
        const std::string linea = trim(cruda);
        if (linea.empty()) {
            continue;
        }
        if (cb) {
            cb(linea);
        }
        if (alRegistro) {
            ses.logConn(Nivel::Normal, connId, linea);
        }
    }
}

void ecoResumen(const TransportSession& ses, const std::string& connId, const std::string& out,
                const std::string& err, bool alRegistro) {
    if (!alRegistro) {
        return;
    }
    if (!trim(out).empty()) {
        ses.logConn(Nivel::Normal, connId, helpers::oneLine(out));
    }
    if (!trim(err).empty()) {
        ses.logConn(Nivel::Normal, connId, helpers::oneLine(err));
    }
}

// --- La caché del material TLS del daemon LOCAL.
struct CacheTlsLocal {
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
    std::uint16_t port{47653};
    Reloj::time_point traidoEn;
};
std::mutex g_localTlsMutex;
CacheTlsLocal g_localTls;

}  // namespace

bool runSshRaw(const ConnectionProfile& p,
               const std::string& remoteCmd,
               int timeoutMs,
               std::string& out,
               std::string& err,
               int& rc) {
    out.clear();
    err.clear();
    rc = -1;

    std::string program = "ssh";
    std::vector<std::string> args;
    // Con contraseña se intenta `sshpass`. Si no está instalado NO se cambia nada y se
    // deja que ssh haga lo que pueda: fallar por falta de credenciales es un mensaje
    // entendible, y colgarse esperando una contraseña que nadie va a teclear no lo es.
    const bool hasPassword = !trim(p.password).empty();
    bool conSshpass = false;
    if (hasPassword) {
        const std::string sshpassExe = H::findLocalExecutable("sshpass");
        if (!sshpassExe.empty()) {
            program = sshpassExe;
            args.push_back("-p");
            args.push_back(p.password);
            args.push_back("ssh");
            conSshpass = true;
        }
    }

    const std::string familyOpt = H::sshAddressFamilyOption(p);
    if (!familyOpt.empty()) {
        args.push_back(familyOpt);
    }
    // **BatchMode se emite UNA sola vez, y con el valor correcto.** En OpenSSH gana el
    // PRIMER valor de cada opción, así que poner `BatchMode=yes` delante y
    // `BatchMode=no` detrás dejaba BatchMode en «yes», que DESACTIVA la
    // autenticación por contraseña. Resultado: cualquier conexión que dependiera de
    // una contraseña guardada fallaba con «Permission denied», y el motivo no estaba
    // a la vista en ninguna parte.
    //
    // Comprobado contra una máquina real: con las dos opciones en ese orden deniega;
    // con solo `BatchMode=no`, entra.
    args.push_back("-o");
    args.push_back(conSshpass ? "BatchMode=no" : "BatchMode=yes");
    args.push_back("-o"); args.push_back("ConnectTimeout=10");
    args.push_back("-o"); args.push_back("LogLevel=ERROR");
    args.push_back("-o"); args.push_back("StrictHostKeyChecking=accept-new");
    if (conSshpass) {
        args.push_back("-o");
        args.push_back("PreferredAuthentications=password,keyboard-interactive,publickey");
        args.push_back("-o"); args.push_back("NumberOfPasswordPrompts=1");
    }
    if (p.port > 0) {
        args.push_back("-p");
        args.push_back(std::to_string(p.port));
    }
    if (!p.keyPath.empty()) {
        args.push_back("-i");
        args.push_back(p.keyPath);
    }
    // Ver asciiSafeShellCommand: en macOS los argumentos de un proceso se descomponen, y
    // la orden remota es un argumento. Inocuo si ya es ASCII.
    args.push_back(H::sshUserHost(p));
    args.push_back(H::asciiSafeShellCommand(remoteCmd));

    const ExecResult r = runExecStream(program, args, std::string(),
                                       timeoutMs > 0 ? timeoutMs : 15000, StreamCallbacks{});
    // Los dos fracasos que quien llama distingue, con el mismo texto que antes: aquí un
    // `false` no significa «el comando dijo que no», significa «no hubo comando».
    if (r.rc == 127 && startsWith(r.err, "cannot start ")) {
        err = "cannot start ssh";
        return false;
    }
    if (r.rc == 124) {
        err = "timeout";
        rc = -1;
        return false;
    }
    out = r.out;
    err = r.err;
    rc = r.rc;
    return true;
}

std::string bindAddressToConnectHost(const std::string& bindAddress) {
    const std::string b = trim(bindAddress);
    if (b.empty()) {
        return "127.0.0.1";
    }
    in_addr v4{};
    if (inet_pton(AF_INET, b.c_str(), &v4) == 1) {
        // 0.0.0.0 significa «escucho en todas», que como destino no es ninguna.
        if (v4.s_addr == 0) {
            return "127.0.0.1";
        }
        char buf[INET_ADDRSTRLEN] = {0};
        return inet_ntop(AF_INET, &v4, buf, sizeof(buf)) ? std::string(buf) : b;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, b.c_str(), &v6) == 1) {
        bool todoCeros = true;
        for (std::size_t i = 0; i < sizeof(v6.s6_addr); ++i) {
            if (v6.s6_addr[i] != 0) {
                todoCeros = false;
                break;
            }
        }
        if (todoCeros) {  // «::»
            return "127.0.0.1";
        }
        char buf[INET6_ADDRSTRLEN] = {0};
        return inet_ntop(AF_INET6, &v6, buf, sizeof(buf)) ? std::string(buf) : b;
    }
    // Ni IPv4 ni IPv6: un valor a mano en agent.conf, o basura. El daemon local está en
    // 127.0.0.1 de todas formas.
    return "127.0.0.1";
}

bool runLocalAgentRpc(const std::vector<std::string>& agentArgs,
                      const std::string& serverCertPem,
                      const std::string& clientCertPem,
                      const std::string& clientKeyPem,
                      std::uint16_t daemonPort,
                      int timeoutMs,
                      std::string& out,
                      std::string& err,
                      int& rc,
                      LocalRpcDiag* diag) {
    out.clear();
    err.clear();
    rc = -1;
    if (diag) {
        *diag = LocalRpcDiag{};
    }
    if (agentArgs.empty()) {
        return false;
    }
    const std::string cmd = agentArgs.front();

    LocalAgentConfig cfg = loadLocalAgentConfig();
    if (daemonPort > 0) {
        cfg.port = daemonPort;
    }

    TlsClientConfig tls;
    tls.host = bindAddressToConnectHost(cfg.bindAddress);
    tls.port = cfg.port;
    tls.serverCertPem = serverCertPem;
    tls.clientCertPem = clientCertPem;
    tls.clientKeyPem = clientKeyPem;
    // TLS contra localhost: o conecta en menos de 10 ms, o rechaza al instante. Un tope de
    // 2500 ms desperdiciaba segundos por llamada con el daemon parado.
    tls.connectTimeoutMs = acota(timeoutMs > 0 ? timeoutMs / 20 : 400, 200, 700);
    // Igual que en el RPC remoto: 0 es «sin límite».
    tls.ioTimeoutMs = (timeoutMs <= 0) ? 0 : std::max(800, timeoutMs);

    json::Value req;
    req.set("cmd", json::Value(cmd));
    json::Array args;
    for (std::size_t i = 1; i < agentArgs.size(); ++i) {
        args.push_back(json::Value(agentArgs[i]));
    }
    req.set("args", json::Value(std::move(args)));

    const auto inicio = std::chrono::steady_clock::now();
    const auto transcurrido = [&inicio]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - inicio)
            .count();
    };

    // Ya no se prueban dos nombres de par. Existían para la verificación de nombre de host
    // de Qt; con la fijación explícita del certificado el nombre no interviene en la
    // validación, así que probar dos era gastar una conexión de más.
    std::string respuesta;
    std::string errorTls;
    if (!tlsRequestLine(tls, json::toCompact(req), respuesta, errorTls)) {
        if (diag) {
            diag->elapsedMs = transcurrido();
            diag->failure = errorTls;
        }
        return false;
    }
    json::Value resp;
    std::string errJson;
    if (!json::parse(respuesta, resp, &errJson)) {
        if (diag) {
            diag->elapsedMs = transcurrido();
            diag->failure = "respuesta ilegible: " + errJson;
        }
        return false;
    }
    rc = static_cast<int>(resp["rc"].toInt(1));
    out = resp["stdout"].toString();
    err = resp["stderr"].toString();
    if (diag) {
        diag->elapsedMs = transcurrido();
    }
    return true;
}


// ---------------------------------------------------------------------------
// Resolución de nombres, solo para contarlo en el registro.
// ---------------------------------------------------------------------------

HostResolution resolveHostAddresses(const std::string& host) {
    HostResolution r;
    const std::string h = trim(host);
    if (h.empty()) {
        r.error = "nombre vacío";
        return r;
    }
    addrinfo pistas{};
    pistas.ai_family = AF_UNSPEC;
    pistas.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int e = getaddrinfo(h.c_str(), nullptr, &pistas, &res);
    if (e != 0 || !res) {
        r.error = gai_strerror(e);
        if (res) {
            freeaddrinfo(res);
        }
        return r;
    }
    for (addrinfo* it = res; it; it = it->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        if (it->ai_family == AF_INET) {
            const auto* a = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            if (inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) {
                r.addresses.push_back(std::string("IPv4:") + buf);
            }
        } else if (it->ai_family == AF_INET6) {
            const auto* a = reinterpret_cast<const sockaddr_in6*>(it->ai_addr);
            if (inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf))) {
                r.addresses.push_back(std::string("IPv6:") + buf);
            }
        }
    }
    freeaddrinfo(res);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// El material TLS del daemon local.
// ---------------------------------------------------------------------------

void clearLocalDaemonTlsCache() {
    std::lock_guard<std::mutex> lock(g_localTlsMutex);
    g_localTls = CacheTlsLocal{};
}

bool ensureLocalDaemonTlsMaterial(TransportSession& ses,
                                  std::string& serverCertPem,
                                  std::string& clientCertPem,
                                  std::string& clientKeyPem,
                                  std::uint16_t& daemonPort) {
    {
        std::lock_guard<std::mutex> lock(g_localTlsMutex);
        const long long edad = msDesde(g_localTls.traidoEn);
        if (edad >= 0 && edad <= 5 * 60 * 1000 && !g_localTls.serverCertPem.empty()) {
            serverCertPem = g_localTls.serverCertPem;
            clientCertPem = g_localTls.clientCertPem;
            clientKeyPem = g_localTls.clientKeyPem;
            daemonPort = g_localTls.port;
            return true;
        }
    }

    // Primero SIN elevar: si la aplicación corre como root, o alguien aflojó los permisos,
    // no hay por qué pedir nada.
    const LocalAgentConfig cfg = loadLocalAgentConfig();
    std::string srv = leeFichero(cfg.tlsCertPath);
    std::string cli = leeFichero(cfg.tlsClientCertPath);
    std::string key = leeFichero(cfg.tlsClientKeyPath);
    std::uint16_t port = cfg.port > 0 ? cfg.port : static_cast<std::uint16_t>(47653);

    if (srv.empty() || cli.empty() || key.empty()) {
#ifdef _WIN32
        // En Windows el material vive bajo C:\ProgramData y lo puede leer el usuario, así
        // que la lectura directa de arriba basta. Si ha fallado no queda camino
        // alternativo: no hay sudo ni intérprete POSIX que ejecute el guion de abajo, y
        // lanzarlo daría un error que no dice nada. Se explica lo que pasa.
        ses.log(Nivel::Warn, "Local: no se pudo leer el material TLS del daemon en "
                                 + cfg.tlsCertPath
                                 + ". Reinstale el daemon desde el menú de la conexión.");
        return false;
#else
        // Mismo guion y mismos marcadores que el camino remoto, para poder reutilizar su
        // analizador en vez de escribir un segundo formato que se desincronice.
        const std::string guion =
            "set -eu; for f in " + shSingleQuote(cfg.tlsCertPath) + " "
            + shSingleQuote(cfg.tlsClientCertPath) + " " + shSingleQuote(cfg.tlsClientKeyPath)
            + "; do "
              "  if [ -r \"$f\" ]; then "
              "    printf '__ZFSMGR_TLS_BEGIN__:%s\\n' \"$f\"; "
              "    cat \"$f\"; "
              "    printf '__ZFSMGR_TLS_END__:%s\\n' \"$f\"; "
              "  fi; "
              "done; "
              "if [ -r "
            + shSingleQuote(defaultAgentConfigPath()) + " ]; then "
              "  port=$(awk -F= '/^[[:space:]]*AGENT_PORT[[:space:]]*=/{print $2}' "
            + shSingleQuote(defaultAgentConfigPath())
            + " | tail -n1 | tr -d \"' \\t\\r\"); "
              "  if [ -n \"$port\" ]; then printf '__ZFSMGR_AGENT_PORT__:%s\\n' \"$port\"; fi; "
              "fi";

        ConnectionProfile sudoProfile;
        sudoProfile.id = "local";
        sudoProfile.connType = "LOCAL";
        sudoProfile.useSudo = true;
        if (!ses.resolveLocalSudo(sudoProfile)) {
            ses.log(Nivel::Warn,
                    "Local: no se pudo leer el material TLS del daemon (faltan credenciales sudo "
                    "locales).");
            return false;
        }
        std::string out;
        std::string err;
        int rc = -1;
        if (!runSsh(ses, sudoProfile, H::withSudoCommand(sudoProfile, "sh -lc " + shSingleQuote(guion)),
                    15000, out, err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false)
            || rc != 0) {
            ses.log(Nivel::Warn, "Local: no se pudo leer el material TLS del daemon -> "
                                     + H::oneLine(err.empty() ? out : err));
            return false;
        }
        RemoteTlsBundle paquete;
        if (!parseRemoteDaemonTlsBundle(out, paquete)) {
            ses.log(Nivel::Warn, "Local: el material TLS del daemon llegó incompleto.");
            return false;
        }
        srv = paquete.serverCertPem;
        cli = paquete.clientCertPem;
        key = paquete.clientKeyPem;
        port = paquete.port;
#endif
    }

    if (srv.empty() || cli.empty() || key.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_localTlsMutex);
        g_localTls.serverCertPem = srv;
        g_localTls.clientCertPem = cli;
        g_localTls.clientKeyPem = key;
        g_localTls.port = port;
        g_localTls.traidoEn = Reloj::now();
    }
    serverCertPem = srv;
    clientCertPem = cli;
    clientKeyPem = key;
    daemonPort = port;
    return true;
}

// ---------------------------------------------------------------------------
// El RPC del agente sobre una conexión SSH.
// ---------------------------------------------------------------------------

bool tryAgentRpcOverSsh(TransportSession& ses,
                        const ConnectionProfile& p,
                        const std::vector<std::string>& agentArgs,
                        int timeoutMs,
                        std::string& out,
                        std::string& err,
                        int& rc,
                        const std::function<void(const std::string&)>& onStdoutLine,
                        const std::function<void(const std::string&)>& onStderrLine,
                        bool echoOutputToLog) {
    if (toLowerAscii(p.connType) != "ssh" || agentArgs.empty()) {
        return false;
    }
    const std::string rpcConnKey = remoteDaemonTlsCacheKey(p);
    const std::string quien = H::sshUserHostPort(p);
    const std::string queOrden = H::maskedAgentArgvForLog(agentArgs);

    // ¿Está esta conexión castigada por un fallo reciente? Sin esto, una conexión con el
    // daemon caído se lleva una ida y vuelta por SSH en cada operación.
    bool sePuedeIntentar = true;
    std::string motivoSuprimido;
    {
        std::lock_guard<std::mutex> lock(ses.mutex);
        const auto it = ses.retryAfterByConnKey.find(rpcConnKey);
        if (it != ses.retryAfterByConnKey.end() && Reloj::now() < it->second) {
            sePuedeIntentar = false;
            const long long quedanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           it->second - Reloj::now())
                                           .count();
            motivoSuprimido = "backoff activo " + std::to_string((quedanMs + 999) / 1000) + "s";
        }
    }

    bool intentoOk = false;
    std::string motivoFallo;
    bool ordenPudoLlegar = false;
    if (sePuedeIntentar) {
        // Al hilo donde se pueden montar túneles, y bloqueando: el resultado se necesita
        // aquí. Ver TransportSession::tunnelsAllowedHere para por qué esto sigue existiendo.
        ses.enElHiloDeTuneles([&]() {
            intentoOk = tryRunRemoteAgentRpcViaTunnel(ses, p, agentArgs, timeoutMs, out, err, rc,
                                                      &motivoFallo, &ordenPudoLlegar);
        });
    }

    if (intentoOk) {
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            ses.retryAfterByConnKey.erase(rpcConnKey);
            ses.retryReasonByConnKey.erase(rpcConnKey);
        }
        ses.logConn(Nivel::Info, p.id, quien + " $ [daemon-rpc] " + queOrden);
        entregaLineas(ses, p.id, out, onStdoutLine, echoOutputToLog);
        entregaLineas(ses, p.id, err, onStderrLine, echoOutputToLog);
        ecoResumen(ses, p.id, out, err, echoOutputToLog);
        return true;
    }

    const std::string motivo = trim(motivoFallo).empty() ? std::string("motivo no especificado")
                                                         : trim(motivoFallo);

    if (sePuedeIntentar && ordenPudoLlegar && isMutatingAgentCommand(agentArgs)) {
        // El daemon recibió una orden que MUTA y nunca llegó su respuesta. Cerrar el túnel
        // no aborta el trabajo remoto, así que caer al camino de SSH aquí ejecutaría la
        // misma orden destructiva por segunda vez, quizá solapada con la primera. Se
        // fracasa en voz alta.
        ses.logConn(Nivel::Error, p.id,
                    quien + " $ [daemon-rpc:sin-fallback] " + queOrden + " -> " + motivo
                        + " (la orden ya llegó al daemon; no se reintenta para no duplicarla)");
        out.clear();
        err = "La orden se envió al daemon pero no se recibió respuesta (" + motivo
              + ").\nPuede seguir ejecutándose en el equipo remoto, así que ZFSMgr no la "
                "reintenta automáticamente.\nCompruebe el estado antes de repetirla.";
        rc = 124;
        return true;
    }
    if (sePuedeIntentar && trim(motivoFallo) == H::rpcTunnelBusyReason()) {
        // Ocupado NO es roto. El túnel se está montando en un marco anterior de la pila, así
        // que esta llamada se salta el RPC y sale por el camino de siempre SIN castigar a la
        // conexión: con el castigo de 30 s, un sondeo que llegara en ese hueco dejaba sin
        // daemon al refresco que venía detrás.
        ses.logConn(Nivel::Info, p.id,
                    quien + " $ [daemon-rpc:skip] " + queOrden + " -> " + H::rpcTunnelBusyReason());
    } else if (sePuedeIntentar) {
        ses.logConn(Nivel::Info, p.id, quien + " $ [daemon-rpc:fallback] " + queOrden + " -> " + motivo);
        constexpr int kCastigoSeg = 30;
        std::lock_guard<std::mutex> lock(ses.mutex);
        ses.retryAfterByConnKey[rpcConnKey] = Reloj::now() + std::chrono::seconds(kCastigoSeg);
        ses.retryReasonByConnKey[rpcConnKey] = motivo;
    } else if (!motivoSuprimido.empty()) {
        ses.logConn(Nivel::Info, p.id,
                    quien + " $ [daemon-rpc:skip] " + queOrden + " -> " + motivoSuprimido);
    }
    return false;
}


// ---------------------------------------------------------------------------
// runSsh: la puerta por la que pasa todo.
// ---------------------------------------------------------------------------

namespace {

// Lo que hacen por igual la rama local y la remota mientras el proceso corre: entregar
// líneas según llegan, avisar de cuánto queda y morir por SILENCIO, no por duración.
struct VigilanteDeInactividad {
    const TransportSession& ses;
    std::string connId;
    int timeoutMs;
    bool echoOutputToLog;
    const std::function<void(const std::string&)>& onStdoutLine;
    const std::function<void(const std::string&)>& onStderrLine;
    const std::function<void(int)>& onIdleTimeoutRemaining;

    Reloj::time_point ultimoDato{Reloj::now()};
    int ultimoAvisoSeg{-1};
    bool porInactividad{false};

    void reinicia() {
        ultimoDato = Reloj::now();
        ultimoAvisoSeg = -1;
    }
    long long inactivoMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Reloj::now() - ultimoDato)
            .count();
    }
    void entrega(const std::string& cruda, const std::function<void(const std::string&)>& cb) {
        const std::string linea = trim(cruda);
        if (linea.empty()) {
            return;
        }
        if (cb) {
            cb(linea);
        }
        if (echoOutputToLog) {
            ses.logConn(Nivel::Normal, connId, linea);
        }
    }
    StreamCallbacks callbacks() {
        StreamCallbacks cbs;
        cbs.onStdoutLine = [this](const std::string& l) {
            reinicia();
            entrega(l, onStdoutLine);
        };
        cbs.onStderrLine = [this](const std::string& l) {
            reinicia();
            entrega(l, onStderrLine);
        };
        cbs.onTick = [this](int) -> bool {
            if (timeoutMs > 0 && onIdleTimeoutRemaining) {
                const int quedanSeg = std::max(
                    0, static_cast<int>((timeoutMs - inactivoMs() + 999) / 1000));
                if (quedanSeg != ultimoAvisoSeg) {
                    ultimoAvisoSeg = quedanSeg;
                    onIdleTimeoutRemaining(quedanSeg);
                }
            }
            if (timeoutMs > 0 && inactivoMs() > timeoutMs) {
                porInactividad = true;
                return false;  // cancela: es la muerte por silencio
            }
            // CON entrada de usuario: es lo que permite pulsar Cancelar mientras corre
            // una transferencia larga.
            ses.respira(/*permitirEntradaDeUsuario=*/true);
            return true;
        };
        return cbs;
    }
};

}  // namespace

bool runSsh(TransportSession& ses,
            const ConnectionProfile& p,
            const std::string& remoteCmd,
            int timeoutMs,
            std::string& out,
            std::string& err,
            int& rc,
            const std::function<void(const std::string&)>& onStdoutLine,
            const std::function<void(const std::string&)>& onStderrLine,
            const std::function<void(int)>& onIdleTimeoutRemaining,
            const std::string& stdinPayload,
            bool allowAgentRpc,
            bool echoOutputToLog) {
    out.clear();
    err.clear();
    rc = -1;

    // Con el transporte de mentira puesto no se abre ninguna conexión. Si la orden es una
    // invocación del agente construida como cadena, se atiende igual —para que los sitios
    // aún sin migrar funcionen en los tests—; si no lo es, se anota y fracasa, que es lo que
    // permite afirmar «esto NO debía irse por shell».
    if (ses.transportForTest) {
        std::vector<std::string> parsedArgs;
        if (allowAgentRpc && stdinPayload.empty()
            && extractLocalAgentArgs(trim(remoteCmd), parsedArgs)) {
            ses.callsForTest.push_back(
                TransportSession::AgentCallForTest{parsedArgs, trim(remoteCmd), stdinPayload});
            return ses.transportForTest(parsedArgs, out, err, rc);
        }
        ses.callsForTest.push_back(
            TransportSession::AgentCallForTest{{}, trim(remoteCmd), stdinPayload});
        err = "transporte de prueba: no se ejecuta shell";
        rc = 127;
        return false;
    }

    // --- La conexión LOCAL: no hay SSH de por medio.
    if (isLocalConnection(p)) {
        const std::string localCmd = trim(remoteCmd);
        ses.logConn(Nivel::Info, p.id, "[local] $ " + localCmd);

        // Una entrada estándar no vacía descarta el RPC: el canal no la transporta, y la
        // interceptación no lo miraba, así que la contraseña de un dataset cifrado se perdía
        // en silencio al desglosarlo.
        std::vector<std::string> localAgentArgs;
        if (allowAgentRpc && stdinPayload.empty()
            && extractLocalAgentArgs(localCmd, localAgentArgs)) {
            bool localRpcOk = false;
            ses.enElHiloDeTuneles([&]() {
                std::string srvPem;
                std::string cliPem;
                std::string keyPem;
                std::uint16_t localPort = 47653;
                localRpcOk =
                    ensureLocalDaemonTlsMaterial(ses, srvPem, cliPem, keyPem, localPort)
                    && runLocalAgentRpc(localAgentArgs, srvPem, cliPem, keyPem, localPort,
                                        timeoutMs, out, err, rc);
            });
            if (localRpcOk) {
                entregaLineas(ses, p.id, out, onStdoutLine, echoOutputToLog);
                entregaLineas(ses, p.id, err, onStderrLine, echoOutputToLog);
                ecoResumen(ses, p.id, out, err, echoOutputToLog);
                return true;
            }
        }

        std::string program;
        std::vector<std::string> args;
#ifdef _WIN32
        program = "cmd.exe";
        args.push_back("/C");
        args.push_back(wrapRemoteCommand(p, localCmd));
#else
        program = "sh";
        args.push_back("-c");
        args.push_back(H::asciiSafeShellCommand(localCmd));
#endif
        VigilanteDeInactividad vig{ses,       p.id,          timeoutMs,          echoOutputToLog,
                                   onStdoutLine, onStderrLine, onIdleTimeoutRemaining};
        // Sin plazo propio del proceso: el control va en onTick, porque el tope es de
        // INACTIVIDAD y no de duración total.
        const ExecResult res = runExecStream(program, args, stdinPayload, 0, vig.callbacks());
        out = res.out;
        err = res.err;
        if (res.rc == 127 && !vig.porInactividad) {
            err = "No se pudo iniciar " + program;
            ses.logConn(Nivel::Normal, p.id, err);
            return false;
        }
        if (vig.porInactividad) {
            rc = -1;
            err = "Timeout";
            ses.logConn(Nivel::Normal, p.id, err);
            return false;
        }
        rc = res.rc;
        // El ruido con forma de XML que escupe PowerShell, también en la conexión LOCAL.
        //
        // Solo se limpiaba en la rama de SSH, y una máquina Windows que se gestiona a sí
        // misma entra por AQUÍ: la orden se envuelve igualmente en PowerShell, así que un
        // fallo llegaba con doscientas líneas de CLIXML por delante del motivo. Venía de
        // antes de sacar el transporte de Qt; se arregla aquí porque es donde toca.
        if (isWindowsConnection(p)) {
            out = sanitizeWindowsCliXml(out);
            err = sanitizeWindowsCliXml(err);
        }
        if (rc != 0) {
            // ssh sale con 255 y un «Host key verification failed» escueto cuando la clave
            // del host no coincide. Sin traducirlo, eso le llega al usuario como un fallo de
            // red cualquiera, y es precisamente el caso que no debe ignorar.
            const std::string pista = H::sshHostKeyProblemHint(err);
            if (!pista.empty()) {
                err = pista + "\n\n" + err;
                ses.log(Nivel::Warn, p.name + ": verificación de host SSH fallida");
            }
        }
        ecoResumen(ses, p.id, out, err, echoOutputToLog);
        return true;
    }

    // --- Por SSH. Windows entra por RPC como cualquier otro sistema: el daemon nativo sirve
    // TLS por el mismo túnel, verificado contra un Windows 11 real ejecutando ZFS.
    //
    // Camino HEREDADO: los argumentos se recuperan analizando la cadena. runAgentCommand los
    // pasa ya hechos y no pasa por aquí. Este análisis desaparece cuando migren todos los
    // sitios; hasta entonces convive con el nuevo.
    if (allowAgentRpc && stdinPayload.empty()) {
        std::vector<std::string> agentArgs;
        if (extractLocalAgentArgs(trim(remoteCmd), agentArgs)
            && tryAgentRpcOverSsh(ses, p, agentArgs, timeoutMs, out, err, rc, onStdoutLine,
                                  onStderrLine, echoOutputToLog)) {
            return true;
        }
    }

    const bool hayClave = !trim(p.password).empty();
    std::string program = "ssh";
    std::vector<std::string> sshpassPrefijo;
    bool conSshpass = false;
    if (hayClave) {
        const std::string sshpassExe = H::findLocalExecutable("sshpass");
        if (!sshpassExe.empty()) {
            program = sshpassExe;
            sshpassPrefijo = {"-p", p.password, "ssh"};
            conSshpass = true;
        }
    }

    const std::string wrappedCmd = wrapRemoteCommand(p, remoteCmd);
    const std::string sshConnKey = p.username + "|" + p.host + "|"
                                   + std::to_string(p.port > 0 ? p.port : 22) + "|" + p.keyPath;
    const std::string sshResolutionKey =
        toLowerAscii(trim(p.host)) + "|" + toLowerAscii(trim(p.sshAddressFamily));

    ses.logConn(Nivel::Info, p.id, H::sshUserHostPort(p) + " $ " + wrappedCmd);
    if (hayClave && !conSshpass) {
        ses.logConn(Nivel::Normal, p.id,
                    "Password guardado, pero sshpass no está disponible; se usará SSH no "
                    "interactivo.");
    }

    const auto intento = [&](bool conMultiplexado, std::string& aOut, std::string& aErr,
                             int& aRc) -> bool {
        aOut.clear();
        aErr.clear();
        aRc = -1;

        std::vector<std::string> args = sshpassPrefijo;
        const std::string familyOpt = H::sshAddressFamilyOption(p);
        if (!familyOpt.empty()) {
            args.push_back(familyOpt);
        }
    // **BatchMode se emite UNA sola vez, y con el valor correcto.** En OpenSSH gana el
    // PRIMER valor de cada opción, así que poner `BatchMode=yes` delante y
    // `BatchMode=no` detrás dejaba BatchMode en «yes», que DESACTIVA la
    // autenticación por contraseña. Resultado: cualquier conexión que dependiera de
    // una contraseña guardada fallaba con «Permission denied», y el motivo no estaba
    // a la vista en ninguna parte.
    //
    // Comprobado contra una máquina real: con las dos opciones en ese orden deniega;
    // con solo `BatchMode=no`, entra.
    args.push_back("-o");
    args.push_back(conSshpass ? "BatchMode=no" : "BatchMode=yes");
        args.push_back("-o"); args.push_back("ConnectTimeout=10");
        args.push_back("-o"); args.push_back("LogLevel=ERROR");
        args.push_back("-o"); args.push_back("StrictHostKeyChecking=accept-new");
        if (conMultiplexado) {
            args.push_back("-o"); args.push_back("ControlMaster=auto");
            args.push_back("-o"); args.push_back("ControlPersist=yes");
            args.push_back("-o"); args.push_back("ControlPath=" + H::sshControlPath());
        }
        if (conSshpass) {
            args.push_back("-o");
            args.push_back("PreferredAuthentications=password,keyboard-interactive,publickey");
            args.push_back("-o"); args.push_back("NumberOfPasswordPrompts=1");
        }
        if (p.port > 0) {
            args.push_back("-p");
            args.push_back(std::to_string(p.port));
        }
        if (!p.keyPath.empty()) {
            args.push_back("-i");
            args.push_back(p.keyPath);
        }
        args.push_back(H::sshUserHost(p));
        args.push_back(H::asciiSafeShellCommand(wrappedCmd));

        VigilanteDeInactividad vig{ses,       p.id,          timeoutMs,          echoOutputToLog,
                                   onStdoutLine, onStderrLine, onIdleTimeoutRemaining};
        const ExecResult res = runExecStream(program, args, stdinPayload, 0, vig.callbacks());
        aOut = res.out;
        aErr = res.err;
        if (res.rc == 127 && !vig.porInactividad) {
            aErr = "No se pudo iniciar " + program;
            ses.logConn(Nivel::Normal, p.id, aErr);
            return false;
        }
        if (vig.porInactividad) {
            aRc = -1;
            aErr = "Timeout";
            ses.logConn(Nivel::Normal, p.id, aErr);
            return false;
        }
        aRc = res.rc;
        return true;
    };

    // A qué se resolvió el nombre, UNA vez por conexión. Solo para los `*.local` —que van
    // por mDNS— y para quien haya forzado familia: son los casos cuyos fallos se
    // diagnostican mal, porque parecen «la máquina no responde».
    const std::string hostLower = toLowerAscii(trim(p.host));
    const std::string familyLower = toLowerAscii(trim(p.sshAddressFamily));
    if ((!hostLower.empty() && endsWith(hostLower, ".local")) || familyLower == "ipv4"
        || familyLower == "ipv6") {
        bool tocaContarlo = false;
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            if (ses.loggedResolutionKeys.count(sshResolutionKey) == 0) {
                ses.loggedResolutionKeys.insert(sshResolutionKey);
                tocaContarlo = true;
            }
        }
        if (tocaContarlo) {
            const HostResolution r = resolveHostAddresses(p.host);
            const std::string familia = familyLower.empty() ? std::string("auto") : familyLower;
            std::string msg = "Resolucion SSH " + p.host + " (" + familia + "): ";
            if (!r.ok) {
                msg += r.error;
                ses.log(Nivel::Warn, p.name + ": " + msg);
            } else {
                msg += r.addresses.empty() ? std::string("sin direcciones") : join(r.addresses, ", ");
                ses.log(Nivel::Info, p.name + ": " + msg);
            }
            ses.logConn(Nivel::Normal, p.id, msg);
        }
    }

    // El multiplexado NO funciona cuando la aplicación corre en Windows: su OpenSSH responde
    // «getsockname failed: Not a socket» (comprobado contra fc16 desde una VM Windows 11), y
    // ControlPersist deja un proceso maestro de fondo que no suelta las tuberías heredadas,
    // así que la espera no vuelve y la ventana se queda bloqueada. El reintento sin
    // multiplexar no salvaba nada: para llegar a él hay que esperar primero a que el intento
    // colgado agote su plazo, y son ~16 órdenes por refresco.
#ifdef _WIN32
    const bool permiteMultiplexado = false;
#else
    bool permiteMultiplexado = true;
    {
        std::lock_guard<std::mutex> lock(ses.mutex);
        permiteMultiplexado = ses.disableMultiplexKeys.count(sshConnKey) == 0;
    }
#endif
    bool arrancoOk = intento(permiteMultiplexado, out, err, rc);
    if (permiteMultiplexado && arrancoOk && rc != 0 && shouldRetrySshWithoutMultiplexing(err)) {
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            ses.disableMultiplexKeys.insert(sshConnKey);
        }
        const std::string aviso =
            "SSH multiplexado falló; reintentando sin ControlMaster/ControlPath.";
        ses.log(Nivel::Warn, p.name + ": " + aviso);
        ses.logConn(Nivel::Normal, p.id, aviso);
        arrancoOk = intento(false, out, err, rc);
    } else if (!permiteMultiplexado) {
        ses.logConn(Nivel::Normal, p.id,
                    "SSH multiplexado deshabilitado para esta conexión en la sesión actual.");
    }

    if (!arrancoOk) {
        return false;
    }
    if (isWindowsConnection(p)) {
        out = sanitizeWindowsCliXml(out);
        err = sanitizeWindowsCliXml(err);
    }
    ecoResumen(ses, p.id, out, err, echoOutputToLog);
    return true;
}

}  // namespace zfsmgr::base::transport
