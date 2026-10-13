#include "cliservice.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

CliService::CliService(QObject* parent) : QObject(parent) {}

CliService::~CliService() {
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->closeWriteChannel();
        if (!m_proc->waitForFinished(3000)) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
    }
}

void CliService::configure(const QString& executable, const QStringList& arguments,
                           const QString& master) {
    m_executable = executable;
    m_arguments = arguments;
    m_master = master;
}

void CliService::run(const QString& line, Done done, const QString& format) {
    m_lines.push_back(line);
    if (m_fake) {
        // Answered after returning, as the real service would be.
        QTimer::singleShot(0, this, [this, line, done]() {
            const Result r = m_fake(line);
            if (done) {
                done(r);
            }
        });
        return;
    }
    m_queue.enqueue(Pending{m_nextId++, line, std::move(done), format});
    if (!m_proc) {
        startProcess();
        return;
    }
    sendNext();
}

void CliService::startProcess() {
    m_ready = false;
    m_busy = false;
    m_buffer.clear();
    QProcess* proc = new QProcess(this);
    m_proc = proc;
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc, &QProcess::readyReadStandardOutput, this, &CliService::onOutput);
    // Either way the process is gone: what was waiting fails, and the next line starts a new
    // one. Each handler looks at ITS process, since the other may already have cleared it.
    const auto acaba = [this, proc](const QString& why) {
        if (m_proc != proc) {
            return;
        }
        m_proc = nullptr;
        failAll(why);
        proc->deleteLater();
    };
    connect(proc, &QProcess::finished, this, [acaba](int, QProcess::ExitStatus) {
        acaba(QStringLiteral("zfsmgr-cli --serve ended"));
    });
    connect(proc, &QProcess::errorOccurred, this, [this, acaba](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            acaba(QStringLiteral("cannot start %1").arg(m_executable));
        }
    });
    m_proc->start(m_executable, m_arguments);
    // The master password is the first line: down the pipe, never in argv or the environment.
    m_proc->write(m_master.toUtf8() + "\n");
}

void CliService::sendNext() {
    if (!m_proc || !m_ready || m_busy || m_queue.isEmpty()) {
        return;
    }
    m_current = m_queue.dequeue();
    m_busy = true;
    Q_EMIT lineStarted(m_current.line);
    QJsonObject req;
    req[QStringLiteral("id")] = m_current.id;
    req[QStringLiteral("line")] = m_current.line;
    if (!m_current.format.isEmpty()) {
        req[QStringLiteral("format")] = m_current.format;
    }
    m_proc->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
}

void CliService::onOutput() {
    m_buffer += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray linea = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        const QJsonObject ev = QJsonDocument::fromJson(linea).object();
        const QString tipo = ev.value(QStringLiteral("type")).toString();
        if (tipo == QStringLiteral("ready")) {
            m_ready = true;
            sendNext();
        } else if (tipo == QStringLiteral("confirm")) {
            const bool si = m_confirm && m_confirm(ev.value(QStringLiteral("text")).toString());
            QJsonObject ans;
            ans[QStringLiteral("id")] = ev.value(QStringLiteral("id"));
            ans[QStringLiteral("answer")] = si;
            m_proc->write(QJsonDocument(ans).toJson(QJsonDocument::Compact) + "\n");
        } else if (tipo == QStringLiteral("secret")) {
            QString secreto;
            const bool dado =
                m_askSecret && m_askSecret(ev.value(QStringLiteral("text")).toString(), secreto);
            QJsonObject ans;
            ans[QStringLiteral("id")] = ev.value(QStringLiteral("id"));
            ans[QStringLiteral("secret")] = dado ? QJsonValue(secreto) : QJsonValue();
            m_proc->write(QJsonDocument(ans).toJson(QJsonDocument::Compact) + "\n");
            secreto.fill(QLatin1Char('\0'));
        } else if (tipo == QStringLiteral("result")) {
            Result r;
            r.rc = ev.value(QStringLiteral("rc")).toInt(-1);
            r.out = ev.value(QStringLiteral("stdout")).toString();
            r.err = ev.value(QStringLiteral("stderr")).toString();
            const Done done = m_current.done;
            m_busy = false;
            if (done) {
                done(r);
            }
            sendNext();
        } else if (tipo == QStringLiteral("error")) {
            failAll(ev.value(QStringLiteral("text")).toString());
        }
    }
}

void CliService::failAll(const QString& why) {
    Result r;
    r.rc = -1;
    r.err = why;
    QQueue<Pending> pendientes;
    if (m_busy) {
        pendientes.enqueue(m_current);
    }
    while (!m_queue.isEmpty()) {
        pendientes.enqueue(m_queue.dequeue());
    }
    m_busy = false;
    m_ready = false;
    for (const Pending& p : pendientes) {
        if (p.done) {
            p.done(r);
        }
    }
}
