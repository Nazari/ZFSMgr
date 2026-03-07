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

bool MainWindow::isConnectionRedirectedToLocal(int idx) const {
    if (idx < 0 || idx >= m_profiles.size() || idx >= m_states.size()) {
        return false;
    }
    if (isLocalConnection(idx)) {
        return false;
    }
    const ConnectionRuntimeState& st = m_states[idx];
    if (st.status.trimmed().toUpper() != QStringLiteral("OK")) {
        return false;
    }

    QString localUuid = m_localMachineUuid.trimmed().toLower();
    if (localUuid.isEmpty()) {
        for (int i = 0; i < m_profiles.size() && i < m_states.size(); ++i) {
            if (!isLocalConnection(i)) {
                continue;
            }
            const QString cand = m_states[i].machineUuid.trimmed().toLower();
            if (!cand.isEmpty()) {
                localUuid = cand;
                break;
            }
        }
    }
    const QString remoteUuid = st.machineUuid.trimmed().toLower();
    if (!localUuid.isEmpty() && !remoteUuid.isEmpty()) {
        return localUuid == remoteUuid;
    }
    return isLocalHostForUi(m_profiles[idx].host);
}

void MainWindow::refreshAllConnections() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_acci_n_en__6facd2"),
                   QStringLiteral("Acción en curso: refresh bloqueado"),
                   QStringLiteral("Action in progress: refresh blocked"),
                   QStringLiteral("操作进行中：刷新被阻止")));
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_refrescar__7f8af2"),
               QStringLiteral("Refrescar todas las conexiones"),
               QStringLiteral("Refresh all connections"),
               QStringLiteral("刷新所有连接")));
    if (m_profiles.isEmpty()) {
        m_refreshInProgress = false;
        updateBusyCursor();
        rebuildConnectionList();
        rebuildDatasetPoolSelectors();
        populateAllPoolsTables();
        updateStatus(trk(QStringLiteral("t_status_refresh_end"),
                         QStringLiteral("Estado: refresco finalizado"),
                         QStringLiteral("Status: refresh finished"),
                         QStringLiteral("状态：刷新完成")));
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = m_profiles.size();
    m_refreshTotal = m_profiles.size();
    m_refreshInProgress = (m_refreshPending > 0);
    updateBusyCursor();
    updateStatus(trk(QStringLiteral("t_status_refresh_001"),
                     QStringLiteral("Estado: refrescando 0/%1"),
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
               trk(QStringLiteral("t_acci_n_en__6facd2"),
                   QStringLiteral("Acción en curso: refresh bloqueado"),
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
    updateStatus(trk(QStringLiteral("t_status_refresh_002"),
                     QStringLiteral("Estado: refrescando 0/1"),
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
    updateStatus(trk(QStringLiteral("t_status_refresh_003"),
                     QStringLiteral("Estado: refrescando %1/%2"),
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
    updateStatus(trk(QStringLiteral("t_status_refresh_end"),
                     QStringLiteral("Estado: refresco finalizado"),
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

void MainWindow::onPoolsListContextMenuRequested(const QPoint& pos) {
    if (actionsLocked() || !m_importedPoolsTable) {
        return;
    }

    QModelIndex idx = m_importedPoolsTable->indexAt(pos);
    if (idx.isValid()) {
        m_importedPoolsTable->setCurrentCell(idx.row(), idx.column());
        onPoolsSelectionChanged();
    }
    const auto sel = m_importedPoolsTable->selectedItems();
    const bool hasSel = !sel.isEmpty();
    const int selRow = hasSel ? sel.first()->row() : -1;

    QMenu menu(this);
    QAction* newAct = menu.addAction(
        trk(QStringLiteral("t_new_pool_lbl001"), QStringLiteral("Nuevo pool"), QStringLiteral("New pool"), QStringLiteral("新建池")));
    menu.addSeparator();
    QAction* refreshAct = menu.addAction(
        trk(QStringLiteral("t_refresh_btn001"), QStringLiteral("Actualizar"), QStringLiteral("Refresh"), QStringLiteral("刷新")));
    QAction* importAct = menu.addAction(
        trk(QStringLiteral("t_import_btn001"), QStringLiteral("Importar"), QStringLiteral("Import"), QStringLiteral("导入")));
    QAction* exportAct = menu.addAction(
        trk(QStringLiteral("t_export_btn001"), QStringLiteral("Exportar"), QStringLiteral("Export"), QStringLiteral("导出")));
    QAction* scrubAct = menu.addAction(QStringLiteral("Scrub"));
    QAction* destroyAct = menu.addAction(QStringLiteral("Destroy"));

    newAct->setEnabled(!actionsLocked() && selectedConnectionIndexForPoolManagement() >= 0);
    refreshAct->setEnabled(hasSel && m_poolStatusRefreshBtn && m_poolStatusRefreshBtn->isEnabled());
    importAct->setEnabled(hasSel && m_poolStatusImportBtn && m_poolStatusImportBtn->isEnabled());
    exportAct->setEnabled(hasSel && m_poolStatusExportBtn && m_poolStatusExportBtn->isEnabled());
    scrubAct->setEnabled(hasSel && m_poolStatusScrubBtn && m_poolStatusScrubBtn->isEnabled());
    destroyAct->setEnabled(hasSel && m_poolStatusDestroyBtn && m_poolStatusDestroyBtn->isEnabled());

    QAction* picked = menu.exec(m_importedPoolsTable->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == newAct) {
        logUiAction(QStringLiteral("Nuevo pool (menú pools)"));
        createPoolForSelectedConnection();
        return;
    }
    if (!hasSel || selRow < 0) {
        return;
    }
    if (picked == refreshAct) {
        logUiAction(QStringLiteral("Actualizar estado de pool (menú pools)"));
        refreshSelectedPoolDetails();
    } else if (picked == importAct) {
        logUiAction(QStringLiteral("Importar pool (menú pools)"));
        importPoolFromRow(selRow);
    } else if (picked == exportAct) {
        logUiAction(QStringLiteral("Exportar pool (menú pools)"));
        exportPoolFromRow(selRow);
    } else if (picked == scrubAct) {
        logUiAction(QStringLiteral("Scrub pool (menú pools)"));
        scrubPoolFromRow(selRow);
    } else if (picked == destroyAct) {
        logUiAction(QStringLiteral("Destroy pool (menú pools)"));
        destroyPoolFromRow(selRow);
    }
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
    QAction* newAct = menu.addAction(
        trk(QStringLiteral("t_new_btn_001"), QStringLiteral("Nueva"), QStringLiteral("New"), QStringLiteral("新建")));
    QAction* refreshAllAct = menu.addAction(
        trk(QStringLiteral("t_refrescar__7f8af2"), QStringLiteral("Refrescar todo"), QStringLiteral("Refresh all"), QStringLiteral("全部刷新")));
    menu.addSeparator();
    QAction* refreshAct = menu.addAction(
        trk(QStringLiteral("t_refresh_menu_01"), QStringLiteral("Refrescar"), QStringLiteral("Refresh"), QStringLiteral("刷新")));
    QAction* editAct = menu.addAction(
        trk(QStringLiteral("t_edit_menu_001"), QStringLiteral("Editar"), QStringLiteral("Edit"), QStringLiteral("编辑")));
    QAction* deleteAct = menu.addAction(
        trk(QStringLiteral("t_delete_menu01"), QStringLiteral("Borrar"), QStringLiteral("Delete"), QStringLiteral("删除")));
    refreshAct->setEnabled(hasSel);
    editAct->setEnabled(hasSel);
    deleteAct->setEnabled(hasSel);

    QAction* picked = menu.exec(m_connectionsList->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == newAct) {
        logUiAction(QStringLiteral("Nueva conexión (menú)"));
        createConnection();
    } else if (picked == refreshAllAct) {
        logUiAction(QStringLiteral("Refrescar todo (menú)"));
        refreshAllConnections();
    } else if (picked == refreshAct) {
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
                                 : trk(QStringLiteral("t_empty_brkt_01"), QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]"));
    m_poolMgmtBox->setTitle(
        trk(QStringLiteral("t_pool_mgmt_of01"),
            QStringLiteral("Gestión de Pools de %1"),
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
    updateStatus(trk(QStringLiteral("t_status_conn_ld1"),
                     QStringLiteral("Estado: %1 conexiones cargadas"),
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
        const bool localConn = isLocalConnection(p);
        const bool redirectedLocal = isConnectionRedirectedToLocal(i);
        if (localConn && !s.machineUuid.trimmed().isEmpty()) {
            m_localMachineUuid = s.machineUuid.trimmed();
        }
        if (st == QStringLiteral("OK")) {
            statusTag = QStringLiteral("[OK]");
            rowColor = s.missingUnixCommands.isEmpty() ? QColor("#1f7a1f") : QColor("#c77900");
        } else if (!st.isEmpty()) {
            statusTag = QStringLiteral("[KO]");
            rowColor = QColor("#a12a2a");
        }
        QString line;
        if (localConn) {
            line = QStringLiteral("Local");
        } else if (redirectedLocal) {
            line = QStringLiteral("%1 %2")
                       .arg(line1,
                            trk(QStringLiteral("t_conn_redirect_l1"),
                                QStringLiteral("[Redirección a 'Local']"),
                                QStringLiteral("[Redirected to 'Local']"),
                                QStringLiteral("[重定向到“本地”]")));
        } else {
            line = line1;
        }
        if (!statusTag.isEmpty()) {
            line += QStringLiteral(" %1").arg(statusTag);
        }

        auto* item = new QTreeWidgetItem(m_connectionsList);
        item->setText(0, line);
        item->setData(0, Qt::UserRole, i);
        item->setForeground(0, QBrush(rowColor));
        if (redirectedLocal) {
            item->setDisabled(true);
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
        }
        item->setToolTip(0, QStringLiteral("Host: %1\nPort: %2\nEstado: %3\nDetalle: %4")
                                .arg(p.host)
                                .arg(p.port)
                                .arg(s.status)
                                .arg(s.detail));
        item->setExpanded(false);
        if (redirectedLocal) {
            continue;
        }

        const QString osLine = !s.osLine.trimmed().isEmpty()
                                   ? s.osLine.trimmed()
                                   : QStringLiteral("%1").arg(p.osType);
        auto* osChild = new QTreeWidgetItem(item);
        osChild->setText(0, trk(QStringLiteral("t_os_line_conn01"),
                                QStringLiteral("Sistema operativo: %1"),
                                QStringLiteral("Operating system: %1"),
                                QStringLiteral("操作系统：%1")).arg(osLine));
        osChild->setData(0, Qt::UserRole, i);

        const QString method = !s.connectionMethod.trimmed().isEmpty() ? s.connectionMethod.trimmed() : p.connType;
        auto* methodChild = new QTreeWidgetItem(item);
        methodChild->setText(0, trk(QStringLiteral("t_conn_method01"),
                                    QStringLiteral("Método de conexión: %1"),
                                    QStringLiteral("Connection method: %1"),
                                    QStringLiteral("连接方式：%1")).arg(method));
        methodChild->setData(0, Qt::UserRole, i);

        const QString zfsFull = !s.zfsVersionFull.trimmed().isEmpty() ? s.zfsVersionFull.trimmed()
                                                                       : (zfsTxt == QStringLiteral("?") ? QStringLiteral("-")
                                                                                                        : QStringLiteral("OpenZFS %1").arg(zfsTxt));
        auto* zfsChild = new QTreeWidgetItem(item);
        zfsChild->setText(0, trk(QStringLiteral("t_openzfs_line01"),
                                 QStringLiteral("OpenZFS: %1"),
                                 QStringLiteral("OpenZFS: %1"),
                                 QStringLiteral("OpenZFS：%1")).arg(zfsFull));
        zfsChild->setData(0, Qt::UserRole, i);

        auto* commandsNode = new QTreeWidgetItem(item);
        QString commandsTitle = trk(QStringLiteral("t_cmds_detect01"),
                                    QStringLiteral("Comandos detectados"),
                                    QStringLiteral("Detected commands"),
                                    QStringLiteral("检测到的命令"));
        if (isWindowsConnection(p)) {
            QString layer = s.commandsLayer.trimmed();
            if (layer.isEmpty()) {
                layer = QStringLiteral("Powershell");
            }
            commandsTitle += QStringLiteral(" [%1]").arg(layer);
        }
        commandsNode->setText(0, commandsTitle);
        commandsNode->setData(0, Qt::UserRole, i);
        QStringList detected = s.detectedUnixCommands;
        QStringList missing = s.missingUnixCommands;
        if (detected.isEmpty() && missing.isEmpty() && !s.powershellFallbackCommands.isEmpty()) {
            detected = s.powershellFallbackCommands;
        }
        if (detected.isEmpty()) {
            detected << trk(QStringLiteral("t_none_000001"),
                            QStringLiteral("(ninguno)"),
                            QStringLiteral("(none)"),
                            QStringLiteral("（无）"));
        }
        auto* detectedNode = new QTreeWidgetItem(commandsNode);
        const QString detectedText = trk(QStringLiteral("t_detected_l001"),
                                         QStringLiteral("Detectados: %1"),
                                         QStringLiteral("Detected: %1"),
                                         QStringLiteral("已检测：%1")).arg(detected.join(QStringLiteral(", ")));
        detectedNode->setText(0, detectedText);
        detectedNode->setForeground(0, QBrush(QColor("#1f7a1f")));
        detectedNode->setToolTip(0, detectedText);

        if (!missing.isEmpty()) {
            auto* missingNode = new QTreeWidgetItem(commandsNode);
            const QString missingText = trk(QStringLiteral("t_missing_l001"),
                                            QStringLiteral("No detectados: %1"),
                                            QStringLiteral("Missing: %1"),
                                            QStringLiteral("未检测：%1")).arg(missing.join(QStringLiteral(", ")));
            missingNode->setText(0, missingText);
            missingNode->setForeground(0, QBrush(QColor("#a12a2a")));
            missingNode->setToolTip(0, missingText);
        }
    }
    m_connectionsList->collapseAll();
    syncConnectionLogTabs();
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
        const bool redirectedLocal = isConnectionRedirectedToLocal(i);
        if (redirectedLocal) {
            continue;
        }
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
                              trk(QStringLiteral("t_conn_create_er1"),
                                  QStringLiteral("No se pudo crear conexión:\n%1"),
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
    if (isLocalConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_local_builtin_01"),
                QStringLiteral("La conexión local integrada no se puede editar."),
                QStringLiteral("Built-in local connection cannot be edited."),
                QStringLiteral("内置本地连接不可编辑。")));
        return;
    }
    const bool redirectedLocal = isConnectionRedirectedToLocal(idx);
    if (redirectedLocal) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_redirect_l2"),
                QStringLiteral("La conexión está redirigida a 'Local' y no se puede editar."),
                QStringLiteral("This connection is redirected to 'Local' and cannot be edited."),
                QStringLiteral("该连接已重定向到“本地”，不可编辑。")));
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
                              trk(QStringLiteral("t_conn_update_er"),
                                  QStringLiteral("No se pudo actualizar conexión:\n%1"),
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
    if (isLocalConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_local_builtin_02"),
                QStringLiteral("La conexión local integrada no se puede borrar."),
                QStringLiteral("Built-in local connection cannot be deleted."),
                QStringLiteral("内置本地连接不可删除。")));
        return;
    }
    const bool redirectedLocal = isConnectionRedirectedToLocal(idx);
    if (redirectedLocal) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_redirect_l3"),
                QStringLiteral("La conexión está redirigida a 'Local' y no se puede borrar."),
                QStringLiteral("This connection is redirected to 'Local' and cannot be deleted."),
                QStringLiteral("该连接已重定向到“本地”，不可删除。")));
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        trk(QStringLiteral("t_del_conn_tit1"), QStringLiteral("Borrar conexión"), QStringLiteral("Delete connection"), QStringLiteral("删除连接")),
        trk(QStringLiteral("t_del_conn_q001"), QStringLiteral("¿Borrar conexión \"%1\"?"),
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
                              trk(QStringLiteral("t_del_conn_err1"),
                                  QStringLiteral("No se pudo borrar conexión:\n%1"),
                                  QStringLiteral("Could not delete connection:\n%1"),
                                  QStringLiteral("无法删除连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
}
