#include "copiespage.h"

#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace zfsmgr::gui {

namespace {

QString keyOf(const ScheduleRow& r) {
    return r.machineId + QLatin1Char('|') + r.dataset;
}

// «zfsm://unib/ppal/user/srvs» → «unib», «ppal/user/srvs».
void splitUrl(const QString& url, QString& machine, QString& dataset) {
    QString rest = url;
    if (rest.startsWith(QLatin1String("zfsm://"))) {
        rest = rest.mid(7);
    }
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    machine = slash < 0 ? rest : rest.left(slash);
    dataset = slash < 0 ? QString() : rest.mid(slash + 1);
}

struct Ends {
    QString originMachine, originDataset, copyMachine, copyDataset;
};

Ends endsOf(const ScheduleRow& r) {
    Ends e;
    if (r.kind == QLatin1String("pull")) {
        splitUrl(r.peer, e.originMachine, e.originDataset);
        e.copyMachine = r.machine;
        e.copyDataset = r.dataset;
    } else {
        e.originMachine = r.machine;
        e.originDataset = r.dataset;
        splitUrl(r.peer, e.copyMachine, e.copyDataset);
    }
    return e;
}

QString howText(const ScheduleRow& r) {
    return r.kind == QLatin1String("pull") ? L("%1 pulls it", "la trae %1").arg(r.machine)
                                           : L("%1 sends it", "la envía %1").arg(r.machine);
}

}  // namespace

CopiesPage::CopiesPage(StateStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    auto* outer = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    auto* title = heading(L("Copies", "Copias"), this);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    title->setFont(f);
    top->addWidget(title);
    top->addStretch(1);
    auto* fresh = new QPushButton(L("New copy", "Nueva copia"), this);
    connect(fresh, &QPushButton::clicked, this, &CopiesPage::newCopyRequested);
    top->addWidget(fresh);
    outer->addLayout(top);
    outer->addWidget(muted(L("Each row is a dataset copied to another machine. Some are SENT by "
                             "the origin; others are PULLED by the machine that keeps the copy, "
                             "which is safer: from it the original cannot be deleted.",
                             "Cada fila es un dataset que se copia a otra máquina. Unas las "
                             "ENVÍA la máquina de origen; otras las TRAE la máquina de copias, "
                             "que es más seguro: desde ella no se puede borrar el original."),
                           this));

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({L("What", "Qué"), L("From → to", "De → a"),
                                        L("How", "Cómo"), L("Keeps", "Guarda"), L("Last", "Última"),
                                        L("State", "Estado")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    outer->addWidget(m_table, 2);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_table->currentRow();
        if (row >= 0 && row < m_rows.size()) {
            m_selectedKey = keyOf(m_rows[row]);
        }
        showDetail();
    });

    m_detail = card(this);
    m_detailLayout = new QVBoxLayout(m_detail);
    outer->addWidget(m_detail, 1);

    connect(m_store, &StateStore::changed, this, &CopiesPage::render);
    render();
}

QVector<ScheduleRow> CopiesPage::copies() const {
    QVector<ScheduleRow> out;
    for (const ScheduleRow& r : scheduleRows(m_store->machines())) {
        if (r.kind != QLatin1String("local")) {
            out.push_back(r);
        }
    }
    return out;
}

void CopiesPage::render() {
    m_rows = copies();
    const QSignalBlocker block(m_table);
    m_table->setRowCount(int(m_rows.size()));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    int selected = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        const ScheduleRow& r = m_rows[i];
        const Ends e = endsOf(r);
        auto put = [this, i](int col, const QString& text) {
            m_table->setItem(i, col, new QTableWidgetItem(text));
        };
        put(0, e.originDataset);
        put(1, e.originMachine + QStringLiteral(" → ") + e.copyMachine);
        put(2, howText(r));
        put(3, keepText(r.keep));
        put(4, r.state == QLatin1String("paused") ? QStringLiteral("—") : ago(r.lastLevel, now));
        auto* st = new QTableWidgetItem(stateWord(r.state));
        st->setForeground(QColor(colourOf(r.state)));
        m_table->setItem(i, 5, st);
        if (keyOf(r) == m_selectedKey) {
            selected = i;
        }
    }
    if (selected >= 0) {
        m_table->selectRow(selected);
    } else if (!m_rows.isEmpty() && m_selectedKey.isEmpty()) {
        m_table->selectRow(0);
        m_selectedKey = keyOf(m_rows[0]);
    }
    showDetail();
}

void CopiesPage::showDetail() {
    while (QLayoutItem* it = m_detailLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->deleteLater();
        } else if (QLayout* l = it->layout()) {
            while (QLayoutItem* sub = l->takeAt(0)) {
                if (sub->widget()) {
                    sub->widget()->deleteLater();
                }
                delete sub;
            }
        }
        delete it;
    }
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size()) {
        m_detailLayout->addWidget(muted(m_rows.isEmpty()
                                            ? L("No copies yet. «New copy» sets one up.",
                                                "Todavía no hay copias. «Nueva copia» prepara una.")
                                            : L("Choose a copy.", "Elige una copia."),
                                        m_detail));
        return;
    }
    const ScheduleRow r = m_rows[row];
    const Ends e = endsOf(r);
    auto* head = new QHBoxLayout();
    head->addWidget(heading(e.originDataset, m_detail));
    head->addWidget(stateLabel(r.state, m_detail));
    head->addStretch(1);
    m_detailLayout->addLayout(head);

    m_detailLayout->addWidget(label(
        L("Origin: %1 on %2", "Origen: %1 en %2").arg(e.originDataset, e.originMachine), m_detail));
    m_detailLayout->addWidget(label(L("Copy: %1 on %2, keeps %3", "Copia: %1 en %2, guarda %3")
                                        .arg(e.copyDataset, e.copyMachine, keepText(r.keep)),
                                    m_detail));
    m_detailLayout->addWidget(muted(
        r.kind == QLatin1String("pull")
            ? L("%1 asks %2 for what is new. From %1 the original cannot be deleted, even if "
                "someone gets into it.",
                "%1 pide lo nuevo a %2. Desde %1 no se puede borrar el original, aunque alguien "
                "entre en ella.")
                  .arg(e.copyMachine, e.originMachine)
            : L("%1 sends it after each snapshot. The copy keeps exactly what the origin keeps.",
                "%1 la manda tras cada snapshot. La copia guarda exactamente lo mismo que el "
                "origen.")
                  .arg(e.originMachine),
        m_detail));
    if (!r.lastSnapshot.isEmpty()) {
        m_detailLayout->addWidget(
            muted(L("Last snapshot: %1", "Último snapshot: %1").arg(r.lastSnapshot), m_detail));
    }
    if (!r.error.isEmpty()) {
        auto* err = label(r.error, m_detail);
        err->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(QStringLiteral("error"))));
        m_detailLayout->addWidget(err);
    }

    // The actions run on the machine that HOLDS the schedule: the one that pulls, or the
    // origin that sends.
    const QString on = QStringLiteral("zfsm://%1/%2").arg(r.machineId, r.dataset);
    auto* buttons = new QHBoxLayout();
    const bool paused = r.state == QLatin1String("paused");
    auto* now = new QPushButton(L("Copy now", "Copiar ahora"), m_detail);
    now->setEnabled(!paused && r.job.isEmpty());
    connect(now, &QPushButton::clicked, this, [this, on] {
        act(QStringLiteral("schedule run --on ") + on,
            L("Copying: follow it in Jobs.", "Copiando: síguelo en Trabajos."));
    });
    buttons->addWidget(now);
    auto* pause =
        new QPushButton(paused ? L("Resume", "Reanudar") : L("Pause", "Pausar"), m_detail);
    connect(pause, &QPushButton::clicked, this, [this, on, paused, r] {
        if (!paused) {
            act(QStringLiteral("schedule --off --on ") + on,
                L("Paused: nothing is copied until it is resumed.",
                  "En pausa: no se copia nada hasta reanudarla."));
            return;
        }
        // Setting any value switches it on again (see `help schedule`): its own daily one.
        const int daily = r.keep.value(QStringLiteral("daily")).toInt();
        act(QStringLiteral("schedule --daily %1 --on %2").arg(daily).arg(on),
            L("Resumed.", "Reanudada."));
    });
    buttons->addWidget(pause);
    auto* stopIt = new QPushButton(L("Stop copying…", "Dejar de copiar…"), m_detail);
    connect(stopIt, &QPushButton::clicked, this, [this, on, r] {
        // Pulled: the copier's schedule goes (the CLI asks first); what was copied stays.
        // Pushed: only the levelling goes; the origin keeps taking its snapshots.
        act(r.kind == QLatin1String("pull")
                ? QStringLiteral("schedule --clear --on ") + on
                : QStringLiteral("schedule --no-level --no-to --on ") + on,
            L("It is no longer copied. What was copied stays where it is.",
              "Ya no se copia. Lo copiado se queda donde está."));
    });
    buttons->addWidget(stopIt);
    buttons->addStretch(1);
    m_detailLayout->addLayout(buttons);
    m_detailLayout->addStretch(1);
}

void CopiesPage::act(const QString& line, const QString& done) {
    m_store->general()->request(line, [this, done](const CliResult& res) {
        Q_EMIT message(res.ok() ? done : res.err.trimmed());
        m_store->refresh();
    });
}

}  // namespace zfsmgr::gui
