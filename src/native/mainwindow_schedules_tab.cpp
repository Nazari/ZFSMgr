#include "mainwindow.h"

#include "commands/gsa.h"
#include "commands/peers.h"
#include "commands/requests.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <ctime>

// The Schedules tab of a dataset: what `schedule status` reports about it and what `schedule`
// sets on it. The rules are the core's (gsa::isValid, isValidSet, targetForScheduleMachine),
// the same the shell applies; the tab only gathers the values and shows the answer.

namespace G = zfsmgr::base::gsa;

namespace {

const char* const kClases[5] = {"hourly", "daily", "weekly", "monthly", "yearly"};

QString fecha(long long t) {
    if (t <= 0) {
        return QStringLiteral("-");
    }
    return QDateTime::fromSecsSinceEpoch(t).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

void ponFila(QTreeWidget* tabla, const QString& campo, const QString& valor) {
    auto* item = new QTreeWidgetItem(tabla);
    item->setText(0, campo);
    item->setText(1, valor);
    item->setToolTip(1, valor);
}

}  // namespace

QWidget* MainWindow::buildPaneSchedulesTab(int paneIdx, QWidget* parent) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const bool isOrigin = (paneIdx == 0);
    auto* page = new QWidget(parent);
    page->setObjectName(isOrigin ? QStringLiteral("originSchedulesTab")
                                 : QStringLiteral("destinationSchedulesTab"));
    auto* layout = new QHBoxLayout(page);

    pane.schedStatus = new QTreeWidget(page);
    pane.schedStatus->setRootIsDecorated(false);
    pane.schedStatus->setHeaderLabels({trk(QStringLiteral("t_sched_col_field_001"),
                                           QStringLiteral("Campo"), QStringLiteral("Field")),
                                       trk(QStringLiteral("t_sched_col_value_001"),
                                           QStringLiteral("Valor"), QStringLiteral("Value"))});
    layout->addWidget(pane.schedStatus, 3);

    auto* formBox = new QWidget(page);
    auto* form = new QFormLayout(formBox);
    pane.schedEnabled = new QCheckBox(formBox);
    pane.schedRecursive = new QCheckBox(formBox);
    pane.schedLevel = new QCheckBox(formBox);
    form->addRow(trk(QStringLiteral("t_sched_enabled_001"), QStringLiteral("Activa"),
                     QStringLiteral("Enabled")),
                 pane.schedEnabled);
    form->addRow(trk(QStringLiteral("t_sched_recursive_001"), QStringLiteral("Recursiva"),
                     QStringLiteral("Recursive")),
                 pane.schedRecursive);
    for (int i = 0; i < 5; ++i) {
        pane.schedRetention[i] = new QSpinBox(formBox);
        pane.schedRetention[i]->setRange(0, 9999);
        form->addRow(QString::fromLatin1(kClases[i]), pane.schedRetention[i]);
    }
    form->addRow(trk(QStringLiteral("t_sched_level_001"), QStringLiteral("Nivelar"),
                     QStringLiteral("Level")),
                 pane.schedLevel);
    pane.schedTarget = new QLineEdit(formBox);
    pane.schedTarget->setPlaceholderText(QStringLiteral("zfsm://connection/pool/dataset"));
    form->addRow(trk(QStringLiteral("t_sched_target_001"), QStringLiteral("Destino"),
                     QStringLiteral("Target")),
                 pane.schedTarget);

    auto* botones = new QHBoxLayout();
    auto* aplicar = new QPushButton(trk(QStringLiteral("t_sched_apply_001"),
                                        QStringLiteral("Aplicar"), QStringLiteral("Apply")),
                                    formBox);
    auto* borrar = new QPushButton(
        trk(QStringLiteral("t_sched_clear_001"), QStringLiteral("Borrar programación"),
            QStringLiteral("Clear schedule")),
        formBox);
    auto* ahora = new QPushButton(trk(QStringLiteral("t_sched_run_001"),
                                      QStringLiteral("Ejecutar ahora"), QStringLiteral("Run now")),
                                  formBox);
    connect(aplicar, &QPushButton::clicked, this,
            [this, paneIdx]() { applyPaneSchedule(paneIdx); });
    connect(borrar, &QPushButton::clicked, this, [this, paneIdx]() { clearPaneSchedule(paneIdx); });
    connect(ahora, &QPushButton::clicked, this, [this, paneIdx]() { runPaneScheduleNow(paneIdx); });
    botones->addWidget(aplicar);
    botones->addWidget(borrar);
    botones->addWidget(ahora);
    form->addRow(botones);
    layout->addWidget(formBox, 2);
    pane.schedulesTab = page;
    return page;
}

// The form from the dataset's own (not inherited) schedule properties, already loaded for the
// Properties tab; the status is asked when the tab is opened (loadPaneScheduleStatusAsync).
void MainWindow::fillPaneSchedules(int paneIdx, int connIdx, const QString& poolName,
                                   const QString& dataset) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (!pane.schedulesTab) {
        return;
    }
    pane.schedConnIdx = connIdx;
    pane.schedPool = poolName;
    pane.schedDataset = dataset;
    std::map<std::string, std::string> props;
    for (const DatasetPropCacheRow& row :
         datasetPropertyRowsFromModelOrCache(connIdx, poolName, dataset)) {
        if (row.prop.startsWith(QStringLiteral("org.zfsmgr:schedule-"), Qt::CaseInsensitive)
            && !row.source.startsWith(QStringLiteral("inherited"), Qt::CaseInsensitive)) {
            props[row.prop.toStdString()] = row.value.toStdString();
        }
    }
    G::Schedule s;
    G::Reason motivo;
    G::fromProperties(props, s, motivo);
    pane.schedEnabled->setChecked(s.enabled);
    pane.schedRecursive->setChecked(s.recursive);
    pane.schedLevel->setChecked(s.level);
    const int valores[5] = {s.hourly, s.daily, s.weekly, s.monthly, s.yearly};
    for (int i = 0; i < 5; ++i) {
        pane.schedRetention[i]->setValue(valores[i]);
    }
    pane.schedTarget->setText(QString::fromStdString(s.target));
    pane.schedStatus->clear();
    if (pane.datasetTabs && pane.datasetTabs->currentWidget() == pane.schedulesTab) {
        loadPaneScheduleStatusAsync(paneIdx);
    }
}

void MainWindow::loadPaneScheduleStatusAsync(int paneIdx) {
    const DatasetPane& pane = m_datasetPanes[paneIdx];
    const int connIdx = pane.schedConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    const QString dataset = pane.schedDataset;
    (void)QtConcurrent::run([this, paneIdx, p, dataset]() {
        if (m_destroying.load()) {
            return;
        }
        QString out;
        QString err;
        int rc = -1;
        G::ConnectionState c;
        std::string error;
        const bool ok =
            runAgentOnConnection(p, mwhelpers::argvQt(zfsmgr::commands::requests::gsaStatus()),
                                 8000, out, err, rc)
            && rc == 0 && G::parseState(out.toStdString(), c, error);
        QMetaObject::invokeMethod(this, [this, paneIdx, dataset, ok, c, err]() {
            DatasetPane& pn = m_datasetPanes[paneIdx];
            if (!pn.schedStatus || pn.schedDataset != dataset) {
                return;  // another dataset was selected meanwhile
            }
            pn.schedStatus->clear();
            if (!ok) {
                ponFila(pn.schedStatus, QStringLiteral("error"), err.trimmed());
                return;
            }
            const G::DatasetState* d = nullptr;
            for (const G::DatasetState& x : c.datasets) {
                if (QString::fromStdString(x.dataset) == dataset) {
                    d = &x;
                }
            }
            if (!d) {
                ponFila(pn.schedStatus,
                        trk(QStringLiteral("t_sched_state_001"), QStringLiteral("Estado"),
                            QStringLiteral("State")),
                        trk(QStringLiteral("t_sch_ninguna_002"), QStringLiteral("sin programación"),
                            QStringLiteral("no schedule")));
                return;
            }
            ponFila(pn.schedStatus,
                    trk(QStringLiteral("t_sched_state_001"), QStringLiteral("Estado"),
                        QStringLiteral("State")),
                    d->failedStep.empty() ? QStringLiteral("green") : QStringLiteral("red"));
            ponFila(pn.schedStatus,
                    trk(QStringLiteral("t_sched_last_snap_001"), QStringLiteral("Último snapshot"),
                        QStringLiteral("Last snapshot")),
                    QStringLiteral("%1  %2").arg(QString::fromStdString(d->lastSnapshot),
                                                 fecha(d->lastSnapshotTime)));
            if (d->level) {
                ponFila(pn.schedStatus,
                        trk(QStringLiteral("t_sched_last_level_001"), QStringLiteral("Nivelación"),
                            QStringLiteral("Levelling")),
                        QStringLiteral("%1  %2  → %3")
                            .arg(d->hasLevel
                                     ? (d->levelOk ? QStringLiteral("ok") : QStringLiteral("error"))
                                     : QStringLiteral("-"),
                                 fecha(d->lastLevelTime), QString::fromStdString(d->destination)));
            }
            ponFila(pn.schedStatus,
                    trk(QStringLiteral("t_sched_next_001"), QStringLiteral("Siguiente"),
                        QStringLiteral("Next")),
                    fecha(d->next));
            if (!d->error.empty()) {
                ponFila(pn.schedStatus, QStringLiteral("✗"),
                        QStringLiteral("%1: %2").arg(QString::fromStdString(d->failedStep),
                                                     QString::fromStdString(d->error)));
            }
        });
    });
}

void MainWindow::setPaneScheduleDailyForTest(int pane, int daily, bool enabled) {
    m_datasetPanes[pane].schedRetention[1]->setValue(daily);
    m_datasetPanes[pane].schedEnabled->setChecked(enabled);
}

void MainWindow::applyPaneSchedule(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const int connIdx = pane.schedConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || pane.schedDataset.isEmpty()) {
        return;
    }
    const ConnectionProfile& p = m_conns.profiles[connIdx];
    G::Schedule s;
    s.enabled = pane.schedEnabled->isChecked();
    s.recursive = pane.schedRecursive->isChecked();
    s.level = pane.schedLevel->isChecked();
    s.hourly = pane.schedRetention[0]->value();
    s.daily = pane.schedRetention[1]->value();
    s.weekly = pane.schedRetention[2]->value();
    s.monthly = pane.schedRetention[3]->value();
    s.yearly = pane.schedRetention[4]->value();
    const auto avisa = [this](const QString& texto) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), texto);
    };
    const QString destino = pane.schedTarget->text().trimmed();
    G::Reason m;
    if (!destino.isEmpty() && !G::targetFromUrl(destino.toStdString(), s.target, m)) {
        avisa(QStringLiteral("%1: %2").arg(destino, QString::fromStdString(G::labelOf(m.failure))));
        return;
    }
    const std::string ds = pane.schedDataset.toStdString();
    const auto conexionExiste = [this](const std::string& nombre) {
        return connectionIndexByNameOrId(QString::fromStdString(nombre)) >= 0;
    };
    if (!G::isValid(ds, s, conexionExiste, m)) {
        avisa(QStringLiteral("%1: %2 %3")
                  .arg(pane.schedDataset, QString::fromStdString(G::labelOf(m.failure)),
                       QString::fromStdString(m.detail)));
        return;
    }
    // The rest of the pool's schedules: two recursive ones must not overlap.
    QString out;
    QString err;
    int rc = -1;
    if (!runAgentOnConnection(p,
                              mwhelpers::argvQt(zfsmgr::commands::requests::gsaOfDataset(
                                  pane.schedPool.toStdString())),
                              30000, out, err, rc)
        || rc != 0) {
        avisa(err.trimmed());
        return;
    }
    std::vector<G::Entry> delPool;
    bool sustituido = false;
    for (const auto& kv : G::groupProperties(out.toStdString())) {
        G::Entry en{kv.first, {}};
        G::Reason r;
        if (!G::fromProperties(kv.second, en.schedule, r)) {
            continue;
        }
        if (en.dataset == ds) {
            en.schedule = s;
            sustituido = true;
        }
        delPool.push_back(en);
    }
    if (!sustituido) {
        delPool.push_back({ds, s});
    }
    if (!G::isValidSet(delPool, m)) {
        avisa(QStringLiteral("%1: %2 %3")
                  .arg(QString::fromStdString(m.dataset),
                       QString::fromStdString(G::labelOf(m.failure)),
                       QString::fromStdString(m.detail)));
        return;
    }
    if (!s.target.empty()) {
        std::string conn;
        std::string dsDestino;
        if (G::splitTarget(s.target, conn, dsDestino)) {
            const int idxDestino = connectionIndexByNameOrId(QString::fromStdString(conn));
            s.target = G::targetForScheduleMachine(
                s.target, idxDestino >= 0 && isLocalConnection(idxDestino),
                isLocalConnection(connIdx), zfsmgr::base::peers::thisMachineName());
        }
    }
    for (const auto& kv : G::toProperties(s)) {
        const QStringList args = daemonizeZfsMutationArgs(
            connIdx,
            QStringList{QStringLiteral("set"), QString::fromStdString(kv.first + "=" + kv.second),
                        pane.schedDataset});
        if (args.isEmpty() || !runAgentOnConnection(p, args, 30000, out, err, rc) || rc != 0) {
            avisa(args.isEmpty() ? trk(QStringLiteral("t_sched_needs_daemon_001"),
                                       QStringLiteral("Hace falta el daemon de la conexión."),
                                       QStringLiteral("The connection's daemon is needed."))
                                 : err.trimmed());
            break;
        }
    }
    appLog(QStringLiteral("INFO"), QStringLiteral("schedule %1 aplicado").arg(pane.schedDataset));
    invalidateDatasetCacheEntry(connIdx, pane.schedPool, pane.schedDataset, false);
    loadPaneScheduleStatusAsync(paneIdx);
}

void MainWindow::clearPaneSchedule(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const int connIdx = pane.schedConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || pane.schedDataset.isEmpty()) {
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conf_sch_clear"),
                QStringLiteral("Se va a borrar la programación de %1. ¿Continuar?"),
                QStringLiteral("The schedule of %1 is going to be cleared. Continue?"))
                .replace(QStringLiteral("%1"), pane.schedDataset))
        != QMessageBox::Yes) {
        return;
    }
    const ConnectionProfile& p = m_conns.profiles[connIdx];
    for (const auto& kv : G::toProperties(G::Schedule{})) {
        const QStringList args = daemonizeZfsMutationArgs(
            connIdx, QStringList{QStringLiteral("inherit"), QString::fromStdString(kv.first),
                                 pane.schedDataset});
        QString out;
        QString err;
        int rc = -1;
        if (args.isEmpty() || !runAgentOnConnection(p, args, 30000, out, err, rc) || rc != 0) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), err.trimmed());
            return;
        }
    }
    invalidateDatasetCacheEntry(connIdx, pane.schedPool, pane.schedDataset, false);
    fillPaneSchedules(paneIdx, connIdx, pane.schedPool, pane.schedDataset);
}

void MainWindow::runPaneScheduleNow(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const int connIdx = pane.schedConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || pane.schedDataset.isEmpty()) {
        return;
    }
    QString out;
    QString err;
    int rc = -1;
    if (!runAgentOnConnection(m_conns.profiles[connIdx],
                              mwhelpers::argvQt(zfsmgr::commands::requests::gsaRunOnce(
                                  pane.schedDataset.toStdString())),
                              600000, out, err, rc)
        || rc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), err.trimmed());
        return;
    }
    for (const QString& linea : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        appendConnectionLog(m_conns.profiles[connIdx].id, linea);
    }
    // A levelling, if due, runs as a job: the Jobs box shows it.
    refreshAllJobsAsync();
    loadPaneScheduleStatusAsync(paneIdx);
}
