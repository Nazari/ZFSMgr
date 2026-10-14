#include "mainwindow.h"

#include "commands/zfsmurl.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>

// Every action of the window runs a line of the shell through CliService. The window only
// gathers what the user chose and shows what the shell answers: the rules, the checks and the
// confirmation texts are the shell's.

void MainWindow::initCliService() {
    m_cli = new CliService(this);
    m_cliReader = new CliReader(this);
#ifdef _WIN32
    const QString nombre = QStringLiteral("zfsmgr-cli.exe");
#else
    const QString nombre = QStringLiteral("zfsmgr-cli");
#endif
    // Next to this program first: the shell of the same build. Otherwise, the one in PATH.
    const QString alLado = QDir(QCoreApplication::applicationDirPath()).filePath(nombre);
    const QString ejecutable = QFileInfo::exists(alLado) ? alLado : nombre;
    m_cli->configure(ejecutable,
                     {QStringLiteral("--serve"), QStringLiteral("--lang"), m_language,
                      QStringLiteral("--config-dir"), m_conns.store.configDir()},
                     m_conns.store.masterPassword());
    m_cliReader->configure(ejecutable,
                           {QStringLiteral("--serve"), QStringLiteral("--lang"), m_language,
                            QStringLiteral("--config-dir"), m_conns.store.configDir()},
                           m_conns.store.masterPassword());
    // In the tests, no real shell unless the test gives one: it would read the user's machines.
    const QByteArray modoPrueba = qgetenv("ZFSMGR_TEST_MODE");
    if (!modoPrueba.isEmpty() && modoPrueba != "0") {
        const auto sinShell = [](const QString&) {
            CliService::Result r;
            r.rc = -1;
            r.err = QStringLiteral("no shell in the tests");
            return r;
        };
        m_cli->setFakeForTest(sinShell);
        m_cliReader->setFakeForTest(sinShell);
    }
    m_cli->setAskSecret([this](const QString& prompt, QString& secreto) {
        if (m_cliPresetSecret) {
            secreto = *m_cliPresetSecret;  // every ask of that line: a passphrase is asked twice
            return true;
        }
        bool ok = false;
        secreto = QInputDialog::getText(this, QStringLiteral("ZFSMgr"), prompt, QLineEdit::Password,
                                        QString(), &ok);
        m_cliRefused = !ok;
        return ok;
    });
    m_cli->setConfirm([this](const QString& pregunta) {
        const bool si =
            QMessageBox::question(this, QStringLiteral("ZFSMgr"), pregunta) == QMessageBox::Yes;
        m_cliRefused = !si;
        return si;
    });
}

void MainWindow::runCli(const QString& line, std::function<void(const CliService::Result&)> done,
                        const QString& format) {
    // The log shows each line as it would be typed in the shell.
    appLog(QStringLiteral("NORMAL"), QStringLiteral("$ %1").arg(line));
    updateStatus(line);
    m_cliRefused = false;
    m_cli->run(
        line,
        [this, line, done](const CliService::Result& r) {
            updateStatus(QString());
            const QString mensaje = r.err.trimmed();
            if (r.rc == 0) {
                if (!mensaje.isEmpty()) {
                    appLog(QStringLiteral("INFO"), mensaje);
                }
            } else if (!m_cliRefused) {
                appLog(QStringLiteral("ERROR"), mensaje.isEmpty() ? line : mensaje);
                QMessageBox::warning(
                    this, QStringLiteral("ZFSMgr"),
                    mensaje.isEmpty() ? QStringLiteral("rc=%1").arg(r.rc) : mensaje);
            }
            if (done) {
                done(r);
            }
        },
        format);
}

QString MainWindow::zfsmUrl(int connIdx, const QString& dataset, const QString& snapshot) const {
    namespace Z = zfsmgr::base;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return QString();
    }
    const ConnectionProfile& p = m_conns.profiles[connIdx];
    Z::ZfsmUrl u;
    u.connection = (p.id.isEmpty() ? p.name : p.id).toStdString();
    u.kind = Z::ZfsmKind::Connection;
    if (!dataset.isEmpty()) {
        u.dataset = dataset.toStdString();
        u.pool = dataset.section(QLatin1Char('/'), 0, 0).toStdString();
        u.kind = Z::ZfsmKind::Dataset;
        if (!snapshot.isEmpty()) {
            u.snapshot = snapshot.toStdString();
            u.kind = Z::ZfsmKind::Snapshot;
        }
    }
    return QString::fromStdString(Z::formatZfsmUrl(u));
}

bool MainWindow::cliReadJson(int connIdx, const QString& line, QJsonObject& out, QString* error,
                             int timeoutMs) {
    const QString clave = cliConnName(connIdx);
    const CliService::Result r =
        m_cliReader->read(clave.isEmpty() ? QStringLiteral("-") : clave, line, timeoutMs);
    if (r.rc != 0) {
        if (error) {
            *error =
                r.err.trimmed().isEmpty() ? QStringLiteral("rc=%1").arg(r.rc) : r.err.trimmed();
        }
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(r.out.toUtf8(), &pe);
    if (!doc.isObject()) {
        if (error) {
            *error = QStringLiteral("%1: %2").arg(line, pe.errorString());
        }
        return false;
    }
    out = doc.object();
    return true;
}

QString MainWindow::cliConnName(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return QString();
    }
    const ConnectionProfile& p = m_conns.profiles[connIdx];
    return cliQuote(p.id.isEmpty() ? p.name : p.id);
}

QString MainWindow::cliConnectionOptions(const ConnectionProfile& p, bool editing) {
    QStringList o;
    const auto pon = [&o](const char* opcion, const QString& valor) {
        if (!valor.trimmed().isEmpty()) {
            o << QLatin1String(opcion) << cliQuote(valor.trimmed());
        }
    };
    pon("--name", p.name);
    pon("--type", p.connType);
    pon("--os", p.osType);
    pon("--host", p.host);
    if (p.port > 0) {
        pon("--port", QString::number(p.port));
    }
    pon("--user", p.username);
    if (!p.keyPath.trimmed().isEmpty()) {
        pon("--key", p.keyPath);
    } else if (editing) {
        o << QStringLiteral("--no-key");
    }
    o << (p.useSudo ? QStringLiteral("--sudo")
                    : (editing ? QStringLiteral("--no-sudo") : QString()));
    pon("--family", p.sshAddressFamily.isEmpty() ? QStringLiteral("auto") : p.sshAddressFamily);
    o.removeAll(QString());
    return o.join(QLatin1Char(' '));
}

void MainWindow::refreshAfterCli(const DatasetSelectionContext& ctx) {
    if (!m_refreshAfterCli) {
        return;
    }
    PendingShellActionDraft d;
    d.refreshScope = PendingShellActionDraft::RefreshScope::TargetOnly;
    d.refreshTarget = ctx;
    refreshPendingShellActionDraft(d);
}

QString MainWindow::cliQuote(const QString& value) {
    static const QString kSeguros =
        QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.,:/+@%");
    bool simple = !value.isEmpty();
    for (const QChar c : value) {
        if (!kSeguros.contains(c)) {
            simple = false;
            break;
        }
    }
    if (simple) {
        return value;
    }
    QString esc = value;
    esc.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    esc.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + esc + QLatin1Char('"');
}
