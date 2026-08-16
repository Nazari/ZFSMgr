#include "helpers.h"

#include "daemonpayload.h"
#include "strutil.h"

#include <cstdio>
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
using zfsmgr::base::join;
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

}  // namespace zfsmgr::base::helpers
