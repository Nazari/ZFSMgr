#include "mainwindow.h"

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
    closeAllRemoteDaemonRpcTunnels();

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
    if (!m_actionsLocked || !m_activeLocalProcess) {
        return;
    }
    m_cancelActionRequested = true;
}

void MainWindow::terminateProcessTree(qint64 rootPid) {
    if (rootPid <= 0) {
        return;
    }
    QProcess::execute(QStringLiteral("sh"), QStringList{
        QStringLiteral("-lc"),
        QStringLiteral("pkill -TERM -P %1 >/dev/null 2>&1 || true; sleep 0.2; pkill -KILL -P %1 >/dev/null 2>&1 || true")
            .arg(rootPid)
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    m_closing = true;
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
        m_activeConnActionName.clear();
        updateApplyPropsButtonState();
        refreshSelectedPoolDetails(false, false);
        updatePoolManagementBoxTitle();
    }
    updateConnectionActionsState();
}
