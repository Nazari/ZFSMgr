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
            mwhelpers::withUnixSearchPathCommand(
                QStringLiteral("if [ -x /usr/local/libexec/zfsmgr-agent ]; then "
                               "/usr/local/libexec/zfsmgr-agent --dump-refresh-basics; "
                               "else uname -a; fi")));
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
        const QStringList zfsVersionCandidates = {
            mwhelpers::withUnixSearchPathCommand(
                QStringLiteral("if [ -x /usr/local/libexec/zfsmgr-agent ]; then "
                               "/usr/local/libexec/zfsmgr-agent --dump-zfs-version; "
                               "else false; fi")),
            mwhelpers::withUnixSearchPathCommand(QStringLiteral("zfs version")),
            mwhelpers::withUnixSearchPathCommand(QStringLiteral("zfs --version")),
            mwhelpers::withUnixSearchPathCommand(QStringLiteral("zpool --version")),
        };
        for (const QString& cand : zfsVersionCandidates) {
            out.clear();
            err.clear();
            rc = -1;
            const QString zfsVersionCmd = withSudo(p, cand);
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

    QString agentProbeCmd;
    WindowsCommandMode agentWinMode = WindowsCommandMode::Auto;
    if (isWinConn) {
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
    }
    const QString importProbeCmd = withSudo(
        p,
        mwhelpers::withUnixSearchPathCommand(QStringLiteral("zpool import; zpool import -s")));
    const QString importProbeCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-import-probe")));
    const QString mountedCmdClassic = withSudo(
        p,
        mwhelpers::withUnixSearchPathCommand(QStringLiteral("zfs mount")));
    const QString mountedCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-mount")));
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
            state.daemonNativeBinary = (kv.value(QStringLiteral("NATIVE")).trimmed() == QStringLiteral("1"));
        }
    }
    cutPhase(QStringLiteral("agent_probe"));

    const QString zpoolListCmdClassic = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(
               isWinConn
                   ? QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio")
                   : QStringLiteral("zpool list -j")));
    const QString daemonHealthCmd = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("/usr/local/libexec/zfsmgr-agent --health")));
    bool daemonReadApiOk =
        !isWinConn
        && state.daemonInstalled
        && state.daemonActive
        && state.daemonNativeBinary
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
                const QString cacheMax = hkv.value(QStringLiteral("CACHE_MAX_ENTRIES")).trimmed();
                const QString cacheInvalidations = hkv.value(QStringLiteral("CACHE_INVALIDATIONS")).trimmed();
                const QString poolInvalidations = hkv.value(QStringLiteral("POOL_INVALIDATIONS")).trimmed();
                const QString reconcilePruned = hkv.value(QStringLiteral("RECONCILE_PRUNED")).trimmed();
                const QString rpcFailures = hkv.value(QStringLiteral("RPC_FAILURES")).trimmed();
                const QString rpcCommands = hkv.value(QStringLiteral("RPC_COMMANDS")).trimmed();
                const QString zedActive = hkv.value(QStringLiteral("ZED_ACTIVE")).trimmed();
                const QString zedRestarts = hkv.value(QStringLiteral("ZED_RESTARTS")).trimmed();
                const QString zedLast = hkv.value(QStringLiteral("ZED_LAST_EVENT_UTC")).trimmed();
                const QString reconcileLast = hkv.value(QStringLiteral("RECONCILE_LAST_UTC")).trimmed();
                // If the daemon has seen new ZED events since our last health poll,
                // schedule an immediate follow-up refresh to pick up the changed state.
                if (!zedLast.isEmpty()
                    && zedLast != state.daemonLastSeenZedEvent
                    && !state.daemonLastSeenZedEvent.isEmpty()) {
                    appLog(QStringLiteral("INFO"),
                           QStringLiteral("%1: ZED event detectado (ts=%2), programando refresh")
                               .arg(p.name, zedLast));
                    const QString profileId = p.id;
                    QMetaObject::invokeMethod(this, [this, profileId]() {
                        for (int i = 0; i < m_profiles.size(); ++i) {
                            if (m_profiles[i].id == profileId) {
                                refreshConnectionByIndex(i);
                                break;
                            }
                        }
                    }, Qt::QueuedConnection);
                }
                state.daemonLastSeenZedEvent = zedLast;
                if (!cacheEntries.isEmpty() || !zedActive.isEmpty() || !zedLast.isEmpty()
                    || !cacheInvalidations.isEmpty() || !poolInvalidations.isEmpty() || !reconcilePruned.isEmpty()
                    || !reconcileLast.isEmpty() || !zedRestarts.isEmpty()
                    || !rpcFailures.isEmpty()) {
                    const int rpcCmdCount = rpcCommands.isEmpty() ? 0 : rpcCommands.split(QLatin1Char(','), Qt::SkipEmptyParts).size();
                    state.daemonDetail = QStringLiteral("cache=%1/%2 inval=%3 pool_inval=%4 rec_pruned=%5 rpc_fail=%6 rpc_cmds=%7 zed_active=%8 zed_restarts=%9 zed_last=%10 rec_last=%11")
                                             .arg(cacheEntries.isEmpty() ? QStringLiteral("-") : cacheEntries,
                                                  cacheMax.isEmpty() ? QStringLiteral("-") : cacheMax,
                                                  cacheInvalidations.isEmpty() ? QStringLiteral("-") : cacheInvalidations,
                                                  poolInvalidations.isEmpty() ? QStringLiteral("-") : poolInvalidations,
                                                  reconcilePruned.isEmpty() ? QStringLiteral("-") : reconcilePruned,
                                                  rpcFailures.isEmpty() ? QStringLiteral("-") : rpcFailures,
                                                  rpcCmdCount <= 0 ? QStringLiteral("-") : QString::number(rpcCmdCount),
                                                  zedActive.isEmpty() ? QStringLiteral("-") : zedActive,
                                                  zedRestarts.isEmpty() ? QStringLiteral("-") : zedRestarts,
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
            mwhelpers::withUnixSearchPathCommand(
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
                mwhelpers::withUnixSearchPathCommand(
                    QStringLiteral("zpool get -H -o value guid %1").arg(mwhelpers::shSingleQuote(poolName))));
            const QString guidCmdDaemon = withSudo(
                p, mwhelpers::withUnixSearchPathCommand(
                       QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-guid %1")
                           .arg(mwhelpers::shSingleQuote(poolName))));
            const QString selectedGuidCmd = daemonReadApiOk ? guidCmdDaemon : guidCmdClassic;
            bool guidOk = runSsh(p, selectedGuidCmd, 12000, gout, gerr, grc) && grc == 0;
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
            mwhelpers::withUnixSearchPathCommand(
                QStringLiteral("zpool status -v %1").arg(mwhelpers::shSingleQuote(poolName))));
        const QString stCmdDaemon = withSudo(
            p, mwhelpers::withUnixSearchPathCommand(
                   QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-status %1")
                       .arg(mwhelpers::shSingleQuote(poolName))));
        const QString selectedStatusCmd = daemonReadApiOk ? stCmdDaemon : stCmdClassic;
        bool statusOk = runSsh(p, selectedStatusCmd, 20000, out, err, rc) && rc == 0;
        if (statusOk) {
            state.poolStatusByName.insert(poolName, out.trimmed());
        } else {
            const QString fallback = !out.trimmed().isEmpty() ? out.trimmed() : err.trimmed();
            state.poolStatusByName.insert(poolName, fallback);
        }
    }
    cutPhase(QStringLiteral("pool_details"));


    {
        AsyncSshResult importRes;
        bool importOk = false;
        if (daemonReadApiOk) {
            importRes = runAsyncCommand(importProbeCmdDaemon, 25000, WindowsCommandMode::Auto).result();
            importOk = importRes.ran && importRes.rc == 0;
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
    }
    if (!mountsViaDaemon) {
        mountsRes = runAsyncCommand(mountedCmdClassic, 18000, WindowsCommandMode::Auto).result();
    }
    if (mountsRes.ran && mountsRes.rc == 0) {
        mountedRows = isWinConn
            ? mwhelpers::parseZfsMountOutput(mountsRes.out)
            : mwhelpers::parseZfsMountJsonOutput(mountsRes.out);
    }
    if (!isWinConn && mountedRows.isEmpty() && !daemonReadApiOk) {
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
    if (!isWinConn) {
        QMap<QString, QMap<QString, QString>> propsByDataset = gsaPropsForCache;
        QString gout;
        QString gerr;
        int grc = -1;
        const QString gsaProps = QStringLiteral("org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino");
        const QString gcmdClassic = withSudo(
            p,
            mwhelpers::withUnixSearchPathCommand(
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
            const QString selectedGsaCmd = daemonReadApiOk ? gcmdDaemon : gcmdClassic;
            bool gsaPropsOk = runSsh(p, selectedGsaCmd, 30000, gout, gerr, grc) && grc == 0;
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
        gsaPropsForCache = propsByDataset;
    }
    if (state.daemonInstalled) {
        const QString expectedAgentVersion = agentversion::currentVersion().trimmed();
        const QString expectedApiVersion = agentversion::expectedApiVersion().trimmed();
        if (!isWinConn && !state.daemonNativeBinary) {
            state.daemonNeedsAttention = true;
            state.daemonAttentionReasons.push_back(QStringLiteral("daemon no nativo (RPC TLS no disponible)"));
        }
        {
            // The version suffix is a hash (not a sequence number), so numeric
            // ordering of the full string is meaningless. Strategy:
            //  - If full strings match → up to date, nothing to do.
            //  - If base versions (major.minor.patch) differ → only update when
            //    remote base is strictly older (avoid downgrading a future agent).
            //  - If base versions are equal but full strings differ (different
            //    suffix/build) → update unconditionally.
            const QString remoteVer = state.daemonVersion.trimmed();
            if (!remoteVer.isEmpty() && remoteVer != expectedAgentVersion) {
                const auto baseOf = [](const QString& v) -> QString {
                    const QStringList p = v.split(QLatin1Char('.'));
                    return p.size() >= 3 ? QStringList{p[0], p[1], p[2]}.join(QLatin1Char('.')) : v;
                };
                const int baseCmp = agentversion::compareVersions(baseOf(remoteVer), baseOf(expectedAgentVersion));
                if (baseCmp <= 0) {
                    state.daemonNeedsAttention = true;
                    state.daemonAttentionReasons.push_back(QStringLiteral("versión daemon desactualizada"));
                }
            }
        }
        if (!expectedApiVersion.isEmpty() && !state.daemonApiVersion.trimmed().isEmpty()
            && state.daemonApiVersion.trimmed() != expectedApiVersion) {
            state.daemonNeedsAttention = true;
            state.daemonAttentionReasons.push_back(QStringLiteral("API daemon incompatible"));
        }
    }
    appLog(
        QStringLiteral("INFO"),
        QStringLiteral(
            "%1: refresh timings ms total=%2 basics=%3 commands=%4 helper=%5 agent_probe=%6 zpool_list=%7 pool_details=%8 import=%9 mounts=%10 zpool_via=%11 mounts_via=%12")
            .arg(p.name)
            .arg(refreshTimer.elapsed())
            .arg(phaseDurMs.value(QStringLiteral("basics"), -1))
            .arg(phaseDurMs.value(QStringLiteral("commands"), -1))
            .arg(phaseDurMs.value(QStringLiteral("helper_plan"), -1))
            .arg(phaseDurMs.value(QStringLiteral("agent_probe"), -1))
            .arg(phaseDurMs.value(QStringLiteral("zpool_list"), -1))
            .arg(phaseDurMs.value(QStringLiteral("pool_details"), -1))
            .arg(phaseDurMs.value(QStringLiteral("import_probe"), -1))
            .arg(phaseDurMs.value(QStringLiteral("mounts"), -1))
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
