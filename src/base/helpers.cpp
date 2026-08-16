#include "helpers.h"

#include "daemonpayload.h"
#include "strutil.h"

#include <cctype>
#include <cstdio>
#include <map>
#include <regex>
#include <filesystem>
#include <system_error>

// Portado a mano desde `mwhelpers`. Los literales llevan dentro código de PowerShell y
// de shell POSIX, con paréntesis y con `.Trim()`/`.ToLower()`: cualquier traducción
// automática de métodos de QString tiene que protegerlos o los rompe en silencio.
namespace zfsmgr::base::helpers {

using zfsmgr::base::contains;
using zfsmgr::base::format;
using zfsmgr::base::indexOf;
using zfsmgr::base::left;
using zfsmgr::base::mid;
using zfsmgr::base::replaceAll;
using zfsmgr::base::shSingleQuote;
using zfsmgr::base::simplify;
using zfsmgr::base::toLowerAscii;
using zfsmgr::base::isLetterAt;
using zfsmgr::base::join;
using zfsmgr::base::split;
using zfsmgr::base::startsWith;
using zfsmgr::base::toUpperUtf8;
using zfsmgr::base::toLowerUtf8;
using zfsmgr::base::trim;
namespace daemonpayload = zfsmgr::base::daemonpayload;

std::string oneLine(const std::string& v, int maxLen) {
    std::string x = simplify(v);
    return left(x, static_cast<std::size_t>(maxLen < 0 ? 0 : maxLen));
}

bool isMountedValueTrue(const std::string& value) {
    const std::string v = toLowerAscii(trim(value));
    return v == ("yes")
        || v == ("on")
        || v == ("true")
        || v == ("1");
}

std::string parentDatasetName(const std::string& dataset) {
    const long long slash = lastIndexOf(dataset, "/");
    if (slash <= 0) {
        return std::string();
    }
    return left(dataset, static_cast<std::size_t>(slash));
}

bool isWindowsOsType(const std::string& osType) {
    return contains(toLowerAscii(trim(osType)), ("windows"));
}

bool parentMountCheckRequired(const std::string& parentMountpoint, const std::string& parentCanmount) {
    const std::string mp = toLowerAscii(trim(parentMountpoint));
    const std::string canmount = toLowerAscii(trim(parentCanmount));
    if (mp.empty() || mp == ("none")) {
        return false;
    }
    if (canmount == ("off")) {
        return false;
    }
    return true;
}

bool parentAllowsChildMount(const std::string& parentMountpoint, const std::string& parentCanmount, const std::string& parentMounted) {
    if (!parentMountCheckRequired(parentMountpoint, parentCanmount)) {
        return true;
    }
    return isMountedValueTrue(parentMounted);
}

std::string buildHasMountedChildrenCommand(bool isWindows, const std::string& datasetName) {
    if (isWindows) {
        std::string dsPs = datasetName;
        replaceAll(dsPs, "\'", ("''"));
        return format("$ds='%1'; "
                   "$has=$false; "
                   "$children=@(zfs list -H -o name -r $ds 2>$null); "
                   "if ($LASTEXITCODE -ne 0) { exit 2 }; "
                   "foreach ($c in $children) { "
                   "  if ([string]::IsNullOrWhiteSpace($c) -or $c -eq $ds) { continue }; "
                   "  $m=(zfs get -H -o value mounted $c 2>$null | Out-String).Trim().ToLower(); "
                   "  if ($m -eq 'yes' -or $m -eq 'on' -or $m -eq 'true' -or $m -eq '1') { $has=$true; break } "
                   "}; "
                   "if ($has) { exit 0 } else { exit 1 }", {dsPs});
    }
    return format("DATASET=%1; zfs mount | "
               "awk -v ds=\"$DATASET\" '$1!=ds && index($1, ds \"/\")==1 { found=1; exit } END { exit found ? 0 : 1 }'", {shSingleQuote(datasetName)});
}

std::string buildRecursiveUmountCommand(bool isWindows, const std::string& datasetName) {
    if (isWindows) {
        std::string dsPs = datasetName;
        replaceAll(dsPs, "\'", ("''"));
        return format("$ds='%1'; "
                   "$list=@(zfs list -H -o name -r $ds 2>$null); "
                   "if ($LASTEXITCODE -ne 0) { throw 'zfs list failed' }; "
                   "$sorted = $list | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object { $_.Length } -Descending; "
                   "foreach ($d in $sorted) { zfs unmount $d 2>$null | Out-Null }", {dsPs});
    }
    return format("set -e; DATASET=%1; zfs mount | "
               "awk -v ds=\"$DATASET\" '$1==ds || index($1, ds \"/\")==1 { print $1 }' | "
               "awk '{print length, $0}' | sort -rn | cut -d' ' -f2- | "
               "while IFS= read -r ds; do [ -n \"$ds\" ] && zfs umount \"$ds\"; done", {shSingleQuote(datasetName)});
}

std::string buildSingleUmountCommand(bool isWindows, const std::string& datasetName) {
    const std::string dsQ = shSingleQuote(datasetName);
    return isWindows ? format("zfs unmount %1", {dsQ})
                     : format("zfs umount %1", {dsQ});
}

std::string buildSingleMountCommand(const std::string& datasetName) {
    return format("zfs mount %1", {shSingleQuote(datasetName)});
}

std::string buildMountChildrenCommand(bool isWindows, const std::string& datasetName) {
    if (isWindows) {
        std::string dsPs = datasetName;
        replaceAll(dsPs, "\'", ("''"));
        return format("$ds='%1'; "
                   "$items = @(zfs list -H -o name -r $ds 2>$null); "
                   "if ($LASTEXITCODE -ne 0) { throw 'zfs list failed' }; "
                   "foreach ($child in $items) { "
                   "  if ([string]::IsNullOrWhiteSpace($child)) { continue }; "
                   "  $m = (zfs get -H -o value mounted $child 2>$null | Out-String).Trim().ToLower(); "
                   "  if ($m -ne 'yes' -and $m -ne 'on' -and $m -ne 'true' -and $m -ne '1') { "
                   "    zfs mount $child 2>$null | Out-Null "
                   "  } "
                   "}", {dsPs});
    }
    return format("set -e; DATASET=%1; "
               "zfs list -H -o name -r \"$DATASET\" | "
               "while IFS= read -r child; do "
               "  [ -n \"$child\" ] || continue; "
               "  mounted=$(zfs get -H -o value mounted \"$child\" 2>/dev/null || true); "
               "  case \"$mounted\" in yes|on|true|1) : ;; *) zfs mount \"$child\" ;; esac; "
               "done", {shSingleQuote(datasetName)});
}

std::string buildWindowsMountPrecheckCommand(const std::string& datasetName, const std::string& effectiveMountpoint) {
    std::string dsPs = datasetName;
    replaceAll(dsPs, "\'", ("''"));
    std::string mpPs = trim(effectiveMountpoint);
    replaceAll(mpPs, "\'", ("''"));
    return format("$ds='%1'; "
               "$mp='%2'; "
               "if ([string]::IsNullOrWhiteSpace($mp) -or $mp -eq '-' -or $mp -eq 'none') { "
               "  throw ('mountpoint efectivo no resuelto para ' + $ds) "
               "}; "
               "$exists = Test-Path -LiteralPath $mp; "
               "$mapped = $false; "
               "foreach ($line in @(zfs mount 2>$null)) { "
               "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
               "    $d = $Matches[1].Trim(); "
               "    $m = $Matches[2].Trim(); "
               "    if ([string]::Equals($m, $mp, [System.StringComparison]::OrdinalIgnoreCase)) { "
               "      if ($d -eq $ds) { $mapped = $true }; "
               "      break; "
               "    } "
               "  } "
               "}; "
               "if ($exists -and -not $mapped) { "
               "  throw ('mountpoint ocupado por ruta existente no-ZFS: ' + $mp) "
               "}", {dsPs, mpPs});
}

std::string streamProgressPipeFilter() {
    return ("((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat)");
}

std::string buildPipedTransferCommand(const std::string& sendSegment, const std::string& recvSegment) {
    return sendSegment
        + (" | ")
        + streamProgressPipeFilter()
        + (" | ")
        + recvSegment;
}

std::string streamCodecName(StreamCodec codec) {
    switch (codec) {
        case StreamCodec::Zstd:
            return ("zstd-fast");
        case StreamCodec::Gzip:
            return ("gzip-fast");
        case StreamCodec::None:
        default:
            return ("none");
    }
}

StreamCodec chooseStreamCodec(bool hasZstdBoth, bool hasGzipBoth) {
    if (hasZstdBoth) {
        return StreamCodec::Zstd;
    }
    if (hasGzipBoth) {
        return StreamCodec::Gzip;
    }
    return StreamCodec::None;
}

std::string buildTarSourceCommand(bool isWindows, const std::string& mountPath, StreamCodec codec) {
    switch (codec) {
        case StreamCodec::Zstd:
            return isWindows
                       ? format("$p=%1; tar -cf - -C $p . | zstd -1 -T0 -q -c", {shSingleQuote(mountPath)})
                       : format("tar --acls --xattrs -cpf - -C %1 . | zstd -1 -T0 -q -c", {shSingleQuote(mountPath)});
        case StreamCodec::Gzip:
            return isWindows
                       ? format("$p=%1; tar -cf - -C $p . | gzip -1 -c", {shSingleQuote(mountPath)})
                       : format("tar --acls --xattrs -cpf - -C %1 . | gzip -1 -c", {shSingleQuote(mountPath)});
        case StreamCodec::None:
        default:
            return isWindows
                       ? format("$p=%1; tar -cf - -C $p .", {shSingleQuote(mountPath)})
                       : format("tar --acls --xattrs -cpf - -C %1 .", {shSingleQuote(mountPath)});
    }
}

std::string buildTarDestinationCommand(bool isWindows, const std::string& mountPath, StreamCodec codec) {
    const std::string decodePipe =
        (codec == StreamCodec::Zstd) ? ("zstd -d -q -c - | ")
        : (codec == StreamCodec::Gzip) ? ("gzip -d -c - | ")
        : std::string();
    if (isWindows) {
        return format("$ProgressPreference='SilentlyContinue'; $p=%1; if (!(Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null }; %2tar -xpf - -C $p", {shSingleQuote(mountPath), decodePipe});
    }
    return format("mkdir -p %1 && %2tar --acls --xattrs -xpf - -C %1", {shSingleQuote(mountPath), decodePipe});
}

std::string withUnixSearchPathCommand(const std::string& cmd) {
    return format("PATH=\"$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin\"; "
               "export PATH; %1", {cmd});
}

std::string rpcTunnelBusyReason() {
    return ("túnel daemon-rpc en construcción para esta conexión");
}

std::string storedSecretMarkerPrefix() {
    return ("@@ZFSMGR_PW:");
}

std::string stripToJson(const std::string& output) {
    const int idx = indexOf(output, "{");
    return idx >= 0 ? mid(output, idx) : output;
}

std::string shPrintfOctalEscaped(const std::string& s) {
    // Byte a byte, no carácter a carácter: `printf '%b'` interpreta \0ddd como UN byte,
    // así que un carácter multibyte sale como varias secuencias y se reensambla al otro
    // lado tal cual se tecleó.
    std::string out;
    out.reserve(s.size() * 4);
    for (const char rawByte : s) {
        const unsigned b = static_cast<unsigned char>(rawByte);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\0%03o", b);
        out += buf;
    }
    return out;
}

std::string sshControlPath() {
#ifdef __APPLE__
    // Ruta corta a propósito: el socket de dominio Unix tiene un límite de ~104 bytes
    // en la longitud de la ruta, y el directorio temporal de macOS es largo.
    return "/tmp/zfsmgr-%C";
#else
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    const std::string base = ec ? std::string("/tmp") : tmp.string();
    return base + "/zfsmgr-ssh-%C";
#endif
}

std::string sshUserHost(const ConnectionProfile& p) {
    return p.username + "@" + p.host;
}

std::string sshUserHostPort(const ConnectionProfile& p) {
    const std::string port = (p.port > 0) ? std::to_string(p.port) : std::string("22");
    return sshUserHost(p) + ":" + port;
}

std::string sshAddressFamilyOption(const ConnectionProfile& p) {
    const std::string family = toLowerAscii(trim(p.sshAddressFamily));
    if (family == "ipv4") {
        return "-4";
    }
    if (family == "ipv6") {
        return "-6";
    }
    return std::string();
}

std::string sshBaseCommand(const ConnectionProfile& p) {
    // accept-new y SIN UserKnownHostsFile: se usa el ~/.ssh/known_hosts del usuario.
    // Antes iba StrictHostKeyChecking=no con UserKnownHostsFile=/dev/null, que no solo
    // no verifica al host: descarta la memoria, así que cada conexión aceptaba
    // cualquier clave para siempre. Como el material TLS del daemon se trae POR SSH,
    // eso permitía que un intermediario entregara su propio certificado, que la
    // aplicación fijaría tan tranquila.
    // Sin multiplexado cuando la aplicación corre en Windows: su OpenSSH responde
    // `getsockname failed: Not a socket`, y ControlPersist deja un maestro de fondo que
    // no suelta las tuberías heredadas.
#ifdef _WIN32
    std::string cmd = "ssh -o BatchMode=yes -o LogLevel=ERROR"
                      " -o StrictHostKeyChecking=accept-new";
#else
    std::string cmd = format("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=accept-new"
                             " -o ControlMaster=auto -o ControlPersist=yes -o ControlPath=%1",
                             {shSingleQuote(sshControlPath())});
#endif
    const std::string familyOpt = sshAddressFamilyOption(p);
    if (!familyOpt.empty()) {
        cmd += " " + familyOpt;
    }
    if (p.port > 0) {
        cmd += " -p " + std::to_string(p.port);
    }
    if (!p.keyPath.empty()) {
        cmd += " -i " + shSingleQuote(p.keyPath);
    }
    return cmd;
}

std::string buildSshTargetPrefix(const ConnectionProfile& p) {
    return sshBaseCommand(p) + " " + shSingleQuote(sshUserHost(p));
}

std::string buildSimpleSshInvocation(const ConnectionProfile& p, const std::string& remoteCmd) {
    return buildSshTargetPrefix(p) + " " + shSingleQuote(remoteCmd);
}

std::string buildSshPreviewCommandText(const ConnectionProfile& p, const std::string& remoteCmd) {
    std::vector<std::string> parts;
    parts.push_back("ssh");
    const std::string familyOpt = sshAddressFamilyOption(p);
    if (!familyOpt.empty()) {
        parts.push_back(familyOpt);
    }
    parts.push_back("-o BatchMode=yes");
    parts.push_back("-o ConnectTimeout=10");
    parts.push_back("-o LogLevel=ERROR");
    // Ver la nota en sshBaseCommand: se verifica contra ~/.ssh/known_hosts.
    parts.push_back("-o StrictHostKeyChecking=accept-new");
    parts.push_back("-o ControlMaster=auto");
    parts.push_back("-o ControlPersist=yes");
    parts.push_back(format("-o ControlPath=%1", {shSingleQuote(sshControlPath())}));
    if (p.port > 0) {
        parts.push_back(format("-p %1", {std::to_string(p.port)}));
    }
    if (!p.keyPath.empty()) {
        parts.push_back(format("-i %1", {shSingleQuote(p.keyPath)}));
    }
    parts.push_back(sshUserHost(p));
    parts.push_back(shSingleQuote(remoteCmd));
    return join(parts, " ");
}

std::vector<std::string> scpUploadArgs(const ConnectionProfile& p,
                                       const std::string& localPath,
                                       const std::string& remotePath,
                                       bool multiplex) {
    std::vector<std::string> args{"-q",
                                  "-o", "BatchMode=yes",
                                  "-o", "LogLevel=ERROR",
                                  "-o", "StrictHostKeyChecking=accept-new"};
    if (multiplex) {
        args.push_back("-o");
        args.push_back("ControlMaster=auto");
        args.push_back("-o");
        args.push_back("ControlPersist=yes");
        args.push_back("-o");
        args.push_back("ControlPath=" + sshControlPath());
    }
    const std::string familyOpt = trim(sshAddressFamilyOption(p));
    if (!familyOpt.empty()) {
        args.push_back(familyOpt);
    }
    if (p.port > 0) {
        // scp usa -P mayúscula para el puerto, no -p como ssh.
        args.push_back("-P");
        args.push_back(std::to_string(p.port));
    }
    if (!p.keyPath.empty()) {
        args.push_back("-i");
        args.push_back(p.keyPath);
    }
    args.push_back(localPath);
    args.push_back(sshUserHost(p) + ":" + remotePath);
    return args;
}

std::string withSudoCommand(const ConnectionProfile& p, const std::string& cmd) {
    if (isWindowsOsType(p.osType)) {
        return cmd;
    }
    const std::string preparedCmd = withUnixSearchPathCommand(cmd);
    if (!p.useSudo) {
        return preparedCmd;
    }
    if (!p.password.empty()) {
        // %b + escapes octales: la contraseña viaja en ASCII puro. Ver
        // shPrintfOctalEscaped: en macOS, Qt descompone los caracteres al pasar la
        // orden al intérprete y sudo recibía otros bytes.
        return format("printf '%b\\n' '%1' | sudo -k -S -p '' sh -c %2",
                      {shPrintfOctalEscaped(p.password), shSingleQuote(preparedCmd)});
    }
    // `sh -c` con la orden entrecomillada. Concatenar `sudo -n ` delante no valía:
    // withUnixSearchPathCommand antepone `PATH="..."; export PATH; `, y los punto y coma
    // partían la línea en tres, dejando el agente SIN sudo y respondiendo «Permiso
    // denegado» porque el binario es 0700 root. La aplicación concluía entonces que el
    // agente no estaba instalado en una máquina donde sí lo está.
    return format("sudo -n sh -c %1", {shSingleQuote(preparedCmd)});
}

std::string withSudoStreamInputCommand(const ConnectionProfile& p, const std::string& cmd) {
    if (isWindowsOsType(p.osType)) {
        return cmd;
    }
    const std::string preparedCmd = withUnixSearchPathCommand(cmd);
    if (!p.useSudo) {
        return preparedCmd;
    }
    if (!p.password.empty()) {
        return format("{ printf '%b\\n' '%1'; cat; } | sudo -k -S -p '' sh -c %2",
                      {shPrintfOctalEscaped(p.password), shSingleQuote(preparedCmd)});
    }
    return format("sudo -n sh -c %1", {shSingleQuote(preparedCmd)});
}

std::string agentCommand(const ConnectionProfile& p, const std::string& agentArgs) {
    if (isWindowsOsType(p.osType)) {
        // El "&" no es decorativo: en PowerShell una cadena entrecomillada al principio
        // de una sentencia es una expresión, no un comando, así que sin el operador de
        // llamada la ruta se evalúa como texto y el primer argumento revienta el parseo.
        return std::string("& \"") + daemonpayload::windowsBinPath() + "\" " + agentArgs;
    }
    // Sin withUnixSearchPathCommand aquí: withSudoCommand ya lo aplica, y hacerlo dos
    // veces dejaba el prefijo PATH duplicado en la orden y en el diálogo de
    // confirmación, donde no ayuda a decidir nada.
    return withSudoCommand(p, daemonpayload::unixBinPath() + " " + agentArgs);
}

std::string agentShellCommand(const ConnectionProfile& p,
                              const std::vector<std::string>& agentArgs) {
    std::vector<std::string> quoted;
    quoted.reserve(agentArgs.size());
    for (const std::string& a : agentArgs) {
        quoted.push_back(isWindowsOsType(p.osType) ? a : shSingleQuote(a));
    }
    return agentCommand(p, join(quoted, " "));
}

std::string agentShellCommandStreamInput(const ConnectionProfile& p,
                                         const std::vector<std::string>& agentArgs) {
    if (isWindowsOsType(p.osType)) {
        return agentShellCommand(p, agentArgs);
    }
    std::vector<std::string> quoted;
    quoted.reserve(agentArgs.size());
    for (const std::string& a : agentArgs) {
        quoted.push_back(shSingleQuote(a));
    }
    return withSudoStreamInputCommand(
        p, withUnixSearchPathCommand(daemonpayload::unixBinPath() + " " + join(quoted, " ")));
}

namespace {

std::string storedSecretMarker(const std::string& key) {
    return storedSecretMarkerPrefix() + key + "@@";
}

// startsWith sin distinguir caja, solo para prefijos ASCII como «gpt=» o «type=».
bool empiezaPorNoCaseAscii(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && toLowerAscii(s.substr(0, pre.size())) == toLowerAscii(pre);
}

}  // namespace

std::string maskedAgentArgvForLog(const std::vector<std::string>& argv) {
    std::vector<std::string> masked = argv;
    for (std::size_t i = 0; i < masked.size(); ++i) {
        const std::string verb = trim(masked[i]);
        if (verb != "--mutate-zfs-load-key" && verb != "--mutate-zfs-change-key"
            && verb != "--mutate-zfs-create") {
            continue;
        }
        // El secreto es el ÚLTIMO argumento del verbo: para load-key/change-key va tras
        // el dataset, y para create tras el argv de zfs. Se tapa todo lo que siga a ese
        // primer argumento, que nunca es más de uno.
        for (std::size_t j = i + 2; j < masked.size(); ++j) {
            masked[j] = "[secret]";
        }
        break;
    }
    return join(masked, " ");
}

std::string normalizeDriveLetterValue(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty() || s == "-" || toLowerAscii(s) == "none") {
        return std::string();
    }
    replaceAll(s, ":\\", "");
    replaceAll(s, ":", "");
    replaceAll(s, "\\", "");
    replaceAll(s, "/", "");
    // toUpperUtf8 y no la variante ASCII: Qt sube la caja también a las acentuadas, y
    // esta función decide si el primer carácter es una letra.
    s = toUpperUtf8(trim(s));
    if (s.empty() || !isLetterAt(s, 0)) {
        return std::string();
    }
    return left(s, 1);
}

std::string sshHostKeyProblemHint(const std::string& sshStderr) {
    if (contains(sshStderr, "REMOTE HOST IDENTIFICATION HAS CHANGED")
        || contains(sshStderr, "Host key verification failed")) {
        return "La clave del host SSH no coincide con la registrada en ~/.ssh/known_hosts. "
               "Si reinstaló o reemplazó esa máquina, elimine su línea de ese fichero "
               "(ssh-keygen -R <host>) y vuelva a conectar. Si no ha cambiado nada, "
               "no continúe: alguien podría estar suplantando al host.";
    }
    if (contains(sshStderr, "Bad configuration option: stricthostkeychecking")) {
        return "Su cliente SSH es demasiado antiguo para 'accept-new' (necesita OpenSSH 7.6 o superior).";
    }
    return std::string();
}

bool isCliOnlyAgentCommand(const std::string& verb) {
    // Estos cuatro no se sirven por RPC a propósito: transportan flujos por la entrada
    // y la salida estándar, que el canal RPC no lleva.
    const std::string v = trim(verb);
    return v == "--mutate-shell-generic" || v == "--mutate-advanced-fromdir"
        || v == "--mutate-sync-temp-tar-source" || v == "--mutate-sync-temp-tar-dest";
}

std::string windowsGptTypeName(const std::string& guid) {
    std::string g = trim(guid);
    if (g.size() > 2 && g.front() == '{' && g.back() == '}') {
        g = g.substr(1, g.size() - 2);
    }
    g = toLowerAscii(g);  // un GUID es hexadecimal: ASCII basta
    static const std::map<std::string, std::string> kMap = {
        {"00000000-0000-0000-0000-000000000000", "Unused entry"},
        {"024dee41-33e7-11d3-9d69-0008c781f39f", "MBR partition scheme"},
        {"c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "EFI System Partition"},
        {"21686148-6449-6e6f-744e-656564454649", "BIOS Boot Partition"},
        {"b334117e-118d-11de-9b0f-001cc0952d53", "gdisk unknown"},
        {"e3c9e316-0b5c-4db8-817d-f92df00215ae", "Windows/Reserved"},
        {"ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "Windows/Basic Data / Linux/Data"},
        {"5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", "Windows/LDM metadata"},
        {"af9b60a0-1431-4f62-bc68-3311714a69ad", "Windows/LDM data"},
        {"75894c1e-3aeb-11d3-b7c1-7b03a0000000", "HP-UX/Data"},
        {"e2a1e728-32e3-11d6-a682-7b03a0000000", "HP-UX/Service"},
        {"a19d880f-05fc-4d3b-a006-743f0f84911e", "Linux/RAID"},
        {"0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "Linux/Swap"},
        {"e6d6d379-f507-44c2-a23c-238f2a3df928", "Linux/LVM"},
        {"8da63339-0007-60c0-c436-083ac8230908", "Linux/Reserved"},
        {"83bd6b9d-7f41-11dc-be0b-001560b84f0f", "FreeBSD/Boot"},
        {"516e7cb4-6ecf-11d6-8ff8-00022d09712b", "FreeBSD/Data"},
        {"516e7cb5-6ecf-11d6-8ff8-00022d09712b", "FreeBSD/Swap"},
        {"516e7cb6-6ecf-11d6-8ff8-00022d09712b", "FreeBSD/UFS"},
        {"516e7cb8-6ecf-11d6-8ff8-00022d09712b", "FreeBSD/Vinum"},
        {"516e7cba-6ecf-11d6-8ff8-00022d09712b", "FreeBSD/ZFS"},
        {"48465300-0000-11aa-aa11-00306543ecac", "Mac OS X/HFS+"},
        {"55465300-0000-11aa-aa11-00306543ecac", "Mac OS X/Apple UFS"},
        {"6a898cc3-1dd2-11b2-99a6-080020736631", "Mac OS X/ZFS / Solaris/usr"},
        {"52414944-0000-11aa-aa11-00306543ecac", "Mac OS X/RAID"},
        {"52414944-5f4f-11aa-aa11-00306543ecac", "Mac OS X/Offline RAID"},
        {"426f6f74-0000-11aa-aa11-00306543ecac", "Mac OS X/Boot"},
        {"4c616265-6c00-11aa-aa11-00306543ecac", "Mac OS X/Label"},
        {"5265636f-7665-11aa-aa11-00306543ecac", "Mac OS X/Apple TV Recovery"},
        {"6a82cb45-1dd2-11b2-99a6-080020736631", "Solaris/Boot"},
        {"6a85cf4d-1dd2-11b2-99a6-080020736631", "Solaris/Root"},
        {"6a87c46f-1dd2-11b2-99a6-080020736631", "Solaris/Swap"},
        {"6a8b642b-1dd2-11b2-99a6-080020736631", "Solaris/Backup"},
        {"6a8ef2e9-1dd2-11b2-99a6-080020736631", "Solaris/var"},
        {"6a90ba39-1dd2-11b2-99a6-080020736631", "Solaris/home"},
        {"6a9283a5-1dd2-11b2-99a6-080020736631", "Solaris/EFI_ALTSCTR"},
        {"6a945a3b-1dd2-11b2-99a6-080020736631", "Solaris/Reserved"},
        {"6a9630d1-1dd2-11b2-99a6-080020736631", "Solaris/Reserved"},
        {"6a980767-1dd2-11b2-99a6-080020736631", "Solaris/Reserved"},
        {"6a96237f-1dd2-11b2-99a6-080020736631", "Solaris/Reserved"},
        {"6a8d2ac7-1dd2-11b2-99a6-080020736631", "Solaris/Reserved"},
        {"49f48d32-b10e-11dc-b99b-0019d1879648", "NetBSD/Swap"},
        {"49f48d5a-b10e-11dc-b99b-0019d1879648", "NetBSD/FFS"},
        {"49f48d82-b10e-11dc-b99b-0019d1879648", "NetBSD/LFS"},
        {"49f48daa-b10e-11dc-b99b-0019d1879648", "NetBSD/RAID"},
        {"2db519c4-b10f-11dc-b99b-0019d1879648", "NetBSD/concatenated"},
        {"2db519ec-b10f-11dc-b99b-0019d1879648", "NetBSD/encrypted"}
    };
    const auto it = kMap.find(g);
    return it == kMap.end() ? std::string() : it->second;
}

std::string formatWindowsFsTypeDetail(const std::string& rawFsType) {
    const std::string raw = trim(rawFsType);
    if (raw.empty() || raw == "-") {
        return rawFsType;
    }
    std::vector<std::string> parts = split(raw, "|", false);
    bool changed = false;
    for (std::string& part : parts) {
        const std::string p = trim(part);
        if (!empiezaPorNoCaseAscii(p, "gpt=")) {
            continue;
        }
        const std::string guidRaw = trim(mid(p, 4));
        if (guidRaw.empty() || guidRaw == "-") {
            continue;
        }
        const std::string name = windowsGptTypeName(guidRaw);
        if (name.empty()) {
            continue;
        }
        part = "gpt=" + name;
        changed = true;
    }
    return changed ? join(parts, "|") : rawFsType;
}

bool windowsPartitionTypeIsProtected(const std::string& rawFsType) {
    if (trim(rawFsType).empty()) {
        return false;
    }
    const std::vector<std::string> parts = split(rawFsType, "|", false);
    for (const std::string& partRaw : parts) {
        const std::string part = trim(partRaw);
        if (!empiezaPorNoCaseAscii(part, "type=")) {
            continue;
        }
        const std::string v = toLowerAscii(trim(mid(part, 5)));
        if (v == "system" || v == "recovery" || v == "reserved") {
            return true;
        }
    }
    // Discos enteros: el de arranque y el del sistema van protegidos.
    //
    // Hasta ahora el disco completo solo se ofrecía cuando no tenía nada aprovechable
    // dentro. Al pasar a ofrecerlo siempre —que es lo que necesita OpenZFS on Windows,
    // incapaz de usar una partición suelta— hay que distinguir el disco que se puede
    // entregar a ZFS del que arranca el sistema.
    for (const std::string& partRaw : parts) {
        const std::string part = toLowerAscii(trim(partRaw));
        if (part == "isboot=true" || part == "issystem=true") {
            return true;
        }
    }
    return false;
}

TransferButtonState computeTransferButtonState(const TransferButtonInputs& in) {
    TransferButtonState out;
    const bool sameSelection = !in.srcSelectionKey.empty()
                            && (in.srcSelectionKey == in.dstSelectionKey);
    out.copyEnabled = in.srcDatasetSelected && in.srcSnapshotSelected && in.dstDatasetSelected
                   && !in.dstSnapshotSelected;
    out.levelEnabled = in.srcDatasetSelected && in.dstDatasetSelected && !in.dstSnapshotSelected
                    && !sameSelection;
    out.syncEnabled = in.srcDatasetSelected && !in.srcSnapshotSelected && in.dstDatasetSelected
                   && !in.dstSnapshotSelected && !sameSelection && in.srcSelectionConsistent
                   && in.dstSelectionConsistent && in.srcDatasetMounted && in.dstDatasetMounted;
    return out;
}

std::map<std::string, std::vector<std::string>> duplicateMountpoints(
    const std::map<std::string, std::string>& datasetMountpoints) {
    // std::map recorre ordenado por clave, igual que QMap: el orden del resultado no
    // depende del de entrada, que es lo que hace comparable la salida.
    std::map<std::string, std::vector<std::string>> grouped;
    for (const auto& [dataset, mpRaw] : datasetMountpoints) {
        const std::string mp = trim(mpRaw);
        const std::string mpl = toLowerAscii(mp);
        if (dataset.empty() || mp.empty() || mpl == "none" || mpl == "-") {
            continue;
        }
        grouped[mp].push_back(dataset);
    }
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& [mp, datasets] : grouped) {
        if (datasets.size() > 1) {
            out.insert({mp, datasets});
        }
    }
    return out;
}

std::vector<MountpointConflict> externalMountpointConflicts(
    const std::map<std::string, std::string>& targetDatasetMountpoints,
    const std::map<std::string, std::vector<std::string>>& mountedByMountpoint) {
    std::vector<MountpointConflict> out;
    for (const auto& [requestedDataset, mpRaw] : targetDatasetMountpoints) {
        const std::string mountpoint = trim(mpRaw);
        if (requestedDataset.empty() || mountpoint.empty()) {
            continue;
        }
        const auto it = mountedByMountpoint.find(mountpoint);
        if (it == mountedByMountpoint.end()) {
            continue;
        }
        for (const std::string& mountedDs : it->second) {
            if (mountedDs.empty() || mountedDs == requestedDataset) {
                continue;
            }
            out.push_back(MountpointConflict{mountpoint, mountedDs, requestedDataset});
        }
    }
    return out;
}

std::vector<std::string> posixShellSplitArgs(const std::string& s) {
    std::vector<std::string> result;
    std::string current;
    bool inSQ = false, inDQ = false, started = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inSQ) {
            if (c == '\'') { inSQ = false; }
            else { current += c; }
        } else if (inDQ) {
            if (c == '"') { inDQ = false; }
            else if (c == '\\' && i + 1 < s.size()) {
                const char next = s[++i];
                if (next == '"' || next == '\\' || next == '$' || next == '`') {
                    current += next;
                } else {
                    current += c;
                    current += next;
                }
            } else {
                current += c;
            }
        } else {
            // "started" distingue un token vacío ESCRITO ('') de la ausencia de token.
            // Sin esto, un argumento vacío desaparecía al recuperarlo, y
            // --zfs-send-to-peer pasa vacíos a menudo (snapshot base y flags), con lo
            // que los siguientes argumentos se corrían de posición.
            if (c == '\'') { inSQ = true; started = true; }
            else if (c == '"') { inDQ = true; started = true; }
            else if (c == '\\' && i + 1 < s.size()) { current += s[++i]; }
            else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!current.empty() || started) {
                    result.push_back(current);
                    current.clear();
                    started = false;
                }
            } else {
                current += c;
            }
        }
    }
    if (!current.empty() || started) { result.push_back(current); }
    return result;
}

std::string redactSecretsForStorage(const std::string& command,
                                    const std::vector<StorableSecret>& secrets,
                                    bool* okOut) {
    if (okOut) {
        *okOut = true;
    }
    std::string out = command;
    for (const StorableSecret& s : secrets) {
        if (s.secret.empty() || trim(s.key).empty()) {
            continue;
        }
        const std::string marker = storedSecretMarker(s.key);
        // La octal primero: es la que produce withSudoCommand y la que contiene a la
        // literal como caso raro. Sustituir al revés dejaría trozos de la octal sueltos.
        replaceAll(out, shPrintfOctalEscaped(s.secret), marker);
        replaceAll(out, s.secret, marker);
    }
    for (const StorableSecret& s : secrets) {
        if (s.secret.empty()) {
            continue;
        }
        if (contains(out, s.secret) || contains(out, shPrintfOctalEscaped(s.secret))) {
            if (okOut) {
                *okOut = false;
            }
            return std::string();
        }
    }
    return out;
}

std::string restoreSecretsFromStorage(const std::string& stored,
                                      const std::vector<StorableSecret>& secrets) {
    std::string out = stored;
    for (const StorableSecret& s : secrets) {
        if (trim(s.key).empty()) {
            continue;
        }
        replaceAll(out, storedSecretMarker(s.key), shPrintfOctalEscaped(s.secret));
    }
    return out;
}

std::string asciiSafeShellCommand(const std::string& cmd) {
    bool hasNonAscii = false;
    for (const char c : cmd) {
        if (static_cast<unsigned char>(c) > 127) {
            hasNonAscii = true;
            break;
        }
    }
    if (!hasNonAscii) {
        return cmd;
    }
    // `eval "$(printf '%b' '...')"`: los escapes octales son ASCII, printf reconstruye
    // los bytes originales y eval ejecuta la orden tal cual era. La entrada estándar
    // queda libre, que hace falta porque por ahí viajan cargas útiles (el binario del
    // agente, la passphrase de un dataset cifrado).
    return format("eval \"$(printf '%b' '%1')\"", {shPrintfOctalEscaped(cmd)});
}

bool looksLikeSudoAuthFailure(const std::string& text) {
    // toLowerUtf8, no la variante ASCII: las agujas llevan acento y con
    // «SUDO: 1 INTENTO DE CONTRASEÑA INCORRECTO» la versión ASCII respondía que no
    // había fallo de contraseña. Ver el comentario de más abajo: ese error exacto ya
    // ocurrió una vez por otro motivo.
    const std::string t = toLowerUtf8(trim(text));
    if (t.empty()) {
        return false;
    }
    // "no está en sudoers" y "no se permite ejecutar" son de autorización, no de
    // autenticación: reintroducir la contraseña no los arregla, así que no se ofrece.
    if (contains(t, "not in the sudoers") || contains(t, "no está en el fichero sudoers")
        || contains(t, "is not allowed to execute")) {
        return false;
    }
    // Cadenas OBSERVADAS, no supuestas. Las de español salen de un sudo 1.9 real en
    // Ubuntu con LANG=es_ES.UTF-8: dice "Lo siento, pruebe otra vez", no "inténtelo de
    // nuevo", que es lo que había escrito de memoria y no casaba con nada. El resultado
    // era que un rechazo de contraseña se clasificaba como "no se pudo comprobar" y el
    // usuario no recibía el aviso.
    static const std::vector<std::string> kNeedles = {
        "sorry, try again",
        "incorrect password attempt",
        "authentication failure",
        "a password is required",
        "lo siento, pruebe otra vez",
        "intento de contraseña incorrecto",
        "intentos de contraseña incorrectos",
        "se requiere una contraseña",
        "se necesita una contraseña",
        "fallo de autenticación"
    };
    for (const std::string& needle : kNeedles) {
        if (contains(t, needle)) {
            return true;
        }
    }
    return false;
}

std::string maskCommandSecrets(const std::string& input) {
    std::string out = input;
    // std::regex con ECMAScript en vez de la PCRE de Qt. Las dos diferencias que había
    // que resolver, y que están comprobadas contra la salida de Qt:
    //   - el modificador en línea (?i) NO existe: va como bandera icase;
    //   - el reemplazo se escribe $1, no \1.
    // La anticipación (?=...) sí existe en ECMAScript, que era lo que podía faltar.
    const auto sub = [&out](const char* pattern, const char* replacement,
                            std::regex::flag_type extra = std::regex::ECMAScript) {
        out = std::regex_replace(out, std::regex(pattern, extra), replacement);
    };

    // Contraseña de sudo, forma antigua (literal) y actual (escapes octales para %b).
    sub("(printf\\s+'%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'(?=\\s*\\|\\s*sudo)", "$1'[secret]'");
    sub("(printf\\s+'%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'(?=\\s*;\\s*cat)", "$1'[secret]'");
    sub("(printf\\s+'%b\\\\n'\\s+)'(?:\\\\0[0-7]{1,3})*'", "$1'[secret]'");

    // Frase de cifrado al crear un dataset: `zfs create -o keyformat=passphrase` la
    // pide DOS veces por la entrada estándar, de ahí el formato con dos %s.
    sub("(printf\\s+'%s\\\\n%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'\\s+'(?:[^'\\\\]|\\\\.)*'",
        "$1'[secret]' '[secret]'");
    // Su equivalente en PowerShell, que la mete en una variable.
    sub("(\\$pp\\s*=\\s*)'(?:[^']|'')*'", "$1'[secret]'");

    // Verbos del agente cuyo ÚLTIMO argumento es un secreto en base64. Los separadores
    // se aceptan como clase de caracteres porque la orden puede venir entrecomillada una
    // o dos veces —`'"'"'` cuando va dentro de otro shSingleQuote—, y escribir cada
    // variante a mano es justo lo que ya falló antes con la frase de `zfs create`.
    sub("(--mutate-zfs-(?:load-key|change-key|create)['\" \\\\]+"
        "[A-Za-z0-9+/=]+['\" \\\\]+)[A-Za-z0-9+/=]+",
        "$1[secret]");

    // Cualquier `password=` / `password:` suelto.
    sub("(password\\s*[:=]\\s*)\\S+", "$1[secret]",
        std::regex::ECMAScript | std::regex::icase);
    return out;
}

std::string parseOpenZfsVersionText(const std::string& text) {
    if (trim(text).empty()) {
        return std::string();
    }
    const std::string lower = toLowerUtf8(text);
    static const std::regex patterns[] = {
        std::regex("\\bzfs(?:-kmod)?[-\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b"),
        std::regex("\\bopenzfs(?:[-\\s]+version)?[:\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b"),
        std::regex("\\b(?:zfs|zpool)[^\\r\\n]*?\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b"),
    };
    for (const std::regex& rx : patterns) {
        std::smatch m;
        if (!std::regex_search(lower, m, rx)) {
            continue;
        }
        const std::string ver = m[1].str();
        // Un mayor por encima de 10 delata que se ha pescado otra cosa, no una versión.
        // Se convierte a mano: stoi lanza donde QString::toInt devolvía 0.
        const std::string mayorTxt = split(ver, ".", false).front();
        int mayor = 0;
        for (const char c : mayorTxt) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                mayor = -1;
                break;
            }
            mayor = mayor * 10 + (c - '0');
            if (mayor > 1000) {
                break;
            }
        }
        if (mayor >= 0 && mayor <= 10) {
            return ver;
        }
    }
    return std::string();
}

std::vector<ImportablePoolInfo> parseZpoolImportOutput(const std::string& text) {
    std::vector<ImportablePoolInfo> rows;
    const std::regex poolNameRx("^[A-Za-z0-9_.:-]+$");
    std::string currentPool, currentGuid, currentState, currentReason;
    bool collectingStatus = false;

    auto flushCurrent = [&]() {
        if (currentPool.empty()) {
            return;
        }
        if (!std::regex_search(currentPool, poolNameRx)) {
            currentPool.clear();
            currentGuid.clear();
            currentState.clear();
            currentReason.clear();
            collectingStatus = false;
            return;
        }
        if (currentState.empty() && currentReason.empty()) {
            currentPool.clear();
            currentGuid.clear();
            collectingStatus = false;
            return;
        }
        rows.push_back(ImportablePoolInfo{
            currentPool,
            currentGuid,
            currentState.empty() ? std::string("UNKNOWN") : currentState,
            currentReason,
        });
        currentPool.clear();
        currentGuid.clear();
        currentState.clear();
        currentReason.clear();
        collectingStatus = false;
    };

    for (const std::string& lineRaw : split(text, "\n", false)) {
        const std::string line = trim(lineRaw);
        if (startsWith(line, "pool: ")) {
            flushCurrent();
            currentPool = trim(mid(line, 6));
            continue;
        }
        if (currentPool.empty()) {
            continue;
        }
        if (startsWith(line, "state: ")) {
            currentState = trim(mid(line, 7));
            collectingStatus = false;
            continue;
        }
        if (startsWith(line, "id: ")) {
            currentGuid = trim(mid(line, 4));
            continue;
        }
        if (startsWith(line, "status: ")) {
            currentReason = trim(mid(line, 8));
            collectingStatus = true;
            continue;
        }
        if (collectingStatus) {
            if (startsWith(line, "action:") || startsWith(line, "see:") || startsWith(line, "config:")) {
                collectingStatus = false;
            } else if (!line.empty()) {
                currentReason = trim(currentReason + " " + line);
                continue;
            }
        }
        if (startsWith(line, "cannot import")) {
            if (!currentReason.empty()) {
                currentReason += " ";
            }
            currentReason += line;
        }
    }
    flushCurrent();
    return rows;
}

}  // namespace zfsmgr::base::helpers
