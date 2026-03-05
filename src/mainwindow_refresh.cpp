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
        QStringLiteral("zfs"),
        QStringLiteral("zpool"),
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
    state.connectionMethod = p.connType;
    state.powershellFallbackCommands = zfsmgrPowershellCommandSet();
    appLog(QStringLiteral("NORMAL"),
           tr3(QStringLiteral("Inicio refresh: %1 [%2]"),
               QStringLiteral("Refresh start: %1 [%2]"),
               QStringLiteral("开始刷新：%1 [%2]")).arg(p.name, p.connType));

    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = tr3(QStringLiteral("Tipo de conexión no soportado aún en cppqt"),
                           QStringLiteral("Connection type not supported yet in cppqt"),
                           QStringLiteral("cppqt 尚不支持该连接类型"));
        appLog(QStringLiteral("NORMAL"),
               tr3(QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(p.name, state.detail));
        return state;
    }
    if (p.host.isEmpty() || p.username.isEmpty()) {
        state.status = QStringLiteral("ERROR");
        state.detail = tr3(QStringLiteral("Host/usuario no definido"),
                           QStringLiteral("Host/user not defined"),
                           QStringLiteral("主机/用户未定义"));
        appLog(QStringLiteral("NORMAL"),
               tr3(QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(p.name, state.detail));
        return state;
    }

    QString out;
    QString err;
    int rc = -1;
    const QString osProbeCmd = isWindowsConnection(p)
                                   ? QStringLiteral("[System.Environment]::OSVersion.VersionString")
                                   : QStringLiteral("uname -a");
    if (!runSsh(p, osProbeCmd, 12000, out, err, rc) || rc != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
        appLog(QStringLiteral("NORMAL"),
               tr3(QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(p.name, state.detail));
        return state;
    }
    state.status = QStringLiteral("OK");
    state.detail = oneLine(out);
    state.osLine = oneLine(out);
    state.zfsVersion.clear();

    if (isWindowsConnection(p)) {
        auto logWhere = [&](const QString& exeName) {
            QString wOut, wErr;
            int wRc = -1;
            const QString whereCmd = QStringLiteral("where.exe %1").arg(exeName);
            if (runSsh(p, whereCmd, 12000, wOut, wErr, wRc) && wRc == 0) {
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
    rc = -1;
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
        if (!runSsh(p, zfsVersionCmd, 12000, out, err, rc)) {
            continue;
        }
        const QString parsed = mwhelpers::parseOpenZfsVersionText(out + QStringLiteral("\n") + err);
        if (!parsed.isEmpty()) {
            state.zfsVersion = parsed;
            state.zfsVersionFull = oneLine((out + QStringLiteral(" ") + err).simplified());
            break;
        }
        if (rc == 0) {
            // Algunos builds de OpenZFS en Windows imprimen solo el número de versión.
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
                "if($present.Count -eq 0){ Write-Output '__NO_UNIX_LAYER__'; exit 0 }; "
                "$cmds='%1'.Split(' '); "
                "foreach($c in $cmds){ $ok=$false; foreach($r in $present){ if(Test-Path -LiteralPath (Join-Path $r ($c + '.exe'))){ $ok=$true; break } }; "
                "if($ok){ Write-Output ('OK:' + $c) } else { Write-Output ('KO:' + $c) } }")
                    .arg(wanted.join(' '));
            if (runSsh(p, roots, 15000, dout, derr, drc) && drc == 0) {
                const QStringList lines = dout.split('\n', Qt::SkipEmptyParts);
                bool noLayer = false;
                for (const QString& raw : lines) {
                    const QString line = raw.trimmed();
                    if (line == QStringLiteral("__NO_UNIX_LAYER__")) {
                        noLayer = true;
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
                } else {
                    state.unixFromMsysOrMingw = false;
                    state.detectedUnixCommands.clear();
                    state.missingUnixCommands.clear();
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
            } else {
                missing = wanted;
            }
            state.detectedUnixCommands = detected;
            state.missingUnixCommands = missing;
            state.unixFromMsysOrMingw = false;
        }
    }

    QString zpoolListCmd = withSudo(p, QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio"));

    out.clear();
    err.clear();
    rc = -1;
    if (runSsh(p, zpoolListCmd, 18000, out, err, rc) && rc == 0) {
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

    const QStringList importProbeArgs = {
        QStringLiteral("zpool import"),
        QStringLiteral("zpool import -s"),
    };
    bool importablesFound = false;
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
            importablesFound = true;
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
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString ln = raw.trimmed();
            if (ln.isEmpty()) {
                continue;
            }
            const int sp = ln.indexOf(' ');
            if (sp <= 0) {
                continue;
            }
            const QString ds = ln.left(sp).trimmed();
            const QString mp = ln.mid(sp + 1).trimmed();
            if (!ds.isEmpty() && !mp.isEmpty()) {
                state.mountedDatasets.push_back(qMakePair(ds, mp));
            }
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zfs mount -> %2").arg(p.name, oneLine(err)));
    }

    appLog(QStringLiteral("NORMAL"),
           tr3(QStringLiteral("Fin refresh: %1 -> OK (%2)"),
               QStringLiteral("Refresh end: %1 -> OK (%2)"),
               QStringLiteral("刷新结束：%1 -> OK (%2)")).arg(p.name, state.detail));
    return state;
}
