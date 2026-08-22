#include "daemoninstall.h"

#include "agentversion.h"
#include "daemonpayload.h"
#include "helpers.h"
#include "process.h"
#include "strutil.h"
#include "transportcmd.h"
#include "transportrpc.h"
#include "transporttunnel.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace zfsmgr::base::daemoninstall {
namespace {

namespace DP = zfsmgr::base::daemonpayload;
namespace H = zfsmgr::base::helpers;
namespace T = zfsmgr::base::transport;

std::string leeFicheroEntero(const std::string& ruta) {
    std::ifstream f(ruta, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

std::string etiquetaDe(Fallo f) {
    switch (f) {
        case Fallo::Ninguno:
            return "sin fallo";
        case Fallo::BinarioIlegible:
            return "el binario del daemon no se puede leer o está vacío";
        case Fallo::NoSePudoSubir:
            return "no se pudo copiar el binario a la máquina";
        case Fallo::LaInstalacionFallo:
            return "la instalación falló en la máquina";
    }
    return "sin fallo";
}

std::string plataformaDe(const ConnectionProfile& p) {
    if (T::isWindowsConnection(p)) {
        return "windows";
    }
    const std::string so = toLowerAscii(p.osType);
    if (contains(so, "mac") || contains(so, "darwin") || contains(so, "os x")) {
        return "macos";
    }
    if (contains(so, "freebsd")) {
        return "freebsd";
    }
    return "linux";
}

std::string arquitecturaRemota(TransportSession& ses, const ConnectionProfile& p, bool verboso) {
    if (T::isWindowsConnection(p)) {
        return "x86_64";
    }
    std::string out;
    std::string err;
    int rc = -1;
    if (T::runSsh(ses, p, "uname -m", 15000, out, err, rc, {}, {}, {}, {}, false, verboso)
        && rc == 0) {
        return trim(out);
    }
    return {};
}

std::string guionDeInstalacion(const std::string& plataforma, const std::string& version,
                               const std::string& apiVersion) {
    // El binario entra por la ENTRADA ESTÁNDAR y el guion lo coloca con `install`. Así no
    // hay un segundo canal que pueda quedarse a medias ni un fichero suelto si esto muere.
    const std::string despliegue =
        "tmp_bin='/tmp/zfsmgr-agent.bin.$$'; cat > \"$tmp_bin\"; install -m 700 \"$tmp_bin\" "
        + shSingleQuote(DP::unixBinPath()) + "; rm -f \"$tmp_bin\"; ";
    const std::string conf = DP::simpleConfigPayload(version, apiVersion);
    const std::string tls = DP::tlsBootstrapShellCommand();
    const std::string bin = DP::unixBinPath();
    const std::string confPath = DP::unixConfigPath();
    const std::string tlsFiles = DP::tlsDirPath() + " " + DP::tlsServerCertPath() + " "
                                 + DP::tlsServerKeyPath() + " " + DP::tlsClientCertPath() + " "
                                 + DP::tlsClientKeyPath();

    if (plataforma == "macos") {
        return "mkdir -p /usr/local/libexec /etc/zfsmgr; " + despliegue
               + "cat > " + confPath + " <<'EOF_AGENT_CONF'\n" + conf + "\nEOF_AGENT_CONF\n"
               + "cat > " + DP::macPlistPath() + " <<'EOF_AGENT_PLIST'\n" + DP::macLaunchdPlist()
               + "\nEOF_AGENT_PLIST\n" + tls + "; "
               + "chmod 600 " + confPath + "; chmod 644 " + DP::macPlistPath() + "; "
               + "chown root:wheel " + bin + " " + confPath + " " + DP::macPlistPath() + "; "
               + "chown root:wheel " + tlsFiles + "; "
                 "launchctl bootout system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                 "launchctl bootstrap system "
               + DP::macPlistPath() + " >/dev/null 2>&1 || true; "
                 "launchctl enable system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                 "ok=0; i=0; "
                 "while [ \"$i\" -lt 30 ]; do "
                 "  if launchctl print system/org.zfsmgr.agent >/dev/null 2>&1; then ok=1; break; fi; "
                 "  launchctl kickstart system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                 "  i=$((i+1)); sleep 1; "
                 "done; "
                 "if [ \"$ok\" -ne 1 ]; then echo 'launchd agent not active after install' >&2; exit 1; fi";
    }
    if (plataforma == "freebsd") {
        return "mkdir -p /usr/local/libexec /etc/zfsmgr /usr/local/etc/rc.d; " + despliegue
               // Sin OpenSSL el daemon se instala y no arranca, y el motivo real queda en
               // un error del cargador que no dice qué falta.
               + "ldd_missing=$(ldd " + bin + " 2>&1 | grep 'not found' || true); "
                 "if [ -n \"$ldd_missing\" ]; then "
                 "  printf 'ERROR: el daemon tiene dependencias sin resolver:\\n%s\\n' \"$ldd_missing\" >&2; "
                 "  printf 'Instala OpenSSL con: pkg install openssl\\n' >&2; exit 1; fi; "
               + "cat > " + confPath + " <<'EOF_AGENT_CONF'\n" + conf + "\nEOF_AGENT_CONF\n"
               + "cat > " + DP::freeBsdRcPath() + " <<'EOF_AGENT_RC'\n" + DP::freeBsdRcScript()
               + "\nEOF_AGENT_RC\n" + tls + "; "
               + "chmod 700 " + DP::freeBsdRcPath() + "; chmod 600 " + confPath + "; "
               + "chown root:wheel " + bin + " " + confPath + " " + DP::freeBsdRcPath() + "; "
               + "chown root:wheel " + tlsFiles + "; "
                 "service zfsmgr_agent stop >/dev/null 2>&1 || true; "
                 "service zfsmgr_agent start; sleep 2; "
                 "if ! service zfsmgr_agent onestatus >/dev/null 2>&1; then "
                 "  printf 'ERROR: el daemon no permanece activo tras el arranque\\n' >&2; exit 1; fi";
    }
    return "if ! command -v systemctl >/dev/null 2>&1; then echo 'systemd not available' >&2; "
           "exit 1; fi; mkdir -p /usr/local/libexec /etc/zfsmgr; "
           + despliegue
           + "cat > " + confPath + " <<'EOF_AGENT_CONF'\n" + conf + "\nEOF_AGENT_CONF\n"
           + "cat > " + DP::linuxServicePath() + " <<'EOF_AGENT_SERVICE'\n"
           + DP::linuxSystemdService() + "\nEOF_AGENT_SERVICE\n" + tls + "; "
           + "chmod 600 " + confPath + "; chmod 644 " + DP::linuxServicePath() + "; "
           + "chown root:root " + bin + " " + confPath + " " + DP::linuxServicePath() + "; "
           + "chown root:root " + tlsFiles + "; "
             "systemctl daemon-reload; systemctl enable zfsmgr-agent.service; "
             "systemctl restart zfsmgr-agent.service";
}

Resultado instala(TransportSession& ses, const ConnectionProfile& perfil,
                  const std::string& rutaBinario,
                  const std::function<void(const std::string&)>& traza, bool verboso) {
    Resultado r;
    const std::string plataforma = plataformaDe(perfil);
    r.esMac = (plataforma == "macos");

    const std::string contenido = leeFicheroEntero(rutaBinario);
    if (contenido.empty()) {
        r.fallo = Fallo::BinarioIlegible;
        r.detalle = rutaBinario;
        return r;
    }

    // La versión que va a `agent.conf` se lee DEL BINARIO que se está copiando, no de la
    // que trae este cliente.
    //
    // Casi siempre son la misma —se compilan juntos—, pero no siempre: el agente
    // empaquetado de una plataforma puede ser más viejo si solo se recompiló el de otra.
    // Escribir entonces la versión de este binario deja el `agent.conf` mintiendo, y la
    // máquina se declara al día con un daemon que no lo está.
    r.version = agentversion::versionEnBinario(rutaBinario);
    if (r.version.empty()) {
        r.version = agentversion::laEsperada();
    } else {
        r.versionAtrasada = (r.version != agentversion::laEsperada());
    }
    const std::string api = agentversion::apiEsperada();

    std::string out;
    std::string err;
    int rc = -1;

    if (plataforma == "windows") {
        // Por scp y no por la entrada estándar: PowerShell no vuelve de ReadToEnd() con
        // megabytes, y la instalación se colgaba hasta agotar el plazo.
        const std::string subida = DP::windowsUploadPath();
        if (T::isLocalConnection(perfil)) {
            std::error_code ec;
            std::filesystem::remove(subida, ec);
            std::filesystem::copy_file(rutaBinario, subida, ec);
            if (ec) {
                r.fallo = Fallo::NoSePudoSubir;
                r.detalle = subida + ": " + ec.message();
                return r;
            }
        } else {
            const H::ScpInvocacion inv = H::scpUpload(perfil, rutaBinario, subida, false);
            const ExecResult sr =
                runExecStream(inv.program, inv.args, std::string(), 300000, StreamCallbacks{});
            if (sr.rc != 0) {
                r.fallo = Fallo::NoSePudoSubir;
                r.rc = sr.rc;
                r.detalle = trim(sr.err);
                return r;
            }
        }
        if (!T::runSsh(ses, perfil, H::withSudoCommand(perfil, DP::windowsNativeInstallCommand()),
                       300000, out, err, rc, traza, traza, {}, {}, false, verboso)
            || rc != 0) {
            r.fallo = Fallo::LaInstalacionFallo;
            r.rc = rc;
            r.detalle = trim(err.empty() ? out : err);
            return r;
        }
        return r;
    }

    // allowAgentRpc=false: se está INSTALANDO el agente; desviar esto al RPC del agente que
    // se quiere sustituir no tendría ningún sentido.
    const std::string guion = guionDeInstalacion(plataforma, r.version, api);
    if (!T::runSsh(ses, perfil, H::withSudoStreamInputCommand(perfil, guion), 300000, out, err, rc,
                   traza, traza, {}, contenido, false, verboso)
        || rc != 0) {
        r.fallo = Fallo::LaInstalacionFallo;
        r.rc = rc;
        r.detalle = trim(err.empty() ? out : err);
        return r;
    }

    // Lo que había cacheado del daemon anterior ya no vale: es otro binario, con otro
    // material TLS y detrás de un túnel que apuntaba al que se acaba de matar.
    T::closeTunnelForConnection(ses, perfil);
    T::clearRemoteDaemonTlsCacheForConnection(perfil);
    T::clearLocalDaemonTlsCache();
    return r;
}

}  // namespace zfsmgr::base::daemoninstall
