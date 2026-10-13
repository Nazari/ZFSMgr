#include "mainwindow.h"

#include "commands/zfsmurl.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>

// Every action of the window runs a line of the shell through CliService. The window only
// gathers what the user chose and shows what the shell answers: the rules, the checks and the
// confirmation texts are the shell's.

void MainWindow::initCliService() {
    m_cli = new CliService(this);
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
    m_cli->setConfirm([this](const QString& pregunta) {
        const bool si =
            QMessageBox::question(this, QStringLiteral("ZFSMgr"), pregunta) == QMessageBox::Yes;
        m_cliRefused = !si;
        return si;
    });
}

void MainWindow::runCli(const QString& line, std::function<void(const CliService::Result&)> done) {
    // The log shows each line as it would be typed in the shell.
    appLog(QStringLiteral("NORMAL"), QStringLiteral("$ %1").arg(line));
    updateStatus(line);
    m_cliRefused = false;
    m_cli->run(line, [this, line, done](const CliService::Result& r) {
        updateStatus(QString());
        const QString mensaje = r.err.trimmed();
        if (r.rc == 0) {
            if (!mensaje.isEmpty()) {
                appLog(QStringLiteral("INFO"), mensaje);
            }
        } else if (!m_cliRefused) {
            appLog(QStringLiteral("ERROR"), mensaje.isEmpty() ? line : mensaje);
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 mensaje.isEmpty() ? QStringLiteral("rc=%1").arg(r.rc) : mensaje);
        }
        if (done) {
            done(r);
        }
    });
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

void MainWindow::refreshAfterCli(const DatasetSelectionContext& ctx) {
    PendingShellActionDraft d;
    d.refreshScope = PendingShellActionDraft::RefreshScope::TargetOnly;
    d.refreshTarget = ctx;
    refreshPendingShellActionDraft(d);
}
