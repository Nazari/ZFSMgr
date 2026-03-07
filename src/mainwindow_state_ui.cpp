#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QSignalBlocker>
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
        if (m_btnAdvancedBreakdown) m_btnAdvancedBreakdown->setEnabled(false);
        if (m_btnAdvancedAssemble) m_btnAdvancedAssemble->setEnabled(false);
        if (m_btnAdvancedFromDir) m_btnAdvancedFromDir->setEnabled(false);
        if (m_btnAdvancedToDir) m_btnAdvancedToDir->setEnabled(false);
        if (m_btnConnBreakdown) m_btnConnBreakdown->setEnabled(false);
        if (m_btnConnAssemble) m_btnConnAssemble->setEnabled(false);
        if (m_btnConnFromDir) m_btnConnFromDir->setEnabled(false);
        if (m_btnConnToDir) m_btnConnToDir->setEnabled(false);
        if (m_btnConnReset) m_btnConnReset->setEnabled(false);
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
    const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("advanced"));
    bool advDatasetOnly = actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty();
    if (advDatasetOnly) {
        const QString key = datasetCacheKey(actx.connIdx, actx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
            advDatasetOnly = false;
        } else {
            bool allMounted = true;
            const QString base = actx.datasetName;
            const QString pref = base + QStringLiteral("/");
            for (auto it = cacheIt->recordByName.constBegin(); it != cacheIt->recordByName.constEnd(); ++it) {
                const QString& ds = it.key();
                if (ds != base && !ds.startsWith(pref)) {
                    continue;
                }
                if (!isMountedValueTrue(it.value().mounted)) {
                    allMounted = false;
                    break;
                }
            }
            advDatasetOnly = allMounted;
        }
    }
    if (m_btnAdvancedBreakdown) {
        m_btnAdvancedBreakdown->setEnabled(advDatasetOnly);
    }
    if (m_btnAdvancedAssemble) {
        m_btnAdvancedAssemble->setEnabled(advDatasetOnly);
    }
    if (m_btnAdvancedFromDir) {
        m_btnAdvancedFromDir->setEnabled(actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty());
    }
    if (m_btnAdvancedToDir) {
        m_btnAdvancedToDir->setEnabled(actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty());
    }
    updateConnectionActionsState();
}

QString MainWindow::connectionOriginSelectionText() const {
    if (!m_connActionOrigin.valid || m_connActionOrigin.datasetName.isEmpty()) {
        return trk(QStringLiteral("t_empty_sel_001"),
                   QStringLiteral("(vacío)"),
                   QStringLiteral("(empty)"),
                   QStringLiteral("（空）"));
    }
    if (m_connActionOrigin.snapshotName.isEmpty()) {
        return m_connActionOrigin.datasetName;
    }
    return QStringLiteral("%1@%2").arg(m_connActionOrigin.datasetName, m_connActionOrigin.snapshotName);
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
    if (m_btnConnReset) m_btnConnReset->setText(
        trk(QStringLiteral("t_reset_btn_001"), QStringLiteral("Reset"), QStringLiteral("Reset"), QStringLiteral("重置")));

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
    QString dstText = fmtSel(m_connActionDest);
    if (m_connActionOrigin.valid && !m_connActionOrigin.datasetName.isEmpty()) {
        dstText = trk(QStringLiteral("t_conn_dest_sel01"),
                      QStringLiteral("Destino:%1"),
                      QStringLiteral("Target:%1"),
                      QStringLiteral("目标：%1")).arg(dstText);
    }
    if (m_connLeftSelectionLabel) {
        m_connLeftSelectionLabel->setText(dstText);
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
        if (m_btnConnReset) m_btnConnReset->setEnabled(false);
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
    const bool hasConnOrigin = m_connActionOrigin.valid && !m_connActionOrigin.datasetName.isEmpty();
    const bool hasConnDest = m_connActionDest.valid && !m_connActionDest.datasetName.isEmpty();
    if (m_btnConnReset) m_btnConnReset->setEnabled(!actionsLocked() && (hasConnOrigin || hasConnDest));

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
}

void MainWindow::executeConnectionAdvancedAction(const QString& action) {
    const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        return;
    }
    const QString prevToken = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const auto prevSel = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
    const QString prevDs = prevSel.isEmpty() ? QString() : prevSel.first()->data(0, Qt::UserRole).toString();
    const QString prevSnap = prevSel.isEmpty() ? QString() : prevSel.first()->data(1, Qt::UserRole).toString();

    const QString targetToken = QStringLiteral("%1::%2").arg(ctx.connIdx).arg(ctx.poolName);
    bool switched = false;
    if (m_advPoolCombo) {
        const QSignalBlocker blocker(m_advPoolCombo);
        const int idx = m_advPoolCombo->findData(targetToken);
        if (idx >= 0) {
            m_advPoolCombo->setCurrentIndex(idx);
            switched = true;
        }
    }
    if (!switched) {
        return;
    }
    onAdvancedPoolChanged();
    auto findTreeItem = [](QTreeWidget* tree, const QString& ds, const QString& snap) -> QTreeWidgetItem* {
        if (!tree) return nullptr;
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
            if (!n) return nullptr;
            if (n->data(0, Qt::UserRole).toString() == ds
                && (snap.isEmpty() || n->data(1, Qt::UserRole).toString() == snap)) {
                return n;
            }
            for (int i = 0; i < n->childCount(); ++i) {
                if (QTreeWidgetItem* f = rec(n->child(i))) return f;
            }
            return nullptr;
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (QTreeWidgetItem* f = rec(tree->topLevelItem(i))) return f;
        }
        return nullptr;
    };
    if (m_advTree) {
        if (QTreeWidgetItem* target = findTreeItem(m_advTree, ctx.datasetName, ctx.snapshotName)) {
            for (QTreeWidgetItem* p = target->parent(); p; p = p->parent()) {
                m_advTree->expandItem(p);
            }
            m_advTree->setCurrentItem(target);
        }
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

    if (m_advPoolCombo) {
        const QSignalBlocker blocker(m_advPoolCombo);
        const int idx = m_advPoolCombo->findData(prevToken);
        if (idx >= 0) {
            m_advPoolCombo->setCurrentIndex(idx);
        }
    }
    onAdvancedPoolChanged();
    if (m_advTree && !prevDs.isEmpty()) {
        if (QTreeWidgetItem* restored = findTreeItem(m_advTree, prevDs, prevSnap)) {
            m_advTree->setCurrentItem(restored);
        }
    }
}

void MainWindow::executeConnectionTransferAction(const QString& action) {
    const DatasetSelectionContext src = m_connActionOrigin;
    const DatasetSelectionContext dst = m_connActionDest;
    if (!src.valid || src.datasetName.isEmpty() || !dst.valid || dst.datasetName.isEmpty()) {
        return;
    }
    const QString oldOriginToken = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
    const QString oldDestToken = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();
    const QString oldOriginDs = m_originSelectedDataset;
    const QString oldOriginSnap = m_originSelectedSnapshot;
    const QString oldDestDs = m_destSelectedDataset;
    const QString oldDestSnap = m_destSelectedSnapshot;

    const QString srcToken = QStringLiteral("%1::%2").arg(src.connIdx).arg(src.poolName);
    const QString dstToken = QStringLiteral("%1::%2").arg(dst.connIdx).arg(dst.poolName);
    bool srcOk = false;
    bool dstOk = false;
    if (m_originPoolCombo) {
        const QSignalBlocker blocker(m_originPoolCombo);
        const int idx = m_originPoolCombo->findData(srcToken);
        if (idx >= 0) {
            m_originPoolCombo->setCurrentIndex(idx);
            srcOk = true;
        }
    }
    if (m_destPoolCombo) {
        const QSignalBlocker blocker(m_destPoolCombo);
        const int idx = m_destPoolCombo->findData(dstToken);
        if (idx >= 0) {
            m_destPoolCombo->setCurrentIndex(idx);
            dstOk = true;
        }
    }
    if (!srcOk || !dstOk) {
        if (m_originPoolCombo) {
            const QSignalBlocker blocker(m_originPoolCombo);
            const int idx = m_originPoolCombo->findData(oldOriginToken);
            if (idx >= 0) m_originPoolCombo->setCurrentIndex(idx);
        }
        if (m_destPoolCombo) {
            const QSignalBlocker blocker(m_destPoolCombo);
            const int idx = m_destPoolCombo->findData(oldDestToken);
            if (idx >= 0) m_destPoolCombo->setCurrentIndex(idx);
        }
        return;
    }
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

    if (m_originPoolCombo) {
        const QSignalBlocker blocker(m_originPoolCombo);
        const int idx = m_originPoolCombo->findData(oldOriginToken);
        if (idx >= 0) m_originPoolCombo->setCurrentIndex(idx);
    }
    if (m_destPoolCombo) {
        const QSignalBlocker blocker(m_destPoolCombo);
        const int idx = m_destPoolCombo->findData(oldDestToken);
        if (idx >= 0) m_destPoolCombo->setCurrentIndex(idx);
    }
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
    fill(m_mountedDatasetsTableAdv);
}
