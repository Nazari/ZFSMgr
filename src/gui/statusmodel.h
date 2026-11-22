#pragma once

// What the status panel shows, computed from what the CLI answers —`ls zfsm://` for the
// machines and `overview <machine>` for each— and nothing else. No widget here: this is the
// part that can be tested without a screen.
//
// The CLI decides what is a warning and writes it; this only counts and orders.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace zfsmgr::gui {

struct MachineView {
    QString id;
    QString name;
    bool connected{true};
    bool loaded{false};    // its overview has arrived (or failed)
    QString failure;       // the service could not ask it
    QJsonObject overview;  // machines[0] of `overview`
    QJsonArray warnings() const;
    bool reachable() const;
};

struct Summary {
    int machines{0};  // connected ones
    int answering{0};
    int pools{0};
    int poolsWithProblems{0};
    int copies{0};  // schedules that copy (push or pull)
    int copiesOk{0};
    int copiesPending{0};
    int copiesFailing{0};
    int jobsRunning{0};
};

struct Attention {
    bool error{false};
    QString connection;
    QString text;
};

struct ScheduleRow {
    QString machine;  // display name of the machine that holds the schedule
    QString dataset;
    QString kind;         // push, pull, local
    QString peer;         // zfsm://… or empty
    QString state;        // ok, pending, error
    qint64 lastLevel{0};  // seconds since the epoch, 0 if never
    QString error;
};

struct JobRow {
    QString machine;
    QString id;
    QString description;
    QString state;  // running, queued
    qint64 bytes{0};
    double rate{0};  // MiB/s
};

// The machines from `ls zfsm://` (JSON), in its order.
QVector<MachineView> machinesFromLs(const QString& lsJson);
// Fills `m` with the answer of `overview <m.id>` (JSON).
void applyOverview(MachineView& m, const QString& overviewJson);

Summary summarize(const QVector<MachineView>& ms);
// Errors first, then warnings; a machine that could not be asked counts as an error.
QVector<Attention> attention(const QVector<MachineView>& ms);
QVector<ScheduleRow> scheduleRows(const QVector<MachineView>& ms);
QVector<JobRow> jobRows(const QVector<MachineView>& ms);

// «hace 2 h» / «2 h ago», from seconds since the epoch; «nunca» / «never» for 0.
QString ago(qint64 epochSeconds, qint64 now);
// 2100000000 → «2,0 GB» / «2.0 GB».
QString bytesText(qint64 bytes);

}  // namespace zfsmgr::gui
