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
