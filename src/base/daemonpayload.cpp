#include "daemonpayload.h"

#include "strutil.h"

namespace zfsmgr::base::daemonpayload {

std::string unixBinPath() { return std::string("/usr/local/libexec/zfsmgr-agent"); }
std::string unixConfigPath() { return std::string("/etc/zfsmgr/agent.conf"); }
std::string macPlistPath() { return std::string("/Library/LaunchDaemons/org.zfsmgr.agent.plist"); }
std::string linuxServicePath() { return std::string("/etc/systemd/system/zfsmgr-agent.service"); }
std::string freeBsdRcPath() { return std::string("/usr/local/etc/rc.d/zfsmgr_agent"); }
std::string windowsDirPath() { return std::string("C:\\ProgramData\\ZFSMgr\\agent"); }
std::string windowsTaskName() { return std::string("ZFSMgr-Agent"); }
std::string windowsBinPath() { return std::string("C:\\ProgramData\\ZFSMgr\\agent\\zfsmgr-agent.exe"); }
// Ruta intermedia del scp: el destino final puede estar en uso por el agente en
// marcha, así que se sube aparte y se mueve tras pararlo.
std::string windowsUploadPath() { return std::string("C:/Users/Public/zfsmgr-agent.upload"); }
std::string tlsDirPath() { return std::string("/etc/zfsmgr/tls"); }
std::string tlsServerCertPath() { return std::string("/etc/zfsmgr/tls/server.crt"); }
std::string tlsServerKeyPath() { return std::string("/etc/zfsmgr/tls/server.key"); }
std::string tlsClientCertPath() { return std::string("/etc/zfsmgr/tls/client.crt"); }
std::string tlsClientKeyPath() { return std::string("/etc/zfsmgr/tls/client.key"); }
std::string defaultAgentPath() {
    return std::string("/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin");
}

// Aquí vivía `unixStubScript()`: 541 líneas de guion `sh` que implementaban un agente
// entero —44 verbos, con cargas de Python incrustadas para las mutaciones—.
//
// **No lo ejecutaba nadie.** Su único llamante era una prueba. Fue el puente mientras el
// agente nativo no existía en todas las plataformas; cuando dejó de hacer falta, se quedó.
// Y no era inofensivo: era la referencia visible de «así se hacen las cosas por shell»,
// justo lo contrario de lo que hace el resto del árbol.


// Instalación del daemon NATIVO en Windows, en sustitución del stub de PowerShell.
//
// El binario llega en base64 por la entrada estándar: mandarlo en la propia línea de
// comandos no cabe (son ~12 MB codificados) y enviarlo en crudo por stdin se topa
// con la conversión de codificación de PowerShell. Se enlaza estáticamente, así que
// no hay que llevar ninguna DLL al lado.
//
// Después se ejecuta --ensure-tls, que genera el material TLS con el OpenSSL que el
// propio agente enlaza: en Windows no hay openssl en el PATH (solo aparece si está
// Git instalado, que no es garantía), así que el bootstrap por shell no sirve aquí.
std::string windowsNativeInstallCommand() {
    // El binario ya está en $tmpUpload, subido por scp ANTES de ejecutar esto.
    //
    // Antes viajaba en base64 por la entrada estándar, y no funcionaba: PowerShell no
    // devuelve de [Console]::In.ReadToEnd() con volúmenes de megabytes, así que la
    // instalación se colgaba hasta agotar el plazo. Falla ya alrededor de 1 MB y el
    // agente son 9,3 MB.
    //
    // Después se ejecuta --ensure-tls, que genera el material TLS con el OpenSSL que
    // el propio agente enlaza: en Windows no hay openssl en el PATH (solo aparece si
    // está Git instalado, que no es garantía).
    return format(std::string(
        // SIN $ErrorActionPreference='Stop': con él, cualquier salida por stderr de un
        // comando nativo se convierte en excepción, y schtasks /End y /Delete escriben
        // ahí cuando la tarea todavía no existe —que es el caso normal en la primera
        // instalación—. Se comprueban explícitamente los pasos que sí importan.
        "$dir='%1'; $bin='%2'; $task='%3'; $up='%4'; "
        "if (-not (Test-Path -LiteralPath $up)) { Write-Error 'no llegó el binario'; exit 1 } "
        "New-Item -ItemType Directory -Force -Path $dir | Out-Null; "
        "cmd /c \"schtasks /End /TN $task >nul 2>&1\" | Out-Null; "
        "cmd /c \"schtasks /Delete /F /TN $task >nul 2>&1\" | Out-Null; "
        "Get-Process zfsmgr-agent,zfsmgr_agent -ErrorAction SilentlyContinue | Stop-Process -Force; "
        "Start-Sleep -Milliseconds 500; "
        "Move-Item -Force -LiteralPath $up -Destination $bin; "
        "if (-not (Test-Path -LiteralPath $bin)) { Write-Output 'ZFSMGR_WIN_AGENT_MOVE_FAIL'; exit 1 } "
        "& $bin --ensure-tls | Out-Null; "
        "if ($LASTEXITCODE -ne 0) { Write-Output 'ZFSMGR_WIN_AGENT_TLS_FAIL'; exit 1 } "
        // [char]34 en vez de comillas escapadas: este texto atraviesa el literal de
        // C++, el envoltorio de shell y el -Command de PowerShell, y en cada capa se
        // reinterpretan los escapes. Así no depende de ninguna.
        // Regla de cortafuegos para el propio binario.
        //
        // Hace falta para RECIBIR una transferencia: el agente abre un puerto efímero y
        // el emisor se conecta a él. Sin regla, Windows rechaza esa conexión y la copia
        // expira sin explicación —comprobado: el cortafuegos viene activo en los tres
        // perfiles y sin ninguna regla nuestra—.
        //
        // Va acotada al PROGRAMA, no a un puerto: los puertos son efímeros y distintos
        // en cada transferencia, así que abrir un rango sería peor y menos preciso.
        // Se borra y se recrea para que apunte al binario actual si cambió de sitio, y
        // no interrumpe la instalación si falla: sin ella el agente sirve igual para
        // todo lo demás.
        "Remove-NetFirewallRule -Name 'ZFSMgr-Agent' -ErrorAction SilentlyContinue; "
        "New-NetFirewallRule -Name 'ZFSMgr-Agent' -DisplayName 'ZFSMgr Agent (transferencias ZFS)' "
        "-Direction Inbound -Action Allow -Program $bin -Profile Any "
        "-ErrorAction SilentlyContinue | Out-Null; "
        "$q=[char]34; $action=$q + $bin + $q + ' --serve'; "
        "schtasks /Create /SC ONSTART /RL HIGHEST /RU SYSTEM /TN $task /TR $action /F | Out-Null; "
        "schtasks /Run /TN $task | Out-Null; "
        "Write-Output 'ZFSMGR_WIN_AGENT_OK'; exit 0"),
        {windowsDirPath(), windowsBinPath(), windowsTaskName(), windowsUploadPath()});
}

std::string macLaunchdPlist() {
    return std::string(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key><string>org.zfsmgr.agent</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>/usr/local/libexec/zfsmgr-agent</string>\n"
        "    <string>--serve</string>\n"
        "  </array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>KeepAlive</key><true/>\n"
        "</dict>\n"
        "</plist>\n");
}

std::string freeBsdRcScript() {
    return std::string(
        "#!/bin/sh\n"
        "# PROVIDE: zfsmgr_agent\n"
        "# REQUIRE: LOGIN\n"
        "# KEYWORD: shutdown\n"
        ". /etc/rc.subr\n"
        "name=\"zfsmgr_agent\"\n"
        "rcvar=zfsmgr_agent_enable\n"
        "pidfile=\"/var/run/${name}.pid\"\n"
        "command=\"/usr/sbin/daemon\"\n"
        "command_args=\"-P ${pidfile} /usr/local/libexec/zfsmgr-agent --serve\"\n"
        "load_rc_config $name\n"
        ": ${zfsmgr_agent_enable:=YES}\n"
        "run_rc_command \"$1\"\n");
}

std::string linuxSystemdService() {
    return std::string(
        "[Unit]\n"
        "Description=ZFSMgr native daemon\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=/usr/local/libexec/zfsmgr-agent --serve\n"
        "Restart=always\n"
        "RestartSec=5\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n");
}

std::string simpleConfigPayload(const std::string& version, const std::string& apiVersion) {
    return format(std::string(
               "VERSION=%1\n"
               "API=%2\n"
               "AGENT_BIND=%3\n"
               "AGENT_PORT=%4\n"
               "AGENT_PATH=%5\n"
               "CACHE_TTL_FAST_MS=%6\n"
               "CACHE_TTL_SLOW_MS=%7\n"
               "CACHE_MAX_ENTRIES=%8\n"
               "RECONCILE_INTERVAL_MS=%9\n"
               "ZED_EVENTS_ENABLED=%10\n"
               "TLS_DIR=%11\n"
               "TLS_CERT=%12\n"
               "TLS_KEY=%13\n"),
        {shSingleQuote(trim(version)),
         shSingleQuote(trim(apiVersion)),
         shSingleQuote(std::string("127.0.0.1")),
         shSingleQuote(std::string("47653")),
         shSingleQuote(defaultAgentPath()),
         shSingleQuote(std::string("2000")),
         shSingleQuote(std::string("8000")),
         shSingleQuote(std::string("512")),
         shSingleQuote(std::string("60000")),
         shSingleQuote(std::string("1")),
         shSingleQuote(tlsDirPath()),
         shSingleQuote(tlsServerCertPath()),
         shSingleQuote(tlsServerKeyPath())})
        + format(std::string("TLS_CLIENT_CERT=%1\nTLS_CLIENT_KEY=%2\n"),
                 {shSingleQuote(tlsClientCertPath()),
                  shSingleQuote(tlsClientKeyPath())});
}

std::string tlsBootstrapShellCommand() {
    // Los certificados DEBEN llevar subjectAltName. Emitirlos solo con CN funciona
    // con el backend OpenSSL de Qt, que aún acepta el CN como nombre de host, pero
    // no con el SecureTransport de Apple, que sigue el RFC 6125 y lo ignora por
    // completo. Con certificados sin SAN, la aplicación de macOS no podía hablar
    // con NINGÚN daemon: "The host name did not match any of the valid hosts for
    // this certificate". Se incluyen los dos nombres que prueba el cliente y la
    // IP de loopback, que es a donde apunta el túnel SSH.
    //
    // La condición de regeneración comprueba además que el certificado existente
    // TENGA SAN: si no, se rehace. Sin eso, los hosts ya aprovisionados se
    // quedarían con el certificado viejo para siempre, porque el fichero existe y
    // no está vacío.
    // Con SAN el handshake dejó de fallar por nombre de host y pasó a fallar por
    // "The root CA certificate is not trusted for this purpose", que es el
    // InvalidPurpose de Qt: al certificado le faltaba extendedKeyUsage. Cada uno
    // hace de ancla y de hoja a la vez, así que el del servidor declara serverAuth
    // y el del cliente clientAuth. keyUsage lleva keyCertSign porque ambos son
    // CA:TRUE de sí mismos, más lo que TLS necesita de la hoja.
    const std::string san = std::string(
        "subjectAltName=DNS:zfsmgr-agent-server,DNS:zfsmgr-agent,DNS:localhost,IP:127.0.0.1");
    const std::string keyUse = std::string(
        "keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign");
    return format(std::string(
        "mkdir -p %1; "
        "_zfsmgr_needs_san() { "
        "  [ -s \"$1\" ] || return 0; "
        "  openssl x509 -in \"$1\" -noout -ext subjectAltName 2>/dev/null | grep -q 'DNS:' || return 0; "
        "  return 1; "
        "}; "
        "if [ ! -s %2 ] || [ ! -s %3 ] || _zfsmgr_needs_san %2; then "
        "  if command -v openssl >/dev/null 2>&1; then "
        "    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 "
        "      -subj '/CN=zfsmgr-agent-server' -addext '%6' -addext '%7' "
        "      -addext 'extendedKeyUsage=serverAuth' "
        "      -keyout %3 -out %2 >/dev/null 2>&1 || true; "
        "  fi; "
        "fi; "
        "if [ ! -s %4 ] || [ ! -s %5 ] || _zfsmgr_needs_san %4; then "
        "  if command -v openssl >/dev/null 2>&1; then "
        "    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 "
        "      -subj '/CN=zfsmgr-agent-client' -addext '%6' -addext '%7' "
        "      -addext 'extendedKeyUsage=clientAuth' "
        "      -keyout %5 -out %4 >/dev/null 2>&1 || true; "
        "  fi; "
        "fi; "
        "touch %2 %3 %4 %5; "
        "chmod 600 %2 %3 %4 %5"),
        {tlsDirPath(),
         tlsServerCertPath(),
         tlsServerKeyPath(),
         tlsClientCertPath(),
         tlsClientKeyPath(),
         san,
         keyUse});
}

}  // namespace zfsmgr::base::daemonpayload
