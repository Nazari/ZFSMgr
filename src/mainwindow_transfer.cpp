#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QApplication>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::shSingleQuote;
using mwhelpers::buildSshTargetPrefix;
using mwhelpers::buildPipedTransferCommand;
using mwhelpers::buildTarDestinationCommand;
using mwhelpers::buildTarSourceCommand;
using mwhelpers::chooseStreamCodec;
using mwhelpers::streamCodecName;
using mwhelpers::sshBaseCommand;
using mwhelpers::sshUserHost;

QString buildDirectWindowsBashSshInvocation(const ConnectionProfile& p, const QString& remoteCmd) {
    const QString sshBase = sshBaseCommand(p);
    const QString target = shSingleQuote(sshUserHost(p));
    const QString winCmd = QStringLiteral("\"C:\\\\msys64\\\\usr\\\\bin\\\\bash.exe\" -lc %1")
                               .arg(shSingleQuote(remoteCmd));
    return sshBase + QStringLiteral(" ") + target + QStringLiteral(" ") + shSingleQuote(winCmd);
}

QString unixAlternateMountHelpersScript() {
    return QStringLiteral(
        "mount_alt_zfs(){ ds=\"$1\"; mp=\"$2\"; "
        "saved_prop='org.fc16.zfsmgr:savedmountpoint'; "
        "current_mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
        "zfs set \"$saved_prop=$current_mp\" \"$ds\"; "
        "zfs set mountpoint=\"$mp\" \"$ds\"; "
        "zfs mount \"$ds\"; }; "
        "umount_alt_zfs(){ ds=\"$1\"; mp=\"$2\"; "
        "saved_prop='org.fc16.zfsmgr:savedmountpoint'; "
        "zfs unmount \"$ds\" >/dev/null 2>&1 || zfs unmount \"$mp\" >/dev/null 2>&1 || true; "
        "saved_mp=$(zfs get -H -o value \"$saved_prop\" \"$ds\" 2>/dev/null || true); "
        "if [ -n \"$saved_mp\" ] && [ \"$saved_mp\" != \"-\" ]; then zfs set mountpoint=\"$saved_mp\" \"$ds\" >/dev/null 2>&1 || true; fi; "
        "zfs inherit \"$saved_prop\" \"$ds\" >/dev/null 2>&1 || true; }; "
        "resolve_mp(){ ds=\"$1\"; zfs mount 2>/dev/null | awk -v d=\"$ds\" '$1==d{print $2; exit}'; }; ");
}

QString buildUnixTemporaryTarSourceScript(const QString& dataset, mwhelpers::StreamCodec codec) {
    const QString tarFromVar =
        (codec == mwhelpers::StreamCodec::Zstd) ? QStringLiteral("tar --acls --xattrs -cpf - -C \"$MP\" . | zstd -1 -T0 -q -c")
        : (codec == mwhelpers::StreamCodec::Gzip) ? QStringLiteral("tar --acls --xattrs -cpf - -C \"$MP\" . | gzip -1 -c")
        : QStringLiteral("tar --acls --xattrs -cpf - -C \"$MP\" .");
    return QStringLiteral(
               "set -e; DATASET=%1; %2"
               "TMP_MP=''; "
               "cleanup(){ if [ -n \"$TMP_MP\" ]; then umount_alt_zfs \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1 || true; rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; fi; }; "
               "trap cleanup EXIT INT TERM; "
               "MP=$(resolve_mp \"$DATASET\"); "
               "if [ -z \"$MP\" ]; then TMP_MP=$(mktemp -d /tmp/zfsmgr-sync-src-XXXXXX); mount_alt_zfs \"$DATASET\" \"$TMP_MP\"; MP=\"$TMP_MP\"; fi; "
               "[ -n \"$MP\" ] && [ -d \"$MP\" ] || exit 41; "
               "%3")
        .arg(shSingleQuote(dataset), unixAlternateMountHelpersScript(), tarFromVar);
}

QString buildUnixTemporaryTarDestinationScript(const QString& dataset, mwhelpers::StreamCodec codec) {
    const QString decodePipe =
        (codec == mwhelpers::StreamCodec::Zstd) ? QStringLiteral("zstd -d -q -c - | ")
        : (codec == mwhelpers::StreamCodec::Gzip) ? QStringLiteral("gzip -d -c - | ")
        : QString();
    return QStringLiteral(
               "set -e; DATASET=%1; %2"
               "TMP_MP=''; "
               "cleanup(){ if [ -n \"$TMP_MP\" ]; then umount_alt_zfs \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1 || true; rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; fi; }; "
               "trap cleanup EXIT INT TERM; "
               "MP=$(resolve_mp \"$DATASET\"); "
               "if [ -z \"$MP\" ]; then TMP_MP=$(mktemp -d /tmp/zfsmgr-sync-dst-XXXXXX); mount_alt_zfs \"$DATASET\" \"$TMP_MP\"; MP=\"$TMP_MP\"; fi; "
               "mkdir -p \"$MP\"; "
               "%3tar --acls --xattrs -xpf - -C \"$MP\"")
        .arg(shSingleQuote(dataset), unixAlternateMountHelpersScript(), decodePipe);
}
} // namespace

QString MainWindow::pendingTransferScopeLabel(const DatasetSelectionContext& src, const DatasetSelectionContext& dst) const {
    auto connPoolLabel = [this](const DatasetSelectionContext& ctx) {
        if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size() || ctx.poolName.trimmed().isEmpty()) {
            return QString();
        }
        const ConnectionProfile& p = m_profiles.at(ctx.connIdx);
        const QString connLabel = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
        return QStringLiteral("%1::%2").arg(connLabel, ctx.poolName.trimmed());
    };
    const QString srcLabel = connPoolLabel(src);
    const QString dstLabel = connPoolLabel(dst);
    if (srcLabel.isEmpty()) {
        return dstLabel;
    }
    if (dstLabel.isEmpty() || dstLabel == srcLabel) {
        return srcLabel;
    }
    return QStringLiteral("%1 -> %2").arg(srcLabel, dstLabel);
}

bool MainWindow::queuePendingShellAction(const PendingShellActionDraft& draft, QString* errorOut) {
    auto fail = [errorOut](const QString& text) {
        if (errorOut) {
            *errorOut = text;
        }
        return false;
    };
    if (draft.command.trimmed().isEmpty()) {
        return fail(QStringLiteral("No hay comando que añadir a cambios pendientes."));
    }
    for (const PendingChange& existing : m_pendingChangesModel) {
        if (existing.kind != PendingChange::Kind::ShellAction) {
            continue;
        }
        if (existing.shellDraft.displayLabel.trimmed() == draft.displayLabel.trimmed()
            && existing.shellDraft.command.trimmed() == draft.command.trimmed()) {
            return fail(QStringLiteral("Ese cambio ya está en la lista de pendientes."));
        }
    }
    PendingChange change;
    change.kind = PendingChange::Kind::ShellAction;
    change.shellDraft = draft;
    change.removableIndividually = true;
    change.executableIndividually = true;
    change.stableId = QStringLiteral("shell|%1|%2")
                          .arg(draft.displayLabel.trimmed(),
                               draft.command.trimmed());
    m_pendingChangesModel.push_back(change);
    return true;
}

void MainWindow::actionCopySnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    QString transferVersionReason;
    if (!isTransferVersionAllowed(src, dst, &transferVersionReason)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), transferVersionReason);
        appLog(QStringLiteral("WARN"), transferVersionReason);
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
    const bool directRemoteToRemote =
        !sameConnection
        && !isLocalConnection(sp)
        && !isLocalConnection(dp)
        && sp.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0
        && dp.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0;
    auto buildDestViaSource = [&](const QString& remoteCmd) {
        const QString wrappedDst = wrapRemoteCommand(dp, remoteCmd);
        const QString target = shSingleQuote(sshUserHost(dp));
        if (!dp.password.trimmed().isEmpty()) {
            return QStringLiteral(
                       "if command -v sshpass >/dev/null 2>&1; then "
                       "SSHPASS=%1 sshpass -e %2 -o BatchMode=no "
                       "-o PreferredAuthentications=password,keyboard-interactive,publickey "
                       "-o NumberOfPasswordPrompts=1 %3 %4; "
                       "else echo %5 >&2; exit 127; fi")
                .arg(shSingleQuote(dp.password),
                     sshBaseCommand(dp),
                     target,
                     shSingleQuote(wrappedDst),
                     shSingleQuote(QStringLiteral("ZFSMgr: sshpass no disponible en el origen para conectar al destino")));
        }
        return sshBaseCommand(dp) + QStringLiteral(" ") + target + QStringLiteral(" ") + shSingleQuote(wrappedDst);
    };
    if (sameConnection) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_copiar_mod_b1d73e"),
                   QStringLiteral("Copiar: modo local remoto (origen y destino en la misma conexión)"),
                   QStringLiteral("Copy: remote-local mode (source and target on same connection)"),
                   QStringLiteral("复制：远端本地模式（源和目标在同一连接）")));
        const QString remotePipe = withSudo(sp, buildPipedTransferCommand(sendRawCmd, recvRawCmd));
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else if (directRemoteToRemote) {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Copiar: modo remoto a remoto directo (%1 -> %2), sin pasar datos por el host local")
                   .arg(sp.name, dp.name));
        const QString remotePipe = buildPipedTransferCommand(sendCmd, buildDestViaSource(recvCmd));
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else {
        const QString srcSeg = isWindowsConnection(sp)
                                   ? buildDirectWindowsBashSshInvocation(sp, sendCmd)
                                   : sshExecFromLocal(sp, sendCmd);
        const QString dstSeg = isWindowsConnection(dp)
                                   ? buildDirectWindowsBashSshInvocation(dp, recvCmd)
                                   : sshExecFromLocal(dp, recvCmd);
        pipeline = buildPipedTransferCommand(srcSeg, dstSeg);
    }

    QString pendingCommand = pipeline;
    QString displayLabel = QStringLiteral("Copiar snapshot %1 -> %2").arg(srcSnap, recvTarget);
    if (isWindowsConnection(sp) || isWindowsConnection(dp)) {
        const bool builtFallback = [&]() -> bool {
            if (!(isWindowsConnection(sp) || isWindowsConnection(dp))) {
                return false;
            }
            QString srcScript = QStringLiteral(
                "set -e; DATASET=%1; SNAP=%2; "
                "WAS_MOUNTED=\"$(zfs get -H -o value mounted \"$DATASET\" 2>/dev/null)\"; "
                "[ -n \"$WAS_MOUNTED\" ] || WAS_MOUNTED=no; "
                "MP=\"$(zfs mount 2>/dev/null | grep -E \"^$DATASET[[:space:]]\" | head -n1 | cut -d\" \" -f2-)\"; "
                "if [ -z \"$MP\" ]; then if ! zfs mount \"$DATASET\" >/dev/null 2>&1; then :; fi; "
                "MP=\"$(zfs mount 2>/dev/null | grep -E \"^$DATASET[[:space:]]\" | head -n1 | cut -d\" \" -f2-)\"; fi; "
                "[ -n \"$MP\" ] && [ -d \"$MP\" ] || { echo \"[COPY][ERROR] cannot mount source dataset: $DATASET\"; exit 41; }; "
                "SRC=\"$MP/.zfs/snapshot/$SNAP\"; "
                "[ -d \"$SRC\" ] || { echo \"[COPY][ERROR] snapshot path unavailable: $SRC\"; exit 42; }; "
                "tar --acls --xattrs -cpf - -C \"$SRC\" .; RC=$?; "
                "if [ \"$WAS_MOUNTED\" != \"yes\" ]; then if ! zfs unmount \"$DATASET\" >/dev/null 2>&1; then :; fi; fi; "
                "exit $RC")
                                          .arg(shSingleQuote(src.datasetName), shSingleQuote(src.snapshotName));

            QString dstScript = QStringLiteral(
                "set -e; DATASET=%1; "
                "if ! zfs list -H -o name \"$DATASET\" >/dev/null 2>&1; then zfs create -u \"$DATASET\"; fi; "
                "WAS_MOUNTED=\"$(zfs get -H -o value mounted \"$DATASET\" 2>/dev/null)\"; "
                "[ -n \"$WAS_MOUNTED\" ] || WAS_MOUNTED=no; "
                "MP=\"$(zfs mount 2>/dev/null | grep -E \"^$DATASET[[:space:]]\" | head -n1 | cut -d\" \" -f2-)\"; "
                "if [ -z \"$MP\" ]; then if ! zfs mount \"$DATASET\" >/dev/null 2>&1; then :; fi; "
                "MP=\"$(zfs mount 2>/dev/null | grep -E \"^$DATASET[[:space:]]\" | head -n1 | cut -d\" \" -f2-)\"; fi; "
                "[ -n \"$MP\" ] && [ -d \"$MP\" ] || { echo \"[COPY][ERROR] cannot mount destination dataset: $DATASET\"; exit 43; }; "
                "mkdir -p \"$MP\"; tar --acls --xattrs -xpf - -C \"$MP\"; RC=$?; "
                "if [ \"$WAS_MOUNTED\" != \"yes\" ]; then if ! zfs unmount \"$DATASET\" >/dev/null 2>&1; then :; fi; fi; "
                "exit $RC")
                                          .arg(shSingleQuote(recvTarget));
            if (!isWindowsConnection(sp) && !isLocalConnection(sp)) {
                (void)ensureRemoteScriptsUpToDate(sp);
                srcScript = remoteScriptCommand(
                    sp,
                    QStringLiteral("zfsmgr-copy-fallback-source"),
                    {src.datasetName, src.snapshotName});
            }
            if (!isWindowsConnection(dp) && !isLocalConnection(dp)) {
                (void)ensureRemoteScriptsUpToDate(dp);
                dstScript = remoteScriptCommand(
                    dp,
                    QStringLiteral("zfsmgr-copy-fallback-dest"),
                    {recvTarget});
            }
            if (directRemoteToRemote) {
                pendingCommand = sshExecFromLocal(
                    sp,
                    buildPipedTransferCommand(withSudo(sp, srcScript),
                                              buildDestViaSource(withSudoStreamInput(dp, dstScript))),
                    MainWindow::WindowsCommandMode::UnixShell);
            } else {
                pendingCommand = buildPipedTransferCommand(sshExecFromLocal(sp, withSudo(sp, srcScript)),
                                                           sshExecFromLocal(dp, withSudoStreamInput(dp, dstScript)));
            }
            displayLabel = QStringLiteral("Copiar snapshot (fallback TAR) %1 -> %2").arg(srcSnap, recvTarget);
            return true;
        }();
        Q_UNUSED(builtFallback);
    }
    QString errorText;
    if (!queuePendingShellAction(PendingShellActionDraft{
            pendingTransferScopeLabel(src, dst),
            displayLabel,
            pendingCommand,
            0,
            true,
            src,
            dst,
            PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Cambio pendiente añadido: %1  %2")
               .arg(pendingTransferScopeLabel(src, dst), displayLabel));
    updateApplyPropsButtonState();
}

void MainWindow::actionCloneSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty() || !dst.snapshotName.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_clone_need_sel001"),
                QStringLiteral("Para Clonar debe seleccionar:\n"
                               "- En Origen: un snapshot\n"
                               "- En Destino: un dataset (sin snapshot)")));
        return;
    }
    if (src.connIdx != dst.connIdx) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_clone_same_conn1"),
                QStringLiteral("Clonar requiere que Origen y Destino estén en la misma conexión.")));
        return;
    }
    if (src.poolName.trimmed().compare(dst.poolName.trimmed(), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_clone_same_pool1"),
                QStringLiteral("Clonar requiere que Origen y Destino estén en el mismo pool.")));
        return;
    }

    const QString sourceSnapshot = QStringLiteral("%1@%2").arg(src.datasetName, src.snapshotName);
    QString targetDataset = dst.datasetName;
    const QString srcLeaf = src.datasetName.section('/', -1);
    const QString dstLeaf = dst.datasetName.section('/', -1);
    if (!srcLeaf.isEmpty() && dstLeaf != srcLeaf) {
        targetDataset = dst.datasetName + QStringLiteral("/") + srcLeaf;
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_clone_wnd_001"),
            QStringLiteral("Clonar snapshot")));
    auto* form = new QFormLayout(&dlg);
    auto* srcEd = new QLineEdit(&dlg);
    srcEd->setReadOnly(true);
    srcEd->setText(sourceSnapshot);
    auto* dstEd = new QLineEdit(&dlg);
    dstEd->setText(targetDataset);
    auto* pChk = new QCheckBox(
        trk(QStringLiteral("t_clone_p_001"),
            QStringLiteral("Crear padres (-p)")),
        &dlg);
    auto* uChk = new QCheckBox(
        trk(QStringLiteral("t_clone_u_001"),
            QStringLiteral("No montar automáticamente (-u)")),
        &dlg);
    auto* propsEd = new QPlainTextEdit(&dlg);
    propsEd->setPlaceholderText(
        trk(QStringLiteral("t_clone_props_ph1"),
            QStringLiteral("Una propiedad por línea, ejemplo:\ncompression=lz4\natime=off")));
    propsEd->setFixedHeight(90);

    form->addRow(
        trk(QStringLiteral("t_clone_src_001"),
            QStringLiteral("Snapshot origen")),
        srcEd);
    form->addRow(
        trk(QStringLiteral("t_clone_dst_001"),
            QStringLiteral("Dataset destino")),
        dstEd);
    form->addRow(QString(), pChk);
    form->addRow(QString(), uChk);
    form->addRow(
        trk(QStringLiteral("t_clone_o_001"),
            QStringLiteral("Propiedades (-o prop=valor)")),
        propsEd);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    targetDataset = dstEd->text().trimmed();
    if (targetDataset.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_clone_dst_req001"),
                QStringLiteral("El dataset destino no puede estar vacío.")));
        return;
    }

    QString cmd = QStringLiteral("zfs clone");
    if (pChk->isChecked()) {
        cmd += QStringLiteral(" -p");
    }
    if (uChk->isChecked()) {
        cmd += QStringLiteral(" -u");
    }
    const QStringList propLines = propsEd->toPlainText().split('\n', Qt::SkipEmptyParts);
    for (const QString& raw : propLines) {
        const QString prop = raw.trimmed();
        if (prop.isEmpty()) {
            continue;
        }
        if (!prop.contains('=')) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_clone_prop_inv001"),
                    QStringLiteral("Propiedad inválida: \"%1\".\nUse formato nombre=valor.")
                        .arg(prop)));
            return;
        }
        cmd += QStringLiteral(" -o %1").arg(shSingleQuote(prop));
    }
    cmd += QStringLiteral(" %1 %2").arg(shSingleQuote(sourceSnapshot), shSingleQuote(targetDataset));

    ConnectionProfile cp = m_profiles[src.connIdx];
    if (isLocalConnection(cp) && !isWindowsConnection(cp)) {
        cp.useSudo = true;
        if (!ensureLocalSudoCredentials(cp)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Clonar cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    const QString fullCmd = sshExecFromLocal(cp, withSudo(cp, cmd));
    DatasetSelectionContext refreshDst = dst;
    const QString targetPool = targetDataset.section('/', 0, 0).trimmed();
    if (!targetPool.isEmpty()) {
        refreshDst.poolName = targetPool;
    }
    refreshDst.datasetName = targetDataset;
    refreshDst.snapshotName.clear();
    QString errorText;
    if (!queuePendingShellAction(PendingShellActionDraft{
            pendingTransferScopeLabel(src, refreshDst),
            QStringLiteral("Clonar snapshot %1 -> %2").arg(sourceSnapshot, targetDataset),
            fullCmd,
            0,
            true,
            src,
            refreshDst,
            PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Cambio pendiente añadido: %1  %2")
               .arg(pendingTransferScopeLabel(src, refreshDst),
                    QStringLiteral("Clonar snapshot %1 -> %2").arg(sourceSnapshot, targetDataset)));
    updateApplyPropsButtonState();
}

void MainWindow::actionDiffSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || src.datasetName.isEmpty() || dst.datasetName.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_diff_need_sel001"),
                QStringLiteral("Para Diff debe seleccionar:\n"
                               "- En Origen: un snapshot\n"
                               "- En Destino: su dataset padre o un snapshot del mismo dataset.")));
        return;
    }
    if (src.connIdx != dst.connIdx) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_diff_same_conn001"),
                QStringLiteral("Diff requiere que Origen y Destino estén en la misma conexión.")));
        return;
    }
    if (src.poolName.trimmed().compare(dst.poolName.trimmed(), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_diff_same_pool001"),
                QStringLiteral("Diff requiere que Origen y Destino estén en el mismo pool.")));
        return;
    }
    if (src.datasetName.trimmed().compare(dst.datasetName.trimmed(), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_diff_same_parent001"),
                QStringLiteral("Diff requiere que Destino sea el dataset padre del snapshot origen o otro snapshot del mismo dataset.")));
        return;
    }
    if (!dst.snapshotName.isEmpty()
        && src.snapshotName.trimmed().compare(dst.snapshotName.trimmed(), Qt::CaseInsensitive) == 0) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_diff_other_snap001"),
                QStringLiteral("Diff entre el mismo snapshot no aporta cambios. Seleccione el dataset padre u otro snapshot.")));
        return;
    }

    ConnectionProfile profile = m_profiles[src.connIdx];
    if (isLocalConnection(profile) && !isWindowsConnection(profile)) {
        profile.useSudo = true;
        if (!ensureLocalSudoCredentials(profile)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Diff cancelado: faltan credenciales sudo locales"));
            return;
        }
    }

    const QString srcObj = QStringLiteral("%1@%2").arg(src.datasetName, src.snapshotName);
    const QString dstObj = dst.snapshotName.isEmpty()
                               ? dst.datasetName
                               : QStringLiteral("%1@%2").arg(dst.datasetName, dst.snapshotName);
    const QString rawCmd = QStringLiteral("zfs diff %1 %2")
                               .arg(shSingleQuote(srcObj),
                                    shSingleQuote(dstObj));
    const QString remoteCmd = withSudo(profile, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3")
                                         .arg(profile.username, profile.host)
                                         .arg(profile.port > 0 ? QString::number(profile.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(profile, remoteCmd));
    if (!confirmActionExecution(QStringLiteral("Diff"), {preview})) {
        return;
    }

    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Diff %1 -> %2").arg(srcObj, dstObj));
    updateStatus(QStringLiteral("Diff %1::%2").arg(profile.name, src.datasetName));
    QString out;
    QString err;
    int rc = -1;
    auto progressLogger = [this](const QString& rawLine) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            return;
        }
        appLog(QStringLiteral("NORMAL"), QStringLiteral("[DIFF] %1").arg(line));
    };
    int lastIdleBucket = -1;
    auto idleLogger = [this, &lastIdleBucket](int remainingSec) {
        const int bucket = (remainingSec / 10) * 10;
        if (remainingSec <= 0 || bucket <= 0 || bucket == lastIdleBucket) {
            return;
        }
        lastIdleBucket = bucket;
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("[DIFF] Sin salida todavía. Timeout por inactividad en %1 s.")
                   .arg(bucket));
    };
    const bool ok = runSsh(profile, remoteCmd, 60000, out, err, rc, progressLogger, progressLogger, idleLogger);
    setActionsLocked(false);
    if (!ok || rc != 0) {
        const QString failureDetail = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error en Diff: %1").arg(mwhelpers::oneLine(failureDetail)));
        QMessageBox::critical(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_diff_fail001"),
                QStringLiteral("Diff falló:\n%1").arg(failureDetail)));
        return;
    }

    auto ensurePathNode = [](QTreeWidgetItem* root,
                             const QString& rawPath,
                             const QString& leafLabel = QString(),
                             const QString& leafToolTip = QString()) {
        if (!root) {
            return;
        }
        QString path = rawPath.trimmed();
        if (path.isEmpty()) {
            if (!leafLabel.isEmpty()) {
                auto* item = new QTreeWidgetItem(root, QStringList{leafLabel});
                if (!leafToolTip.isEmpty()) {
                    item->setToolTip(0, leafToolTip);
                }
            }
            return;
        }
        while (path.startsWith('/')) {
            path.remove(0, 1);
        }
        QStringList parts = path.split('/', Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            parts << path;
        }
        if (!leafLabel.isEmpty()) {
            parts[parts.size() - 1] = leafLabel;
        }
        QTreeWidgetItem* parent = root;
        for (const QString& part : parts) {
            QTreeWidgetItem* child = nullptr;
            for (int i = 0; i < parent->childCount(); ++i) {
                if (parent->child(i)->text(0) == part) {
                    child = parent->child(i);
                    break;
                }
            }
            if (!child) {
                child = new QTreeWidgetItem(parent, QStringList{part});
            }
            parent = child;
        }
        if (!leafToolTip.isEmpty()) {
            parent->setToolTip(0, leafToolTip);
        }
    };

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(900, 620);
    dlg.setWindowTitle(trk(QStringLiteral("t_diff_btn_001"), QStringLiteral("Diff")));
    auto* rootLayout = new QVBoxLayout(&dlg);
    auto* tree = new QTreeWidget(&dlg);
    tree->setColumnCount(1);
    tree->setHeaderLabel(QStringLiteral("%1 -> %2").arg(srcObj, dstObj));
    tree->setAlternatingRowColors(false);
    tree->setRootIsDecorated(true);
    QPalette treePalette = tree->palette();
    treePalette.setColor(QPalette::Base, QColor(QStringLiteral("#fbfdff")));
    treePalette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#fbfdff")));
    treePalette.setColor(QPalette::Text, QColor(QStringLiteral("#132231")));
    treePalette.setColor(QPalette::WindowText, QColor(QStringLiteral("#132231")));
    tree->setPalette(treePalette);
    tree->setStyleSheet(QStringLiteral(
        "QTreeWidget{background:#fbfdff; color:#132231; alternate-background-color:#fbfdff;}"
        "QTreeWidget::item{background:#fbfdff; color:#132231;}"
        "QTreeWidget::item:selected{background:#dcecff; color:#132231;}"
        "QHeaderView::section{background:#eef4f8; color:#132231;}"
    ));
    auto* addedRoot = new QTreeWidgetItem(tree, QStringList{QStringLiteral("Añadido")});
    auto* deletedRoot = new QTreeWidgetItem(tree, QStringList{QStringLiteral("Borrado")});
    auto* modifiedRoot = new QTreeWidgetItem(tree, QStringList{QStringLiteral("Modificado")});
    auto* renamedRoot = new QTreeWidgetItem(tree, QStringList{QStringLiteral("Renombrado")});

    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList cols = line.split('\t');
        if (cols.isEmpty()) {
            continue;
        }
        const QChar kind = cols.value(0).trimmed().isEmpty() ? QChar() : cols.value(0).trimmed().at(0);
        QString path = cols.size() >= 3 ? cols.value(2).trimmed() : cols.value(cols.size() - 1).trimmed();
        QString leafLabel;
        QString leafToolTip;
        QTreeWidgetItem* targetRoot = modifiedRoot;
        if (kind == QLatin1Char('+')) {
            targetRoot = addedRoot;
        } else if (kind == QLatin1Char('-')) {
            targetRoot = deletedRoot;
        } else if (kind == QLatin1Char('M')) {
            targetRoot = modifiedRoot;
        } else if (kind == QLatin1Char('R')) {
            targetRoot = renamedRoot;
            const QString oldPath = cols.size() >= 3 ? cols.value(2).trimmed() : QString();
            const QString newPath = cols.size() >= 4 ? cols.value(3).trimmed() : QString();
            if (!newPath.isEmpty()) {
                const QString oldLeaf = QFileInfo(oldPath).fileName();
                const QString newLeaf = QFileInfo(newPath).fileName();
                leafLabel = oldLeaf.isEmpty()
                                ? newLeaf
                                : QStringLiteral("%1 <- %2").arg(newLeaf, oldLeaf);
                leafToolTip = QStringLiteral("Antes: %1\nAhora: %2").arg(oldPath, newPath);
                path = newPath;
            }
        } else {
            continue;
        }
        ensurePathNode(targetRoot, path, leafLabel, leafToolTip);
    }

    addedRoot->setExpanded(true);
    deletedRoot->setExpanded(true);
    modifiedRoot->setExpanded(true);
    renamedRoot->setExpanded(true);
    tree->header()->setStretchLastSection(true);
    rootLayout->addWidget(tree, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    rootLayout->addWidget(bb);
    dlg.exec();
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
    QString transferVersionReason;
    if (!isTransferVersionAllowed(src, dst, &transferVersionReason)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), transferVersionReason);
        appLog(QStringLiteral("WARN"), transferVersionReason);
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
    const QStringList srcSnaps = datasetSnapshotsFromModel(src.connIdx, src.poolName, src.datasetName);
    const QStringList dstSnaps = datasetSnapshotsFromModel(dst.connIdx, dst.poolName, dst.datasetName);
    const QString srcCacheKey = datasetCacheKey(src.connIdx, src.poolName);
    const QString dstCacheKey = datasetCacheKey(dst.connIdx, dst.poolName);
    const auto srcCacheIt = m_poolDatasetCache.constFind(srcCacheKey);
    const auto dstCacheIt = m_poolDatasetCache.constFind(dstCacheKey);
    if (srcCacheIt == m_poolDatasetCache.constEnd() || dstCacheIt == m_poolDatasetCache.constEnd()
        || !srcCacheIt->loaded || !dstCacheIt->loaded) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_load_sn01"),
                                 QStringLiteral("No se pudieron cargar snapshots para Nivelar."),
                                 QStringLiteral("Could not load snapshots for Level."),
                                 QStringLiteral("无法加载用于同步快照的快照列表。")));
        return;
    }

    auto snapshotGuidFor = [this](int connIdx,
                                  const QString& poolName,
                                  const PoolDatasetCache& cache,
                                  const QString& datasetName,
                                  const QString& snapName) {
        const QString key = QStringLiteral("%1@%2").arg(datasetName.trimmed(), snapName.trimmed());
        QString guid = cache.objectGuidByName.value(key).trimmed();
        if (!guid.isEmpty() && guid != QStringLiteral("-")) {
            return guid;
        }
        QString loaded;
        if (ensureObjectGuidLoaded(connIdx, poolName, key, &loaded)) {
            return loaded.trimmed();
        }
        return QString();
    };
    auto findSnapshotNameByGuid = [&](const PoolDatasetCache& cache,
                                      int connIdx,
                                      const QString& poolName,
                                      const QString& datasetName,
                                      const QStringList& orderedSnaps,
                                      const QString& guid) -> QString {
        const QString g = guid.trimmed();
        if (g.isEmpty()) {
            return QString();
        }
        for (const QString& snapName : orderedSnaps) {
            const QString snapGuid = snapshotGuidFor(connIdx, poolName, cache, datasetName, snapName);
            if (!snapGuid.isEmpty() && snapGuid == g) {
                return snapName;
            }
        }
        return QString();
    };

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
    const QString targetGuid = snapshotGuidFor(src.connIdx, src.poolName, *srcCacheIt, src.datasetName, targetSnapName);
    if (targetGuid.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_guid_missing_target_001"),
                                 QStringLiteral("No se pudo obtener el GUID del snapshot objetivo (%1).\nNo se permite comparar por nombre.")
                                     .arg(targetSnapName),
                                 QStringLiteral("Could not resolve GUID for target snapshot (%1).\nName-based comparison is not allowed.")
                                     .arg(targetSnapName),
                                 QStringLiteral("无法获取目标快照（%1）的 GUID。\n不允许按名称比较。")
                                     .arg(targetSnapName)));
        return;
    }

    const QString dstLatestSnap = dstSnaps.first();
    const QString dstLatestGuid = snapshotGuidFor(dst.connIdx, dst.poolName, *dstCacheIt, dst.datasetName, dstLatestSnap);
    if (dstLatestGuid.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_guid_missing_dst_latest_001"),
                                 QStringLiteral("No se pudo obtener el GUID del snapshot más reciente en destino (%1).\nNo se permite comparar por nombre.")
                                     .arg(dstLatestSnap),
                                 QStringLiteral("Could not resolve GUID for target latest snapshot (%1).\nName-based comparison is not allowed.")
                                     .arg(dstLatestSnap),
                                 QStringLiteral("无法获取目标端最新快照（%1）的 GUID。\n不允许按名称比较。")
                                     .arg(dstLatestSnap)));
        return;
    }

    const QString srcBaseSnapByGuid =
        findSnapshotNameByGuid(*srcCacheIt, src.connIdx, src.poolName, src.datasetName, srcSnaps, dstLatestGuid);
    const int baseIdxInSrc = srcSnaps.indexOf(srcBaseSnapByGuid);
    if (baseIdxInSrc < 0 || srcBaseSnapByGuid.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_dst_abs01"),
                                 QStringLiteral("El snapshot más reciente de destino (guid=%1) no existe en origen.\nNo se puede nivelar con diferencial.")
                                     .arg(dstLatestGuid),
                                 QStringLiteral("Target latest snapshot (guid=%1) does not exist in source.\nCannot level with differential send.")
                                     .arg(dstLatestGuid),
                                 QStringLiteral("目标最新快照（guid=%1）在源端不存在，无法执行增量同步。")
                                     .arg(dstLatestGuid)));
        return;
    }
    if (baseIdxInSrc < targetIdxInSrc) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_level_dst_new01"),
                                 QStringLiteral("Destino tiene un snapshot más moderno (guid=%1) que el snapshot objetivo (guid=%2).\nNivelar cancelado.")
                                     .arg(dstLatestGuid, targetGuid),
                                 QStringLiteral("Target has a newer snapshot (guid=%1) than target snapshot to send (guid=%2).\nLevel canceled.")
                                     .arg(dstLatestGuid, targetGuid),
                                 QStringLiteral("目标端存在比要发送目标快照（guid=%2）更“新”的快照（guid=%1），已取消同步。")
                                     .arg(dstLatestGuid, targetGuid)));
        return;
    }
    if (baseIdxInSrc == targetIdxInSrc) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_level_already01"),
                                     QStringLiteral("Destino ya está nivelado en el snapshot objetivo (guid=%1).")
                                         .arg(targetGuid),
                                     QStringLiteral("Target is already leveled at target snapshot (guid=%1).")
                                         .arg(targetGuid),
                                     QStringLiteral("目标已处于目标快照（guid=%1），无需同步。")
                                         .arg(targetGuid)));
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
    const QString fromSnap = src.datasetName + QStringLiteral("@") + srcBaseSnapByGuid;
    const QString srcSnap = src.datasetName + QStringLiteral("@") + targetSnapName;
    const QString sendRawCmd = QStringLiteral("zfs send -wLecR -I %1 %2").arg(shSingleQuote(fromSnap), shSingleQuote(srcSnap));
    QString sendCmd = withSudo(sp, sendRawCmd);
    const QString recvTarget = dst.datasetName;

    const QString recvRawCmd = QStringLiteral("zfs recv -Fus %1").arg(shSingleQuote(recvTarget));
    QString recvCmd = withSudoStreamInput(dp, recvRawCmd);

    QString pipeline;
    const bool directRemoteToRemote =
        !sameConnection
        && !isLocalConnection(sp)
        && !isLocalConnection(dp)
        && sp.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0
        && dp.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0;
    auto buildDestViaSource = [&](const QString& remoteCmd) {
        const QString wrappedDst = wrapRemoteCommand(dp, remoteCmd);
        const QString target = shSingleQuote(sshUserHost(dp));
        if (!dp.password.trimmed().isEmpty()) {
            return QStringLiteral(
                       "if command -v sshpass >/dev/null 2>&1; then "
                       "SSHPASS=%1 sshpass -e %2 -o BatchMode=no "
                       "-o PreferredAuthentications=password,keyboard-interactive,publickey "
                       "-o NumberOfPasswordPrompts=1 %3 %4; "
                       "else echo %5 >&2; exit 127; fi")
                .arg(shSingleQuote(dp.password),
                     sshBaseCommand(dp),
                     target,
                     shSingleQuote(wrappedDst),
                     shSingleQuote(QStringLiteral("ZFSMgr: sshpass no disponible en el origen para conectar al destino")));
        }
        return sshBaseCommand(dp) + QStringLiteral(" ") + target + QStringLiteral(" ") + shSingleQuote(wrappedDst);
    };
    if (sameConnection) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_nivelar_mo_2edd21"),
                   QStringLiteral("Nivelar: modo local remoto (origen y destino en la misma conexión)"),
                   QStringLiteral("Level: remote-local mode (source and target on same connection)"),
                   QStringLiteral("同步快照：远端本地模式（源和目标在同一连接）")));
        const QString remotePipe = withSudo(sp, buildPipedTransferCommand(sendRawCmd, recvRawCmd));
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else if (directRemoteToRemote) {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Nivelar: modo remoto a remoto directo (%1 -> %2), sin pasar datos por el host local")
                   .arg(sp.name, dp.name));
        const QString remotePipe = buildPipedTransferCommand(sendCmd, buildDestViaSource(recvCmd));
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else {
        const QString srcSeg = isWindowsConnection(sp)
                                   ? buildDirectWindowsBashSshInvocation(sp, sendCmd)
                                   : sshExecFromLocal(sp, sendCmd);
        const QString dstSeg = isWindowsConnection(dp)
                                   ? buildDirectWindowsBashSshInvocation(dp, recvCmd)
                                   : sshExecFromLocal(dp, recvCmd);
        pipeline = buildPipedTransferCommand(srcSeg, dstSeg);
    }

    QString errorText;
    const QString displayLabel = QStringLiteral("Nivelar snapshot %1 -> %2").arg(srcSnap, recvTarget);
    if (!queuePendingShellAction(PendingShellActionDraft{
            pendingTransferScopeLabel(src, dst),
            displayLabel,
            pipeline,
            0,
            true,
            src,
            dst,
            PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Cambio pendiente añadido: %1  %2")
               .arg(pendingTransferScopeLabel(src, dst), displayLabel));
    updateApplyPropsButtonState();
}

void MainWindow::actionSyncDatasets() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    QString transferVersionReason;
    if (!isTransferVersionAllowed(src, dst, &transferVersionReason)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), transferVersionReason);
        appLog(QStringLiteral("WARN"), transferVersionReason);
        return;
    }
    ConnectionProfile sp = m_profiles[src.connIdx];
    ConnectionProfile dp = m_profiles[dst.connIdx];
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
    auto buildRsyncOptsProbeInline = [](bool includeDelete, bool dryRun) {
        QString opts = QStringLiteral("-aHWS");
        if (includeDelete) {
            opts += QStringLiteral(" --delete");
        }
        if (dryRun) {
            opts += QStringLiteral(" --dry-run");
        }
        return QStringLiteral(
            "RSYNC_PROGRESS='--info=progress2'; "
            "rsync --help 2>/dev/null | grep -q -- '--info' || RSYNC_PROGRESS='--progress'; "
            "RSYNC_OPTS='%1'; "
            "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
            "if rsync -X --version >/dev/null 2>&1; then "
            "  RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
            "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then "
            "  RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; "
            "fi")
            .arg(opts);
    };
    auto buildRsyncOptsProbe = [&](int connIdx,
                                   ConnectionProfile& profile,
                                   bool includeDelete,
                                   bool dryRun) {
        const bool remoteUnix = !isWindowsConnection(connIdx) && !isLocalConnection(profile);
        if (!remoteUnix) {
            return buildRsyncOptsProbeInline(includeDelete, dryRun);
        }
        (void)ensureRemoteScriptsUpToDate(profile);
        QStringList args;
        if (includeDelete) {
            args << QStringLiteral("--delete");
        }
        if (dryRun) {
            args << QStringLiteral("--dry-run");
        }
        const QString scriptCmd = remoteScriptCommand(profile, QStringLiteral("zfsmgr-rsync-opts"), args);
        return QStringLiteral("if %1 >/dev/null 2>&1; then eval \"$(%1)\"; else %2; fi")
            .arg(scriptCmd,
                 buildRsyncOptsProbeInline(includeDelete, dryRun));
    };
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
    auto queueSyncCommand = [&](const QString& displayLabel, const QString& command) {
        QString errorText;
        if (!queuePendingShellAction(PendingShellActionDraft{
                pendingTransferScopeLabel(src, dst),
                displayLabel,
                command,
                0,
                true,
                src,
                dst,
                PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
            return false;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Cambio pendiente añadido: %1  %2")
                   .arg(pendingTransferScopeLabel(src, dst), displayLabel));
        updateApplyPropsButtonState();
        return true;
    };
    auto showSyncOptionsDialog = [&](bool deleteSupported,
                                     const std::function<QString(bool, bool)>& buildCommand,
                                     bool* deleteOut) -> bool {
        if (!deleteOut) {
            return false;
        }
        *deleteOut = deleteSupported;
        QDialog dlg(this);
        dlg.setModal(true);
        dlg.resize(900, 560);
        dlg.setWindowTitle(trk(QStringLiteral("t_sync_opts_title_001"),
                               QStringLiteral("Opciones de sincronización"),
                               QStringLiteral("Synchronization options"),
                               QStringLiteral("同步选项")));
        auto* root = new QVBoxLayout(&dlg);
        auto* form = new QFormLayout();
        auto* srcEd = new QLineEdit(QStringLiteral("%1::%2")
                                        .arg((src.connIdx >= 0 && src.connIdx < m_profiles.size())
                                                 ? m_profiles[src.connIdx].name
                                                 : QStringLiteral("?"),
                                             src.datasetName),
                                    &dlg);
        srcEd->setReadOnly(true);
        auto* dstEd = new QLineEdit(QStringLiteral("%1::%2")
                                        .arg((dst.connIdx >= 0 && dst.connIdx < m_profiles.size())
                                                 ? m_profiles[dst.connIdx].name
                                                 : QStringLiteral("?"),
                                             dst.datasetName),
                                    &dlg);
        dstEd->setReadOnly(true);
        auto* deleteCb = new QCheckBox(QStringLiteral("--delete"), &dlg);
        deleteCb->setChecked(deleteSupported);
        deleteCb->setEnabled(deleteSupported);
        if (!deleteSupported) {
            deleteCb->setToolTip(trk(QStringLiteral("t_sync_delete_disabled_tar_001"),
                                     QStringLiteral("No disponible con modo tar."),
                                     QStringLiteral("Not available with tar mode."),
                                     QStringLiteral("tar 模式下不可用。")));
        }
        form->addRow(trk(QStringLiteral("t_origin_001"), QStringLiteral("Origen"), QStringLiteral("Source"), QStringLiteral("源")), srcEd);
        form->addRow(trk(QStringLiteral("t_dest_001"), QStringLiteral("Destino"), QStringLiteral("Target"), QStringLiteral("目标")), dstEd);
        form->addRow(trk(QStringLiteral("t_sync_opts_params_001"),
                         QStringLiteral("Parámetros"),
                         QStringLiteral("Parameters"),
                         QStringLiteral("参数")),
                     deleteCb);
        root->addLayout(form);

        auto* checkRow = new QHBoxLayout();
        auto* checkBtn = new QPushButton(trk(QStringLiteral("t_sync_check_001"),
                                             QStringLiteral("Check"),
                                             QStringLiteral("Check"),
                                             QStringLiteral("检查")),
                                         &dlg);
        auto* cancelCheckBtn = new QPushButton(
            trk(QStringLiteral("t_sync_cancel_check_001"),
                QStringLiteral("Cancel check"),
                QStringLiteral("Cancel check"),
                QStringLiteral("取消检查")),
            &dlg);
        cancelCheckBtn->setEnabled(false);
        checkRow->addWidget(checkBtn);
        checkRow->addWidget(cancelCheckBtn);
        checkRow->addStretch(1);
        root->addLayout(checkRow);

        auto* outBox = new QPlainTextEdit(&dlg);
        outBox->setReadOnly(true);
        outBox->setPlaceholderText(trk(QStringLiteral("t_sync_check_output_ph_001"),
                                       QStringLiteral("Salida de dry-run..."),
                                       QStringLiteral("Dry-run output..."),
                                       QStringLiteral("dry-run 输出...")));
        root->addWidget(outBox, 1);

        bool checkRunning = false;
        bool cancelRequested = false;
        QObject::connect(cancelCheckBtn, &QPushButton::clicked, &dlg, [&]() {
            cancelRequested = true;
        });
        QObject::connect(checkBtn, &QPushButton::clicked, &dlg, [&]() {
            if (checkRunning) {
                return;
            }
            const QString cmd = buildCommand(deleteCb->isChecked(), true);
            if (cmd.trimmed().isEmpty()) {
                outBox->setPlainText(
                    trk(QStringLiteral("t_sync_dryrun_na_001"),
                        QStringLiteral("Dry-run no disponible para este modo de sincronización."),
                        QStringLiteral("Dry-run is not available for this synchronization mode."),
                        QStringLiteral("此同步模式不支持 dry-run。")));
                return;
            }
            checkRunning = true;
            cancelRequested = false;
            checkBtn->setEnabled(false);
            cancelCheckBtn->setEnabled(true);
            deleteCb->setEnabled(false);
            outBox->setPlainText(trk(QStringLiteral("t_sync_dryrun_running_001"),
                                     QStringLiteral("Ejecutando dry-run..."),
                                     QStringLiteral("Running dry-run..."),
                                     QStringLiteral("正在执行 dry-run...")));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QApplication::setOverrideCursor(Qt::WaitCursor);

            QString out;
            QString err;
            int rc = -1;
            bool started = false;
            bool timedOut = false;
            bool cancelled = false;
            QProcess proc;
            proc.start(QStringLiteral("sh"), QStringList{QStringLiteral("-c"), cmd});
            started = proc.waitForStarted(4000);
            if (!started) {
                err = QStringLiteral("Could not start local shell.");
            } else {
                QElapsedTimer timer;
                timer.start();
                while (proc.state() != QProcess::NotRunning) {
                    proc.waitForReadyRead(120);
                    out += QString::fromUtf8(proc.readAllStandardOutput());
                    err += QString::fromUtf8(proc.readAllStandardError());
                    QString live;
                    live += QStringLiteral("$ %1\n\n").arg(maskSecrets(cmd));
                    live += trk(QStringLiteral("t_sync_dryrun_running_002"),
                                QStringLiteral("Ejecutando dry-run..."),
                                QStringLiteral("Running dry-run..."),
                                QStringLiteral("正在执行 dry-run..."));
                    if (!out.trimmed().isEmpty()) {
                        live += QStringLiteral("\n\nstdout:\n") + maskSecrets(out.trimmed());
                    }
                    if (!err.trimmed().isEmpty()) {
                        live += QStringLiteral("\n\nstderr:\n") + maskSecrets(err.trimmed());
                    }
                    outBox->setPlainText(live);
                    outBox->moveCursor(QTextCursor::End);
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    if (cancelRequested) {
                        cancelled = true;
                        proc.kill();
                        proc.waitForFinished(1000);
                        break;
                    }
                    if (timer.elapsed() > 120000) {
                        timedOut = true;
                        proc.kill();
                        proc.waitForFinished(1000);
                        break;
                    }
                }
                out += QString::fromUtf8(proc.readAllStandardOutput());
                err += QString::fromUtf8(proc.readAllStandardError());
                rc = proc.exitCode();
            }
            QApplication::restoreOverrideCursor();
            checkRunning = false;
            checkBtn->setEnabled(true);
            cancelCheckBtn->setEnabled(false);
            deleteCb->setEnabled(deleteSupported);

            QString rendered;
            rendered += QStringLiteral("$ %1\n\n").arg(maskSecrets(cmd));
            if (!started) {
                rendered += trk(QStringLiteral("t_sync_dryrun_start_fail_001"),
                                QStringLiteral("No se pudo ejecutar dry-run."),
                                QStringLiteral("Could not execute dry-run."),
                                QStringLiteral("无法执行 dry-run。"));
                if (!err.trimmed().isEmpty()) {
                    rendered += QStringLiteral("\n\nstderr:\n") + maskSecrets(err.trimmed());
                }
                outBox->setPlainText(rendered);
                return;
            }
            if (cancelled) {
                rendered += trk(QStringLiteral("t_sync_dryrun_cancelled_001"),
                                QStringLiteral("Dry-run cancelado por el usuario."),
                                QStringLiteral("Dry-run cancelled by user."),
                                QStringLiteral("dry-run 已由用户取消。"));
            } else if (timedOut) {
                rendered += trk(QStringLiteral("t_sync_dryrun_timeout_001"),
                                QStringLiteral("Dry-run cancelado por timeout."),
                                QStringLiteral("Dry-run cancelled due to timeout."),
                                QStringLiteral("dry-run 因超时而取消。"));
            } else {
                rendered += QStringLiteral("exit=%1").arg(rc);
            }
            if (!out.trimmed().isEmpty()) {
                rendered += QStringLiteral("\n\nstdout:\n") + maskSecrets(out.trimmed());
            }
            if (!err.trimmed().isEmpty()) {
                rendered += QStringLiteral("\n\nstderr:\n") + maskSecrets(err.trimmed());
            }
            outBox->setPlainText(rendered);
        });

        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        root->addWidget(bb);
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        *deleteOut = deleteCb->isChecked();
        return true;
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
            auto buildTarSyncCommand = [&](bool /*useDelete*/, bool dryRun) -> QString {
                if (dryRun) {
                    return QString();
                }
                const QString srcTarCmd = buildTarSourceCommand(isWindowsConnection(sp), srcEffectiveMp, codec);
                const QString dstTarCmd = buildTarDestinationCommand(isWindowsConnection(dp), dstEffectiveMp, codec);
                if (sameConnection) {
                    const QString remotePipe = buildPipedTransferCommand(srcTarCmd, dstTarCmd);
                    return sshExecFromLocal(sp, remotePipe);
                }
                return buildPipedTransferCommand(sshExecFromLocal(sp, srcTarCmd),
                                                 sshExecFromLocal(dp, dstTarCmd));
            };
            bool useDelete = false;
            if (!showSyncOptionsDialog(false, buildTarSyncCommand, &useDelete)) {
                return;
            }
            const QString command = buildTarSyncCommand(false, false);
            appLog(QStringLiteral("WARN"),
                   trk(QStringLiteral("t_sincroniza_6ccd2e"),
                       QStringLiteral("Sincronizar en Windows usa fallback tar/ssh (codec=%1, sin --delete)."),
                       QStringLiteral("Sync on Windows uses tar/ssh fallback (codec=%1, no --delete)."),
                       QStringLiteral("Windows 上同步使用 tar/ssh 回退（编码=%1，无 --delete）。")).arg(streamCodecName(codec)));
            queueSyncCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command);
            return;
        }
        auto buildRsyncSyncCommand = [&](bool useDelete, bool dryRun) -> QString {
            const QString rsyncOptsProbe = buildRsyncOptsProbe(src.connIdx, sp, useDelete, dryRun);
            QString srcMpCmd;
            if (!isWindowsConnection(src.connIdx) && !isLocalConnection(sp)) {
                (void)ensureRemoteScriptsUpToDate(sp);
                srcMpCmd = remoteScriptCommand(sp, QStringLiteral("zfsmgr-zfs-mountpoint"), {src.datasetName});
            } else {
                srcMpCmd = mwhelpers::withUnixSearchPathCommand(
                    QStringLiteral("zfs get -H -o value mountpoint %1")
                        .arg(shSingleQuote(src.datasetName)));
            }
            QString dstMpCmd;
            if (!isWindowsConnection(dst.connIdx) && !isLocalConnection(dp)) {
                (void)ensureRemoteScriptsUpToDate(dp);
                dstMpCmd = remoteScriptCommand(dp, QStringLiteral("zfsmgr-zfs-mountpoint"), {dst.datasetName});
            } else {
                dstMpCmd = mwhelpers::withUnixSearchPathCommand(
                    QStringLiteral("zfs get -H -o value mountpoint %1")
                        .arg(shSingleQuote(dst.datasetName)));
            }
            if (sameConnection) {
                QString remoteRsync =
                    QStringLiteral("%1; SRC_MP=$(%2); DST_MP=$(%3); rsync $RSYNC_OPTS $RSYNC_PROGRESS \"$SRC_MP\"/ \"$DST_MP\"/")
                        .arg(rsyncOptsProbe,
                             srcMpCmd,
                             dstMpCmd);
                remoteRsync = withSudo(sp, remoteRsync);
                return srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
            }
            const QString dstRemoteMpCmd = withSudo(dp, dstMpCmd);
            const QString dstRemoteProbe =
                QStringLiteral("%1 %2 %3")
                    .arg(sshBaseCommand(dp),
                         shSingleQuote(sshUserHost(dp)),
                         shSingleQuote(dstRemoteMpCmd));
            QString remoteRsync =
                QStringLiteral("%1; SRC_MP=$(%2); DST_MP=$(%3); rsync $RSYNC_OPTS $RSYNC_PROGRESS -e %4 \"$SRC_MP\"/ %5:\"$DST_MP\"/")
                    .arg(rsyncOptsProbe,
                         srcMpCmd,
                         dstRemoteProbe,
                         shSingleQuote(sshBaseCommand(dp)),
                         shSingleQuote(sshUserHost(dp)));
            remoteRsync = withSudo(sp, remoteRsync);
            return srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        };
        bool useDelete = true;
        if (!showSyncOptionsDialog(true, buildRsyncSyncCommand, &useDelete)) {
            return;
        }
        if (sameConnection) {
            appLog(QStringLiteral("INFO"),
                   trk(QStringLiteral("t_sincroniza_d0a688"),
                       QStringLiteral("Sincronizar: modo local remoto (rsync, misma conexión)"),
                       QStringLiteral("Sync: remote-local mode (rsync, same connection)"),
                       QStringLiteral("同步：远端本地模式（rsync，同一连接）")));
        }
        const QString command = buildRsyncSyncCommand(useDelete, false);
        queueSyncCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command);
        return;
    }

    const bool canTempMountSrc = supportsAlternateDatasetMount(src.connIdx);
    const bool canTempMountDst = supportsAlternateDatasetMount(dst.connIdx);
    if ((!srcRootMounted && !srcCanmountOff) || (!dstRootMounted && !dstCanmountOff)) {
        if (!isWindowsConnection(sp) && !isWindowsConnection(dp) && canTempMountSrc && canTempMountDst) {
            const mwhelpers::StreamCodec codec = chooseCodec();
            auto codecArg = [&](mwhelpers::StreamCodec c) -> QString {
                if (c == mwhelpers::StreamCodec::Zstd) {
                    return QStringLiteral("zstd");
                }
                if (c == mwhelpers::StreamCodec::Gzip) {
                    return QStringLiteral("gzip");
                }
                return QStringLiteral("none");
            };
            auto buildTempTarCommand = [&](bool /*useDelete*/, bool dryRun) -> QString {
                if (dryRun) {
                    return QString();
                }
                QString srcScript;
                QString dstScript;
                const bool srcRemoteUnix = !isWindowsConnection(sp) && !isLocalConnection(sp);
                const bool dstRemoteUnix = !isWindowsConnection(dp) && !isLocalConnection(dp);
                if (srcRemoteUnix) {
                    (void)ensureRemoteScriptsUpToDate(sp);
                    srcScript = remoteScriptCommand(
                        sp,
                        QStringLiteral("zfsmgr-sync-temp-tar-source"),
                        {src.datasetName, codecArg(codec)});
                } else {
                    srcScript = buildUnixTemporaryTarSourceScript(src.datasetName, codec);
                }
                if (dstRemoteUnix) {
                    (void)ensureRemoteScriptsUpToDate(dp);
                    dstScript = remoteScriptCommand(
                        dp,
                        QStringLiteral("zfsmgr-sync-temp-tar-dest"),
                        {dst.datasetName, codecArg(codec)});
                } else {
                    dstScript = buildUnixTemporaryTarDestinationScript(dst.datasetName, codec);
                }
                if (sameConnection) {
                    const QString remotePipe =
                        buildPipedTransferCommand(withSudo(sp, srcScript), withSudoStreamInput(dp, dstScript));
                    return sshExecFromLocal(sp, remotePipe);
                }
                return buildPipedTransferCommand(sshExecFromLocal(sp, withSudo(sp, srcScript)),
                                                 sshExecFromLocal(dp, withSudoStreamInput(dp, dstScript)));
            };
            bool useDelete = false;
            if (!showSyncOptionsDialog(false, buildTempTarCommand, &useDelete)) {
                return;
            }
            QString command = buildTempTarCommand(false, false);
            if (sameConnection) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("Sincronizar: usando montaje temporal alternativo (%1)")
                           .arg(streamCodecName(codec)));
            }
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Sincronizar: fallback tar/ssh con montajes temporales alternativos (codec=%1, sin --delete)")
                       .arg(streamCodecName(codec)));
            queueSyncCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command);
            return;
        }
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_origen_y_d_79e6b3"),
                                 QStringLiteral("Origen y destino deben estar montados para sincronizar,\no bien tener canmount=off con subdatasets montados.\n"
                                                "En Linux, macOS y FreeBSD también puede usarse montaje temporal alternativo.\n"
                                                "Origen mounted=%1 canmount=%2\nDestino mounted=%3 canmount=%4"),
                                 QStringLiteral("Source and target must be mounted to synchronize,\nor canmount=off with mounted subdatasets.\n"
                                                "Linux, macOS and FreeBSD can also use a temporary alternate mount.\n"
                                                "Source mounted=%1 canmount=%2\nTarget mounted=%3 canmount=%4"),
                                 QStringLiteral("源和目标必须已挂载才能同步，\n或设置 canmount=off 且子数据集已挂载。\n"
                                                "在 Linux、macOS 和 FreeBSD 上也可使用临时替代挂载。\n"
                                                "源 mounted=%1 canmount=%2\n目标 mounted=%3 canmount=%4"))
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
        auto buildSubdatasetTarCommand = [&](bool /*useDelete*/, bool dryRun) -> QString {
            if (dryRun) {
                return QString();
            }
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
            return tarPipelines.join(QStringLiteral(" && "));
        };
        bool useDelete = false;
        if (!showSyncOptionsDialog(false, buildSubdatasetTarCommand, &useDelete)) {
            return;
        }
        const QString command = buildSubdatasetTarCommand(false, false);
        appLog(QStringLiteral("WARN"),
               trk(QStringLiteral("t_sincroniza_86c64e"),
                   QStringLiteral("Sincronizar subdatasets en Windows usa fallback tar/ssh (codec=%1, sin --delete)."),
                   QStringLiteral("Subdataset sync on Windows uses tar/ssh fallback (codec=%1, no --delete)."),
                   QStringLiteral("Windows 上子数据集同步使用 tar/ssh 回退（编码=%1，无 --delete）。")).arg(streamCodecName(codec)));
        queueSyncCommand(QStringLiteral("Sincronizar subdatasets %1 -> %2 (%3)")
                             .arg(src.datasetName, dst.datasetName)
                             .arg(syncPairs.size()),
                         command);
    } else {
        auto buildSubdatasetRsyncCommand = [&](bool useDelete, bool dryRun) -> QString {
            QStringList cmds;
            cmds.reserve(syncPairs.size());
            for (const auto& pair : syncPairs) {
                if (sameConnection) {
                    cmds << QStringLiteral("rsync $RSYNC_OPTS $RSYNC_PROGRESS %1/ %2/")
                                .arg(shSingleQuote(pair.first), shSingleQuote(pair.second));
                } else {
                    cmds << QStringLiteral("rsync $RSYNC_OPTS $RSYNC_PROGRESS -e %1 %2/ %3:%4/")
                                .arg(shSingleQuote(sshTransport),
                                     shSingleQuote(pair.first),
                                     shSingleQuote(sshUserHost(dp)),
                                     shSingleQuote(pair.second));
                }
            }
            QString remoteRsync = QStringLiteral("set -e; %1; %2")
                                      .arg(buildRsyncOptsProbe(src.connIdx, sp, useDelete, dryRun),
                                           cmds.join(QStringLiteral(" && ")));
            remoteRsync = withSudo(sp, remoteRsync);
            return srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        };
        bool useDelete = true;
        if (!showSyncOptionsDialog(true, buildSubdatasetRsyncCommand, &useDelete)) {
            return;
        }
        const QString command = buildSubdatasetRsyncCommand(useDelete, false);
        queueSyncCommand(QStringLiteral("Sincronizar subdatasets %1 -> %2 (%3)")
                             .arg(src.datasetName, dst.datasetName)
                             .arg(syncPairs.size()),
                         command);
    }
}
