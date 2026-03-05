#include "mainwindow.h"

#include <QAction>
#include <QGroupBox>
#include <QMenu>
#include <QMetaObject>
#include <QPoint>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <QtConcurrent/QtConcurrent>

void MainWindow::refreshAllConnections() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Acción en curso: refresh bloqueado"),
                   QStringLiteral("Action in progress: refresh blocked"),
                   QStringLiteral("操作进行中：刷新被阻止")));
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           tr3(QStringLiteral("Refrescar todas las conexiones"),
               QStringLiteral("Refresh all connections"),
               QStringLiteral("刷新所有连接")));
    if (m_profiles.isEmpty()) {
        m_refreshInProgress = false;
        updateBusyCursor();
        rebuildConnectionList();
        rebuildDatasetPoolSelectors();
        populateAllPoolsTables();
        updateStatus(tr3(QStringLiteral("Estado: refresco finalizado"),
                         QStringLiteral("Status: refresh finished"),
                         QStringLiteral("状态：刷新完成")));
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = m_profiles.size();
    m_refreshTotal = m_profiles.size();
    m_refreshInProgress = (m_refreshPending > 0);
    updateBusyCursor();
    updateStatus(tr3(QStringLiteral("Estado: refrescando 0/%1"),
                     QStringLiteral("Status: refreshing 0/%1"),
                     QStringLiteral("状态：刷新中 0/%1"))
                     .arg(m_refreshTotal));

    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile profile = m_profiles[i];
        (void)QtConcurrent::run([this, generation, i, profile]() {
            const ConnectionRuntimeState state = refreshConnection(profile);
            QMetaObject::invokeMethod(this, [this, generation, i, state]() {
                onAsyncRefreshResult(generation, i, state);
            }, Qt::QueuedConnection);
        });
    }
}

void MainWindow::refreshSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Acción en curso: refresh bloqueado"),
                   QStringLiteral("Action in progress: refresh blocked"),
                   QStringLiteral("操作进行中：刷新被阻止")));
        return;
    }
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    QTreeWidgetItem* selItem = selected.first();
    while (selItem && selItem->parent()) {
        selItem = selItem->parent();
    }
    const int idx = selItem ? selItem->data(0, Qt::UserRole).toInt() : -1;
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = 1;
    m_refreshTotal = 1;
    m_refreshInProgress = true;
    updateBusyCursor();
    updateStatus(tr3(QStringLiteral("Estado: refrescando 0/1"),
                     QStringLiteral("Status: refreshing 0/1"),
                     QStringLiteral("状态：刷新中 0/1")));
    const ConnectionProfile profile = m_profiles[idx];
    (void)QtConcurrent::run([this, generation, idx, profile]() {
        const ConnectionRuntimeState state = refreshConnection(profile);
        QMetaObject::invokeMethod(this, [this, generation, idx, state]() {
            onAsyncRefreshResult(generation, idx, state);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onAsyncRefreshResult(int generation, int idx, const ConnectionRuntimeState& state) {
    if (generation != m_refreshGeneration) {
        return;
    }
    if (idx < 0 || idx >= m_states.size()) {
        return;
    }
    int selectedIdx = -1;
    const auto selected = m_connectionsList ? m_connectionsList->selectedItems() : QList<QTreeWidgetItem*>{};
    if (!selected.isEmpty()) {
        QTreeWidgetItem* selItem = selected.first();
        while (selItem && selItem->parent()) {
            selItem = selItem->parent();
        }
        selectedIdx = selItem ? selItem->data(0, Qt::UserRole).toInt() : -1;
    }
    m_states[idx] = state;
    rebuildConnectionList();
    if (selectedIdx >= 0 && selectedIdx < m_connectionsList->topLevelItemCount()) {
        if (QTreeWidgetItem* top = m_connectionsList->topLevelItem(selectedIdx)) {
            m_connectionsList->setCurrentItem(top);
        }
    }
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
    if (m_refreshPending > 0) {
        --m_refreshPending;
    }
    const int done = qMax(0, m_refreshTotal - m_refreshPending);
    updateStatus(tr3(QStringLiteral("Estado: refrescando %1/%2"),
                     QStringLiteral("Status: refreshing %1/%2"),
                     QStringLiteral("状态：刷新中 %1/%2"))
                     .arg(done)
                     .arg(qMax(1, m_refreshTotal)));
    if (m_refreshPending == 0) {
        onAsyncRefreshDone(generation);
    }
}

void MainWindow::onAsyncRefreshDone(int generation) {
    if (generation != m_refreshGeneration) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refresco paralelo finalizado"));
    m_refreshInProgress = false;
    updateBusyCursor();
    updateStatus(tr3(QStringLiteral("Estado: refresco finalizado"),
                     QStringLiteral("Status: refresh finished"),
                     QStringLiteral("状态：刷新完成")));
}

void MainWindow::onConnectionSelectionChanged() {
    const auto selected = m_connectionsList->selectedItems();
    if (!selected.isEmpty()) {
        QTreeWidgetItem* selItem = selected.first();
        while (selItem && selItem->parent()) {
            selItem = selItem->parent();
        }
        if (selItem && m_connectionsList->currentItem() != selItem) {
            m_connectionsList->setCurrentItem(selItem);
        }
    }
    if (m_leftTabs->currentIndex() == 0 && !m_syncingConnectionFromPoolSelection) {
        populateAllPoolsTables();
    }
    updatePoolManagementBoxTitle();
}

void MainWindow::onPoolsSelectionChanged() {
    const int idx = selectedConnectionIndexForPoolManagement();
    if (idx >= 0 && idx < m_profiles.size() && m_connectionsList) {
        m_syncingConnectionFromPoolSelection = true;
        for (int i = 0; i < m_connectionsList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = m_connectionsList->topLevelItem(i);
            if (!top) {
                continue;
            }
            if (top->data(0, Qt::UserRole).toInt() == idx) {
                m_connectionsList->setCurrentItem(top);
                break;
            }
        }
        m_syncingConnectionFromPoolSelection = false;
    }
    refreshSelectedPoolDetails();
    updatePoolManagementBoxTitle();
}

void MainWindow::onConnectionListContextMenuRequested(const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    QTreeWidgetItem* item = m_connectionsList->itemAt(pos);
    if (item) {
        while (item->parent()) {
            item = item->parent();
        }
        m_connectionsList->setCurrentItem(item);
    }
    const bool hasSel = (m_connectionsList->currentItem() != nullptr);

    QMenu menu(this);
    QAction* refreshAct = menu.addAction(tr3(QStringLiteral("Refrescar"), QStringLiteral("Refresh"), QStringLiteral("刷新")));
    menu.addSeparator();
    QAction* editAct = menu.addAction(tr3(QStringLiteral("Editar"), QStringLiteral("Edit"), QStringLiteral("编辑")));
    QAction* deleteAct = menu.addAction(tr3(QStringLiteral("Borrar"), QStringLiteral("Delete"), QStringLiteral("删除")));
    refreshAct->setEnabled(hasSel);
    editAct->setEnabled(hasSel);
    deleteAct->setEnabled(hasSel);

    QAction* picked = menu.exec(m_connectionsList->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == refreshAct) {
        logUiAction(QStringLiteral("Refrescar conexión (menú)"));
        refreshSelectedConnection();
    } else if (picked == editAct) {
        logUiAction(QStringLiteral("Editar conexión (menú)"));
        editConnection();
    } else if (picked == deleteAct) {
        logUiAction(QStringLiteral("Borrar conexión (menú)"));
        deleteConnection();
    }
}

int MainWindow::selectedConnectionIndexForPoolManagement() const {
    if (m_importedPoolsTable) {
        const auto sel = m_importedPoolsTable->selectedItems();
        if (!sel.isEmpty()) {
            const int row = sel.first()->row();
            if (QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0)) {
                const int idx = findConnectionIndexByName(connItem->text().trimmed());
                if (idx >= 0 && idx < m_profiles.size()) {
                    return idx;
                }
            }
        }
    }
    if (m_connectionsList) {
        const auto selected = m_connectionsList->selectedItems();
        if (!selected.isEmpty()) {
            QTreeWidgetItem* item = selected.first();
            while (item && item->parent()) {
                item = item->parent();
            }
            if (item) {
                const int idx = item->data(0, Qt::UserRole).toInt();
                if (idx >= 0 && idx < m_profiles.size()) {
                    return idx;
                }
            }
        }
    }
    return -1;
}

void MainWindow::updatePoolManagementBoxTitle() {
    if (!m_poolMgmtBox) {
        return;
    }
    const int idx = selectedConnectionIndexForPoolManagement();
    const QString connText = (idx >= 0 && idx < m_profiles.size())
                                 ? m_profiles[idx].name
                                 : tr3(QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]"));
    m_poolMgmtBox->setTitle(
        tr3(QStringLiteral("Gestión de Pools de %1"),
            QStringLiteral("Pool Management of %1"),
            QStringLiteral("%1 的池管理"))
            .arg(connText));
    if (m_btnPoolNew) {
        m_btnPoolNew->setEnabled(!actionsLocked() && idx >= 0 && idx < m_profiles.size());
    }
}

void MainWindow::refreshConnectionByIndex(int idx) {
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    m_states[idx] = refreshConnection(m_profiles[idx]);
    rebuildConnectionList();
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
}
