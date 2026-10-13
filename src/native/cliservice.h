#pragma once

#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

// The shell as a service, driven by the graphical interface: `zfsmgr-cli --serve`.
//
// Every action of the interface runs a line of the shell here —`destroy zfsm://…`,
// `set compression=zstd --on zfsm://…`— so the interface does exactly what the shell does,
// with the same rules, checks and messages, and carries no logic of its own. A confirmation
// the command asks for reaches the interface with the shell's own text.
//
// Lines run one at a time, in order. The process is started on the first line and started
// again if it dies.
class CliService : public QObject {
    Q_OBJECT
public:
    struct Result {
        int rc{-1};
        QString out;  // the command's standard output (JSON with --format json)
        QString err;  // its standard error: messages and errors, in the user's language
    };
    using Done = std::function<void(const Result&)>;
    using Confirm = std::function<bool(const QString& question)>;

    explicit CliService(QObject* parent = nullptr);
    ~CliService() override;

    // How to start the service; nothing is started until the first line.
    void configure(const QString& executable, const QStringList& arguments, const QString& master);
    void setConfirm(Confirm confirm) { m_confirm = std::move(confirm); }
    // A secret the command asks for (a passphrase): false to refuse.
    using AskSecret = std::function<bool(const QString& prompt, QString& secret)>;
    void setAskSecret(AskSecret ask) { m_askSecret = std::move(ask); }
    void run(const QString& line, Done done);

    // For the tests: no process; each line is answered by `fake`, and remembered.
    using Fake = std::function<Result(const QString& line)>;
    void setFakeForTest(Fake fake) { m_fake = std::move(fake); }
    const QStringList& linesForTest() const { return m_lines; }

Q_SIGNALS:
    void lineStarted(const QString& line);

private:
    struct Pending {
        int id{0};
        QString line;
        Done done;
    };
    void startProcess();
    void sendNext();
    void onOutput();
    void failAll(const QString& why);

    QString m_executable;
    QStringList m_arguments;
    QString m_master;
    Confirm m_confirm;
    AskSecret m_askSecret;
    Fake m_fake;
    QStringList m_lines;
    QProcess* m_proc{nullptr};
    bool m_ready{false};
    bool m_busy{false};
    int m_nextId{1};
    QQueue<Pending> m_queue;
    Pending m_current;
    QByteArray m_buffer;
};
