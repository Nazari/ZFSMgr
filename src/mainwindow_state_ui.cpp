#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <functional>

namespace {
using mwhelpers::isMountedValueTrue;

QString datasetLeafNameStateUi(const QString& datasetName) {
    const QString trimmed = datasetName.trimmed();
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return (slash >= 0) ? trimmed.mid(slash + 1) : trimmed;
}
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

bool MainWindow::connAdvancedDatasetActionAllowed(const DatasetSelectionContext& ctx) const {
    if (!ctx.valid || ctx.datasetName.isEmpty() || !ctx.snapshotName.isEmpty()) {
        return false;
    }
    if (supportsAlternateDatasetMount(ctx.connIdx)) {
        return true;
    }
    const PoolInfo* poolInfo = findPoolInfo(ctx.connIdx, ctx.poolName);
    if (!poolInfo) {
        return false;
    }
    const QString base = ctx.datasetName;
    const QString pref = base + QStringLiteral("/");
    for (auto it = poolInfo->objectsByFullName.constBegin(); it != poolInfo->objectsByFullName.constEnd(); ++it) {
        const QString& ds = it.key();
        if (ds != base && !ds.startsWith(pref)) {
            continue;
        }
        if (it->kind == DSKind::Snapshot) {
            continue;
        }
        if (!isMountedValueTrue(it->runtime.properties.value(QStringLiteral("mounted")))) {
            return false;
        }
    }
    return true;
}

bool MainWindow::connDirectoryDatasetActionAllowed(const DatasetSelectionContext& ctx) const {
    return ctx.valid && !ctx.datasetName.isEmpty() && ctx.snapshotName.isEmpty();
}

void MainWindow::updateConnectionActionsState() {
    if (!(m_topDetailConnIdx >= 0 && m_topDetailConnIdx < m_profiles.size()
          && !isConnectionDisconnected(m_topDetailConnIdx))) {
        m_connActionOrigin = DatasetSelectionContext{};
    }

    if (m_btnConnCopy) m_btnConnCopy->setText(
        trk(QStringLiteral("t_copy_001"), QStringLiteral("Copiar"), QStringLiteral("Copy"), QStringLiteral("复制")));
    if (m_btnConnClone) m_btnConnClone->setText(
        trk(QStringLiteral("t_clone_btn_001"), QStringLiteral("Clonar"), QStringLiteral("Clone"), QStringLiteral("克隆")));
    if (m_btnConnMove) m_btnConnMove->setText(
        trk(QStringLiteral("t_move_btn_001"), QStringLiteral("Mover"), QStringLiteral("Move"), QStringLiteral("移动")));
    if (m_btnConnDiff) m_btnConnDiff->setText(
        trk(QStringLiteral("t_diff_btn_001"), QStringLiteral("Diff")));
    if (m_btnConnLevel) m_btnConnLevel->setText(
        trk(QStringLiteral("t_level_btn_001"), QStringLiteral("Nivelar"), QStringLiteral("Level"), QStringLiteral("同步快照")));
    if (m_btnConnSync) m_btnConnSync->setText(
        trk(QStringLiteral("t_sync_btn_001"), QStringLiteral("Sincronizar"), QStringLiteral("Sync"), QStringLiteral("同步文件")));

    const DatasetSelectionContext dctx = currentConnContentSelection(m_connContentTree);
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
    const QString dstText = trk(QStringLiteral("t_conn_dest_sel01"),
                                QStringLiteral("Destino:%1"),
                                QStringLiteral("Target:%1"),
                                QStringLiteral("目标：%1")).arg(fmtSel(m_connActionDest));
    if (m_connDestSelectionLabel) {
        m_connDestSelectionLabel->setText(dstText);
    }
    const QString originText =
        trk(QStringLiteral("t_conn_origin_sel1"),
            QStringLiteral("Origen:%1"),
            QStringLiteral("Source:%1"),
            QStringLiteral("源：%1"))
            .arg(fmtSel(m_connActionOrigin));
    if (m_connOriginSelectionLabel) {
        m_connOriginSelectionLabel->setText(originText);
    }
    QString versionTransferReason;
    const bool transferVersionAllowed = isTransferVersionAllowed(m_connActionOrigin, m_connActionDest, &versionTransferReason);
    const bool paintActionLabelsRed = (!transferVersionAllowed
                                       && m_connActionOrigin.valid
                                       && m_connActionDest.valid);
    const QString actionLabelsStyle = paintActionLabelsRed
        ? QStringLiteral("QLabel { color: #b00020; }")
        : QStringLiteral("QLabel { color: #000000; }");
    if (m_connOriginSelectionLabel) {
        m_connOriginSelectionLabel->setStyleSheet(actionLabelsStyle);
    }
    if (m_connDestSelectionLabel) {
        m_connDestSelectionLabel->setStyleSheet(actionLabelsStyle);
    }

    if (actionsLocked()) {
        if (m_btnConnCopy) m_btnConnCopy->setEnabled(false);
        if (m_btnConnClone) m_btnConnClone->setEnabled(false);
        if (m_btnConnDiff) m_btnConnDiff->setEnabled(false);
        if (m_btnConnLevel) m_btnConnLevel->setEnabled(false);
        if (m_btnConnSync) m_btnConnSync->setEnabled(false);
        return;
    }

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
        QString mountedValue;
        if (datasetMountedFromModel(c.connIdx, c.poolName, c.datasetName, &mountedValue)) {
            return isMountedValueTrue(mountedValue);
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
        datasetMountedForCtx(m_connActionDest, m_connContentTree),
    };
    const mwhelpers::TransferButtonState st = mwhelpers::computeTransferButtonState(transferIn);
    if (m_btnConnCopy) m_btnConnCopy->setEnabled(!actionsLocked() && st.copyEnabled && transferVersionAllowed);
    const bool sameConnForClone = m_connActionOrigin.valid
        && m_connActionDest.valid
        && (m_connActionOrigin.connIdx == m_connActionDest.connIdx);
    const bool samePoolForClone = sameConnForClone
        && !m_connActionOrigin.poolName.trimmed().isEmpty()
        && (m_connActionOrigin.poolName.trimmed() == m_connActionDest.poolName.trimmed());
    const bool cloneEnabled = srcSnap && dstDs && !dstSnap && samePoolForClone;
    auto datasetIsVolume = [this](const DatasetSelectionContext& ctx) -> bool {
        if (!ctx.valid || ctx.connIdx < 0 || ctx.poolName.trimmed().isEmpty() || ctx.datasetName.trimmed().isEmpty()) {
            return false;
        }
        const DSInfo* dsInfo = findDsInfo(ctx.connIdx, ctx.poolName, ctx.datasetName);
        if (!dsInfo) {
            return false;
        }
        const QString mounted = dsInfo->runtime.properties.value(QStringLiteral("mounted")).trimmed();
        const QString mountpoint = dsInfo->runtime.properties.value(QStringLiteral("mountpoint")).trimmed();
        return mounted == QStringLiteral("-") && mountpoint == QStringLiteral("-");
    };
    const bool sourceDatasetOnly = srcDs && !srcSnap;
    const bool destDatasetOnly = dstDs && !dstSnap;
    const QString moveTargetName =
        (sourceDatasetOnly && destDatasetOnly)
            ? QStringLiteral("%1/%2").arg(m_connActionDest.datasetName.trimmed(),
                                          datasetLeafNameStateUi(m_connActionOrigin.datasetName))
            : QString();
    const bool moveIntoSelfOrDescendant =
        sourceDatasetOnly
        && destDatasetOnly
        && (m_connActionDest.datasetName.trimmed() == m_connActionOrigin.datasetName.trimmed()
            || m_connActionDest.datasetName.trimmed().startsWith(
                   m_connActionOrigin.datasetName.trimmed() + QStringLiteral("/")));
    const bool moveEnabled = sourceDatasetOnly
        && destDatasetOnly
        && samePoolForClone
        && !datasetIsVolume(m_connActionDest)
        && !moveIntoSelfOrDescendant
        && moveTargetName != m_connActionOrigin.datasetName.trimmed();
    const bool diffEnabled = srcSnap
        && dstDs
        && samePoolForClone
        && m_connActionOrigin.datasetName.trimmed() == m_connActionDest.datasetName.trimmed()
        && (!dstSnap || m_connActionOrigin.snapshotName.trimmed() != m_connActionDest.snapshotName.trimmed());
    if (m_btnConnClone) m_btnConnClone->setEnabled(!actionsLocked() && cloneEnabled && transferVersionAllowed);
    if (m_btnConnMove) m_btnConnMove->setEnabled(!actionsLocked() && moveEnabled);
    if (m_btnConnDiff) m_btnConnDiff->setEnabled(!actionsLocked() && diffEnabled);
    if (m_btnConnLevel) m_btnConnLevel->setEnabled(!actionsLocked() && st.levelEnabled && transferVersionAllowed);
    if (m_btnConnSync) m_btnConnSync->setEnabled(!actionsLocked() && st.syncEnabled && transferVersionAllowed);

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

bool MainWindow::isTransferVersionAllowed(const DatasetSelectionContext& src,
                                          const DatasetSelectionContext& dst,
                                          QString* reasonOut) const {
    auto parseVer = [](const QString& raw, int& a, int& b, int& c) -> bool {
        const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
        const QRegularExpressionMatch m = rx.match(raw.trimmed());
        if (!m.hasMatch()) {
            return false;
        }
        a = m.captured(1).toInt();
        b = m.captured(2).toInt();
        c = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
        return true;
    };
    auto isTooOld = [&](const DatasetSelectionContext& ctx, QString* connNameOut, QString* verOut) -> bool {
        if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size() || ctx.connIdx >= m_states.size()) {
            return false;
        }
        const QString ver = m_states[ctx.connIdx].zfsVersion.trimmed();
        if (ver.isEmpty()) {
            return false;
        }
        int ma = 0, mi = 0, pa = 0;
        if (!parseVer(ver, ma, mi, pa)) {
            return false;
        }
        if (connNameOut) {
            *connNameOut = m_profiles[ctx.connIdx].name;
        }
        if (verOut) {
            *verOut = ver;
        }
        if (ma != 2) return false;
        if (mi < 3) return true;
        if (mi > 3) return false;
        return pa < 3;
    };
    if (!src.valid || !dst.valid) {
        return true;
    }
    QString badConn;
    QString badVer;
    const bool srcTooOld = isTooOld(src, &badConn, &badVer);
    const bool dstTooOld = isTooOld(dst, &badConn, &badVer);
    if (!srcTooOld && !dstTooOld) {
        return true;
    }
    if (reasonOut) {
        *reasonOut = trk(QStringLiteral("t_zfs_ver_blk_001"),
                         QStringLiteral("Operación no permitida: la conexión %1 usa OpenZFS %2 (< 2.3.3).")
                             .arg(badConn, badVer),
                         QStringLiteral("Operation not allowed: connection %1 uses OpenZFS %2 (< 2.3.3).")
                             .arg(badConn, badVer),
                         QStringLiteral("不允许的操作：连接 %1 使用 OpenZFS %2（低于 2.3.3）。")
                             .arg(badConn, badVer));
    }
    return false;
}

void MainWindow::executeConnectionTransferAction(const QString& action) {
    const DatasetSelectionContext src = m_connActionOrigin;
    const DatasetSelectionContext dst = m_connActionDest;
    if (!src.valid || src.datasetName.isEmpty() || !dst.valid || dst.datasetName.isEmpty()) {
        return;
    }
    if (action == QStringLiteral("move")) {
        if (!src.snapshotName.isEmpty() || !dst.snapshotName.isEmpty()
            || src.connIdx != dst.connIdx
            || src.poolName.trimmed() != dst.poolName.trimmed()) {
            return;
        }
        const QString targetName = QStringLiteral("%1/%2")
                                       .arg(dst.datasetName.trimmed(),
                                            datasetLeafNameStateUi(src.datasetName));
        QString errorText;
        if (!queuePendingDatasetRename(PendingDatasetRenameDraft{src.connIdx, src.poolName, src.datasetName, targetName}, &errorText)) {
            QMessageBox::warning(this,
                                 QStringLiteral("ZFSMgr"),
                                 errorText.isEmpty()
                                     ? trk(QStringLiteral("t_pending_move_failed_001"),
                                           QStringLiteral("No se pudo añadir el movimiento pendiente."),
                                           QStringLiteral("Could not queue the pending move."),
                                           QStringLiteral("无法加入待处理移动。"))
                                     : errorText);
            return;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Cambio pendiente añadido: %1::%2  %3")
                   .arg(m_profiles.at(src.connIdx).name,
                        src.poolName.trimmed(),
                        pendingDatasetRenameCommand(PendingDatasetRenameDraft{src.connIdx, src.poolName, src.datasetName, targetName})));
        updateApplyPropsButtonState();
        return;
    }
    if (action != QStringLiteral("diff")) {
        QString transferVersionReason;
        if (!isTransferVersionAllowed(src, dst, &transferVersionReason)) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), transferVersionReason);
            appLog(QStringLiteral("WARN"), transferVersionReason);
            return;
        }
    }
    // Fuerza el uso de la selección real Origen/Destino de Conexiones
    // (árbol superior/inferior), evitando depender de combos legacy ocultos.
    m_transferSelectionOverrideActive = true;
    m_transferSelectionOverrideOrigin = src;
    m_transferSelectionOverrideDest = dst;

    if (action == QStringLiteral("copy")) {
        actionCopySnapshot();
    } else if (action == QStringLiteral("clone")) {
        actionCloneSnapshot();
    } else if (action == QStringLiteral("diff")) {
        actionDiffSnapshot();
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
