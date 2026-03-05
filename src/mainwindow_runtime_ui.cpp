#include "mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTextEdit>
#include <QThread>

void MainWindow::updateStatus(const QString& text) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, text]() {
            updateStatus(text);
        }, Qt::QueuedConnection);
        return;
    }
    if (m_statusText) {
        m_statusText->setPlainText(maskSecrets(text));
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
    if (m_actionsLocked) {
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

void MainWindow::updateBusyCursor() {
    const bool shouldShow = m_actionsLocked || m_refreshInProgress || (m_uiBusyDepth > 0);
    if (shouldShow) {
        if (!m_waitCursorActive) {
            QApplication::setOverrideCursor(Qt::BusyCursor);
            m_waitCursorActive = true;
        }
    } else if (m_waitCursorActive) {
        QApplication::restoreOverrideCursor();
        m_waitCursorActive = false;
    }
}

void MainWindow::setActionsLocked(bool locked) {
    m_actionsLocked = locked;
    updateBusyCursor();
    if (m_logCancelBtn) {
        m_logCancelBtn->setVisible(locked);
        m_logCancelBtn->setEnabled(locked);
    }
    if (m_btnNew) m_btnNew->setEnabled(!locked);
    if (m_btnRefreshAll) m_btnRefreshAll->setEnabled(!locked);
    if (m_btnPoolNew) m_btnPoolNew->setEnabled(!locked && selectedConnectionIndexForPoolManagement() >= 0);
    if (m_poolStatusRefreshBtn) {
        const bool canRefresh = m_poolStatusRefreshBtn->property("zfsmgr_can_refresh").toBool();
        m_poolStatusRefreshBtn->setEnabled(!locked && canRefresh);
    }
    if (m_poolStatusImportBtn) m_poolStatusImportBtn->setEnabled(!locked && m_poolStatusImportBtn->isEnabled());
    if (m_poolStatusExportBtn) m_poolStatusExportBtn->setEnabled(!locked && m_poolStatusExportBtn->isEnabled());
    if (m_poolStatusScrubBtn) m_poolStatusScrubBtn->setEnabled(!locked && m_poolStatusScrubBtn->isEnabled());
    if (m_poolStatusDestroyBtn) m_poolStatusDestroyBtn->setEnabled(!locked && m_poolStatusDestroyBtn->isEnabled());
    if (m_btnApplyDatasetProps) m_btnApplyDatasetProps->setEnabled(!locked && m_btnApplyDatasetProps->isEnabled());
    if (m_btnApplyAdvancedProps) m_btnApplyAdvancedProps->setEnabled(!locked && m_btnApplyAdvancedProps->isEnabled());
    if (locked) {
        if (m_btnCopy) m_btnCopy->setEnabled(false);
        if (m_btnLevel) m_btnLevel->setEnabled(false);
        if (m_btnSync) m_btnSync->setEnabled(false);
        if (m_btnAdvancedBreakdown) m_btnAdvancedBreakdown->setEnabled(false);
        if (m_btnAdvancedAssemble) m_btnAdvancedAssemble->setEnabled(false);
        if (m_btnAdvancedFromDir) m_btnAdvancedFromDir->setEnabled(false);
        if (m_btnAdvancedToDir) m_btnAdvancedToDir->setEnabled(false);
    } else {
        updateTransferButtonsState();
        updateApplyPropsButtonState();
        refreshSelectedPoolDetails();
        updatePoolManagementBoxTitle();
    }
}
