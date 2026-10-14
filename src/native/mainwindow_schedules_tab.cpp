#include "mainwindow.h"

#include "commands/gsa.h"
#include "commands/peers.h"
#include "commands/requests.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <ctime>

// The Schedules tab of a dataset: what `schedule status` reports about it and what `schedule`
// sets on it. Apply, Clear and Run now are the shell's `schedule` lines: the tab only gathers
// the values and shows the answer.

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

bool MainWindow::readScheduleStatus(int connIdx, G::ConnectionState& out, QString* error) {
    out = G::ConnectionState{};
    out.connection = cliConnName(connIdx).toStdString();
    QJsonObject r;
    QString err;
    const bool leido = cliReadJson(
        connIdx, QStringLiteral("schedule status %1").arg(cliConnName(connIdx)), r, &err, 20000);
    if (error) {
        *error = err;
    }
    if (!leido) {
        out.whyNotChecked = err.toStdString();
        return false;
    }
    out.checked = true;
    const auto texto = [](const QJsonObject& o, const char* k) {
        return o.value(QLatin1String(k)).toString().toStdString();
    };
    const auto entero = [](const QJsonObject& o, const char* k) {
        return static_cast<long long>(o.value(QLatin1String(k)).toDouble());
    };
    for (const QJsonValue& v : r.value(QStringLiteral("schedule_status")).toArray()) {
        const QJsonObject o = v.toObject();
        G::DatasetState d;
        d.dataset = texto(o, "dataset");
        d.destination = texto(o, "destination");
        d.lastSnapshot = texto(o, "last_snapshot");
        d.lastSnapshotTime = entero(o, "last_snapshot_time");
        d.level = o.value(QStringLiteral("levels")).toBool();
        d.hasLevel = o.value(QStringLiteral("has_level")).toBool();
        d.levelOk = o.value(QStringLiteral("level_ok")).toBool(true);
        d.lastLevelTime = entero(o, "last_level_time");
        d.next = entero(o, "next");
        d.failedStep = texto(o, "failed_step");
        d.error = texto(o, "error");
        out.datasets.push_back(std::move(d));
    }
    return true;
}

void MainWindow::loadPaneScheduleStatusAsync(int paneIdx) {
    const DatasetPane& pane = m_datasetPanes[paneIdx];
    const int connIdx = pane.schedConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }
    const QString dataset = pane.schedDataset;
    (void)QtConcurrent::run([this, paneIdx, connIdx, dataset]() {
        if (m_destroying.load()) {
            return;
        }
        QString err;
        G::ConnectionState c;
        const bool ok = readScheduleStatus(connIdx, c, &err);
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
    // The whole schedule as the tab shows it, in one `schedule` line: the shell checks it
    // against the rest of the pool and stores it as the scheduling machine reads it.
    QStringList partes{QStringLiteral("schedule")};
    for (int i = 0; i < 5; ++i) {
        partes << QStringLiteral("--%1").arg(QLatin1String(kClases[i]))
               << QString::number(pane.schedRetention[i]->value());
    }
    partes << (pane.schedRecursive->isChecked() ? QStringLiteral("--recursive")
                                                : QStringLiteral("--no-recursive"));
    partes << (pane.schedLevel->isChecked() ? QStringLiteral("--level")
                                            : QStringLiteral("--no-level"));
    const QString destino = pane.schedTarget->text().trimmed();
    if (destino.isEmpty()) {
        partes << QStringLiteral("--no-to");
    } else {
        partes << QStringLiteral("--to") << cliQuote(destino);
    }
    if (!pane.schedEnabled->isChecked()) {
        partes << QStringLiteral("--off");
    }
    partes << QStringLiteral("--on") << zfsmUrl(connIdx, pane.schedDataset);
    if (runCliAndWait(partes.join(QLatin1Char(' '))).rc == 0 && m_refreshAfterCli) {
        invalidateDatasetCacheEntry(connIdx, pane.schedPool, pane.schedDataset, false);
        loadPaneScheduleStatusAsync(paneIdx);
    }
}

void MainWindow::clearPaneSchedule(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const int connIdx = pane.schedConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || pane.schedDataset.isEmpty()) {
        return;
    }
    if (runCliAndWait(
            QStringLiteral("schedule --clear --on %1").arg(zfsmUrl(connIdx, pane.schedDataset)))
                .rc
            == 0
        && m_refreshAfterCli) {
        invalidateDatasetCacheEntry(connIdx, pane.schedPool, pane.schedDataset, false);
        fillPaneSchedules(paneIdx, connIdx, pane.schedPool, pane.schedDataset);
    }
}

void MainWindow::runPaneScheduleNow(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const int connIdx = pane.schedConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || pane.schedDataset.isEmpty()) {
        return;
    }
    const QString id = m_conns.profiles[connIdx].id;
    runCli(
        QStringLiteral("schedule run --on %1").arg(zfsmUrl(connIdx, pane.schedDataset)),
        [this, paneIdx, id](const CliService::Result& r) {
            for (const QString& linea : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                appendConnectionLog(id, linea);
            }
            if (!m_refreshAfterCli) {
                return;
            }
            // A levelling, if due, runs as a job: the Jobs box shows it.
            refreshAllJobsAsync();
            loadPaneScheduleStatusAsync(paneIdx);
        },
        QStringLiteral("text"));
}
