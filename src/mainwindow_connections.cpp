#include "mainwindow.h"

#include <QAction>
#include <QComboBox>
#include <QGroupBox>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QPoint>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QPushButton>
#include <QSet>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <QtConcurrent/QtConcurrent>

namespace {
constexpr int RoleNodeType = Qt::UserRole + 1;
constexpr int RoleConnName = Qt::UserRole + 2;
constexpr int RolePoolName = Qt::UserRole + 3;
constexpr int RolePoolAction = Qt::UserRole + 4;
constexpr int RolePoolState = Qt::UserRole + 5;
constexpr int RolePoolImported = Qt::UserRole + 6;

constexpr int NodeConnection = 1;
constexpr int NodePool = 2;
constexpr int NodePoolContent = 3;

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
    if (m_busyOnImportRefresh) {
        m_busyOnImportRefresh = false;
        endUiBusy();
    }
    updateStatus(trk(QStringLiteral("t_status_refresh_end"),
                     QStringLiteral("Estado: refresco finalizado"),
                     QStringLiteral("Status: refresh finished"),
                     QStringLiteral("状态：刷新完成")));
}

void MainWindow::onConnectionSelectionChanged() {
    if (!m_syncingConnectionFromPoolSelection) {
        populateAllPoolsTables();
    }
    refreshConnectionNodeDetails();
    updatePoolManagementBoxTitle();
}

void MainWindow::refreshConnectionNodeDetails() {
    auto resetPoolActionButtons = [this]() {
        if (m_poolStatusImportBtn) {
            m_poolStatusImportBtn->setEnabled(false);
        }
        if (m_poolStatusRefreshBtn) {
            m_poolStatusRefreshBtn->setProperty("zfsmgr_can_refresh", false);
            m_poolStatusRefreshBtn->setEnabled(false);
        }
        if (m_poolStatusExportBtn) {
            m_poolStatusExportBtn->setEnabled(false);
        }
        if (m_poolStatusScrubBtn) {
            m_poolStatusScrubBtn->setEnabled(false);
        }
        if (m_poolStatusDestroyBtn) {
            m_poolStatusDestroyBtn->setEnabled(false);
        }
    };

    const auto selected = m_connectionsList ? m_connectionsList->selectedItems() : QList<QTreeWidgetItem*>{};
    if (selected.isEmpty()) {
        if (m_connPropsStack && m_connPoolPropsPage) {
            m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
        }
        if (m_connBottomStack && m_connStatusPage) {
            m_connBottomStack->setCurrentWidget(m_connStatusPage);
        }
        if (m_poolPropsTable) {
            setTablePopulationMode(m_poolPropsTable, true);
            m_poolPropsTable->setRowCount(0);
            setTablePopulationMode(m_poolPropsTable, false);
        }
        if (m_poolStatusText) {
            m_poolStatusText->clear();
        }
        resetPoolActionButtons();
        if (m_connContentTree) {
            m_connContentTree->clear();
        }
        if (m_connContentPropsTable) {
            setTablePopulationMode(m_connContentPropsTable, true);
            m_connContentPropsTable->setRowCount(0);
            setTablePopulationMode(m_connContentPropsTable, false);
        }
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    auto* item = selected.first();
    const int nodeType = item->data(0, RoleNodeType).toInt();
    if (nodeType != NodePool && nodeType != NodePoolContent) {
        if (m_connPropsStack && m_connPoolPropsPage) {
            m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
        }
        if (m_connBottomStack && m_connStatusPage) {
            m_connBottomStack->setCurrentWidget(m_connStatusPage);
        }
        QTreeWidgetItem* top = item;
        while (top && top->parent()) {
            top = top->parent();
        }
        const int connIdx = top ? top->data(0, Qt::UserRole).toInt() : -1;
        if (connIdx >= 0 && connIdx < m_profiles.size() && connIdx < m_states.size()) {
            const ConnectionProfile& p = m_profiles[connIdx];
            const ConnectionRuntimeState& st = m_states[connIdx];

            if (m_poolPropsTable) {
                setTablePopulationMode(m_poolPropsTable, true);
                m_poolPropsTable->setRowCount(0);
                auto addProp = [this](const QString& k, const QString& v, const QString& src) {
                    const int r = m_poolPropsTable->rowCount();
                    m_poolPropsTable->insertRow(r);
                    m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(k));
                    m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(v));
                    m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(src));
                };
                const QString srcIni = QStringLiteral("connections.ini");
                addProp(QStringLiteral("id"), p.id, srcIni);
                addProp(QStringLiteral("name"), p.name, srcIni);
                addProp(QStringLiteral("host"), p.host, srcIni);
                addProp(QStringLiteral("port"), QString::number(p.port), srcIni);
                addProp(QStringLiteral("username"), p.username, srcIni);
                addProp(QStringLiteral("password"), p.password.isEmpty() ? QString() : QStringLiteral("[secret]"), srcIni);
                addProp(QStringLiteral("keyPath"), p.keyPath, srcIni);
                addProp(QStringLiteral("connType"), p.connType, srcIni);
                addProp(QStringLiteral("transport"), p.transport, srcIni);
                addProp(QStringLiteral("osType"), p.osType, srcIni);
                addProp(QStringLiteral("useSudo"), p.useSudo ? QStringLiteral("true") : QStringLiteral("false"), srcIni);
                setTablePopulationMode(m_poolPropsTable, false);
            }

            if (m_poolStatusText) {
                QStringList lines;
                lines << QStringLiteral("Estado: %1").arg(st.status.trimmed().isEmpty() ? QStringLiteral("-") : st.status.trimmed());
                lines << QStringLiteral("Detalle: %1").arg(st.detail.trimmed().isEmpty() ? QStringLiteral("-") : st.detail.trimmed());
                lines << QStringLiteral("Sistema operativo: %1").arg(st.osLine.trimmed().isEmpty() ? QStringLiteral("-") : st.osLine.trimmed());
                lines << QStringLiteral("Método de conexión: %1").arg(st.connectionMethod.trimmed().isEmpty() ? p.connType : st.connectionMethod.trimmed());
                lines << QStringLiteral("OpenZFS: %1").arg(st.zfsVersionFull.trimmed().isEmpty()
                                                               ? (st.zfsVersion.trimmed().isEmpty() ? QStringLiteral("-")
                                                                                                    : QStringLiteral("OpenZFS %1").arg(st.zfsVersion.trimmed()))
                                                               : st.zfsVersionFull.trimmed());
                QStringList detected = st.detectedUnixCommands;
                QStringList missing = st.missingUnixCommands;
                if (detected.isEmpty() && missing.isEmpty() && !st.powershellFallbackCommands.isEmpty()) {
                    detected = st.powershellFallbackCommands;
                }
                lines << QStringLiteral("Comandos detectados: %1")
                             .arg(detected.isEmpty() ? QStringLiteral("(ninguno)") : detected.join(QStringLiteral(", ")));
                lines << QStringLiteral("Comandos no detectados: %1")
                             .arg(missing.isEmpty() ? QStringLiteral("(ninguno)") : missing.join(QStringLiteral(", ")));
                m_poolStatusText->setPlainText(lines.join(QStringLiteral("\n")));
            }
            resetPoolActionButtons();
        }
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    const QString connName = item->data(0, RoleConnName).toString().trimmed();
    const QString poolName = item->data(0, RolePoolName).toString().trimmed();
    if (!connName.isEmpty() && !poolName.isEmpty() && m_importedPoolsTable) {
        for (int row = 0; row < m_importedPoolsTable->rowCount(); ++row) {
            const QTableWidgetItem* c = m_importedPoolsTable->item(row, 0);
            const QTableWidgetItem* p = m_importedPoolsTable->item(row, 1);
            if (!c || !p) {
                continue;
            }
            if (c->text().trimmed().compare(connName, Qt::CaseInsensitive) == 0
                && p->text().trimmed().compare(poolName, Qt::CaseInsensitive) == 0) {
                m_syncingConnectionFromPoolSelection = true;
                m_importedPoolsTable->setCurrentCell(row, 0);
                m_syncingConnectionFromPoolSelection = false;
                break;
            }
        }
    }

    if (nodeType == NodePoolContent) {
        if (m_connPropsStack && m_connContentPage) {
            m_connPropsStack->setCurrentWidget(m_connContentPage);
        }
        if (m_connBottomStack && m_connDatasetPropsPage) {
            m_connBottomStack->setCurrentWidget(m_connDatasetPropsPage);
        }
        int connIdx = findConnectionIndexByName(connName);
        if (connIdx >= 0 && connIdx < m_profiles.size() && m_connContentTree) {
            m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            populateDatasetTree(m_connContentTree, connIdx, poolName, QStringLiteral("conncontent"));
            refreshDatasetProperties(QStringLiteral("conncontent"));
            updateConnectionActionsState();
        }
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    if (m_connPropsStack && m_connPoolPropsPage) {
        m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
    }
    if (m_connBottomStack && m_connStatusPage) {
        m_connBottomStack->setCurrentWidget(m_connStatusPage);
    }
    refreshSelectedPoolDetails();
    updateConnectionActionsState();
    updateConnectionDetailTitlesForCurrentSelection();
}

void MainWindow::updateConnectionDetailTitlesForCurrentSelection() {
    QString propsTitle = trk(QStringLiteral("t_pool_props001"),
                             QStringLiteral("Propiedades"),
                             QStringLiteral("Properties"),
                             QStringLiteral("属性"));
    QString bottomTitle = trk(QStringLiteral("t_status_col_001"),
                              QStringLiteral("Estado"),
                              QStringLiteral("Status"),
                              QStringLiteral("状态"));

    const auto selected = m_connectionsList ? m_connectionsList->selectedItems() : QList<QTreeWidgetItem*>{};
    if (!selected.isEmpty()) {
        QTreeWidgetItem* item = selected.first();
        const int nodeType = item->data(0, RoleNodeType).toInt();
        if (nodeType == NodeConnection) {
            QTreeWidgetItem* top = item;
            while (top && top->parent()) {
                top = top->parent();
            }
            const QString connName = top ? m_profiles.value(top->data(0, Qt::UserRole).toInt()).name : QString();
            propsTitle = QStringLiteral("Propiedades de la conexión %1").arg(connName.isEmpty() ? QStringLiteral("-") : connName);
            bottomTitle = QStringLiteral("Estado de la conexión %1").arg(connName.isEmpty() ? QStringLiteral("-") : connName);
        } else if (nodeType == NodePool) {
            const QString poolName = item->data(0, RolePoolName).toString().trimmed();
            propsTitle = QStringLiteral("Propiedades del Pool %1").arg(poolName.isEmpty() ? QStringLiteral("-") : poolName);
            bottomTitle = QStringLiteral("Estado del Pool %1").arg(poolName.isEmpty() ? QStringLiteral("-") : poolName);
        } else if (nodeType == NodePoolContent) {
            QString datasetName;
            if (m_connContentTree) {
                const auto dsSel = m_connContentTree->selectedItems();
                if (!dsSel.isEmpty()) {
                    datasetName = dsSel.first()->data(0, Qt::UserRole).toString().trimmed();
                }
            }
            if (datasetName.isEmpty()) {
                datasetName = trk(QStringLiteral("t_no_sel_001"),
                                  QStringLiteral("(sin selección)"),
                                  QStringLiteral("(no selection)"),
                                  QStringLiteral("（未选择）"));
            }
            propsTitle = QStringLiteral("Contenido del Dataset %1").arg(datasetName);
            bottomTitle = QStringLiteral("Propiedades del dataset %1").arg(datasetName);
        }
    }

    if (m_connPropsGroup) {
        m_connPropsGroup->setTitle(propsTitle);
    }
    if (m_connBottomGroup) {
        m_connBottomGroup->setTitle(bottomTitle);
    }
}

void MainWindow::onPoolsSelectionChanged() {
    if (m_syncingConnectionFromPoolSelection) {
        refreshSelectedPoolDetails();
        updatePoolManagementBoxTitle();
        return;
    }
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
        m_connectionsList->setCurrentItem(item);
    }

    auto selectPoolRow = [this](const QString& connName, const QString& poolName) -> int {
        if (!m_importedPoolsTable) {
            return -1;
        }
        for (int row = 0; row < m_importedPoolsTable->rowCount(); ++row) {
            const QTableWidgetItem* c = m_importedPoolsTable->item(row, 0);
            const QTableWidgetItem* p = m_importedPoolsTable->item(row, 1);
            if (!c || !p) {
                continue;
            }
            if (c->text().trimmed().compare(connName, Qt::CaseInsensitive) == 0
                && p->text().trimmed().compare(poolName, Qt::CaseInsensitive) == 0) {
                m_importedPoolsTable->setCurrentCell(row, 0);
                refreshSelectedPoolDetails();
                return row;
            }
        }
        return -1;
    };

    if (item && item->data(0, RoleNodeType).toInt() == NodePool) {
        const QString connName = item->data(0, RoleConnName).toString().trimmed();
        const QString poolName = item->data(0, RolePoolName).toString().trimmed();
        const int row = selectPoolRow(connName, poolName);
        const bool hasRow = row >= 0;
        const QString stateUp = item->data(0, RolePoolState).toString().trimmed().toUpper();
        const QString action = item->data(0, RolePoolAction).toString().trimmed();

        QMenu poolMenu(this);
        QAction* refreshAct = poolMenu.addAction(
            trk(QStringLiteral("t_refresh_pool_ctx001"), QStringLiteral("Actualizar Pool"), QStringLiteral("Refresh Pool"), QStringLiteral("刷新池")));
        QAction* importAct = poolMenu.addAction(
            trk(QStringLiteral("t_import_pool_ctx001"), QStringLiteral("Importar Pool"), QStringLiteral("Import Pool"), QStringLiteral("导入池")));
        QAction* exportAct = poolMenu.addAction(
            trk(QStringLiteral("t_export_pool_ctx001"), QStringLiteral("Exportar Pool"), QStringLiteral("Export Pool"), QStringLiteral("导出池")));
        QAction* scrubAct = poolMenu.addAction(QStringLiteral("Scrub Pool"));
        QAction* destroyAct = poolMenu.addAction(QStringLiteral("Destroy Pool"));

        const bool canExport = hasRow && action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0;
        const bool canImport = hasRow && action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0
                               && stateUp == QStringLiteral("ONLINE");
        refreshAct->setEnabled(hasRow && canExport);
        importAct->setEnabled(canImport);
        exportAct->setEnabled(canExport);
        scrubAct->setEnabled(canExport && stateUp == QStringLiteral("ONLINE"));
        destroyAct->setEnabled(canExport);

        QAction* picked = poolMenu.exec(m_connectionsList->viewport()->mapToGlobal(pos));
        if (!picked || !hasRow) {
            return;
        }
        if (picked == refreshAct) {
            logUiAction(QStringLiteral("Actualizar estado de pool (menú conexión/pool)"));
            refreshSelectedPoolDetails();
        } else if (picked == importAct) {
            logUiAction(QStringLiteral("Importar pool (menú conexión/pool)"));
            importPoolFromRow(row);
        } else if (picked == exportAct) {
            logUiAction(QStringLiteral("Exportar pool (menú conexión/pool)"));
            exportPoolFromRow(row);
        } else if (picked == scrubAct) {
            logUiAction(QStringLiteral("Scrub pool (menú conexión/pool)"));
            scrubPoolFromRow(row);
        } else if (picked == destroyAct) {
            logUiAction(QStringLiteral("Destroy pool (menú conexión/pool)"));
            destroyPoolFromRow(row);
        }
        return;
    }

    QTreeWidgetItem* top = item;
    while (top && top->parent()) {
        top = top->parent();
    }
    if (top) {
        m_connectionsList->setCurrentItem(top);
    }
    const bool hasSel = (m_connectionsList->currentItem() != nullptr);

    QMenu menu(this);
    QAction* newAct = menu.addAction(
        trk(QStringLiteral("t_new_conn_ctx001"), QStringLiteral("Nueva Conexión"), QStringLiteral("New Connection"), QStringLiteral("新建连接")));
    QAction* refreshAllAct = menu.addAction(
        trk(QStringLiteral("t_refrescar__7f8af2"), QStringLiteral("Refrescar todo"), QStringLiteral("Refresh all"), QStringLiteral("全部刷新")));
    menu.addSeparator();
    QAction* refreshAct = menu.addAction(
        trk(QStringLiteral("t_refresh_conn_ctx001"), QStringLiteral("Refrescar Conexión"), QStringLiteral("Refresh Connection"), QStringLiteral("刷新连接")));
    QAction* editAct = menu.addAction(
        trk(QStringLiteral("t_edit_conn_ctx001"), QStringLiteral("Editar Conexión"), QStringLiteral("Edit Connection"), QStringLiteral("编辑连接")));
    QAction* deleteAct = menu.addAction(
        trk(QStringLiteral("t_del_conn_ctx001"), QStringLiteral("Borrar Conexión"), QStringLiteral("Delete Connection"), QStringLiteral("删除连接")));
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
    QStringList redirectedToLocalNames;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (i >= m_states.size()) {
            continue;
        }
        if (isLocalConnection(i)) {
            continue;
        }
        if (isConnectionRedirectedToLocal(i)) {
            redirectedToLocalNames << m_profiles[i].name;
        }
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        const auto& s = m_states[i];

        const QString line1 = QStringLiteral("%1").arg(p.name);
        QString zfsTxt = s.zfsVersion.trimmed();
        if (zfsTxt.isEmpty()) {
            zfsTxt = QStringLiteral("?");
        }
        QString statusTag = QStringLiteral("[Ko]");
        QColor rowColor("#14212b");
        const QString st = s.status.trimmed().toUpper();
        const bool localConn = isLocalConnection(p);
        const bool redirectedLocal = isConnectionRedirectedToLocal(i);
        if (redirectedLocal) {
            continue;
        }
        if (localConn && !s.machineUuid.trimmed().isEmpty()) {
            m_localMachineUuid = s.machineUuid.trimmed();
        }
        if (st == QStringLiteral("OK")) {
            statusTag = QStringLiteral("[Ok]");
            rowColor = s.missingUnixCommands.isEmpty() ? QColor("#1f7a1f") : QColor("#c77900");
        } else if (!st.isEmpty()) {
            statusTag = QStringLiteral("[Ko]");
            rowColor = QColor("#a12a2a");
        }
        QString connLabel = line1;
        if (localConn) {
            connLabel = redirectedToLocalNames.isEmpty() ? QStringLiteral("Local")
                                                         : redirectedToLocalNames.first();
        }
        QString line = QStringLiteral("%1 %2").arg(statusTag, connLabel);
        if (localConn) {
            line += QStringLiteral(" [Local]");
        }

        auto* item = new QTreeWidgetItem(m_connectionsList);
        item->setText(0, line);
        item->setData(0, Qt::UserRole, i);
        item->setData(0, RoleNodeType, NodeConnection);
        item->setForeground(0, QBrush(rowColor));
        item->setToolTip(0, QStringLiteral("Host: %1\nPort: %2\nEstado: %3\nDetalle: %4")
                                .arg(p.host)
                                .arg(p.port)
                                .arg(s.status)
                                .arg(s.detail));
        item->setExpanded(false);

        auto addPoolNode = [&](const QString& poolName,
                               const QString& stateTxt,
                               const QString& importedTxt,
                               const QString& actionTxt,
                               const QString& reasonTxt) -> QTreeWidgetItem* {
            const QString importedUp = importedTxt.trimmed().toUpper();
            const QString emptyTxt = trk(QStringLiteral("t_none_000001"),
                                         QStringLiteral("(ninguno)"),
                                         QStringLiteral("(none)"),
                                         QStringLiteral("（无）"));
            const QString reasonShown = reasonTxt.trimmed().isEmpty() ? emptyTxt : reasonTxt.trimmed();
            QString text;
            QColor poolColor("#a12a2a");
            if (importedUp == QStringLiteral("SÍ") || importedUp == QStringLiteral("SI")
                || importedUp == QStringLiteral("YES")) {
                text = QStringLiteral("%1 [Importado]").arg(poolName);
                poolColor = QColor("#1f7a1f");
            } else if (!actionTxt.trimmed().isEmpty()) {
                text = QStringLiteral("%1 [No Importado]").arg(poolName);
                poolColor = QColor("#c77900");
            } else {
                text = QStringLiteral("%1 [No Importable - %2]").arg(poolName, reasonShown);
                poolColor = QColor("#a12a2a");
            }

            auto* poolNode = new QTreeWidgetItem(item);
            poolNode->setText(0, text);
            poolNode->setData(0, Qt::UserRole, i);
            poolNode->setData(0, RoleNodeType, NodePool);
            poolNode->setData(0, RoleConnName, p.name);
            poolNode->setData(0, RolePoolName, poolName);
            poolNode->setData(0, RolePoolAction, actionTxt);
            poolNode->setData(0, RolePoolState, stateTxt);
            poolNode->setData(0, RolePoolImported, importedTxt);
            poolNode->setToolTip(0, trk(QStringLiteral("t_conn_pool_tip1"),
                                        QStringLiteral("Conexión: %1\nPool: %2\nEstado: %3\nImportado: %4\nAcción: %5\nMotivo: %6"),
                                        QStringLiteral("Connection: %1\nPool: %2\nState: %3\nImported: %4\nAction: %5\nReason: %6"),
                                        QStringLiteral("连接：%1\n池：%2\n状态：%3\n已导入：%4\n操作：%5\n原因：%6"))
                                        .arg(p.name,
                                             poolName,
                                             stateTxt,
                                             importedTxt,
                                             actionTxt.isEmpty() ? emptyTxt : actionTxt,
                                             reasonTxt.isEmpty() ? emptyTxt : reasonTxt));
            poolNode->setForeground(0, QBrush(poolColor));
            return poolNode;
        };

        for (const PoolImported& pool : s.importedPools) {
            QTreeWidgetItem* poolNode = addPoolNode(pool.pool, QStringLiteral("ONLINE"), QStringLiteral("Sí"), QStringLiteral("Exportar"), QString());
            if (poolNode) {
                auto* contentNode = new QTreeWidgetItem(poolNode);
                contentNode->setText(0, trk(QStringLiteral("t_content_node_001"),
                                            QStringLiteral("Contenido"),
                                            QStringLiteral("Content"),
                                            QStringLiteral("内容")));
                contentNode->setData(0, Qt::UserRole, i);
                contentNode->setData(0, RoleNodeType, NodePoolContent);
                contentNode->setData(0, RoleConnName, p.name);
                contentNode->setData(0, RolePoolName, pool.pool);
                poolNode->setExpanded(false);
            }
        }
        for (const PoolImportable& pool : s.importablePools) {
            const QString stateUp = pool.state.trimmed().toUpper();
            const QString action = (stateUp == QStringLiteral("ONLINE")) ? pool.action : QString();
            addPoolNode(pool.pool, pool.state, QStringLiteral("No"), action, pool.reason);
        }

    }
    m_connectionsList->collapseAll();
    syncConnectionLogTabs();
    endUiBusy();
    refreshConnectionNodeDetails();
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
