#include "mainwindow.h"

#include <QtWidgets>
#include <QRegularExpression>
#include <algorithm>
#include <functional>

namespace {
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

QString oneLine(const QString& v) {
    QString x = v.simplified();
    return x.left(220);
}

struct CreateDatasetOptions {
    QString datasetPath;
    QString dsType;
    QString volsize;
    QString blocksize;
    bool parents{true};
    bool sparse{false};
    bool nomount{false};
    bool snapshotRecursive{false};
    QStringList properties;
    QString extraArgs;
};

QString buildZfsCreateCmd(const CreateDatasetOptions& opt) {
    const QString dsType = opt.dsType.trimmed().toLower();
    if (dsType == QStringLiteral("snapshot")) {
        QStringList parts;
        parts << QStringLiteral("zfs") << QStringLiteral("snapshot");
        if (opt.snapshotRecursive) {
            parts << QStringLiteral("-r");
        }
        for (const QString& p : opt.properties) {
            const QString pp = p.trimmed();
            if (!pp.isEmpty()) {
                parts << QStringLiteral("-o") << shSingleQuote(pp);
            }
        }
        if (!opt.extraArgs.trimmed().isEmpty()) {
            parts << opt.extraArgs.trimmed();
        }
        parts << shSingleQuote(opt.datasetPath.trimmed());
        return parts.join(' ');
    }

    QStringList parts;
    parts << QStringLiteral("zfs") << QStringLiteral("create");
    if (opt.parents) {
        parts << QStringLiteral("-p");
    }
    if (opt.sparse) {
        parts << QStringLiteral("-s");
    }
    if (opt.nomount) {
        parts << QStringLiteral("-u");
    }
    if (!opt.blocksize.trimmed().isEmpty()) {
        parts << QStringLiteral("-b") << shSingleQuote(opt.blocksize.trimmed());
    }
    if (dsType == QStringLiteral("volume") && !opt.volsize.trimmed().isEmpty()) {
        parts << QStringLiteral("-V") << shSingleQuote(opt.volsize.trimmed());
    }
    for (const QString& p : opt.properties) {
        const QString pp = p.trimmed();
        if (!pp.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(pp);
        }
    }
    if (!opt.extraArgs.trimmed().isEmpty()) {
        parts << opt.extraArgs.trimmed();
    }
    parts << shSingleQuote(opt.datasetPath.trimmed());
    return parts.join(' ');
}
} // namespace

void MainWindow::actionAdvancedBreakdown() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const ConnectionProfile& p = m_profiles[connIdx];
    QString mountOut;
    QString mountErr;
    int mountRc = -1;
    const QString mountCheckCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o name,value -r mounted %1").arg(shSingleQuote(ds)));
    if (!runSsh(p, mountCheckCmd, 25000, mountOut, mountErr, mountRc) || mountRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    QStringList unmounted;
    for (const QString& ln : mountOut.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = ln.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString name = parts[0].trimmed();
        const QString mountedVal = parts[1].trimmed();
        if (name.isEmpty() || name.contains('@')) {
            continue; // ignorar snapshots
        }
        if (!isMountedValueTrue(mountedVal)) {
            unmounted << name;
        }
    }
    if (!unmounted.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Desglosar requiere dataset y descendientes montados.\nNo montados:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("Break down requires dataset and descendants mounted.\nNot mounted:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("拆分要求数据集及其所有后代已挂载。\n未挂载：\n%1")
                    .arg(unmounted.join('\n'))));
        return;
    }

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
                                    "Get-ChildItem -LiteralPath $mp -Directory -Force | Select-Object -ExpandProperty Name | Sort-Object -Unique")
                                    .arg(dsPs);
        if (!runSsh(p, withSudo(p, listCmd), 25000, listOut, listErr, listRc) || listRc != 0) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No se pudieron listar directorios para desglosar."),
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
        const QString listCmd = withSudo(
            p,
            QStringLiteral("set -e; DATASET=%1; "
                           "resolve_mp(){ ds=\"$1\"; mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$ds\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"; }; "
                           "MP=$(resolve_mp \"$DATASET\"); "
                           "[ -n \"$MP\" ] || exit 0; "
                           "[ -d \"$MP\" ] || exit 0; "
                           "for d in \"$MP\"/.[!.]* \"$MP\"/..?* \"$MP\"/*; do [ -d \"$d\" ] || continue; bn=$(basename \"$d\"); [ -n \"$bn\" ] && printf '%s\\n' \"$bn\"; done | sort -u")
                .arg(shSingleQuote(ds)));
        if (!runSsh(p, listCmd, 25000, listOut, listErr, listRc) || listRc != 0) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No se pudieron listar directorios para desglosar."),
                                     QStringLiteral("Could not list directories for breakdown."),
                                     QStringLiteral("无法列出可拆分目录。")));
            return;
        }
        dirs = listOut.split('\n', Qt::SkipEmptyParts);
        for (QString& d : dirs) {
            d = d.trimmed();
        }
        dirs.removeAll(QString());
        QString mpOut;
        QString mpErr;
        int mpRc = -1;
        const QString mpCmd = withSudo(
            p,
            QStringLiteral("DATASET=%1; "
                           "mp=$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$DATASET\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"")
                .arg(shSingleQuote(ds)));
        runSsh(p, mpCmd, 20000, mpOut, mpErr, mpRc);
        resolvedMp = mpOut.trimmed();
    }

    QString dsListOut;
    QString dsListErr;
    int dsListRc = -1;
    QStringList datasetsDetected;
    QSet<QString> childDatasetNames;
    const QString dsListCmd = withSudo(
        p,
        QStringLiteral("zfs list -H -o name -r %1")
            .arg(shSingleQuote(ds)));
    if (runSsh(p, dsListCmd, 25000, dsListOut, dsListErr, dsListRc) && dsListRc == 0) {
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
            const QString childPath = datasetName.mid(prefix.size());
            const QString childName = childPath.section('/', 0, 0).trimmed();
            if (!childName.isEmpty()) {
                childDatasetNames.insert(childName);
            }
        }
    }
    if (!childDatasetNames.isEmpty()) {
        dirs.erase(std::remove_if(dirs.begin(), dirs.end(),
                                  [&](const QString& d) { return childDatasetNames.contains(d); }),
                   dirs.end());
    }
    QStringList mountedDescMountpoints;
    if (!isWindowsConnection(p)) {
        QString mountedDescOut;
        QString mountedDescErr;
        int mountedDescRc = -1;
        const QString mountedDescCmd = withSudo(
            p,
            QStringLiteral("DATASET=%1; zfs mount | awk -v pfx=\"$DATASET/\" 'index($1,pfx)==1 {print $2}'")
                .arg(shSingleQuote(ds)));
        if (runSsh(p, mountedDescCmd, 25000, mountedDescOut, mountedDescErr, mountedDescRc) && mountedDescRc == 0) {
            mountedDescMountpoints = mountedDescOut.split('\n', Qt::SkipEmptyParts);
            for (QString& mp : mountedDescMountpoints) {
                mp = mp.trimmed();
            }
            mountedDescMountpoints.removeAll(QString());
        }
    }
    if (!resolvedMp.isEmpty() && !mountedDescMountpoints.isEmpty()) {
        QString baseMp = resolvedMp;
        while (baseMp.endsWith('/')) {
            baseMp.chop(1);
        }
        dirs.erase(std::remove_if(dirs.begin(), dirs.end(),
                                  [&](const QString& d) {
                                      const QString dirPath = baseMp + QStringLiteral("/") + d;
                                      const QString dirPathWithSlash = dirPath + QStringLiteral("/");
                                      for (const QString& childMp : mountedDescMountpoints) {
                                          if (childMp == dirPath || childMp.startsWith(dirPathWithSlash)) {
                                              return true;
                                          }
                                      }
                                      return false;
                                  }),
                   dirs.end());
    }
    if (dirs.isEmpty()) {
        const QString dirsText = dirs.isEmpty()
                                     ? tr3(QStringLiteral("(ninguno)"), QStringLiteral("(none)"), QStringLiteral("（无）"))
                                     : dirs.join('\n');
        const QString datasetsText = datasetsDetected.isEmpty()
                                         ? tr3(QStringLiteral("(ninguno)"), QStringLiteral("(none)"), QStringLiteral("（无）"))
                                         : datasetsDetected.join('\n');
        const QString mpText = resolvedMp.isEmpty()
                                   ? tr3(QStringLiteral("(sin resolver)"), QStringLiteral("(unresolved)"), QStringLiteral("（未解析）"))
                                   : resolvedMp;

        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No hay directorios para desglosar en el dataset seleccionado.\n\n"
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
    QStringList selectedDirs;
    if (!selectItemsDialog(
            tr3(QStringLiteral("Desglosar: seleccionar directorios"),
                QStringLiteral("Break down: select directories"),
                QStringLiteral("拆分：选择目录")),
            tr3(QStringLiteral("Seleccione los directorios que desea desglosar en subdatasets."),
                QStringLiteral("Select directories to split into subdatasets."),
                QStringLiteral("请选择要拆分为子数据集的目录。")),
            dirs,
            selectedDirs)) {
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Desglosar cancelado o sin selección."),
                   QStringLiteral("Break down canceled or no selection."),
                   QStringLiteral("拆分已取消或无选择。")));
        return;
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
                  "  $child=\"$ds/$bn\"; "
                  "  zfs list -H -o name $child 2>$null | Out-Null; if ($LASTEXITCODE -ne 0) { zfs create $child | Out-Null; if($LASTEXITCODE -ne 0){ throw \"zfs create failed for $child\" } }; "
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
                  "}")
                  .arg(dsPs, selectedPs.join(QStringLiteral(",")));
        allowWindowsScript = true;
    } else {
        QStringList selectedQuoted;
        selectedQuoted.reserve(selectedDirs.size());
        for (const QString& d : selectedDirs) {
            selectedQuoted << shSingleQuote(d);
        }
        const QString selectedList = selectedQuoted.join(' ');
        cmd =
            QStringLiteral("set -e; DATASET=%1; "
                           "RSYNC_OPTS='-aHWS'; "
                           "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                           "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                           "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                           "resolve_mp(){ ds=\"$1\"; mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$ds\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"; }; "
                           "MP=$(resolve_mp \"$DATASET\"); "
                           "[ -n \"$MP\" ] || { echo \"mountpoint=none\"; exit 2; }; "
                           "SELECTED_DIRS=(%2); is_selected_dir(){ for s in \"${SELECTED_DIRS[@]}\"; do [ \"$s\" = \"$1\" ] && return 0; done; return 1; }; "
                           "for d in \"$MP\"/.[!.]* \"$MP\"/..?* \"$MP\"/*; do [ -d \"$d\" ] || continue; bn=$(basename \"$d\"); is_selected_dir \"$bn\" || continue; "
                           "child=\"$DATASET/$bn\"; "
                           "zfs list -H -o name \"$child\" >/dev/null 2>&1 && { echo \"child_exists=$child\"; continue; }; "
                           "FINAL_MP=\"$MP/$bn\"; "
                           "TMP_CHILD_MP=$(mktemp -d /tmp/zfsmgr-breakdown-child-XXXXXX); "
                           "zfs create -o mountpoint=\"$TMP_CHILD_MP\" \"$child\"; "
                           "zfs mount \"$child\" >/dev/null 2>&1 || true; "
                           "try=0; "
                           "while :; do "
                           "  rsync $RSYNC_OPTS \"$d\"/ \"$TMP_CHILD_MP\"/; "
                           "  PENDING=$(rsync -rni --ignore-existing \"$d\"/ \"$TMP_CHILD_MP\"/ | awk 'length($0)>11{c=substr($0,1,11); if(substr(c,1,1)==\">\" && substr(c,2,1)==\"f\") n++} END{print n+0}'); "
                           "  [ \"$PENDING\" = \"0\" ] && break; "
                           "  try=$((try+1)); "
                           "  [ \"$try\" -lt 5 ] || break; "
                           "done; "
                           "[ \"$PENDING\" = \"0\" ] || { echo \"verify_failed=$child pending=$PENDING\"; exit 42; }; "
                           "rm -rf \"$d\"; "
                           "zfs set mountpoint=\"$FINAL_MP\" \"$child\"; "
                           "zfs mount \"$child\" >/dev/null 2>&1 || true; "
                           "rmdir \"$TMP_CHILD_MP\" >/dev/null 2>&1 || true; "
                           "done")
                .arg(shSingleQuote(ds), selectedList);
    }
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Desglosar"), ctx, cmd, 0, allowWindowsScript)) {
        updateAdvancedSelectionUi(ds, QString());
    }
}

void MainWindow::actionAdvancedAssemble() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const ConnectionProfile& p = m_profiles[connIdx];
    QString mountOut;
    QString mountErr;
    int mountRc = -1;
    const QString mountCheckCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o name,value -r mounted %1").arg(shSingleQuote(ds)));
    if (!runSsh(p, mountCheckCmd, 25000, mountOut, mountErr, mountRc) || mountRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    QStringList unmounted;
    for (const QString& ln : mountOut.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = ln.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString name = parts[0].trimmed();
        const QString mountedVal = parts[1].trimmed();
        if (name.isEmpty() || name.contains('@')) {
            continue; // ignorar snapshots
        }
        if (!isMountedValueTrue(mountedVal)) {
            unmounted << name;
        }
    }
    if (!unmounted.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Ensamblar requiere dataset y descendientes montados.\nNo montados:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("Assemble requires dataset and descendants mounted.\nNot mounted:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("组装要求数据集及其所有后代已挂载。\n未挂载：\n%1")
                    .arg(unmounted.join('\n'))));
        return;
    }

    QString listOut;
    QString listErr;
    int listRc = -1;
    const QString listCmd = withSudo(
        p,
        QStringLiteral("zfs list -H -o name -r %1")
            .arg(shSingleQuote(ds)));
    if (!runSsh(p, listCmd, 25000, listOut, listErr, listRc) || listRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudieron listar subdatasets para ensamblar."),
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
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No hay subdatasets para ensamblar."),
                                     QStringLiteral("No child datasets available to assemble."),
                                     QStringLiteral("没有可组装的子数据集。")));
        return;
    }
    QStringList selectedChildren;
    if (!selectItemsDialog(
            tr3(QStringLiteral("Ensamblar: seleccionar subdatasets"),
                QStringLiteral("Assemble: select child datasets"),
                QStringLiteral("组装：选择子数据集")),
            tr3(QStringLiteral("Seleccione los subdatasets que desea ensamblar en el dataset padre."),
                QStringLiteral("Select child datasets to assemble into parent dataset."),
                QStringLiteral("请选择要组装回父数据集的子数据集。")),
            children,
            selectedChildren)) {
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Ensamblar cancelado o sin selección."),
                   QStringLiteral("Assemble canceled or no selection."),
                   QStringLiteral("组装已取消或无选择。")));
        return;
    }
    QString cmd;
    bool allowWindowsScript = false;
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
                  "}")
                  .arg(dsPs, selectedPs.join(QStringLiteral(",")));
        allowWindowsScript = true;
    } else {
        QStringList selectedQuoted;
        selectedQuoted.reserve(selectedChildren.size());
        for (const QString& c : selectedChildren) {
            selectedQuoted << shSingleQuote(c);
        }
        const QString selectedList = selectedQuoted.join(' ');
        cmd =
            QStringLiteral("set -e; DATASET=%1; "
                           "RSYNC_OPTS='-aHWS'; "
                           "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                           "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                           "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                           "resolve_mp(){ ds=\"$1\"; mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$ds\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"; }; "
                           "MP=$(resolve_mp \"$DATASET\"); "
                           "[ -n \"$MP\" ] || { echo \"mountpoint=none\"; exit 2; }; "
                           "SELECTED_CHILDREN=(%2); "
                           "for child in \"${SELECTED_CHILDREN[@]}\"; do bn=${child##*/}; "
                           "CMP=$(resolve_mp \"$child\"); [ -n \"$CMP\" ] || continue; "
                           "TMP=$(mktemp -d /tmp/zfsmgr-assemble-XXXXXX); "
                           "rsync $RSYNC_OPTS \"$CMP\"/ \"$TMP\"/; "
                           "zfs destroy -r \"$child\"; "
                           "mkdir -p \"$MP/$bn\"; "
                           "rsync $RSYNC_OPTS \"$TMP\"/ \"$MP/$bn\"/; "
                           "rm -rf \"$TMP\"; "
                           "done")
                .arg(shSingleQuote(ds), selectedList);
    }
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Ensamblar"), ctx, cmd, 0, allowWindowsScript)) {
        updateAdvancedSelectionUi(ds, QString());
        refreshConnectionByIndex(ctx.connIdx);
    }
}

void MainWindow::actionAdvancedCreateFromDir() {
    if (actionsLocked()) {
        return;
    }
    const auto selected = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString().trimmed();
    const QString snap = selected.first()->data(1, Qt::UserRole).toString().trimmed();
    if (ds.isEmpty() || !snap.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un dataset (no snapshot)."),
                                     QStringLiteral("You must select a dataset (not a snapshot)."),
                                     QStringLiteral("必须选择数据集（不能是快照）。")));
        return;
    }

    const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = token.left(sep).toInt();
    ctx.poolName = token.mid(sep + 2);
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    struct PropSpec {
        QString name;
        QString kind;
        QStringList values;
    };
    struct PropEditor {
        QString name;
        QLineEdit* edit{nullptr};
        QComboBox* combo{nullptr};
    };

    QDialog dlg(this);
    dlg.setWindowTitle(tr3(QStringLiteral("Crear dataset desde directorio"),
                           QStringLiteral("Create dataset from directory"),
                           QStringLiteral("从目录创建数据集")));
    dlg.setModal(true);
    dlg.resize(900, 760);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QWidget* formWidget = new QWidget(&dlg);
    QGridLayout* form = new QGridLayout(formWidget);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    int row = 0;

    QLabel* pathLabel = new QLabel(tr3(QStringLiteral("Path"), QStringLiteral("Path"), QStringLiteral("路径")), formWidget);
    QLineEdit* pathEdit = new QLineEdit(formWidget);
    pathEdit->setText(ds + QStringLiteral("/new_dataset"));
    form->addWidget(pathLabel, row, 0);
    form->addWidget(pathEdit, row, 1, 1, 3);
    row++;

    QLabel* typeLabel = new QLabel(tr3(QStringLiteral("Tipo"), QStringLiteral("Type"), QStringLiteral("类型")), formWidget);
    QComboBox* typeCombo = new QComboBox(formWidget);
    typeCombo->addItem(QStringLiteral("filesystem"), QStringLiteral("filesystem"));
    typeCombo->setCurrentIndex(0);
    typeCombo->setEnabled(false);
    form->addWidget(typeLabel, row, 0);
    form->addWidget(typeCombo, row, 1);
    row++;

    QLabel* mountDirLabel = new QLabel(tr3(QStringLiteral("Directorio local"),
                                           QStringLiteral("Local directory"),
                                           QStringLiteral("本地目录")),
                                       formWidget);
    QLineEdit* mountDirEdit = new QLineEdit(formWidget);
    QPushButton* browseDirBtn = new QPushButton(
        tr3(QStringLiteral("Seleccionar..."), QStringLiteral("Select..."), QStringLiteral("选择...")),
        formWidget);
    form->addWidget(mountDirLabel, row, 0);
    form->addWidget(mountDirEdit, row, 1, 1, 2);
    form->addWidget(browseDirBtn, row, 3);
    row++;
    QObject::connect(browseDirBtn, &QPushButton::clicked, &dlg, [&]() {
        if (ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
            QMessageBox::warning(
                &dlg,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("Conexión inválida para explorar directorios remotos."),
                    QStringLiteral("Invalid connection for remote directory browsing."),
                    QStringLiteral("用于远程目录浏览的连接无效。")));
            return;
        }
        const ConnectionProfile& prof = m_profiles[ctx.connIdx];
        const bool isWinRemote = isWindowsConnection(ctx.connIdx);

        auto parentPath = [&](const QString& current) -> QString {
            if (isWinRemote) {
                QString p = current.trimmed();
                if (p.isEmpty()) {
                    return QStringLiteral("C:\\");
                }
                p.replace('/', '\\');
                const QRegularExpression rootRx(QStringLiteral("^[A-Za-z]:\\\\?$"));
                if (rootRx.match(p).hasMatch()) {
                    return p.endsWith('\\') ? p : (p + QStringLiteral("\\"));
                }
                const int pos = p.lastIndexOf('\\');
                if (pos <= 2) {
                    return (p.size() >= 2) ? (p.left(2) + QStringLiteral("\\")) : QStringLiteral("C:\\");
                }
                return p.left(pos);
            }
            const QString p = current.trimmed();
            if (p.isEmpty() || p == QStringLiteral("/")) {
                return QStringLiteral("/");
            }
            const int pos = p.lastIndexOf('/');
            if (pos <= 0) {
                return QStringLiteral("/");
            }
            return p.left(pos);
        };

        auto joinPath = [&](const QString& base, const QString& name) -> QString {
            if (isWinRemote) {
                QString b = base.trimmed();
                if (b.isEmpty()) {
                    b = QStringLiteral("C:\\");
                }
                b.replace('/', '\\');
                return b.endsWith('\\') ? (b + name) : (b + QStringLiteral("\\") + name);
            }
            QString b = base.trimmed();
            if (b.isEmpty()) {
                b = QStringLiteral("/");
            }
            if (b == QStringLiteral("/")) {
                return b + name;
            }
            return b.endsWith('/') ? (b + name) : (b + QStringLiteral("/") + name);
        };

        auto listRemoteDirs = [&](const QString& basePath, QString& resolvedPath, QStringList& children, QString& errorMsg) -> bool {
            const QString fallbackRoot = isWinRemote ? QStringLiteral("C:\\") : QStringLiteral("/");
            const QString requested = basePath.trimmed().isEmpty() ? fallbackRoot : basePath.trimmed();
            QString remoteCmd;
            if (isWinRemote) {
                auto psSingle = [](const QString& v) {
                    QString out = v;
                    out.replace(QStringLiteral("'"), QStringLiteral("''"));
                    return QStringLiteral("'") + out + QStringLiteral("'");
                };
                remoteCmd = QStringLiteral(
                                "$base=%1; "
                                "if (Test-Path -LiteralPath $base -PathType Container) { $p=(Resolve-Path -LiteralPath $base).Path } else { $p='C:\\' }; "
                                "Write-Output $p; "
                                "Get-ChildItem -LiteralPath $p -Directory -Name -ErrorAction SilentlyContinue | Sort-Object")
                                .arg(psSingle(requested));
            } else {
                remoteCmd = QStringLiteral(
                                "BASE=%1; "
                                "if [ -d \"$BASE\" ]; then cd \"$BASE\" 2>/dev/null || cd /; else cd /; fi; "
                                "pwd; "
                                "ls -1A 2>/dev/null | while IFS= read -r e; do [ -d \"$e\" ] && echo \"$e\"; done | sort")
                                .arg(shSingleQuote(requested));
            }
            QString out;
            QString err;
            int rc = -1;
            if (!runSsh(prof, remoteCmd, 20000, out, err, rc) || rc != 0) {
                errorMsg = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
                return false;
            }
            QStringList lines = out.split('\n', Qt::KeepEmptyParts);
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
                lines.removeLast();
            }
            if (lines.isEmpty()) {
                errorMsg = QStringLiteral("empty remote response");
                return false;
            }
            resolvedPath = lines.takeFirst().trimmed();
            children.clear();
            for (const QString& ln : lines) {
                const QString v = ln.trimmed();
                if (!v.isEmpty()) {
                    children << v;
                }
            }
            return true;
        };

        QDialog picker(&dlg);
        picker.setModal(true);
        picker.resize(640, 460);
        picker.setWindowTitle(
            tr3(QStringLiteral("Seleccionar directorio remoto"),
                QStringLiteral("Select remote directory"),
                QStringLiteral("选择远程目录")));
        QVBoxLayout* pv = new QVBoxLayout(&picker);
        pv->setContentsMargins(10, 10, 10, 10);
        pv->setSpacing(8);
        QLabel* connInfo = new QLabel(
            tr3(QStringLiteral("Conexión: %1").arg(prof.name),
                QStringLiteral("Connection: %1").arg(prof.name),
                QStringLiteral("连接：%1").arg(prof.name)),
            &picker);
        pv->addWidget(connInfo);
        QLineEdit* currentPathEdit = new QLineEdit(&picker);
        currentPathEdit->setReadOnly(true);
        pv->addWidget(currentPathEdit);
        QListWidget* dirList = new QListWidget(&picker);
        dirList->setSelectionMode(QAbstractItemView::SingleSelection);
        pv->addWidget(dirList, 1);
        QHBoxLayout* navRow = new QHBoxLayout();
        QPushButton* upBtn = new QPushButton(tr3(QStringLiteral("Subir"), QStringLiteral("Up"), QStringLiteral("上级")), &picker);
        QPushButton* refreshBtn = new QPushButton(tr3(QStringLiteral("Actualizar"), QStringLiteral("Refresh"), QStringLiteral("刷新")), &picker);
        navRow->addWidget(upBtn);
        navRow->addWidget(refreshBtn);
        navRow->addStretch(1);
        pv->addLayout(navRow);
        QDialogButtonBox* pb = new QDialogButtonBox(&picker);
        QPushButton* cancelBtn = pb->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
        QPushButton* selectBtn = pb->addButton(tr3(QStringLiteral("Seleccionar"), QStringLiteral("Select"), QStringLiteral("选择")), QDialogButtonBox::AcceptRole);
        pv->addWidget(pb);
        QObject::connect(cancelBtn, &QPushButton::clicked, &picker, &QDialog::reject);

        QString current = mountDirEdit->text().trimmed();
        if (current.isEmpty()) {
            current = isWinRemote ? QStringLiteral("C:\\") : QStringLiteral("/");
        }
        std::function<void()> reloadDir;
        reloadDir = [&]() {
            QString resolved;
            QStringList children;
            QString errMsg;
            dirList->clear();
            if (!listRemoteDirs(current, resolved, children, errMsg)) {
                QMessageBox::warning(
                    &picker,
                    QStringLiteral("ZFSMgr"),
                    tr3(QStringLiteral("No se pudo listar directorios remotos:\n%1").arg(errMsg),
                        QStringLiteral("Could not list remote directories:\n%1").arg(errMsg),
                        QStringLiteral("无法列出远程目录：\n%1").arg(errMsg)));
                return;
            }
            current = resolved;
            currentPathEdit->setText(current);
            const QString par = parentPath(current);
            if (par != current) {
                QListWidgetItem* upItem = new QListWidgetItem(QStringLiteral(".."), dirList);
                upItem->setData(Qt::UserRole, par);
            }
            for (const QString& ch : children) {
                QListWidgetItem* it = new QListWidgetItem(ch, dirList);
                it->setData(Qt::UserRole, joinPath(current, ch));
            }
            if (dirList->count() > 0) {
                dirList->setCurrentRow(0);
            }
        };
        QObject::connect(refreshBtn, &QPushButton::clicked, &picker, [&]() { reloadDir(); });
        QObject::connect(upBtn, &QPushButton::clicked, &picker, [&]() {
            current = parentPath(current);
            reloadDir();
        });
        QObject::connect(dirList, &QListWidget::itemDoubleClicked, &picker, [&](QListWidgetItem* item) {
            if (!item) {
                return;
            }
            const QString next = item->data(Qt::UserRole).toString().trimmed();
            if (next.isEmpty()) {
                return;
            }
            current = next;
            reloadDir();
        });
        QObject::connect(selectBtn, &QPushButton::clicked, &picker, [&]() {
            mountDirEdit->setText(currentPathEdit->text().trimmed());
            picker.accept();
        });
        reloadDir();
        picker.exec();
    });

    QLabel* blocksizeLabel = new QLabel(tr3(QStringLiteral("Blocksize"), QStringLiteral("Blocksize"), QStringLiteral("块大小")), formWidget);
    QLineEdit* blocksizeEdit = new QLineEdit(formWidget);
    form->addWidget(blocksizeLabel, row, 0);
    form->addWidget(blocksizeEdit, row, 1);
    row++;

    QWidget* optsWidget = new QWidget(formWidget);
    QHBoxLayout* optsLay = new QHBoxLayout(optsWidget);
    optsLay->setContentsMargins(0, 0, 0, 0);
    optsLay->setSpacing(12);
    QCheckBox* parentsChk = new QCheckBox(tr3(QStringLiteral("Crear padres (-p)"), QStringLiteral("Create parents (-p)"), QStringLiteral("创建父级(-p)")), optsWidget);
    parentsChk->setChecked(true);
    optsLay->addWidget(parentsChk);
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(tr3(QStringLiteral("Argumentos extra"), QStringLiteral("Extra args"), QStringLiteral("额外参数")), formWidget);
    QLineEdit* extraEdit = new QLineEdit(formWidget);
    form->addWidget(extraLabel, row, 0);
    form->addWidget(extraEdit, row, 1, 1, 3);
    row++;

    root->addWidget(formWidget);

    QGroupBox* propsGroup = new QGroupBox(tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")), &dlg);
    QVBoxLayout* propsGroupLay = new QVBoxLayout(propsGroup);
    propsGroupLay->setContentsMargins(6, 6, 6, 6);
    propsGroupLay->setSpacing(4);

    QScrollArea* propsScroll = new QScrollArea(propsGroup);
    propsScroll->setWidgetResizable(true);
    QWidget* propsContainer = new QWidget(propsScroll);
    QGridLayout* propsGrid = new QGridLayout(propsContainer);
    propsGrid->setHorizontalSpacing(8);
    propsGrid->setVerticalSpacing(4);

    const QList<PropSpec> propSpecs = {
        {QStringLiteral("compression"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("lz4"), QStringLiteral("gzip"), QStringLiteral("zstd"), QStringLiteral("zle")}},
        {QStringLiteral("atime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("xattr"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa")}},
        {QStringLiteral("acltype"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), QStringLiteral("combo"), {QString(), QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("recordsize"), QStringLiteral("entry"), {}},
        {QStringLiteral("quota"), QStringLiteral("entry"), {}},
        {QStringLiteral("reservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("refquota"), QStringLiteral("entry"), {}},
        {QStringLiteral("refreservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("copies"), QStringLiteral("combo"), {QString(), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}},
        {QStringLiteral("checksum"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("fletcher2"), QStringLiteral("fletcher4"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr")}},
        {QStringLiteral("sync"), QStringLiteral("combo"), {QString(), QStringLiteral("standard"), QStringLiteral("always"), QStringLiteral("disabled")}},
        {QStringLiteral("logbias"), QStringLiteral("combo"), {QString(), QStringLiteral("latency"), QStringLiteral("throughput")}},
        {QStringLiteral("primarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("secondarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("dedup"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("verify"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein")}},
        {QStringLiteral("normalization"), QStringLiteral("combo"), {QString(), QStringLiteral("none"), QStringLiteral("formC"), QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")}},
        {QStringLiteral("casesensitivity"), QStringLiteral("combo"), {QString(), QStringLiteral("sensitive"), QStringLiteral("insensitive"), QStringLiteral("mixed")}},
        {QStringLiteral("utf8only"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
    };

    QList<PropEditor> propEditors;
    propEditors.reserve(propSpecs.size());
    for (int i = 0; i < propSpecs.size(); ++i) {
        const PropSpec& spec = propSpecs[i];
        const int r = i / 2;
        const int cBase = (i % 2) * 2;
        QLabel* lbl = new QLabel(spec.name, propsContainer);
        propsGrid->addWidget(lbl, r, cBase);
        PropEditor editor;
        editor.name = spec.name;
        if (spec.kind == QStringLiteral("combo")) {
            QComboBox* cb = new QComboBox(propsContainer);
            cb->addItems(spec.values);
            if (spec.name == QStringLiteral("compression")) {
                // Permite niveles, por ejemplo: zstd-19, gzip-9.
                cb->setEditable(true);
                cb->setInsertPolicy(QComboBox::NoInsert);
            }
            editor.combo = cb;
            propsGrid->addWidget(cb, r, cBase + 1);
        } else {
            QLineEdit* le = new QLineEdit(propsContainer);
            editor.edit = le;
            propsGrid->addWidget(le, r, cBase + 1);
        }
        propEditors.push_back(editor);
    }
    propsContainer->setLayout(propsGrid);
    propsScroll->setWidget(propsContainer);
    propsGroupLay->addWidget(propsScroll);
    root->addWidget(propsGroup, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        tr3(QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")),
        QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    bool accepted = false;
    CreateDatasetOptions opt;
    QString selectedMountDir;
    QObject::connect(createBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString path = pathEdit->text().trimmed();
        const QString mountDir = mountDirEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe indicar el path del dataset."),
                                     QStringLiteral("Dataset path is required."),
                                     QStringLiteral("必须指定数据集路径。")));
            return;
        }
        if (mountDir.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un directorio local."),
                                     QStringLiteral("You must select a local directory."),
                                     QStringLiteral("必须选择本地目录。")));
            return;
        }

        QStringList properties;
        for (const PropEditor& pe : propEditors) {
            QString v;
            if (pe.combo) {
                v = pe.combo->currentText().trimmed();
            } else if (pe.edit) {
                v = pe.edit->text().trimmed();
            }
            if (!v.isEmpty()) {
                properties.push_back(pe.name + QStringLiteral("=") + v);
            }
        }

        opt.datasetPath = path;
        opt.dsType = QStringLiteral("filesystem");
        opt.volsize.clear();
        opt.blocksize = blocksizeEdit->text().trimmed();
        opt.parents = parentsChk->isChecked();
        opt.sparse = false;
        opt.nomount = false;
        opt.snapshotRecursive = false;
        opt.properties = properties;
        opt.extraArgs = extraEdit->text().trimmed();
        selectedMountDir = mountDir;
        accepted = true;
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted || !accepted) {
        return;
    }

    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString createCmd = buildZfsCreateCmd(opt);
    QString cmd;
    bool allowWindowsScript = false;
    if (!isWin) {
        cmd = QStringLiteral(
                  "set -e; "
                  "DATASET=%1; "
                  "SRC_DIR=%2; "
                  "RSYNC_OPTS='-aHWS'; "
                  "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                  "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                  "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                  "TMP_MP=$(mktemp -d /tmp/zfsmgr-fromdir-mp-XXXXXX); "
                  "BACKUP_DIR=''; "
                  "cleanup(){ "
                  "  rc=$?; "
                  "  if [ $rc -ne 0 ]; then "
                  "    if [ -n \"$BACKUP_DIR\" ] && [ ! -e \"$SRC_DIR\" ] && [ -d \"$BACKUP_DIR\" ]; then mv \"$BACKUP_DIR\" \"$SRC_DIR\" || true; fi; "
                  "  fi; "
                  "  rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; "
                  "  exit $rc; "
                  "}; "
                  "trap cleanup EXIT INT TERM; "
                  "[ -d \"$SRC_DIR\" ] || { echo 'source directory does not exist'; exit 2; }; "
                  "if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx \"$SRC_DIR\" >/dev/null 2>&1; then "
                  "  echo 'mountpoint already in use'; exit 3; "
                  "fi; "
                  "%3; "
                  "zfs set canmount=on \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$TMP_MP\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "ACTIVE_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "[ \"$ACTIVE_MP\" = \"$TMP_MP\" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 4; }; "
                  "rsync $RSYNC_OPTS \"$SRC_DIR\"/ \"$TMP_MP\"/; "
                  "BACKUP_DIR=\"$SRC_DIR.zfsmgr-bak-$$\"; "
                  "i=0; "
                  "while [ -e \"$BACKUP_DIR\" ]; do i=$((i+1)); BACKUP_DIR=\"$SRC_DIR.zfsmgr-bak-$$-$i\"; done; "
                  "mv \"$SRC_DIR\" \"$BACKUP_DIR\"; "
                  "mkdir -p \"$SRC_DIR\"; "
                  "zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$SRC_DIR\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "FINAL_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "if [ \"$FINAL_MP\" != \"$SRC_DIR\" ]; then "
                  "  zfs set mountpoint=\"$TMP_MP\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  rm -rf \"$SRC_DIR\"; "
                  "  mv \"$BACKUP_DIR\" \"$SRC_DIR\"; "
                  "  echo 'failed to switch mountpoint to destination directory'; "
                  "  exit 5; "
                  "fi; "
                  "rm -rf \"$BACKUP_DIR\"; "
                  "BACKUP_DIR=''; "
                  "trap - EXIT INT TERM; "
                  "rmdir \"$TMP_MP\" >/dev/null 2>&1 || true")
                  .arg(shSingleQuote(opt.datasetPath),
                       shSingleQuote(selectedMountDir),
                       createCmd);
    } else {
        allowWindowsScript = true;
        auto psSingle = [](const QString& v) {
            QString out = v;
            out.replace(QStringLiteral("'"), QStringLiteral("''"));
            return QStringLiteral("'") + out + QStringLiteral("'");
        };
        cmd = QStringLiteral(
                  "$ErrorActionPreference = 'Stop'; "
                  "$dataset = %1; "
                  "$srcDir = %2; "
                  "if (-not (Test-Path -LiteralPath $srcDir -PathType Container)) { throw 'source directory does not exist'; } "
                  "$srcDir = (Resolve-Path -LiteralPath $srcDir).Path; "
                  "%3; "
                  "zfs set canmount=on $dataset 2>$null | Out-Null; "
                  "zfs mount $dataset 2>$null | Out-Null; "
                  "$activeMp = ''; "
                  "foreach ($line in @(zfs mount 2>$null)) { "
                  "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "    if ($Matches[1] -eq $dataset) { $activeMp = $Matches[2].Trim(); break; } "
                  "  } "
                  "} "
                  "if ([string]::IsNullOrWhiteSpace($activeMp)) { throw 'could not resolve effective mountpoint'; } "
                  "if ([string]::Equals($activeMp, $srcDir, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'mountpoint already in use'; } "
                  "$null = robocopy $srcDir $activeMp /E /COPYALL /R:1 /W:1 /NFL /NDL /NP; "
                  "if ($LASTEXITCODE -ge 8) { throw ('robocopy failed with code ' + $LASTEXITCODE); } "
                  "Write-Output ('[FROMDIR] copied to effective mountpoint: ' + $activeMp)")
                  .arg(psSingle(opt.datasetPath),
                       psSingle(selectedMountDir),
                       createCmd);
    }
    executeDatasetAction(QStringLiteral("advanced"),
                         tr3(QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")),
                         ctx,
                         cmd,
                         90000,
                         allowWindowsScript);
}

void MainWindow::actionAdvancedToDir() {
    if (actionsLocked()) {
        return;
    }
    const auto selected = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString().trimmed();
    const QString snap = selected.first()->data(1, Qt::UserRole).toString().trimmed();
    if (ds.isEmpty() || !snap.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un dataset (no snapshot)."),
                                     QStringLiteral("You must select a dataset (not a snapshot)."),
                                     QStringLiteral("必须选择数据集（不能是快照）。")));
        return;
    }

    const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = token.left(sep).toInt();
    ctx.poolName = token.mid(sep + 2);
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    QDialog dlg(this);
    dlg.setWindowTitle(tr3(QStringLiteral("Exportar dataset a directorio"),
                           QStringLiteral("Export dataset to directory"),
                           QStringLiteral("导出数据集到目录")));
    dlg.setModal(true);
    dlg.resize(720, 180);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QLabel* intro = new QLabel(
        tr3(QStringLiteral("Se copiará el contenido de %1 a un directorio local y luego se destruirá el dataset.")
                .arg(ds),
            QStringLiteral("Contents of %1 will be copied to a local directory and dataset will then be destroyed.")
                .arg(ds),
            QStringLiteral("将把 %1 的内容复制到本地目录，随后销毁该数据集。")
                .arg(ds)),
        &dlg);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QHBoxLayout* dirRow = new QHBoxLayout();
    QLabel* dirLabel = new QLabel(tr3(QStringLiteral("Directorio local"),
                                      QStringLiteral("Local directory"),
                                      QStringLiteral("本地目录")),
                                  &dlg);
    QLineEdit* dirEdit = new QLineEdit(&dlg);
    QPushButton* browseBtn = new QPushButton(
        tr3(QStringLiteral("Seleccionar..."), QStringLiteral("Select..."), QStringLiteral("选择...")),
        &dlg);
    dirRow->addWidget(dirLabel, 0);
    dirRow->addWidget(dirEdit, 1);
    dirRow->addWidget(browseBtn, 0);
    root->addLayout(dirRow);

    QObject::connect(browseBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString picked = QFileDialog::getExistingDirectory(
            &dlg,
            tr3(QStringLiteral("Seleccionar directorio local"),
                QStringLiteral("Select local directory"),
                QStringLiteral("选择本地目录")),
            dirEdit->text().trimmed());
        if (!picked.trimmed().isEmpty()) {
            dirEdit->setText(picked);
        }
    });

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* acceptBtn = buttons->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(acceptBtn, &QPushButton::clicked, &dlg, [&]() {
        if (dirEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un directorio local."),
                                     QStringLiteral("You must select a local directory."),
                                     QStringLiteral("必须选择本地目录。")));
            return;
        }
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QString localDir = dirEdit->text().trimmed();

    const bool isWin = isWindowsConnection(ctx.connIdx);
    QString cmd;
    bool allowWindowsScript = false;
    if (!isWin) {
        cmd = QStringLiteral(
                  "set -e; "
                  "DATASET=%1; "
                  "DST_DIR=%2; "
                  "RSYNC_OPTS='-aHWS'; "
                  "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                  "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                  "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                  "TMP_MP=$(mktemp -d /tmp/zfsmgr-todir-mp-XXXXXX); "
                  "TMP_OUT=$(mktemp -d /tmp/zfsmgr-todir-out-XXXXXX); "
                  "BACKUP_DIR=''; RESTORE_NEEDED=0; "
                  "OLD_MP=$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true); "
                  "OLD_MOUNTED=$(zfs get -H -o value mounted \"$DATASET\" 2>/dev/null || true); "
                  "cleanup(){ "
                  "  rc=$?; "
                  "  if [ $rc -ne 0 ]; then "
                  "    if [ \"$RESTORE_NEEDED\" = \"1\" ] && [ -n \"$BACKUP_DIR\" ] && [ -d \"$BACKUP_DIR\" ]; then "
                  "      rm -rf \"$DST_DIR\" >/dev/null 2>&1 || true; "
                  "      mv \"$BACKUP_DIR\" \"$DST_DIR\" >/dev/null 2>&1 || true; "
                  "    fi; "
                  "    if zfs list -H -o name \"$DATASET\" >/dev/null 2>&1; then "
                  "      zfs set mountpoint=\"$OLD_MP\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "      if [ \"$OLD_MOUNTED\" = \"yes\" ] || [ \"$OLD_MOUNTED\" = \"on\" ]; then zfs mount \"$DATASET\" >/dev/null 2>&1 || true; fi; "
                  "    fi; "
                  "  fi; "
                  "  rm -rf \"$TMP_MP\" >/dev/null 2>&1 || true; "
                  "  rm -rf \"$TMP_OUT\" >/dev/null 2>&1 || true; "
                  "  exit $rc; "
                  "}; "
                  "trap cleanup EXIT INT TERM; "
                  "if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx \"$DST_DIR\" >/dev/null 2>&1; then "
                  "  echo 'destination directory is already a zfs mountpoint'; exit 2; "
                  "fi; "
                  "zfs set canmount=on \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$TMP_MP\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "ACTIVE_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "[ \"$ACTIVE_MP\" = \"$TMP_MP\" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 3; }; "
                  "rsync $RSYNC_OPTS \"$TMP_MP\"/ \"$TMP_OUT\"/; "
                  "if [ -e \"$DST_DIR\" ]; then "
                  "  BACKUP_DIR=\"$DST_DIR.zfsmgr-bak-$$\"; "
                  "  i=0; while [ -e \"$BACKUP_DIR\" ]; do i=$((i+1)); BACKUP_DIR=\"$DST_DIR.zfsmgr-bak-$$-$i\"; done; "
                  "  mv \"$DST_DIR\" \"$BACKUP_DIR\"; "
                  "  RESTORE_NEEDED=1; "
                  "else "
                  "  mkdir -p \"$(dirname \"$DST_DIR\")\"; "
                  "fi; "
                  "mv \"$TMP_OUT\" \"$DST_DIR\"; "
                  "zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs destroy -r \"$DATASET\"; "
                  "if [ -n \"$BACKUP_DIR\" ]; then rm -rf \"$BACKUP_DIR\"; fi; "
                  "RESTORE_NEEDED=0; "
                  "trap - EXIT INT TERM; "
                  "rm -rf \"$TMP_MP\" >/dev/null 2>&1 || true")
                  .arg(shSingleQuote(ds),
                       shSingleQuote(localDir));
    } else {
        allowWindowsScript = true;
        auto psSingle = [](const QString& v) {
            QString out = v;
            out.replace(QStringLiteral("'"), QStringLiteral("''"));
            return QStringLiteral("'") + out + QStringLiteral("'");
        };
        cmd = QStringLiteral(
                  "$ErrorActionPreference = 'Stop'; "
                  "$dataset = %1; "
                  "$dstDir = %2; "
                  "$dstParent = Split-Path -Parent $dstDir; "
                  "if ([string]::IsNullOrWhiteSpace($dstParent)) { throw 'invalid destination directory'; } "
                  "if (-not (Test-Path -LiteralPath $dstParent)) { New-Item -ItemType Directory -Force -Path $dstParent | Out-Null; } "
                  "$mountRows = @(zfs mount 2>$null); "
                  "$used = $false; "
                  "foreach ($line in $mountRows) { "
                  "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "    $mp = $Matches[2].Trim(); "
                  "    if ([string]::Equals($mp, $dstDir, [System.StringComparison]::OrdinalIgnoreCase)) { $used = $true; break; } "
                  "  } "
                  "} "
                  "if ($used) { throw 'destination directory is already a zfs mountpoint'; } "
                  "$oldMp = (zfs get -H -o value mountpoint $dataset 2>$null | Select-Object -First 1); "
                  "$oldMounted = (zfs get -H -o value mounted $dataset 2>$null | Select-Object -First 1); "
                  "$tmpMp = Join-Path $env:TEMP ('zfsmgr-todir-mp-' + [Guid]::NewGuid().ToString('N')); "
                  "$tmpOut = Join-Path $env:TEMP ('zfsmgr-todir-out-' + [Guid]::NewGuid().ToString('N')); "
                  "New-Item -ItemType Directory -Force -Path $tmpMp | Out-Null; "
                  "New-Item -ItemType Directory -Force -Path $tmpOut | Out-Null; "
                  "$backupDir = ''; "
                  "$restoreNeeded = $false; "
                  "try { "
                  "  zfs set canmount=on $dataset 2>$null | Out-Null; "
                  "  zfs set mountpoint=$tmpMp $dataset | Out-Null; "
                  "  zfs mount $dataset 2>$null | Out-Null; "
                  "  $activeMp = ''; "
                  "  foreach ($line in @(zfs mount 2>$null)) { "
                  "    if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "      if ($Matches[1] -eq $dataset) { $activeMp = $Matches[2].Trim(); break; } "
                  "    } "
                  "  } "
                  "  if (-not [string]::Equals($activeMp, $tmpMp, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'could not mount dataset on temporary mountpoint'; } "
                  "  $rc = (robocopy $tmpMp $tmpOut /E /COPYALL /R:1 /W:1 /NFL /NDL /NP); "
                  "  if ($LASTEXITCODE -ge 8) { throw ('robocopy failed with code ' + $LASTEXITCODE); } "
                  "  if (Test-Path -LiteralPath $dstDir) { "
                  "    $backupDir = $dstDir + '.zfsmgr-bak-' + [Guid]::NewGuid().ToString('N'); "
                  "    Move-Item -LiteralPath $dstDir -Destination $backupDir; "
                  "    $restoreNeeded = $true; "
                  "  } "
                  "  Move-Item -LiteralPath $tmpOut -Destination $dstDir; "
                  "  zfs unmount $dataset 2>$null | Out-Null; "
                  "  zfs destroy -r $dataset | Out-Null; "
                  "  if ($backupDir -and (Test-Path -LiteralPath $backupDir)) { Remove-Item -LiteralPath $backupDir -Recurse -Force; } "
                  "  $restoreNeeded = $false; "
                  "} catch { "
                  "  if ($restoreNeeded -and $backupDir -and (Test-Path -LiteralPath $backupDir)) { "
                  "    if (Test-Path -LiteralPath $dstDir) { Remove-Item -LiteralPath $dstDir -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "    Move-Item -LiteralPath $backupDir -Destination $dstDir -ErrorAction SilentlyContinue; "
                  "  } "
                  "  if (zfs list -H -o name $dataset 2>$null) { "
                  "    if ($oldMp) { zfs set mountpoint=$oldMp $dataset 2>$null | Out-Null; } "
                  "    if ($oldMounted -match '^(yes|on)$') { zfs mount $dataset 2>$null | Out-Null; } "
                  "  } "
                  "  throw; "
                  "} finally { "
                  "  if (Test-Path -LiteralPath $tmpMp) { Remove-Item -LiteralPath $tmpMp -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "  if (Test-Path -LiteralPath $tmpOut) { Remove-Item -LiteralPath $tmpOut -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "}")
                  .arg(psSingle(ds),
                       psSingle(localDir));
    }

    executeDatasetAction(QStringLiteral("advanced"),
                         tr3(QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")),
                         ctx,
                         cmd,
                         0,
                         allowWindowsScript);
}
