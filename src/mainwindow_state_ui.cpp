#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {
using mwhelpers::isMountedValueTrue;
} // namespace

void MainWindow::updateTransferButtonsState() {
    if (actionsLocked()) {
        if (m_btnCopy) m_btnCopy->setEnabled(false);
        if (m_btnLevel) m_btnLevel->setEnabled(false);
        if (m_btnSync) m_btnSync->setEnabled(false);
        if (m_btnConnBreakdown) m_btnConnBreakdown->setEnabled(false);
        if (m_btnConnAssemble) m_btnConnAssemble->setEnabled(false);
        if (m_btnConnFromDir) m_btnConnFromDir->setEnabled(false);
        if (m_btnConnToDir) m_btnConnToDir->setEnabled(false);
        if (m_btnConnCopy) m_btnConnCopy->setEnabled(false);
        if (m_btnConnLevel) m_btnConnLevel->setEnabled(false);
        if (m_btnConnSync) m_btnConnSync->setEnabled(false);
        updateConnectionActionsState();
        return;
    }
    const bool srcDs = !m_originSelectedDataset.isEmpty();
    const bool srcSnap = !m_originSelectedSnapshot.isEmpty();
    const bool dstDs = !m_destSelectedDataset.isEmpty();
    const bool dstSnap = !m_destSelectedSnapshot.isEmpty();
    const QString srcSel = srcDs ? (srcSnap ? QStringLiteral("%1@%2").arg(m_originSelectedDataset, m_originSelectedSnapshot)
                                            : m_originSelectedDataset)
                                 : QString();
    const QString dstSel = dstDs ? (dstSnap ? QStringLiteral("%1@%2").arg(m_destSelectedDataset, m_destSelectedSnapshot)
                                            : m_destSelectedDataset)
                                 : QString();
    const DatasetSelectionContext srcCtx = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dstCtx = currentDatasetSelection(QStringLiteral("dest"));
    const bool srcSelectionConsistent = srcCtx.valid
        && srcCtx.datasetName == m_originSelectedDataset
        && srcCtx.snapshotName == m_originSelectedSnapshot;
    const bool dstSelectionConsistent = dstCtx.valid
        && dstCtx.datasetName == m_destSelectedDataset
        && dstCtx.snapshotName == m_destSelectedSnapshot;
    auto datasetMountedInCache = [this](const DatasetSelectionContext& c) -> bool {
        if (!c.valid || c.datasetName.isEmpty()) {
            return false;
        }
        const QString key = datasetCacheKey(c.connIdx, c.poolName);
        const auto it = m_poolDatasetCache.constFind(key);
        if (it == m_poolDatasetCache.constEnd() || !it->loaded) {
            return false;
        }
        const auto recIt = it->recordByName.constFind(c.datasetName);
        if (recIt == it->recordByName.constEnd()) {
            return false;
        }
        return isMountedValueTrue(recIt->mounted);
    };
    const mwhelpers::TransferButtonInputs transferIn{
        srcDs,
        srcSnap,
        dstDs,
        dstSnap,
        srcSel,
        dstSel,
        srcSelectionConsistent,
        dstSelectionConsistent,
        datasetMountedInCache(srcCtx),
        datasetMountedInCache(dstCtx),
    };
    const mwhelpers::TransferButtonState transferState = mwhelpers::computeTransferButtonState(transferIn);
    m_btnCopy->setEnabled(transferState.copyEnabled);
    m_btnLevel->setEnabled(transferState.levelEnabled);
    m_btnSync->setEnabled(transferState.syncEnabled);
    updateConnectionActionsState();
}

void MainWindow::setConnectionOriginSelection(const DatasetSelectionContext& ctx) {
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        m_connActionOrigin = DatasetSelectionContext{};
    } else {
        m_connActionOrigin = ctx;
    }
    updateConnectionActionsState();
}

void MainWindow::setConnectionDestinationSelection(const DatasetSelectionContext& ctx) {
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        m_connActionDest = DatasetSelectionContext{};
    } else {
        m_connActionDest = ctx;
    }
    updateConnectionActionsState();
}

void MainWindow::updateConnectionActionsState() {
    if (m_btnConnBreakdown) m_btnConnBreakdown->setText(
        trk(QStringLiteral("t_breakdown_btn1"), QStringLiteral("Desglosar"), QStringLiteral("Break down"), QStringLiteral("拆分")));
    if (m_btnConnAssemble) m_btnConnAssemble->setText(
        trk(QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("组装")));
    if (m_btnConnFromDir) m_btnConnFromDir->setText(
        trk(QStringLiteral("t_from_dir_btn1"), QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")));
    if (m_btnConnToDir) m_btnConnToDir->setText(
        trk(QStringLiteral("t_to_dir_btn_001"), QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")));
    if (m_btnConnCopy) m_btnConnCopy->setText(
        trk(QStringLiteral("t_copy_001"), QStringLiteral("Copiar"), QStringLiteral("Copy"), QStringLiteral("复制")));
    if (m_btnConnLevel) m_btnConnLevel->setText(
        trk(QStringLiteral("t_level_btn_001"), QStringLiteral("Nivelar"), QStringLiteral("Level"), QStringLiteral("同步快照")));
    if (m_btnConnSync) m_btnConnSync->setText(
        trk(QStringLiteral("t_sync_btn_001"), QStringLiteral("Sincronizar"), QStringLiteral("Sync"), QStringLiteral("同步文件")));

    const DatasetSelectionContext dctx = currentDatasetSelection(QStringLiteral("conncontent"));
    auto fmtSel = [this](const DatasetSelectionContext& c) -> QString {
        if (!c.valid || c.datasetName.isEmpty() || c.connIdx < 0 || c.connIdx >= m_profiles.size()) {
            return trk(QStringLiteral("t_empty_sel_001"),
                       QStringLiteral("(vacío)"),
                       QStringLiteral("(empty)"),
                       QStringLiteral("（空）"));
        }
        const QString base = c.snapshotName.isEmpty()
                                 ? c.datasetName
                                 : QStringLiteral("%1@%2").arg(c.datasetName, c.snapshotName);
        return QStringLiteral("%1::%2").arg(m_profiles[c.connIdx].name, base);
    };
    QString dstText = trk(QStringLiteral("t_conn_dest_sel01"),
                          QStringLiteral("Destino:%1"),
                          QStringLiteral("Target:%1"),
                          QStringLiteral("目标：%1")).arg(fmtSel(m_connActionDest));
    if (m_connDestSelectionLabel) {
        m_connDestSelectionLabel->setText(dstText);
    }
    if (m_connOriginSelectionLabel) {
        m_connOriginSelectionLabel->setText(
            trk(QStringLiteral("t_conn_origin_sel1"),
                QStringLiteral("Origen:%1"),
                QStringLiteral("Source:%1"),
                QStringLiteral("源：%1"))
                .arg(fmtSel(m_connActionOrigin)));
    }

    if (actionsLocked()) {
        if (m_btnConnBreakdown) m_btnConnBreakdown->setEnabled(false);
        if (m_btnConnAssemble) m_btnConnAssemble->setEnabled(false);
        if (m_btnConnFromDir) m_btnConnFromDir->setEnabled(false);
        if (m_btnConnToDir) m_btnConnToDir->setEnabled(false);
        if (m_btnConnCopy) m_btnConnCopy->setEnabled(false);
        if (m_btnConnLevel) m_btnConnLevel->setEnabled(false);
        if (m_btnConnSync) m_btnConnSync->setEnabled(false);
        if (m_activeConnActionBtn) {
            const QString actionName = m_activeConnActionName.isEmpty()
                                           ? m_activeConnActionBtn->text()
                                           : m_activeConnActionName;
            m_activeConnActionBtn->setText(
                trk(QStringLiteral("t_cancel_action1"),
                    QStringLiteral("Cancelar %1"),
                    QStringLiteral("Cancel %1"),
                    QStringLiteral("取消 %1")).arg(actionName));
            m_activeConnActionBtn->setEnabled(true);
        }
        return;
    }

    bool connAdvDatasetOnly = dctx.valid && !dctx.datasetName.isEmpty() && dctx.snapshotName.isEmpty();
    if (connAdvDatasetOnly) {
        const QString key = datasetCacheKey(dctx.connIdx, dctx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
            connAdvDatasetOnly = false;
        } else {
            const QString base = dctx.datasetName;
            const QString pref = base + QStringLiteral("/");
            for (auto it = cacheIt->recordByName.constBegin(); it != cacheIt->recordByName.constEnd(); ++it) {
                const QString& ds = it.key();
                if (ds != base && !ds.startsWith(pref)) {
                    continue;
                }
                if (!isMountedValueTrue(it.value().mounted)) {
                    connAdvDatasetOnly = false;
                    break;
                }
            }
        }
    }
    if (m_btnConnBreakdown) m_btnConnBreakdown->setEnabled(!actionsLocked() && connAdvDatasetOnly);
    if (m_btnConnAssemble) m_btnConnAssemble->setEnabled(!actionsLocked() && connAdvDatasetOnly);
    if (m_btnConnFromDir) m_btnConnFromDir->setEnabled(!actionsLocked() && dctx.valid && !dctx.datasetName.isEmpty() && dctx.snapshotName.isEmpty());
    if (m_btnConnToDir) m_btnConnToDir->setEnabled(!actionsLocked() && dctx.valid && !dctx.datasetName.isEmpty() && dctx.snapshotName.isEmpty());
    const bool srcDs = m_connActionOrigin.valid && !m_connActionOrigin.datasetName.isEmpty();
    const bool srcSnap = srcDs && !m_connActionOrigin.snapshotName.isEmpty();
    const bool dstDs = m_connActionDest.valid && !m_connActionDest.datasetName.isEmpty();
    const bool dstSnap = dstDs && !m_connActionDest.snapshotName.isEmpty();
    const QString srcSel = srcDs ? (srcSnap ? QStringLiteral("%1@%2").arg(m_connActionOrigin.datasetName, m_connActionOrigin.snapshotName)
                                            : m_connActionOrigin.datasetName)
                                 : QString();
    const QString dstSel = dstDs ? (dstSnap ? QStringLiteral("%1@%2").arg(m_connActionDest.datasetName, m_connActionDest.snapshotName)
                                            : m_connActionDest.datasetName)
                                 : QString();
    auto datasetMountedInCache = [this](const DatasetSelectionContext& c) -> bool {
        if (!c.valid || c.datasetName.isEmpty()) {
            return false;
        }
        const QString key = datasetCacheKey(c.connIdx, c.poolName);
        const auto it = m_poolDatasetCache.constFind(key);
        if (it == m_poolDatasetCache.constEnd() || !it->loaded) {
            return false;
        }
        const auto recIt = it->recordByName.constFind(c.datasetName);
        if (recIt == it->recordByName.constEnd()) {
            return false;
        }
        return isMountedValueTrue(recIt->mounted);
    };
    const mwhelpers::TransferButtonInputs transferIn{
        srcDs,
        srcSnap,
        dstDs,
        dstSnap,
        srcSel,
        dstSel,
        srcDs,
        dstDs,
        datasetMountedInCache(m_connActionOrigin),
        datasetMountedInCache(m_connActionDest),
    };
    const mwhelpers::TransferButtonState st = mwhelpers::computeTransferButtonState(transferIn);
    if (m_btnConnCopy) m_btnConnCopy->setEnabled(!actionsLocked() && st.copyEnabled);
    if (m_btnConnLevel) m_btnConnLevel->setEnabled(!actionsLocked() && st.levelEnabled);
    if (m_btnConnSync) m_btnConnSync->setEnabled(!actionsLocked() && st.syncEnabled);

    const bool hasConnSel = dctx.valid && !dctx.datasetName.isEmpty();
    const bool hasConnSnap = hasConnSel && !dctx.snapshotName.isEmpty();
    const bool alreadyOrigin = hasConnSel
        && m_connActionOrigin.valid
        && dctx.connIdx == m_connActionOrigin.connIdx
        && dctx.poolName == m_connActionOrigin.poolName
        && dctx.datasetName == m_connActionOrigin.datasetName
        && dctx.snapshotName == m_connActionOrigin.snapshotName;
    const bool alreadyDest = hasConnSel
        && m_connActionDest.valid
        && dctx.connIdx == m_connActionDest.connIdx
        && dctx.poolName == m_connActionDest.poolName
        && dctx.datasetName == m_connActionDest.datasetName
        && dctx.snapshotName == m_connActionDest.snapshotName;
    Q_UNUSED(alreadyOrigin);
    Q_UNUSED(alreadyDest);
    Q_UNUSED(hasConnSnap);
    Q_UNUSED(hasConnSel);
}

void MainWindow::executeConnectionAdvancedAction(const QString& action) {
    const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        return;
    }

    if (action == QStringLiteral("breakdown")) {
        actionAdvancedBreakdown();
    } else if (action == QStringLiteral("assemble")) {
        actionAdvancedAssemble();
    } else if (action == QStringLiteral("fromdir")) {
        actionAdvancedCreateFromDir();
    } else if (action == QStringLiteral("todir")) {
        actionAdvancedToDir();
    }
}

void MainWindow::executeConnectionTransferAction(const QString& action) {
    const DatasetSelectionContext src = m_connActionOrigin;
    const DatasetSelectionContext dst = m_connActionDest;
    if (!src.valid || src.datasetName.isEmpty() || !dst.valid || dst.datasetName.isEmpty()) {
        return;
    }
    // Fuerza el uso de la selección real Origen/Destino de Conexiones
    // (árbol superior/inferior), evitando depender de combos legacy ocultos.
    m_transferSelectionOverrideActive = true;
    m_transferSelectionOverrideOrigin = src;
    m_transferSelectionOverrideDest = dst;
    const QString oldOriginDs = m_originSelectedDataset;
    const QString oldOriginSnap = m_originSelectedSnapshot;
    const QString oldDestDs = m_destSelectedDataset;
    const QString oldDestSnap = m_destSelectedSnapshot;
    m_originSelectedDataset = src.datasetName;
    m_originSelectedSnapshot = src.snapshotName;
    m_destSelectedDataset = dst.datasetName;
    m_destSelectedSnapshot = dst.snapshotName;

    if (action == QStringLiteral("copy")) {
        actionCopySnapshot();
    } else if (action == QStringLiteral("level")) {
        actionLevelSnapshot();
    } else if (action == QStringLiteral("sync")) {
        actionSyncDatasets();
    }
    m_transferSelectionOverrideActive = false;
    m_transferSelectionOverrideOrigin = DatasetSelectionContext{};
    m_transferSelectionOverrideDest = DatasetSelectionContext{};
    m_originSelectedDataset = oldOriginDs;
    m_originSelectedSnapshot = oldOriginSnap;
    m_destSelectedDataset = oldDestDs;
    m_destSelectedSnapshot = oldDestSnap;
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::populateMountedDatasetsTables() {
    auto fill = [this](QTableWidget* table) {
        if (!table) {
            return;
        }
        setTablePopulationMode(table, true);
        table->setRowCount(0);
        QMap<QString, int> mountpointCountByConn;
        struct RowData {
            QString conn;
            QString dataset;
            QString mountpoint;
        };
        QVector<RowData> allRows;
        for (int i = 0; i < m_states.size() && i < m_profiles.size(); ++i) {
            const QString connName = m_profiles[i].name;
            const auto& rows = m_states[i].mountedDatasets;
            for (const auto& pair : rows) {
                allRows.push_back({connName, pair.first, pair.second});
                mountpointCountByConn[connName + QStringLiteral("::") + pair.second] += 1;
            }
        }
        for (const RowData& row : allRows) {
            const int r = table->rowCount();
            table->insertRow(r);
            auto* dsItem = new QTableWidgetItem(QStringLiteral("%1::%2").arg(row.conn, row.dataset));
            auto* mpItem = new QTableWidgetItem(row.mountpoint);
            const bool duplicated = mountpointCountByConn.value(row.conn + QStringLiteral("::") + row.mountpoint, 0) > 1;
            if (duplicated) {
                const QColor redWarn(QStringLiteral("#b22a2a"));
                dsItem->setForeground(QBrush(redWarn));
                mpItem->setForeground(QBrush(redWarn));
            }
            table->setItem(r, 0, dsItem);
            table->setItem(r, 1, mpItem);
        }
        setTablePopulationMode(table, false);
    };
    fill(m_mountedDatasetsTableLeft);
}
