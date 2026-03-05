#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QAction>
#include <QComboBox>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::oneLine;
using mwhelpers::parentDatasetName;
using mwhelpers::shSingleQuote;
} // namespace

void MainWindow::showDatasetContextMenu(const QString& side, QTreeWidget* tree, const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    QTreeWidgetItem* item = tree->itemAt(pos);
    if (!item) {
        return;
    }
    tree->setCurrentItem(item);
    if (side == QStringLiteral("origin")) {
        onOriginTreeSelectionChanged();
    } else if (side == QStringLiteral("dest")) {
        onDestTreeSelectionChanged();
    } else {
        refreshDatasetProperties(QStringLiteral("advanced"));
        const QString ds = item->data(0, Qt::UserRole).toString();
        const QString snap = item->data(1, Qt::UserRole).toString();
        updateAdvancedSelectionUi(ds, snap);
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }

    QMenu menu(this);
    QAction* mountAct = menu.addAction(tr3(QStringLiteral("Montar"), QStringLiteral("Mount"), QStringLiteral("挂载")));
    QAction* mountWithChildrenAct = menu.addAction(tr3(QStringLiteral("Montar con todos los hijos"),
                                                       QStringLiteral("Mount with all children"),
                                                       QStringLiteral("挂载并包含所有子项")));
    QAction* umountAct = menu.addAction(tr3(QStringLiteral("Desmontar"), QStringLiteral("Unmount"), QStringLiteral("卸载")));
    QAction* rollbackAct = nullptr;
    if (!ctx.snapshotName.isEmpty()) {
        rollbackAct = menu.addAction(QStringLiteral("Rollback"));
    }
    menu.addSeparator();
    QAction* createAct = menu.addAction(tr3(QStringLiteral("Crear hijo"), QStringLiteral("Create child"), QStringLiteral("创建子项")));
    QAction* deleteAct = menu.addAction(tr3(QStringLiteral("Borrar"), QStringLiteral("Delete"), QStringLiteral("删除")));
    const bool isWinConn = isWindowsConnection(ctx.connIdx);

    if (!ctx.snapshotName.isEmpty()) {
        mountAct->setEnabled(false);
        mountWithChildrenAct->setEnabled(false);
        umountAct->setEnabled(false);
        createAct->setEnabled(false);
    } else {
        bool knownMounted = false;
        bool isMounted = false;
        const QString key = datasetCacheKey(ctx.connIdx, ctx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt != m_poolDatasetCache.constEnd()) {
            const auto recIt = cacheIt->recordByName.constFind(ctx.datasetName);
            if (recIt != cacheIt->recordByName.constEnd()) {
                const QString m = recIt->mounted.trimmed().toLower();
                if (m == QStringLiteral("yes") || m == QStringLiteral("on") || m == QStringLiteral("true") || m == QStringLiteral("1")) {
                    knownMounted = true;
                    isMounted = true;
                } else if (m == QStringLiteral("no") || m == QStringLiteral("off") || m == QStringLiteral("false") || m == QStringLiteral("0")) {
                    knownMounted = true;
                    isMounted = false;
                }
            }
        }
        if (knownMounted) {
            mountAct->setEnabled(!isMounted);
            umountAct->setEnabled(isMounted);
        }
    }
    Q_UNUSED(isWinConn);

    QAction* picked = menu.exec(tree->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == mountAct) {
        logUiAction(QStringLiteral("Montar dataset (menú)"));
        actionMountDataset(side);
    } else if (picked == mountWithChildrenAct) {
        logUiAction(QStringLiteral("Montar dataset con hijos (menú)"));
        actionMountDatasetWithChildren(side);
    } else if (picked == umountAct) {
        logUiAction(QStringLiteral("Desmontar dataset (menú)"));
        actionUmountDataset(side);
    } else if (picked == createAct) {
        logUiAction(QStringLiteral("Crear hijo dataset (menú)"));
        actionCreateChildDataset(side);
    } else if (rollbackAct && picked == rollbackAct) {
        const QString snapObj = QStringLiteral("%1@%2").arg(ctx.datasetName, ctx.snapshotName);
        const auto confirm = QMessageBox::question(
            this,
            QStringLiteral("Rollback"),
            QStringLiteral("¿Confirmar rollback de snapshot?\n%1").arg(snapObj),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirm != QMessageBox::Yes) {
            return;
        }
        logUiAction(QStringLiteral("Rollback snapshot (menú)"));
        const QString cmd = QStringLiteral("zfs rollback %1").arg(shSingleQuote(snapObj));
        executeDatasetAction(side, QStringLiteral("Rollback"), ctx, cmd, 90000);
    } else if (picked == deleteAct) {
        logUiAction(QStringLiteral("Borrar dataset/snapshot (menú)"));
        actionDeleteDatasetOrSnapshot(side);
    }
}

bool MainWindow::executeDatasetAction(const QString& side, const QString& actionName, const DatasetSelectionContext& ctx, const QString& cmd, int timeoutMs, bool allowWindowsScript) {
    if (!ctx.valid) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    if (isWindowsConnection(p) && !allowWindowsScript) {
        const QString c = cmd.toLower();
        const bool unixScriptLike = c.contains(QStringLiteral("&&"))
            || c.contains(QStringLiteral(" set -e"))
            || c.contains(QStringLiteral("trap "))
            || c.contains(QStringLiteral("awk "))
            || c.contains(QStringLiteral("grep "))
            || c.contains(QStringLiteral("mktemp "))
            || c.contains(QStringLiteral("rsync "))
            || c.contains(QStringLiteral("tail "));
        if (unixScriptLike) {
            QMessageBox::information(
                this,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("La acción \"%1\" usa shell Unix y no está disponible en conexiones Windows por ahora.")
                        .arg(actionName),
                    QStringLiteral("Action \"%1\" uses Unix shell and is not available on Windows connections yet.")
                        .arg(actionName),
                    QStringLiteral("操作“%1”依赖 Unix shell，当前在 Windows 连接中不可用。")
                        .arg(actionName)));
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Acción no soportada en Windows: %1 (%2)").arg(actionName, p.name));
            return false;
        }
    }
    QString remoteCmd = withSudo(p, cmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, remoteCmd));
    if (!confirmActionExecution(actionName, {preview})) {
        return false;
    }
    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 %2::%3").arg(actionName, p.name, ctx.datasetName));
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, remoteCmd, timeoutMs, out, err, rc) || rc != 0) {
        QString failureDetail = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
        if (!out.trimmed().isEmpty()) {
            failureDetail += QStringLiteral("\n\nstdout:\n%1").arg(out.trimmed());
        }
        if (actionName == QStringLiteral("Desmontar")) {
            const QString diag = diagnoseUmountFailure(ctx).trimmed();
            if (!diag.isEmpty()) {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("Diagnóstico de desmontaje %1::%2 -> %3")
                           .arg(p.name, ctx.datasetName, oneLine(diag)));
                failureDetail += QStringLiteral("\n\nProcesos/diagnóstico sobre el mountpoint:\n%1").arg(diag);
            }
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error en %1: %2")
                   .arg(actionName, oneLine(failureDetail)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("%1 falló:\n%2"),
                                  QStringLiteral("%1 failed:\n%2"),
                                  QStringLiteral("%1 失败：\n%2"))
                                  .arg(actionName, failureDetail));
        setActionsLocked(false);
        return false;
    }
    if (!out.trimmed().isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 finalizado").arg(actionName));
    invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
    const bool needsDeferredRefresh =
        (actionName == QStringLiteral("Montar")
         || actionName == QStringLiteral("Montar con todos los hijos")
         || actionName == QStringLiteral("Desmontar")
         || actionName == QStringLiteral("Desde Dir")
         || actionName == QStringLiteral("Desglosar"));
    if (needsDeferredRefresh) {
        const int refreshIdx = ctx.connIdx;
        QTimer::singleShot(0, this, [this, refreshIdx]() {
            refreshConnectionByIndex(refreshIdx);
            setActionsLocked(false);
        });
        return true;
    }
    reloadDatasetSide(side);
    setActionsLocked(false);
    return true;
}

QString MainWindow::diagnoseUmountFailure(const DatasetSelectionContext& ctx) {
    if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
        return QString();
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString mp;
    QString mpHint;
    QString mountedValue;
    getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mountpoint"), mpHint);
    getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mounted"), mountedValue);
    mp = effectiveMountPath(ctx.connIdx, ctx.poolName, ctx.datasetName, mpHint, mountedValue).trimmed();
    if (mp.isEmpty()) {
        mp = mpHint.trimmed();
    }
    if (mp.isEmpty()) {
        return tr3(QStringLiteral("No se pudo resolver el mountpoint para diagnóstico."),
                   QStringLiteral("Could not resolve mountpoint for diagnostics."),
                   QStringLiteral("无法解析用于诊断的挂载点。"));
    }

    QString out;
    QString err;
    int rc = -1;
    QString diagCmd;
    if (isWindowsConnection(p)) {
        QString dsPs = ctx.datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        QString mpPs = mp;
        mpPs.replace('\'', QStringLiteral("''"));
        diagCmd = QStringLiteral(
                      "$ds='%1'; $mp='%2'; "
                      "Write-Output ('dataset=' + $ds); "
                      "Write-Output ('mountpoint=' + $mp); "
                      "Write-Output 'No hay lsof/fuser por defecto en Windows para identificar el proceso bloqueante.'")
                      .arg(dsPs, mpPs);
    } else {
        const QString mpQ = shSingleQuote(mp);
        diagCmd = QStringLiteral(
                      "MP=%1; "
                      "echo \"mountpoint=$MP\"; "
                      "if command -v lsof >/dev/null 2>&1; then "
                      "  echo \"--- lsof ---\"; "
                      "  lsof +f -- \"$MP\" 2>/dev/null | head -n 80; "
                      "else "
                      "  echo \"lsof no disponible\"; "
                      "fi; "
                      "if command -v fuser >/dev/null 2>&1; then "
                      "  echo \"--- fuser ---\"; "
                      "  fuser -vm \"$MP\" 2>/dev/null | head -n 80; "
                      "else "
                      "  echo \"fuser no disponible\"; "
                      "fi")
                      .arg(mpQ);
    }

    if (!runSsh(p, withSudo(p, diagCmd), 15000, out, err, rc)) {
        return tr3(QStringLiteral("No se pudo ejecutar el diagnóstico remoto."),
                   QStringLiteral("Could not execute remote diagnostics."),
                   QStringLiteral("无法执行远程诊断。"));
    }
    if (rc != 0 && out.trimmed().isEmpty()) {
        return err.trimmed().isEmpty() ? QStringLiteral("diagnostic exit %1").arg(rc) : err.trimmed();
    }
    if (!out.trimmed().isEmpty()) {
        return out.trimmed();
    }
    return err.trimmed();
}

void MainWindow::invalidateDatasetCacheForPool(int connIdx, const QString& poolName) {
    m_poolDatasetCache.remove(datasetCacheKey(connIdx, poolName));
}

void MainWindow::reloadDatasetSide(const QString& side) {
    if (side == QStringLiteral("origin")) {
        onOriginPoolChanged();
    } else if (side == QStringLiteral("dest")) {
        onDestPoolChanged();
    } else {
        const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep > 0) {
            const int connIdx = token.left(sep).toInt();
            const QString poolName = token.mid(sep + 2);
            populateDatasetTree(m_advTree, connIdx, poolName, QStringLiteral("advanced"));
            refreshDatasetProperties(QStringLiteral("advanced"));
        }
    }
}

void MainWindow::actionMountDataset(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    mountDataset(side, ctx);
}

bool MainWindow::mountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return false;
    }
    if (!ensureParentMountedBeforeMount(ctx)) {
        return false;
    }
    if (!ensureNoMountpointConflictsBeforeMount(ctx, false)) {
        return false;
    }
    if (isWindowsConnection(ctx.connIdx)) {
        const ConnectionProfile& p = m_profiles[ctx.connIdx];
        QString effectiveMp = effectiveMountPath(ctx.connIdx,
                                                 ctx.poolName,
                                                 ctx.datasetName,
                                                 QString(),
                                                 QStringLiteral("yes"));
        if (effectiveMp.trimmed().isEmpty()) {
            QString mpRaw;
            if (getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mountpoint"), mpRaw)) {
                effectiveMp = mpRaw.trimmed();
            }
        }
        const QString precheckCmd = mwhelpers::buildWindowsMountPrecheckCommand(ctx.datasetName, effectiveMp);
        QString preOut;
        QString preErr;
        int preRc = -1;
        if (!runSsh(p, withSudo(p, precheckCmd), 15000, preOut, preErr, preRc) || preRc != 0) {
            const QString reason = oneLine((preErr.isEmpty() ? preOut : preErr));
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("No se puede montar %1.\n%2").arg(ctx.datasetName, reason),
                    QStringLiteral("Cannot mount %1.\n%2").arg(ctx.datasetName, reason),
                    QStringLiteral("无法挂载 %1。\n%2").arg(ctx.datasetName, reason)));
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Precheck montar falló %1::%2 -> %3")
                       .arg(p.name, ctx.datasetName, reason));
            return false;
        }
    }
    const QString cmd = mwhelpers::buildSingleMountCommand(ctx.datasetName);
    return executeDatasetAction(side, QStringLiteral("Montar"), ctx, cmd);
}

void MainWindow::actionMountDatasetWithChildren(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    if (!ensureParentMountedBeforeMount(ctx)) {
        return;
    }
    if (!ensureNoMountpointConflictsBeforeMount(ctx, true)) {
        return;
    }
    if (isWindowsConnection(ctx.connIdx)) {
        const QString cmd = mwhelpers::buildMountChildrenCommand(true, ctx.datasetName);
        executeDatasetAction(side, QStringLiteral("Montar con todos los hijos"), ctx, cmd, 90000, true);
        return;
    }
    const QString cmd = mwhelpers::buildMountChildrenCommand(false, ctx.datasetName);
    executeDatasetAction(side, QStringLiteral("Montar con todos los hijos"), ctx, cmd);
}

bool MainWindow::ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx) {
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        return false;
    }
    const QString parent = parentDatasetName(ctx.datasetName);
    if (parent.isEmpty()) {
        return true;
    }

    QString parentMountpoint;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("mountpoint"), parentMountpoint)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar mountpoint del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mountpoint %1").arg(parent),
                                 QStringLiteral("无法检查父数据集 mountpoint：%1").arg(parent)));
        return false;
    }
    QString parentCanmount;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("canmount"), parentCanmount)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar canmount del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent canmount %1").arg(parent),
                                 QStringLiteral("无法检查父数据集 canmount：%1").arg(parent)));
        return false;
    }
    QString parentMounted;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("mounted"), parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar estado mounted del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mounted state %1").arg(parent),
                                 QStringLiteral("无法检查父数据集挂载状态：%1").arg(parent)));
        return false;
    }
    if (!mwhelpers::parentAllowsChildMount(parentMountpoint, parentCanmount, parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("El dataset padre %1 no está montado, móntelo antes por favor").arg(parent),
                                 QStringLiteral("Parent dataset %1 is not mounted, mount it first").arg(parent),
                                 QStringLiteral("父数据集 %1 未挂载，请先挂载").arg(parent)));
        return false;
    }
    return true;
}

bool MainWindow::ensureNoMountpointConflictsBeforeMount(const DatasetSelectionContext& ctx, bool includeDescendants) {
    if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size() || ctx.datasetName.isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];

    if (!ensureDatasetsLoaded(ctx.connIdx, ctx.poolName)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar conflictos de mountpoint."),
                                 QStringLiteral("Could not validate mountpoint conflicts."),
                                 QStringLiteral("无法检查挂载点冲突。")));
        return false;
    }
    const QString key = datasetCacheKey(ctx.connIdx, ctx.poolName);
    const auto cacheIt = m_poolDatasetCache.constFind(key);
    if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar conflictos de mountpoint."),
                                 QStringLiteral("Could not validate mountpoint conflicts."),
                                 QStringLiteral("无法检查挂载点冲突。")));
        return false;
    }
    const PoolDatasetCache& cache = cacheIt.value();

    QMap<QString, QString> targetMpByDs;
    const QString prefix = ctx.datasetName + QStringLiteral("/");
    for (auto it = cache.recordByName.constBegin(); it != cache.recordByName.constEnd(); ++it) {
        const QString ds = it.key();
        if (ds != ctx.datasetName) {
            if (!includeDescendants || !ds.startsWith(prefix)) {
                continue;
            }
        }
        const DatasetRecord& rec = it.value();
        const QString mp = effectiveMountPath(ctx.connIdx, ctx.poolName, ds, rec.mountpoint, rec.mounted);
        const QString mpl = mp.trimmed().toLower();
        if (ds.isEmpty() || mp.isEmpty() || mpl == QStringLiteral("none") || mpl == QStringLiteral("-")) {
            continue;
        }
        targetMpByDs[ds] = mp;
    }

    const QMap<QString, QStringList> duplicateMps = mwhelpers::duplicateMountpoints(targetMpByDs);
    for (auto it = duplicateMps.constBegin(); it != duplicateMps.constEnd(); ++it) {
        const QStringList dsList = it.value();
        if (!dsList.isEmpty()) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("Conflicto de mountpoint dentro de la selección.\nMountpoint: %1\nDatasets:\n%2")
                        .arg(it.key(), dsList.join('\n')),
                    QStringLiteral("Mountpoint conflict inside selection.\nMountpoint: %1\nDatasets:\n%2")
                        .arg(it.key(), dsList.join('\n')),
                    QStringLiteral("所选项内部存在挂载点冲突。\n挂载点：%1\n数据集：\n%2")
                        .arg(it.key(), dsList.join('\n'))));
            return false;
        }
    }

    QString mountedOut;
    QString mountedErr;
    int mountedRc = -1;
    const QString mountedCmd = withSudo(p, QStringLiteral("zfs mount"));
    if (!runSsh(p, mountedCmd, 20000, mountedOut, mountedErr, mountedRc) || mountedRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo leer datasets montados."),
                                 QStringLiteral("Could not read mounted datasets."),
                                 QStringLiteral("无法读取已挂载数据集。")));
        return false;
    }

    QMap<QString, QStringList> mountedByMp;
    for (const QString& ln : mountedOut.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = ln.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const int sp = trimmed.indexOf(' ');
        if (sp <= 0) {
            continue;
        }
        const QString ds = trimmed.left(sp).trimmed();
        const QString mp = trimmed.mid(sp + 1).trimmed();
        if (ds.isEmpty() || mp.isEmpty()) {
            continue;
        }
        mountedByMp[mp].push_back(ds);
    }

    const QVector<mwhelpers::MountpointConflict> conflicts =
        mwhelpers::externalMountpointConflicts(targetMpByDs, mountedByMp);
    if (!conflicts.isEmpty()) {
        const mwhelpers::MountpointConflict& c = conflicts.front();
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("No se permite montar más de un dataset en el mismo directorio.\nMountpoint: %1\nMontado: %2\nSolicitado: %3")
                    .arg(c.mountpoint, c.mountedDataset, c.requestedDataset),
                QStringLiteral("Only one mounted dataset per directory is allowed.\nMountpoint: %1\nMounted: %2\nRequested: %3")
                    .arg(c.mountpoint, c.mountedDataset, c.requestedDataset),
                QStringLiteral("同一目录不允许挂载多个数据集。\n挂载点：%1\n已挂载：%2\n请求：%3")
                    .arg(c.mountpoint, c.mountedDataset, c.requestedDataset)));
        return false;
    }
    return true;
}

void MainWindow::actionUmountDataset(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    umountDataset(side, ctx);
}

bool MainWindow::umountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return false;
    }
    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString hasChildrenCmd = mwhelpers::buildHasMountedChildrenCommand(isWin, ctx.datasetName);

    QString out;
    QString err;
    int rc = -1;
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString checkCmd = withSudo(p, hasChildrenCmd);
    const bool ran = runSsh(p, checkCmd, 12000, out, err, rc);
    bool hasChildrenMounted = ran && rc == 0;
    QString cmd;
    if (hasChildrenMounted) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("ZFSMgr"),
            QStringLiteral("Hay hijos montados bajo %1.\n¿Desmontar recursivamente?").arg(ctx.datasetName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Desmontar abortado por usuario"));
            return false;
        }
        cmd = mwhelpers::buildRecursiveUmountCommand(isWin, ctx.datasetName);
    } else {
        cmd = mwhelpers::buildSingleUmountCommand(isWin, ctx.datasetName);
    }
    return executeDatasetAction(side, QStringLiteral("Desmontar"), ctx, cmd, 90000, isWin);
}
