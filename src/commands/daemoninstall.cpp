#include "daemoninstall.h"
#include "i18n.h"

#include "agentversion.h"
#include "daemonpayload.h"
#include "helpers.h"
#include "processes.h"
#include "strutil.h"
#include "transportcmd.h"
#include "transportrpc.h"
#include "transporttunnel.h"

#include <cstdio>
#include <cstring>
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
            return {};
        case Failure::UnreadableBinary:
            return i18n::tr("t_cmd_daemoninstall_binario_daemon_puede_leer", "el binario del daemon no se puede leer o está vacío");
        case Failure::UploadFailed:
            return i18n::tr("t_cmd_daemoninstall_pudo_copiar_binario_maquina", "no se pudo copiar el binario a la máquina");
        case Failure::InstallFailed:
            return i18n::tr("t_cmd_daemoninstall_instalacion_fallo_maquina", "la instalación falló en la máquina");
        case Failure::UnsupportedOpenZfs:
            return i18n::tr("t_cmd_daemoninstall_esa_maquina_tiene_version", "esa máquina no tiene una versión de OpenZFS con la que esto funcione");
    }
    return {};
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

std::string windowsOpenZfsProbeCommand() {
    return "$z = Get-Command zfs -ErrorAction SilentlyContinue; "
           "if ($z) { & $z.Source version 2>&1 | Out-String } "
           "else { '__ZFSMGR_SIN_ZFS__' }";
}

// A PowerShell script on THIS Windows machine, with the transport's prologue. From a .ps1 file
// and not through `cmd /C`: the install script is long enough for the local path of
// wrapRemoteCommand to inline it, and cmd.exe would then take its `>nul` as its own.
ExecResult runLocalPowerShell(const std::string& script) {
    std::error_code ec;
    const std::filesystem::path ruta =
        std::filesystem::temp_directory_path(ec) / "zfsmgr-install-daemon.ps1";
    {
        std::ofstream f(ruta, std::ios::binary | std::ios::trunc);
        // UTF-8 BOM: without it Windows PowerShell 5.1 reads the file as ANSI.
        f << "\xEF\xBB\xBF" << T::windowsPowerShellPrologue() << "\r\n" << script << "\r\n";
        if (!f) {
            ExecResult r;
            r.rc = 1;
            r.err = "cannot write " + ruta.string();
            return r;
        }
    }
    ExecResult r = runExecCapture(
        "powershell.exe",
        {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", ruta.string()});
    std::filesystem::remove(ruta, ec);
    return r;
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
    // On Windows the same probe in PowerShell: the transport already puts the OpenZFS On
    // Windows folders on $env:Path. It used to answer «Ok» without looking —«the check is not
    // written yet»—, and a1278, reinstalled with Windows and no OpenZFS, got an agent that
    // answered without a single pool to see (2026-10-20). `zfs version` there says
    // «zfswin-2.4.1rc14», which leeSoporteOpenZfs reads as 2.4.
    const std::string sonda = T::isWindowsConnection(p) ? windowsOpenZfsProbeCommand() : orden;
    const bool llego =
        T::runSsh(ses, p, sonda, 20000, out, err, rc, {}, {}, {}, {}, false, verboso);
    // ssh's own failure (rc 255) or no answer at all is «could not ask», not «there is no
    // OpenZFS»: the probe prints the marker or a version whenever it runs.
    if (!llego || rc == 255) {
        helpers::OpenZfsLectura r;
        r.soporte = helpers::OpenZfsSoporte::SinRespuesta;
        r.detalle = trim(helpers::oneLine(err));
        return r;
    }
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
    // El `agent.conf` se escribe entero en cada instalación, pero la DIRECCIÓN en la que
    // atiende el daemon no es de la instalación: la pone `peers --listen` para que otras
    // máquinas puedan nivelar contra esta. Se escribía siempre `127.0.0.1`, así que cada
    // actualización dejaba sin red al daemon —y sin nivelación las programaciones que
    // apuntaban a él— sin que nada lo dijera. Visto el 2026-10-12 tras `install-daemon -a`.
    // Se lee la que hubiera ANTES de escribir y se vuelve a poner; con awk, que es igual en
    // Linux, macOS y FreeBSD (sus `sed -i` no lo son).
    const std::string escribeConf =
        "bind_previo=$(sed -n 's/^AGENT_BIND=//p' " + DP::unixConfigPath()
        + " 2>/dev/null | tail -n1); "
        + "cat > " + DP::unixConfigPath() + " <<'EOF_AGENT_CONF'\n" + conf + "\nEOF_AGENT_CONF\n"
        + "if [ -n \"$bind_previo\" ]; then "
          "awk -v b=\"$bind_previo\" '/^AGENT_BIND=/{print \"AGENT_BIND=\" b; next} {print}' "
        + DP::unixConfigPath() + " > " + DP::unixConfigPath() + ".tmp && mv "
        + DP::unixConfigPath() + ".tmp " + DP::unixConfigPath() + "; fi\n";
    const std::string tls = DP::tlsBootstrapShellCommand();
    const std::string bin = DP::unixBinPath();
    const std::string confPath = DP::unixConfigPath();
    const std::string tlsFiles = DP::tlsDirPath() + " " + DP::tlsServerCertPath() + " "
                                 + DP::tlsServerKeyPath() + " " + DP::tlsClientCertPath() + " "
                                 + DP::tlsClientKeyPath();

    if (plataforma == "macos") {
        return "mkdir -p /usr/local/libexec /etc/zfsmgr; " + despliegue
               + escribeConf
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
                 "  echo \"launchd did not leave the agent running after installing; bootstrap said: $err\" >&2; "
                 "  exit 1; fi";
    }
    if (plataforma == "freebsd") {
        return "mkdir -p /usr/local/libexec /etc/zfsmgr /usr/local/etc/rc.d; " + despliegue
               // Sin OpenSSL el daemon se instala y no arranca, y el motivo real queda en
               // un error del cargador que no dice qué falta.
               + "ldd_missing=$(ldd " + bin + " 2>&1 | grep 'not found' || true); "
                 "if [ -n \"$ldd_missing\" ]; then "
                 "  printf 'ERROR: the daemon has unresolved dependencies:\\n%s\\n' \"$ldd_missing\" >&2; "
                 "  printf 'Install OpenSSL with: pkg install openssl\\n' >&2; exit 1; fi; "
               + escribeConf
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
                 "  printf 'ERROR: the daemon does not stay active after starting\\n' >&2; exit 1; fi";
    }
    return "if ! command -v systemctl >/dev/null 2>&1; then echo 'systemd not available' >&2; "
           "exit 1; fi; mkdir -p /usr/local/libexec /etc/zfsmgr; "
           + despliegue
           + escribeConf
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
            traza(format(i18n::tr("t_cmd_daemoninstall_zfs_vale", "OpenZFS %1 en la otra máquina: vale"),
                         {zfs.version}));
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
        if (traza && out.find("ZFSMGR_WIN_AGENT_POWER_WARN") != std::string::npos) {
            traza(i18n::tr("t_win_agente_bateria",
                           "aviso: no se pudo quitar a la tarea del daemon la parada al pasar "
                           "a batería; en un portátil, Windows lo detendrá al desenchufarlo"));
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

std::string cliProbeCommand() {
    // `command -v` primero, por si se instaló con otro prefijo; y las dos rutas de siempre
    // detrás, porque una sesión SSH no interactiva —macOS sobre todo— no lleva /usr/local/bin
    // en el PATH. Sin sudo: solo se mira.
    return "for p in \"$(command -v zfsmgr-cli 2>/dev/null)\" /usr/local/bin/zfsmgr-cli "
           "/usr/bin/zfsmgr-cli; do "
           "if [ -n \"$p\" ] && [ -e \"$p\" ]; then "
           "if [ -L \"$p\" ]; then echo \"ZFSMGR_CLI_LINK=$p\"; else echo \"ZFSMGR_CLI=$p\"; fi; "
           "echo \"ZFSMGR_CLI_VER=$(\"$p\" version 2>/dev/null | head -1)\"; break; fi; done; true";
}

CliProbe readCliProbe(const std::string& salida) {
    CliProbe r;
    for (const std::string& linea : split(salida, "\n", true)) {
        const std::string l = trim(linea);
        if (startsWith(l, "ZFSMGR_CLI_LINK=")) {
            r.found = true;
            r.isLink = true;
            r.path = l.substr(16);
        } else if (startsWith(l, "ZFSMGR_CLI=")) {
            r.found = true;
            r.path = l.substr(11);
        } else if (startsWith(l, "ZFSMGR_CLI_VER=")) {
            // «zfsmgr-cli 0.99.3 (0.99.3.829775496)»: la huella entre paréntesis. Uno
            // anterior a decirla solo trae «zfsmgr-cli 0.99.3», y se queda vacía: no se sabe
            // qué compilación es, y eso se trata como «a actualizar».
            const std::size_t a = l.find('(');
            const std::size_t b = l.find(')', a == std::string::npos ? 0 : a);
            if (a != std::string::npos && b != std::string::npos && b > a + 1) {
                r.fullVersion = trim(l.substr(a + 1, b - a - 1));
            }
        }
    }
    return r;
}

CliProbe probeRemoteCli(TransportSession& ses, const ConnectionProfile& p, bool verboso) {
    if (T::isWindowsConnection(p)) {
        return {};  // en Windows no se toca: el CLI llega con su instalador
    }
    std::string out;
    std::string err;
    int rc = -1;
    T::runSsh(ses, p, cliProbeCommand(), 20000, out, err, rc, {}, {}, {}, {}, false, verboso);
    return readCliProbe(out);
}

std::string tarArchive(const std::vector<std::pair<std::string, std::string>>& entradas,
                       std::string& error) {
    error.clear();
    std::string tar;
    for (const auto& e : entradas) {
        const std::string& nombre = e.first;
        if (nombre.empty() || nombre.size() > 99) {
            error = "name that does not fit in the tar: " + nombre;
            return {};
        }
        const std::string datos = leeFicheroEntero(e.second);
        std::error_code ec;
        if (datos.empty() && std::filesystem::file_size(e.second, ec) != 0) {
            error = "could not read " + e.second;
            return {};
        }
        if (ec) {
            error = "could not read " + e.second;
            return {};
        }
        // La cabecera ustar: 512 bytes, números en octal y terminados en NUL.
        char cab[512] = {0};
        const auto octal = [&](std::size_t pos, std::size_t largo, unsigned long long v) {
            std::snprintf(cab + pos, largo, "%0*llo", static_cast<int>(largo - 1), v);
        };
        std::memcpy(cab, nombre.data(), nombre.size());
        octal(100, 8, 0755);
        octal(108, 8, 0);
        octal(116, 8, 0);
        octal(124, 12, datos.size());
        octal(136, 12, 0);
        std::memset(cab + 148, ' ', 8);   // la suma se calcula con su propio campo en blanco
        cab[156] = '0';                   // fichero corriente
        std::memcpy(cab + 257, "ustar", 6);
        std::memcpy(cab + 263, "00", 2);
        unsigned int suma = 0;
        for (unsigned char ch : cab) {
            suma += ch;
        }
        std::snprintf(cab + 148, 8, "%06o", suma);
        cab[155] = ' ';
        tar.append(cab, sizeof(cab));
        tar += datos;
        tar.append((512 - datos.size() % 512) % 512, '\0');
    }
    tar.append(1024, '\0');   // fin del archivo: dos bloques vacíos
    return tar;
}

std::string cliInstallScript(const std::string& binPath) {
    const std::string bin = shSingleQuote(binPath);
    return "set -e; d=$(mktemp -d /tmp/zfsmgr-cli.XXXXXX); trap 'rm -rf \"$d\"' EXIT; "
           "tar -xf - -C \"$d\"; "
           "bin=" + bin + "; share=\"$(dirname \"$(dirname \"$bin\")\")/share/zfsmgr\"; "
           // Por renombrado: el fichero viejo sigue existiendo para quien lo tenga abierto.
           "install -m 0755 \"$d/bin/zfsmgr-cli\" \"$bin.nuevo\"; mv -f \"$bin.nuevo\" \"$bin\"; "
           "mkdir -p \"$share/i18n\" \"$share/agents\"; "
           "for j in \"$d\"/i18n/*.json; do [ -f \"$j\" ] && cp -f \"$j\" \"$share/i18n/\"; done; "
           "for a in \"$d\"/agents/*/; do [ -d \"$a\" ] || continue; k=$(basename \"$a\"); "
           "mkdir -p \"$share/agents/$k\"; cp -f \"$a\"* \"$share/agents/$k/\"; "
           "chmod 0755 \"$share/agents/$k\"/*; done; "
           "for c in \"$d\"/cli/*/; do [ -d \"$c\" ] || continue; k=$(basename \"$c\"); "
           "mkdir -p \"$share/cli/$k\"; cp -f \"$c\"* \"$share/cli/$k/\"; "
           "chmod 0755 \"$share/cli/$k\"/*; done; "
           "\"$bin\" version";
}

Result installCli(TransportSession& ses, const ConnectionProfile& perfil,
                  const std::string& cliBinary, const std::string& i18nDir,
                  const std::string& agentsDir,
                  const std::vector<std::pair<std::string, std::string>>& todosLosCli,
                  const std::string& binPath,
                  const std::function<void(const std::string&)>& traza, bool verboso) {
    Result r;
    std::vector<std::pair<std::string, std::string>> kit;
    kit.emplace_back("bin/zfsmgr-cli", cliBinary);
    std::error_code ec;
    for (const auto& j : std::filesystem::directory_iterator(i18nDir, ec)) {
        if (j.path().extension() == ".json") {
            kit.emplace_back("i18n/" + j.path().filename().string(), j.path().string());
        }
    }
    for (const auto& d : std::filesystem::directory_iterator(agentsDir, ec)) {
        if (!d.is_directory()) {
            continue;
        }
        for (const auto& a : std::filesystem::directory_iterator(d.path(), ec)) {
            if (a.is_regular_file()) {
                kit.emplace_back("agents/" + d.path().filename().string() + "/"
                                     + a.path().filename().string(),
                                 a.path().string());
            }
        }
    }
    for (const auto& c : todosLosCli) {
        const std::string ext = std::filesystem::path(c.second).extension().string();
        kit.emplace_back("cli/" + c.first + "/zfsmgr-cli" + ext, c.second);
    }
    std::string err;
    const std::string tar = tarArchive(kit, err);
    if (tar.empty()) {
        r.fallo = Failure::UnreadableBinary;
        r.detail = err;
        return r;
    }
    r.version = agentversion::versionInBinary(cliBinary);
    r.versionBehind = !r.version.empty() && r.version != agentversion::expected();
    std::string out;
    std::string errSsh;
    int rc = -1;
    if (!T::runSsh(ses, perfil, H::withSudoStreamInputCommand(perfil, cliInstallScript(binPath)),
                   300000, out, errSsh, rc, traza, traza, {}, tar, false, verboso)
        || rc != 0) {
        r.fallo = Failure::InstallFailed;
        r.rc = rc;
        r.detail = trim(errSsh.empty() ? out : errSsh);
        return r;
    }
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
        if (simular) {
            if (traza) {
                traza(DP::windowsUninstallCommand());
            }
            return r;
        }
        const ExecResult ej = runLocalPowerShell(DP::windowsUninstallCommand());
        if (ej.rc != 0 || ej.out.find("ZFSMGR_WIN_AGENT_OK") == std::string::npos) {
            r.fallo = Failure::InstallFailed;
            r.rc = ej.rc;
            r.detail = trim(ej.err.empty() ? ej.out : ej.err);
        }
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
    const bool windows = (plataforma == "windows");
    const std::string contenido = leeFicheroEntero(rutaBinario);
    if (contenido.empty()) {
        r.fallo = Failure::UnreadableBinary;
        r.detail = rutaBinario;
        return r;
    }
    // La MISMA sonda que se manda por SSH, ejecutada aquí. Antes de tocar nada.
    {
        const ExecResult sonda = windows ? runLocalPowerShell(windowsOpenZfsProbeCommand())
                                         : runExecCapture("sh", {"-c", openZfsProbeCommand()});
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
    if (windows) {
        // The same script `install` runs on a Windows machine, after putting the binary where
        // it expects it (the running agent may hold the final path).
        if (simular) {
            if (traza) {
                traza(DP::windowsNativeInstallCommand());
            }
            return r;
        }
        std::error_code ec;
        std::filesystem::remove(DP::windowsUploadPath(), ec);
        std::filesystem::copy_file(rutaBinario, DP::windowsUploadPath(), ec);
        if (ec) {
            r.fallo = Failure::UploadFailed;
            r.detail = DP::windowsUploadPath() + ": " + ec.message();
            return r;
        }
        const ExecResult ej = runLocalPowerShell(DP::windowsNativeInstallCommand());
        if (ej.rc != 0 || ej.out.find("ZFSMGR_WIN_AGENT_OK") == std::string::npos) {
            r.fallo = Failure::InstallFailed;
            r.rc = ej.rc;
            r.detail = trim(ej.err.empty() ? ej.out : ej.err);
            return r;
        }
        if (traza && ej.out.find("ZFSMGR_WIN_AGENT_POWER_WARN") != std::string::npos) {
            traza(i18n::tr("t_win_agente_bateria",
                           "aviso: no se pudo quitar a la tarea del daemon la parada al pasar "
                           "a batería; en un portátil, Windows lo detendrá al desenchufarlo"));
        }
        return r;
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
