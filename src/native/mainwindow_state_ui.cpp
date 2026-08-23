#include "mainwindow.h"

#include "transferencia.h"

#include <QFontMetrics>
#include "mainwindow_helpers.h"
#include "connectioncapabilities.h"

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
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;

QString datasetLeafNameStateUi(const QString& datasetName) {
    const QString trimmed = datasetName.trimmed();
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return (slash >= 0) ? trimmed.mid(slash + 1) : trimmed;
}
} // namespace

MainWindow::DatasetSelectionContext MainWindow::normalizeDatasetSelectionContext(
    const DatasetSelectionContext& rawCtx,
    const QTreeWidget* treeHint) const {
    DatasetSelectionContext ctx = rawCtx;
    ctx.poolName = ctx.poolName.trimmed();
    ctx.datasetName = ctx.datasetName.trimmed();
    ctx.snapshotName = ctx.snapshotName.trimmed();
    if (!ctx.valid) {
        return ctx;
    }
    if (ctx.datasetName.isEmpty() && treeHint) {
        QTreeWidgetItem* item = treeHint->currentItem();
        if (!item) {
            const QList<QTreeWidgetItem*> selected = treeHint->selectedItems();
            if (!selected.isEmpty()) {
                item = selected.first();
            }
        }
        while (item && item->data(0, Qt::UserRole).toString().trimmed().isEmpty() && item->parent()) {
            item = item->parent();
        }
        if (item) {
            const int itemConnIdx = item->data(0, kConnIdxRole).toInt();
            const QString itemPool = item->data(0, kPoolNameRole).toString().trimmed();
            QString itemDataset = item->data(0, Qt::UserRole).toString().trimmed();
            if (itemDataset.isEmpty() && item->data(0, kIsPoolRootRole).toBool()) {
                itemDataset = itemPool;
            }
            if (itemConnIdx == ctx.connIdx
                && !itemPool.isEmpty()
                && itemPool.compare(ctx.poolName, Qt::CaseInsensitive) == 0
                && !itemDataset.isEmpty()) {
                ctx.datasetName = itemDataset;
                if (ctx.snapshotName.isEmpty()) {
                    ctx.snapshotName = item->data(1, Qt::UserRole).toString().trimmed();
                }
            }
        }
    }
    if (ctx.datasetName.isEmpty()
        && ctx.connIdx >= 0
        && !ctx.poolName.isEmpty()
        && findDsInfo(ctx.connIdx, ctx.poolName, ctx.poolName)) {
        ctx.datasetName = ctx.poolName;
    }
    if (ctx.datasetName.isEmpty()) {
        ctx.snapshotName.clear();
    }
    return ctx;
}

void MainWindow::setConnectionOriginSelection(const DatasetSelectionContext& ctx) {
    const DatasetSelectionContext normalized = normalizeDatasetSelectionContext(
        ctx,
        m_topDatasetTreeWidget ? m_topDatasetTreeWidget->tree() : nullptr);
    if (!normalized.valid || normalized.datasetName.isEmpty()) {
        m_connActionOrigin = DatasetSelectionContext{};
    } else {
        m_connActionOrigin = normalized;
    }
    updateConnectionActionsState();
}

void MainWindow::setConnectionDestinationSelection(const DatasetSelectionContext& ctx) {
    const DatasetSelectionContext normalized = normalizeDatasetSelectionContext(
        ctx,
        m_bottomDatasetTreeWidget ? m_bottomDatasetTreeWidget->tree() : nullptr);
    if (!normalized.valid || normalized.datasetName.isEmpty()) {
        m_connActionDest = DatasetSelectionContext{};
    } else {
        m_connActionDest = normalized;
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

MainWindow::TransferActionAvailability
MainWindow::transferActionAvailabilityFor(const DatasetSelectionContext& src,
                                          const DatasetSelectionContext& dst) {
    TransferActionAvailability out;

    auto deny = [&out](const QString& why) {
        for (TransferActionAvailability::Entry* e :
             {&out.send, &out.clone, &out.move, &out.level, &out.sync, &out.diff}) {
            e->enabled = false;
            e->reason = why;
        }
        return out;
    };

    const bool srcDs = src.valid && !src.datasetName.trimmed().isEmpty();
    const bool dstDs = dst.valid && !dst.datasetName.trimmed().isEmpty();
    if (!srcDs) {
        return deny(trk(QStringLiteral("t_avail_no_origin001"),
                        QStringLiteral("No hay origen marcado."),
                        QStringLiteral("No source marked."),
                        QStringLiteral("尚未标记源。")));
    }
    if (!dstDs) {
        return deny(trk(QStringLiteral("t_avail_no_dest001"),
                        QStringLiteral("No hay destino."),
                        QStringLiteral("No target."),
                        QStringLiteral("没有目标。")));
    }

    const bool srcSnap = !src.snapshotName.trimmed().isEmpty();
    const bool dstSnap = !dst.snapshotName.trimmed().isEmpty();
    const QString srcSel = srcSnap
        ? QStringLiteral("%1@%2").arg(src.datasetName.trimmed(), src.snapshotName.trimmed())
        : src.datasetName.trimmed();
    const QString dstSel = dstSnap
        ? QStringLiteral("%1@%2").arg(dst.datasetName.trimmed(), dst.snapshotName.trimmed())
        : dst.datasetName.trimmed();

    QString versionReason;
    const bool versionOk = isTransferVersionAllowed(src, dst, &versionReason);

    // Enviar y Nivelar encadenan "zfs send | zfs recv" por una tubería, y el agente de
    // Windows todavía no lo implementa. La tabla de capacidades ya lo sabía, pero solo se
    // consultaba AL EJECUTAR (requireNonWindowsStreamingEndpoints): la entrada salía
    // habilitada y el rechazo llegaba después de pulsar. La regla de la casa es la
    // contraria —no intentarlo y decir por qué—, así que se consulta también aquí.
    const bool streamingOk =
        featureAvailable(src.connIdx, zfsmgr::caps::Feature::SendRecvStreaming)
        && featureAvailable(dst.connIdx, zfsmgr::caps::Feature::SendRecvStreaming);

    auto datasetMountedForCtx = [this](const DatasetSelectionContext& c) -> bool {
        if (!c.valid || c.datasetName.trimmed().isEmpty()) {
            return false;
        }
        QString mountedValue;
        if (datasetMountedFromModel(c.connIdx, c.poolName, c.datasetName, &mountedValue)) {
            return mwhelpers::isMountedValueTrue(mountedValue);
        }
        return false;
    };

    const mwhelpers::TransferButtonInputs in{
        srcDs, srcSnap, dstDs, dstSnap,
        srcSel, dstSel, srcDs, dstDs,
        datasetMountedForCtx(src), datasetMountedForCtx(dst),
    };
    const mwhelpers::TransferButtonState st = mwhelpers::computeTransferButtonState(in);

    const bool sameConn = (src.connIdx == dst.connIdx);
    const bool samePool = sameConn
        && !src.poolName.trimmed().isEmpty()
        && (src.poolName.trimmed() == dst.poolName.trimmed());

    auto datasetIsVolume = [this](const DatasetSelectionContext& c) -> bool {
        if (!c.valid || c.connIdx < 0 || c.poolName.trimmed().isEmpty()
            || c.datasetName.trimmed().isEmpty()) {
            return false;
        }
        const DSInfo* info = findDsInfo(c.connIdx, c.poolName, c.datasetName);
        if (!info) {
            return false;
        }
        const QString mounted = info->runtime.properties.value(QStringLiteral("mounted")).trimmed();
        const QString mountpoint = info->runtime.properties.value(QStringLiteral("mountpoint")).trimmed();
        return mounted == QStringLiteral("-") && mountpoint == QStringLiteral("-");
    };

    const QString needsSnapshotSrc = trk(QStringLiteral("t_avail_need_snap_src001"),
        QStringLiteral("El origen tiene que ser un snapshot."),
        QStringLiteral("The source must be a snapshot."),
        QStringLiteral("源必须是快照。"));
    const QString needsDatasetDst = trk(QStringLiteral("t_avail_need_ds_dst001"),
        QStringLiteral("El destino tiene que ser un dataset, no un snapshot."),
        QStringLiteral("The target must be a dataset, not a snapshot."),
        QStringLiteral("目标必须是数据集，不能是快照。"));
    const QString needsSamePool = trk(QStringLiteral("t_avail_same_pool001"),
        QStringLiteral("Origen y destino tienen que estar en el mismo pool."),
        QStringLiteral("Source and target must be in the same pool."),
        QStringLiteral("源和目标必须位于同一存储池。"));

    // Enviar
    out.send.enabled = st.sendEnabled && versionOk && streamingOk;
    if (!out.send.enabled) {
        out.send.reason = !versionOk ? versionReason.trimmed()
                        : !streamingOk
                              ? streamingUnavailableReason(
                                    trk(QStringLiteral("t_avail_lbl_copy001"),
                                        QStringLiteral("Enviar snapshot"),
                                        QStringLiteral("Send snapshot"),
                                        QStringLiteral("发送快照")))
                        : !srcSnap   ? needsSnapshotSrc
                        : dstSnap    ? needsDatasetDst
                                     : trk(QStringLiteral("t_avail_copy_generic001"),
                                           QStringLiteral("Enviar necesita un snapshot en el origen y un dataset en el destino."),
                                           QStringLiteral("Send needs a snapshot as source and a dataset as target."),
                                           QStringLiteral("复制需要以快照为源、以数据集为目标。"));
    }

    // Clonar
    out.clone.enabled = srcSnap && dstDs && !dstSnap && samePool && versionOk;
    if (!out.clone.enabled) {
        out.clone.reason = !versionOk ? versionReason.trimmed()
                         : !srcSnap   ? needsSnapshotSrc
                         : dstSnap    ? needsDatasetDst
                         : !samePool  ? needsSamePool
                                      : QString();
    }

    // Mover
    const bool srcDatasetOnly = srcDs && !srcSnap;
    const bool dstDatasetOnly = dstDs && !dstSnap;
    const QString moveTargetName = (srcDatasetOnly && dstDatasetOnly)
        ? QStringLiteral("%1/%2").arg(dst.datasetName.trimmed(),
                                      datasetLeafNameStateUi(src.datasetName))
        : QString();
    const bool moveIntoSelf = srcDatasetOnly && dstDatasetOnly
        && (dst.datasetName.trimmed() == src.datasetName.trimmed()
            || dst.datasetName.trimmed().startsWith(src.datasetName.trimmed() + QStringLiteral("/")));
    out.move.enabled = srcDatasetOnly && dstDatasetOnly && samePool
        && !datasetIsVolume(dst) && !moveIntoSelf
        && moveTargetName != src.datasetName.trimmed();
    if (!out.move.enabled) {
        out.move.reason = !srcDatasetOnly ? trk(QStringLiteral("t_avail_move_src_ds001"),
                                                QStringLiteral("Mover necesita un dataset como origen, no un snapshot."),
                                                QStringLiteral("Move needs a dataset as source, not a snapshot."),
                                                QStringLiteral("移动需要以数据集为源，而不是快照。"))
                        : !dstDatasetOnly ? needsDatasetDst
                        : !samePool       ? needsSamePool
                        : moveIntoSelf    ? trk(QStringLiteral("t_avail_move_self001"),
                                                QStringLiteral("No se puede mover un dataset dentro de sí mismo."),
                                                QStringLiteral("A dataset cannot be moved inside itself."),
                                                QStringLiteral("数据集不能移动到其自身之下。"))
                                          : QString();
    }

    // Nivelar y Sincronizar
    out.level.enabled = st.levelEnabled && versionOk && streamingOk;
    if (!out.level.enabled) {
        out.level.reason = !versionOk ? versionReason.trimmed()
                        : !streamingOk
                              ? streamingUnavailableReason(
                                    trk(QStringLiteral("t_avail_lbl_level001"),
                                        QStringLiteral("Nivelar snapshot"),
                                        QStringLiteral("Level snapshot"),
                                        QStringLiteral("同步快照")))
                                      : trk(QStringLiteral("t_avail_level_generic001"),
                                            QStringLiteral("Nivelar necesita un snapshot en el origen y su dataset en el destino."),
                                            QStringLiteral("Level needs a snapshot as source and its dataset as target."),
                                            QStringLiteral("同步快照需要以快照为源、以其数据集为目标。"));
    }
    out.sync.enabled = st.syncEnabled && versionOk;
    if (!out.sync.enabled) {
        out.sync.reason = !versionOk ? versionReason.trimmed()
                        : (srcSnap || dstSnap)
                              ? trk(QStringLiteral("t_sync_disable_reason_snapshot_001"),
                                    QStringLiteral("Sync requiere datasets en Origen y Destino (sin snapshot)."),
                                    QStringLiteral("Sync requires datasets in Source and Target (no snapshot selected)."),
                                    QStringLiteral("Sync 要求源和目标都选择数据集（不能选择快照）。"))
                        : (sameConn && samePool
                           && src.datasetName.trimmed() == dst.datasetName.trimmed())
                              ? trk(QStringLiteral("t_sync_disable_reason_same_001"),
                                    QStringLiteral("Sync requiere Origen y Destino diferentes."),
                                    QStringLiteral("Sync requires Source and Target to be different."),
                                    QStringLiteral("Sync 要求源和目标必须不同。"))
                              : trk(QStringLiteral("t_sync_disable_reason_mounted_001"),
                                    QStringLiteral("Sync inmediato requiere ambos datasets montados (si no, se usará fallback según plataforma al ejecutar)."),
                                    QStringLiteral("Immediate Sync requires both datasets mounted (otherwise platform fallback will be used at execution time)."),
                                    QStringLiteral("立即 Sync 要求两个数据集都已挂载（否则执行时会按平台使用回退方案）。"));
    }

    // Diff
    out.diff.enabled = srcSnap && dstDs && samePool
        && src.datasetName.trimmed() == dst.datasetName.trimmed()
        && (!dstSnap || src.snapshotName.trimmed() != dst.snapshotName.trimmed());
    if (!out.diff.enabled) {
        out.diff.reason = !srcSnap ? needsSnapshotSrc
                        : !samePool ? needsSamePool
                        : (src.datasetName.trimmed() != dst.datasetName.trimmed())
                              ? trk(QStringLiteral("t_avail_diff_same_ds001"),
                                    QStringLiteral("Diff compara dos puntos del MISMO dataset."),
                                    QStringLiteral("Diff compares two points of the SAME dataset."),
                                    QStringLiteral("Diff 比较同一数据集的两个时间点。"))
                              : trk(QStringLiteral("t_avail_diff_same_snap001"),
                                    QStringLiteral("Origen y destino son el mismo snapshot."),
                                    QStringLiteral("Source and target are the same snapshot."),
                                    QStringLiteral("源和目标是同一个快照。"));
    }
    return out;
}

void MainWindow::updateConnectionActionsState() {
    if (!(m_topDetailConnIdx >= 0 && m_topDetailConnIdx < m_conns.profiles.size()
          && !isConnectionDisconnected(m_topDetailConnIdx))) {
        m_connActionOrigin = DatasetSelectionContext{};
    }

    // Lo único que queda por pintar es el ORIGEN.
    //
    // Antes esta función habilitaba y etiquetaba seis botones, y para eso repetía las
    // reglas de qué combinación de origen y destino vale para cada acción: 253 líneas.
    // Las seis viven ahora en el menú contextual del destino y las reglas en
    // transferActionAvailabilityFor, así que aquí no queda nada de eso.
    //
    // Y el destino ya no es un estado que recordar: es el nodo sobre el que se pulsa, de
    // modo que mostrarlo aquí solo podía mentir —enseñaría el último usado—.
    auto fmtSel = [this](const DatasetSelectionContext& c) -> QString {
        if (!c.valid || c.datasetName.isEmpty() || c.connIdx < 0 || c.connIdx >= m_conns.profiles.size()) {
            return trk(QStringLiteral("t_empty_sel_001"),
                       QStringLiteral("(vacío)"),
                       QStringLiteral("(empty)"),
                       QStringLiteral("（空）"));
        }
        const QString base = c.snapshotName.isEmpty()
                                 ? c.datasetName
                                 : QStringLiteral("%1@%2").arg(c.datasetName, c.snapshotName);
        return QStringLiteral("%1::%2").arg(m_conns.profiles[c.connIdx].name, base);
    };
    if (m_connOriginSelectionLabel) {
        const QString originText =
            trk(QStringLiteral("t_conn_origin_sel1"),
                QStringLiteral("Origen: %1"),
                QStringLiteral("Source: %1"),
                QStringLiteral("源：%1"))
                .arg(fmtSel(m_connActionOrigin));
        // Acortado a lo que quepa: comparte fila con Estado y Progreso, y un nombre largo
        // no puede robarles sitio ni forzar una segunda línea.
        const QFontMetrics fm(m_connOriginSelectionLabel->font());
        const int avail = qMax(60, m_connOriginSelectionLabel->width() - 4);
        m_connOriginSelectionLabel->setText(fm.elidedText(originText, Qt::ElideMiddle, avail));
        // El tooltip lleva el origen COMPLETO delante de la explicación: la etiqueta se
        // recorta cuando la ruta no cabe, así que este es el único sitio donde se lee
        // entero.
        const QString fullOrigin = fmtSel(m_connActionOrigin);
        m_connOriginSelectionLabel->setToolTip(
            fullOrigin + QStringLiteral("\n\n") +
            trk(QStringLiteral("t_conn_origin_tt001"),
                QStringLiteral("Marque un origen con el botón derecho sobre un dataset o "
                               "snapshot. Después, el botón derecho sobre otro nodo ofrece "
                               "las acciones que lo toman como destino."),
                QStringLiteral("Mark a source by right-clicking a dataset or snapshot. Then "
                               "right-clicking another node offers the actions that use it "
                               "as the target."),
                QStringLiteral("在数据集或快照上点右键以标记源。之后在另一个节点上点右键，"
                               "即可看到以该节点为目标的操作。")));
        m_connOriginSelectionLabel->setStyleSheet(QStringLiteral("QLabel { color: #000000; }"));
    }
}

bool MainWindow::isTransferVersionAllowed(const DatasetSelectionContext& src,
                                          const DatasetSelectionContext& dst,
                                          QString* reasonOut) const {
    // La REGLA —por debajo de 2.3.3 no se transfiere— vive en `base/transferencia`, junto
    // al resto de las decisiones de una transferencia. Aquí queda mirar la versión de cada
    // extremo y componer el aviso, que es lo que sí es de la ventana.
    const auto demasiadoViejo = [this](const DatasetSelectionContext& ctx, QString* connOut,
                                       QString* verOut) {
        if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_conns.profiles.size()
            || ctx.connIdx >= m_conns.states.size()) {
            return false;
        }
        const QString ver = m_conns.states[ctx.connIdx].zfsVersion.trimmed();
        if (zfsmgr::base::transferencia::versionAdmiteTransferencia(ver.toStdString())) {
            return false;
        }
        if (connOut) { *connOut = m_conns.profiles[ctx.connIdx].name; }
        if (verOut) { *verOut = ver; }
        return true;
    };
    if (!src.valid || !dst.valid) {
        return true;
    }
    QString badConn;
    QString badVer;
    const bool srcTooOld = demasiadoViejo(src, &badConn, &badVer);
    const bool dstTooOld = demasiadoViejo(dst, &badConn, &badVer);
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
        if (!runDatasetRenameNow(PendingDatasetRenameDraft{src.connIdx, src.poolName, src.datasetName, targetName}, &errorText)) {
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
                   .arg(m_conns.profiles.at(src.connIdx).name,
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

    if (action == QStringLiteral("send")) {
        actionSendSnapshot();
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
