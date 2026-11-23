#include "jobspage.h"

#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace zfsmgr::gui {

namespace {

constexpr int kHistoryShown = 30;

QString resultWord(const QString& state) {
    if (state == QLatin1String("done")) {
        return L("● Done", "● Hecho");
    }
    if (state == QLatin1String("failed")) {
        return L("✕ Failed", "✕ Falló");
    }
    if (state == QLatin1String("cancelled")) {
        return L("‖ Stopped", "‖ Parado");
    }
    return state;
}

QString resultColour(const QString& state) {
    return colourOf(state == QLatin1String("done")     ? QStringLiteral("ok")
                    : state == QLatin1String("failed") ? QStringLiteral("error")
                                                       : QStringLiteral("paused"));
}

}  // namespace

JobsPage::JobsPage(StateStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    auto* outer = new QVBoxLayout(this);
    auto* title = heading(L("Jobs", "Trabajos"), this);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    title->setFont(f);
    outer->addWidget(title);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_scroll, 1);
    // The history of each machine is asked once per refresh, as soon as the machine is known:
    // at start-up the list arrives after the page may already be showing.
    connect(m_store, &StateStore::changed, this, [this] {
        if (isVisible()) {
            loadHistory();
            render();
        }
    });
    connect(m_store, &StateStore::refreshing, this, [this] { m_asked.clear(); });
    render();
}

void JobsPage::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    loadHistory();
    render();
}

void JobsPage::loadHistory() {
    for (const MachineView& m : m_store->machines()) {
        if (!m.connected || m_asked.contains(m.id)) {
            continue;
        }
        m_asked.insert(m.id);
        const QString id = m.id;
        m_store->forMachine(id)->request(
            QStringLiteral("jobs --history zfsm://") + id, this, [this, id](const CliResult& r) {
                const qsizetype start = r.out.indexOf(QLatin1Char('{'));
                m_history.insert(id, start < 0 ? QJsonArray()
                                               : QJsonDocument::fromJson(r.out.mid(start).toUtf8())
                                                     .object()
                                                     .value(QStringLiteral("jobs"))
                                                     .toArray());
                if (isVisible()) {
                    render();
                }
            });
    }
}

void JobsPage::stop(const QString& machineId, const QString& jobId) {
    // The CLI asks first (the question arrives as a dialog) and says what happened.
    m_store->forMachine(machineId)->request(
        QStringLiteral("job cancel %1 --on zfsm://%2").arg(jobId, machineId), this,
        [this](const CliResult& r) {
            Q_EMIT message(r.ok() ? L("Job stopped.", "Trabajo parado.") : r.err.trimmed());
            m_store->refresh();
        });
}

void JobsPage::render() {
    const int pos = m_scroll->verticalScrollBar()->value();
    auto* body = new QWidget();
    auto* lay = new QVBoxLayout(body);
    lay->setAlignment(Qt::AlignTop);

    // Running and queued, from the shared state.
    lay->addWidget(heading(L("In progress", "En curso"), body));
    const QVector<JobRow> jobs = jobRows(m_store->machines());
    if (jobs.isEmpty()) {
        lay->addWidget(muted(L("Nothing is running. Scheduled copies start on their own at "
                               "the next pass, every five minutes.",
                               "No hay nada en curso. Las copias programadas arrancan solas en "
                               "la siguiente pasada, cada cinco minutos."),
                             body));
    } else {
        QFrame* box = card(body);
        auto* bl = new QVBoxLayout(box);
        for (const JobRow& j : jobs) {
            auto* row = new QHBoxLayout();
            auto* left = new QVBoxLayout();
            left->addWidget(label(j.description, box));
            left->addWidget(
                muted(j.state == QLatin1String("queued")
                          ? L("on %1 · waiting its turn", "en %1 · esperando turno").arg(j.machine)
                          : L("on %1 · %2 · %3 MB/s", "en %1 · %2 · %3 MB/s")
                                .arg(j.machine, bytesText(j.bytes))
                                .arg(j.rate, 0, 'f', 1),
                      box));
            row->addLayout(left, 1);
            auto* b = new QPushButton(L("Stop", "Parar"), box);
            const QString mid = j.machineId;
            const QString jid = j.id;
            connect(b, &QPushButton::clicked, this, [this, mid, jid] { stop(mid, jid); });
            row->addWidget(b, 0, Qt::AlignTop);
            bl->addLayout(row);
        }
        lay->addWidget(box);
    }

    // Finished, newest first, from every machine.
    struct Done {
        QString machine;
        QJsonObject job;
    };
    QVector<Done> done;
    for (const MachineView& m : m_store->machines()) {
        for (const QJsonValue& v : m_history.value(m.id)) {
            const QJsonObject j = v.toObject();
            const QString st = j.value(QStringLiteral("state")).toString();
            if (st == QLatin1String("running") || st == QLatin1String("queued")) {
                continue;
            }
            done.push_back({m.name, j});
        }
    }
    std::sort(done.begin(), done.end(), [](const Done& a, const Done& b) {
        return a.job.value(QStringLiteral("finished")).toString()
               > b.job.value(QStringLiteral("finished")).toString();
    });
    lay->addWidget(heading(L("Finished", "Terminados"), body));
    if (done.isEmpty()) {
        lay->addWidget(muted(L("Nothing yet.", "Nada todavía."), body));
    } else {
        QFrame* box = card(body);
        auto* grid = new QGridLayout(box);
        const QStringList heads{L("What", "Qué"), L("Where", "Dónde"), L("When", "Cuándo"),
                                L("Size", "Tamaño"), L("Result", "Resultado")};
        for (int c = 0; c < heads.size(); ++c) {
            grid->addWidget(muted(heads[c], box), 0, c);
        }
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        int row = 1;
        for (const Done& d : done.mid(0, kHistoryShown)) {
            const QJsonObject& j = d.job;
            QString what = j.value(QStringLiteral("description")).toString();
            if (what.isEmpty() || what == QLatin1String("-")) {
                what = j.value(QStringLiteral("snap")).toString();
            }
            const QDateTime fin =
                QDateTime::fromString(j.value(QStringLiteral("finished")).toString(), Qt::ISODate);
            const QString st = j.value(QStringLiteral("state")).toString();
            grid->addWidget(label(what, box), row, 0);
            grid->addWidget(label(d.machine, box, false), row, 1);
            grid->addWidget(
                label(fin.isValid() ? ago(fin.toSecsSinceEpoch(), now) : QString(), box, false),
                row, 2);
            grid->addWidget(
                label(bytesText(j.value(QStringLiteral("bytes")).toVariant().toLongLong()), box,
                      false),
                row, 3);
            auto* res = label(resultWord(st), box, false);
            res->setStyleSheet(QStringLiteral("color:%1; font-weight:600").arg(resultColour(st)));
            grid->addWidget(res, row, 4);
            ++row;
            const QString err = j.value(QStringLiteral("error")).toString();
            if (!err.isEmpty()) {
                auto* e = label(L("What happened: %1", "Qué pasó: %1").arg(err), box);
                e->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(QStringLiteral("error"))));
                grid->addWidget(e, row, 0, 1, 5);
                ++row;
            }
        }
        grid->setColumnStretch(0, 1);
        lay->addWidget(box);
    }

    replaceScrollWidget(m_scroll, body);
    QTimer::singleShot(0, this, [this, pos] { m_scroll->verticalScrollBar()->setValue(pos); });
}

}  // namespace zfsmgr::gui
