#include "mainwindow.h"

#include "agentmsg.h"

#include "transfer.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "daemonpayload.h"
#include "agentversion.h"

#include <QCheckBox>
#include <QLabel>
#include <QFrame>
#include <QGroupBox>
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
#include <QUuid>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QListWidget>
#include <QTimer>
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

} // namespace

bool MainWindow::runShellActionNow(const PendingShellActionDraft& draft, QString* errorOut) {
    // Antes esto ENCOLABA. Ahora ejecuta.
    //
    // La lista de cambios pendientes se retira: una acción de menú se hace al pulsarla, como
    // en cualquier otro sitio. Lo que se va con ella no es solo la espera, son tres cosas que
    // solo existían por diferir:
    //
    //   - La advertencia de «esta orden la construyó una versión anterior»: una orden que
    //     esperaba días en disco podía ser más vieja que el arreglo del código que la
    //     construye. Ejecutando al instante, quien construye y quien ejecuta son el mismo
    //     binario, siempre.
    //   - La casilla «Activa» y la reactivación de una entrada ya ejecutada: pedir dos veces
    //     lo mismo es, sencillamente, hacerlo dos veces.
    //   - El rechazo por duplicado, que obligaba a ir a la lista a quitar la entrada antes
    //     de poder repetir la acción.
    //
    // El registro del rechazo se queda AQUÍ y no en los llamantes: son veinte y ninguno lo
    // hacía, así que un fallo salía en un diálogo y el log no decía nada.
    auto fail = [this, errorOut, &draft](const QString& text) {
        if (errorOut) {
            *errorOut = text;
        }
        appLog(QStringLiteral("WARN"),
               QStringLiteral("[accion] no se ejecutó «%1»: %2")
                   .arg(draft.displayLabel.trimmed().isEmpty() ? draft.command.trimmed()
                                                               : draft.displayLabel.trimmed(),
                        text));
        return false;
    };
    if (draft.command.trimmed().isEmpty() && draft.datasetActionName.trimmed().isEmpty()
        && draft.rpcArgv.isEmpty()) {
        return fail(QStringLiteral("No hay nada que ejecutar."));
    }
    if (!executeShellActionDraft(draft)) {
        // Cancelar no es fallar: quien cancela ya sabe lo que ha hecho y un diálogo de error
        // detrás asusta sin motivo.
        if (m_lastActionWasCancelled) {
            m_lastActionWasCancelled = false;
            if (errorOut) {
                errorOut->clear();
            }
            return false;
        }
        return fail(QStringLiteral("La acción falló; revise el registro."));
    }
    return true;
}

// La transfer entre máquinas encadena "zfs send | zfs recv" por SSH, y cuando eso no
// aplica cae a un TAR con ACLs y atributos extendidos. Las dos son tuberías de shell Unix.
// En Windows se ejecutaban a través del bash de MSYS2, que se ha retirado: la aplicación
// trabaja allí solo con el agente nativo, y el agente aún no implementa el streaming.
//
// El texto vive aquí, en un solo sitio, porque lo dicen DOS: el menú al deshabilitar la
// entrada y esta comprobación al ejecutar. Si divergieran, el usuario leería dos motivos
// distintos para el mismo impedimento.
QString MainWindow::streamingUnavailableReason(const QString& actionLabel) const {
    return trk(QStringLiteral("t_win_stream_na001"),
               QStringLiteral("%1 entre máquinas no está disponible cuando algún extremo es Windows: "
                              "necesita transmitir el flujo por una tubería, y el agente de Windows "
                              "todavía no lo implementa."),
               QStringLiteral("%1 between machines is not available when either end is Windows: it needs "
                              "to stream through a pipe, and the Windows agent does not implement that yet."))
        .arg(actionLabel);
}

bool MainWindow::requireNonWindowsStreamingEndpoints(int srcConnIdx,
                                                     int dstConnIdx,
                                                     const QString& actionLabel) {
    if (featureAvailable(srcConnIdx, zfsmgr::caps::Feature::SendRecvStreaming)
        && featureAvailable(dstConnIdx, zfsmgr::caps::Feature::SendRecvStreaming)) {
        return true;
    }
    const QString reason = streamingUnavailableReason(actionLabel);
    appLog(QStringLiteral("WARN"), reason);
    QMessageBox::warning(this, QStringLiteral("ZFSMgr"), reason);
    return false;
}

void MainWindow::actionSendSnapshot() {
    cliSend(QString());
}

void MainWindow::cliSend(const QString& extra) {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    // «Send here»: the clicked dataset receives a child with the source's name —the rule of
    // base/transfer—, and the shell's `send` does the rest as a job of the daemons.
    const QString recibe = QString::fromStdString(zfsmgr::base::transfer::actualDestination(
        src.datasetName.toStdString(), dst.datasetName.toStdString()));
    QString linea = QStringLiteral("send %1 --on %2")
                        .arg(zfsmUrl(dst.connIdx, recibe),
                             zfsmUrl(src.connIdx, src.datasetName, src.snapshotName));
    if (!extra.isEmpty()) {
        linea += QLatin1Char(' ') + extra;
    }
    runCli(linea, [this](const CliService::Result& r) {
        if (r.rc == 0 && m_refreshAfterCli) {
            refreshAllJobsAsync();
        }
    });
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

    QStringList banderas;
    if (pChk->isChecked()) {
        banderas << QStringLiteral("-p");
    }
    if (uChk->isChecked()) {
        banderas << QStringLiteral("-u");
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
        banderas << QStringLiteral("-o") << prop;
    }
    // El argv y sus dos comprobaciones —origen instantánea, destino no— salen de
    // `commands::snapshots`. Aquí no había ninguna: se mandaba a ZFS y se leía su queja,
    // que además habla de otra cosa cuando el origen no es una instantánea.
    // The shell's `clone`: the name, then its flags and properties.
    QStringList linea{QStringLiteral("clone"), cliQuote(targetDataset)};
    for (int n = 0; n < banderas.size(); ++n) {
        if (banderas[n] == QStringLiteral("-o") && n + 1 < banderas.size()) {
            linea << cliQuote(banderas[++n]);
        } else {
            linea << banderas[n];
        }
    }
    linea << QStringLiteral("--on") << zfsmUrl(src.connIdx, src.datasetName, src.snapshotName);
    if (runCliAndWait(linea.join(QLatin1Char(' '))).rc == 0 && m_refreshAfterCli) {
        refreshAfterCli(dst);
    }
}

void MainWindow::actionDiffSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    // From the marked snapshot to the clicked one (or the dataset as it is now).
    cliShowOutput(QStringLiteral("diff %1 --from %2")
                      .arg(zfsmUrl(dst.connIdx, dst.datasetName, dst.snapshotName),
                           zfsmUrl(src.connIdx, src.datasetName, src.snapshotName)),
                  QStringLiteral("diff"));
}

void MainWindow::actionLevelSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty()
        || !dst.snapshotName.isEmpty()) {
        return;
    }
    // Up to the marked snapshot, or the newest of the source; the base is the shell's to find.
    QString hasta = src.snapshotName;
    if (hasta.isEmpty()) {
        const QStringList snaps =
            datasetSnapshotsFromModel(src.connIdx, src.poolName, src.datasetName);
        if (snaps.isEmpty()) {
            return;
        }
        hasta = snaps.first();
    }
    runCli(QStringLiteral("send %1 --on %2 --level")
               .arg(zfsmUrl(dst.connIdx, dst.datasetName),
                    zfsmUrl(src.connIdx, src.datasetName, hasta)),
           [this](const CliService::Result& r) {
               if (r.rc == 0 && m_refreshAfterCli) {
                   refreshAllJobsAsync();
               }
           });
}

void MainWindow::actionSyncDatasets() {
    cliSync(QString());
}

void MainWindow::cliSync(const QString& extra) {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    QString linea =
        QStringLiteral("rsync %1 --on %2")
            .arg(zfsmUrl(dst.connIdx, dst.datasetName), zfsmUrl(src.connIdx, src.datasetName));
    if (!extra.isEmpty()) {
        linea += QLatin1Char(' ') + extra;
    }
    runCli(linea, [this](const CliService::Result& r) {
        if (r.rc == 0 && m_refreshAfterCli) {
            refreshAllJobsAsync();
        }
    });
}

// ── Daemon background job support ────────────────────────────────────────────

void MainWindow::pollDaemonJobs() {
    bool anyRunning = false;
    // Indexed, not a range-for: the RPC below yields, and an append() to
    // m_activeDaemonJobs would reallocate the QList, invalidating both a held
    // reference and the range-for's iterator.
    for (int i = 0; i < m_activeDaemonJobs.size(); ++i) {
        if (m_activeDaemonJobs[i].state != QStringLiteral("running")) continue;
        anyRunning = true;
        const QString pollJobId = m_activeDaemonJobs[i].jobId;
        const int srcConnIdx = m_activeDaemonJobs[i].srcConnIdx;
        if (srcConnIdx < 0 || srcConnIdx >= m_conns.profiles.size()) continue;
        const ConnectionProfile sp = m_conns.profiles[srcConnIdx];
        QStringList args;
        args << mwhelpers::argvQt(
            zfsmgr::commands::requests::jobStatus(pollJobId.toStdString()));
        QString out, err;
        int rc = -1;
        if (!runAgentOnConnection(sp, args, 5000, out, err, rc) || rc != 0) continue;

        // Re-acquire after the yield, and confirm the slot still holds the same job.
        if (i >= m_activeDaemonJobs.size() || m_activeDaemonJobs[i].jobId != pollJobId) continue;
        ActiveDaemonJob& job = m_activeDaemonJobs[i];

        QString errorCode;
        QString errorArgs;
        for (const QString& line : out.split('\n')) {
            if (line.startsWith(QStringLiteral("STATE=")))
                job.state = line.mid(6).trimmed();
            else if (line.startsWith(QStringLiteral("BYTES=")))
                job.bytesTransferred = line.mid(6).trimmed().toULongLong();
            else if (line.startsWith(QStringLiteral("RATE_MIB_S=")))
                job.rateMiBs = line.mid(11).trimmed().toDouble();
            else if (line.startsWith(QStringLiteral("ELAPSED_SECS=")))
                job.elapsedSecs = line.mid(13).trimmed().toLong();
            else if (line.startsWith(QStringLiteral("PROGRESS_LINE=")))
                job.lastProgressLine = line.mid(14).trimmed();
            else if (line.startsWith(QStringLiteral("ERROR_CODE=")))
                errorCode = line.mid(11).trimmed();
            else if (line.startsWith(QStringLiteral("ERROR_ARGS=")))
                errorArgs = line.mid(11).trimmed();
        }
        // Why it failed comes as a code and its arguments; it is worded here.
        if (!errorCode.isEmpty()) {
            job.errorText = QString::fromStdString(zfsmgr::base::agentmsg::renderJsonArgs(
                errorCode.toStdString(), errorArgs.toStdString()));
        }

        if (job.state == QStringLiteral("done")) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Job %1 completado: %2").arg(job.jobId, job.displayLabel));
            // Trigger dataset tree refresh on destination
            if (job.dstConnIdx >= 0 && job.dstConnIdx < m_conns.profiles.size()) {
                QTimer::singleShot(0, this, [this, dstIdx = job.dstConnIdx]() {
                    refreshConnectionByIndex(dstIdx);
                });
            }
        } else if (job.state == QStringLiteral("failed")) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Job %1 falló: %2 — %3")
                       .arg(job.jobId, job.displayLabel, job.errorText));
        }
    }

    updateJobsListWidget();
    if (!anyRunning && m_jobPollTimer) m_jobPollTimer->stop();
}

void MainWindow::scanOrphanedJobsForConnection(int connIdx) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) return;
    const ConnectionProfile sp = m_conns.profiles[connIdx];
    QStringList args;
    args << mwhelpers::argvQt(zfsmgr::commands::requests::jobList());
    QString out, err;
    int rc = -1;
    if (!runAgentOnConnection(sp, args, 8000, out, err, rc) || rc != 0) return;

    for (const QString& line : out.split('\n')) {
        if (!line.startsWith(QStringLiteral("JOB="))) continue;
        const QString json = line.mid(4);
        // Extract id and state from inline JSON
        auto extractStr = [&](const QString& key) -> QString {
            const QString pat = QStringLiteral("\"%1\":\"").arg(key);
            const int p = json.indexOf(pat);
            if (p < 0) return {};
            const int start = p + pat.size();
            const int end = json.indexOf('"', start);
            return end > start ? json.mid(start, end - start) : QString();
        };
        const QString jobId = extractStr(QStringLiteral("id"));
        const QString state = extractStr(QStringLiteral("state"));
        if (jobId.isEmpty() || state != QStringLiteral("running")) continue;

        // Already tracked?
        bool known = false;
        for (const ActiveDaemonJob& j : m_activeDaemonJobs) {
            if (j.jobId == jobId) { known = true; break; }
        }
        if (known) continue;

        ActiveDaemonJob job;
        job.srcConnIdx   = connIdx;
        job.dstConnIdx   = connIdx;
        job.jobId        = jobId;
        job.state        = state;
        job.displayLabel = extractStr(QStringLiteral("snap"));
        m_activeDaemonJobs.append(job);
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Job activo recuperado tras reconexión: %1 en %2")
                   .arg(jobId, sp.name));
    }

    if (!m_activeDaemonJobs.isEmpty()) {
        updateJobsListWidget();
        if (m_jobPollTimer && !m_jobPollTimer->isActive()) m_jobPollTimer->start();
    }
}
