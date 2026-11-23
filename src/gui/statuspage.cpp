#include "statuspage.h"

#include "statestore.h"
#include "uikit.h"
#include "text.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace zfsmgr::gui {

namespace {

// One of the four numbers at the top.
QFrame* tile(const QString& title, const QString& big, const QString& small, const QString& state,
             QWidget* parent) {
    QFrame* f = card(parent);
    auto* lay = new QVBoxLayout(f);
    lay->addWidget(muted(title, f));
    auto* n = label(big, f, false);
    QFont fn = n->font();
    fn.setPointSizeF(fn.pointSizeF() * 1.8);
    fn.setBold(true);
    n->setFont(fn);
    n->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(state)));
    lay->addWidget(n);
    lay->addWidget(muted(small, f));
    return f;
}

}  // namespace

StatusPage::StatusPage(StateStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    auto* outer = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    auto* title = heading(L("Status", "Estado"), this);
    QFont ft = title->font();
    ft.setPointSizeF(ft.pointSizeF() * 1.3);
    title->setFont(ft);
    top->addWidget(title);
    top->addStretch(1);
    m_ago = muted(QString(), this);
    m_ago->setWordWrap(false);
    m_ago->setMinimumWidth(
        m_ago->fontMetrics().horizontalAdvance(L("Updated 59 min ago", "Actualizado hace 59 min")));
    top->addWidget(m_ago);
    m_refresh = new QPushButton(L("Refresh", "Actualizar"), this);
    connect(m_refresh, &QPushButton::clicked, m_store, &StateStore::refresh);
    top->addWidget(m_refresh);
    outer->addLayout(top);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_scroll, 1);

    connect(m_store, &StateStore::changed, this, &StatusPage::render);
    connect(m_store, &StateStore::refreshing, this,
            [this] { m_ago->setText(L("Updating…", "Actualizando…")); });
    m_tick = new QTimer(this);
    m_tick->setInterval(10 * 1000);
    connect(m_tick, &QTimer::timeout, this, &StatusPage::updateAgo);
    m_tick->start();

    render();
}

void StatusPage::updateAgo() {
    if (m_store->updatedAt() > 0) {
        m_ago->setText(L("Updated %1", "Actualizado %1")
                           .arg(ago(m_store->updatedAt(), QDateTime::currentSecsSinceEpoch())));
    }
}

void StatusPage::render() {
    // Rebuilt whole, in a NEW widget that replaces the old one (setWidget deletes it): a few
    // dozen labels, once per answer. Emptying the old layout instead left behind the widgets
    // of its nested layouts —the tiles, the grid—, painted under the new ones.
    const int scrollPos = m_scroll->verticalScrollBar()->value();
    m_body = new QWidget();
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setAlignment(Qt::AlignTop);
    replaceScrollWidget(m_scroll, m_body);
    QTimer::singleShot(0, this,
                       [this, scrollPos] { m_scroll->verticalScrollBar()->setValue(scrollPos); });
    updateAgo();
    const QVector<MachineView>& machines = m_store->machines();

    if (m_store->listFailed()) {
        m_bodyLayout->addWidget(
            label(L("Could not list the machines: %1", "No se pudieron listar las máquinas: %1")
                      .arg(m_store->listError()),
                  m_body));
        return;
    }
    if (machines.isEmpty()) {
        m_bodyLayout->addWidget(muted(L("Asking…", "Preguntando…"), m_body));
        return;
    }

    const Summary s = summarize(machines);
    auto* tiles = new QHBoxLayout();
    tiles->addWidget(
        tile(L("Machines", "Máquinas"), QStringLiteral("%1 / %2").arg(s.answering).arg(s.machines),
             L("answer", "responden"),
             s.answering == s.machines ? QStringLiteral("ok") : QStringLiteral("error"), m_body));
    tiles->addWidget(tile(
        L("Pools", "Pools"),
        s.poolsWithProblems == 0 ? QString::number(s.pools) : QString::number(s.poolsWithProblems),
        s.poolsWithProblems == 0 ? L("all fine", "todos bien")
                                 : L("need a look", "necesitan atención"),
        s.poolsWithProblems == 0 ? QStringLiteral("ok") : QStringLiteral("warning"), m_body));
    tiles->addWidget(
        tile(L("Copies", "Copias"), QStringLiteral("%1 / %2").arg(s.copiesOk).arg(s.copies),
             s.copiesFailing > 0
                 ? L("up to date · %1 failing", "al día · %1 fallan").arg(s.copiesFailing)
             : s.copiesPending > 0
                 ? L("up to date · %1 pending", "al día · %1 pendientes").arg(s.copiesPending)
                 : L("up to date", "al día"),
             s.copiesFailing > 0   ? QStringLiteral("error")
             : s.copiesPending > 0 ? QStringLiteral("pending")
                                   : QStringLiteral("ok"),
             m_body));
    tiles->addWidget(tile(L("Jobs", "Trabajos"), QString::number(s.jobsRunning),
                          L("copying now", "copiando ahora"), QStringLiteral("ok"), m_body));
    m_bodyLayout->addLayout(tiles);

    // What needs attention, already written by the CLI.
    const QVector<Attention> att = attention(machines);
    Q_EMIT attentionChanged(int(att.size()));
    if (!att.isEmpty()) {
        QFrame* box = card(m_body);
        auto* lay = new QVBoxLayout(box);
        lay->addWidget(heading(
            att.size() == 1 ? L("1 thing needs your attention", "1 cosa necesita tu atención")
                            : L("%1 things need your attention", "%1 cosas necesitan tu atención")
                                  .arg(att.size()),
            box));
        for (const Attention& a : att) {
            auto* l = label((a.error ? QStringLiteral("✕ ") : QStringLiteral("▲ ")) + a.text, box);
            l->setStyleSheet(
                QStringLiteral("color:%1")
                    .arg(colourOf(a.error ? QStringLiteral("error") : QStringLiteral("warning"))));
            auto* row = new QHBoxLayout();
            row->addWidget(l, 1);
            // Where to look at it: the machine, in Machines.
            auto* see = new QPushButton(L("See", "Ver"), box);
            see->setFlat(true);
            see->setCursor(Qt::PointingHandCursor);
            const QString id = a.connection;
            connect(see, &QPushButton::clicked, this, [this, id] { Q_EMIT machineRequested(id); });
            row->addWidget(see, 0, Qt::AlignTop);
            lay->addLayout(row);
        }
        m_bodyLayout->addWidget(box);
    }

    // Machines and pools.
    m_bodyLayout->addWidget(heading(L("Machines and pools", "Máquinas y pools"), m_body));
    auto* grid = new QGridLayout();
    int col = 0;
    int row = 0;
    for (const MachineView& m : machines) {
        QFrame* c = card(m_body);
        auto* lay = new QVBoxLayout(c);
        lay->addWidget(heading(m.name, c));
        if (!m.connected) {
            lay->addWidget(muted(L("Disconnected", "Desconectada"), c));
        } else if (!m.loaded) {
            lay->addWidget(stateLabel(QString(), c));
        } else if (!m.reachable()) {
            lay->addWidget(stateLabel(QStringLiteral("error"), c));
            lay->addWidget(muted(m.failure.isEmpty()
                                     ? m.overview.value(QStringLiteral("reason")).toString()
                                     : m.failure,
                                 c));
        } else {
            const QJsonArray pools = m.overview.value(QStringLiteral("pools")).toArray();
            if (pools.isEmpty()) {
                lay->addWidget(muted(L("No pools", "Sin pools"), c));
            }
            for (const QJsonValue& v : pools) {
                const QJsonObject p = v.toObject();
                auto* line = new QHBoxLayout();
                const int cap = p.value(QStringLiteral("capacity")).toInt(-1);
                line->addWidget(label(p.value(QStringLiteral("name")).toString(), c, false));
                line->addWidget(muted(cap >= 0
                                          ? L("%1 % of %2", "%1 % de %2")
                                                .arg(cap)
                                                .arg(p.value(QStringLiteral("size")).toString())
                                          : p.value(QStringLiteral("size")).toString(),
                                      c));
                line->addStretch(1);
                line->addWidget(stateLabel(p.value(QStringLiteral("status")).toString(), c));
                lay->addLayout(line);
            }
        }
        c->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        grid->addWidget(c, row, col, Qt::AlignTop);
        if (++col == 3) {
            col = 0;
            ++row;
        }
    }
    m_bodyLayout->addLayout(grid);

    // Scheduled copies.
    const QVector<ScheduleRow> sch = scheduleRows(machines);
    if (!sch.isEmpty()) {
        m_bodyLayout->addWidget(heading(L("Scheduled copies", "Copias programadas"), m_body));
        QFrame* box = card(m_body);
        auto* lay = new QVBoxLayout(box);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (const ScheduleRow& r : sch) {
            auto* line = new QHBoxLayout();
            auto* left = new QVBoxLayout();
            left->addWidget(label(r.dataset, box, false));
            QString how;
            if (r.kind == QLatin1String("pull")) {
                how = L("%1 pulls it from %2", "la trae %1 desde %2").arg(r.machine, r.peer);
            } else if (r.kind == QLatin1String("push")) {
                how = L("%1 sends it to %2", "la envía %1 a %2").arg(r.machine, r.peer);
            } else {
                how = L("snapshots only, on %1", "solo snapshots, en %1").arg(r.machine);
            }
            if (r.kind != QLatin1String("local")) {
                how += QStringLiteral(" · ") + ago(r.lastLevel, now);
            }
            left->addWidget(muted(how, box));
            if (!r.error.isEmpty()) {
                auto* e = label(r.error, box);
                e->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(QStringLiteral("error"))));
                left->addWidget(e);
            }
            line->addLayout(left, 1);
            line->addWidget(stateLabel(r.state, box), 0, Qt::AlignTop);
            lay->addLayout(line);
        }
        m_bodyLayout->addWidget(box);
    }

    // Copying now.
    const QVector<JobRow> jobs = jobRows(machines);
    if (!jobs.isEmpty()) {
        m_bodyLayout->addWidget(heading(L("Copying now", "Copiando ahora"), m_body));
        QFrame* box = card(m_body);
        auto* lay = new QVBoxLayout(box);
        for (const JobRow& j : jobs) {
            lay->addWidget(label(j.description, box));
            lay->addWidget(
                muted(j.state == QLatin1String("queued")
                          ? L("on %1 · waiting its turn", "en %1 · esperando turno").arg(j.machine)
                          : L("on %1 · %2 · %3 MB/s", "en %1 · %2 · %3 MB/s")
                                .arg(j.machine, bytesText(j.bytes))
                                .arg(j.rate, 0, 'f', 1),
                      box));
        }
        m_bodyLayout->addWidget(box);
    }
}

}  // namespace zfsmgr::gui
