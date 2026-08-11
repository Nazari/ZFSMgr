#include "mainwindow.h"
#include "agentversion.h"
#include "mainwindow_helpers.h"
#include "daemonpayload.h"

#include <QtWidgets>
#include <QRegularExpression>
#include <algorithm>
#include <functional>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;

QString unixAlternateMountHelpersScript() {
    return QStringLiteral(
        "mount_alt_zfs(){ ds=\"$1\"; mp=\"$2\"; "
        "saved_prop='org.fc16.zfsmgr:savedmountpoint'; "
        "current_mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
        "zfs set \"$saved_prop=$current_mp\" \"$ds\"; "
        "zfs set mountpoint=\"$mp\" \"$ds\"; "
        "zfs mount \"$ds\" >/dev/null 2>&1 || true; }; "
        "umount_alt_zfs(){ ds=\"$1\"; mp=\"$2\"; "
        "saved_prop='org.fc16.zfsmgr:savedmountpoint'; "
        "zfs unmount \"$ds\" >/dev/null 2>&1 || zfs unmount \"$mp\" >/dev/null 2>&1 || true; "
        "saved_mp=$(zfs get -H -o value \"$saved_prop\" \"$ds\" 2>/dev/null || true); "
        "if [ -n \"$saved_mp\" ] && [ \"$saved_mp\" != \"-\" ]; then zfs set mountpoint=\"$saved_mp\" \"$ds\" >/dev/null 2>&1 || true; fi; "
        "zfs inherit \"$saved_prop\" \"$ds\" >/dev/null 2>&1 || true; }; "
        "resolve_mp(){ ds=\"$1\"; zfs mount 2>/dev/null | awk -v d=\"$ds\" '$1==d{print $2; exit}'; }; "
        "load_key_if_needed(){ ds=\"$1\"; "
        "ks=$(zfs get -H -o value keystatus \"$ds\" 2>/dev/null || true); "
        "[ \"$ks\" = \"available\" ] && return 0; "
        "kl=$(zfs get -H -o value keylocation \"$ds\" 2>/dev/null || true); "
        "if [ \"$kl\" = \"prompt\" ]; then "
        "  if [ \"$ZFSMGR_PASS_READ\" != 1 ]; then IFS= read -r ZFSMGR_KEY_PASS || return 1; ZFSMGR_PASS_READ=1; fi; "
        "  printf '%s\\n' \"$ZFSMGR_KEY_PASS\" | zfs load-key \"$ds\" >/dev/null; "
        "else "
        "  zfs load-key \"$ds\" >/dev/null 2>&1 || true; "
        "fi; }; ");
}
} // namespace

void MainWindow::actionAdvancedBreakdown() {
    actionAdvancedBreakdown(currentConnContentSelection(m_connContentTree));
}

void MainWindow::actionAdvancedBreakdown(const DatasetSelectionContext& explicitCtx) {
    const DatasetSelectionContext curr = explicitCtx.valid ? explicitCtx : currentConnContentSelection(m_connContentTree);
    if (!curr.valid || curr.datasetName.isEmpty() || !curr.snapshotName.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_sel_ds_001"), QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    if (!requireFeature(curr.connIdx, zfsmgr::caps::Feature::DirBreakdown)) {
        return;
    }
    const QString ds = curr.datasetName;
    if (ds.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = curr.connIdx;
    ctx.poolName = curr.poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();
    beginUiBusy();
    bool busyActive = true;
    auto stopBusy = [&]() {
        if (busyActive) {
            endUiBusy();
            busyActive = false;
        }
    };
    const ConnectionProfile p = m_profiles[ctx.connIdx];
    const bool daemonReadApiOk =
        ctx.connIdx >= 0
        && ctx.connIdx < m_states.size()
        && m_states[ctx.connIdx].daemonInstalled
        && m_states[ctx.connIdx].daemonActive
        && m_states[ctx.connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    QString mountedValue;
    if (!getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mounted"), mountedValue)) {
        stopBusy();
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_adv_chk_mnt_01"), QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    const bool rootMounted = isMountedValueTrue(mountedValue);
    const bool canTempMount = supportsAlternateDatasetMount(ctx.connIdx);
    QByteArray mountStdinPayload;

    QStringList dirs;
    QString resolvedMp;
    QString listOut;
    QString listErr;
    int listRc = -1;
    if (isWindowsConnection(p)) {
        QString dsPs = ds;
        dsPs.replace('\'', QStringLiteral("''"));
        const QString listCmd = QStringLiteral(
                                    "$ds='%1'; "
                                    "function Resolve-Mp([string]$name){ "
                                    "  $cur=$name; $anchor=$null; $drive=''; "
                                    "  while($true){ "
                                    "    $dl=(zfs get -H -o value driveletter $cur 2>$null | Out-String).Trim(); "
                                    "    if(-not [string]::IsNullOrWhiteSpace($dl) -and $dl -ne '-' -and $dl.ToLower() -ne 'none'){ $drive=$dl; $anchor=$cur; break }; "
                                    "    if($cur -notmatch '/'){ break }; "
                                    "    $cur=$cur.Substring(0,$cur.LastIndexOf('/')); "
                                    "  }; "
                                    "  if([string]::IsNullOrWhiteSpace($drive)){ return '' }; "
                                    "  $drive=$drive.Trim(); "
                                    "  if($drive.Length -gt 0 -and $drive[$drive.Length-1] -eq ':'){ $drive=$drive.Substring(0,$drive.Length-1) }; "
                                    "  $drive=$drive.Substring(0,1).ToUpper(); "
                                    "  $base=$drive + ':\\\\'; "
                                    "  if($name -eq $anchor){ return $base }; "
                                    "  $suffix=$name.Substring($anchor.Length).TrimStart('/'); "
                                    "  if([string]::IsNullOrWhiteSpace($suffix)){ return $base }; "
                                    "  return ($base + ($suffix -replace '/','\\\\')) "
                                    "}; "
                                    "$mp=Resolve-Mp $ds; "
                                    "if (-not [string]::IsNullOrWhiteSpace($mp)) { Write-Output ('__MP__=' + $mp) }; "
                                    "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { exit 0 }; "
                                    "$base=[System.IO.Path]::GetFullPath($mp).TrimEnd('\\\\'); "
                                    "Get-ChildItem -LiteralPath $mp -Directory -Recurse -Force | "
                                    "Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | "
                                    "ForEach-Object { "
                                    "  $full=[System.IO.Path]::GetFullPath($_.FullName).TrimEnd('\\\\'); "
                                    "  if (-not $full.StartsWith($base,[System.StringComparison]::OrdinalIgnoreCase)) { return }; "
                                    "  $rel=$full.Substring($base.Length).TrimStart('\\\\'); "
                                    "  if ([string]::IsNullOrWhiteSpace($rel)) { return }; "
                                    "  if ($rel.StartsWith('.zfs\\\\',[System.StringComparison]::OrdinalIgnoreCase) -or $rel.Equals('.zfs',[System.StringComparison]::OrdinalIgnoreCase)) { return }; "
                                    "  Write-Output ($rel -replace '\\\\','/') "
                                    "} | Sort-Object -Unique")
                                    .arg(dsPs);
        if (!runSsh(p, withSudo(p, listCmd), 180000, listOut, listErr, listRc) || listRc != 0) {
            stopBusy();
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_break_ls01"), QStringLiteral("No se pudieron listar directorios para desglosar."),
                                     QStringLiteral("Could not list directories for breakdown."),
                                     QStringLiteral("无法列出可拆分目录。")));
            return;
        }
        const QStringList lines = listOut.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString ln = raw.trimmed();
            if (ln.startsWith(QStringLiteral("__MP__="))) {
                resolvedMp = ln.mid(QStringLiteral("__MP__=").size()).trimmed();
            } else if (!ln.isEmpty()) {
                dirs << ln;
            }
        }
    } else {
        if (!rootMounted && !canTempMount) {
            stopBusy();
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_adv_break_mnt1"), QStringLiteral("Desglosar requiere dataset montado, o un sistema que permita montaje temporal alternativo."),
                    QStringLiteral("Break down requires the dataset mounted, or a system that supports temporary alternate mounts."),
                    QStringLiteral("拆分要求数据集已挂载，或系统支持临时替代挂载。")));
            return;
        }
        QString listScript;
        QString remoteListScriptCmd;
        if (!isWindowsConnection(p)) {
            remoteListScriptCmd = mwhelpers::withUnixSearchPathCommand(
                daemonpayload::unixBinPath() + QStringLiteral(" --dump-advanced-breakdown-list %1")
                    .arg(shSingleQuote(ds)));
        } else {
            listScript =
                QStringLiteral("set -e; DATASET=%1; ZFSMGR_PASS_READ=0; ZFSMGR_KEY_PASS=''; %2"
                               "TMP_ROOT=''; "
                               "cleanup(){ "
                               "if [ -n \"$TMP_ROOT\" ]; then umount_alt_zfs \"$DATASET\" \"$TMP_ROOT\" >/dev/null 2>&1 || true; fi; "
                               "if [ -n \"$TMP_ROOT\" ]; then rmdir \"$TMP_ROOT\" >/dev/null 2>&1 || true; fi; "
                               "}; "
                               "trap cleanup EXIT INT TERM; "
                               "MP=$(resolve_mp \"$DATASET\"); "
                               "if [ -z \"$MP\" ]; then "
                               "  load_key_if_needed \"$DATASET\"; "
                               "  TMP_ROOT=$(mktemp -d /tmp/zfsmgr-break-root-XXXXXX); "
                               "  mount_alt_zfs \"$DATASET\" \"$TMP_ROOT\"; "
                               "  MP=$(resolve_mp \"$DATASET\"); "
                               "fi; "
                               "[ -n \"$MP\" ] || exit 0; "
                               "[ -d \"$MP\" ] || exit 0; "
                               "printf '__MP__=%s\\n' \"$MP\"; "
                               "find \"$MP\" -mindepth 1 -type d -print 2>/dev/null | while IFS= read -r d; do "
                               "  rel=\"$d\"; case \"$rel\" in \"$MP\"/*) rel=${rel#\"$MP\"/} ;; *) rel='' ;; esac; "
                               "  [ -n \"$rel\" ] || continue; "
                               "  case \"$rel\" in .zfs|.zfs/*) continue ;; esac; "
                               "  printf '%s\\n' \"$rel\"; "
                               "done | sort -u")
                    .arg(shSingleQuote(ds), unixAlternateMountHelpersScript());
        }
        QString keyLocation;
        getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("keylocation"), keyLocation);
        keyLocation = keyLocation.trimmed().toLower();
        QString keyStatus;
        getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("keystatus"), keyStatus);
        keyStatus = keyStatus.trimmed().toLower();
        if (!rootMounted
            && keyLocation == QStringLiteral("prompt")
            && keyStatus != QStringLiteral("available")) {
            bool ok = false;
            const QString passphrase = QInputDialog::getText(
                this,
                QStringLiteral("Load key"),
                QStringLiteral("Clave"),
                QLineEdit::Password,
                QString(),
                &ok);
            if (!ok || passphrase.isEmpty()) {
                stopBusy();
                return;
            }
            mountStdinPayload = (passphrase + QStringLiteral("\n")).toUtf8();
        }
        const QString effectiveListCmd = mountStdinPayload.isEmpty()
            ? withSudo(p,
                       remoteListScriptCmd.isEmpty()
                           ? mwhelpers::withUnixSearchPathCommand(listScript)
                           : remoteListScriptCmd)
            : withSudoStreamInput(p,
                                  remoteListScriptCmd.isEmpty()
                                      ? mwhelpers::withUnixSearchPathCommand(listScript)
                                      : remoteListScriptCmd);
        if (!runSsh(p,
                    effectiveListCmd,
                    180000,
                    listOut,
                    listErr,
                    listRc,
                    {},
                    {},
                    {},
                    mountStdinPayload)
            || listRc != 0) {
            stopBusy();
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_break_ls01"), QStringLiteral("No se pudieron listar directorios para desglosar."),
                                     QStringLiteral("Could not list directories for breakdown."),
                                     QStringLiteral("无法列出可拆分目录。")));
            return;
        }
        const QStringList lines = listOut.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString ln = raw.trimmed();
            if (ln.startsWith(QStringLiteral("__MP__="))) {
                resolvedMp = ln.mid(QStringLiteral("__MP__=").size()).trimmed();
            } else if (!ln.isEmpty()) {
                dirs << ln;
            }
        }
        dirs.removeAll(QString());
    }

    QString dsListOut;
    QString dsListErr;
    int dsListRc = -1;
    QStringList datasetsDetected;
    QSet<QString> childDatasetPathsLower;
    QString dsListCmd;
    if (!isWindowsConnection(p)) {
        if (daemonReadApiOk) {
            dsListCmd = mwhelpers::agentShellCommand(p, {QStringLiteral("--dump-zfs-list-children"), ds});
        } else {
            dsListCmd = withSudo(
                p, mwhelpers::withUnixSearchPathCommand(
                       QStringLiteral("zfs list -H -o name -r %1")
                           .arg(shSingleQuote(ds))));
        }
    } else {
        dsListCmd = withSudo(
            p,
            QStringLiteral("zfs list -H -o name -r %1")
                .arg(shSingleQuote(ds)));
    }
    if (runSsh(p, dsListCmd, 180000, dsListOut, dsListErr, dsListRc) && dsListRc == 0) {
        datasetsDetected = dsListOut.split('\n', Qt::SkipEmptyParts);
        for (QString& n : datasetsDetected) {
            n = n.trimmed();
        }
        datasetsDetected.removeAll(QString());
        const QString prefix = ds + QStringLiteral("/");
        for (const QString& datasetName : datasetsDetected) {
            if (!datasetName.startsWith(prefix)) {
                continue;
            }
            const QString childPath = datasetName.mid(prefix.size()).trimmed();
            if (!childPath.isEmpty()) {
                childDatasetPathsLower.insert(childPath.toLower());
            }
        }
    }
    if (!childDatasetPathsLower.isEmpty()) {
        dirs.erase(std::remove_if(dirs.begin(), dirs.end(),
                                  [&](const QString& d) {
                                      const QString path = d.trimmed().toLower();
                                      if (path.isEmpty()) {
                                          return true;
                                      }
                                      for (const QString& dsPath : childDatasetPathsLower) {
                                          if (path == dsPath || path.startsWith(dsPath + QStringLiteral("/"))) {
                                              return true;
                                          }
                                      }
                                      return false;
                                  }),
                   dirs.end());
    }
    // Aquí había un filtro por punto de montaje que descartaba dos cosas distintas y las
    // dos hay que ofrecerlas:
    //
    //   - el directorio que YA ES el punto de montaje de un subdataset con otro nombre
    //     —lo que deja un Ensamblar que los conservó—: desglosarlo es justo lo que le
    //     devuelve el nombre canónico y restaura el paralelismo nombre/ruta;
    //   - el directorio que CONTIENE uno: se descartaba por `startsWith`, así que tras
    //     un Ensamblar el padre dejaba de poder desglosarse y el viaje de ida y vuelta
    //     quedaba cortado a la mitad.
    //
    // El filtro tapaba, de paso, que el daemon borraba el original con `remove_all` sin
    // mirar los puntos de montaje. Eso ya está resuelto allí (desmonta antes, remonta
    // después, y el borrado se planta si cambia de dispositivo), así que el filtro
    // sobra. Los subdatasets con nombre canónico los sigue quitando el filtro por
    // NOMBRE de más arriba, que es donde corresponde: ahí no hay nada que hacer.
    //
    // Con el filtro se va también su consulta, que era un `zfs mount | awk` por shell.
    if (dirs.isEmpty()) {
        const QString dirsText = dirs.isEmpty()
                                     ? trk(QStringLiteral("t_none_txt_0001"), QStringLiteral("(ninguno)"), QStringLiteral("(none)"), QStringLiteral("（无）"))
                                     : dirs.join('\n');
        const QString datasetsText = datasetsDetected.isEmpty()
                                         ? trk(QStringLiteral("t_none_txt_0001"), QStringLiteral("(ninguno)"), QStringLiteral("(none)"), QStringLiteral("（无）"))
                                         : datasetsDetected.join('\n');
        const QString mpText = resolvedMp.isEmpty()
                                   ? trk(QStringLiteral("t_unresolved001"), QStringLiteral("(sin resolver)"), QStringLiteral("(unresolved)"), QStringLiteral("（未解析）"))
                                   : resolvedMp;

        stopBusy();
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_break_nod1"), QStringLiteral("No hay directorios para desglosar en el dataset seleccionado.\n\n"
                                                    "Dataset: %1\n"
                                                    "Mountpoint resuelto: %2\n\n"
                                                    "Directorios detectados:\n%3\n\n"
                                                    "Datasets detectados:\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText),
                                     QStringLiteral("No directories available to break down in selected dataset.\n\n"
                                                    "Dataset: %1\n"
                                                    "Resolved mountpoint: %2\n\n"
                                                    "Detected directories:\n%3\n\n"
                                                    "Detected datasets:\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText),
                                     QStringLiteral("所选数据集中没有可拆分目录。\n\n"
                                                    "数据集：%1\n"
                                                    "解析后的挂载点：%2\n\n"
                                                    "检测到的目录：\n%3\n\n"
                                                    "检测到的数据集：\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText)));
        return;
    }
    stopBusy();
    QStringList selectedDirs;
    QMap<QString, QString> invalidDirReasons;
    auto invalidDatasetPathReason = [](const QString& path) -> QString {
        const QString p = path.trimmed();
        if (p.isEmpty()) {
            return QStringLiteral("empty");
        }
        const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            return QStringLiteral("empty");
        }
        for (const QString& rawPart : parts) {
            const QString n = rawPart.trimmed();
            if (n.isEmpty()) {
                return QStringLiteral("empty path component");
            }
            if (n == QStringLiteral(".") || n == QStringLiteral("..")) {
                return QStringLiteral("reserved name");
            }
            for (const QChar c : n) {
                const ushort uc = c.unicode();
                if (uc < 0x20 || uc == 0x7F) {
                    return QStringLiteral("control character");
                }
            }
            // ZFS solo admite [a-zA-Z0-9_.:-] y espacio en un nombre de dataset. Antes
            // aquí solo se miraban '@', '#' y ',', así que un directorio con apóstrofo o
            // con acento —«Alessandro D'Avenia», «Canción»— se dejaba seleccionar y
            // después bloqueaba el botón Aceptar sin explicar por qué. En una biblioteca
            // de Calibre eso son miles de directorios y el diálogo quedaba inservible.
            for (const QChar c : n) {
                const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                                || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                                || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                                || c == QLatin1Char('_') || c == QLatin1Char('.')
                                || c == QLatin1Char(':') || c == QLatin1Char('-')
                                || c == QLatin1Char(' ');
                if (!ok) {
                    return QStringLiteral("ZFS no admite '%1' en un nombre de dataset").arg(c);
                }
            }
        }
        return QString();
    };
    for (const QString& dir : dirs) {
        const QString reason = invalidDatasetPathReason(dir);
        if (!reason.isEmpty()) {
            invalidDirReasons.insert(dir, reason);
        }
    }
    // Nombre del dataset en el que se convertirá cada directorio. No tiene por qué
    // reflejar la ruta: `zfs create` solo exige que exista el dataset PADRE por nombre,
    // no que el directorio de encima sea un dataset. Así se puede convertir `a/b/c`
    // dejando `a` y `b` como directorios normales, con el nombre plano `a:b:c`.
    // Los tramos que sí serán dataset se separan con '/', los que no con ':' — de los
    // caracteres que ZFS admite ([a-zA-Z0-9_.:- ] y espacio) es el que menos se
    // confunde con un nombre de directorio corriente.
    MainWindow::TreeNameColumn nameColumn;
    nameColumn.header = trk(QStringLiteral("t_col_dataset_001"),
                            QStringLiteral("Dataset resultante"),
                            QStringLiteral("Resulting dataset"),
                            QStringLiteral("生成的数据集"));
    nameColumn.editable = true;
    nameColumn.takenNames = childDatasetPathsLower;
    nameColumn.propose = [childDatasetPathsLower](const QString& path,
                                                  const QSet<QString>& checked) {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString name;
        QString prefix;
        for (int i = 0; i < parts.size(); ++i) {
            if (i == 0) {
                name = parts[0];
                prefix = parts[0];
                continue;
            }
            const bool prefixIsDataset = checked.contains(prefix)
                                         || childDatasetPathsLower.contains(prefix.toLower());
            name += (prefixIsDataset ? QStringLiteral("/") : QStringLiteral(":")) + parts[i];
            prefix += QStringLiteral("/") + parts[i];
        }
        return name;
    };

    if (!selectTreeItemsDialog(
            trk(QStringLiteral("t_adv_break_tit1"), QStringLiteral("Desglosar: seleccionar directorios"),
                QStringLiteral("Break down: select directories"),
                QStringLiteral("拆分：选择目录")),
            trk(QStringLiteral("t_adv_break_msg1"), QStringLiteral("Seleccione los directorios que desea desglosar en subdatasets."),
                QStringLiteral("Select directories to split into subdatasets."),
                QStringLiteral("请选择要拆分为子数据集的目录。")),
            dirs,
            selectedDirs,
            trk(QStringLiteral("t_adv_break_mp001"),
                QStringLiteral("Mountpoint usado para explorar directorios: %1").arg(resolvedMp),
                QStringLiteral("Mountpoint used to scan directories: %1").arg(resolvedMp),
                QStringLiteral("用于扫描目录的挂载点：%1").arg(resolvedMp)),
            invalidDirReasons,
            isWindowsConnection(p) ? nullptr : &nameColumn)) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_adv_break_can1"), QStringLiteral("Desglosar cancelado o sin selección."),
                   QStringLiteral("Break down canceled or no selection."),
                   QStringLiteral("拆分已取消或无选择。")));
        return;
    }
    for (const QString& d : selectedDirs) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("[BREAKDOWN] pendiente: %1 -> %2")
                                             .arg(d, nameColumn.chosen.value(d, d)));
    }

    QString cmd;
    bool allowWindowsScript = false;
    if (isWindowsConnection(p)) {
        QStringList selectedPs;
        selectedPs.reserve(selectedDirs.size());
        for (QString d : selectedDirs) {
            d.replace('\'', QStringLiteral("''"));
            selectedPs << QStringLiteral("'%1'").arg(d);
        }
        QString dsPs = ds;
        dsPs.replace('\'', QStringLiteral("''"));
        cmd = QStringLiteral(
                  "$ds='%1'; "
                  "$selected=@(%2); "
                  "function Resolve-Mp([string]$name){ "
                  "  $cur=$name; $anchor=$null; $drive=''; "
                  "  while($true){ "
                  "    $dl=(zfs get -H -o value driveletter $cur 2>$null | Out-String).Trim(); "
                  "    if(-not [string]::IsNullOrWhiteSpace($dl) -and $dl -ne '-' -and $dl.ToLower() -ne 'none'){ $drive=$dl; $anchor=$cur; break }; "
                  "    if($cur -notmatch '/'){ break }; "
                  "    $cur=$cur.Substring(0,$cur.LastIndexOf('/')); "
                  "  }; "
                  "  if([string]::IsNullOrWhiteSpace($drive)){ return '' }; "
                  "  $drive=$drive.Trim(); if($drive.Length -gt 0 -and $drive[$drive.Length-1] -eq ':'){ $drive=$drive.Substring(0,$drive.Length-1) }; "
                  "  $drive=$drive.Substring(0,1).ToUpper(); "
                  "  $base=$drive + ':\\\\'; "
                  "  if($name -eq $anchor){ return $base }; "
                  "  $suffix=$name.Substring($anchor.Length).TrimStart('/'); "
                  "  if([string]::IsNullOrWhiteSpace($suffix)){ return $base }; "
                  "  return ($base + ($suffix -replace '/','\\\\')) "
                  "}; "
                  "$mp=Resolve-Mp $ds; "
                  "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { throw 'mountpoint=none' }; "
                  "foreach($bn in $selected){ "
                  "  $src=Join-Path $mp $bn; if (-not (Test-Path -LiteralPath $src -PathType Container)) { continue }; "
                  "  $srcItem=Get-Item -LiteralPath $src -Force -ErrorAction SilentlyContinue; "
                  "  if ($srcItem -and ($srcItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }; "
                  "  Write-Output ('[BREAKDOWN] start ' + $bn); "
                  "  $child=\"$ds/$bn\"; "
                  "  zfs list -H -o name $child 2>$null | Out-Null; if ($LASTEXITCODE -ne 0) { zfs create -p $child | Out-Null; if($LASTEXITCODE -ne 0){ throw \"zfs create failed for $child\" } }; "
                  "  zfs mount $child 2>$null | Out-Null; "
                  "  $cmp=Resolve-Mp $child; if ([string]::IsNullOrWhiteSpace($cmp)) { throw \"cannot resolve mountpoint for $child\" }; "
                  "  if (-not (Test-Path -LiteralPath $cmp)) { New-Item -ItemType Directory -Force -Path $cmp | Out-Null }; "
                  "  $srcNorm=[System.IO.Path]::GetFullPath($src).TrimEnd('\\\\'); "
                  "  $cmpNorm=[System.IO.Path]::GetFullPath($cmp).TrimEnd('\\\\'); "
                  "  if ($srcNorm.Equals($cmpNorm,[System.StringComparison]::OrdinalIgnoreCase)) { throw \"unsafe breakdown paths (same): $srcNorm\" }; "
                  "  if ($cmpNorm.StartsWith($srcNorm + '\\\\',[System.StringComparison]::OrdinalIgnoreCase)) { throw \"unsafe breakdown paths (dst under src): $srcNorm -> $cmpNorm\" }; "
                  "  if ($srcNorm.StartsWith($cmpNorm + '\\\\',[System.StringComparison]::OrdinalIgnoreCase)) { throw \"unsafe breakdown paths (src under dst): $srcNorm -> $cmpNorm\" }; "
                  "  robocopy $src $cmp /E /MOVE /COPYALL /R:1 /W:1 /NFL /NDL /NP | Out-Null; "
                  "  $r=$LASTEXITCODE; if ($r -ge 8) { throw \"robocopy failed ($r) for $bn\" }; "
                  "  Write-Output ('[BREAKDOWN] ok ' + $bn + ' -> ' + $child); "
                  "}")
                  .arg(dsPs, selectedPs.join(QStringLiteral(",")));
        allowWindowsScript = true;
    } else {
        if (!isWindowsConnection(p)) {
            QStringList argv{QStringLiteral("--mutate-advanced-breakdown"), ds};
            for (const QString& d : selectedDirs) {
                argv << d << nameColumn.chosen.value(d, d);
            }
            cmd = mwhelpers::agentShellCommand(p, argv);
            // Encolado, no ejecutado en el acto. Era la única familia de acciones que se
            // saltaba la lista de cambios pendientes, así que no había forma de revisarla
            // antes ni de quitarla de la cola. Al aplicarla conserva todo lo suyo:
            // confirmación, envío como trabajo del daemon y progreso en tiempo real.
            PendingShellActionDraft draft;
            // "conexión::pool", como el resto de entradas de la lista.
            draft.scopeLabel = [&]() {
                if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
                    return QString();
                }
                const ConnectionProfile& cp = m_profiles.at(ctx.connIdx);
                const QString conn = cp.name.trimmed().isEmpty() ? cp.id.trimmed()
                                                                 : cp.name.trimmed();
                return QStringLiteral("%1::%2").arg(conn, ctx.poolName.trimmed());
            }();
            draft.displayLabel = QStringLiteral("Desglosar %1").arg(ds);
            draft.command = cmd;
            draft.timeoutMs = 0;
            draft.streamProgress = true;
            draft.refreshTarget = ctx;
            draft.refreshScope = PendingShellActionDraft::RefreshScope::TargetOnly;
            draft.datasetActionSide = QStringLiteral("conncontent");
            draft.datasetActionName = QStringLiteral("Desglosar");
            draft.datasetActionCtx = ctx;
            draft.datasetActionArgv = argv;
            QString errorText;
            if (!queuePendingShellAction(draft, &errorText)) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
                return;
            }
            appLog(QStringLiteral("NORMAL"),
                   QStringLiteral("Cambio pendiente añadido: %1  %2")
                       .arg(draft.scopeLabel, draft.displayLabel));
            updateApplyPropsButtonState();
            return;

        }
        // El respaldo por shell vivía aquí y era INALCANZABLE: el `if` de arriba se
        // cumple siempre dentro de este `else` y retorna. Además la acción exige daemon
        // (requiresDaemon), así que sin agente se rechaza antes de llegar. La ayuda ya
        // decía "no hay alternativa por shell"; ahora el código coincide.
    }
    // Igual que el camino con daemon: encolado, no ejecutado en el acto. Si no, la
    // acción se comportaría de una forma u otra según la conexión.
    {
        PendingShellActionDraft draft;
        draft.scopeLabel = [&]() {
            if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
                return QString();
            }
            const ConnectionProfile& cp = m_profiles.at(ctx.connIdx);
            const QString conn = cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
            return QStringLiteral("%1::%2").arg(conn, ctx.poolName.trimmed());
        }();
        draft.displayLabel = QStringLiteral("Desglosar %1").arg(ds);
        draft.command = cmd;
        draft.timeoutMs = 0;
        draft.streamProgress = true;
        draft.refreshTarget = ctx;
        draft.refreshScope = PendingShellActionDraft::RefreshScope::TargetOnly;
        draft.datasetActionSide = QStringLiteral("conncontent");
        draft.datasetActionName = QStringLiteral("Desglosar");
        draft.datasetActionCtx = ctx;
        draft.datasetActionAllowWindowsScript = allowWindowsScript;
        draft.datasetActionStdin = QByteArray();
        QString errorText;
        if (!queuePendingShellAction(draft, &errorText)) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
            return;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Cambio pendiente añadido: %1  %2")
                   .arg(draft.scopeLabel, draft.displayLabel));
        updateApplyPropsButtonState();
    }
}


void MainWindow::actionAdvancedAssemble() {
    actionAdvancedAssemble(currentConnContentSelection(m_connContentTree));
}

void MainWindow::actionAdvancedAssemble(const DatasetSelectionContext& explicitCtx) {
    const DatasetSelectionContext curr = explicitCtx.valid ? explicitCtx : currentConnContentSelection(m_connContentTree);
    if (!curr.valid || curr.datasetName.isEmpty() || !curr.snapshotName.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_sel_ds_001"), QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    if (!requireFeature(curr.connIdx, zfsmgr::caps::Feature::DirAssemble)) {
        return;
    }
    const QString ds = curr.datasetName;
    if (ds.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = curr.connIdx;
    ctx.poolName = curr.poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();
    beginUiBusy();
    bool busyActive = true;
    auto stopBusy = [&]() {
        if (busyActive) {
            endUiBusy();
            busyActive = false;
        }
    };
    const ConnectionProfile p = m_profiles[ctx.connIdx];
    const bool daemonReadApiOk =
        ctx.connIdx >= 0
        && ctx.connIdx < m_states.size()
        && m_states[ctx.connIdx].daemonInstalled
        && m_states[ctx.connIdx].daemonActive
        && m_states[ctx.connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    QString mountedValue;
    if (!getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mounted"), mountedValue)) {
        stopBusy();
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_adv_chk_mnt_01"), QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    const bool rootMounted = isMountedValueTrue(mountedValue);
    const bool canTempMount = supportsAlternateDatasetMount(ctx.connIdx);
    if (!rootMounted && !canTempMount) {
        stopBusy();
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_adv_ass_mnt01"), QStringLiteral("Ensamblar requiere dataset montado, o un sistema que permita montaje temporal alternativo."),
                QStringLiteral("Assemble requires the dataset mounted, or a system that supports temporary alternate mounts."),
                QStringLiteral("组装要求数据集已挂载，或系统支持临时替代挂载。")));
        return;
    }

    QString listOut;
    QString listErr;
    int listRc = -1;
    QString listCmd;
    if (!isWindowsConnection(p)) {
        listCmd = mwhelpers::agentShellCommand(p, {QStringLiteral("--dump-zfs-list-children"), ds});
    } else {
        listCmd = withSudo(
            p,
            QStringLiteral("zfs list -H -o name -r %1")
                .arg(shSingleQuote(ds)));
    }
    if (!runSsh(p, listCmd, 180000, listOut, listErr, listRc) || listRc != 0) {
        stopBusy();
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_adv_ass_list01"), QStringLiteral("No se pudieron listar subdatasets para ensamblar."),
                                 QStringLiteral("Could not list child datasets for assemble."),
                                 QStringLiteral("无法列出可组装子数据集。")));
        return;
    }
    QStringList children = listOut.split('\n', Qt::SkipEmptyParts);
    for (QString& c : children) {
        c = c.trimmed();
    }
    children.removeAll(QString());
    children.erase(std::remove(children.begin(), children.end(), ds), children.end());
    if (children.isEmpty()) {
        stopBusy();
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_ass_none01"), QStringLiteral("No hay subdatasets para ensamblar."),
                                     QStringLiteral("No child datasets available to assemble."),
                                     QStringLiteral("没有可组装的子数据集。")));
        return;
    }
    stopBusy();
    QStringList childPaths;
    childPaths.reserve(children.size());
    QMap<QString, QString> childPathToDataset;
    const QString childPrefix = ds + QStringLiteral("/");
    for (const QString& childDataset : children) {
        if (!childDataset.startsWith(childPrefix)) {
            continue;
        }
        const QString rel = childDataset.mid(childPrefix.size()).trimmed();
        if (rel.isEmpty()) {
            continue;
        }
        childPaths.push_back(rel);
        childPathToDataset.insert(rel, childDataset);
    }
    QStringList selectedChildPaths;
    // Columna informativa: qué dataset es cada fila. Aquí el nombre es un hecho, no una
    // propuesta, así que no se edita. Tras absorberlo, los datasets que cuelguen de él
    // se conservan y pasan a llamarse por su ruta bajo el padre con ':' en lugar de '/'.
    MainWindow::TreeNameColumn assembleNames;
    assembleNames.header = trk(QStringLiteral("t_col_dataset_002"),
                               QStringLiteral("Dataset actual"),
                               QStringLiteral("Current dataset"),
                               QStringLiteral("当前数据集"));
    assembleNames.editable = false;
    assembleNames.propose = [childPathToDataset](const QString& path, const QSet<QString>&) {
        return childPathToDataset.value(path);
    };

    if (!selectTreeItemsDialog(
            trk(QStringLiteral("t_adv_ass_tit001"), QStringLiteral("Ensamblar: seleccionar subdatasets"),
                QStringLiteral("Assemble: select child datasets"),
                QStringLiteral("组装：选择子数据集")),
            trk(QStringLiteral("t_adv_ass_msg001"), QStringLiteral("Seleccione los subdatasets que desea ensamblar en el dataset padre."),
                QStringLiteral("Select child datasets to assemble into parent dataset."),
                QStringLiteral("请选择要组装回父数据集的子数据集。")),
            childPaths,
            selectedChildPaths,
            QString(),
            {},
            &assembleNames)) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_adv_ass_can001"), QStringLiteral("Ensamblar cancelado o sin selección."),
                   QStringLiteral("Assemble canceled or no selection."),
                   QStringLiteral("组装已取消或无选择。")));
        return;
    }
    QStringList selectedChildren;
    selectedChildren.reserve(selectedChildPaths.size());
    for (const QString& rel : selectedChildPaths) {
        const QString full = childPathToDataset.value(rel).trimmed();
        if (!full.isEmpty()) {
            selectedChildren.push_back(full);
        }
    }
    if (selectedChildren.isEmpty()) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_adv_ass_can001"), QStringLiteral("Ensamblar cancelado o sin selección."),
                   QStringLiteral("Assemble canceled or no selection."),
                   QStringLiteral("组装已取消或无选择。")));
        return;
    }
    for (const QString& child : selectedChildren) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("[ASSEMBLE] pendiente: %1").arg(child));
    }
    QString cmd;
    bool allowWindowsScript = false;
    QByteArray mountStdinPayload;
    if (isWindowsConnection(p)) {
        QStringList selectedPs;
        selectedPs.reserve(selectedChildren.size());
        for (QString c : selectedChildren) {
            c.replace('\'', QStringLiteral("''"));
            selectedPs << QStringLiteral("'%1'").arg(c);
        }
        QString dsPs = ds;
        dsPs.replace('\'', QStringLiteral("''"));
        cmd = QStringLiteral(
                  "$parent='%1'; "
                  "$selected=@(%2); "
                  "function Resolve-Mp([string]$name){ "
                  "  $cur=$name; $anchor=$null; $drive=''; "
                  "  while($true){ "
                  "    $dl=(zfs get -H -o value driveletter $cur 2>$null | Out-String).Trim(); "
                  "    if(-not [string]::IsNullOrWhiteSpace($dl) -and $dl -ne '-' -and $dl.ToLower() -ne 'none'){ $drive=$dl; $anchor=$cur; break }; "
                  "    if($cur -notmatch '/'){ break }; "
                  "    $cur=$cur.Substring(0,$cur.LastIndexOf('/')); "
                  "  }; "
                  "  if([string]::IsNullOrWhiteSpace($drive)){ return '' }; "
                  "  $drive=$drive.Trim(); if($drive.Length -gt 0 -and $drive[$drive.Length-1] -eq ':'){ $drive=$drive.Substring(0,$drive.Length-1) }; "
                  "  $drive=$drive.Substring(0,1).ToUpper(); "
                  "  $base=$drive + ':\\\\'; "
                  "  if($name -eq $anchor){ return $base }; "
                  "  $suffix=$name.Substring($anchor.Length).TrimStart('/'); "
                  "  if([string]::IsNullOrWhiteSpace($suffix)){ return $base }; "
                  "  return ($base + ($suffix -replace '/','\\\\')) "
                  "}; "
                  "$pmp=Resolve-Mp $parent; "
                  "if ([string]::IsNullOrWhiteSpace($pmp) -or -not (Test-Path -LiteralPath $pmp)) { throw 'mountpoint=none' }; "
                  "foreach($child in $selected){ "
                  "  Write-Output ('[ASSEMBLE] start ' + $child); "
                  "  $bn=($child -split '/')[($child -split '/').Count-1]; "
                  "  $cmp=Resolve-Mp $child; if ([string]::IsNullOrWhiteSpace($cmp) -or -not (Test-Path -LiteralPath $cmp)) { continue }; "
                  "  $tmp=Join-Path $env:TEMP ('zfsmgr-assemble-' + [guid]::NewGuid().ToString('N')); "
                  "  New-Item -ItemType Directory -Force -Path $tmp | Out-Null; "
                  "  robocopy $cmp $tmp /E /COPYALL /R:1 /W:1 /NFL /NDL /NP | Out-Null; "
                  "  $r=$LASTEXITCODE; if ($r -ge 8) { throw \"robocopy (to tmp) failed ($r) for $child\" }; "
                  "  zfs destroy -r $child | Out-Null; if ($LASTEXITCODE -ne 0) { throw \"zfs destroy failed for $child\" }; "
                  "  $dst=Join-Path $pmp $bn; if (-not (Test-Path -LiteralPath $dst)) { New-Item -ItemType Directory -Force -Path $dst | Out-Null }; "
                  "  robocopy $tmp $dst /E /COPYALL /R:1 /W:1 /NFL /NDL /NP | Out-Null; "
                  "  $r=$LASTEXITCODE; if ($r -ge 8) { throw \"robocopy (tmp->dst) failed ($r) for $child\" }; "
                  "  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue; "
                  "  Write-Output ('[ASSEMBLE] ok ' + $child + ' -> ' + $dst); "
                  "}")
                  .arg(dsPs, selectedPs.join(QStringLiteral(",")));
        allowWindowsScript = true;
    } else {
        if (!isWindowsConnection(p)) {
            QStringList argv{QStringLiteral("--mutate-advanced-assemble"), ds};
            argv += selectedChildren;
            cmd = mwhelpers::agentShellCommand(p, argv);
            // Encolado, no ejecutado en el acto. Era la única familia de acciones que se
            // saltaba la lista de cambios pendientes, así que no había forma de revisarla
            // antes ni de quitarla de la cola. Al aplicarla conserva todo lo suyo:
            // confirmación, envío como trabajo del daemon y progreso en tiempo real.
            PendingShellActionDraft draft;
            // "conexión::pool", como el resto de entradas de la lista.
            draft.scopeLabel = [&]() {
                if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
                    return QString();
                }
                const ConnectionProfile& cp = m_profiles.at(ctx.connIdx);
                const QString conn = cp.name.trimmed().isEmpty() ? cp.id.trimmed()
                                                                 : cp.name.trimmed();
                return QStringLiteral("%1::%2").arg(conn, ctx.poolName.trimmed());
            }();
            draft.displayLabel = QStringLiteral("Ensamblar %1").arg(ds);
            draft.command = cmd;
            draft.timeoutMs = 0;
            draft.streamProgress = true;
            draft.refreshTarget = ctx;
            draft.refreshScope = PendingShellActionDraft::RefreshScope::TargetOnly;
            draft.datasetActionSide = QStringLiteral("conncontent");
            draft.datasetActionName = QStringLiteral("Ensamblar");
            draft.datasetActionCtx = ctx;
            draft.datasetActionArgv = argv;
            QString errorText;
            if (!queuePendingShellAction(draft, &errorText)) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
                return;
            }
            appLog(QStringLiteral("NORMAL"),
                   QStringLiteral("Cambio pendiente añadido: %1  %2")
                       .arg(draft.scopeLabel, draft.displayLabel));
            updateApplyPropsButtonState();
            return;

            return;
        }
        // El respaldo por shell vivía aquí y era INALCANZABLE, por lo mismo que en
        // Desglosar: el `if` de arriba se cumple siempre dentro de este `else` y retorna.
    }
    // Igual que el camino con daemon: encolado, no ejecutado en el acto. Si no, la
    // acción se comportaría de una forma u otra según la conexión.
    {
        PendingShellActionDraft draft;
        draft.scopeLabel = [&]() {
            if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
                return QString();
            }
            const ConnectionProfile& cp = m_profiles.at(ctx.connIdx);
            const QString conn = cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
            return QStringLiteral("%1::%2").arg(conn, ctx.poolName.trimmed());
        }();
        draft.displayLabel = QStringLiteral("Ensamblar %1").arg(ds);
        draft.command = cmd;
        draft.timeoutMs = 0;
        draft.streamProgress = true;
        draft.refreshTarget = ctx;
        draft.refreshScope = PendingShellActionDraft::RefreshScope::TargetOnly;
        draft.datasetActionSide = QStringLiteral("conncontent");
        draft.datasetActionName = QStringLiteral("Ensamblar");
        draft.datasetActionCtx = ctx;
        draft.datasetActionAllowWindowsScript = allowWindowsScript;
        draft.datasetActionStdin = mountStdinPayload;
        QString errorText;
        if (!queuePendingShellAction(draft, &errorText)) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
            return;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Cambio pendiente añadido: %1  %2")
                   .arg(draft.scopeLabel, draft.displayLabel));
        updateApplyPropsButtonState();
    }
}

