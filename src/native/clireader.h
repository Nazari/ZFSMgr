#pragma once

#include "cliservice.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QThread;

// What the window READS, asked to the shell too: `ls … --format json`, `jobs -a`,
// `schedule status`… The window keeps no listing logic of its own.
//
// Unlike CliService, which runs the user's actions one after another, reads come from worker
// threads and from several machines at once. So there is one `zfsmgr-cli --serve` per
// connection, each in its own thread: reads of different machines run in parallel, and each
// process keeps its machine's tunnel between reads. `read` blocks the caller; from the
// interface thread it waits pumping events, as runCliAndWait does.
//
// A read never asks: a confirmation is answered «no» and a secret is not given.
class CliReader : public QObject {
    Q_OBJECT
public:
    explicit CliReader(QObject* parent = nullptr);
    ~CliReader() override;

    void configure(const QString& executable, const QStringList& arguments, const QString& master);
    // `connection` picks the process; it is only a key, the line names its own targets.
    // `format`: empty for json, "text" for what is read as it is printed.
    CliService::Result read(const QString& connection, const QString& line, int timeoutMs = 120000,
                            const QString& format = QString());

    // For the tests: no processes; each line is answered by `fake`, and remembered.
    void setFakeForTest(CliService::Fake fake) { m_fake = std::move(fake); }
    QStringList linesForTest() const;

private:
    struct Worker {
        QThread* thread{nullptr};
        CliService* service{nullptr};
    };
    Worker& workerFor(const QString& connection);

    QString m_executable;
    QStringList m_arguments;
    QString m_master;
    CliService::Fake m_fake;
    mutable QMutex m_mutex;
    QHash<QString, Worker> m_workers;
    QStringList m_lines;
};
