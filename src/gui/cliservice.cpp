#include "cliservice.h"

#include "text.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace zfsmgr::gui {

namespace {

QStringList stringsOf(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& x : v.toArray()) {
        out << x.toString();
    }
    return out;
}

}  // namespace

std::function<bool()> BusyHooks::userInput;
std::function<void(int)> BusyHooks::adjust;
std::function<int()> BusyHooks::begin;
std::function<void(int)> BusyHooks::end;

void CliService::holdBusy(Pending& p) {
    if (!p.busy && BusyHooks::adjust) {
        p.busy = true;
        BusyHooks::adjust(+1);
    }
}

void CliService::releaseBusy(Pending& p) {
    if (p.busy && BusyHooks::adjust) {
        p.busy = false;
        BusyHooks::adjust(-1);
    }
}

void CliService::finishBusy(Pending& p) {
    releaseBusy(p);
    if (p.token != 0 && BusyHooks::end) {
        BusyHooks::end(p.token);
    }
    p.token = 0;
}

QString findCli() {
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("zfsmgr-cli.exe");
#else
    const QString name = QStringLiteral("zfsmgr-cli");
#endif
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if (QFileInfo(beside).isExecutable()) {
        return beside;
    }
    return QStandardPaths::findExecutable(name);
}

CliService::CliService(QString cliPath, QString lang, QObject* parent)
    : QObject(parent), m_cliPath(std::move(cliPath)), m_lang(std::move(lang)) {
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, &CliService::onOutput);
    connect(&m_proc, &QProcess::finished, this, &CliService::onFinished);
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_failed = true;
            failAll(L("could not start %1", "no se pudo arrancar %1").arg(m_cliPath));
            Q_EMIT failed(L("could not start %1", "no se pudo arrancar %1").arg(m_cliPath), false);
        }
    });
}

CliService::~CliService() {
    m_dataKey.fill('\0');
    // Quiet first: while the window that owns this service is being destroyed, the process
    // ending must not reach anyone. It emitted `failed` («the service ended») into a
    // MainWindow half destroyed, which wrote on its status bar and crashed on every exit
    // (2026-11-22). Pending answers are dropped too: whoever asked is going away.
    m_proc.disconnect(this);
    blockSignals(true);
    for (auto& p : m_queue) {
        finishBusy(p);
    }
    if (m_current) {
        finishBusy(*m_current);
    }
    m_queue.clear();
    m_current.reset();
    if (m_proc.state() != QProcess::NotRunning) {
        // EOF ends the service with code 0; give it a moment before forcing it.
        m_proc.closeWriteChannel();
        // Long enough for the service that announces the interface's presence to say
        // goodbye to each machine; the others end at once.
        if (!m_proc.waitForFinished(m_presence ? 4000 : 1500)) {
            m_proc.kill();
            m_proc.waitForFinished(1000);
        }
    }
}

void CliService::start(const QByteArray& masterPassword, bool isDataKey) {
    QStringList args{QStringLiteral("--serve"), QStringLiteral("--lang"), m_lang};
    if (isDataKey) {
        args << QStringLiteral("--data-key");
    }
    if (m_presence) {
        args << QStringLiteral("--presence")
             << QStringLiteral("gui:%1").arg(QCoreApplication::applicationPid());
    }
    m_proc.start(m_cliPath, args);
    // Written right away: QProcess keeps it until the child reads it. The first line is the
    // master password —or the data key another service opened— (see docs/protocolo_serve.md,
    // «Arranque»).
    QByteArray first = masterPassword;
    first.append('\n');
    m_proc.write(first);
    first.fill('\0');
}

void CliService::request(const QString& line, QObject* owner, Done done, const QString& format,
                         bool confirmed) {
    m_queue.push_back(Pending{m_nextId++, line, format, owner, std::move(done)});
    m_queue.back().confirmed = confirmed;
    // Made while a click or a key is handled: the person's, and the cursor says «working»
    // until the answer —or a question— arrives. Without it a slow command looked ignored,
    // and the button got pressed again.
    if (BusyHooks::userInput && BusyHooks::userInput()) {
        holdBusy(m_queue.back());
        if (BusyHooks::begin) {
            m_queue.back().token = BusyHooks::begin();
        }
    }
    sendNext();
}

void CliService::sendNext() {
    if (!m_ready || m_current || m_queue.empty()) {
        return;
    }
    m_current = std::move(m_queue.front());
    m_queue.pop_front();
    m_pausedForUser = false;
    QJsonObject req;
    req.insert(QStringLiteral("id"), m_current->id);
    req.insert(QStringLiteral("line"), m_current->line);
    req.insert(QStringLiteral("format"), m_current->format);
    write(req);
}

void CliService::resumeAfterQuestion() {
    if (m_pausedForUser && m_current) {
        m_pausedForUser = false;
        holdBusy(*m_current);
    }
}

void CliService::answerConfirm(int id, bool yes, const QString& typed) {
    resumeAfterQuestion();
    QJsonObject a;
    a.insert(QStringLiteral("id"), id);
    a.insert(QStringLiteral("answer"), yes);
    if (!typed.isEmpty()) {
        a.insert(QStringLiteral("typed"), typed);
    }
    write(a);
}

void CliService::unlockWithPassword(const QByteArray& password) {
    QJsonObject a;
    a.insert(QStringLiteral("unlock"), QStringLiteral("password"));
    a.insert(QStringLiteral("password"), QString::fromUtf8(password));
    write(a);
}

void CliService::answerSecret(int id, const std::optional<QString>& secret) {
    resumeAfterQuestion();
    QJsonObject a;
    a.insert(QStringLiteral("id"), id);
    a.insert(QStringLiteral("secret"), secret ? QJsonValue(*secret) : QJsonValue());
    write(a);
}

void CliService::write(const QJsonObject& obj) {
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    line.append('\n');
    m_proc.write(line);
}

void CliService::onOutput() {
    m_buffer.append(m_proc.readAllStandardOutput());
    qsizetype nl = 0;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject()) {
            handleEvent(doc.object());
        }
    }
}

void CliService::handleEvent(const QJsonObject& ev) {
    const QString type = ev.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("ready")) {
        m_ready = true;
        m_version = ev.value(QStringLiteral("version")).toString();
        m_noMaster = ev.value(QStringLiteral("no_master")).toBool();
        m_dataKey = ev.value(QStringLiteral("data_key")).toString().toUtf8();
        Q_EMIT ready(ev.value(QStringLiteral("warning")).toString());
        sendNext();
        return;
    }
    // Locked: keys are registered and no password came (see servicioCerrado in the CLI).
    if (type == QLatin1String("locked")) {
        Q_EMIT locked(ev.value(QStringLiteral("key_present")).toBool());
        return;
    }
    if (type == QLatin1String("touch")) {
        Q_EMIT touchWaiting();
        return;
    }
    if (type == QLatin1String("key_present") || type == QLatin1String("key_absent")) {
        Q_EMIT keyPresence(type == QLatin1String("key_present"));
        return;
    }
    if (type == QLatin1String("close_asked")) {
        Q_EMIT closeAsked(ev.value(QStringLiteral("host")).toString(),
                          ev.value(QStringLiteral("user")).toString());
        return;
    }
    if (type == QLatin1String("unlock_failed")) {
        Q_EMIT unlockFailed(ev.value(QStringLiteral("reason")).toString(),
                            ev.value(QStringLiteral("code")).toString());
        return;
    }
    if (type == QLatin1String("error")) {
        // Only at start-up: the master password does not open the secrets.
        m_failed = true;
        const QString text = ev.value(QStringLiteral("text")).toString();
        failAll(text);
        Q_EMIT failed(text, text.contains(QLatin1String("master password")));
        return;
    }
    if (type == QLatin1String("result")) {
        if (!m_current) {
            return;
        }
        CliResult r;
        r.rc = ev.value(QStringLiteral("rc")).toInt(-1);
        r.out = ev.value(QStringLiteral("stdout")).toString();
        r.err = ev.value(QStringLiteral("stderr")).toString();
        Pending done = std::move(*m_current);
        m_current.reset();
        m_pausedForUser = false;
        finishBusy(done);  // before `done`: what it opens is not «working»
        if (done.done && done.owner) {
            done.done(r);
        }
        sendNext();
        return;
    }
    if (type == QLatin1String("confirm")) {
        CliQuestion q;
        q.id = ev.value(QStringLiteral("id")).toInt();
        q.title = ev.value(QStringLiteral("title")).toString();
        q.text = ev.value(QStringLiteral("text")).toString();
        q.irreversible = ev.value(QStringLiteral("irreversible")).toBool();
        q.lose = stringsOf(ev.value(QStringLiteral("lose")));
        q.keep = stringsOf(ev.value(QStringLiteral("keep")));
        q.notes = stringsOf(ev.value(QStringLiteral("notes")));
        q.typeToConfirm = ev.value(QStringLiteral("type_to_confirm")).toString();
        if (q.title.isEmpty()) {
            q.title = q.text;
        }
        if (m_current && m_current->id == q.id && m_current->confirmed && !q.irreversible
            && q.typeToConfirm.isEmpty()) {
            answerConfirm(q.id, true);
            return;
        }
        pauseForQuestion();
        Q_EMIT confirmRequested(q);
        return;
    }
    if (type == QLatin1String("secret")) {
        pauseForQuestion();
        Q_EMIT secretRequested(ev.value(QStringLiteral("id")).toInt(),
                               ev.value(QStringLiteral("text")).toString());
        return;
    }
}

// A question is open: it is the person's turn, not «working».
void CliService::pauseForQuestion() {
    if (m_current && m_current->busy) {
        releaseBusy(*m_current);
        m_pausedForUser = true;
    }
}

void CliService::onFinished() {
    m_ready = false;
    if (!m_failed) {
        m_failed = true;
        const QString why = QString::fromUtf8(m_proc.readAllStandardError()).trimmed();
        failAll(why.isEmpty() ? L("the service ended", "el servicio terminó") : why);
        Q_EMIT failed(why.isEmpty() ? L("the service ended", "el servicio terminó") : why, false);
    }
}

void CliService::failAll(const QString& reason) {
    CliResult r;
    r.rc = -1;
    r.err = reason;
    std::deque<Pending> all;
    if (m_current) {
        all.push_back(std::move(*m_current));
        m_current.reset();
    }
    for (auto& p : m_queue) {
        all.push_back(std::move(p));
    }
    m_queue.clear();
    m_pausedForUser = false;
    for (auto& p : all) {
        finishBusy(p);
        if (p.done && p.owner) {
            p.done(r);
        }
    }
}

}  // namespace zfsmgr::gui
