#include "mainwindow_helpers.h"

#include <QDir>
#include <QRegularExpression>

namespace mwhelpers {

QString oneLine(const QString& v, int maxLen) {
    QString x = v.simplified();
    return x.left(maxLen);
}

QString shSingleQuote(const QString& s) {
    QString out = s;
    out.replace('\'', "'\"'\"'");
    return QStringLiteral("'") + out + QStringLiteral("'");
}

bool isMountedValueTrue(const QString& value) {
    const QString v = value.trimmed().toLower();
    return v == QStringLiteral("yes")
        || v == QStringLiteral("on")
        || v == QStringLiteral("true")
        || v == QStringLiteral("1");
}

QString parentDatasetName(const QString& dataset) {
    const int slash = dataset.lastIndexOf('/');
    if (slash <= 0) {
        return QString();
    }
    return dataset.left(slash);
}

QString normalizeDriveLetterValue(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty() || s == QStringLiteral("-") || s.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    s.replace(QStringLiteral(":\\"), QString());
    s.replace(':', QString());
    s.replace('\\', QString());
    s.replace('/', QString());
    s = s.trimmed().toUpper();
    if (s.isEmpty()) {
        return QString();
    }
    const QChar d = s[0];
    if (!d.isLetter()) {
        return QString();
    }
    return QString(d);
}

bool looksLikePowerShellScript(const QString& cmd) {
    const QString c = cmd.toLower();
    const QString t = c.trimmed();
    if (t.startsWith('[') || t.startsWith('$') || c.contains(QStringLiteral("::"))) {
        return true;
    }
    if (t.startsWith(QStringLiteral("zfs "))
        || t == QStringLiteral("zfs")
        || t.startsWith(QStringLiteral("zpool "))
        || t == QStringLiteral("zpool")
        || t.startsWith(QStringLiteral("where.exe "))) {
        return true;
    }
    return c.contains(QStringLiteral("out-null"))
        || c.contains(QStringLiteral("test-path"))
        || c.contains(QStringLiteral("resolve-path"))
        || c.contains(QStringLiteral("join-path"))
        || c.contains(QStringLiteral("new-item"))
        || c.contains(QStringLiteral("remove-item"))
        || c.contains(QStringLiteral("sort-object"))
        || c.contains(QStringLiteral("select-object"))
        || c.contains(QStringLiteral("get-childitem"))
        || c.contains(QStringLiteral("write-output"))
        || c.contains(QStringLiteral("invoke-expression"))
        || c.contains(QStringLiteral("foreach("))
        || c.contains(QStringLiteral("$lastexitcode"))
        || c.contains(QStringLiteral("[string]::"))
        || c.contains(QStringLiteral("$env:path"))
        || c.contains(QStringLiteral("powershell "));
}

bool isWindowsOsType(const QString& osType) {
    return osType.trimmed().toLower().contains(QStringLiteral("windows"));
}

QString parseOpenZfsVersionText(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    const QString lower = text.toLower();
    const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("\\bzfs(?:-kmod)?[-\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
        QRegularExpression(QStringLiteral("\\bopenzfs(?:[-\\s]+version)?[:\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
        QRegularExpression(QStringLiteral("\\b(?:zfs|zpool)[^\\r\\n]*?\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
    };
    for (const QRegularExpression& rx : patterns) {
        const QRegularExpressionMatch m = rx.match(lower);
        if (m.hasMatch()) {
            const QString ver = m.captured(1);
            const int major = ver.section('.', 0, 0).toInt();
            if (major <= 10) {
                return ver;
            }
        }
    }
    return QString();
}

QVector<ImportablePoolInfo> parseZpoolImportOutput(const QString& text) {
    QVector<ImportablePoolInfo> rows;
    const QRegularExpression poolNameRx(QStringLiteral("^[A-Za-z0-9_.:-]+$"));
    QString currentPool;
    QString currentState;
    QString currentReason;
    bool collectingStatus = false;

    auto flushCurrent = [&]() {
        if (currentPool.isEmpty()) {
            return;
        }
        if (!poolNameRx.match(currentPool).hasMatch()) {
            currentPool.clear();
            currentState.clear();
            currentReason.clear();
            collectingStatus = false;
            return;
        }
        if (currentState.isEmpty() && currentReason.isEmpty()) {
            currentPool.clear();
            collectingStatus = false;
            return;
        }
        rows.push_back(ImportablePoolInfo{
            currentPool,
            currentState.isEmpty() ? QStringLiteral("UNKNOWN") : currentState,
            currentReason,
        });
        currentPool.clear();
        currentState.clear();
        currentReason.clear();
        collectingStatus = false;
    };

    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("pool: "))) {
            flushCurrent();
            currentPool = line.mid(QStringLiteral("pool: ").size()).trimmed();
            continue;
        }
        if (currentPool.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("state: "))) {
            currentState = line.mid(QStringLiteral("state: ").size()).trimmed();
            collectingStatus = false;
            continue;
        }
        if (line.startsWith(QStringLiteral("status: "))) {
            currentReason = line.mid(QStringLiteral("status: ").size()).trimmed();
            collectingStatus = true;
            continue;
        }
        if (collectingStatus) {
            if (line.startsWith(QStringLiteral("action:")) || line.startsWith(QStringLiteral("see:")) || line.startsWith(QStringLiteral("config:"))) {
                collectingStatus = false;
            } else if (!line.isEmpty()) {
                currentReason = (currentReason + QStringLiteral(" ") + line).trimmed();
                continue;
            }
        }
        if (line.startsWith(QStringLiteral("cannot import"))) {
            if (!currentReason.isEmpty()) {
                currentReason += QStringLiteral(" ");
            }
            currentReason += line;
        }
    }
    flushCurrent();
    return rows;
}

TransferButtonState computeTransferButtonState(const TransferButtonInputs& in) {
    TransferButtonState out;
    const bool sameSelection = !in.srcSelectionKey.isEmpty() && (in.srcSelectionKey == in.dstSelectionKey);
    out.copyEnabled = in.srcDatasetSelected && in.srcSnapshotSelected && in.dstDatasetSelected && !in.dstSnapshotSelected;
    out.levelEnabled = in.srcDatasetSelected && in.dstDatasetSelected && !in.dstSnapshotSelected && !sameSelection;
    out.syncEnabled = in.srcDatasetSelected
        && !in.srcSnapshotSelected
        && in.dstDatasetSelected
        && !in.dstSnapshotSelected
        && !sameSelection
        && in.srcSelectionConsistent
        && in.dstSelectionConsistent
        && in.srcDatasetMounted
        && in.dstDatasetMounted;
    return out;
}

bool parentMountCheckRequired(const QString& parentMountpoint, const QString& parentCanmount) {
    const QString mp = parentMountpoint.trimmed().toLower();
    const QString canmount = parentCanmount.trimmed().toLower();
    if (mp.isEmpty() || mp == QStringLiteral("none")) {
        return false;
    }
    if (canmount == QStringLiteral("off")) {
        return false;
    }
    return true;
}

bool parentAllowsChildMount(const QString& parentMountpoint, const QString& parentCanmount, const QString& parentMounted) {
    if (!parentMountCheckRequired(parentMountpoint, parentCanmount)) {
        return true;
    }
    return isMountedValueTrue(parentMounted);
}

QMap<QString, QStringList> duplicateMountpoints(const QMap<QString, QString>& datasetMountpoints) {
    QMap<QString, QStringList> grouped;
    for (auto it = datasetMountpoints.constBegin(); it != datasetMountpoints.constEnd(); ++it) {
        const QString dataset = it.key();
        const QString mp = it.value().trimmed();
        const QString mpl = mp.toLower();
        if (dataset.isEmpty() || mp.isEmpty() || mpl == QStringLiteral("none") || mpl == QStringLiteral("-")) {
            continue;
        }
        grouped[mp].push_back(dataset);
    }
    QMap<QString, QStringList> out;
    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        if (it.value().size() > 1) {
            out.insert(it.key(), it.value());
        }
    }
    return out;
}

QVector<MountpointConflict> externalMountpointConflicts(const QMap<QString, QString>& targetDatasetMountpoints,
                                                        const QMap<QString, QStringList>& mountedByMountpoint) {
    QVector<MountpointConflict> out;
    for (auto it = targetDatasetMountpoints.constBegin(); it != targetDatasetMountpoints.constEnd(); ++it) {
        const QString requestedDataset = it.key();
        const QString mountpoint = it.value().trimmed();
        if (requestedDataset.isEmpty() || mountpoint.isEmpty()) {
            continue;
        }
        const QStringList mountedDatasets = mountedByMountpoint.value(mountpoint);
        for (const QString& mountedDs : mountedDatasets) {
            if (mountedDs.isEmpty() || mountedDs == requestedDataset) {
                continue;
            }
            out.push_back(MountpointConflict{mountpoint, mountedDs, requestedDataset});
        }
    }
    return out;
}

QVector<QPair<QString, QString>> parseZfsMountOutput(const QString& text) {
    QVector<QPair<QString, QString>> out;
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    static const QRegularExpression rowRx(QStringLiteral("^\\s*(\\S+)\\s+(.+?)\\s*$"));
    for (const QString& raw : lines) {
        const QString ln = raw.trimmed();
        if (ln.isEmpty()) {
            continue;
        }
        const QRegularExpressionMatch m = rowRx.match(ln);
        if (!m.hasMatch()) {
            continue;
        }
        const QString ds = m.captured(1).trimmed();
        const QString mp = m.captured(2).trimmed();
        if (ds.isEmpty() || mp.isEmpty()) {
            continue;
        }
        out.push_back(qMakePair(ds, mp));
    }
    return out;
}

QString buildHasMountedChildrenCommand(bool isWindows, const QString& datasetName) {
    if (isWindows) {
        QString dsPs = datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        return QStringLiteral(
                   "$ds='%1'; "
                   "$has=$false; "
                   "$children=@(zfs list -H -o name -r $ds 2>$null); "
                   "if ($LASTEXITCODE -ne 0) { exit 2 }; "
                   "foreach ($c in $children) { "
                   "  if ([string]::IsNullOrWhiteSpace($c) -or $c -eq $ds) { continue }; "
                   "  $m=(zfs get -H -o value mounted $c 2>$null | Out-String).Trim().ToLower(); "
                   "  if ($m -eq 'yes' -or $m -eq 'on' -or $m -eq 'true' -or $m -eq '1') { $has=$true; break } "
                   "}; "
                   "if ($has) { exit 0 } else { exit 1 }")
            .arg(dsPs);
    }
    return QStringLiteral(
               "DATASET=%1; zfs mount | "
               "awk -v ds=\"$DATASET\" '$1!=ds && index($1, ds \"/\")==1 { found=1; exit } END { exit found ? 0 : 1 }'")
        .arg(shSingleQuote(datasetName));
}

QString buildRecursiveUmountCommand(bool isWindows, const QString& datasetName) {
    if (isWindows) {
        QString dsPs = datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        return QStringLiteral(
                   "$ds='%1'; "
                   "$list=@(zfs list -H -o name -r $ds 2>$null); "
                   "if ($LASTEXITCODE -ne 0) { throw 'zfs list failed' }; "
                   "$sorted = $list | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object { $_.Length } -Descending; "
                   "foreach ($d in $sorted) { zfs unmount $d 2>$null | Out-Null }")
            .arg(dsPs);
    }
    return QStringLiteral(
               "set -e; DATASET=%1; zfs mount | "
               "awk -v ds=\"$DATASET\" '$1==ds || index($1, ds \"/\")==1 { print $1 }' | "
               "awk '{print length, $0}' | sort -rn | cut -d' ' -f2- | "
               "while IFS= read -r ds; do [ -n \"$ds\" ] && zfs umount \"$ds\"; done")
        .arg(shSingleQuote(datasetName));
}

QString buildSingleUmountCommand(bool isWindows, const QString& datasetName) {
    const QString dsQ = shSingleQuote(datasetName);
    return isWindows ? QStringLiteral("zfs unmount %1").arg(dsQ)
                     : QStringLiteral("zfs umount %1").arg(dsQ);
}

QString buildSingleMountCommand(const QString& datasetName) {
    return QStringLiteral("zfs mount %1").arg(shSingleQuote(datasetName));
}

QString buildMountChildrenCommand(bool isWindows, const QString& datasetName) {
    if (isWindows) {
        QString dsPs = datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        return QStringLiteral(
                   "$ds='%1'; "
                   "$items = @(zfs list -H -o name -r $ds 2>$null); "
                   "if ($LASTEXITCODE -ne 0) { throw 'zfs list failed' }; "
                   "foreach ($child in $items) { "
                   "  if ([string]::IsNullOrWhiteSpace($child)) { continue }; "
                   "  $m = (zfs get -H -o value mounted $child 2>$null | Out-String).Trim().ToLower(); "
                   "  if ($m -ne 'yes' -and $m -ne 'on' -and $m -ne 'true' -and $m -ne '1') { "
                   "    zfs mount $child 2>$null | Out-Null "
                   "  } "
                   "}")
            .arg(dsPs);
    }
    return QStringLiteral(
               "set -e; DATASET=%1; "
               "zfs list -H -o name -r \"$DATASET\" | "
               "while IFS= read -r child; do "
               "  [ -n \"$child\" ] || continue; "
               "  mounted=$(zfs get -H -o value mounted \"$child\" 2>/dev/null || true); "
               "  case \"$mounted\" in yes|on|true|1) : ;; *) zfs mount \"$child\" ;; esac; "
               "done")
        .arg(shSingleQuote(datasetName));
}

QString buildWindowsMountPrecheckCommand(const QString& datasetName, const QString& effectiveMountpoint) {
    QString dsPs = datasetName;
    dsPs.replace('\'', QStringLiteral("''"));
    QString mpPs = effectiveMountpoint.trimmed();
    mpPs.replace('\'', QStringLiteral("''"));
    return QStringLiteral(
               "$ds='%1'; "
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
               "}")
        .arg(dsPs, mpPs);
}

QString sshControlPath() {
#ifdef Q_OS_MAC
    return QStringLiteral("/tmp/zfsmgr-%C");
#else
    return QDir::tempPath() + QStringLiteral("/zfsmgr-ssh-%C");
#endif
}

QString sshUserHost(const ConnectionProfile& p) {
    return QStringLiteral("%1@%2").arg(p.username, p.host);
}

QString sshUserHostPort(const ConnectionProfile& p) {
    const QString port = (p.port > 0) ? QString::number(p.port) : QStringLiteral("22");
    return QStringLiteral("%1:%2").arg(sshUserHost(p), port);
}

QString sshBaseCommand(const ConnectionProfile& p) {
    QString cmd = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
                                 " -o ControlMaster=auto -o ControlPersist=300 -o ControlPath=%1")
                      .arg(shSingleQuote(sshControlPath()));
    if (p.port > 0) {
        cmd += QStringLiteral(" -p ") + QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        cmd += QStringLiteral(" -i ") + shSingleQuote(p.keyPath);
    }
    return cmd;
}

QString buildSshTargetPrefix(const ConnectionProfile& p) {
    return sshBaseCommand(p)
        + QStringLiteral(" ")
        + shSingleQuote(sshUserHost(p));
}

QString buildSimpleSshInvocation(const ConnectionProfile& p, const QString& remoteCmd) {
    return buildSshTargetPrefix(p)
        + QStringLiteral(" ")
        + shSingleQuote(remoteCmd);
}

QString streamProgressPipeFilter() {
    return QStringLiteral("((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat)");
}

QString buildPipedTransferCommand(const QString& sendSegment, const QString& recvSegment) {
    return sendSegment
        + QStringLiteral(" | ")
        + streamProgressPipeFilter()
        + QStringLiteral(" | ")
        + recvSegment;
}

QString streamCodecName(StreamCodec codec) {
    switch (codec) {
        case StreamCodec::Zstd:
            return QStringLiteral("zstd-fast");
        case StreamCodec::Gzip:
            return QStringLiteral("gzip-fast");
        case StreamCodec::None:
        default:
            return QStringLiteral("none");
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

QString buildTarSourceCommand(bool isWindows, const QString& mountPath, StreamCodec codec) {
    switch (codec) {
        case StreamCodec::Zstd:
            return isWindows
                       ? QStringLiteral("$p=%1; tar -cf - -C $p . | zstd -1 -T0 -q -c").arg(shSingleQuote(mountPath))
                       : QStringLiteral("tar --acls --xattrs -cpf - -C %1 . | zstd -1 -T0 -q -c").arg(shSingleQuote(mountPath));
        case StreamCodec::Gzip:
            return isWindows
                       ? QStringLiteral("$p=%1; tar -cf - -C $p . | gzip -1 -c").arg(shSingleQuote(mountPath))
                       : QStringLiteral("tar --acls --xattrs -cpf - -C %1 . | gzip -1 -c").arg(shSingleQuote(mountPath));
        case StreamCodec::None:
        default:
            return isWindows
                       ? QStringLiteral("$p=%1; tar -cf - -C $p .").arg(shSingleQuote(mountPath))
                       : QStringLiteral("tar --acls --xattrs -cpf - -C %1 .").arg(shSingleQuote(mountPath));
    }
}

QString buildTarDestinationCommand(bool isWindows, const QString& mountPath, StreamCodec codec) {
    const QString decodePipe =
        (codec == StreamCodec::Zstd) ? QStringLiteral("zstd -d -q -c - | ")
        : (codec == StreamCodec::Gzip) ? QStringLiteral("gzip -d -c - | ")
        : QString();
    if (isWindows) {
        return QStringLiteral("$ProgressPreference='SilentlyContinue'; $p=%1; if (!(Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null }; %2tar -xpf - -C $p")
            .arg(shSingleQuote(mountPath), decodePipe);
    }
    return QStringLiteral("mkdir -p %1 && %2tar --acls --xattrs -xpf - -C %1")
        .arg(shSingleQuote(mountPath), decodePipe);
}

QString withSudoCommand(const ConnectionProfile& p, const QString& cmd) {
    if (isWindowsOsType(p.osType)) {
        return cmd;
    }
    if (!p.useSudo) {
        return cmd;
    }
    if (!p.password.isEmpty()) {
        return QStringLiteral("printf '%s\\n' %1 | sudo -S -p '' sh -lc %2")
            .arg(shSingleQuote(p.password), shSingleQuote(cmd));
    }
    return QStringLiteral("sudo -n ") + cmd;
}

QString withSudoStreamInputCommand(const ConnectionProfile& p, const QString& cmd) {
    if (isWindowsOsType(p.osType)) {
        return cmd;
    }
    if (!p.useSudo) {
        return cmd;
    }
    if (!p.password.isEmpty()) {
        return QStringLiteral("{ printf '%s\\n' %1; cat; } | sudo -S -p '' sh -lc %2")
            .arg(shSingleQuote(p.password), shSingleQuote(cmd));
    }
    return QStringLiteral("sudo -n sh -lc %1").arg(shSingleQuote(cmd));
}

QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd) {
    QStringList parts;
    parts << QStringLiteral("ssh");
    parts << QStringLiteral("-o BatchMode=yes");
    parts << QStringLiteral("-o ConnectTimeout=10");
    parts << QStringLiteral("-o LogLevel=ERROR");
    parts << QStringLiteral("-o StrictHostKeyChecking=no");
    parts << QStringLiteral("-o UserKnownHostsFile=/dev/null");
    parts << QStringLiteral("-o ControlMaster=auto");
    parts << QStringLiteral("-o ControlPersist=300");
    parts << QStringLiteral("-o ControlPath=%1").arg(shSingleQuote(sshControlPath()));
    if (p.port > 0) {
        parts << QStringLiteral("-p %1").arg(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        parts << QStringLiteral("-i %1").arg(shSingleQuote(p.keyPath));
    }
    parts << sshUserHost(p);
    parts << shSingleQuote(remoteCmd);
    return parts.join(' ');
}

} // namespace mwhelpers
