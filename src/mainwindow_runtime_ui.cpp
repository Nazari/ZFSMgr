#include "mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTabBar>
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

void MainWindow::updateConnectivityMatrixButtonState() {
    if (!m_connectivityMatrixBtn) {
        return;
    }
    const bool enabled = !m_refreshInProgress && !m_connectivityMatrixInProgress;
    m_connectivityMatrixBtn->setEnabled(enabled);
}

void MainWindow::setActionsLocked(bool locked) {
    auto captureTabSelection = [this](QTabBar* tabs, QMap<int, QString>& outMap) {
        if (!tabs) {
            return;
        }
        const int t = tabs->currentIndex();
        if (t < 0 || t >= tabs->count()) {
            return;
        }
        const QString key = tabs->tabData(t).toString();
        const QStringList parts = key.split(':');
        if (parts.size() < 3 || parts.value(0) != QStringLiteral("pool")) {
            return;
        }
        bool ok = false;
        const int connIdx = parts.value(1).toInt(&ok);
        if (!ok || connIdx < 0) {
            return;
        }
        outMap[connIdx] = key;
    };
    auto restoreTabSelection = [this](QTabBar* tabs, QMap<int, QString>& map, int connIdx) {
        if (!tabs || connIdx < 0) {
            return;
        }
        const QString wanted = map.value(connIdx).trimmed();
        if (wanted.isEmpty()) {
            return;
        }
        for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->tabData(i).toString() == wanted) {
                tabs->setCurrentIndex(i);
                break;
            }
        }
    };

    if (locked) {
        captureTabSelection(m_connectionEntityTabs, m_pendingRefreshTopTabDataByConn);
        captureTabSelection(m_bottomConnectionEntityTabs, m_pendingRefreshBottomTabDataByConn);
    }

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
    if (m_connPropsRefreshBtn) {
        const bool can = m_connPropsRefreshBtn->property("zfsmgr_can_conn_action").toBool();
        m_connPropsRefreshBtn->setEnabled(!locked && can);
    }
    if (m_connPropsEditBtn) {
        const bool can = m_connPropsEditBtn->property("zfsmgr_can_conn_action").toBool();
        m_connPropsEditBtn->setEnabled(!locked && can);
    }
    if (m_connPropsDeleteBtn) {
        const bool can = m_connPropsDeleteBtn->property("zfsmgr_can_conn_action").toBool();
        m_connPropsDeleteBtn->setEnabled(!locked && can);
    }
    if (!locked) {
        restoreTabSelection(m_connectionEntityTabs, m_pendingRefreshTopTabDataByConn, m_topDetailConnIdx);
        restoreTabSelection(m_bottomConnectionEntityTabs, m_pendingRefreshBottomTabDataByConn, m_bottomDetailConnIdx);
        m_pendingRefreshTopTabDataByConn.remove(m_topDetailConnIdx);
        m_pendingRefreshBottomTabDataByConn.remove(m_bottomDetailConnIdx);
        m_activeConnActionBtn = nullptr;
        m_activeConnActionName.clear();
        updateApplyPropsButtonState();
        refreshSelectedPoolDetails(false, false);
        updatePoolManagementBoxTitle();
    }
    updateConnectionActionsState();
}
