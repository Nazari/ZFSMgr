#pragma once

// One `zfsmgr-cli --serve` process, and the only way the interface acts or reads.
//
// The contract is docs/protocolo_serve.md. This class speaks it and nothing else: it does
// not open ssh, read config.json or decide any ZFS rule. Everything is asynchronous —a
// request is queued and its answer arrives through a callback— and nothing pumps the event
// loop, so a slow machine never freezes a window.
//
// The service runs ONE command at a time, so requests are queued here and the next one is
// written only when the previous result arrives. To do things in parallel, several services:
// one per machine for its reads.

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <deque>
#include <functional>
#include <optional>

namespace zfsmgr::gui {

struct CliResult {
    int rc{-1};
    QString out;  // stdout: one JSON document with format «json»
    QString err;  // warnings and errors, already written for a person
    bool ok() const { return rc == 0; }
};

// A `confirm` event, field by field. The text is the CLI's: shown as it comes.
struct CliQuestion {
    int id{0};
    QString title;
    QString text;
    bool irreversible{false};
    QStringList lose;
    QStringList keep;
    QStringList notes;
    QString typeToConfirm;  // non-empty: the answer must carry this word, typed by the person
};

// The busy cursor and the pressed button, from outside: this class links only QtCore, both
// are QtGui/QtWidgets. main.cpp sets the hooks.
//   userInput  true while a click or a key is being handled: a request made right then is
//              the person's.
//   adjust     +1 / -1 on the count that shows the busy cursor. Given back while a question
//              is open —the person's turn— and taken again when it is answered.
//   begin/end  the control that was pressed stays disabled from the request to its answer,
//              question included: pressing it twice launched the command twice. `begin`
//              returns a token for `end`.
struct BusyHooks {
    static std::function<bool()> userInput;
    static std::function<void(int)> adjust;
    static std::function<int()> begin;
    static std::function<void(int)> end;
};

class CliService : public QObject {
    Q_OBJECT
public:
    // `cliPath`: the zfsmgr-cli executable. `lang`: «es» or «en», for every text the CLI writes.
    CliService(QString cliPath, QString lang, QObject* parent = nullptr);
    ~CliService() override;

    // Starts the process and hands it the master password as the first line of its stdin
    // (empty when nothing is encrypted). Never through argv or the environment.
    void start(const QByteArray& masterPassword);
    bool isReady() const { return m_ready; }
    // The zfsmgr-cli version this service runs, as its `ready` said.
    const QString& cliVersion() const { return m_version; }
    // Its `ready` said there is no master password yet: a first run, or a configuration that
    // never had one. Without one no password can be stored.
    bool noMaster() const { return m_noMaster; }

    using Done = std::function<void(const CliResult&)>;
    // Queues one command line, exactly as it would be typed in the terminal.
    //
    // `owner`: who asked. If it no longer exists when the answer arrives —a dialog closed
    // while its machine was still answering— the answer is DROPPED: `done` captures that
    // owner, and calling it would write into a widget already deleted (a crash, 2026-11-22:
    // closing «Add a machine» before its check came back). Required on purpose, so no
    // caller can forget it.
    void request(const QString& line, QObject* owner, Done done,
                 const QString& format = QStringLiteral("json"));

    void answerConfirm(int id, bool yes, const QString& typed = {});
    void answerSecret(int id, const std::optional<QString>& secret);

Q_SIGNALS:
    void ready(const QString& warning);
    // The service could not start or died: wrong master password, missing executable…
    void failed(const QString& reason, bool wrongPassword);
    void confirmRequested(const zfsmgr::gui::CliQuestion& question);
    void secretRequested(int id, const QString& text);

private:
    struct Pending {
        int id{0};
        QString line;
        QString format;
        QPointer<QObject> owner;
        Done done;
        bool busy{false};  // holds one count of the busy cursor
        int token{0};      // BusyHooks::begin's, for the control disabled meanwhile
    };

    static void holdBusy(Pending& p);
    static void releaseBusy(Pending& p);
    static void finishBusy(Pending& p);  // the cursor and the control, both given back

    void onOutput();
    void onFinished();
    void handleEvent(const QJsonObject& ev);
    void sendNext();
    void pauseForQuestion();
    void resumeAfterQuestion();
    void failAll(const QString& reason);
    void write(const QJsonObject& obj);

    QString m_cliPath;
    QString m_lang;
    QProcess m_proc;
    QByteArray m_buffer;
    bool m_ready{false};
    QString m_version;
    bool m_noMaster{false};
    bool m_failed{false};
    int m_nextId{1};
    std::deque<Pending> m_queue;
    std::optional<Pending> m_current;
    bool m_pausedForUser{false};  // m_current's busy count given back while a question is open
};

// Where zfsmgr-cli is: next to this executable, else on PATH. Empty if nowhere.
QString findCli();

}  // namespace zfsmgr::gui
