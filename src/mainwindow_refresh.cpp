#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "helperinstallcatalog.h"
#include "agentversion.h"

#include <algorithm>
#include <QElapsedTimer>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>

namespace {
using mwhelpers::oneLine;

QStringList zfsmgrUnixCommandSet() {
    return {
        QStringLiteral("uname"),
        QStringLiteral("sh"),
        QStringLiteral("sudo"),
        QStringLiteral("awk"),
        QStringLiteral("grep"),
        QStringLiteral("sort"),
        QStringLiteral("find"),
        QStringLiteral("mktemp"),
        QStringLiteral("printf"),
        QStringLiteral("cat"),
        QStringLiteral("tar"),
        QStringLiteral("gzip"),
        QStringLiteral("zstd"),
        QStringLiteral("pv"),
        QStringLiteral("rsync"),
        QStringLiteral("ssh"),
        QStringLiteral("zfs"),
        QStringLiteral("zpool"),
    };
}

QStringList zfsmgrPowershellCommandSet() {
    return {
        QStringLiteral("tar"),
        QStringLiteral("robocopy"),
        QStringLiteral("Get-ChildItem"),
        QStringLiteral("Test-Path"),
        QStringLiteral("Resolve-Path"),
        QStringLiteral("New-Item"),
        QStringLiteral("Remove-Item"),
        QStringLiteral("Sort-Object"),
        QStringLiteral("Select-Object"),
        QStringLiteral("Out-String"),
    };
}

QString normalizeMachineUuid(QString s) {
    s = s.trimmed().toLower();
    if (s.startsWith('{') && s.endsWith('}') && s.size() > 2) {
        s = s.mid(1, s.size() - 2);
    }
    return s;
}

QString extractMachineUuid(const QString& text) {
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        return QString();
    }
    const QRegularExpression rxDashed(
        QStringLiteral("([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})"));
    const auto m = rxDashed.match(t);
    if (m.hasMatch()) {
        return normalizeMachineUuid(m.captured(1));
    }
    const QRegularExpression rxCompact(QStringLiteral("([0-9a-fA-F]{32})"));
    const auto m2 = rxCompact.match(t);
    if (m2.hasMatch()) {
        return normalizeMachineUuid(m2.captured(1));
    }
    return normalizeMachineUuid(t.section('\n', 0, 0).trimmed());
}

QMap<QString, QString> parseKeyValueOutput(const QString& text) {
    QMap<QString, QString> out;
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const int eq = raw.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        const QString key = raw.left(eq).trimmed().toUpper();
        const QString value = raw.mid(eq + 1).trimmed();
        if (!key.isEmpty()) {
            out.insert(key, value);
        }
    }
    return out;
}

QStringList parseGsaKnownConnections(const QString& text) {
    QStringList out;
    QSet<QString> seen;
    const QRegularExpression rx(QStringLiteral("^\\s*(?:'([^']+)'|([^'\\s\\)]+))\\)\\s*$"));
    const QStringList lines = text.split('\n');
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line == QStringLiteral("*) return 1 ;;") || line.startsWith(QStringLiteral("#"))) {
            continue;
        }
        const QRegularExpressionMatch m = rx.match(line);
        if (!m.hasMatch()) {
            continue;
        }
        QString name = m.captured(1).trimmed();
        if (name.isEmpty()) {
            name = m.captured(2).trimmed();
        }
        if (name.isEmpty() || name == QStringLiteral("*")) {
            continue;
        }
        const QString key = name.toLower();
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        out.push_back(name);
    }
    return out;
}

QSet<QString> toLowerSet(const QStringList& values) {
    QSet<QString> out;
    for (const QString& raw : values) {
        const QString key = raw.trimmed().toLower();
        if (!key.isEmpty()) {
            out.insert(key);
        }
    }
    return out;
}

QVector<int> refreshVersionOrderingKey(const QString& version) {
    QVector<int> out;
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)(?:rc(\\d+))?(?:[.-](\\d+))?$"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = rx.match(version.trimmed());
    if (!m.hasMatch()) {
        return out;
    }
    out << m.captured(1).toInt()
        << m.captured(2).toInt()
        << m.captured(3).toInt();
    out << (m.captured(4).isEmpty() ? 999999 : m.captured(4).toInt());
    out << (m.captured(5).isEmpty() ? 0 : m.captured(5).toInt());
    return out;
}

int refreshCompareAppVersions(const QString& a, const QString& b) {
    const QVector<int> ka = refreshVersionOrderingKey(a);
    const QVector<int> kb = refreshVersionOrderingKey(b);
    if (ka.isEmpty() || kb.isEmpty()) {
        return QString::compare(a.trimmed(), b.trimmed(), Qt::CaseInsensitive);
    }
    for (int i = 0; i < qMin(ka.size(), kb.size()); ++i) {
        if (ka[i] < kb[i]) return -1;
        if (ka[i] > kb[i]) return 1;
    }
    return 0;
}

struct PoolGuidStatusEntry {
    QString guid;
    QString status;
};

QMap<QString, PoolGuidStatusEntry> parsePoolGuidStatusBatch(const QString& text) {
    QMap<QString, PoolGuidStatusEntry> out;
    QString currentPool;
    QString currentGuid;
    QStringList statusLines;
    bool collectingStatus = false;

    auto flushCurrent = [&]() {
        const QString pool = currentPool.trimmed();
        if (pool.isEmpty()) {
            currentPool.clear();
            currentGuid.clear();
            statusLines.clear();
            collectingStatus = false;
            return;
        }
        PoolGuidStatusEntry entry;
        entry.guid = currentGuid.trimmed();
        entry.status = statusLines.join('\n').trimmed();
        out.insert(pool, entry);
        currentPool.clear();
        currentGuid.clear();
        statusLines.clear();
        collectingStatus = false;
    };

    const QStringList lines = text.split('\n');
    for (const QString& rawLine : lines) {
        const QString line = rawLine;
        if (line.startsWith(QStringLiteral("__ZFSMGR_POOL__:"))) {
            flushCurrent();
            currentPool = line.mid(QStringLiteral("__ZFSMGR_POOL__:").size()).trimmed();
            continue;
        }
        if (line.startsWith(QStringLiteral("__ZFSMGR_GUID__:"))) {
            currentGuid = line.mid(QStringLiteral("__ZFSMGR_GUID__:").size()).trimmed();
            continue;
        }
        if (line == QStringLiteral("__ZFSMGR_STATUS_BEGIN__")) {
            collectingStatus = true;
            continue;
        }
        if (line == QStringLiteral("__ZFSMGR_STATUS_END__")) {
            collectingStatus = false;
            continue;
        }
        if (collectingStatus) {
            statusLines.push_back(rawLine);
        }
    }
    flushCurrent();
    return out;
}

QString refreshGsaScriptVersion() {
    return QStringLiteral(ZFSMGR_APP_VERSION) + QStringLiteral(".") + QStringLiteral(ZFSMGR_GSA_VERSION_SUFFIX);
}

} // namespace

int MainWindow::findConnectionIndexByName(const QString& name) const {
    const QString key = name.trimmed();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].name.compare(key, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

MainWindow::ConnectionRuntimeState MainWindow::refreshConnection(const ConnectionProfile& p) {
    ConnectionRuntimeState state;
    ConnectionProfile profile = p;
    state.connectionMethod = profile.connType;
    state.powershellFallbackCommands = zfsmgrPowershellCommandSet();
    state.commandsLayer = isWindowsConnection(profile) ? QStringLiteral("Powershell") : QString();
    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_inicio_ref_521ce1"),
               QStringLiteral("Inicio refresh: %1 [%2]"),
               QStringLiteral("Refresh start: %1 [%2]"),
               QStringLiteral("开始刷新：%1 [%2]")).arg(profile.name, profile.connType));
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    qint64 phaseCheckpointMs = 0;
    QMap<QString, qint64> phaseDurMs;
    auto cutPhase = [&](const QString& phaseName) {
        const qint64 now = refreshTimer.elapsed();
        phaseDurMs.insert(phaseName, qMax<qint64>(0, now - phaseCheckpointMs));
        phaseCheckpointMs = now;
    };

    const MainWindow::WindowsCommandMode winPsMode = MainWindow::WindowsCommandMode::PowerShellNative;
    const bool localMode = isLocalConnection(profile);
    auto refineOsLineForRefresh = [&](const QString& currentOsLine) -> QString {
        if (isWindowsConnection(profile)) {
            return currentOsLine.trimmed();
        }
        QString unameOut;
        QString unameErr;
        int unameRc = -1;
        if (!runSsh(profile, QStringLiteral("uname -s"), 8000, unameOut, unameErr, unameRc)
            || unameRc != 0) {
            return currentOsLine.trimmed();
        }
        const QString uname = oneLine(unameOut);
        QString probeOut;
        QString probeErr;
        int probeRc = -1;
        if (uname.compare(QStringLiteral("Linux"), Qt::CaseInsensitive) == 0) {
            const QString cmd = QStringLiteral(
                "sh -lc '. /etc/os-release 2>/dev/null; printf \"%s %s\" \"$NAME\" \"$VERSION_ID\"'");
            if (runSsh(profile, cmd, 8000, probeOut, probeErr, probeRc) && probeRc == 0) {
                const QString refined = oneLine(probeOut);
                if (!refined.isEmpty()) {
                    return refined;
                }
            }
            return QStringLiteral("Linux");
        }
        if (uname.compare(QStringLiteral("Darwin"), Qt::CaseInsensitive) == 0) {
            if (runSsh(profile,
                       QStringLiteral(
                           "sh -lc 'system_profiler SPSoftwareDataType 2>/dev/null | sed -n \"s/^ *System Version: //p\" | head -1'"),
                       10000,
                       probeOut,
                       probeErr,
                       probeRc,
                       {},
                       {},
                       {},
                       winPsMode)
                && probeRc == 0) {
                const QString refined = oneLine(probeOut);
                if (!refined.isEmpty()) {
                    return refined;
                }
            }
            return QStringLiteral("macOS");
        }
        if (uname.compare(QStringLiteral("FreeBSD"), Qt::CaseInsensitive) == 0) {
            if (runSsh(profile,
                       QStringLiteral("freebsd-version -k || freebsd-version || uname -r"),
                       8000,
                       probeOut,
                       probeErr,
                       probeRc,
                       {},
                       {},
                       {},
                       winPsMode)
                && probeRc == 0) {
                const QString refined = oneLine(probeOut);
                if (!refined.isEmpty()) {
                    return QStringLiteral("FreeBSD %1").arg(refined);
                }
            }
            return QStringLiteral("FreeBSD");
        }
        return currentOsLine.trimmed();
    };
    QMap<QString, bool> packageManagerAvailabilityCache;
    auto detectPackageManagerAvailability = [&](const QString& packageManagerId) -> bool {
        const QString pm = packageManagerId.trimmed().toLower();
        if (pm.isEmpty()) {
            return false;
        }
        const auto pmIt = packageManagerAvailabilityCache.constFind(pm);
        if (pmIt != packageManagerAvailabilityCache.cend()) {
            return pmIt.value();
        }
        if (pm == QStringLiteral("msys2")) {
            return true;
        }
        QString cmd;
        if (pm == QStringLiteral("apt")) {
            cmd = QStringLiteral("command -v apt-get >/dev/null 2>&1");
        } else if (pm == QStringLiteral("pacman")) {
            cmd = QStringLiteral("command -v pacman >/dev/null 2>&1");
        } else if (pm == QStringLiteral("zypper")) {
            cmd = QStringLiteral("command -v zypper >/dev/null 2>&1");
        } else if (pm == QStringLiteral("brew")) {
            cmd = QStringLiteral(
                "PATH=\"$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin\"; command -v brew >/dev/null 2>&1");
        } else if (pm == QStringLiteral("pkg")) {
            cmd = QStringLiteral("command -v pkg >/dev/null 2>&1");
        } else {
            return false;
        }
        QString probeOut;
        QString probeErr;
        int probeRc = -1;
        const bool available = runSsh(profile, cmd, 8000, probeOut, probeErr, probeRc, {}, {}, {}, winPsMode) && probeRc == 0;
        packageManagerAvailabilityCache.insert(pm, available);
        return available;
    };
    if (localMode) {
        state.connectionMethod = QStringLiteral("LOCAL/CLI");
        appLog(QStringLiteral("INFO"),
               QStringLiteral("%1: %2").arg(profile.name, state.connectionMethod));
        if (!ensureLocalSudoCredentials(profile)) {
            state.status = QStringLiteral("ERROR");
            state.detail = trk(QStringLiteral("t_local_sudo_req1"),
                               QStringLiteral("Usuario y password sudo son obligatorios."),
                               QStringLiteral("Sudo user and password are required."),
                               QStringLiteral("必须提供 sudo 用户和密码。"));
            appLog(QStringLiteral("NORMAL"),
                   trk(QStringLiteral("t_fin_refres_5a87d4"),
                       QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                       QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                       QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(profile.name, state.detail));
            return state;
        }
    }
    const bool sshMode = (profile.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0);
    const bool psrpMode = (profile.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
    if (!localMode && !sshMode && !psrpMode) {
        state.status = QStringLiteral("ERROR");
        state.detail = trk(QStringLiteral("t_tipo_de_co_e73161"),
                           QStringLiteral("Tipo de conexión no soportado aún en cppqt"),
                           QStringLiteral("Connection type not supported yet in cppqt"),
                           QStringLiteral("cppqt 尚不支持该连接类型"));
        appLog(QStringLiteral("NORMAL"),
               trk(QStringLiteral("t_fin_refres_5a87d4"),
                   QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(profile.name, state.detail));
        return state;
    }
    if (!localMode && (profile.host.isEmpty() || profile.username.isEmpty())) {
        state.status = QStringLiteral("ERROR");
        state.detail = trk(QStringLiteral("t_host_usuar_97cc58"),
                           QStringLiteral("Host/usuario no definido"),
                           QStringLiteral("Host/user not defined"),
                           QStringLiteral("主机/用户未定义"));
        appLog(QStringLiteral("NORMAL"),
               trk(QStringLiteral("t_fin_refres_5a87d4"),
                   QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(profile.name, state.detail));
        return state;
    }

    const bool isWinConn = isWindowsConnection(p);
    const bool useRemoteScripts = !isWinConn;
    const QString refreshCacheKey = p.id.trimmed().isEmpty()
                                        ? p.name.trimmed().toLower()
                                        : p.id.trimmed();
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    constexpr qint64 kRefreshCacheTtlMs = 8000;
    constexpr qint64 kCommandProbeCacheTtlMs = 30000;
    RefreshRuntimeCacheEntry cachedRuntime;
    bool hasCachedRuntime = false;
    bool hasFreshRuntimeCache = false;
    bool hasFreshCommandProbeCache = false;
    if (!refreshCacheKey.isEmpty()) {
        const auto it = m_refreshRuntimeCacheByConnId.constFind(refreshCacheKey);
        if (it != m_refreshRuntimeCacheByConnId.cend() && it->loadedAt.isValid()) {
            cachedRuntime = it.value();
            hasCachedRuntime = true;
            const qint64 ageMs = it->loadedAt.msecsTo(nowUtc);
            hasFreshRuntimeCache = ageMs <= kRefreshCacheTtlMs;
            hasFreshCommandProbeCache =
                cachedRuntime.commandsProbeLoaded && ageMs <= kCommandProbeCacheTtlMs;
        }
    }
    QMap<QString, QMap<QString, QString>> gsaPropsForCache =
        hasFreshRuntimeCache ? cachedRuntime.gsaPropsByDataset : QMap<QString, QMap<QString, QString>>{};
    if (hasCachedRuntime && !cachedRuntime.packageManagerAvailabilityById.isEmpty()) {
        for (auto it = cachedRuntime.packageManagerAvailabilityById.cbegin();
             it != cachedRuntime.packageManagerAvailabilityById.cend();
             ++it) {
            packageManagerAvailabilityCache.insert(it.key(), it.value());
        }
    }
    if (!isWindowsConnection(profile)) {
        (void)ensureRemoteScriptsUpToDate(profile);
    }

    QString out;
    QString err;
    int rc = -1;
    bool unixBasicsLoaded = false;
    if (!isWinConn) {
        QString bOut;
        QString bErr;
        int bRc = -1;
        const QString basicsCmd = withSudo(
            p,
            useRemoteScripts
                ? mwhelpers::withUnixSearchPathCommand(
                      QStringLiteral("if [ -x /usr/local/libexec/zfsmgr-agent ]; then "
                                     "/usr/local/libexec/zfsmgr-agent --dump-refresh-basics; "
                                     "else %1; fi")
                          .arg(remoteScriptCommand(p, QStringLiteral("zfsmgr-refresh-basics"))))
                : mwhelpers::withUnixSearchPathCommand(QStringLiteral("uname -a")));
        if (runSsh(p, basicsCmd, 15000, bOut, bErr, bRc, {}, {}, {}, MainWindow::WindowsCommandMode::Auto)
            && bRc == 0) {
            const QMap<QString, QString> kv = parseKeyValueOutput(bOut + QStringLiteral("\n") + bErr);
            const QString osLine = kv.value(QStringLiteral("OS_LINE")).trimmed();
            if (!osLine.isEmpty()) {
                state.status = QStringLiteral("OK");
                state.detail = osLine;
                state.osLine = osLine;
                unixBasicsLoaded = true;
            }
            const QString machineRaw = kv.value(QStringLiteral("MACHINE_UUID")).trimmed();
            if (!machineRaw.isEmpty()) {
                state.machineUuid = extractMachineUuid(machineRaw);
            }
            const QString zfsRaw = kv.value(QStringLiteral("ZFS_VERSION_RAW")).trimmed();
            if (!zfsRaw.isEmpty()) {
                const QString parsed = mwhelpers::parseOpenZfsVersionText(zfsRaw);
                if (!parsed.isEmpty()) {
                    state.zfsVersion = parsed;
                    state.zfsVersionFull = oneLine(zfsRaw);
                } else {
                    const QRegularExpression simpleVerRx(QStringLiteral("\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b"));
                    const QRegularExpressionMatch m = simpleVerRx.match(zfsRaw);
                    if (m.hasMatch()) {
                        const QString ver = m.captured(1);
                        const int major = ver.section('.', 0, 0).toInt();
                        if (major <= 10) {
                            state.zfsVersion = ver;
                            state.zfsVersionFull = oneLine(zfsRaw);
                        }
                    }
                }
            }
        }
    }
    const QString osProbeCmd = isWindowsConnection(p)
                                   ? QStringLiteral("[System.Environment]::OSVersion.VersionString")
                                   : QStringLiteral("uname -a");
    if (!unixBasicsLoaded) {
        if (!runSsh(profile, osProbeCmd, 12000, out, err, rc, {}, {}, {},
                    isWindowsConnection(profile) ? winPsMode : MainWindow::WindowsCommandMode::Auto) || rc != 0) {
            state.status = QStringLiteral("ERROR");
            state.detail = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
            appLog(QStringLiteral("NORMAL"),
                   trk(QStringLiteral("t_fin_refres_5a87d4"),
                       QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                       QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                       QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(profile.name, state.detail));
            return state;
        }
        state.status = QStringLiteral("OK");
        state.detail = oneLine(out);
        state.osLine = oneLine(out);
        state.osLine = refineOsLineForRefresh(state.osLine);
        state.zfsVersion.clear();
        state.machineUuid.clear();
    }

    if (!unixBasicsLoaded || isWindowsConnection(p)) {
        QString uOut;
        QString uErr;
        int uRc = -1;
        QStringList uuidCmds;
        if (isWindowsConnection(p)) {
            uuidCmds << QStringLiteral("(Get-ItemProperty -Path 'HKLM:\\SOFTWARE\\Microsoft\\Cryptography' -Name MachineGuid).MachineGuid");
        } else {
            uuidCmds << QStringLiteral("cat /etc/machine-id 2>/dev/null")
                     << QStringLiteral("cat /var/lib/dbus/machine-id 2>/dev/null")
                     << QStringLiteral("ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F\\\" '/IOPlatformUUID/{print $(NF-1); exit}'");
        }
        for (const QString& cmd : uuidCmds) {
            uOut.clear();
            uErr.clear();
            uRc = -1;
            if (!runSsh(p, cmd, 8000, uOut, uErr, uRc, {}, {}, {},
                        isWindowsConnection(p) ? winPsMode : MainWindow::WindowsCommandMode::Auto) || uRc != 0) {
                continue;
            }
            const QString uuid = extractMachineUuid(uOut + QStringLiteral("\n") + uErr);
            if (!uuid.isEmpty()) {
                state.machineUuid = uuid;
                break;
            }
        }
    }

    if (isWindowsConnection(p)) {
        auto logWhere = [&](const QString& exeName) {
            QString wOut, wErr;
            int wRc = -1;
            const QString whereCmd = QStringLiteral(
                "$cmd='%1.exe'; "
                "$gc=Get-Command $cmd -ErrorAction SilentlyContinue | Select-Object -First 1; "
                "if($gc -and $gc.Source){ Write-Output $gc.Source; exit 0 }; "
                "try { "
                "  $w = (& where.exe %1 2>$null | Select-Object -First 1); "
                "  if(-not [string]::IsNullOrWhiteSpace($w)){ Write-Output $w } "
                "} catch {}; "
                "exit 0").arg(exeName);
            const bool ran = runSsh(p, whereCmd, 12000, wOut, wErr, wRc, {}, {}, {}, winPsMode);
            if (!ran) {
                const QString reason = oneLine(wErr.isEmpty() ? QStringLiteral("probe failed") : wErr);
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: where %2 -> %3").arg(p.name, exeName, reason));
                return;
            }
            const QStringList lines = wOut.split('\n', Qt::SkipEmptyParts);
            const QString firstPath = lines.isEmpty() ? QString() : lines.first().trimmed();
            if (firstPath.isEmpty()) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: where %2 -> not found").arg(p.name, exeName));
                return;
            }
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: where %2 -> %3").arg(p.name, exeName, oneLine(firstPath)));
        };
        logWhere(QStringLiteral("zfs"));
        logWhere(QStringLiteral("zpool"));
    }

    out.clear();
    err.clear();
    if (isWindowsConnection(p)) {
        const QString zfsVersionCmd = QStringLiteral(
            "$zfsCand=@(); $zpoolCand=@(); "
            "$known=@('C:\\\\Program Files\\\\OpenZFS On Windows\\\\zfs.exe','C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin\\\\zfs.exe'); "
            "foreach($k in $known){ if(Test-Path -LiteralPath $k){ $zfsCand += $k } }; "
            "$knownPool=@('C:\\\\Program Files\\\\OpenZFS On Windows\\\\zpool.exe','C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin\\\\zpool.exe'); "
            "foreach($k in $knownPool){ if(Test-Path -LiteralPath $k){ $zpoolCand += $k } }; "
            "$gc = Get-Command zfs.exe -ErrorAction SilentlyContinue; if($gc -and $gc.Source){ $zfsCand += $gc.Source }; "
            "$gp = Get-Command zpool.exe -ErrorAction SilentlyContinue; if($gp -and $gp.Source){ $zpoolCand += $gp.Source }; "
            "$zfsCand = $zfsCand | Select-Object -Unique; "
            "$zpoolCand = $zpoolCand | Select-Object -Unique; "
            "foreach($e in $zfsCand){ "
            "  try { $o = (& $e 'version' 2>&1 | Out-String).Trim(); if($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($o)){ Write-Output $o; exit 0 } } catch {}; "
            "  try { $o = (& $e '--version' 2>&1 | Out-String).Trim(); if($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($o)){ Write-Output $o; exit 0 } } catch {}; "
            "}; "
            "foreach($e in $zpoolCand){ "
            "  try { $o = (& $e '--version' 2>&1 | Out-String).Trim(); if($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($o)){ Write-Output $o; exit 0 } } catch {}; "
            "  try { $o = (& $e 'version' 2>&1 | Out-String).Trim(); if($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($o)){ Write-Output $o; exit 0 } } catch {}; "
            "}; "
            "exit 1");
        rc = -1;
        if (runSsh(p, zfsVersionCmd, 15000, out, err, rc, {}, {}, {}, winPsMode)) {
            const QString merged = out + QStringLiteral("\n") + err;
            const QString parsed = mwhelpers::parseOpenZfsVersionText(merged);
            if (!parsed.isEmpty()) {
                state.zfsVersion = parsed;
                state.zfsVersionFull = oneLine(merged.simplified());
            } else if (rc == 0) {
                const QRegularExpression simpleVerRx(QStringLiteral("\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b"));
                const QRegularExpressionMatch m = simpleVerRx.match(merged);
                if (m.hasMatch()) {
                    const QString ver = m.captured(1);
                    const int major = ver.section('.', 0, 0).toInt();
                    if (major <= 10) {
                        state.zfsVersion = ver;
                        state.zfsVersionFull = oneLine(merged);
                    }
                }
            }
        }
    } else if (!unixBasicsLoaded || state.zfsVersion.trimmed().isEmpty()) {
        const bool useRemoteScriptsConn = !isWindowsConnection(p);
        const QStringList zfsVersionCandidates = useRemoteScriptsConn
            ? QStringList{
                  mwhelpers::withUnixSearchPathCommand(
                      QStringLiteral("if [ -x /usr/local/libexec/zfsmgr-agent ]; then "
                                     "/usr/local/libexec/zfsmgr-agent --dump-zfs-version; "
                                     "else false; fi")),
                  remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-version"))}
            : QStringList{
                  QStringLiteral("zfs version"),
                  QStringLiteral("zfs --version"),
                  QStringLiteral("zpool --version"),
              };
        for (const QString& cand : zfsVersionCandidates) {
            out.clear();
            err.clear();
            rc = -1;
            const QString zfsVersionCmd = withSudo(
                p,
                useRemoteScriptsConn ? cand : mwhelpers::withUnixSearchPathCommand(cand));
            if (!runSsh(p, zfsVersionCmd, 12000, out, err, rc, {}, {}, {},
                        MainWindow::WindowsCommandMode::Auto)) {
                continue;
            }
            const QString parsed = mwhelpers::parseOpenZfsVersionText(out + QStringLiteral("\n") + err);
            if (!parsed.isEmpty()) {
                state.zfsVersion = parsed;
                state.zfsVersionFull = oneLine((out + QStringLiteral(" ") + err).simplified());
                break;
            }
            if (rc == 0) {
                // Algunos builds imprimen solo el número de versión.
                const QString merged = out + QStringLiteral("\n") + err;
                const QRegularExpression simpleVerRx(QStringLiteral("\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b"));
                const QRegularExpressionMatch m = simpleVerRx.match(merged);
                if (m.hasMatch()) {
                    const QString ver = m.captured(1);
                    const int major = ver.section('.', 0, 0).toInt();
                    if (major <= 10) {
                        state.zfsVersion = ver;
                        state.zfsVersionFull = oneLine(merged);
                        break;
                    }
                }
            }
        }
    }
    if (state.zfsVersion.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: ZFS version not detected").arg(p.name));
    } else if (state.zfsVersionFull.trimmed().isEmpty()) {
        state.zfsVersionFull = QStringLiteral("OpenZFS %1").arg(state.zfsVersion);
    }
    cutPhase(QStringLiteral("basics"));

    // Detección de comandos disponibles para mostrar en detalle de conexión.
    {
        const QStringList wanted = zfsmgrUnixCommandSet();
        if (hasFreshCommandProbeCache) {
            state.detectedUnixCommands = cachedRuntime.detectedUnixCommands;
            state.missingUnixCommands = cachedRuntime.missingUnixCommands;
            state.unixFromMsysOrMingw = cachedRuntime.unixFromMsysOrMingw;
            if (!cachedRuntime.commandsLayer.trimmed().isEmpty()) {
                state.commandsLayer = cachedRuntime.commandsLayer.trimmed();
            }
        } else {
            QStringList detected;
            QStringList missing;
            if (isWindowsConnection(p)) {
            QString dout, derr;
            int drc = -1;
            const QString roots = QStringLiteral(
                "$roots=@('C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin','C:\\\\Program Files\\\\OpenZFS On Windows','C:\\\\msys64\\\\usr\\\\bin','C:\\\\msys64\\\\mingw64\\\\bin','C:\\\\msys64\\\\mingw32\\\\bin','C:\\\\MinGW\\\\bin','C:\\\\mingw64\\\\bin'); "
                "$present=@(); foreach($r in $roots){ if(Test-Path -LiteralPath $r){ $present += $r } }; "
                "$hasMsys = $present | Where-Object { $_ -like 'C:\\\\msys64*' }; "
                "$hasMingw = $present | Where-Object { $_ -like 'C:\\\\MinGW*' -or $_ -like 'C:\\\\mingw64*' }; "
                "if($hasMsys){ Write-Output '__LAYER__:MSYS64' } elseif($hasMingw){ Write-Output '__LAYER__:MingGW' } else { Write-Output '__LAYER__:Powershell' }; "
                "if($present.Count -eq 0){ Write-Output '__NO_UNIX_LAYER__'; exit 0 }; "
                "$cmds='%1'.Split(' '); "
                "foreach($c in $cmds){ $ok=$false; foreach($r in $present){ "
                "$exe = Join-Path $r ($c + '.exe'); "
                "if(-not (Test-Path -LiteralPath $exe)){ continue }; "
                "if($c -eq 'zfs' -or $c -eq 'zpool'){ "
                "  try { & $exe 'version' | Out-Null; if($LASTEXITCODE -eq 0){ $ok=$true; break } } catch {}; "
                "  try { & $exe '--version' | Out-Null; if($LASTEXITCODE -eq 0){ $ok=$true; break } } catch {}; "
                "  try { & $exe 'list' '-H' '-o' 'name' | Out-Null; if($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 1){ $ok=$true; break } } catch {}; "
                "} else { "
                "  $ok=$true; break; "
                "} "
                "}; "
                "if($ok){ Write-Output ('OK:' + $c) } else { Write-Output ('KO:' + $c) } }")
                    .arg(wanted.join(' '));
            if (runSsh(p, roots, 15000, dout, derr, drc, {}, {}, {}, winPsMode) && drc == 0) {
                const QStringList lines = dout.split('\n', Qt::SkipEmptyParts);
                bool noLayer = false;
                for (const QString& raw : lines) {
                    const QString line = raw.trimmed();
                    if (line.startsWith(QStringLiteral("__LAYER__:"))) {
                        state.commandsLayer = line.mid(QStringLiteral("__LAYER__:").size()).trimmed();
                        continue;
                    }
                    if (line == QStringLiteral("__NO_UNIX_LAYER__")) {
                        noLayer = true;
                        state.commandsLayer = QStringLiteral("Powershell");
                        break;
                    }
                    if (line.startsWith(QStringLiteral("OK:"))) {
                        detected << line.mid(3).trimmed();
                    } else if (line.startsWith(QStringLiteral("KO:"))) {
                        missing << line.mid(3).trimmed();
                    }
                }
                if (!noLayer && (!detected.isEmpty() || !missing.isEmpty())) {
                    state.unixFromMsysOrMingw = true;
                    state.detectedUnixCommands = detected;
                    state.missingUnixCommands = missing;
                    if (state.commandsLayer.trimmed().isEmpty()) {
                        state.commandsLayer = QStringLiteral("MSYS64");
                    }
                } else {
                    state.unixFromMsysOrMingw = false;
                    state.detectedUnixCommands.clear();
                    state.missingUnixCommands = wanted;
                    state.commandsLayer = QStringLiteral("Powershell");
                }
            }
            } else {
            QString dout, derr;
            int drc = -1;
            const QString checkCmd = QStringLiteral(
                "PATH=\"$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin\"; "
                "for c in %1; do "
                "  found=0; "
                "  command -v \"$c\" >/dev/null 2>&1 && found=1; "
                "  if [ \"$found\" -eq 0 ] && command -v which >/dev/null 2>&1; then "
                "    which \"$c\" >/dev/null 2>&1 && found=1; "
                "  fi; "
                "  if [ \"$found\" -eq 0 ]; then "
                "    for d in /opt/homebrew/bin /opt/homebrew/sbin /usr/local/bin /usr/local/sbin /usr/local/zfs/bin /usr/sbin /sbin /usr/bin /bin; do "
                "      [ -x \"$d/$c\" ] && found=1 && break; "
                "    done; "
                "  fi; "
                "  if [ \"$found\" -eq 1 ]; then echo \"OK:$c\"; else echo \"KO:$c\"; fi; "
                "done; "
                "for pm in apt-get pacman zypper brew pkg; do "
                "  if command -v \"$pm\" >/dev/null 2>&1; then echo \"PM:$pm:1\"; else echo \"PM:$pm:0\"; fi; "
                "done")
                    .arg(wanted.join(' '));
            if (runSsh(p, checkCmd, 12000, dout, derr, drc) && drc == 0) {
                const QStringList lines = dout.split('\n', Qt::SkipEmptyParts);
                for (const QString& raw : lines) {
                    const QString line = raw.trimmed();
                    if (line.startsWith(QStringLiteral("OK:"))) {
                        detected << line.mid(3).trimmed();
                    } else if (line.startsWith(QStringLiteral("KO:"))) {
                        missing << line.mid(3).trimmed();
                    } else if (line.startsWith(QStringLiteral("PM:"))) {
                        const QStringList parts = line.split(':');
                        if (parts.size() >= 3) {
                            QString pm = parts.value(1).trimmed().toLower();
                            if (pm == QStringLiteral("apt-get")) {
                                pm = QStringLiteral("apt");
                            }
                            packageManagerAvailabilityCache.insert(pm, parts.value(2).trimmed() == QStringLiteral("1"));
                        }
                    }
                }
            } else if (!derr.trimmed().isEmpty()) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: detección de comandos Unix no concluyente -> %2")
                           .arg(p.name, oneLine(derr)));
            } else if (drc != 0) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: detección de comandos Unix no concluyente (rc=%2)")
                           .arg(p.name)
                           .arg(drc));
            }
            state.detectedUnixCommands = detected;
            state.missingUnixCommands = missing;
            state.unixFromMsysOrMingw = false;
            state.commandsLayer.clear();
            }
        }
    }
    cutPhase(QStringLiteral("commands"));

    {
        const helperinstall::PlatformInfo helperPlatform = helperinstall::detectPlatform(profile, state.osLine);
        state.helperPlatformId = helperPlatform.platformId;
        state.helperPlatformLabel = helperPlatform.platformLabel;
        state.helperPackageManagerId = helperPlatform.packageManagerId;
        state.helperPackageManagerLabel = helperPlatform.packageManagerLabel;
        state.helperPackageManagerDetected = detectPackageManagerAvailability(helperPlatform.packageManagerId);
        if (!state.missingUnixCommands.isEmpty()) {
            const bool canElevate = localMode || profile.useSudo
                || profile.username.trimmed().compare(QStringLiteral("root"), Qt::CaseInsensitive) == 0;
            const helperinstall::InstallPlan helperPlan =
                helperinstall::buildInstallPlan(helperPlatform, state.missingUnixCommands, canElevate);
            state.helperInstallableCommands = helperPlan.supportedCommands;
            state.helperUnsupportedCommands = helperPlan.unsupportedCommands;
            state.helperInstallPackages = helperPlan.packages;
            state.helperInstallCommandPreview = helperPlan.commandPreview;
            if (!helperPlatform.supportedByDesign) {
                state.helperInstallReason = helperPlatform.reason.trimmed();
            } else if (!state.helperPackageManagerDetected && !helperPlatform.windowsUsesMsys2) {
                state.helperInstallReason =
                    QStringLiteral("Gestor de paquetes no detectado: %1").arg(helperPlatform.packageManagerLabel);
            } else if (!helperPlan.supported) {
                if (!helperPlan.unsupportedCommands.isEmpty()) {
                    state.helperInstallReason =
                        QStringLiteral("Hay comandos faltantes sin instalación asistida: %1")
                            .arg(helperPlan.unsupportedCommands.join(QStringLiteral(", ")));
                } else if (!helperPlan.warnings.isEmpty()) {
                    state.helperInstallReason = helperPlan.warnings.join(QStringLiteral(" | "));
                }
            } else {
                state.helperInstallSupported = true;
            }
            if (state.helperInstallReason.isEmpty() && !helperPlan.warnings.isEmpty()) {
                state.helperInstallReason = helperPlan.warnings.join(QStringLiteral(" | "));
            }
        }
    }
    cutPhase(QStringLiteral("helper_plan"));

    struct AsyncSshResult {
        bool ran{false};
        QString out;
        QString err;
        int rc{-1};
    };
    auto runAsyncCommand = [this, p](const QString& cmd, int timeoutMs, WindowsCommandMode mode) -> QFuture<AsyncSshResult> {
        return QtConcurrent::run([this, p, cmd, timeoutMs, mode]() -> AsyncSshResult {
            AsyncSshResult r;
            r.ran = runSsh(p, cmd, timeoutMs, r.out, r.err, r.rc, {}, {}, {}, mode);
            return r;
        });
    };

    QString gsaProbeCmd;
    WindowsCommandMode gsaWinMode = WindowsCommandMode::Auto;
    QString agentProbeCmd;
    WindowsCommandMode agentWinMode = WindowsCommandMode::Auto;
    if (isWinConn) {
        gsaWinMode = winPsMode;
        gsaProbeCmd = QStringLiteral(
                "$taskName='ZFSMgr-GSA'; "
                "$scriptPath='C:\\ProgramData\\ZFSMgr\\gsa.ps1'; "
                "$scheduler='taskschd'; "
                "$installed=$false; $active=$false; $ver=''; $detail=''; "
                "$task=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue; "
                "if ($task) { $installed=$true; if ($task.State -eq 'Ready' -or $task.State -eq 'Running') { $active=$true } } "
                "if (Test-Path -LiteralPath $scriptPath) { "
                "  $installed=$true; "
                "  $m=Select-String -LiteralPath $scriptPath -Pattern '^# ZFSMgr GSA Version: (.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1; "
                "  if ($m) { $ver=$m.Matches[0].Groups[1].Value.Trim() } "
                "} "
                "Write-Output ('SCHEDULER=' + $scheduler); "
                "Write-Output ('INSTALLED=' + ($(if($installed){'1'}else{'0'}))); "
                "Write-Output ('ACTIVE=' + ($(if($active){'1'}else{'0'}))); "
                "Write-Output ('VERSION=' + $ver); "
                "Write-Output ('DETAIL=' + $detail)");
        agentWinMode = winPsMode;
        agentProbeCmd = QStringLiteral(
                "$taskName='ZFSMgr-Agent'; "
                "$agentPath='C:\\ProgramData\\ZFSMgr\\agent\\zfsmgr-agent.ps1'; "
                "$scheduler='taskschd'; "
                "$installed=$false; $active=$false; $ver=''; $api=''; $detail=''; "
                "$task=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue; "
                "if ($task) { $installed=$true; if ($task.State -eq 'Ready' -or $task.State -eq 'Running') { $active=$true } } "
                "if (Test-Path -LiteralPath $agentPath) { "
                "  $installed=$true; "
                "  $m1=Select-String -LiteralPath $agentPath -Pattern '^# ZFSMgr Agent Version: (.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1; "
                "  if ($m1) { $ver=$m1.Matches[0].Groups[1].Value.Trim() } "
                "  $m2=Select-String -LiteralPath $agentPath -Pattern '^# ZFSMgr Agent API: (.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1; "
                "  if ($m2) { $api=$m2.Matches[0].Groups[1].Value.Trim() } "
                "} "
                "Write-Output ('SCHEDULER=' + $scheduler); "
                "Write-Output ('INSTALLED=' + ($(if($installed){'1'}else{'0'}))); "
                "Write-Output ('ACTIVE=' + ($(if($active){'1'}else{'0'}))); "
                "Write-Output ('VERSION=' + ($ver -as [string]).Trim()); "
                "Write-Output ('API=' + ($api -as [string]).Trim()); "
                "Write-Output ('DETAIL=' + $detail)");
    } else if (useRemoteScripts) {
        gsaProbeCmd = withSudo(p, remoteScriptCommand(p, QStringLiteral("zfsmgr-gsa-status")));
        agentProbeCmd = withSudo(
                p,
                QStringLiteral(
                    "set +e; scheduler=''; installed=0; active=0; version=''; api=''; detail=''; "
                    "if [ \"$(uname -s 2>/dev/null)\" = 'Darwin' ]; then "
                    "  scheduler='launchd'; "
                    "  bin='/usr/local/libexec/zfsmgr-agent'; "
                    "  plist='/Library/LaunchDaemons/org.zfsmgr.agent.plist'; "
                    "  [ -x \"$bin\" ] && [ -f \"$plist\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$($bin --version 2>/dev/null | head -n1); "
                    "    api=$($bin --api-version 2>/dev/null | head -n1); "
                    "    launchctl print system/org.zfsmgr.agent >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "elif [ \"$(uname -s 2>/dev/null)\" = 'FreeBSD' ]; then "
                    "  scheduler='rc.d'; "
                    "  bin='/usr/local/libexec/zfsmgr-agent'; "
                    "  rc='/usr/local/etc/rc.d/zfsmgr_agent'; "
                    "  [ -x \"$bin\" ] && [ -f \"$rc\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$($bin --version 2>/dev/null | head -n1); "
                    "    api=$($bin --api-version 2>/dev/null | head -n1); "
                    "    service zfsmgr_agent onestatus >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "elif command -v systemctl >/dev/null 2>&1; then "
                    "  scheduler='systemd'; "
                    "  bin='/usr/local/libexec/zfsmgr-agent'; "
                    "  service='/etc/systemd/system/zfsmgr-agent.service'; "
                    "  [ -x \"$bin\" ] && [ -f \"$service\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$($bin --version 2>/dev/null | head -n1); "
                    "    api=$($bin --api-version 2>/dev/null | head -n1); "
                    "    systemctl is-enabled zfsmgr-agent.service >/dev/null 2>&1 && "
                    "systemctl is-active zfsmgr-agent.service >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "else "
                    "  detail='No native scheduler detected'; "
                    "fi; "
                    "printf 'SCHEDULER=%s\\nINSTALLED=%s\\nACTIVE=%s\\nVERSION=%s\\nAPI=%s\\nDETAIL=%s\\n' "
                    "\"$scheduler\" \"$installed\" \"$active\" \"$version\" \"$api\" \"$detail\""));
    } else {
        gsaProbeCmd = withSudo(
                p,
                QStringLiteral(
                    "set +e; "
                    "scheduler=''; installed=0; active=0; version=''; detail=''; guid_compare='0'; "
                    "if [ \"$(uname -s 2>/dev/null)\" = 'Darwin' ]; then "
                    "  scheduler='launchd'; "
                    "  script='/usr/local/libexec/zfsmgr-gsa.sh'; "
                    "  plist='/Library/LaunchDaemons/org.zfsmgr.gsa.plist'; "
                    "  [ -f \"$script\" ] && [ -f \"$plist\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$(sed -n 's/^# ZFSMgr GSA Version: //p' \"$script\" | head -n1); "
                    "    grep -q '^target_has_snapshot_guid()' \"$script\" >/dev/null 2>&1 && "
                    "grep -q '^source_snapshot_name_by_guid()' \"$script\" >/dev/null 2>&1 && guid_compare='1'; "
                    "    launchctl print system/org.zfsmgr.gsa >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "elif [ \"$(uname -s 2>/dev/null)\" = 'FreeBSD' ]; then "
                    "  scheduler='cron'; "
                    "  script='/usr/local/libexec/zfsmgr-gsa.sh'; "
                    "  conf='/etc/zfsmgr/gsa.conf'; "
                    "  [ -f \"$script\" ] && [ -f \"$conf\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$(sed -n 's/^# ZFSMgr GSA Version: //p' \"$script\" | head -n1); "
                    "    grep -q '^target_has_snapshot_guid()' \"$script\" >/dev/null 2>&1 && "
                    "grep -q '^source_snapshot_name_by_guid()' \"$script\" >/dev/null 2>&1 && guid_compare='1'; "
                    "    if crontab -l 2>/dev/null | grep -F '/usr/local/libexec/zfsmgr-gsa.sh' >/dev/null 2>&1; then active=1; fi; "
                    "  fi; "
                    "elif command -v systemctl >/dev/null 2>&1; then "
                    "  scheduler='systemd'; "
                    "  script='/usr/local/libexec/zfsmgr-gsa.sh'; "
                    "  service='/etc/systemd/system/zfsmgr-gsa.service'; "
                    "  timer='/etc/systemd/system/zfsmgr-gsa.timer'; "
                    "  [ -f \"$script\" ] && [ -f \"$service\" ] && [ -f \"$timer\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$(sed -n 's/^# ZFSMgr GSA Version: //p' \"$script\" | head -n1); "
                    "    grep -q '^target_has_snapshot_guid()' \"$script\" >/dev/null 2>&1 && "
                    "grep -q '^source_snapshot_name_by_guid()' \"$script\" >/dev/null 2>&1 && guid_compare='1'; "
                    "    systemctl is-enabled zfsmgr-gsa.timer >/dev/null 2>&1 && systemctl is-active zfsmgr-gsa.timer >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "else "
                    "  detail='No native scheduler detected'; "
                    "fi; "
                    "printf 'SCHEDULER=%s\\nINSTALLED=%s\\nACTIVE=%s\\nVERSION=%s\\nDETAIL=%s\\nGUID_COMPARE=%s\\n' \"$scheduler\" \"$installed\" \"$active\" \"$version\" \"$detail\" \"$guid_compare\""));
        agentProbeCmd = withSudo(
                p,
                QStringLiteral(
                    "set +e; "
                    "scheduler=''; installed=0; active=0; version=''; api=''; detail=''; "
                    "if [ \"$(uname -s 2>/dev/null)\" = 'Darwin' ]; then "
                    "  scheduler='launchd'; "
                    "  bin='/usr/local/libexec/zfsmgr-agent'; "
                    "  plist='/Library/LaunchDaemons/org.zfsmgr.agent.plist'; "
                    "  [ -x \"$bin\" ] && [ -f \"$plist\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$($bin --version 2>/dev/null | head -n1); "
                    "    api=$($bin --api-version 2>/dev/null | head -n1); "
                    "    launchctl print system/org.zfsmgr.agent >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "elif [ \"$(uname -s 2>/dev/null)\" = 'FreeBSD' ]; then "
                    "  scheduler='rc.d'; "
                    "  bin='/usr/local/libexec/zfsmgr-agent'; "
                    "  rc='/usr/local/etc/rc.d/zfsmgr_agent'; "
                    "  [ -x \"$bin\" ] && [ -f \"$rc\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$($bin --version 2>/dev/null | head -n1); "
                    "    api=$($bin --api-version 2>/dev/null | head -n1); "
                    "    service zfsmgr_agent onestatus >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "elif command -v systemctl >/dev/null 2>&1; then "
                    "  scheduler='systemd'; "
                    "  bin='/usr/local/libexec/zfsmgr-agent'; "
                    "  service='/etc/systemd/system/zfsmgr-agent.service'; "
                    "  [ -x \"$bin\" ] && [ -f \"$service\" ] && installed=1; "
                    "  if [ \"$installed\" -eq 1 ]; then "
                    "    version=$($bin --version 2>/dev/null | head -n1); "
                    "    api=$($bin --api-version 2>/dev/null | head -n1); "
                    "    systemctl is-enabled zfsmgr-agent.service >/dev/null 2>&1 && "
                    "systemctl is-active zfsmgr-agent.service >/dev/null 2>&1 && active=1; "
                    "  fi; "
                    "else "
                    "  detail='No native scheduler detected'; "
                    "fi; "
                    "printf 'SCHEDULER=%s\\nINSTALLED=%s\\nACTIVE=%s\\nVERSION=%s\\nAPI=%s\\nDETAIL=%s\\n' "
                    "\"$scheduler\" \"$installed\" \"$active\" \"$version\" \"$api\" \"$detail\""));
    }
    const QString importProbeCmd = withSudo(
        p,
        useRemoteScripts
            ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-import-probe"))
            : mwhelpers::withUnixSearchPathCommand(QStringLiteral("zpool import; zpool import -s")));
    const QString importProbeCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-import-probe")));
    const QString mountedCmdClassic = withSudo(
        p,
        (!isWinConn)
            ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-mount-list"))
            : mwhelpers::withUnixSearchPathCommand(QStringLiteral("zfs mount")));
    const QString mountedCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-mount")));
    QFuture<AsyncSshResult> gsaFuture = runAsyncCommand(gsaProbeCmd, 15000, gsaWinMode);
    QFuture<AsyncSshResult> agentFuture = runAsyncCommand(agentProbeCmd, 15000, agentWinMode);
    QFuture<AsyncSshResult> importFuture = runAsyncCommand(importProbeCmd, 18000, WindowsCommandMode::Auto);

    {
        const AsyncSshResult agentRes = agentFuture.result();
        if (agentRes.ran) {
            const QMap<QString, QString> kv = parseKeyValueOutput(agentRes.out + QStringLiteral("\n") + agentRes.err);
            state.daemonScheduler = kv.value(QStringLiteral("SCHEDULER")).trimmed();
            state.daemonVersion = kv.value(QStringLiteral("VERSION")).trimmed();
            state.daemonApiVersion = kv.value(QStringLiteral("API")).trimmed();
            state.daemonDetail = kv.value(QStringLiteral("DETAIL")).trimmed();
            state.daemonInstalled = (kv.value(QStringLiteral("INSTALLED")).trimmed() == QStringLiteral("1"));
            state.daemonActive = (kv.value(QStringLiteral("ACTIVE")).trimmed() == QStringLiteral("1"));
        }
    }
    cutPhase(QStringLiteral("agent_probe"));

    const QString zpoolListCmdClassic = withSudo(
        p, useRemoteScripts
               ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-list-json"))
               : mwhelpers::withUnixSearchPathCommand(
                     isWinConn
                         ? QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio")
                         : QStringLiteral("zpool list -j")));
    const QString daemonHealthCmd = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --health")));
    bool daemonReadApiOk =
        !isWinConn
        && state.daemonInstalled
        && state.daemonActive
        && state.daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    if (daemonReadApiOk) {
        QString hout;
        QString herr;
        int hrc = -1;
        const bool healthOk = runSsh(p, daemonHealthCmd, 8000, hout, herr, hrc) && hrc == 0;
        if (!healthOk) {
            daemonReadApiOk = false;
            state.daemonActive = false;
            state.daemonDetail = QStringLiteral("health check failed: %1")
                                     .arg(oneLine(herr.isEmpty() ? hout : herr));
        } else {
            const QMap<QString, QString> hkv = parseKeyValueOutput(hout + QStringLiteral("\n") + herr);
            const bool statusOk =
                hkv.value(QStringLiteral("STATUS")).trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
            const bool serverOk = hkv.value(QStringLiteral("SERVER")).trimmed() == QStringLiteral("1");
            if (!statusOk || !serverOk) {
                daemonReadApiOk = false;
                state.daemonActive = false;
                state.daemonDetail = QStringLiteral("health check not OK (status=%1 server=%2)")
                                         .arg(hkv.value(QStringLiteral("STATUS")).trimmed(),
                                              hkv.value(QStringLiteral("SERVER")).trimmed());
            } else {
                const QString cacheEntries = hkv.value(QStringLiteral("CACHE_ENTRIES")).trimmed();
                const QString cacheInvalidations = hkv.value(QStringLiteral("CACHE_INVALIDATIONS")).trimmed();
                const QString zedActive = hkv.value(QStringLiteral("ZED_ACTIVE")).trimmed();
                const QString zedLast = hkv.value(QStringLiteral("ZED_LAST_EVENT_UTC")).trimmed();
                const QString reconcileLast = hkv.value(QStringLiteral("RECONCILE_LAST_UTC")).trimmed();
                if (!cacheEntries.isEmpty() || !zedActive.isEmpty() || !zedLast.isEmpty()
                    || !cacheInvalidations.isEmpty() || !reconcileLast.isEmpty()) {
                    state.daemonDetail = QStringLiteral("cache=%1 inval=%2 zed_active=%3 zed_last=%4 rec_last=%5")
                                             .arg(cacheEntries.isEmpty() ? QStringLiteral("-") : cacheEntries,
                                                  cacheInvalidations.isEmpty() ? QStringLiteral("-") : cacheInvalidations,
                                                  zedActive.isEmpty() ? QStringLiteral("-") : zedActive,
                                                  zedLast.isEmpty() ? QStringLiteral("-") : zedLast,
                                                  reconcileLast.isEmpty() ? QStringLiteral("-") : reconcileLast);
                }
            }
        }
    }
    const QString zpoolListCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-list")));
    const QString zpoolGuidStatusBatchCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-guid-status-batch")));
    out.clear(); err.clear(); rc = -1;
    bool zpoolListOk = false;
    bool zpoolListViaDaemon = false;
    if (daemonReadApiOk) {
        zpoolListOk = runSsh(p, zpoolListCmdDaemon, 18000, out, err, rc) && rc == 0;
        zpoolListViaDaemon = zpoolListOk;
        if (!zpoolListOk) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: daemon zpool list fallback -> %2").arg(p.name, oneLine(err)));
            out.clear();
            err.clear();
            rc = -1;
        }
    }
    if (!zpoolListOk) {
        zpoolListOk = runSsh(p, zpoolListCmdClassic, 18000, out, err, rc) && rc == 0;
    }
    if (zpoolListOk) {
        if (isWinConn) {
            const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QString poolName = line.section('\t', 0, 0).trimmed();
                if (poolName.isEmpty()) {
                    continue;
                }
                state.importedPools.push_back(PoolImported{p.name, poolName, QStringLiteral("Exportar")});
            }
        } else {
            const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
            const QJsonObject pools = doc.object().value(QStringLiteral("pools")).toObject();
            for (auto it = pools.constBegin(); it != pools.constEnd(); ++it) {
                const QString poolName = it.key();
                if (poolName.trimmed().isEmpty()) {
                    continue;
                }
                const QJsonObject poolObj = it.value().toObject();
                QString poolGuid = poolObj.value(QStringLiteral("pool_guid")).toString().trimmed();
                if (poolGuid.isEmpty()) {
                    const QJsonObject props = poolObj.value(QStringLiteral("properties")).toObject();
                    poolGuid = props.value(QStringLiteral("guid")).toObject()
                                   .value(QStringLiteral("value")).toString().trimmed();
                }
                if (!poolGuid.isEmpty() && poolGuid != QStringLiteral("-")) {
                    state.poolGuidByName.insert(poolName.trimmed(), poolGuid);
                }
                state.importedPools.push_back(PoolImported{p.name, poolName, QStringLiteral("Exportar")});
            }
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zpool list -> %2").arg(p.name, oneLine(err)));
    }
    cutPhase(QStringLiteral("zpool_list"));

    if (!isWinConn && hasFreshRuntimeCache) {
        for (auto it = cachedRuntime.poolGuidByName.cbegin(); it != cachedRuntime.poolGuidByName.cend(); ++it) {
            if (!it.key().trimmed().isEmpty() && !it.value().trimmed().isEmpty()) {
                state.poolGuidByName.insert(it.key(), it.value());
            }
        }
        for (auto it = cachedRuntime.poolStatusByName.cbegin(); it != cachedRuntime.poolStatusByName.cend(); ++it) {
            if (!it.key().trimmed().isEmpty() && !it.value().trimmed().isEmpty()) {
                state.poolStatusByName.insert(it.key(), it.value());
            }
        }
    }
    if (!isWinConn) {
        QString bout;
        QString berr;
        int brc = -1;
        const QString batchCmdClassic = withSudo(
            p,
            useRemoteScripts
                ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-guid-status-all"))
                : mwhelpers::withUnixSearchPathCommand(
                      QStringLiteral(
                          "zpool list -H -o name 2>/dev/null | while IFS= read -r pool; do "
                          "[ -n \"$pool\" ] || continue; "
                          "guid=$(zpool get -H -o value guid \"$pool\" 2>/dev/null | head -n1 || true); "
                          "printf '__ZFSMGR_POOL__:%s\\n' \"$pool\"; "
                          "printf '__ZFSMGR_GUID__:%s\\n' \"$guid\"; "
                          "printf '__ZFSMGR_STATUS_BEGIN__\\n'; "
                          "zpool status -v \"$pool\" 2>&1 || true; "
                          "printf '__ZFSMGR_STATUS_END__\\n'; "
                          "done")));
        const QString batchCmd = daemonReadApiOk ? zpoolGuidStatusBatchCmdDaemon : batchCmdClassic;
        if (state.poolStatusByName.isEmpty() || state.poolGuidByName.isEmpty()) {
            bool batchOk = runSsh(p, batchCmd, 45000, bout, berr, brc) && brc == 0;
            if (!batchOk && daemonReadApiOk) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: daemon zpool guid/status fallback -> %2")
                           .arg(p.name, oneLine(berr.isEmpty() ? bout : berr)));
                bout.clear();
                berr.clear();
                brc = -1;
                batchOk = runSsh(p, batchCmdClassic, 45000, bout, berr, brc) && brc == 0;
            }
            if (batchOk) {
                const QMap<QString, PoolGuidStatusEntry> parsed =
                    parsePoolGuidStatusBatch(bout + QStringLiteral("\n") + berr);
                for (auto it = parsed.cbegin(); it != parsed.cend(); ++it) {
                    const QString poolName = it.key().trimmed();
                    if (poolName.isEmpty()) {
                        continue;
                    }
                    const QString guid = it.value().guid.trimmed();
                    if (!guid.isEmpty() && guid != QStringLiteral("-")) {
                        state.poolGuidByName.insert(poolName, guid);
                    }
                    if (!it.value().status.trimmed().isEmpty()) {
                        state.poolStatusByName.insert(poolName, it.value().status.trimmed());
                    }
                }
            }
        }
    }

    for (const PoolImported& pool : state.importedPools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        if (state.poolGuidByName.value(poolName).trimmed().isEmpty()) {
            QString gout;
            QString gerr;
            int grc = -1;
            const QString guidCmdClassic = withSudo(
                p,
                useRemoteScripts
                    ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-guid"), {poolName})
                    : mwhelpers::withUnixSearchPathCommand(
                          QStringLiteral("zpool get -H -o value guid %1").arg(mwhelpers::shSingleQuote(poolName))));
            const QString guidCmdDaemon = withSudo(
                p, mwhelpers::withUnixSearchPathCommand(
                       QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-guid %1")
                           .arg(mwhelpers::shSingleQuote(poolName))));
            bool guidOk = runSsh(p, (daemonReadApiOk ? guidCmdDaemon : guidCmdClassic), 12000, gout, gerr, grc) && grc == 0;
            if (!guidOk && daemonReadApiOk) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: daemon zpool guid fallback (%2) -> %3")
                           .arg(p.name, poolName, oneLine(gerr.isEmpty() ? gout : gerr)));
                gout.clear();
                gerr.clear();
                grc = -1;
                guidOk = runSsh(p, guidCmdClassic, 12000, gout, gerr, grc) && grc == 0;
            }
            if (guidOk) {
                const QString guid = gout.section('\n', 0, 0).trimmed();
                if (!guid.isEmpty() && guid != QStringLiteral("-")) {
                    state.poolGuidByName.insert(poolName, guid);
                }
            }
        }
        if (state.poolStatusByName.contains(poolName) && !state.poolStatusByName.value(poolName).trimmed().isEmpty()) {
            continue;
        }
        out.clear();
        err.clear();
        rc = -1;
        const QString stCmdClassic = withSudo(
            p,
            useRemoteScripts
                ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-status"), {poolName})
                : mwhelpers::withUnixSearchPathCommand(
                      QStringLiteral("zpool status -v %1").arg(mwhelpers::shSingleQuote(poolName))));
        const QString stCmdDaemon = withSudo(
            p, mwhelpers::withUnixSearchPathCommand(
                   QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-status %1")
                       .arg(mwhelpers::shSingleQuote(poolName))));
        bool statusOk = runSsh(p, (daemonReadApiOk ? stCmdDaemon : stCmdClassic), 20000, out, err, rc) && rc == 0;
        if (!statusOk && daemonReadApiOk) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: daemon zpool status fallback (%2) -> %3")
                       .arg(p.name, poolName, oneLine(err.isEmpty() ? out : err)));
            out.clear();
            err.clear();
            rc = -1;
            statusOk = runSsh(p, stCmdClassic, 20000, out, err, rc) && rc == 0;
        }
        if (statusOk) {
            state.poolStatusByName.insert(poolName, out.trimmed());
        } else {
            const QString fallback = !out.trimmed().isEmpty() ? out.trimmed() : err.trimmed();
            state.poolStatusByName.insert(poolName, fallback);
        }
    }
    cutPhase(QStringLiteral("pool_details"));

    bool gsaSupportsGuidSnapshotCompare = true;
    {
        const AsyncSshResult gsaRes = gsaFuture.result();
        if (gsaRes.ran) {
            const QMap<QString, QString> kv = parseKeyValueOutput(gsaRes.out + QStringLiteral("\n") + gsaRes.err);
            state.gsaScheduler = kv.value(QStringLiteral("SCHEDULER")).trimmed();
            state.gsaVersion = kv.value(QStringLiteral("VERSION")).trimmed();
            state.gsaDetail = kv.value(QStringLiteral("DETAIL")).trimmed();
            state.gsaInstalled = (kv.value(QStringLiteral("INSTALLED")).trimmed() == QStringLiteral("1"));
            state.gsaActive = (kv.value(QStringLiteral("ACTIVE")).trimmed() == QStringLiteral("1"));
            if (!isWinConn && state.gsaInstalled) {
                gsaSupportsGuidSnapshotCompare = (kv.value(QStringLiteral("GUID_COMPARE")).trimmed() == QStringLiteral("1"));
            }
        }
    }
    cutPhase(QStringLiteral("gsa_probe"));
    if (state.gsaInstalled && !isWinConn) {
        QString mapOut;
        QString mapErr;
        int mapRc = -1;
        const QString cmdClassic =
            useRemoteScripts
                ? withSudo(p, remoteScriptCommand(p, QStringLiteral("zfsmgr-gsa-connections-cat")))
                : withSudo(
                      p,
                      QStringLiteral("if [ -f /etc/zfsmgr/gsa-connections.conf ]; then cat /etc/zfsmgr/gsa-connections.conf; fi"));
        const QString cmdDaemon = withSudo(
            p, mwhelpers::withUnixSearchPathCommand(
                   QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-gsa-connections-conf")));
        bool mapOk = runSsh(p, (daemonReadApiOk ? cmdDaemon : cmdClassic), 12000, mapOut, mapErr, mapRc) && mapRc == 0;
        if (!mapOk && daemonReadApiOk) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: daemon gsa-connections fallback -> %2")
                       .arg(p.name, oneLine(mapErr.isEmpty() ? mapOut : mapErr)));
            mapOut.clear();
            mapErr.clear();
            mapRc = -1;
            mapOk = runSsh(p, cmdClassic, 12000, mapOut, mapErr, mapRc) && mapRc == 0;
        }
        if (mapOk) {
            state.gsaKnownConnections = parseGsaKnownConnections(mapOut);
        }
    }
    cutPhase(QStringLiteral("gsa_connections"));

    {
        AsyncSshResult importRes;
        bool importOk = false;
        if (daemonReadApiOk) {
            importRes = runAsyncCommand(importProbeCmdDaemon, 25000, WindowsCommandMode::Auto).result();
            importOk = importRes.ran && importRes.rc == 0;
            if (!importOk) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: daemon zpool import probe fallback -> %2")
                           .arg(p.name, oneLine(importRes.err.isEmpty() ? importRes.out : importRes.err)));
            }
        }
        if (!importOk) {
            importRes = importFuture.result();
        }
        if (importRes.ran) {
            const QString merged = importRes.out + QStringLiteral("\n") + importRes.err;
            const QVector<mwhelpers::ImportablePoolInfo> parsed = mwhelpers::parseZpoolImportOutput(merged);
            if (!parsed.isEmpty()) {
                state.importablePools.clear();
                for (const auto& row : parsed) {
                    state.importablePools.push_back(PoolImportable{
                        p.name,
                        row.pool,
                        row.guid,
                        row.state,
                        row.reason,
                        QStringLiteral("Importar"),
                    });
                }
            }
        }
    }
    cutPhase(QStringLiteral("import_probe"));

    QVector<QPair<QString, QString>> mountedRows;
    bool mountsViaDaemon = false;
    AsyncSshResult mountsRes;
    if (daemonReadApiOk) {
        mountsRes = runAsyncCommand(mountedCmdDaemon, 18000, WindowsCommandMode::Auto).result();
        mountsViaDaemon = mountsRes.ran && mountsRes.rc == 0;
        if (!mountsViaDaemon) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: daemon zfs mount fallback -> %2")
                       .arg(p.name, oneLine(mountsRes.err.isEmpty() ? mountsRes.out : mountsRes.err)));
        }
    }
    if (!mountsViaDaemon) {
        mountsRes = runAsyncCommand(mountedCmdClassic, 18000, WindowsCommandMode::Auto).result();
    }
    if (mountsRes.ran && mountsRes.rc == 0) {
        mountedRows = isWinConn
            ? mwhelpers::parseZfsMountOutput(mountsRes.out)
            : mwhelpers::parseZfsMountJsonOutput(mountsRes.out);
    }
    if (!isWinConn && mountedRows.isEmpty() && !useRemoteScripts) {
        QString fbOut;
        QString fbErr;
        int fbRc = -1;
        const QString fallbackCmd = withSudo(p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("zfs mount")));
        if (runSsh(p, fallbackCmd, 18000, fbOut, fbErr, fbRc) && fbRc == 0) {
            mountedRows = mwhelpers::parseZfsMountOutput(fbOut);
        }
    }
    if (!mountedRows.isEmpty()) {
        for (const auto& row : mountedRows) {
            const QString ds = row.first;
            const QString mp = row.second;
            if (!ds.isEmpty() && !mp.isEmpty()) {
                state.mountedDatasets.push_back(qMakePair(ds, mp));
            }
        }
    } else if (!mountsRes.err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zfs mount -> %2").arg(p.name, oneLine(mountsRes.err)));
    }
    cutPhase(QStringLiteral("mounts"));
    if (state.gsaInstalled) {
        const QString expectedGsaVersion = refreshGsaScriptVersion().trimmed();
        if (state.gsaVersion.trimmed() != expectedGsaVersion) {
            state.gsaNeedsAttention = true;
            state.gsaAttentionReasons.push_back(QStringLiteral("versión GSA antigua"));
        }
        if (!isWindowsConnection(p) && !gsaSupportsGuidSnapshotCompare) {
            state.gsaNeedsAttention = true;
            state.gsaAttentionReasons.push_back(QStringLiteral("script GSA sin comparación por GUID"));
        }
        if (!isWindowsConnection(p)) {
            const auto boolOnString = [](const QString& raw) {
                const QString v = raw.trimmed().toLower();
                return v == QStringLiteral("on")
                       || v == QStringLiteral("yes")
                       || v == QStringLiteral("true")
                       || v == QStringLiteral("1");
            };
            QSet<QString> requiredConnections;
            QMap<QString, QMap<QString, QString>> propsByDataset = gsaPropsForCache;
            QString gout;
            QString gerr;
            int grc = -1;
            const QString gsaProps = QStringLiteral("org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino");
            const QString gcmdClassic = withSudo(
                p,
                useRemoteScripts
                    ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-get-gsa-raw-all-pools"))
                    : mwhelpers::withUnixSearchPathCommand(
                          QStringLiteral(
                              "props=%1; "
                              "zpool list -H -o name 2>/dev/null | while IFS= read -r pool; do "
                              "[ -n \"$pool\" ] || continue; "
                              "zfs get -H -o name,property,value -r \"$props\" \"$pool\" 2>/dev/null || true; "
                              "done")
                              .arg(mwhelpers::shSingleQuote(gsaProps))));
            const QString gcmdDaemon = withSudo(
                p, mwhelpers::withUnixSearchPathCommand(
                       QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-get-gsa-raw-all-pools")));
            if (propsByDataset.isEmpty()) {
                bool gsaPropsOk = runSsh(p, (daemonReadApiOk ? gcmdDaemon : gcmdClassic), 30000, gout, gerr, grc) && grc == 0;
                if (!gsaPropsOk && daemonReadApiOk) {
                    appLog(QStringLiteral("INFO"),
                           QStringLiteral("%1: daemon gsa raw scan fallback -> %2")
                               .arg(p.name, oneLine(gerr.isEmpty() ? gout : gerr)));
                    gout.clear();
                    gerr.clear();
                    grc = -1;
                    gsaPropsOk = runSsh(p, gcmdClassic, 30000, gout, gerr, grc) && grc == 0;
                }
                if (gsaPropsOk) {
                    const QString merged = gout + QStringLiteral("\n") + gerr;
                    const QStringList lines = merged.split('\n', Qt::SkipEmptyParts);
                    for (const QString& raw : lines) {
                        const QString line = raw.trimmed();
                        if (line.isEmpty() || line.startsWith(QStringLiteral("cannot ")) || line.startsWith(QStringLiteral("zfs:"))) {
                            continue;
                        }
                        const QStringList cols = raw.split('\t');
                        if (cols.size() < 3) {
                            continue;
                        }
                        const QString datasetName = cols.value(0).trimmed();
                        const QString propName = cols.value(1).trimmed();
                        const QString propValue = cols.value(2).trimmed();
                        if (datasetName.isEmpty() || datasetName.contains(QLatin1Char('@')) || propName.isEmpty()) {
                            continue;
                        }
                        propsByDataset[datasetName].insert(propName, propValue);
                    }
                }
            }
            for (auto dsIt = propsByDataset.cbegin(); dsIt != propsByDataset.cend(); ++dsIt) {
                const QMap<QString, QString>& props = dsIt.value();
                if (!boolOnString(props.value(QStringLiteral("org.fc16.gsa:activado")))) {
                    continue;
                }
                if (!boolOnString(props.value(QStringLiteral("org.fc16.gsa:nivelar")))) {
                    continue;
                }
                const QString dest = props.value(QStringLiteral("org.fc16.gsa:destino")).trimmed();
                const QString destConnName = dest.section(QStringLiteral("::"), 0, 0).trimmed();
                if (!destConnName.isEmpty()) {
                    requiredConnections.insert(destConnName);
                }
            }
            gsaPropsForCache = propsByDataset;
            state.gsaRequiredConnections = QStringList(requiredConnections.begin(), requiredConnections.end());
            std::sort(state.gsaRequiredConnections.begin(), state.gsaRequiredConnections.end(),
                      [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
            if (toLowerSet(state.gsaKnownConnections) != toLowerSet(state.gsaRequiredConnections)) {
                state.gsaNeedsAttention = true;
                state.gsaAttentionReasons.push_back(QStringLiteral("conexiones GSA desactualizadas"));
            }
        }
    }
    if (state.daemonInstalled) {
        const QString expectedAgentVersion = agentversion::currentVersion().trimmed();
        const QString expectedApiVersion = agentversion::expectedApiVersion().trimmed();
        if (!state.daemonVersion.trimmed().isEmpty()
            && agentversion::compareVersions(state.daemonVersion.trimmed(), expectedAgentVersion) < 0) {
            state.daemonNeedsAttention = true;
            state.daemonAttentionReasons.push_back(QStringLiteral("versión daemon antigua"));
        }
        if (!expectedApiVersion.isEmpty() && !state.daemonApiVersion.trimmed().isEmpty()
            && state.daemonApiVersion.trimmed() != expectedApiVersion) {
            state.daemonNeedsAttention = true;
            state.daemonAttentionReasons.push_back(QStringLiteral("API daemon incompatible"));
        }
    }
    cutPhase(QStringLiteral("gsa_eval"));
    appLog(
        QStringLiteral("INFO"),
        QStringLiteral(
            "%1: refresh timings ms total=%2 basics=%3 commands=%4 helper=%5 gsa_probe=%6 agent_probe=%7 gsa_conn=%8 zpool_list=%9 pool_details=%10 import=%11 mounts=%12 gsa_eval=%13 zpool_via=%14 mounts_via=%15")
            .arg(p.name)
            .arg(refreshTimer.elapsed())
            .arg(phaseDurMs.value(QStringLiteral("basics"), -1))
            .arg(phaseDurMs.value(QStringLiteral("commands"), -1))
            .arg(phaseDurMs.value(QStringLiteral("helper_plan"), -1))
            .arg(phaseDurMs.value(QStringLiteral("gsa_probe"), -1))
            .arg(phaseDurMs.value(QStringLiteral("agent_probe"), -1))
            .arg(phaseDurMs.value(QStringLiteral("gsa_connections"), -1))
            .arg(phaseDurMs.value(QStringLiteral("zpool_list"), -1))
            .arg(phaseDurMs.value(QStringLiteral("pool_details"), -1))
            .arg(phaseDurMs.value(QStringLiteral("import_probe"), -1))
            .arg(phaseDurMs.value(QStringLiteral("mounts"), -1))
            .arg(phaseDurMs.value(QStringLiteral("gsa_eval"), -1))
            .arg(zpoolListViaDaemon ? QStringLiteral("daemon") : QStringLiteral("ssh"))
            .arg(mountsViaDaemon ? QStringLiteral("daemon") : QStringLiteral("ssh")));

    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_fin_refres_6eead9"),
               QStringLiteral("Fin refresh: %1 -> OK (%2)"),
               QStringLiteral("Refresh end: %1 -> OK (%2)"),
               QStringLiteral("刷新结束：%1 -> OK (%2)")).arg(p.name, state.detail));
    if (!refreshCacheKey.isEmpty() && !isWinConn) {
        RefreshRuntimeCacheEntry fresh;
        fresh.loadedAt = QDateTime::currentDateTimeUtc();
        fresh.poolStatusByName = state.poolStatusByName;
        fresh.poolGuidByName = state.poolGuidByName;
        fresh.gsaPropsByDataset = gsaPropsForCache;
        fresh.commandsProbeLoaded = true;
        fresh.detectedUnixCommands = state.detectedUnixCommands;
        fresh.missingUnixCommands = state.missingUnixCommands;
        fresh.unixFromMsysOrMingw = state.unixFromMsysOrMingw;
        fresh.commandsLayer = state.commandsLayer;
        fresh.packageManagerAvailabilityById = packageManagerAvailabilityCache;
        m_refreshRuntimeCacheByConnId.insert(refreshCacheKey, fresh);
        constexpr qint64 kRefreshCacheMaxAgeMs = 60000;
        auto it = m_refreshRuntimeCacheByConnId.begin();
        while (it != m_refreshRuntimeCacheByConnId.end()) {
            if (!it->loadedAt.isValid() || it->loadedAt.msecsTo(fresh.loadedAt) > kRefreshCacheMaxAgeMs) {
                it = m_refreshRuntimeCacheByConnId.erase(it);
            } else {
                ++it;
            }
        }
    }
    return state;
}
