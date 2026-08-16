#include "helpers.h"

#include "strutil.h"

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
using zfsmgr::base::trim;

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

}  // namespace zfsmgr::base::helpers
