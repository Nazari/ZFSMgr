#include "transportrpc.h"

#include "helpers.h"
#include "json.h"
#include "process.h"
#include "strutil.h"
#include "tlsclient.h"
#include "transportcmd.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace zfsmgr::base::transport {
namespace {

namespace H = zfsmgr::base::helpers;

int acota(int v, int minimo, int maximo) { return std::max(minimo, std::min(v, maximo)); }

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
    args.push_back("-o"); args.push_back("BatchMode=yes");
    args.push_back("-o"); args.push_back("ConnectTimeout=10");
    args.push_back("-o"); args.push_back("LogLevel=ERROR");
    args.push_back("-o"); args.push_back("StrictHostKeyChecking=accept-new");
    if (conSshpass) {
        args.push_back("-o"); args.push_back("BatchMode=no");
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

}  // namespace zfsmgr::base::transport
