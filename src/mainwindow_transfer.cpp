#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QMessageBox>
#include <QRegularExpression>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::shSingleQuote;
using mwhelpers::buildSshTargetPrefix;
using mwhelpers::buildPipedTransferCommand;
using mwhelpers::buildSimpleSshInvocation;
using mwhelpers::buildTarDestinationCommand;
using mwhelpers::buildTarSourceCommand;
using mwhelpers::chooseStreamCodec;
using mwhelpers::streamCodecName;
using mwhelpers::sshBaseCommand;
using mwhelpers::sshUserHost;
} // namespace

void MainWindow::actionCopySnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    ConnectionProfile sp = m_profiles[src.connIdx];
    ConnectionProfile dp = m_profiles[dst.connIdx];
    if (isLocalConnection(sp) && !isWindowsConnection(sp)) {
        sp.useSudo = true;
        if (!ensureLocalSudoCredentials(sp)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Copiar cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    if (isLocalConnection(dp) && !isWindowsConnection(dp)) {
        dp.useSudo = true;
        if (!ensureLocalSudoCredentials(dp)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Copiar cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    const bool sameConnection = (src.connIdx == dst.connIdx);
    const QString srcSnap = src.datasetName + QStringLiteral("@") + src.snapshotName;
    const QString srcLeaf = src.datasetName.section('/', -1);
    QString recvTarget = dst.datasetName;
    if (!srcLeaf.isEmpty()) {
        const QString dstLeaf = dst.datasetName.section('/', -1);
        if (!dst.datasetName.endsWith(QStringLiteral("/") + srcLeaf) && dstLeaf != srcLeaf) {
            recvTarget = dst.datasetName + QStringLiteral("/") + srcLeaf;
        }
    }

    const QString sendRawCmd = QStringLiteral("zfs send -wLecR %1").arg(shSingleQuote(srcSnap));
    const QString recvRawCmd = QStringLiteral("zfs recv -Fus %1").arg(shSingleQuote(recvTarget));
    QString sendCmd = withSudo(sp, sendRawCmd);
    QString recvCmd = withSudoStreamInput(dp, recvRawCmd);

    QString pipeline;
    if (sameConnection) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_copiar_mod_b1d73e"),
                   QStringLiteral("Copiar: modo local remoto (origen y destino en la misma conexión)"),
                   QStringLiteral("Copy: remote-local mode (source and target on same connection)"),
                   QStringLiteral("复制：远端本地模式（源和目标在同一连接）")));
        const QString remotePipe = withSudo(sp, buildPipedTransferCommand(sendRawCmd, recvRawCmd));
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else {
        pipeline = buildPipedTransferCommand(buildSimpleSshInvocation(sp, sendCmd),
                                             buildSimpleSshInvocation(dp, recvCmd));
    }

    if (runLocalCommand(QStringLiteral("Copiar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0, false, true)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        refreshConnectionByIndex(dst.connIdx);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionLevelSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty() || !dst.snapshotName.isEmpty()) {
        appLog(QStringLiteral("INFO"), trk(QStringLiteral("t_nivelar_om_fd38a5"),
                                           QStringLiteral("Nivelar omitido: selección incompleta (src.valid=%1 dst.valid=%2 src.dataset=%3 dst.dataset=%4 dst.snap=%5)"),
                                           QStringLiteral("Level skipped: incomplete selection (src.valid=%1 dst.valid=%2 src.dataset=%3 dst.dataset=%4 dst.snap=%5)"),
                                           QStringLiteral("同步快照已跳过：选择不完整（src.valid=%1 dst.valid=%2 src.dataset=%3 dst.dataset=%4 dst.snap=%5）"))
                                      .arg(src.valid ? QStringLiteral("1") : QStringLiteral("0"))
                                      .arg(dst.valid ? QStringLiteral("1") : QStringLiteral("0"))
                                      .arg(src.datasetName.isEmpty() ? QStringLiteral("0") : QStringLiteral("1"))
                                      .arg(dst.datasetName.isEmpty() ? QStringLiteral("0") : QStringLiteral("1"))
                                      .arg(dst.snapshotName.isEmpty() ? QStringLiteral("0") : QStringLiteral("1")));
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_level_need_sel1"),
                QStringLiteral("Para Nivelar debe seleccionar:\n"
                               "- En Origen: un dataset o snapshot\n"
                               "- En Destino: un dataset (sin snapshot)"),
                QStringLiteral("To Level you must select:\n"
                               "- Source: a dataset or snapshot\n"
                               "- Target: a dataset (without snapshot)"),
                QStringLiteral("执行“同步快照”前请先选择：\n"
                               "- 源：数据集或快照\n"
                               "- 目标：数据集（不带快照）")));
        return;
    }
    if (!ensureDatasetsLoaded(src.connIdx, src.poolName) || !ensureDatasetsLoaded(dst.connIdx, dst.poolName)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_load_sn01"),
                                 QStringLiteral("No se pudieron cargar snapshots para Nivelar."),
                                 QStringLiteral("Could not load snapshots for Level."),
                                 QStringLiteral("无法加载用于同步快照的快照列表。")));
        return;
    }
    const QString srcKey = datasetCacheKey(src.connIdx, src.poolName);
    const QString dstKey = datasetCacheKey(dst.connIdx, dst.poolName);
    const QStringList srcSnaps = m_poolDatasetCache.value(srcKey).snapshotsByDataset.value(src.datasetName);
    const QStringList dstSnaps = m_poolDatasetCache.value(dstKey).snapshotsByDataset.value(dst.datasetName);
    if (srcSnaps.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_level_no_src01"),
                                     QStringLiteral("Origen no tiene snapshots para Nivelar."),
                                     QStringLiteral("Source has no snapshots to Level."),
                                     QStringLiteral("源数据集没有可用于同步的快照。")));
        return;
    }
    if (dstSnaps.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_no_dst01"),
                                 QStringLiteral("Destino no tiene snapshots. Nivelar siempre se hace con diferencial desde el snapshot más reciente de destino."),
                                 QStringLiteral("Target has no snapshots. Level always uses a differential send from target latest snapshot."),
                                 QStringLiteral("目标数据集没有快照。“同步快照”始终要求从目标最新快照开始做增量发送。")));
        return;
    }

    const QString targetSnapName = src.snapshotName.isEmpty() ? srcSnaps.first() : src.snapshotName;
    const int targetIdxInSrc = srcSnaps.indexOf(targetSnapName);
    if (targetIdxInSrc < 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_tgt_abs01"),
                                 QStringLiteral("El snapshot objetivo (%1) no existe en origen.").arg(targetSnapName),
                                 QStringLiteral("Target snapshot (%1) does not exist in source.").arg(targetSnapName),
                                 QStringLiteral("目标快照（%1）在源端不存在。").arg(targetSnapName)));
        return;
    }

    const QString dstLatestSnap = dstSnaps.first();
    const int baseIdxInSrc = srcSnaps.indexOf(dstLatestSnap);
    if (baseIdxInSrc < 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_dst_abs01"),
                                 QStringLiteral("El snapshot más reciente de destino (%1) no existe en origen.\nNo se puede nivelar con diferencial.")
                                     .arg(dstLatestSnap),
                                 QStringLiteral("Target latest snapshot (%1) does not exist in source.\nCannot level with differential send.")
                                     .arg(dstLatestSnap),
                                 QStringLiteral("目标最新快照（%1）在源端不存在，无法执行增量同步。")
                                     .arg(dstLatestSnap)));
        return;
    }
    if (baseIdxInSrc < targetIdxInSrc) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_dst_new01"),
                                 QStringLiteral("Destino tiene un snapshot más moderno (%1) que el snapshot objetivo (%2).\nNivelar cancelado.")
                                     .arg(dstLatestSnap, targetSnapName),
                                 QStringLiteral("Target has a newer snapshot (%1) than target snapshot to send (%2).\nLevel canceled.")
                                     .arg(dstLatestSnap, targetSnapName),
                                 QStringLiteral("目标端存在比要发送目标快照（%2）更“新”的快照（%1），已取消同步。")
                                     .arg(dstLatestSnap, targetSnapName)));
        return;
    }
    if (baseIdxInSrc == targetIdxInSrc) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_level_already01"),
                                     QStringLiteral("Destino ya está nivelado en el snapshot objetivo (%1).")
                                         .arg(targetSnapName),
                                     QStringLiteral("Target is already leveled at target snapshot (%1).")
                                         .arg(targetSnapName),
                                     QStringLiteral("目标已处于目标快照（%1），无需同步。")
                                         .arg(targetSnapName)));
        return;
    }

    ConnectionProfile sp = m_profiles[src.connIdx];
    ConnectionProfile dp = m_profiles[dst.connIdx];
    if (isLocalConnection(sp) && !isWindowsConnection(sp)) {
        sp.useSudo = true;
        if (!ensureLocalSudoCredentials(sp)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Nivelar cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    if (isLocalConnection(dp) && !isWindowsConnection(dp)) {
        dp.useSudo = true;
        if (!ensureLocalSudoCredentials(dp)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Nivelar cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    const bool sameConnection = (src.connIdx == dst.connIdx);
    const QString fromSnap = src.datasetName + QStringLiteral("@") + dstLatestSnap;
    const QString srcSnap = src.datasetName + QStringLiteral("@") + targetSnapName;
    const QString sendRawCmd = QStringLiteral("zfs send -wLecR -I %1 %2").arg(shSingleQuote(fromSnap), shSingleQuote(srcSnap));
    QString sendCmd = withSudo(sp, sendRawCmd);
    const QString recvTarget = dst.datasetName;

    const QString recvRawCmd = QStringLiteral("zfs recv -Fus %1").arg(shSingleQuote(recvTarget));
    QString recvCmd = withSudoStreamInput(dp, recvRawCmd);

    QString pipeline;
    if (sameConnection) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_nivelar_mo_2edd21"),
                   QStringLiteral("Nivelar: modo local remoto (origen y destino en la misma conexión)"),
                   QStringLiteral("Level: remote-local mode (source and target on same connection)"),
                   QStringLiteral("同步快照：远端本地模式（源和目标在同一连接）")));
        const QString remotePipe = withSudo(sp, buildPipedTransferCommand(sendRawCmd, recvRawCmd));
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else {
        pipeline = buildPipedTransferCommand(buildSimpleSshInvocation(sp, sendCmd),
                                             buildSimpleSshInvocation(dp, recvCmd));
    }

    if (runLocalCommand(QStringLiteral("Nivelar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0, false, true)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        refreshConnectionByIndex(dst.connIdx);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionSyncDatasets() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const bool sameConnection = (src.connIdx == dst.connIdx);

    QString srcMp;
    QString dstMp;
    QString srcMounted;
    QString dstMounted;
    QString srcCanmount;
    QString dstCanmount;
    if (!getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mountpoint"), srcMp)
        || !getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mounted"), srcMounted)
        || !getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("canmount"), srcCanmount)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mountpoint"), dstMp)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mounted"), dstMounted)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("canmount"), dstCanmount)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_no_se_pudi_4f5238"),
                                 QStringLiteral("No se pudieron leer mountpoints para sincronizar."),
                                 QStringLiteral("Could not read mountpoints for synchronization."),
                                 QStringLiteral("无法读取用于同步的挂载点。")));
        return;
    }
    const bool srcRootMounted = isMountedValueTrue(srcMounted);
    const bool dstRootMounted = isMountedValueTrue(dstMounted);
    const bool srcCanmountOff = (srcCanmount.trimmed().toLower() == QStringLiteral("off"));
    const bool dstCanmountOff = (dstCanmount.trimmed().toLower() == QStringLiteral("off"));

    const QString srcSsh = buildSshTargetPrefix(sp);

    const QString rsyncOptsProbe = QStringLiteral(
        "RSYNC_PROGRESS='--info=progress2'; "
        "rsync --help 2>/dev/null | grep -q -- '--info' || RSYNC_PROGRESS='--progress'; "
        "RSYNC_OPTS='-aHWS --delete'; "
        "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
        "if rsync -X --version >/dev/null 2>&1; then "
        "  RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
        "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then "
        "  RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; "
        "fi");
    const QString dstSsh = buildSshTargetPrefix(dp);
    const QString srcEffectiveMp = effectiveMountPath(src.connIdx, src.poolName, src.datasetName, srcMp, srcMounted);
    const QString dstEffectiveMp = effectiveMountPath(dst.connIdx, dst.poolName, dst.datasetName, dstMp, dstMounted);
    auto isUsableMountPath = [&](int cidx, const QString& path) -> bool {
        const QString pth = path.trimmed();
        if (pth.isEmpty() || pth == QStringLiteral("none")) {
            return false;
        }
        if (isWindowsConnection(cidx)) {
            return pth.contains(':');
        }
        return pth.startsWith('/');
    };
    auto remoteHasTool = [&](int connIdx, const QString& tool) -> bool {
        if (connIdx < 0 || connIdx >= m_profiles.size() || tool.trimmed().isEmpty()) {
            return false;
        }
        const ConnectionProfile& p = m_profiles[connIdx];
        QString out;
        QString err;
        int rc = -1;
        const QString probeCmd = isWindowsConnection(connIdx)
                                     ? QStringLiteral("if (Get-Command %1 -ErrorAction SilentlyContinue) { Write-Output 1 } else { Write-Output 0 }")
                                           .arg(tool)
                                     : QStringLiteral("command -v %1 >/dev/null 2>&1 && echo 1 || echo 0").arg(tool);
        if (!runSsh(p, probeCmd, 15000, out, err, rc) || rc != 0) {
            return false;
        }
        return out.contains(QStringLiteral("1"));
    };
    auto chooseCodec = [&]() -> mwhelpers::StreamCodec {
        const bool zstdBoth = remoteHasTool(src.connIdx, QStringLiteral("zstd")) && remoteHasTool(dst.connIdx, QStringLiteral("zstd"));
        const bool gzipBoth = remoteHasTool(src.connIdx, QStringLiteral("gzip")) && remoteHasTool(dst.connIdx, QStringLiteral("gzip"));
        return chooseStreamCodec(zstdBoth, gzipBoth);
    };

    if (srcRootMounted && dstRootMounted) {
        if (!isUsableMountPath(src.connIdx, srcEffectiveMp) || !isUsableMountPath(dst.connIdx, dstEffectiveMp)) {
            QMessageBox::warning(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_no_se_pudo_07b2ca"),
                                     QStringLiteral("No se pudo resolver el punto de montaje efectivo para sincronizar.\nOrigen: %1\nDestino: %2"),
                                     QStringLiteral("Could not resolve effective mountpoint for synchronization.\nSource: %1\nTarget: %2"),
                                     QStringLiteral("无法解析用于同步的有效挂载点。\n源：%1\n目标：%2"))
                                     .arg(srcEffectiveMp, dstEffectiveMp));
            return;
        }
        if (isWindowsConnection(sp) || isWindowsConnection(dp)) {
            const mwhelpers::StreamCodec codec = chooseCodec();
            const QString srcTarCmd = buildTarSourceCommand(isWindowsConnection(sp), srcEffectiveMp, codec);
            const QString dstTarCmd = buildTarDestinationCommand(isWindowsConnection(dp), dstEffectiveMp, codec);
            QString command;
            if (sameConnection) {
                appLog(QStringLiteral("INFO"),
                       trk(QStringLiteral("t_sincroniza_aed9fc"),
                           QStringLiteral("Sincronizar: modo local remoto (tar, misma conexión)"),
                           QStringLiteral("Sync: remote-local mode (tar, same connection)"),
                           QStringLiteral("同步：远端本地模式（tar，同一连接）")));
                const QString remotePipe = buildPipedTransferCommand(srcTarCmd, dstTarCmd);
                command = sshExecFromLocal(sp, remotePipe);
            } else {
                command = buildPipedTransferCommand(sshExecFromLocal(sp, srcTarCmd),
                                                    sshExecFromLocal(dp, dstTarCmd));
            }
            appLog(QStringLiteral("WARN"),
                   trk(QStringLiteral("t_sincroniza_6ccd2e"),
                       QStringLiteral("Sincronizar en Windows usa fallback tar/ssh (codec=%1, sin --delete)."),
                       QStringLiteral("Sync on Windows uses tar/ssh fallback (codec=%1, no --delete)."),
                       QStringLiteral("Windows 上同步使用 tar/ssh 回退（编码=%1，无 --delete）。")).arg(streamCodecName(codec)));
            if (runLocalCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command, 0, false, true)) {
                invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
                refreshConnectionByIndex(dst.connIdx);
                reloadDatasetSide(QStringLiteral("dest"));
            }
            return;
        }
        QString command;
        if (sameConnection) {
            appLog(QStringLiteral("INFO"),
                   trk(QStringLiteral("t_sincroniza_d0a688"),
                       QStringLiteral("Sincronizar: modo local remoto (rsync, misma conexión)"),
                       QStringLiteral("Sync: remote-local mode (rsync, same connection)"),
                       QStringLiteral("同步：远端本地模式（rsync，同一连接）")));
            QString remoteRsync =
                QStringLiteral("%1; rsync $RSYNC_OPTS $RSYNC_PROGRESS %2/ %3/")
                    .arg(rsyncOptsProbe,
                         shSingleQuote(srcEffectiveMp),
                         shSingleQuote(dstEffectiveMp));
            remoteRsync = withSudo(sp, remoteRsync);
            command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        } else {
            QString remoteRsync =
                QStringLiteral("%1; rsync $RSYNC_OPTS $RSYNC_PROGRESS -e %2 %3/ %4:%5/")
                    .arg(rsyncOptsProbe,
                         shSingleQuote(sshBaseCommand(dp)),
                         shSingleQuote(srcEffectiveMp),
                         shSingleQuote(sshUserHost(dp)),
                         shSingleQuote(dstEffectiveMp));
            remoteRsync = withSudo(sp, remoteRsync);
            command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        }
        if (runLocalCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command, 0, false, true)) {
            invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
            refreshConnectionByIndex(dst.connIdx);
            reloadDatasetSide(QStringLiteral("dest"));
        }
        return;
    }

    if ((!srcRootMounted && !srcCanmountOff) || (!dstRootMounted && !dstCanmountOff)) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_origen_y_d_79e6b3"),
                                 QStringLiteral("Origen y destino deben estar montados para sincronizar,\no bien tener canmount=off con subdatasets montados.\nOrigen mounted=%1 canmount=%2\nDestino mounted=%3 canmount=%4"),
                                 QStringLiteral("Source and target must be mounted to synchronize,\nor canmount=off with mounted subdatasets.\nSource mounted=%1 canmount=%2\nTarget mounted=%3 canmount=%4"),
                                 QStringLiteral("源和目标必须已挂载才能同步，\n或设置 canmount=off 且子数据集已挂载。\n源 mounted=%1 canmount=%2\n目标 mounted=%3 canmount=%4"))
                                 .arg(srcMounted, srcCanmount, dstMounted, dstCanmount));
        return;
    }

    if (!ensureDatasetsLoaded(src.connIdx, src.poolName) || !ensureDatasetsLoaded(dst.connIdx, dst.poolName)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_no_se_pudi_e1c0c7"),
                                 QStringLiteral("No se pudieron cargar datasets para sincronización por subdatasets."),
                                 QStringLiteral("Could not load datasets for subdataset synchronization."),
                                 QStringLiteral("无法加载用于子数据集同步的数据集。")));
        return;
    }
    const QString srcKey = datasetCacheKey(src.connIdx, src.poolName);
    const QString dstKey = datasetCacheKey(dst.connIdx, dst.poolName);
    const auto srcCacheIt = m_poolDatasetCache.constFind(srcKey);
    const auto dstCacheIt = m_poolDatasetCache.constFind(dstKey);
    if (srcCacheIt == m_poolDatasetCache.constEnd() || dstCacheIt == m_poolDatasetCache.constEnd()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_no_hay_cac_c8370a"),
                                 QStringLiteral("No hay caché de datasets para sincronización por subdatasets."),
                                 QStringLiteral("No dataset cache for subdataset synchronization."),
                                 QStringLiteral("没有用于子数据集同步的数据集缓存。")));
        return;
    }

    const QString srcPrefix = src.datasetName + QStringLiteral("/");
    QStringList missingInDest;
    QStringList notMountedPairs;
    QVector<QPair<QString, QString>> syncPairs;

    for (auto it = srcCacheIt->recordByName.constBegin(); it != srcCacheIt->recordByName.constEnd(); ++it) {
        const QString& srcDsName = it.key();
        if (!srcDsName.startsWith(srcPrefix)) {
            continue;
        }
        const DatasetRecord& srcRec = it.value();
        if (!isMountedValueTrue(srcRec.mounted)) {
            continue;
        }
        const QString srcRecMp = effectiveMountPath(src.connIdx, src.poolName, srcDsName, srcRec.mountpoint, srcRec.mounted);
        if (!isUsableMountPath(src.connIdx, srcRecMp)) {
            continue;
        }
        const QString rel = srcDsName.mid(srcPrefix.size());
        if (rel.isEmpty()) {
            continue;
        }
        const QString dstDsName = dst.datasetName + QStringLiteral("/") + rel;
        const auto dstRecIt = dstCacheIt->recordByName.constFind(dstDsName);
        if (dstRecIt == dstCacheIt->recordByName.constEnd()) {
            missingInDest << dstDsName;
            continue;
        }
        const DatasetRecord& dstRec = dstRecIt.value();
        const QString dstRecMp = effectiveMountPath(dst.connIdx, dst.poolName, dstDsName, dstRec.mountpoint, dstRec.mounted);
        if (!isMountedValueTrue(dstRec.mounted) || !isUsableMountPath(dst.connIdx, dstRecMp)) {
            notMountedPairs << QStringLiteral("%1 -> %2").arg(srcDsName, dstDsName);
            continue;
        }
        syncPairs.push_back(qMakePair(srcRecMp, dstRecMp));
    }

    if (!missingInDest.isEmpty() || !notMountedPairs.isEmpty()) {
        QString details;
        if (!missingInDest.isEmpty()) {
            details += QStringLiteral("Faltan en destino:\n") + missingInDest.join('\n') + QStringLiteral("\n\n");
        }
        if (!notMountedPairs.isEmpty()) {
            details += QStringLiteral("No montados en ambos extremos:\n") + notMountedPairs.join('\n');
        }
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_no_se_pued_8dace0"),
                                 QStringLiteral("No se puede sincronizar por subdatasets.\n%1"),
                                 QStringLiteral("Cannot synchronize by subdatasets.\n%1"),
                                 QStringLiteral("无法按子数据集同步。\n%1")).arg(details.trimmed()));
        return;
    }
    if (syncPairs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_no_hay_sub_86be11"),
                                     QStringLiteral("No hay subdatasets montados equivalentes para sincronizar."),
                                     QStringLiteral("No equivalent mounted subdatasets to synchronize."),
                                     QStringLiteral("没有可同步的等效已挂载子数据集。")));
        return;
    }

    const QString sshTransport = sshBaseCommand(dp);
    QStringList rsyncCommands;
    if (isWindowsConnection(sp) || isWindowsConnection(dp)) {
        const mwhelpers::StreamCodec codec = chooseCodec();
        QStringList tarPipelines;
        tarPipelines.reserve(syncPairs.size());
        for (const auto& pair : syncPairs) {
            const QString srcTarCmd = buildTarSourceCommand(isWindowsConnection(sp), pair.first, codec);
            const QString dstTarCmd = buildTarDestinationCommand(isWindowsConnection(dp), pair.second, codec);
            if (sameConnection) {
                tarPipelines << sshExecFromLocal(sp, buildPipedTransferCommand(srcTarCmd, dstTarCmd));
            } else {
                tarPipelines << buildPipedTransferCommand(sshExecFromLocal(sp, srcTarCmd),
                                                          sshExecFromLocal(dp, dstTarCmd));
            }
        }
        const QString command = tarPipelines.join(QStringLiteral(" && "));
        appLog(QStringLiteral("WARN"),
               trk(QStringLiteral("t_sincroniza_86c64e"),
                   QStringLiteral("Sincronizar subdatasets en Windows usa fallback tar/ssh (codec=%1, sin --delete)."),
                   QStringLiteral("Subdataset sync on Windows uses tar/ssh fallback (codec=%1, no --delete)."),
                   QStringLiteral("Windows 上子数据集同步使用 tar/ssh 回退（编码=%1，无 --delete）。")).arg(streamCodecName(codec)));
        if (runLocalCommand(QStringLiteral("Sincronizar subdatasets %1 -> %2 (%3)")
                                .arg(src.datasetName, dst.datasetName)
                                .arg(syncPairs.size()),
                            command,
                            0,
                            false,
                            true)) {
            invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
            refreshConnectionByIndex(dst.connIdx);
            reloadDatasetSide(QStringLiteral("dest"));
        }
    } else {
        rsyncCommands.reserve(syncPairs.size());
        for (const auto& pair : syncPairs) {
            if (sameConnection) {
                rsyncCommands << QStringLiteral("rsync $RSYNC_OPTS $RSYNC_PROGRESS %1/ %2/")
                                     .arg(shSingleQuote(pair.first), shSingleQuote(pair.second));
            } else {
                rsyncCommands << QStringLiteral("rsync $RSYNC_OPTS $RSYNC_PROGRESS -e %1 %2/ %3:%4/")
                                     .arg(shSingleQuote(sshTransport),
                                          shSingleQuote(pair.first),
                                          shSingleQuote(sshUserHost(dp)),
                                          shSingleQuote(pair.second));
            }
        }
        QString remoteRsync = QStringLiteral("set -e; %1; %2")
                                  .arg(rsyncOptsProbe, rsyncCommands.join(QStringLiteral(" && ")));
        remoteRsync = withSudo(sp, remoteRsync);
        const QString command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        if (runLocalCommand(QStringLiteral("Sincronizar subdatasets %1 -> %2 (%3)")
                                .arg(src.datasetName, dst.datasetName)
                                .arg(syncPairs.size()),
                            command,
                            0,
                            false,
                            true)) {
            invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
            refreshConnectionByIndex(dst.connIdx);
            reloadDatasetSide(QStringLiteral("dest"));
        }
    }
}
