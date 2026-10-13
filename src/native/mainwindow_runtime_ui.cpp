#include "mainwindow.h"
#include "base/processes.h"

#include <QApplication>
#include <QThreadPool>
#include <QCoreApplication>
#include <QGroupBox>
#include <QCloseEvent>
#include <QMessageBox>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QThread>
#include <QTreeWidget>

MainWindow::~MainWindow() {
    m_closing = true;
    m_destroying = true;
    // Dropping the posted events also releases any pool task blocked on a call onto this
    // thread (a blocking queued call is freed with its event).
    QCoreApplication::removePostedEvents(this);
    // The QtConcurrent tasks capture `this`. One still running when the members go away calls
    // into freed memory: the intermittent SIGSEGV of zfsmgr_gui_mainwindow_test. They are
    // waited for here, with a limit so a machine that does not answer cannot hold the exit.
    // In short slices, dropping the posted events again each time: a task that checked
    // m_destroying just before it was set may still queue a blocking call.
    for (int i = 0; i < 150 && !QThreadPool::globalInstance()->waitForDone(100); ++i) {
        QCoreApplication::removePostedEvents(this);
    }
    stopAllDaemonEventWatchers();
    closeAllRemoteDaemonRpcTunnels();

    // Barrido final de procesos hijo, y NO es redundante con la línea de arriba: aquella
    // solo recorre el mapa de túneles, así que cualquier QProcess que no esté registrado
    // en él sobrevive. Eso es exactamente lo que pasaba con los túneles duplicados por
    // reentrancia, y el resultado era una violación del montículo confirmada con
    // AddressSanitizer:
    //
    //   ~MainWindow()  → cuerpo del destructor
    //     → se destruyen los MIEMBROS      (muere m_transport.tunnelsByConnKey)
    //     → ~QWidget() → deleteChildren()  (mueren los QProcess que quedaban)
    //         → ~QProcess() emite finished()
    //             → la lambda hace .find() sobre el mapa YA LIBERADO
    //
    // Es el orden de destrucción de C++: los miembros de la derivada mueren antes que la
    // base, y los hijos QObject los borra la base. Por eso hay que matarlos AQUÍ, en el
    // cuerpo del destructor, mientras los miembros siguen vivos; y desconectarlos de
    // `this` antes, para que morir no dispare nada.
    //
    // Se borran en el acto, sin deleteLater: a estas alturas ya no queda ciclo de
    // eventos que llegue a procesarlo.
    const QList<QProcess*> strayProcesses = findChildren<QProcess*>(QString(), Qt::FindDirectChildrenOnly);
    for (QProcess* proc : strayProcesses) {
        if (!proc) {
            continue;
        }
        disconnect(proc, nullptr, this, nullptr);
        proc->blockSignals(true);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(1000);
        }
        delete proc;
    }

    auto quiesceObject = [](QObject* obj) {
        if (!obj) {
            return;
        }
        obj->blockSignals(true);
        QCoreApplication::removePostedEvents(obj);
    };

    quiesceObject(m_connContentTree);
    quiesceObject(m_connContentPropsTable);
    quiesceObject(m_pendingChangesList);
    quiesceObject(m_transfersBox);
}

void MainWindow::updateStatus(const QString& text) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, text]() {
            updateStatus(text);
        }, Qt::QueuedConnection);
        return;
    }
    // What is being done now. «Ready» is not shown: the bar says only what is not routine.
    const QString masked = maskSecrets(text.trimmed());
    const QString ahora = masked.isEmpty() ? defaultStatusTextForCurrentState() : masked;
    m_statusActivity = (ahora == defaultIdleStatusText()) ? QString() : ahora;
    composeStatusBar();
}

bool MainWindow::actionsLocked() const {
    return m_actionsLocked;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    m_closing = true;
    flushAppLogFile();
    // Salir sin aplicar ya no pierde la lista. Se guarda también en cada cambio, así que
    // esto es la red por si el proceso se va por otro camino; barato y sin secretos
    // dentro (ver mainwindow_pending_store.cpp).
    if (m_actionsLocked) {
        m_closing = false;
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_close_block_001"),
                QStringLiteral("Hay una acción en ejecución. Cancele la acción antes de cerrar la aplicación."),
                QStringLiteral("An action is running. Cancel it before closing the application.")));
        event->ignore();
        return;
    }
    if (m_refreshInProgress) {
        m_closing = false;
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_close_refresh_block_001"),
                QStringLiteral("Hay un refresco de conexiones en curso. Espere a que termine antes de cerrar la aplicación."),
                QStringLiteral("A connection refresh is in progress. Wait for it to finish before closing the application.")));
        event->ignore();
        return;
    }
    // Warn if daemon background jobs are still running
    {
        int runningJobs = 0;
        for (const ActiveDaemonJob& j : m_activeDaemonJobs) {
            if (j.state == QStringLiteral("running")) ++runningJobs;
        }
        if (runningJobs > 0) {
            const auto choice = QMessageBox::question(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_close_jobs_running001"),
                    QStringLiteral("Hay %1 transferencia(s) ejecutándose en el daemon.\n"
                                   "¿Cerrar de todas formas? Seguirán en segundo plano."),
                    QStringLiteral("%1 transfer(s) are still running in the daemon.\n"
                                   "Close anyway? They will keep running in the background."))
                    .arg(runningJobs),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (choice != QMessageBox::Yes) {
                m_closing = false;
                event->ignore();
                return;
            }
        }
    }
    saveUiSettings();
    closeAllRemoteDaemonRpcTunnels();
    closeAllSshControlMasters();
    QMainWindow::closeEvent(event);
}

void MainWindow::beginUiBusy() {
    ++m_uiBusyDepth;
    updateBusyCursor();
}

void MainWindow::endUiBusy() {
    if (m_uiBusyDepth > 0) {
        --m_uiBusyDepth;
    }
    updateBusyCursor();
}

void MainWindow::beginTransientUiBusy(const QString& statusText) {
    m_transientStatusStack.push_back(m_statusActivity);
    m_statusLastError.clear();  // a new action: the previous error no longer applies
    beginUiBusy();
    if (!statusText.trimmed().isEmpty()) {
        updateStatus(statusText);
    }
}

void MainWindow::endTransientUiBusy() {
    const QString previous = m_transientStatusStack.isEmpty() ? QString() : m_transientStatusStack.takeLast();
    endUiBusy();
    updateStatus(previous);
}

QString MainWindow::defaultStatusTextForCurrentState() const {
    if (!m_initialRefreshCompleted) {
        return trk(QStringLiteral("t_status_loading_001"),
                   QStringLiteral("Loading..."),
                   QStringLiteral("Loading..."));
    }
    if (m_refreshInProgress) {
        return trk(QStringLiteral("t_status_refreshing_001"),
                   QStringLiteral("Refreshing connections..."),
                   QStringLiteral("Refreshing connections..."));
    }
    if (m_actionsLocked || m_uiBusyDepth > 0) {
        return trk(QStringLiteral("t_status_busy_001"),
                   QStringLiteral("Working..."),
                   QStringLiteral("Working..."));
    }
    return defaultIdleStatusText();
}

QString MainWindow::defaultIdleStatusText() const {
    return trk(QStringLiteral("t_status_ready_001"), QStringLiteral("Ready"),
               QStringLiteral("Ready"));
}

void MainWindow::updateBusyCursor() {
    const bool shouldShow = m_actionsLocked || m_refreshInProgress || (m_uiBusyDepth > 0);
    if (shouldShow) {
        if (!m_waitCursorActive) {
            QApplication::setOverrideCursor(Qt::BusyCursor);
            m_waitCursorActive = true;
        }
        if (m_statusActivity.isEmpty()) {
            updateStatus(QString());
        }
    } else if (m_waitCursorActive) {
        QApplication::restoreOverrideCursor();
        m_waitCursorActive = false;
        // An automatic text («Working…», «Refreshing…») ends with the work; one an action
        // set stays until the next.
        const QStringList automaticos = {
            trk(QStringLiteral("t_status_loading_001"), QStringLiteral("Loading..."),
                QStringLiteral("Loading...")),
            trk(QStringLiteral("t_status_refreshing_001"),
                QStringLiteral("Refreshing connections..."),
                QStringLiteral("Refreshing connections...")),
            trk(QStringLiteral("t_status_busy_001"), QStringLiteral("Working..."),
                QStringLiteral("Working..."))};
        if (m_statusActivity.isEmpty() || automaticos.contains(m_statusActivity)) {
            updateStatus(QString());
        }
    }
}
