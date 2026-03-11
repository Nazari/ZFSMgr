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
#include <functional>

namespace {
using mwhelpers::isMountedValueTrue;
} // namespace

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
    auto datasetMountedForCtx = [this](const DatasetSelectionContext& c, QTreeWidget* treeHint) -> bool {
        if (!c.valid || c.datasetName.isEmpty()) {
            return false;
        }
        const QString key = datasetCacheKey(c.connIdx, c.poolName);
        const auto it = m_poolDatasetCache.constFind(key);
        if (it != m_poolDatasetCache.constEnd() && it->loaded) {
            const auto recIt = it->recordByName.constFind(c.datasetName);
            if (recIt != it->recordByName.constEnd()) {
                return isMountedValueTrue(recIt->mounted);
            }
        }
        if (!treeHint) {
            return false;
        }
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> recFind = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
            if (!n) {
                return nullptr;
            }
            if (n->data(0, Qt::UserRole).toString().trimmed() == c.datasetName) {
                return n;
            }
            for (int i = 0; i < n->childCount(); ++i) {
                if (QTreeWidgetItem* f = recFind(n->child(i))) {
                    return f;
                }
            }
            return nullptr;
        };
        QTreeWidgetItem* dsItem = nullptr;
        for (int i = 0; i < treeHint->topLevelItemCount() && !dsItem; ++i) {
            dsItem = recFind(treeHint->topLevelItem(i));
        }
        if (!dsItem) {
            return false;
        }
        const QString mountedText = dsItem->text(2).trimmed().toLower();
        if (mountedText == QStringLiteral("montado")) {
            return true;
        }
        if (mountedText == QStringLiteral("desmontado")) {
            return false;
        }
        return isMountedValueTrue(mountedText);
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
        datasetMountedForCtx(m_connActionOrigin, m_connContentTree),
        datasetMountedForCtx(m_connActionDest, m_bottomConnContentTree),
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
    updateConnectionActionsState();
}
