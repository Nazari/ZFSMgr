#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "helperinstallcatalog.h"

#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

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
    auto detectPackageManagerAvailability = [&](const QString& packageManagerId) -> bool {
        const QString pm = packageManagerId.trimmed().toLower();
        if (pm.isEmpty()) {
            return false;
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
        return runSsh(profile, cmd, 8000, probeOut, probeErr, probeRc, {}, {}, {}, winPsMode) && probeRc == 0;
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

    if (!isWindowsConnection(profile)) {
        (void)ensureRemoteScriptsUpToDate(profile);
    }

    QString out;
    QString err;
    int rc = -1;
    const QString osProbeCmd = isWindowsConnection(p)
                                   ? QStringLiteral("[System.Environment]::OSVersion.VersionString")
                                   : QStringLiteral("uname -a");
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

    {
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
            const QString whereCmd = QStringLiteral("where.exe %1").arg(exeName);
            if (runSsh(p, whereCmd, 12000, wOut, wErr, wRc, {}, {}, {}, winPsMode) && wRc == 0) {
                const QStringList lines = wOut.split('\n', Qt::SkipEmptyParts);
                const QString firstPath = lines.isEmpty() ? QStringLiteral("(sin salida)") : lines.first().trimmed();
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: where %2 -> %3").arg(p.name, exeName, oneLine(firstPath)));
            } else {
                const QString reason = oneLine(wErr.isEmpty() ? QStringLiteral("not found (rc=%1)").arg(wRc) : wErr);
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("%1: where %2 -> %3").arg(p.name, exeName, reason));
            }
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
    } else {
        const bool useRemoteScriptsConn = !isWindowsConnection(p);
        const QStringList zfsVersionCandidates = useRemoteScriptsConn
            ? QStringList{remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-version"))}
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

    // Detección de comandos disponibles para mostrar en detalle de conexión.
    {
        const QStringList wanted = zfsmgrUnixCommandSet();
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

    const bool isWinConn = isWindowsConnection(p);
    const bool useRemoteScripts = !isWinConn;

    bool gsaSupportsGuidSnapshotCompare = true;
    {
        QString gsaOut;
        QString gsaErr;
        int gsaRc = -1;
        QString gsaProbeCmd;
        WindowsCommandMode gsaWinMode = WindowsCommandMode::Auto;
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
        } else if (useRemoteScripts) {
            gsaProbeCmd = withSudo(p, remoteScriptCommand(p, QStringLiteral("zfsmgr-gsa-status")));
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
        }
        if (runSsh(p, gsaProbeCmd, 15000, gsaOut, gsaErr, gsaRc, {}, {}, {}, gsaWinMode)) {
            const QMap<QString, QString> kv = parseKeyValueOutput(gsaOut + QStringLiteral("\n") + gsaErr);
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
    if (state.gsaInstalled && !isWinConn) {
        QString mapOut;
        QString mapErr;
        int mapRc = -1;
        const QString cmd =
            useRemoteScripts
                ? withSudo(p, remoteScriptCommand(p, QStringLiteral("zfsmgr-gsa-connections-cat")))
                : withSudo(
                      p,
                      QStringLiteral("if [ -f /etc/zfsmgr/gsa-connections.conf ]; then cat /etc/zfsmgr/gsa-connections.conf; fi"));
        if (runSsh(p, cmd, 12000, mapOut, mapErr, mapRc) && mapRc == 0) {
            state.gsaKnownConnections = parseGsaKnownConnections(mapOut);
        }
    }
    const QString zpoolListCmd = withSudo(
        p, useRemoteScripts
               ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-list-json"))
               : mwhelpers::withUnixSearchPathCommand(
                     isWinConn
                         ? QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio")
                         : QStringLiteral("zpool list -j")));
    out.clear(); err.clear(); rc = -1;
    if (runSsh(p, zpoolListCmd, 18000, out, err, rc) && rc == 0) {
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

    for (const PoolImported& pool : state.importedPools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        if (state.poolGuidByName.value(poolName).trimmed().isEmpty()) {
            QString gout;
            QString gerr;
            int grc = -1;
            const QString guidCmd = withSudo(
                p,
                useRemoteScripts
                    ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-guid"), {poolName})
                    : mwhelpers::withUnixSearchPathCommand(
                          QStringLiteral("zpool get -H -o value guid %1").arg(mwhelpers::shSingleQuote(poolName))));
            if (runSsh(p, guidCmd, 12000, gout, gerr, grc) && grc == 0) {
                const QString guid = gout.section('\n', 0, 0).trimmed();
                if (!guid.isEmpty() && guid != QStringLiteral("-")) {
                    state.poolGuidByName.insert(poolName, guid);
                }
            }
        }
        out.clear();
        err.clear();
        rc = -1;
        const QString stCmd = withSudo(
            p,
            useRemoteScripts
                ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-status"), {poolName})
                : mwhelpers::withUnixSearchPathCommand(
                      QStringLiteral("zpool status -v %1").arg(mwhelpers::shSingleQuote(poolName))));
        if (runSsh(p, stCmd, 20000, out, err, rc) && rc == 0) {
            state.poolStatusByName.insert(poolName, out.trimmed());
        } else {
            const QString fallback = !out.trimmed().isEmpty() ? out.trimmed() : err.trimmed();
            state.poolStatusByName.insert(poolName, fallback);
        }
    }

    {
        out.clear();
        err.clear();
        rc = -1;
        const QString cmd = withSudo(
            p,
            useRemoteScripts
                ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-import-probe"))
                : mwhelpers::withUnixSearchPathCommand(QStringLiteral("zpool import; zpool import -s")));
        if (runSsh(p, cmd, 18000, out, err, rc)) {
            const QString merged = out + QStringLiteral("\n") + err;
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

    out.clear();
    err.clear();
    rc = -1;
    const QString mountedCmd = withSudo(
        p,
        (!isWinConn)
            ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-mount-list"))
            : mwhelpers::withUnixSearchPathCommand(QStringLiteral("zfs mount")));
    QVector<QPair<QString, QString>> mountedRows;
    if (runSsh(p, mountedCmd, 18000, out, err, rc) && rc == 0) {
        mountedRows = isWinConn
            ? mwhelpers::parseZfsMountOutput(out)
            : mwhelpers::parseZfsMountJsonOutput(out);
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
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zfs mount -> %2").arg(p.name, oneLine(err)));
    }
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
            for (const PoolImported& pool : state.importedPools) {
                const QString poolName = pool.pool.trimmed();
                if (poolName.isEmpty()) {
                    continue;
                }
                QString gout;
                QString gerr;
                int grc = -1;
                const QString gsaProps = QStringLiteral("org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino");
                const QString gcmd = withSudo(
                    p,
                    useRemoteScripts
                        ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-get-gsa-json-recursive"), {poolName})
                        : QStringLiteral("zfs get -j -r %1 %2")
                              .arg(gsaProps, mwhelpers::shSingleQuote(poolName)));
                if (!runSsh(p, gcmd, 20000, gout, gerr, grc) || grc != 0) {
                    continue;
                }
                QMap<QString, QMap<QString, QString>> propsByDataset;
                {
                    const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(gout).toUtf8());
                    const QJsonObject datasets = doc.object().value(QStringLiteral("datasets")).toObject();
                    for (auto dsIt = datasets.constBegin(); dsIt != datasets.constEnd(); ++dsIt) {
                        const QString datasetName = dsIt.key();
                        if (datasetName.isEmpty() || datasetName.contains(QLatin1Char('@'))) {
                            continue;
                        }
                        const QJsonObject props = dsIt.value().toObject()
                                                      .value(QStringLiteral("properties")).toObject();
                        for (auto pIt = props.constBegin(); pIt != props.constEnd(); ++pIt) {
                            const QString val = pIt.value().toObject()
                                                    .value(QStringLiteral("value")).toString().trimmed();
                            propsByDataset[datasetName].insert(pIt.key(), val);
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
            }
            state.gsaRequiredConnections = QStringList(requiredConnections.begin(), requiredConnections.end());
            std::sort(state.gsaRequiredConnections.begin(), state.gsaRequiredConnections.end(),
                      [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
            if (toLowerSet(state.gsaKnownConnections) != toLowerSet(state.gsaRequiredConnections)) {
                state.gsaNeedsAttention = true;
                state.gsaAttentionReasons.push_back(QStringLiteral("conexiones GSA desactualizadas"));
            }
        }
    }

    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_fin_refres_6eead9"),
               QStringLiteral("Fin refresh: %1 -> OK (%2)"),
               QStringLiteral("Refresh end: %1 -> OK (%2)"),
               QStringLiteral("刷新结束：%1 -> OK (%2)")).arg(p.name, state.detail));
    return state;
}
