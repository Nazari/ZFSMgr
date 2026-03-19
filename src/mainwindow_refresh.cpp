#include "mainwindow.h"
#include "mainwindow_helpers.h"

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

    const bool localMode = isLocalConnection(profile);
    if (localMode) {
        QString libzfsDetail;
        const bool hasLibzfs = detectLocalLibzfs(&libzfsDetail);
        state.connectionMethod = hasLibzfs ? QStringLiteral("LOCAL/libzfs") : QStringLiteral("LOCAL/CLI");
        appLog(hasLibzfs ? QStringLiteral("INFO") : QStringLiteral("WARN"),
               QStringLiteral("%1: %2 (%3)")
                   .arg(profile.name, state.connectionMethod, libzfsDetail));
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

    QString out;
    QString err;
    int rc = -1;
    const QString osProbeCmd = isWindowsConnection(p)
                                   ? QStringLiteral("[System.Environment]::OSVersion.VersionString")
                                   : QStringLiteral("uname -a");
    const MainWindow::WindowsCommandMode winPsMode = MainWindow::WindowsCommandMode::PowerShellNative;
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
        const QStringList zfsVersionCandidates = {
            QStringLiteral("zfs version"),
            QStringLiteral("zfs --version"),
            QStringLiteral("zpool --version"),
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
                "PATH=\"$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin\"; "
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

    QString zpoolListCmd = withSudo(p, QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio"));
    bool loadedPoolsFromLibzfs = false;
    if (localMode) {
        QStringList localPools;
        QString poolLibDetail;
        if (detectLocalLibzfs() && listLocalImportedPoolsLibzfs(localPools, &poolLibDetail)) {
            for (const QString& poolName : localPools) {
                if (poolName.trimmed().isEmpty()) {
                    continue;
                }
                state.importedPools.push_back(PoolImported{p.name, poolName.trimmed(), QStringLiteral("Exportar")});
            }
            loadedPoolsFromLibzfs = true;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: imported pools via libzfs (%2)")
                       .arg(p.name)
                       .arg(poolLibDetail));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    if (!loadedPoolsFromLibzfs && runSsh(p, zpoolListCmd, 18000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString poolName = line.section('\t', 0, 0).trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            state.importedPools.push_back(PoolImported{p.name, poolName, QStringLiteral("Exportar")});
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zpool list -> %2").arg(p.name, oneLine(err)));
    }

    for (const PoolImported& pool : state.importedPools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        out.clear();
        err.clear();
        rc = -1;
        const QString stCmd = withSudo(
            p, QStringLiteral("zpool status -v %1").arg(mwhelpers::shSingleQuote(poolName)));
        if (runSsh(p, stCmd, 20000, out, err, rc) && rc == 0) {
            state.poolStatusByName.insert(poolName, out.trimmed());
        } else {
            const QString fallback = !out.trimmed().isEmpty() ? out.trimmed() : err.trimmed();
            state.poolStatusByName.insert(poolName, fallback);
        }
    }

    const QStringList importProbeArgs = {
        QStringLiteral("zpool import"),
        QStringLiteral("zpool import -s"),
    };
    for (const QString& probe : importProbeArgs) {
        out.clear();
        err.clear();
        rc = -1;
        const QString cmd = withSudo(p, probe);
        if (!runSsh(p, cmd, 18000, out, err, rc)) {
            continue;
        }
        const QString merged = out + QStringLiteral("\n") + err;
        const QVector<mwhelpers::ImportablePoolInfo> parsed = mwhelpers::parseZpoolImportOutput(merged);
        if (!parsed.isEmpty()) {
            state.importablePools.clear();
            for (const auto& row : parsed) {
                state.importablePools.push_back(PoolImportable{
                    p.name,
                    row.pool,
                    row.state,
                    row.reason,
                    QStringLiteral("Importar"),
                });
            }
            break;
        }
        if (!err.isEmpty()) {
            appLog(QStringLiteral("INFO"), QStringLiteral("%1: %2 -> %3").arg(p.name, probe, oneLine(err)));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    const QString mountedCmd = withSudo(p, QStringLiteral("zfs mount"));
    if (runSsh(p, mountedCmd, 18000, out, err, rc) && rc == 0) {
        const QVector<QPair<QString, QString>> mountedRows = mwhelpers::parseZfsMountOutput(out);
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

    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_fin_refres_6eead9"),
               QStringLiteral("Fin refresh: %1 -> OK (%2)"),
               QStringLiteral("Refresh end: %1 -> OK (%2)"),
               QStringLiteral("刷新结束：%1 -> OK (%2)")).arg(p.name, state.detail));
    return state;
}
