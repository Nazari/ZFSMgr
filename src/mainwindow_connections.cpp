#include "mainwindow.h"

#include <QAction>
#include <QComboBox>
#include <QGroupBox>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QSet>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <QtConcurrent/QtConcurrent>

namespace {
QString normalizeHostToken(QString host) {
    host = host.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']') && host.size() > 2) {
        host = host.mid(1, host.size() - 2);
    }
    while (host.endsWith('.')) {
        host.chop(1);
    }
    return host;
}

bool isLocalHostForUi(const QString& host) {
    const QString h = normalizeHostToken(host);
    if (h.isEmpty()) {
        return false;
    }
    if (h == QStringLiteral("localhost")
        || h == QStringLiteral("127.0.0.1")
        || h == QStringLiteral("::1")) {
        return true;
    }

    static const QSet<QString> aliases = []() {
        QSet<QString> s;
        s.insert(QStringLiteral("localhost"));
        s.insert(QStringLiteral("127.0.0.1"));
        s.insert(QStringLiteral("::1"));
        const QString local = normalizeHostToken(QSysInfo::machineHostName());
        if (!local.isEmpty()) {
            s.insert(local);
            s.insert(local + QStringLiteral(".local"));
            const int dot = local.indexOf('.');
            if (dot > 0) {
                const QString shortName = local.left(dot);
                s.insert(shortName);
                s.insert(shortName + QStringLiteral(".local"));
            }
        }
        return s;
    }();
    return aliases.contains(h);
}
} // namespace

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
void MainWindow::loadConnections() {
    QMap<QString, ConnectionRuntimeState> prevById;
    QMap<QString, ConnectionRuntimeState> prevByName;
    const int oldCount = qMin(m_profiles.size(), m_states.size());
    for (int i = 0; i < oldCount; ++i) {
        const QString idKey = m_profiles[i].id.trimmed().toLower();
        const QString nameKey = m_profiles[i].name.trimmed().toLower();
        if (!idKey.isEmpty()) {
            prevById[idKey] = m_states[i];
        }
        if (!nameKey.isEmpty()) {
            prevByName[nameKey] = m_states[i];
        }
    }

    const LoadResult loaded = m_store.loadConnections();
    m_profiles = loaded.profiles;
    m_states.clear();
    m_states.resize(m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i) {
        const QString idKey = m_profiles[i].id.trimmed().toLower();
        const QString nameKey = m_profiles[i].name.trimmed().toLower();
        if (!idKey.isEmpty() && prevById.contains(idKey)) {
            m_states[i] = prevById.value(idKey);
            continue;
        }
        if (!nameKey.isEmpty() && prevByName.contains(nameKey)) {
            m_states[i] = prevByName.value(nameKey);
        }
    }

    rebuildConnectionList();
    updateStatus(tr3(QStringLiteral("Estado: %1 conexiones cargadas"),
                     QStringLiteral("Status: %1 connections loaded"),
                     QStringLiteral("状态：已加载 %1 个连接"))
                     .arg(m_profiles.size()));
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }
    rebuildDatasetPoolSelectors();
    syncConnectionLogTabs();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildConnectionList() {
    beginUiBusy();
    m_connectionsList->clear();
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        const auto& s = m_states[i];

        const QString line1 = QStringLiteral("%1").arg(p.name);
        QString zfsTxt = s.zfsVersion.trimmed();
        if (zfsTxt.isEmpty()) {
            zfsTxt = QStringLiteral("?");
        }
        QString statusTag;
        QColor rowColor("#14212b");
        const QString st = s.status.trimmed().toUpper();
        if (st == QStringLiteral("OK")) {
            statusTag = isLocalHostForUi(p.host) ? QStringLiteral("[OK/Local] ") : QStringLiteral("[OK] ");
            rowColor = QColor("#1f7a1f");
        } else if (!st.isEmpty()) {
            statusTag = QStringLiteral("[KO] ");
            rowColor = QColor("#a12a2a");
        }
        QString line = QStringLiteral("%1%2").arg(statusTag, line1);

        auto* item = new QTreeWidgetItem(m_connectionsList);
        item->setText(0, line);
        item->setData(0, Qt::UserRole, i);
        item->setForeground(0, QBrush(rowColor));
        item->setToolTip(0, QStringLiteral("Host: %1\nPort: %2\nEstado: %3\nDetalle: %4")
                                .arg(p.host)
                                .arg(p.port)
                                .arg(s.status)
                                .arg(s.detail));
        item->setExpanded(false);

        const QString osLine = !s.osLine.trimmed().isEmpty()
                                   ? s.osLine.trimmed()
                                   : QStringLiteral("%1").arg(p.osType);
        auto* osChild = new QTreeWidgetItem(item);
        osChild->setText(0, tr3(QStringLiteral("Sistema operativo: %1"),
                                QStringLiteral("Operating system: %1"),
                                QStringLiteral("操作系统：%1")).arg(osLine));
        osChild->setData(0, Qt::UserRole, i);

        const QString method = !s.connectionMethod.trimmed().isEmpty() ? s.connectionMethod.trimmed() : p.connType;
        auto* methodChild = new QTreeWidgetItem(item);
        methodChild->setText(0, tr3(QStringLiteral("Método de conexión: %1"),
                                    QStringLiteral("Connection method: %1"),
                                    QStringLiteral("连接方式：%1")).arg(method));
        methodChild->setData(0, Qt::UserRole, i);

        const QString zfsFull = !s.zfsVersionFull.trimmed().isEmpty() ? s.zfsVersionFull.trimmed()
                                                                       : (zfsTxt == QStringLiteral("?") ? QStringLiteral("-")
                                                                                                        : QStringLiteral("OpenZFS %1").arg(zfsTxt));
        auto* zfsChild = new QTreeWidgetItem(item);
        zfsChild->setText(0, tr3(QStringLiteral("OpenZFS: %1"),
                                 QStringLiteral("OpenZFS: %1"),
                                 QStringLiteral("OpenZFS：%1")).arg(zfsFull));
        zfsChild->setData(0, Qt::UserRole, i);

        auto* commandsNode = new QTreeWidgetItem(item);
        commandsNode->setText(0, tr3(QStringLiteral("Comandos detectados"),
                                     QStringLiteral("Detected commands"),
                                     QStringLiteral("检测到的命令")));
        commandsNode->setData(0, Qt::UserRole, i);

        if (!s.detectedUnixCommands.isEmpty() || !s.missingUnixCommands.isEmpty()) {
            for (const QString& c : s.detectedUnixCommands) {
                auto* cc = new QTreeWidgetItem(commandsNode);
                cc->setText(0, c);
                cc->setForeground(0, QBrush(QColor("#1f7a1f")));
            }
            for (const QString& c : s.missingUnixCommands) {
                auto* cc = new QTreeWidgetItem(commandsNode);
                cc->setText(0, c);
                cc->setForeground(0, QBrush(QColor("#a12a2a")));
            }
        } else if (!s.powershellFallbackCommands.isEmpty()) {
            auto* psHeader = new QTreeWidgetItem(commandsNode);
            psHeader->setText(0, tr3(QStringLiteral("PowerShell (fallback):"),
                                     QStringLiteral("PowerShell (fallback):"),
                                     QStringLiteral("PowerShell（回退）：")));
            for (const QString& c : s.powershellFallbackCommands) {
                auto* cc = new QTreeWidgetItem(commandsNode);
                cc->setText(0, c);
                cc->setForeground(0, QBrush(QColor("#5a4a00")));
            }
        } else {
            auto* none = new QTreeWidgetItem(commandsNode);
            none->setText(0, tr3(QStringLiteral("(sin datos)"),
                                 QStringLiteral("(no data)"),
                                 QStringLiteral("（无数据）")));
        }
    }
    m_connectionsList->collapseAll();
    endUiBusy();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildDatasetPoolSelectors() {
    m_originPoolCombo->blockSignals(true);
    m_destPoolCombo->blockSignals(true);
    m_advPoolCombo->blockSignals(true);

    const QString originPrev = m_originPoolCombo->currentData().toString();
    const QString destPrev = m_destPoolCombo->currentData().toString();
    const QString advPrev = m_advPoolCombo->currentData().toString();

    m_originPoolCombo->clear();
    m_destPoolCombo->clear();
    m_advPoolCombo->clear();

    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& p : st.importedPools) {
            if (p.pool.isEmpty() || p.pool == QStringLiteral("Sin pools")) {
                continue;
            }
            const QString token = QStringLiteral("%1::%2").arg(i).arg(p.pool);
            const QString label = QStringLiteral("%1::%2").arg(m_profiles[i].name, p.pool);
            m_originPoolCombo->addItem(label, token);
            m_destPoolCombo->addItem(label, token);
            m_advPoolCombo->addItem(label, token);
        }
    }

    auto restoreCurrent = [](QComboBox* combo, const QString& token) {
        if (combo->count() <= 0) {
            return;
        }
        int idx = combo->findData(token);
        if (idx < 0) {
            idx = 0;
        }
        combo->setCurrentIndex(idx);
    };
    restoreCurrent(m_originPoolCombo, originPrev);
    restoreCurrent(m_destPoolCombo, destPrev);
    restoreCurrent(m_advPoolCombo, advPrev);

    m_originPoolCombo->blockSignals(false);
    m_destPoolCombo->blockSignals(false);
    m_advPoolCombo->blockSignals(false);
    onOriginPoolChanged();
    onDestPoolChanged();
    onAdvancedPoolChanged();
}

void MainWindow::createConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: nueva conexión bloqueada"));
        return;
    }
    ConnectionDialog dlg(m_language, this);
    ConnectionProfile p;
    p.connType = QStringLiteral("SSH");
    p.transport = QStringLiteral("SSH");
    p.osType = QStringLiteral("Linux");
    p.port = 22;
    dlg.setProfile(p);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile created = dlg.profile();
    QString createdId = created.id.trimmed();
    if (createdId.isEmpty()) {
        createdId = created.name.trimmed().toLower();
        createdId.replace(' ', '_');
        createdId.replace(':', '_');
        createdId.replace('/', '_');
    }
    QString err;
    if (!m_store.upsertConnection(created, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("No se pudo crear conexión:\n%1"),
                                  QStringLiteral("Could not create connection:\n%1"),
                                  QStringLiteral("无法创建连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == createdId) {
            if (QTreeWidgetItem* top = m_connectionsList->topLevelItem(i)) {
                m_connectionsList->setCurrentItem(top);
            }
            refreshSelectedConnection();
            break;
        }
    }
}

void MainWindow::editConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: edición bloqueada"));
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
    ConnectionDialog dlg(m_language, this);
    dlg.setProfile(m_profiles[idx]);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile edited = dlg.profile();
    edited.id = m_profiles[idx].id;
    QString err;
    if (!m_store.upsertConnection(edited, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("No se pudo actualizar conexión:\n%1"),
                                  QStringLiteral("Could not update connection:\n%1"),
                                  QStringLiteral("无法更新连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
}

void MainWindow::deleteConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: borrado bloqueado"));
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
    const auto confirm = QMessageBox::question(
        this,
        tr3(QStringLiteral("Borrar conexión"), QStringLiteral("Delete connection"), QStringLiteral("删除连接")),
        tr3(QStringLiteral("¿Borrar conexión \"%1\"?"),
            QStringLiteral("Delete connection \"%1\"?"),
            QStringLiteral("删除连接“%1”？")).arg(m_profiles[idx].name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    QString err;
    if (!m_store.deleteConnectionById(m_profiles[idx].id, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("No se pudo borrar conexión:\n%1"),
                                  QStringLiteral("Could not delete connection:\n%1"),
                                  QStringLiteral("无法删除连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
}
