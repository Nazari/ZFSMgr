#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace {
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;
using mwhelpers::sshUserHostPort;
using mwhelpers::formatWindowsFsTypeDetail;
using mwhelpers::windowsPartitionTypeIsProtected;

struct ZPoolCreationDefaults {
    bool force{true};
    QString altroot{QStringLiteral("/mnt/fc16")};
    QString ashift{QStringLiteral("12")};
    QString autotrim{QStringLiteral("on")};
    QString compatibility{QStringLiteral("openzfs-2.4-linux")};
    QString fsProps{
        QStringLiteral("acltype=posixacl,"
                       "xattr=sa,"
                       "dnodesize=auto,"
                       "compression=lz4,"
                       "normalization=formD,"
                       "relatime=on,"
                       "canmount=noauto,"
                       "mountpoint=none")};
};

ZPoolCreationDefaults loadZPoolCreationDefaults(const QString& iniPath) {
    ZPoolCreationDefaults d;
    if (iniPath.trimmed().isEmpty()) {
        return d;
    }
    QSettings ini(iniPath, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("ZPoolCreationDefaults"));
    bool touched = false;
    auto ensure = [&](const QString& key, const QVariant& value) {
        if (!ini.contains(key)) {
            ini.setValue(key, value);
            touched = true;
        }
    };
    ensure(QStringLiteral("force"), d.force);
    ensure(QStringLiteral("altroot"), d.altroot);
    ensure(QStringLiteral("ashift"), d.ashift);
    ensure(QStringLiteral("autotrim"), d.autotrim);
    ensure(QStringLiteral("compatibility"), d.compatibility);
    ensure(QStringLiteral("fs_properties"), d.fsProps);

    d.force = ini.value(QStringLiteral("force"), d.force).toBool();
    d.altroot = ini.value(QStringLiteral("altroot"), d.altroot).toString().trimmed();
    d.ashift = ini.value(QStringLiteral("ashift"), d.ashift).toString().trimmed();
    d.autotrim = ini.value(QStringLiteral("autotrim"), d.autotrim).toString().trimmed();
    d.compatibility = ini.value(QStringLiteral("compatibility"), d.compatibility).toString().trimmed();
    d.fsProps = ini.value(QStringLiteral("fs_properties"), d.fsProps).toString().trimmed();
    ini.endGroup();
    if (touched) {
        ini.sync();
    }
    return d;
}

QString parentDiskDevicePath(const QString& rawPath) {
    const QString path = rawPath.trimmed();
    if (path.isEmpty()) {
        return QString();
    }
    static const QList<QRegularExpression> rules = {
        QRegularExpression(QStringLiteral(R"(^(.*\/nvme\d+n\d+)p\d+$)")),
        QRegularExpression(QStringLiteral(R"(^(.*\/mmcblk\d+)p\d+$)")),
        QRegularExpression(QStringLiteral(R"(^(.*\/disk\d+)s\d+$)")),
        QRegularExpression(QStringLiteral(R"(^(.*\/(?:sd[a-z]+|vd[a-z]+|xvd[a-z]+))\d+$)")),
    };
    for (const auto& rx : rules) {
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            return m.captured(1);
        }
    }
    return QString();
}

QString wrapDeviceDisplayText(const QString& raw, int softWidth = 42) {
    const QString s = raw;
    if (s.size() <= softWidth) {
        return s;
    }
    QString out;
    out.reserve(s.size() + (s.size() / softWidth) + 4);
    int col = 0;
    for (int i = 0; i < s.size(); ++i) {
        const QChar ch = s[i];
        out += ch;
        ++col;
        const bool breakChar = (ch == QChar('/') || ch == QChar('\\') || ch == QChar(' '));
        if ((breakChar && col >= (softWidth / 2)) || col >= softWidth) {
            if (i + 1 < s.size()) {
                out += QChar('\n');
            }
            col = 0;
        }
    }
    return out;
}

bool isRootDevicePath(const QString& rawPath) {
    const QString p = rawPath.trimmed();
    if (p.isEmpty()) {
        return true;
    }
    static const QList<QRegularExpression> rootPatterns = {
        QRegularExpression(QStringLiteral(R"(^/dev/(sd[a-z]+|vd[a-z]+|xvd[a-z]+)$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/nvme\d+n\d+$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/mmcblk\d+$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/disk\d+$)")),
        QRegularExpression(QStringLiteral(R"(^(\\\\\.\\PHYSICALDRIVE\d+|\\\\\?\\PhysicalDrive\d+)$)"),
                           QRegularExpression::CaseInsensitiveOption),
    };
    for (const auto& rx : rootPatterns) {
        if (rx.match(p).hasMatch()) {
            return true;
        }
    }
    return false;
}
} // namespace

void MainWindow::createPoolForSelectedConnection() {
    if (actionsLocked()) {
        return;
    }
    const int idx = selectedConnectionIndexForPoolManagement();
    if (idx < 0 || idx >= m_profiles.size()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_poolcrt_auto001"), QStringLiteral("Seleccione una conexión para gestionar pools."),
                QStringLiteral("Select a connection to manage pools."),
                QStringLiteral("请选择一个连接来管理池。")));
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    const bool isMacConn = p.osType.trimmed().toLower().contains(QStringLiteral("mac"));
    beginUiBusy();
    struct UiBusyGuard {
        MainWindow* w{nullptr};
        bool active{false};
        ~UiBusyGuard() {
            if (active && w) {
                w->endUiBusy();
            }
        }
        void release() {
            if (active && w) {
                w->endUiBusy();
                active = false;
            }
        }
    } busyGuard{this, true};

    struct DeviceEntry {
        QString path;
        QString resolvedPath;
        QString byIdAlias;
        QString size;
        QString mountpoint;
        QString fsType;
        QString devType;
        bool inPool{false};
        bool mounted{false};
        bool childBusy{false};
    };

    auto runRemote = [this, &p](const QString& cmd, int timeoutMs, QString& outText) -> bool {
        QString err;
        int rc = -1;
        outText.clear();
        if (!runSsh(p, cmd, timeoutMs, outText, err, rc) || rc != 0) {
            return false;
        }
        return true;
    };
    QStringList compatibilityNames;
    {
        QString compOut;
        QString compCmd;
        if (isWindowsConnection(p)) {
            compCmd = QStringLiteral(
                "$ErrorActionPreference='SilentlyContinue'; "
                "$dirs=@("
                "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\share\\\\zfs\\\\compatibility.d',"
                "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\compatibility.d',"
                "'C:\\\\msys64\\\\usr\\\\share\\\\zfs\\\\compatibility.d'"
                "); "
                "foreach($d in $dirs){ if(Test-Path -LiteralPath $d){ Get-ChildItem -LiteralPath $d -File | ForEach-Object { $_.Name } } }");
        } else {
            compCmd = QStringLiteral(
                "for d in /etc/zfs/compatibility.d /usr/share/zfs/compatibility.d /usr/local/zfs/share/zfs/compatibility.d; do "
                "  [ -d \"$d\" ] || continue; "
                "  for f in \"$d\"/*; do [ -f \"$f\" ] || continue; basename \"$f\"; done; "
                "done | sort -u");
        }
        if (runRemote(compCmd, 15000, compOut)) {
            const QStringList lines = compOut.split('\n', Qt::SkipEmptyParts);
            for (QString n : lines) {
                n = n.trimmed();
                if (!n.isEmpty()) {
                    compatibilityNames << n;
                }
            }
            compatibilityNames.removeDuplicates();
            std::sort(compatibilityNames.begin(), compatibilityNames.end());
        }
    }

    QMap<QString, DeviceEntry> devicesByPath;
    QString out;
    QString devCmd;
    if (isWindowsConnection(p)) {
        devCmd = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "Get-Partition | "
            "ForEach-Object { "
            "  $path = ('\\\\.\\PhysicalDrive' + $_.DiskNumber + '\\\\Partition' + $_.PartitionNumber); "
            "  $size = if($_.Size){ [string]([math]::Round($_.Size/1GB,2)) + 'G' } else { '-' }; "
            "  $mp = '-'; "
            "  if ($_.DriveLetter) { $mp = ($_.DriveLetter + ':\\') } "
            "  $vol = Get-Volume -Partition $_ -ErrorAction SilentlyContinue; "
            "  $fs = '-'; if($vol -and $vol.FileSystem){ $fs = [string]$vol.FileSystem }; "
            "  $gpt = '-'; if($_.GptType){ $gpt = [string]$_.GptType }; "
            "  $ptypeRaw = '-'; if($_.Type){ $ptypeRaw = [string]$_.Type }; "
            "  $mbr = '-'; if($_.MbrType){ $mbr = [string]$_.MbrType }; "
            "  $ptype = ($fs + '|gpt=' + $gpt + '|type=' + $ptypeRaw + '|mbr=' + $mbr); "
            "  Write-Output ($path + \"`t\" + $size + \"`t\" + $mp + \"`t\" + $path + \"`t\" + $ptype + \"`tpart\") "
            "}");
    } else {
        devCmd = QStringLiteral(
            "if [ \"$(uname -s 2>/dev/null)\" = \"Darwin\" ] && [ -d /private/var/run/disk/by-id ]; then "
            "  for id in /private/var/run/disk/by-id/*; do "
            "    [ -e \"$id\" ] || continue; "
            "    real=\"$(perl -MCwd=realpath -e 'print realpath($ARGV[0])' \"$id\" 2>/dev/null)\"; "
            "    [ -n \"$real\" ] || continue; "
            "    base=\"$(basename \"$real\")\"; "
            "    parent=\"${base%%s*}\"; "
            "    if diskutil list | awk -v d=\"$parent\" '$1==\"/dev/\"d { if (index(tolower($0),\"synthesized\")>0) { found=1 } } END{ exit(found?0:1) }'; then "
            "      continue; "
            "    fi; "
            "    info=\"$(diskutil info \"$real\" 2>/dev/null || true)\"; "
            "    ptype=\"$(diskutil list | awk -v id=\"$base\" '$NF==id { t=$2; if($2==\"APFS\" && ($3==\"Volume\" || $3==\"Snapshot\")){ t=$2\" \"$3 } print t; exit }')\"; "
            "    [ -n \"$ptype\" ] || ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Partition Type:/ {print $2; exit}')\"; "
            "    [ -n \"$ptype\" ] || ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Type \\(Bundle\\):/ {print $2; exit}')\"; "
            "    [ -n \"$ptype\" ] || ptype='-'; "
            "    mnt=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Mount Point:/ {print $2; exit}')\"; "
            "    [ -n \"$mnt\" ] || mnt='-'; "
            "    size=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Disk Size:/ {print $2; exit}')\"; "
            "    [ -n \"$size\" ] || size='-'; "
            "    printf '%s\\t%s\\t%s\\t%s\\t%s\\tpart\\n' \"$id\" \"$size\" \"$mnt\" \"$real\" \"$ptype\"; "
            "  done | awk -F '\\t' '!seen[$4]++'; "
            "elif command -v lsblk >/dev/null 2>&1; then "
            "  lsblk -fpPno NAME,SIZE,FSTYPE,MOUNTPOINTS,TYPE; "
            "elif command -v diskutil >/dev/null 2>&1; then "
            "  diskutil list | awk '"
            "    /^\\/dev\\/disk[0-9]+/ { "
            "      current=$1; sub(/:$/, \"\", current); "
            "      synth=(index(tolower($0),\"synthesized\")>0); "
            "      next; "
            "    } "
            "    { "
            "      id=$NF; "
            "      if(id ~ /^disk[0-9]+s[0-9]+$/){ "
            "        if(synth){ next; } "
            "        t=$2; "
            "        if(t==\"APFS\" && ($3==\"Volume\" || $3==\"Snapshot\")){ t=t\" \"$3 } "
            "        print \"/dev/\"id\"\\t-\\t-\\t/dev/\"id\"\\t\"t\"\\tpart\"; "
            "      } "
            "    }"
            "  '; "
            "else "
            "  for d in /dev/sd?* /dev/vd?* /dev/xvd?* /dev/nvme*n* /dev/disk?s*; do [ -e \"$d\" ] && printf \"%s\\t-\\t-\\t%s\\t-\\tpart\\n\" \"$d\" \"$d\"; done; "
            "fi");
    }
    if (runRemote(devCmd, 25000, out)) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        QSet<QString> dedupeKeys;
        auto parseKv = [](const QString& line, const QString& key) -> QString {
            const QRegularExpression rx(QStringLiteral("\\b%1=\"([^\"]*)\"").arg(QRegularExpression::escape(key)));
            const QRegularExpressionMatch m = rx.match(line);
            return m.hasMatch() ? m.captured(1) : QString();
        };
        for (const QString& line : lines) {
            QString path;
            QString size;
            QString mp;
            QString resolved;
            QString fsType;
            QString type;
            if (line.contains(QStringLiteral("NAME=\"")) && line.contains(QStringLiteral("TYPE=\""))) {
                path = parseKv(line, QStringLiteral("NAME")).trimmed();
                size = parseKv(line, QStringLiteral("SIZE")).trimmed();
                fsType = parseKv(line, QStringLiteral("FSTYPE")).trimmed();
                mp = parseKv(line, QStringLiteral("MOUNTPOINTS")).trimmed();
                type = parseKv(line, QStringLiteral("TYPE")).trimmed().toLower();
                resolved = path;
                if (type != QStringLiteral("disk") && type != QStringLiteral("part")) {
                    continue;
                }
            } else {
                const QStringList cols = line.split('\t');
                if (cols.isEmpty()) {
                    continue;
                }
                path = cols.value(0).trimmed();
                size = cols.value(1).trimmed();
                mp = cols.value(2).trimmed();
                resolved = cols.value(3).trimmed();
                fsType = cols.value(4).trimmed();
                type = cols.value(5).trimmed().toLower();
                if (type.isEmpty()) {
                    type = QStringLiteral("part");
                }
            }
            if (path.isEmpty()) {
                continue;
            }
            DeviceEntry e;
            e.resolvedPath = resolved.isEmpty() ? path : resolved;
            e.path = path;
            if (path.startsWith(QStringLiteral("/private/var/run/disk/by-id/"))) {
                e.byIdAlias = QFileInfo(path).fileName();
            }
            if (e.path.startsWith(QStringLiteral("/private/var/run/disk/by-id/"))
                && !e.resolvedPath.isEmpty()) {
                e.path = e.resolvedPath;
            }
            e.size = size.isEmpty() ? QStringLiteral("-") : size;
            if (isMacConn && e.size != QStringLiteral("-")) {
                const QRegularExpression gbRx(QStringLiteral("(\\d+[\\.,]?\\d*)\\s*GB"),
                                              QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch mm = gbRx.match(e.size);
                if (mm.hasMatch()) {
                    e.size = mm.captured(1) + QStringLiteral(" GB");
                } else {
                    const QRegularExpression numRx(QStringLiteral("(\\d+[\\.,]?\\d*)"));
                    const QRegularExpressionMatch m2 = numRx.match(e.size);
                    if (m2.hasMatch()) {
                        e.size = m2.captured(1) + QStringLiteral(" GB");
                    }
                }
            }
            e.mountpoint = mp.isEmpty() ? QStringLiteral("-") : mp;
            e.fsType = fsType.isEmpty() ? QStringLiteral("-") : fsType;
            if (isWindowsConnection(p)) {
                e.fsType = formatWindowsFsTypeDetail(e.fsType);
            }
            e.devType = type.isEmpty() ? QStringLiteral("part") : type;
            QString fsLower = e.fsType.trimmed().toLower();
            fsLower.remove('{');
            fsLower.remove('}');
            if (e.fsType.compare(QStringLiteral("zfs_member"), Qt::CaseInsensitive) == 0
                || fsLower.contains(QStringLiteral("zfs"))
                || fsLower.contains(QStringLiteral("6a945a3b-1dd2-11b2-99a6-080020736631"))
                || fsLower.contains(QStringLiteral("6a898cc3-1dd2-11b2-99a6-080020736631"))) {
                e.inPool = true;
            }
            const QString dedupeKey = (e.resolvedPath.isEmpty() ? e.path : e.resolvedPath).trimmed();
            if (!dedupeKey.isEmpty()) {
                if (dedupeKeys.contains(dedupeKey)) {
                    continue;
                }
                dedupeKeys.insert(dedupeKey);
            }
            devicesByPath[path] = e;
        }
    }

    auto collectPoolTokens = [](const QString& text) -> QSet<QString> {
        QSet<QString> usedTokens;
        static const QSet<QString> skip = {
            QStringLiteral("NAME"), QStringLiteral("MIRROR"), QStringLiteral("RAIDZ"), QStringLiteral("RAIDZ1"),
            QStringLiteral("RAIDZ2"), QStringLiteral("RAIDZ3"), QStringLiteral("SPARE"),
            QStringLiteral("LOGS"), QStringLiteral("CACHE"), QStringLiteral("SPECIAL"), QStringLiteral("DEDUP"),
            QStringLiteral("ONLINE"), QStringLiteral("OFFLINE"), QStringLiteral("UNAVAIL"),
            QStringLiteral("UNAVAILABLE"), QStringLiteral("DEGRADED"), QStringLiteral("FAULTED"),
            QStringLiteral("REMOVED"), QStringLiteral("AVAIL")
        };
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            if (trimmed.startsWith(QStringLiteral("pool:")) || trimmed.startsWith(QStringLiteral("state:"))
                || trimmed.startsWith(QStringLiteral("scan:")) || trimmed.startsWith(QStringLiteral("config:"))
                || trimmed.startsWith(QStringLiteral("errors:")) || trimmed.startsWith(QStringLiteral("status:"))
                || trimmed.startsWith(QStringLiteral("action:")) || trimmed.startsWith(QStringLiteral("see:"))
                || trimmed == QStringLiteral("NAME")) {
                continue;
            }
            const QString token = trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).value(0).trimmed();
            if (token.isEmpty()) {
                continue;
            }
            if (skip.contains(token.toUpper()) || token.endsWith(':') || token.endsWith('-')) {
                continue;
            }
            usedTokens.insert(token);
            if (token.startsWith('/')) {
                usedTokens.insert(QFileInfo(token).fileName());
            }
            const QRegularExpression pdRx(QStringLiteral("PhysicalDrive\\d+"), QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch m = pdRx.match(token);
            if (m.hasMatch()) {
                usedTokens.insert(m.captured(0));
            }
        }
        return usedTokens;
    };
    auto normalizeWinPhysPart = [](const QString& raw) -> QString {
        const QString s = raw.trimmed();
        if (s.isEmpty()) {
            return QString();
        }
        static const QRegularExpression rx(
            QStringLiteral("physicaldrive\\s*(\\d+)(?:\\D+partition\\s*(\\d+))?"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = rx.match(s);
        if (!m.hasMatch()) {
            return QString();
        }
        const QString d = m.captured(1);
        const QString partNo = m.captured(2);
        return partNo.isEmpty() ? QStringLiteral("physicaldrive%1").arg(d)
                                : QStringLiteral("physicaldrive%1/partition%2").arg(d, partNo);
    };
    auto markDevicesInPoolByTokens = [&devicesByPath, &normalizeWinPhysPart](const QSet<QString>& usedTokens) {
        if (usedTokens.isEmpty()) {
            return;
        }
        for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
            const QString pth = it.value().path.trimmed();
            const QString real = it.value().resolvedPath.trimmed();
            const QString alias = it.value().byIdAlias.trimmed();
            const QString pthBase = QFileInfo(pth).fileName();
            const QString realBase = QFileInfo(real).fileName();
            const QString pthLower = pth.toLower();
            const QString realLower = real.toLower();
            const QString pthNorm = normalizeWinPhysPart(pthLower);
            const QString realNorm = normalizeWinPhysPart(realLower);
            for (const QString& tkRaw : usedTokens) {
                const QString tk = tkRaw.trimmed();
                if (tk.isEmpty()) {
                    continue;
                }
                const QString tkLower = tk.toLower();
                const QString tkNorm = normalizeWinPhysPart(tkLower);
                if (tk == pth || tk == real || tk == alias
                    || tk == pthBase || tk == realBase
                    || pthLower.contains(tkLower) || realLower.contains(tkLower)
                    || (!tkNorm.isEmpty() && (tkNorm == pthNorm || tkNorm == realNorm))) {
                    it.value().inPool = true;
                    break;
                }
            }
        }
    };

    auto countInPool = [&devicesByPath]() -> int {
        int n = 0;
        for (auto it = devicesByPath.cbegin(); it != devicesByPath.cend(); ++it) {
            if (it.value().inPool) {
                ++n;
            }
        }
        return n;
    };
    auto markPhysicalDriveWholeDisk = [&devicesByPath](const QString& text) {
        static const QRegularExpression pdRx(QStringLiteral("physicaldrive\\s*(\\d+)"),
                                             QRegularExpression::CaseInsensitiveOption);
        QSet<QString> disks;
        auto it = pdRx.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (m.hasMatch()) {
                disks.insert(m.captured(1));
            }
        }
        if (disks.isEmpty()) {
            return;
        }
        for (auto itDev = devicesByPath.begin(); itDev != devicesByPath.end(); ++itDev) {
            const QString blob = (itDev.value().path + QStringLiteral(" ") + itDev.value().resolvedPath).toLower();
            for (const QString& d : disks) {
                if (blob.contains(QStringLiteral("physicaldrive%1").arg(d))) {
                    itDev.value().inPool = true;
                    break;
                }
            }
        }
    };

    out.clear();
    const QString inPoolCmd = withSudo(p, QStringLiteral(
        "(zpool status -LP 2>/dev/null || zpool status -P 2>/dev/null || zpool status -L 2>/dev/null || zpool status 2>/dev/null)"));
    if (runRemote(inPoolCmd, 25000, out)) {
        markDevicesInPoolByTokens(collectPoolTokens(out));
        if (isWindowsConnection(p)) {
            markPhysicalDriveWholeDisk(out);
        }
    }
    QString outListV;
    const QString listVCmd = withSudo(p, QStringLiteral("(zpool list -v -P 2>/dev/null || zpool list -v 2>/dev/null)"));
    if (runRemote(listVCmd, 25000, outListV)) {
        markDevicesInPoolByTokens(collectPoolTokens(outListV));
        if (isWindowsConnection(p)) {
            markPhysicalDriveWholeDisk(outListV);
        }
    }
    // Windows: explicit ZFS GPT signature scan (covers imported pools when names do not map cleanly).
    if (isWindowsConnection(p)) {
        QString zfsPartOut;
        const QString zfsPartCmd = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "Get-Partition | "
            "Where-Object { $_.GptType -and @('6a945a3b-1dd2-11b2-99a6-080020736631','6a898cc3-1dd2-11b2-99a6-080020736631') -contains $_.GptType.ToString().Trim('{}').ToLower() } | "
            "ForEach-Object { Write-Output ('\\\\.\\PhysicalDrive' + $_.DiskNumber + '\\\\Partition' + $_.PartitionNumber) }");
        if (runRemote(zfsPartCmd, 20000, zfsPartOut)) {
            QSet<QString> zfsTokens;
            const QStringList lines = zfsPartOut.split('\n', Qt::SkipEmptyParts);
            for (const QString& raw : lines) {
                const QString token = raw.trimmed();
                if (!token.isEmpty()) {
                    zfsTokens.insert(token);
                    zfsTokens.insert(token.toLower());
                }
            }
            markDevicesInPoolByTokens(zfsTokens);
        }
    }
    // Also probe importable (not imported) pools and mark their devices as in-pool.
    out.clear();
    QString outImp;
    const QString importProbeCmd = withSudo(p, QStringLiteral("(zpool import 2>/dev/null; zpool import -s 2>/dev/null)"));
    if (runRemote(importProbeCmd, 25000, outImp)) {
        markDevicesInPoolByTokens(collectPoolTokens(outImp));
        if (isWindowsConnection(p)) {
            markPhysicalDriveWholeDisk(outImp);
        }
    }

    if (isWindowsConnection(p)) {
        const int marked = countInPool();
        if (marked == 0) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("%1: no device marked IN_POOL while probing zpool output in pool-create dialog")
                       .arg(p.name));
        }
    }

    out.clear();
    QString mountedCmd;
    if (isWindowsConnection(p)) {
        mountedCmd = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "Get-CimInstance Win32_LogicalDisk | "
            "Where-Object { $_.DriveType -eq 3 } | "
            "ForEach-Object { Write-Output ($_.DeviceID + \"`t\" + $_.DeviceID + \"\\\\\") }");
    } else {
        mountedCmd = QStringLiteral("mount | awk '{print $1\"\\t\"$3}'");
    }
    if (runRemote(mountedCmd, 20000, out)) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList cols = line.split('\t');
            if (cols.isEmpty()) {
                continue;
            }
            const QString dev = cols.value(0).trimmed();
            const QString mp = cols.value(1).trimmed();
            if (dev.isEmpty()) {
                continue;
            }
            for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
                const QString real = it.value().resolvedPath.isEmpty() ? it.value().path : it.value().resolvedPath;
                if (it.value().path == dev || real == dev) {
                    it.value().mounted = true;
                    if (!mp.isEmpty()) {
                        it.value().mountpoint = mp;
                    }
                }
            }
        }
    }

    // Propagate partition usage to parent disk:
    // if /dev/disk0s1 or /dev/sda1 is mounted/in-pool, /dev/disk0 or /dev/sda must not appear free.
    const QStringList allPaths = devicesByPath.keys();
    for (const QString& path : allPaths) {
        const DeviceEntry child = devicesByPath.value(path);
        if (!child.mounted && !child.inPool) {
            continue;
        }
        const QString childReal = child.resolvedPath.isEmpty() ? child.path : child.resolvedPath;
        const QString parent = parentDiskDevicePath(childReal);
        if (parent.isEmpty()) {
            continue;
        }
        for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
            const QString real = it.value().resolvedPath.isEmpty() ? it.value().path : it.value().resolvedPath;
            if (it.value().path == parent || real == parent) {
                it.value().childBusy = true;
            }
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_poolcrt_auto002"), QStringLiteral("Crear pool en %1"), QStringLiteral("Create pool on %1"), QStringLiteral("在 %1 创建池"))
            .arg(p.name));
    dlg.setModal(true);
    dlg.resize(1180, 760);
    auto* lay = new QVBoxLayout(&dlg);

    auto* body = new QHBoxLayout();
    body->setSpacing(8);
    auto* leftPane = new QWidget(&dlg);
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(8);

    auto* baseBox = new QGroupBox(
        trk(QStringLiteral("t_poolcrt_auto003"), QStringLiteral("Parámetros del pool"), QStringLiteral("Pool parameters"), QStringLiteral("池参数")), leftPane);
    auto* form = new QFormLayout(baseBox);
    QLineEdit* poolNameEd = new QLineEdit(baseBox);
    QComboBox* quickLayoutCb = new QComboBox(baseBox);
    quickLayoutCb->addItems({QStringLiteral("stripe"), QStringLiteral("mirror"), QStringLiteral("raidz"),
                             QStringLiteral("raidz2"), QStringLiteral("raidz3")});
    QCheckBox* forceCb = new QCheckBox(QStringLiteral("-f"), baseBox);
    QCheckBox* dryRunCb = new QCheckBox(QStringLiteral("-n"), baseBox);
    QLineEdit* mountpointEd = new QLineEdit(baseBox);
    QLineEdit* altrootEd = new QLineEdit(baseBox);
    QComboBox* ashiftCb = new QComboBox(baseBox);
    ashiftCb->addItems({QString(), QStringLiteral("9"), QStringLiteral("10"), QStringLiteral("11"),
                        QStringLiteral("12"), QStringLiteral("13"), QStringLiteral("14"),
                        QStringLiteral("15"), QStringLiteral("16")});
    QComboBox* autotrimCb = new QComboBox(baseBox);
    autotrimCb->addItems({QString(), QStringLiteral("off"), QStringLiteral("on")});
    QComboBox* autoexpandCb = new QComboBox(baseBox);
    autoexpandCb->addItems({QString(), QStringLiteral("off"), QStringLiteral("on")});
    QComboBox* compatibilityCb = new QComboBox(baseBox);
    compatibilityCb->setEditable(false);
    compatibilityCb->addItem(QString());
    compatibilityCb->addItem(QStringLiteral("off"));
    compatibilityCb->addItem(QStringLiteral("legacy"));
    for (const QString& c : compatibilityNames) {
        if (compatibilityCb->findText(c) < 0) {
            compatibilityCb->addItem(c);
        }
    }
    QLineEdit* bootfsEd = new QLineEdit(baseBox);
    QLineEdit* poolOptsEd = new QLineEdit(baseBox);
    QLineEdit* fsPropsEd = new QLineEdit(baseBox);
    QLineEdit* extraEd = new QLineEdit(baseBox);
    const ZPoolCreationDefaults zdefs = loadZPoolCreationDefaults(m_store.iniPath());
    forceCb->setChecked(zdefs.force);
    altrootEd->setText(zdefs.altroot);
    ashiftCb->setCurrentText(zdefs.ashift);
    autotrimCb->setCurrentText(zdefs.autotrim);
    if (!zdefs.compatibility.isEmpty() && compatibilityCb->findText(zdefs.compatibility) < 0) {
        compatibilityCb->addItem(zdefs.compatibility);
    }
    compatibilityCb->setCurrentText(zdefs.compatibility);
    fsPropsEd->setText(zdefs.fsProps);
    form->addRow(trk(QStringLiteral("t_poolcrt_auto004"), QStringLiteral("Nombre"), QStringLiteral("Name"), QStringLiteral("名称")), poolNameEd);
    form->addRow(trk(QStringLiteral("t_poolcrt_auto005"), QStringLiteral("Tipo rápido (si no hay spec vdev)"),
                     QStringLiteral("Quick type (if no vdev spec)"),
                     QStringLiteral("快速类型（若无 vdev 规格）")),
                 quickLayoutCb);
    auto* flagsRow = new QHBoxLayout();
    flagsRow->addWidget(forceCb);
    flagsRow->addWidget(dryRunCb);
    flagsRow->addStretch(1);
    form->addRow(trk(QStringLiteral("t_poolcrt_auto006"), QStringLiteral("Flags"), QStringLiteral("Flags"), QStringLiteral("标志")), flagsRow);
    form->addRow(QStringLiteral("mountpoint (-m)"), mountpointEd);
    form->addRow(QStringLiteral("altroot (-R)"), altrootEd);
    form->addRow(QStringLiteral("ashift (-o ashift=)"), ashiftCb);
    form->addRow(QStringLiteral("autotrim (-o autotrim=)"), autotrimCb);
    form->addRow(QStringLiteral("autoexpand (-o autoexpand=)"), autoexpandCb);
    form->addRow(QStringLiteral("compatibility (-o compatibility=)"), compatibilityCb);
    form->addRow(QStringLiteral("bootfs (-o bootfs=)"), bootfsEd);
    form->addRow(QStringLiteral("-o (coma: k=v,k=v)"), poolOptsEd);
    form->addRow(QStringLiteral("-O (coma: k=v,k=v)"), fsPropsEd);
    form->addRow(QStringLiteral("extra args"), extraEd);
    leftLay->addWidget(baseBox, 0);

    auto* vdevBox =
        new QGroupBox(trk(QStringLiteral("t_poolcrt_auto007"), QStringLiteral("Constructor de VDEV"),
                          QStringLiteral("VDEV builder"),
                          QStringLiteral("VDEV 构建器")),
                      leftPane);
    auto* vdevLay = new QVBoxLayout(vdevBox);
    auto* row1 = new QHBoxLayout();
    QComboBox* vdevClassCb = new QComboBox(vdevBox);
    vdevClassCb->addItem(QStringLiteral("data"));
    vdevClassCb->addItem(QStringLiteral("special"));
    vdevClassCb->addItem(QStringLiteral("log"));
    vdevClassCb->addItem(QStringLiteral("cache"));
    vdevClassCb->addItem(QStringLiteral("dedup"));
    vdevClassCb->addItem(QStringLiteral("spare"));
    QComboBox* vdevTypeCb = new QComboBox(vdevBox);
    vdevTypeCb->addItems({QStringLiteral("stripe"), QStringLiteral("mirror"), QStringLiteral("raidz"),
                          QStringLiteral("raidz2"), QStringLiteral("raidz3")});
    auto* addVdevBtn =
        new QPushButton(trk(QStringLiteral("t_poolcrt_auto008"), QStringLiteral("Añadir con seleccionados"),
                            QStringLiteral("Add from selected"),
                            QStringLiteral("从已选添加")),
                        vdevBox);
    row1->addWidget(new QLabel(trk(QStringLiteral("t_poolcrt_auto009"), QStringLiteral("Clase"), QStringLiteral("Class"), QStringLiteral("类别")), vdevBox));
    row1->addWidget(vdevClassCb);
    row1->addWidget(new QLabel(trk(QStringLiteral("t_poolcrt_auto010"), QStringLiteral("Tipo"), QStringLiteral("Type"), QStringLiteral("类型")), vdevBox));
    row1->addWidget(vdevTypeCb);
    row1->addWidget(addVdevBtn, 1);
    vdevLay->addLayout(row1);
    auto* row2 = new QHBoxLayout();
    auto* clearSelBtn = new QPushButton(trk(QStringLiteral("t_poolcrt_auto011"), QStringLiteral("Limpiar selección dispositivos"),
                                            QStringLiteral("Clear device selection"),
                                            QStringLiteral("清除设备选择")),
                                        vdevBox);
    auto* clearSpecBtn =
        new QPushButton(trk(QStringLiteral("t_poolcrt_auto012"), QStringLiteral("Limpiar spec"), QStringLiteral("Clear spec"), QStringLiteral("清除规格")), vdevBox);
    row2->addWidget(clearSelBtn);
    row2->addWidget(clearSpecBtn);
    row2->addStretch(1);
    vdevLay->addLayout(row2);
    auto* vdevHelp =
        new QLabel(trk(QStringLiteral("t_poolcrt_auto013"), QStringLiteral("Puede construir algo como: raidz2 d1 d2 d3 d4 d5 d6 | raidz2 d7 d8 d9 d10 d11 d12 | special mirror nvme0 nvme1 | log mirror nvme2 nvme3 | cache nvme4 | dedup mirror nvme5 nvme6 | spare d13"),
                       QStringLiteral("Build specs like: raidz2 d1 d2 d3 d4 d5 d6 | raidz2 d7 d8 d9 d10 d11 d12 | special mirror nvme0 nvme1 | log mirror nvme2 nvme3 | cache nvme4 | dedup mirror nvme5 nvme6 | spare d13"),
                       QStringLiteral("可构建如下规格：raidz2 d1 d2 d3 d4 d5 d6 | raidz2 d7 d8 d9 d10 d11 d12 | special mirror nvme0 nvme1 | log mirror nvme2 nvme3 | cache nvme4 | dedup mirror nvme5 nvme6 | spare d13")),
                   vdevBox);
    vdevHelp->setWordWrap(true);
    vdevLay->addWidget(vdevHelp, 0);
    QPlainTextEdit* vdevSpecEdit = new QPlainTextEdit(vdevBox);
    vdevSpecEdit->setPlaceholderText(
        trk(QStringLiteral("t_poolcrt_auto014"), QStringLiteral("Una línea por grupo o use '|' en una línea.\nEjemplo:\nraidz2 /dev/sda /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf\nraidz2 /dev/sdg /dev/sdh /dev/sdi /dev/sdj /dev/sdk /dev/sdl\nspecial mirror /dev/nvme0n1 /dev/nvme1n1\nlog mirror /dev/nvme2n1 /dev/nvme3n1\ncache /dev/nvme4n1\ndedup mirror /dev/nvme5n1 /dev/nvme6n1\nspare /dev/sdm"),
            QStringLiteral("One line per group or use '|' in one line.\nExample:\nraidz2 /dev/sda /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf\nraidz2 /dev/sdg /dev/sdh /dev/sdi /dev/sdj /dev/sdk /dev/sdl\nspecial mirror /dev/nvme0n1 /dev/nvme1n1\nlog mirror /dev/nvme2n1 /dev/nvme3n1\ncache /dev/nvme4n1\ndedup mirror /dev/nvme5n1 /dev/nvme6n1\nspare /dev/sdm"),
            QStringLiteral("每行一个组，或在一行里用“|”。\n示例：\nraidz2 /dev/sda /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf\nraidz2 /dev/sdg /dev/sdh /dev/sdi /dev/sdj /dev/sdk /dev/sdl\nspecial mirror /dev/nvme0n1 /dev/nvme1n1\nlog mirror /dev/nvme2n1 /dev/nvme3n1\ncache /dev/nvme4n1\ndedup mirror /dev/nvme5n1 /dev/nvme6n1\nspare /dev/sdm")));
    vdevLay->addWidget(vdevSpecEdit, 1);
    leftLay->addWidget(vdevBox, 1);

    auto* devicesBox = new QGroupBox(
        trk(QStringLiteral("t_poolcrt_auto015"), QStringLiteral("Block devices disponibles"),
            QStringLiteral("Available block devices"),
            QStringLiteral("可用块设备")),
        leftPane);
    auto* devicesLayout = new QVBoxLayout(devicesBox);
    QTableWidget* devicesTable = new QTableWidget(devicesBox);
    devicesTable->setColumnCount(6);
    devicesTable->setHorizontalHeaderLabels({
        trk(QStringLiteral("t_poolcrt_auto016"), QStringLiteral("Usar"), QStringLiteral("Use"), QStringLiteral("使用")),
        trk(QStringLiteral("t_poolcrt_auto017"), QStringLiteral("Device"), QStringLiteral("Device"), QStringLiteral("设备")),
        trk(QStringLiteral("t_poolcrt_auto018"), QStringLiteral("Tamaño"), QStringLiteral("Size"), QStringLiteral("大小")),
        trk(QStringLiteral("t_poolcrt_auto019"), QStringLiteral("Mount"), QStringLiteral("Mount"), QStringLiteral("挂载")),
        trk(QStringLiteral("t_poolcrt_auto020"), QStringLiteral("Estado"), QStringLiteral("State"), QStringLiteral("状态")),
        trk(QStringLiteral("t_poolcrt_auto021"), QStringLiteral("Detalle"), QStringLiteral("Detail"), QStringLiteral("详情")),
    });
    devicesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    devicesTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    devicesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    devicesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    devicesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    devicesTable->setWordWrap(true);
    devicesTable->setTextElideMode(Qt::ElideNone);
    devicesTable->verticalHeader()->setVisible(false);
    devicesTable->verticalHeader()->setDefaultSectionSize(22);
    const QColor stRed("#ffcccc");
    const QColor stYellow("#fff5bf");
    const QColor stGreen("#d7f7d7");

    struct DeviceRenderRow {
        DeviceEntry entry;
        QString stateText;
        QString detailText;
        QColor bgColor;
        int colorRank{2}; // 0=green, 1=yellow, 2=red
        bool selectable{true};
    };
    QVector<DeviceRenderRow> renderRows;
    renderRows.reserve(devicesByPath.size());
    QSet<QString> yellowDevicePaths;
    QStringList paths = devicesByPath.keys();
    auto hasProtectedSystemMount = [](const DeviceEntry& e) -> bool {
        const QString fs = e.fsType.trimmed().toLower();
        if (windowsPartitionTypeIsProtected(e.fsType)) {
            return true;
        }
        if (fs == QStringLiteral("swap")) {
            return true;
        }
        static const QSet<QString> macSystemTypes = {
            QStringLiteral("apple_apfs_isc"),
            QStringLiteral("apple_apfs"),
            QStringLiteral("efi"),
            QStringLiteral("apfs volume"),
            QStringLiteral("apfs snapshot"),
            QStringLiteral("apple_apfs_recovery"),
        };
        if (macSystemTypes.contains(fs)) {
            return true;
        }
        // macOS: classify any APFS* partition as SYSTEM (container volumes, snapshots, recovery, etc.).
        if (fs.contains(QStringLiteral("apfs"))) {
            return true;
        }
        QString mp = e.mountpoint;
        mp.replace(QStringLiteral("\\n"), QStringLiteral(" "));
        mp.replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QStringList toks = mp.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
        for (const QString& tRaw : toks) {
            const QString t = tRaw.trimmed();
            const QString tw = t.toUpper();
            // Windows: anything mounted on C: is system/protected and must stay red.
            if (tw == QStringLiteral("C:")
                || tw == QStringLiteral("C:\\")
                || tw.startsWith(QStringLiteral("C:\\"))) {
                return true;
            }
            if (t == QStringLiteral("/") || t == QStringLiteral("/boot") || t == QStringLiteral("/boot/efi")
                || t == QStringLiteral("[SWAP]") || t.startsWith(QStringLiteral("/System"))) {
                return true;
            }
        }
        return false;
    };
    for (const QString& path : paths) {
        const DeviceEntry e = devicesByPath.value(path);
        const QString realPath = e.resolvedPath.isEmpty() ? e.path : e.resolvedPath;
        if (e.devType == QStringLiteral("disk") || isRootDevicePath(e.path) || isRootDevicePath(realPath)) {
            continue;
        }
        DeviceRenderRow rr;
        rr.entry = e;
        const bool protectedMount = hasProtectedSystemMount(e);
        if (protectedMount || e.inPool || e.childBusy) {
            rr.stateText = trk(QStringLiteral("t_poolcrt_auto022"), QStringLiteral("EN_POOL"), QStringLiteral("IN_POOL"), QStringLiteral("在池中"));
            if (protectedMount) {
                rr.stateText = trk(QStringLiteral("t_poolcrt_auto023"), QStringLiteral("SISTEMA"), QStringLiteral("SYSTEM"), QStringLiteral("系统"));
                rr.detailText = trk(QStringLiteral("t_poolcrt_auto024"), QStringLiteral("Dispositivo de sistema (/ /boot /boot/efi o SWAP)"),
                                    QStringLiteral("System device (/ /boot /boot/efi or SWAP)"),
                                    QStringLiteral("系统设备（/ /boot /boot/efi 或 SWAP）"));
            } else if (e.childBusy) {
                rr.detailText = trk(QStringLiteral("t_poolcrt_auto025"), QStringLiteral("Alguna partición está montada o pertenece a un pool"),
                                    QStringLiteral("A child partition is mounted or belongs to a pool"),
                                    QStringLiteral("某个子分区已挂载或属于池"));
            } else {
                rr.detailText = trk(QStringLiteral("t_poolcrt_auto026"), QStringLiteral("Ya pertenece a un pool"),
                                    QStringLiteral("Already part of a pool"),
                                    QStringLiteral("已属于某个池"));
            }
            rr.bgColor = stRed;
            rr.colorRank = 2;
            rr.selectable = false;
        } else if (e.mounted || (!e.mountpoint.isEmpty() && e.mountpoint != QStringLiteral("-"))) {
            rr.stateText = trk(QStringLiteral("t_poolcrt_auto027"), QStringLiteral("MONTADO"), QStringLiteral("MOUNTED"), QStringLiteral("已挂载"));
            rr.detailText = e.mountpoint;
            rr.bgColor = stYellow;
            rr.colorRank = 1;
            yellowDevicePaths.insert(e.path);
        } else {
            rr.stateText = trk(QStringLiteral("t_poolcrt_auto028"), QStringLiteral("LIBRE"), QStringLiteral("FREE"), QStringLiteral("空闲"));
            rr.detailText = trk(QStringLiteral("t_poolcrt_auto029"), QStringLiteral("Disponible"), QStringLiteral("Available"), QStringLiteral("可用"));
            rr.bgColor = stGreen;
            rr.colorRank = 0;
        }
        if (!e.fsType.isEmpty() && e.fsType != QStringLiteral("-")) {
            rr.detailText = rr.detailText + QStringLiteral(" | fstype=") + e.fsType;
        }
        if (!e.resolvedPath.isEmpty() && e.resolvedPath != e.path) {
            rr.detailText = rr.detailText + QStringLiteral(" | ") + e.resolvedPath;
        }
        renderRows.push_back(rr);
    }
    std::sort(renderRows.begin(), renderRows.end(), [](const DeviceRenderRow& a, const DeviceRenderRow& b) {
        if (a.colorRank != b.colorRank) {
            return a.colorRank < b.colorRank; // green, yellow, red
        }
        return a.entry.path < b.entry.path;
    });
    for (const DeviceRenderRow& rr : renderRows) {
        const DeviceEntry& e = rr.entry;
        const int row = devicesTable->rowCount();
        devicesTable->insertRow(row);

        auto* useItem = new QTableWidgetItem();
        Qt::ItemFlags uf = (useItem->flags() & ~Qt::ItemIsEditable);
        if (rr.selectable) {
            uf |= Qt::ItemIsUserCheckable;
            useItem->setCheckState(Qt::Unchecked);
        } else {
            uf &= ~Qt::ItemIsUserCheckable;
            useItem->setText(QStringLiteral("—"));
        }
        useItem->setFlags(uf);
        devicesTable->setItem(row, 0, useItem);

        auto* devItem = new QTableWidgetItem(wrapDeviceDisplayText(e.path));
        devItem->setData(Qt::UserRole, e.path);
        devItem->setToolTip(e.path);
        devicesTable->setItem(row, 1, devItem);
        devicesTable->setItem(row, 2, new QTableWidgetItem(e.size));
        devicesTable->setItem(row, 3, new QTableWidgetItem(e.mountpoint));
        auto* stateItem = new QTableWidgetItem(rr.stateText);
        if (!e.fsType.isEmpty() && e.fsType != QStringLiteral("-")) {
            stateItem->setToolTip(QStringLiteral("fstype=%1").arg(e.fsType));
        }
        auto* detailItem = new QTableWidgetItem(rr.detailText);
        devicesTable->setItem(row, 4, stateItem);
        devicesTable->setItem(row, 5, detailItem);
        for (int c = 0; c < devicesTable->columnCount(); ++c) {
            if (QTableWidgetItem* cell = devicesTable->item(row, c)) {
                cell->setBackground(rr.bgColor);
            }
        }
    }
    devicesTable->resizeRowsToContents();
    devicesLayout->addWidget(devicesTable, 1);
    body->addWidget(leftPane, 1);
    body->addWidget(devicesBox, 1);
    lay->addLayout(body, 1);

    auto checkedDevices = [devicesTable]() -> QStringList {
        QStringList selected;
        for (int r = 0; r < devicesTable->rowCount(); ++r) {
            QTableWidgetItem* use = devicesTable->item(r, 0);
            QTableWidgetItem* dev = devicesTable->item(r, 1);
            if (!use || !dev) {
                continue;
            }
            if (use->checkState() == Qt::Checked) {
                QString d = dev->data(Qt::UserRole).toString().trimmed();
                if (d.isEmpty()) {
                    d = dev->text();
                    d.replace('\n', QString());
                    d = d.trimmed();
                }
                if (!d.isEmpty()) {
                    selected << d;
                }
            }
        }
        return selected;
    };
    connect(clearSelBtn, &QPushButton::clicked, &dlg, [devicesTable]() {
        for (int r = 0; r < devicesTable->rowCount(); ++r) {
            if (QTableWidgetItem* use = devicesTable->item(r, 0)) {
                use->setCheckState(Qt::Unchecked);
            }
        }
    });
    connect(clearSpecBtn, &QPushButton::clicked, &dlg, [vdevSpecEdit]() {
        vdevSpecEdit->clear();
    });
    connect(addVdevBtn, &QPushButton::clicked, &dlg, [&]() {
        QStringList selected = checkedDevices();
        if (selected.isEmpty()) {
            QMessageBox::information(
                &dlg, QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_poolcrt_auto030"), QStringLiteral("Seleccione dispositivos en la tabla de la derecha."),
                    QStringLiteral("Select devices in the table on the right."),
                    QStringLiteral("请在右侧表格中选择设备。")));
            return;
        }
        QStringList line;
        const QString klass = vdevClassCb->currentText().trimmed().toLower();
        const QString vtype = vdevTypeCb->currentText().trimmed().toLower();
        if (klass != QStringLiteral("data")) {
            line << klass;
        }
        const bool noRedundancyClass = (klass == QStringLiteral("cache") || klass == QStringLiteral("spare"));
        if (!noRedundancyClass && vtype != QStringLiteral("stripe")) {
            line << vtype;
        }
        for (const QString& d : selected) {
            line << d;
        }
        const QString textLine = line.join(' ');
        QString cur = vdevSpecEdit->toPlainText().trimmed();
        if (!cur.isEmpty()) {
            cur += QLatin1Char('\n');
        }
        cur += textLine;
        vdevSpecEdit->setPlainText(cur);
        for (int r = 0; r < devicesTable->rowCount(); ++r) {
            QTableWidgetItem* use = devicesTable->item(r, 0);
            if (use) {
                use->setCheckState(Qt::Unchecked);
            }
        }
    });

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    busyGuard.release();
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QString poolName = poolNameEd->text().trimmed();
    if (poolName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_poolcrt_auto031"), QStringLiteral("Nombre de pool vacío."),
                                 QStringLiteral("Pool name is empty."),
                                 QStringLiteral("池名称为空。")));
        return;
    }

    QStringList selectedDevices = checkedDevices();

    QStringList parts;
    parts << QStringLiteral("zpool create");
    if (forceCb->isChecked()) {
        parts << QStringLiteral("-f");
    }
    if (dryRunCb->isChecked()) {
        parts << QStringLiteral("-n");
    }
    if (!mountpointEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-m") << shSingleQuote(mountpointEd->text().trimmed());
    }
    if (!altrootEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-R") << shSingleQuote(altrootEd->text().trimmed());
    }
    auto addOpt = [&parts](const QString& key, const QString& value) {
        if (!value.trimmed().isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(key + QStringLiteral("=") + value.trimmed());
        }
    };
    addOpt(QStringLiteral("ashift"), ashiftCb->currentText());
    addOpt(QStringLiteral("autotrim"), autotrimCb->currentText());
    addOpt(QStringLiteral("autoexpand"), autoexpandCb->currentText());
    addOpt(QStringLiteral("compatibility"), compatibilityCb->currentText());
    addOpt(QStringLiteral("bootfs"), bootfsEd->text());
    for (const QString& item : poolOptsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString t = item.trimmed();
        if (!t.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(t);
        }
    }
    for (const QString& item : fsPropsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString t = item.trimmed();
        if (!t.isEmpty()) {
            parts << QStringLiteral("-O") << shSingleQuote(t);
        }
    }

    parts << shSingleQuote(poolName);
    QString vdevSpec = vdevSpecEdit->toPlainText();
    QStringList specLines;
    for (QString ln : vdevSpec.split('\n')) {
        ln = ln.trimmed();
        if (ln.isEmpty()) {
            continue;
        }
        const int hash = ln.indexOf('#');
        if (hash >= 0) {
            ln = ln.left(hash).trimmed();
        }
        if (ln.isEmpty()) {
            continue;
        }
        ln.replace('|', '\n');
        const QStringList splitByPipe = ln.split('\n', Qt::SkipEmptyParts);
        for (QString seg : splitByPipe) {
            seg = seg.trimmed();
            if (!seg.isEmpty()) {
                specLines << seg;
            }
        }
    }
    if (specLines.isEmpty()) {
        if (selectedDevices.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_poolcrt_auto032"), QStringLiteral("Defina una especificación VDEV o seleccione dispositivos para modo rápido."),
                    QStringLiteral("Provide a VDEV spec or select devices for quick mode."),
                    QStringLiteral("请提供 VDEV 规格，或在快速模式中选择设备。")));
            return;
        }
        QStringList quick;
        const QString quickType = quickLayoutCb->currentText().trimmed().toLower();
        if (!quickType.isEmpty() && quickType != QStringLiteral("stripe")) {
            quick << quickType;
        }
        quick.append(selectedDevices);
        specLines << quick.join(' ');
    }
    for (const QString& line : specLines) {
        const QStringList toks = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString& tok : toks) {
            parts << shSingleQuote(tok.trimmed());
        }
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    const QString createCmd = parts.join(' ');
    QString cmd = createCmd;
    if (isMacConn) {
        QStringList selectedYellow;
        for (const QString& d : selectedDevices) {
            if (yellowDevicePaths.contains(d)) {
                selectedYellow << d;
            }
        }
        if (!selectedYellow.isEmpty()) {
            QStringList pre;
            for (const QString& d : selectedYellow) {
                pre << QStringLiteral("diskutil unmount %1").arg(shSingleQuote(d));
            }
            cmd = QStringLiteral("set -e; %1; %2").arg(pre.join(QStringLiteral("; ")), createCmd);
        }
    }
    cmd = withSudo(p, cmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Crear pool"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio crear pool en %1: %2").arg(p.name, poolName));
    setActionsLocked(true);
    QString cmdOut;
    QString cmdErr;
    int rc = -1;
    if (!runSsh(p, cmd, 120000, cmdOut, cmdErr, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error creando pool en %1::%2 -> %3")
                   .arg(p.name, poolName, oneLine(cmdErr.isEmpty() ? QStringLiteral("exit %1").arg(rc) : cmdErr)));
        QMessageBox::critical(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_poolcrt_auto033"), QStringLiteral("Crear pool falló:\n%1"),
                QStringLiteral("Pool creation failed:\n%1"),
                QStringLiteral("创建池失败：\n%1")).arg(cmdErr.isEmpty() ? QStringLiteral("exit %1").arg(rc) : cmdErr));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin crear pool en %1: %2").arg(p.name, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
    populateAllPoolsTables();
    refreshSelectedPoolDetails();
}
