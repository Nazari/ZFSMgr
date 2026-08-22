#include "mainwindow.h"
#include "base/process.h"

#include <QApplication>
#include <QCoreApplication>
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
    QCoreApplication::removePostedEvents(this);
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
    quiesceObject(m_logsTabs);
}

void MainWindow::updateStatus(const QString& text) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, text]() {
            updateStatus(text);
        }, Qt::QueuedConnection);
        return;
    }
    if (m_statusText) {
        const QString masked = maskSecrets(text.trimmed());
        m_statusText->setPlainText(masked.isEmpty() ? defaultStatusTextForCurrentState() : masked);
    }
}

bool MainWindow::actionsLocked() const {
    return m_actionsLocked;
}

void MainWindow::requestCancelRunningAction() {
    // Antes exigía además un proceso local vivo, y eso dejaba fuera justamente las
    // operaciones largas: Desglosar y Ensamblar se envían como trabajo del daemon, así
    // que no hay proceso local que matar y la petición se descartaba en silencio. Las
    // dos esperas —la del proceso local y la del sondeo del trabajo— consumen esta
    // misma bandera, de modo que basta con que haya una acción en curso.
    if (!m_actionsLocked) {
        return;
    }
    m_cancelActionRequested = true;
}

void MainWindow::terminateProcessTree(qint64 rootPid) {
    if (rootPid <= 0) {
        return;
    }
    // Todo el recorrido vive en la capa base (`base::mataDescendencia`), que además es
    // donde ya estaba la regla de no meter nunca un intérprete por medio.
    //
    // Antes esto era un guion de `sh -lc` que llamaba a `pgrep -P` una vez por proceso y
    // por cada uno de OCHO niveles, y remataba con `sleep 0.3` y dos bucles de `kill`. La
    // versión de ahora lee `ps` una sola vez y usa `kill()`, que es una llamada al sistema
    // y no un proceso: se cambia una decena larga de lanzamientos por uno. El tope de ocho
    // niveles desaparece de paso —no tenía por qué existir— y la rama de Windows, que era
    // un `#ifdef` aquí arriba, se queda dentro de la misma función.
    zfsmgr::base::mataDescendencia(static_cast<long long>(rootPid));
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
                QStringLiteral("An action is running. Cancel it before closing the application."),
                QStringLiteral("当前有操作正在执行。请先取消操作再关闭应用。")));
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
                QStringLiteral("A connection refresh is in progress. Wait for it to finish before closing the application."),
                QStringLiteral("连接刷新正在进行中。请等待其完成后再关闭应用。")));
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
                                   "Close anyway? They will keep running in the background."),
                    QStringLiteral("守护进程中仍有 %1 个传输在执行。\n"
                                   "仍要关闭吗？它们会在后台继续运行。"))
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
    m_transientStatusStack.push_back(m_statusText ? m_statusText->toPlainText() : QString());
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
                   QStringLiteral("Loading..."),
                   QStringLiteral("加载中..."));
    }
    if (m_refreshInProgress) {
        return trk(QStringLiteral("t_status_refreshing_001"),
                   QStringLiteral("Refreshing connections..."),
                   QStringLiteral("Refreshing connections..."),
                   QStringLiteral("正在刷新连接..."));
    }
    if (m_actionsLocked || m_uiBusyDepth > 0) {
        return trk(QStringLiteral("t_status_busy_001"),
                   QStringLiteral("Working..."),
                   QStringLiteral("Working..."),
                   QStringLiteral("处理中..."));
    }
    return trk(QStringLiteral("t_status_ready_001"),
               QStringLiteral("Ready"),
               QStringLiteral("Ready"),
               QStringLiteral("就绪"));
}

void MainWindow::updateBusyCursor() {
    const bool shouldShow = m_actionsLocked || m_refreshInProgress || (m_uiBusyDepth > 0);
    if (shouldShow) {
        if (!m_waitCursorActive) {
            QApplication::setOverrideCursor(Qt::BusyCursor);
            m_waitCursorActive = true;
        }
        if (m_statusText && m_statusText->toPlainText().trimmed().isEmpty()) {
            m_statusText->setPlainText(defaultStatusTextForCurrentState());
        }
    } else if (m_waitCursorActive) {
        QApplication::restoreOverrideCursor();
        m_waitCursorActive = false;
        if (m_statusText && m_statusText->toPlainText().trimmed().isEmpty()) {
            m_statusText->setPlainText(defaultStatusTextForCurrentState());
        }
    } else if (m_statusText && m_statusText->toPlainText().trimmed().isEmpty()) {
        m_statusText->setPlainText(defaultStatusTextForCurrentState());
    }
}

void MainWindow::updateConnectivityMatrixButtonState() {
    if (!m_connectivityMatrixAction) {
        return;
    }
    const bool enabled = !m_refreshInProgress && !m_connectivityMatrixInProgress;
    m_connectivityMatrixAction->setEnabled(enabled);
}

void MainWindow::setActionsLocked(bool locked) {
    m_actionsLocked = locked;
    updateBusyCursor();
    if (m_menuExitAction) {
        m_menuExitAction->setEnabled(!locked);
    }
    if (m_poolStatusRefreshBtn) {
        const bool canRefresh = m_poolStatusRefreshBtn->property("zfsmgr_can_refresh").toBool();
        m_poolStatusRefreshBtn->setEnabled(!locked && canRefresh);
    }
    if (m_poolStatusImportBtn) m_poolStatusImportBtn->setEnabled(!locked && m_poolStatusImportBtn->isEnabled());
    if (m_poolStatusExportBtn) m_poolStatusExportBtn->setEnabled(!locked && m_poolStatusExportBtn->isEnabled());
    if (m_poolStatusScrubBtn) m_poolStatusScrubBtn->setEnabled(!locked && m_poolStatusScrubBtn->isEnabled());
    if (m_poolStatusDestroyBtn) m_poolStatusDestroyBtn->setEnabled(!locked && m_poolStatusDestroyBtn->isEnabled());
    if (m_btnApplyConnContentProps) m_btnApplyConnContentProps->setEnabled(!locked && m_btnApplyConnContentProps->isEnabled());
    if (!locked) {
        updateApplyPropsButtonState();
        refreshSelectedPoolDetails(false, false);
        updatePoolManagementBoxTitle();
    }
    updateConnectionActionsState();
}
