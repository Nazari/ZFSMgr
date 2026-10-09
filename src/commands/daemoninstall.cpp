#include "daemoninstall.h"

#include "agentversion.h"
#include "daemonpayload.h"
#include "helpers.h"
#include "processes.h"
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

std::string labelOf(Failure f) {
    switch (f) {
        case Failure::None_:
            return "sin fallo";
        case Failure::UnreadableBinary:
            return "el binario del daemon no se puede leer o está vacío";
        case Failure::UploadFailed:
            return "no se pudo copiar el binario a la máquina";
        case Failure::InstallFailed:
            return "la instalación falló en la máquina";
        case Failure::UnsupportedOpenZfs:
            return "esa máquina no tiene una versión de OpenZFS con la que esto funcione";
    }
    return "sin fallo";
}

std::string platformOf(const ConnectionProfile& p) {
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

std::string openZfsProbeCommand() {
    return "for z in zfs /usr/local/zfs/bin/zfs /usr/local/bin/zfs /sbin/zfs /usr/sbin/zfs; do "
           "command -v \"$z\" >/dev/null 2>&1 && { \"$z\" version 2>&1; exit 0; }; done; "
           "echo '__ZFSMGR_SIN_ZFS__'";
}

helpers::OpenZfsLectura readOpenZfsProbe(const std::string& salida) {
    if (salida.find("__ZFSMGR_SIN_ZFS__") != std::string::npos) {
        helpers::OpenZfsLectura r;
        r.soporte = helpers::OpenZfsSoporte::NoHay;
        return r;
    }
    return helpers::leeSoporteOpenZfs(salida);
}

helpers::OpenZfsLectura remoteOpenZfs(TransportSession& ses, const ConnectionProfile& p,
                                      bool verboso) {
    // `zfs version` y no `zfs --version`: la primera es la que existe en todas las ramas, y
    // la segunda no está en las antiguas — que son justo las que hay que detectar.
    //
    // Las rutas se nombran a mano porque el PATH de una sesión SSH no interactiva NO es el
    // del terminal: en macOS, OpenZFS vive en /usr/local/zfs/bin y `zfs` a secas no se
    // encuentra. Comprobado en mmela y en mbp, donde `zfs version` por SSH no daba nada
    // mientras su daemon lo usaba sin problema.
    //
    // Y se junta la salida normal con la de error, porque lo que distingue «no está» de
    // «está pero sin cargar» viene por la segunda.
    const std::string orden = openZfsProbeCommand();
    std::string out;
    std::string err;
    int rc = -1;
    if (T::isWindowsConnection(p)) {
        // En Windows el reparto es otro y la comprobación de versión todavía no está
        // escrita; no se bloquea por algo que no se sabe mirar.
        helpers::OpenZfsLectura r;
        r.soporte = helpers::OpenZfsSoporte::Ok;
        return r;
    }
    T::runSsh(ses, p, orden, 20000, out, err, rc, {}, {}, {}, {}, false, verboso);
    return readOpenZfsProbe(out + "\n" + err);
}

std::string remoteArchitecture(TransportSession& ses, const ConnectionProfile& p, bool verboso) {
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

std::string installScript(const std::string& plataforma, const std::string& version,
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
               // **`bootout` no es inmediato, y de ahí salía una instalación que decía que
               // sí y dejaba la máquina sin daemon.** El trabajo sigue en el dominio unos
               // instantes después de sacarlo; `bootstrap` lanzado ahí falla —«Bootstrap
               // failed: 5»— y el `|| true` se lo tragaba; y la comprobación se conformaba
               // con que `print` encontrara ALGO con esa etiqueta, que era justo el
               // cadáver del servicio anterior. Salía «daemon instalado» y un minuto
               // después no había ni proceso ni puerto. Comprobado en mmela el 2026-08-23.
               //
               // Tres cambios, uno por cada eslabón: esperar a que el viejo se vaya de
               // verdad, reintentar el `bootstrap` GUARDANDO su motivo, y exigir un PID
               // —que el trabajo esté declarado no es que esté corriendo—.
               + "launchctl bootout system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                 "i=0; "
                 "while [ \"$i\" -lt 20 ]; do "
                 "  launchctl print system/org.zfsmgr.agent >/dev/null 2>&1 || break; "
                 "  i=$((i+1)); sleep 1; "
                 "done; "
                 "err=''; i=0; "
                 "while [ \"$i\" -lt 10 ]; do "
                 "  err=\"$(launchctl bootstrap system "
               + DP::macPlistPath() + " 2>&1)\" && break; "
                 "  i=$((i+1)); sleep 1; "
                 "done; "
                 "launchctl enable system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                 "pid=''; i=0; "
                 "while [ \"$i\" -lt 30 ]; do "
                 "  pid=\"$(launchctl print system/org.zfsmgr.agent 2>/dev/null | "
                 "         sed -n 's/^[[:space:]]*pid = \\([0-9][0-9]*\\).*/\\1/p' | head -1)\"; "
                 "  [ -n \"$pid\" ] && break; "
                 "  launchctl kickstart system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                 "  i=$((i+1)); sleep 1; "
                 "done; "
                 "if [ -z \"$pid\" ]; then "
                 "  echo \"launchd no dejo el agente corriendo tras instalar; bootstrap dijo: $err\" >&2; "
                 "  exit 1; fi";
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
                 // Parada ACOTADA. Un agente de antes de este arreglo no sabía pararse —se quedaba
                 // esperando al lector de `zpool events`— y `rc.d` espera sin límite, así que
                 // actualizar uno de esos colgaba la instalación. Si en 30 s no se ha ido, se
                 // mata por NOMBRE EXACTO: `pkill -f` con la ruta casaría también con este
                 // mismo guion, que la lleva escrita, y se mataría a sí mismo.
                 "timeout 30 service zfsmgr_agent stop >/dev/null 2>&1 || { "
                 // El GRUPO del supervisor, no solo su PID: ahí viven el agente viejo y su
                 // `zpool events`. Matando solo el PID, ese `zpool` quedaba huérfano.
                 "  [ -f /var/run/zfsmgr_agent.pid ] && kill -9 -- -\"$(cat /var/run/zfsmgr_agent.pid)\" 2>/dev/null; "
                 "  pkill -9 -x zfsmgr-agent 2>/dev/null; rm -f /var/run/zfsmgr_agent.pid; sleep 1; }; "
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

Result install(TransportSession& ses, const ConnectionProfile& perfil,
                  const std::string& rutaBinario,
                  const std::function<void(const std::string&)>& traza, bool verboso) {
    Result r;
    const std::string plataforma = platformOf(perfil);
    r.isMac = (plataforma == "macos");

    const std::string contenido = leeFicheroEntero(rutaBinario);
    if (contenido.empty()) {
        r.fallo = Failure::UnreadableBinary;
        r.detail = rutaBinario;
        return r;
    }

    // **La versión de OpenZFS, antes de subir nada.** Un agente instalado donde no hay ZFS
    // arranca, contesta STATUS=OK y no ve un solo pool; desde el cliente eso se lee como «la
    // máquina está bien y está vacía», que es exactamente lo que no es. Mismo razonamiento
    // que el del respaldo por guion: una máquina que PARECE atendida y no lo está es peor
    // que una que falla a la cara.
    //
    // Va DESPUÉS de leer el binario y no antes, y la razón la puso una prueba que ya
    // existía: «sin binario no se toca la maquina». Preguntar la versión ABRE UNA SESIÓN
    // SSH, o sea que toca la máquina; hacerlo primero rompía ese contrato por molestar a la
    // otra punta para contestar algo que no se iba a poder hacer de todas formas. Leer el
    // fichero es local y gratis: primero lo que no cuesta nada.
    {
        const helpers::OpenZfsLectura zfs = remoteOpenZfs(ses, perfil, verboso);
        if (zfs.soporte != helpers::OpenZfsSoporte::Ok) {
            r.fallo = Failure::UnsupportedOpenZfs;
            r.detail = helpers::textoDeSoporteOpenZfs(zfs);
            r.version = zfs.version;
            return r;
        }
        if (traza) {
            traza("OpenZFS " + zfs.version + " en la otra máquina: vale");
        }
    }

    // La versión que va a `agent.conf` se lee DEL BINARIO que se está copiando, no de la
    // que trae este cliente.
    //
    // Casi siempre son la misma —se compilan juntos—, pero no siempre: el agente
    // empaquetado de una plataforma puede ser más viejo si solo se recompiló el de otra.
    // Escribir entonces la versión de este binario deja el `agent.conf` mintiendo, y la
    // máquina se declara al día con un daemon que no lo está.
    r.version = agentversion::versionInBinary(rutaBinario);
    if (r.version.empty()) {
        r.version = agentversion::expected();
    } else {
        r.versionBehind = (r.version != agentversion::expected());
    }
    const std::string api = agentversion::expectedApi();

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
                r.fallo = Failure::UploadFailed;
                r.detail = subida + ": " + ec.message();
                return r;
            }
        } else {
            const H::ScpInvocation inv = H::scpUpload(perfil, rutaBinario, subida, false);
            const ExecResult sr =
                runExecStream(inv.program, inv.args, std::string(), 300000, StreamCallbacks{});
            if (sr.rc != 0) {
                r.fallo = Failure::UploadFailed;
                r.rc = sr.rc;
                r.detail = trim(sr.err);
                return r;
            }
        }
        if (!T::runSsh(ses, perfil, H::withSudoCommand(perfil, DP::windowsNativeInstallCommand()),
                       300000, out, err, rc, traza, traza, {}, {}, false, verboso)
            || rc != 0) {
            r.fallo = Failure::InstallFailed;
            r.rc = rc;
            r.detail = trim(err.empty() ? out : err);
            return r;
        }
        return r;
    }

    // allowAgentRpc=false: se está INSTALANDO el agente; desviar esto al RPC del agente que
    // se quiere sustituir no tendría ningún sentido.
    const std::string guion = installScript(plataforma, r.version, api);
    if (!T::runSsh(ses, perfil, H::withSudoStreamInputCommand(perfil, guion), 300000, out, err, rc,
                   traza, traza, {}, contenido, false, verboso)
        || rc != 0) {
        r.fallo = Failure::InstallFailed;
        r.rc = rc;
        r.detail = trim(err.empty() ? out : err);
        return r;
    }

    // Lo que había cacheado del daemon anterior ya no vale: es otro binario, con otro
    // material TLS y detrás de un túnel que apuntaba al que se acaba de matar.
    T::closeTunnelForConnection(ses, perfil);
    T::clearRemoteDaemonTlsCacheForConnection(perfil);
    T::clearLocalDaemonTlsCache();
    return r;
}

std::string uninstallScript(const std::string& plataforma) {
    // Lo contrario de `installScript`, y AL LADO de él a propósito: las rutas y los nombres
    // de servicio salen de los mismos sitios, así que no pueden quedarse desparejados.
    //
    // Se quitan los ficheros que `installScript` pone y nada más. El directorio de
    // configuración solo se borra si queda vacío: si alguien dejó algo propio ahí, no es
    // de esto decidir que sobra.
    const std::string comunes = "rm -f " + shSingleQuote(DP::unixBinPath()) + " "
                                + shSingleQuote(DP::unixConfigPath()) + "; rm -rf "
                                + shSingleQuote(DP::tlsDirPath()) + "; rmdir /etc/zfsmgr 2>/dev/null || true";
    if (plataforma == "macos") {
        return "launchctl bootout system/org.zfsmgr.agent >/dev/null 2>&1 || true; rm -f "
               + shSingleQuote(DP::macPlistPath()) + "; " + comunes;
    }
    if (plataforma == "freebsd") {
        return "service zfsmgr_agent onestop >/dev/null 2>&1 || true; rm -f "
               + shSingleQuote(DP::freeBsdRcPath()) + "; " + comunes;
    }
    return "if command -v systemctl >/dev/null 2>&1; then "
           "systemctl disable --now zfsmgr-agent.service >/dev/null 2>&1 || true; fi; rm -f "
           + shSingleQuote(DP::linuxServicePath()) + "; " + comunes
           + "; if command -v systemctl >/dev/null 2>&1; then systemctl daemon-reload; fi";
}

Result uninstallLocal(const std::string& plataforma, bool simular,
                      const std::function<void(const std::string&)>& traza) {
    Result r;
    r.isMac = (plataforma == "macos");
    if (plataforma == "windows") {
        r.fallo = Failure::InstallFailed;
        r.detail = "windows";
        return r;
    }
    const std::string guion = uninstallScript(plataforma);
    if (simular) {
        if (traza) {
            traza(guion);
        }
        return r;
    }
    const ExecResult ej = runExecCapture("sh", {"-c", guion});
    if (ej.rc != 0) {
        r.fallo = Failure::InstallFailed;
        r.rc = ej.rc;
        r.detail = trim(ej.err.empty() ? ej.out : ej.err);
    }
    return r;
}

Result installLocal(const std::string& plataforma, const std::string& rutaBinario,
                    bool simular, const std::function<void(const std::string&)>& traza) {
    Result r;
    r.isMac = (plataforma == "macos");
    if (plataforma == "windows") {
        // En Windows no hay guion de servicio que ejecutar aquí: el agente llega hoy por
        // `install-daemon` desde otra máquina. No se finge que se puede.
        r.fallo = Failure::InstallFailed;
        r.detail = "windows";
        return r;
    }
    const std::string contenido = leeFicheroEntero(rutaBinario);
    if (contenido.empty()) {
        r.fallo = Failure::UnreadableBinary;
        r.detail = rutaBinario;
        return r;
    }
    // La MISMA sonda que se manda por SSH, ejecutada aquí. Antes de tocar nada.
    {
        const ExecResult sonda = runExecCapture("sh", {"-c", openZfsProbeCommand()});
        const helpers::OpenZfsLectura zfs = readOpenZfsProbe(sonda.out + "\n" + sonda.err);
        if (zfs.soporte != helpers::OpenZfsSoporte::Ok) {
            r.fallo = Failure::UnsupportedOpenZfs;
            r.detail = helpers::textoDeSoporteOpenZfs(zfs);
            r.version = zfs.version;
            return r;
        }
        if (traza) {
            traza("OpenZFS " + zfs.version + ": vale");
        }
    }
    r.version = agentversion::versionInBinary(rutaBinario);
    if (r.version.empty()) {
        r.version = agentversion::expected();
    } else {
        r.versionBehind = (r.version != agentversion::expected());
    }
    // EL MISMO guion que reciben las instalaciones remotas: una sola fuente de verdad para
    // dónde va el binario, qué gestor de servicios se usa y qué se comprueba después.
    const std::string guion = installScript(plataforma, r.version, agentversion::expectedApi());
    if (simular) {
        if (traza) {
            traza(guion);
        }
        return r;
    }
    const ExecResult ej = runExecCaptureWithStdin("sh", {"-c", guion}, contenido);
    if (traza) {
        for (const std::string& l : split(ej.out, "\n", true)) {
            traza(l);
        }
    }
    if (ej.rc != 0) {
        r.fallo = Failure::InstallFailed;
        r.rc = ej.rc;
        r.detail = trim(ej.err.empty() ? ej.out : ej.err);
    }
    return r;
}

}  // namespace zfsmgr::base::daemoninstall
