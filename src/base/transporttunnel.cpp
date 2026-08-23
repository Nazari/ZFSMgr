#include "transporttunnel.h"

#include "helpers.h"
#include "json.h"
#include "procesos.h"
#include "strutil.h"
#include "tlsclient.h"
#include "transportcmd.h"
#include "transportrpc.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace zfsmgr::base::transport {
namespace {

namespace H = zfsmgr::base::helpers;
using Nivel = TransportSession::Nivel;
using Reloj = std::chrono::steady_clock;

long long msDesde(Reloj::time_point t) {
    if (t.time_since_epoch().count() == 0) {
        return -1;  // nunca usado
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(Reloj::now() - t).count();
}

int acota(int v, int minimo, int maximo) { return std::max(minimo, std::min(v, maximo)); }

// --- La caché en memoria del material TLS remoto.
//
// Es global y no de la sesión a propósito: sobrevive a que se reconstruya la ventana, y
// traer el material cuesta una ida y vuelta por SSH.
//
// **La clave sale de las coordenadas de la conexión**, no de su posición en la lista. Ver
// docs/diseno_tecnico_capa_base_sin_qt.md: cuando se indexaba por posición, al borrar una
// conexión la siguiente heredaba lo que había cacheado la anterior.
struct EntradaTlsCache {
    RemoteTlsMaterial material;
    Reloj::time_point traidoEn;
};
std::mutex g_cacheMutex;
std::map<std::string, EntradaTlsCache> g_cache;
constexpr long long kCacheValidezMs = 5 * 60 * 1000;

// Los dos guiones que traen el material del otro lado, uno por familia de sistema.
std::string guionPaqueteUnix() {
    return "set -eu; "
           "for f in /etc/zfsmgr/tls/server.crt /etc/zfsmgr/tls/client.crt "
           "/etc/zfsmgr/tls/client.key; do "
           "  if [ -r \"$f\" ]; then "
           "    printf '__ZFSMGR_TLS_BEGIN__:%s\\n' \"$f\"; "
           "    cat \"$f\"; "
           "    printf '__ZFSMGR_TLS_END__:%s\\n' \"$f\"; "
           "  fi; "
           "done; "
           "if [ -r /etc/zfsmgr/agent.conf ]; then "
           "  port=$(awk -F= '/^[[:space:]]*AGENT_PORT[[:space:]]*=/{print $2}' "
           "/etc/zfsmgr/agent.conf | tail -n1 | tr -d \"' \\t\\r\"); "
           "  if [ -n \"$port\" ]; then printf '__ZFSMGR_AGENT_PORT__:%s\\n' \"$port\"; fi; "
           "fi";
}

// Windows guarda el material bajo C:\ProgramData, no bajo /etc, y su shell por omisión es
// cmd: el bucle de arriba no vale. Las rutas van con barra normal a propósito —Windows las
// acepta— y así los marcadores terminan en «/server.crt» y el analizador del paquete sirve
// sin tocar nada.
std::string guionPaqueteWindows() {
    const std::string dir = "C:/ProgramData/ZFSMgr/agent/tls";
    return "$ErrorActionPreference='SilentlyContinue'; "
           "foreach($f in @('" + dir + "/server.crt','" + dir + "/client.crt','" + dir
           + "/client.key')){ "
             "  if(Test-Path -LiteralPath $f){ "
             "    Write-Output ('__ZFSMGR_TLS_BEGIN__:' + $f); "
             "    Get-Content -LiteralPath $f -Raw; "
             "    Write-Output ('__ZFSMGR_TLS_END__:' + $f); "
             "  } "
             "}; "
             "$conf='C:/ProgramData/ZFSMgr/agent/agent.conf'; "
             "if(Test-Path -LiteralPath $conf){ "
             "  $m=Select-String -LiteralPath $conf -Pattern '^\\s*AGENT_PORT\\s*=\\s*(\\d+)' "
             "     | Select-Object -Last 1; "
             "  if($m){ Write-Output ('__ZFSMGR_AGENT_PORT__:' + $m.Matches[0].Groups[1].Value) } "
             "}";
}

// Cierra el túnel de una clave, si lo hay. El proceso se saca del mapa CON EL CERROJO
// PUESTO y se mata fuera: matarlo dentro tendría el cerrojo cogido hasta segundo y medio,
// y por ahí pasa todo el refresco.
void cierraTunel(TransportSession& ses, const std::string& key) {
    RemoteRpcTunnelState muerto;
    {
        std::lock_guard<std::mutex> lock(ses.mutex);
        const auto it = ses.tunnelsByConnKey.find(key);
        if (it == ses.tunnelsByConnKey.end()) {
            return;
        }
        muerto = std::move(it->second);
        ses.tunnelsByConnKey.erase(it);
    }
    muerto.process.stop(700);
}

}  // namespace

void clearRemoteDaemonTlsCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.clear();
}

void clearRemoteDaemonTlsCacheForConnection(const ConnectionProfile& p) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.erase(remoteDaemonTlsCacheKey(p));
}

void closeAllTunnels(TransportSession& ses) {
    std::map<std::string, RemoteRpcTunnelState> muertos;
    {
        std::lock_guard<std::mutex> lock(ses.mutex);
        muertos.swap(ses.tunnelsByConnKey);
    }
    for (auto& kv : muertos) {
        kv.second.process.stop(700);
    }
}

void closeTunnelForConnection(TransportSession& ses, const ConnectionProfile& p) {
    cierraTunel(ses, remoteDaemonTlsCacheKey(p));
}

bool fetchRemoteDaemonTlsMaterial(const ConnectionProfile& p,
                                  bool forceRefresh,
                                  RemoteTlsMaterial& out,
                                  MotivoFallo* failureReason) {
    out = RemoteTlsMaterial{};
    if (failureReason) {
        *failureReason = MotivoFallo{};
    }
    const std::string key = remoteDaemonTlsCacheKey(p);

    // 1) La caché en memoria.
    if (!forceRefresh) {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        const auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            const long long edad = msDesde(it->second.traidoEn);
            if (edad >= 0 && edad <= kCacheValidezMs) {
                out = it->second.material;
                out.fetchedFromRemote = false;
                out.clientKeyFetchedFromRemote = false;
                return true;
            }
        }
    }

    // 2) Lo que ya está guardado en el perfil. Se mira por texto y no parseando: es la
    // criba barata, y quien lo use de verdad ya valida el PEM antes de montar nada.
    if (!forceRefresh && contains(p.daemonTlsServerCertPem, "BEGIN CERTIFICATE")
        && contains(p.daemonTlsClientCertPem, "BEGIN CERTIFICATE")
        && contains(p.daemonTlsClientKeyPem, "BEGIN")) {
        out.serverCertPem = p.daemonTlsServerCertPem;
        out.clientCertPem = p.daemonTlsClientCertPem;
        out.clientKeyPem = p.daemonTlsClientKeyPem;
        out.daemonPort = (p.daemonTlsPort > 0 && p.daemonTlsPort <= 65535)
                             ? static_cast<std::uint16_t>(p.daemonTlsPort)
                             : static_cast<std::uint16_t>(47653);
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache[key] = EntradaTlsCache{out, Reloj::now()};
        return true;
    }

    // 3) Por SSH.
    std::string cmdPlain;
    if (H::isWindowsOsType(p.osType)) {
        cmdPlain = wrapRemoteCommand(p, guionPaqueteWindows());
    } else {
        cmdPlain = "sh -lc " + shSingleQuote(guionPaqueteUnix());
    }
    // withSudoCommand ya devuelve la orden intacta en Windows, así que este reintento es
    // inocuo allí: simplemente repite el mismo PowerShell.
    const std::string cmdSudo = H::withSudoCommand(p, cmdPlain);

    const auto lee = [&p](const std::string& cmd, int timeoutMs, std::string& outText,
                          std::string& errText) {
        int runRc = -1;
        outText.clear();
        errText.clear();
        return runSshRaw(p, cmd, timeoutMs, outText, errText, runRc) && runRc == 0;
    };

    std::string texto;
    std::string errTexto;
    bool ok = lee(cmdPlain, 12000, texto, errTexto);
    if (!ok) {
        ok = lee(cmdSudo, 15000, texto, errTexto);
    } else if (!contains(texto, "__ZFSMGR_TLS_BEGIN__:")) {
        // En instalaciones con el TLS en 600 root:root, la lectura sin sudo puede devolver
        // rc=0 y no traer material. Se reintenta con sudo.
        std::string sudoOut;
        std::string sudoErr;
        if (lee(cmdSudo, 15000, sudoOut, sudoErr) && contains(sudoOut, "__ZFSMGR_TLS_BEGIN__:")) {
            texto = sudoOut;
            errTexto = sudoErr;
        }
    }
    if (!ok) {
        if (failureReason) {
            *failureReason = {Fallo::MaterialNoSeLee, trim(H::oneLine(errTexto))};
        }
        return false;
    }

    RemoteTlsBundle paquete;
    if (!parseRemoteDaemonTlsBundle(texto, paquete)) {
        if (failureReason) {
            *failureReason = {Fallo::MaterialIncompleto, {}};
        }
        return false;
    }
    out.serverCertPem = paquete.serverCertPem;
    out.clientCertPem = paquete.clientCertPem;
    out.clientKeyPem = paquete.clientKeyPem;
    out.daemonPort = paquete.port;
    // La clave privada solo se entrega una vez: si el remoto ya no la da, vale la guardada.
    if (out.clientKeyPem.empty() && !trim(p.daemonTlsClientKeyPem).empty()) {
        out.clientKeyPem = p.daemonTlsClientKeyPem;
    }
    if (out.clientKeyPem.empty()) {
        if (failureReason) {
            *failureReason = {Fallo::ClaveClienteNoDisponible, {}};
        }
        return false;
    }
    out.fetchedFromRemote = true;
    out.clientKeyFetchedFromRemote = paquete.clientKeyIncluded;
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache[key] = EntradaTlsCache{out, Reloj::now()};
    }
    return true;
}

bool tryReviveRemoteDaemonService(const ConnectionProfile& p) {
    static const std::string kGuion =
        "set +e; "
        "if command -v systemctl >/dev/null 2>&1; then "
        "  systemctl daemon-reload >/dev/null 2>&1 || true; "
        "  systemctl enable zfsmgr-agent.service >/dev/null 2>&1 || true; "
        "  systemctl restart zfsmgr-agent.service >/dev/null 2>&1 || "
        "    systemctl start zfsmgr-agent.service >/dev/null 2>&1 || true; "
        "fi; "
        "if command -v launchctl >/dev/null 2>&1; then "
        "  launchctl bootstrap system /Library/LaunchDaemons/org.zfsmgr.agent.plist "
        ">/dev/null 2>&1 || true; "
        "  launchctl enable system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
        "  launchctl kickstart system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
        "fi; "
        "if command -v service >/dev/null 2>&1; then "
        "  service zfsmgr_agent onestart >/dev/null 2>&1 || "
        "    service zfsmgr_agent start >/dev/null 2>&1 || "
        "    service zfsmgr-agent start >/dev/null 2>&1 || true; "
        "fi; "
        "if [ -x /etc/rc.d/zfsmgr_agent ]; then /etc/rc.d/zfsmgr_agent onestart >/dev/null "
        "2>&1 || true; fi; "
        "if [ -x /usr/local/etc/rc.d/zfsmgr_agent ]; then /usr/local/etc/rc.d/zfsmgr_agent "
        "onestart >/dev/null 2>&1 || true; fi; "
        "if [ -x /etc/init.d/zfsmgr-agent ]; then /etc/init.d/zfsmgr-agent restart "
        ">/dev/null 2>&1 || /etc/init.d/zfsmgr-agent start >/dev/null 2>&1 || true; fi; "
        "exit 0";
    std::string out;
    std::string err;
    int rc = -1;
    return runSshRaw(p, H::withSudoCommand(p, "sh -lc " + shSingleQuote(kGuion)), 15000, out, err,
                     rc);
}

bool tryRunRemoteAgentRpcViaTunnel(TransportSession& ses,
                                   const ConnectionProfile& p,
                                   const std::vector<std::string>& agentArgs,
                                   int timeoutMs,
                                   std::string& out,
                                   std::string& err,
                                   int& rc,
                                   MotivoFallo* failureReason,
                                   bool* commandMayHaveRunOut) {
    if (failureReason) {
        *failureReason = MotivoFallo{};
    }
    if (commandMayHaveRunOut) {
        *commandMayHaveRunOut = false;
    }
    if (!ses.puedeMontarTuneles()) {
        if (failureReason) {
            *failureReason = {Fallo::FueraDelHiloDeTuneles, {}};
        }
        return false;
    }

    out.clear();
    err.clear();
    rc = -1;
    if (agentArgs.empty()) {
        if (failureReason) {
            *failureReason = {Fallo::ArgumentosVacios, {}};
        }
        return false;
    }
    if (toLowerAscii(p.connType) != "ssh") {
        if (failureReason) {
            *failureReason = {Fallo::ConexionNoSsh, {}};
        }
        return false;
    }

    const std::string rpcConnKey = remoteDaemonTlsCacheKey(p);
    // Reentrancia: si el túnel de esta conexión se está montando en un marco anterior de la
    // pila, NO se monta otro ni se intenta nada.
    //
    // Se sale con el motivo reservado, que NO cuenta como fallo: la primera versión de esto
    // devolvía un false corriente, el llamante lo tomaba por un túnel caído, metía la
    // conexión en espera de 30 s y con eso tumbaba el refresco de verdad. **Estar ocupado
    // no es estar roto.**
    {
        std::lock_guard<std::mutex> lock(ses.mutex);
        if (ses.tunnelsBeingCreated.count(rpcConnKey) > 0) {
            if (failureReason) {
                *failureReason = {Fallo::TunelOcupado, {}};
            }
            return false;
        }
    }

    // Túneles muertos o parados hace más de un minuto: fuera.
    {
        std::vector<std::string> viejos;
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            for (auto& kv : ses.tunnelsByConnKey) {
                const long long parado = msDesde(kv.second.lastUsed);
                if (!kv.second.process.isRunning() || (parado >= 0 && parado > 60000)) {
                    viejos.push_back(kv.first);
                }
            }
        }
        for (const std::string& k : viejos) {
            cierraTunel(ses, k);
        }
    }

    // Monta —o reutiliza— el túnel de esta conexión. Devuelve el puerto local.
    const auto aseguraTunel = [&](std::uint16_t remotePort, std::uint16_t& localPortOut) -> bool {
        localPortOut = 0;
        // Marca de «montando este túnel ahora mismo», contra la REENTRANCIA que los
        // multiplicaba. Montar un túnel espera a que el puerto acepte conexiones, y esa
        // espera deja respirar a la interfaz; en ese hueco saltaba el temporizador del
        // latido, pedía RPC a la MISMA conexión, no encontraba túnel en el mapa —el de
        // fuera todavía no se había registrado— y montaba un segundo. Al terminar, el
        // registro del de fuera pisaba la entrada del de dentro y lo dejaba huérfano: vivo
        // y fuera del mapa, así que ni se reutilizaba ni se cerraba nunca. Medido: 3
        // túneles por máquina donde debe haber 1.
        struct Marca {
            TransportSession* ses;
            std::string key;
            ~Marca() {
                std::lock_guard<std::mutex> lock(ses->mutex);
                ses->tunnelsBeingCreated.erase(key);
            }
        };
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            ses.tunnelsBeingCreated.insert(rpcConnKey);
        }
        Marca marca{&ses, rpcConnKey};

        bool hayQueRehacer = false;
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            const auto it = ses.tunnelsByConnKey.find(rpcConnKey);
            if (it != ses.tunnelsByConnKey.end()) {
                const long long parado = msDesde(it->second.lastUsed);
                const bool sirve = it->second.process.isRunning()
                                   && it->second.remotePort == remotePort
                                   && !(parado >= 0 && parado > 45000);
                if (sirve) {
                    it->second.lastUsed = Reloj::now();
                    localPortOut = it->second.localPort;
                    return localPortOut > 0;
                }
                hayQueRehacer = true;
            }
        }
        if (hayQueRehacer) {
            cierraTunel(ses, rpcConnKey);
        }

        const std::uint16_t localPort = reserveFreeLocalPort();
        if (localPort == 0) {
            return false;
        }

        std::string programa = "ssh";
        std::vector<std::string> args;
        const bool hayClave = !trim(p.password).empty();
        bool conSshpass = false;
        // La contraseña viaja por un descriptor, no por el argv. Este objeto tiene que
        // seguir vivo hasta el `start()` de más abajo, que es quien lanza el proceso;
        // por eso se declara en este ámbito y no dentro del `if`.
        H::SecretoPorDescriptor secreto(hayClave ? p.password : std::string());
        if (hayClave) {
            const std::string sshpassExe = H::findLocalExecutable("sshpass");
            if (!sshpassExe.empty() && secreto.vale()) {
                programa = sshpassExe;
                args.push_back(secreto.opcionSshpass());
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
        // Sin esto, ssh se queda conectado aunque el reenvío falle, y el túnel parecería
        // vivo mientras nadie escucha en el puerto.
        args.push_back("-o"); args.push_back("ExitOnForwardFailure=yes");
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
        args.push_back("-L");
        args.push_back(std::to_string(localPort) + ":127.0.0.1:" + std::to_string(remotePort));
        args.push_back("-N");
        args.push_back(H::sshUserHost(p));

        RemoteRpcTunnelState nuevo;
        if (!nuevo.process.start(programa, args)) {
            return false;
        }

        // Se espera a que el puerto ACEPTE conexiones. Una pausa fija solo bastaba en
        // enlaces calientes: levantar el túnel necesita un saludo SSH entero —más la
        // resolución mDNS de los nombres *.local—, que medido contra unib.local son unos
        // 830 ms. Conectarse antes da ECONNREFUSED, que el llamante contaba como fallo del
        // saludo TLS y le valía a la conexión un castigo que no arregla nada.
        const auto inicio = Reloj::now();
        bool listo = false;
        bool sshMurio = false;
        while (msDesde(inicio) < 5000) {
            if (!nuevo.process.isRunning()) {
                sshMurio = true;
                break;
            }
            if (canConnectLocal(localPort, 200)) {
                listo = true;
                break;
            }
            // Sin entrada de usuario: ver TransportSession::pump. Por aquí se colaba una
            // recarga de conexiones que dejaba colgando las referencias de quien llamó.
            ses.respira(/*permitirEntradaDeUsuario=*/false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!listo) {
            // El tiempo REAL y el motivo, no un «5 s» fijo. Este bucle sale antes si el ssh
            // muere —máquina apagada, sin ruta—, así que el mensaje anterior afirmaba cinco
            // segundos cuando podían haber sido doscientos milisegundos. Un registro que
            // miente sobre cuánto tardó algo hace perder horas buscando la lentitud donde
            // no está.
            ses.aviso(Nivel::Warn, p.id,
                      {sshMurio ? Aviso::TunelNoAceptaSshMurio : Aviso::TunelNoAceptaEsperaAgotada,
                       {}, std::to_string(msDesde(inicio))});
            nuevo.process.stop(1500);
            return false;
        }

        nuevo.localPort = localPort;
        nuevo.remotePort = remotePort;
        nuevo.startedAt = Reloj::now();
        nuevo.lastUsed = nuevo.startedAt;
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            ses.tunnelsByConnKey[rpcConnKey] = std::move(nuevo);
        }
        localPortOut = localPort;
        return true;
    };

    MotivoFallo motivo;
    const auto intento = [&](bool forceRefreshTls) -> bool {
        RemoteTlsMaterial mat;
        if (!fetchRemoteDaemonTlsMaterial(p, forceRefreshTls, mat, &motivo)) {
            return false;
        }
        if (mat.fetchedFromRemote) {
            std::string persistErr;
            if (!ses.persistTls(p, mat.serverCertPem, mat.clientCertPem, mat.clientKeyPem,
                                mat.daemonPort, &persistErr)) {
                ses.log(Nivel::Warn,
                        "daemon-rpc TLS persist fallback " + p.name + " -> "
                            + (persistErr.empty() ? std::string("upsert failed") : persistErr));
            }
        }
        // Se valida ANTES de montar el túnel: descubrirlo dentro del saludo costaría casi
        // un segundo y el fallo se leería como problema de red.
        if (!pemCertificateIsValid(mat.serverCertPem) || !pemCertificateIsValid(mat.clientCertPem)) {
            motivo = {Fallo::CertificadosInvalidos, {}};
            return false;
        }
        if (!pemPrivateKeyIsValid(mat.clientKeyPem)) {
            motivo = {Fallo::ClaveClienteInvalida, {}};
            return false;
        }

        std::uint16_t localPort = 0;
        if (!aseguraTunel(mat.daemonPort, localPort) || localPort == 0) {
            motivo = {Fallo::TunelNoSeMonta, {}};
            return false;
        }

        TlsClientConfig tls;
        tls.host = "127.0.0.1";
        tls.port = localPort;
        tls.serverCertPem = mat.serverCertPem;
        tls.clientCertPem = mat.clientCertPem;
        tls.clientKeyPem = mat.clientKeyPem;
        tls.connectTimeoutMs = acota(timeoutMs > 0 ? timeoutMs / 5 : 1200, 600, 3500);
        // timeoutMs <= 0 significa SIN LÍMITE, igual que en el camino SSH al que esta vía
        // sustituye. Antes se convertía en 30 s, y un tope adicional capaba además
        // cualquier plazo explícito: pedir 600000 daba 70 s.
        //
        // Desglosar, Ensamblar y Hacia Dir piden 0 a propósito porque copian datos reales.
        // Al expirar se cerraba el túnel y se reintentaba, y cerrar el túnel NO aborta nada
        // en el remoto —comprobado: el daemon completa la operación tras la desconexión—,
        // así que la misma orden destructiva podía solaparse consigo misma.
        //
        // «Sin límite» no es «colgado para siempre»: la espera sale en cuanto el proceso
        // del túnel muere, que es la misma red de seguridad que tiene SSH.
        tls.ioTimeoutMs = (timeoutMs <= 0) ? 0 : std::max(1000, timeoutMs);

        json::Value req;
        req.set("cmd", json::Value(trim(agentArgs.front())));
        json::Array jargs;
        for (std::size_t i = 1; i < agentArgs.size(); ++i) {
            jargs.push_back(json::Value(agentArgs[i]));
        }
        req.set("args", json::Value(std::move(jargs)));

        TlsRequestHooks hooks;
        // Se marca ANTES de escribir, no después: una escritura parcial puede llegar al
        // daemon y arrancar la orden, así que a partir de aquí todo es ambiguo.
        hooks.onBeforeWrite = [&commandMayHaveRunOut]() {
            if (commandMayHaveRunOut) {
                *commandMayHaveRunOut = true;
            }
        };
        // Mientras se espera respuesta: dejar respirar a quien nos llamó y comprobar que el
        // túnel sigue vivo. Si murió, no hay nada que esperar.
        hooks.keepWaiting = [&ses, &rpcConnKey]() {
            ses.respira(/*permitirEntradaDeUsuario=*/false);
            std::lock_guard<std::mutex> lock(ses.mutex);
            const auto it = ses.tunnelsByConnKey.find(rpcConnKey);
            return it != ses.tunnelsByConnKey.end() && it->second.process.isRunning();
        };

        // UN SOLO intento de sesión TLS, sin recorrer nombres de par. Los dos que había
        // existían para la verificación de nombre de host de Qt; con la fijación explícita
        // del certificado el nombre no interviene, y el propio código ya avisaba de que
        // tras escribir la petición no se podía probar el segundo sin ENVIAR LA MISMA ORDEN
        // otra vez.
        std::string respuesta;
        std::string errTls;
        TlsFailure punto = TlsFailure::None;
        if (!tlsRequestLine(tls, json::toCompact(req), respuesta, errTls, punto, hooks)) {
            switch (punto) {
                case TlsFailure::Connect:
                    // El socket no llegó a abrirse —puerto rechazado, túnel a medio montar—.
                    // Contarlo como fallo del saludo TLS apunta el diagnóstico a los
                    // certificados y marca la conexión como «TLS desincronizado», que
                    // dispara un reaprovisionamiento incapaz de arreglar un problema de
                    // transporte.
                    motivo = {Fallo::ConexionRechazada, errTls};
                    break;
                case TlsFailure::Pinning:
                    motivo = {Fallo::CertificadoNoCoincide, {}};
                    break;
                case TlsFailure::Write:
                    motivo = {Fallo::EnvioFallido, {}};
                    break;
                case TlsFailure::Read:
                    motivo = {Fallo::TunelCortadoEnEspera, {}};
                    break;
                default:
                    motivo = {Fallo::HandshakeFallido, errTls};
                    break;
            }
            cierraTunel(ses, rpcConnKey);
            if (motivo.vacio()) {
                motivo = {Fallo::RespuestaNoValida, {}};
            }
            return false;
        }

        json::Value resp;
        std::string errJson;
        if (!json::parse(respuesta, resp, &errJson)) {
            motivo = {Fallo::RespuestaNoValida, errJson};
            cierraTunel(ses, rpcConnKey);
            return false;
        }
        rc = static_cast<int>(resp["rc"].toInt(1));
        out = resp["stdout"].toString();
        err = resp["stderr"].toString();
        {
            std::lock_guard<std::mutex> lock(ses.mutex);
            const auto it = ses.tunnelsByConnKey.find(rpcConnKey);
            if (it != ses.tunnelsByConnKey.end()) {
                it->second.lastUsed = Reloj::now();
            }
        }
        return true;
    };

    if (intento(false)) {
        return true;
    }
    if (commandMayHaveRunOut && *commandMayHaveRunOut) {
        // La petición YA llegó al daemon. Reintentar enviaría la misma orden por segunda
        // vez mientras la primera puede seguir corriendo en la otra máquina, lo que para
        // una mutación significa trabajo destructivo duplicado.
        if (failureReason && !motivo.vacio()) {
            *failureReason = motivo;
        }
        return false;
    }
    // La decisión sale del TIPO del fallo, no de leer su frase. `sugiereRevivirDaemon` es
    // un `switch` sin `default`: un motivo nuevo no compila hasta haber dicho si esto le
    // toca. Antes, un motivo nuevo simplemente no casaba con ninguna cadena y el reintento
    // dejaba de intentarse sin que nadie se enterara.
    if (sugiereRevivirDaemon(motivo.fallo)) {
        if (tryReviveRemoteDaemonService(p)) {
            ses.log(Nivel::Info, "daemon-rpc revive requested on " + p.name + " after failure: "
                                     + etiquetaDe(motivo.fallo));
        }
    }
    if (intento(true)) {
        return true;
    }
    if (failureReason && !motivo.vacio()) {
        *failureReason = motivo;
    }
    return false;
}

}  // namespace zfsmgr::base::transport
