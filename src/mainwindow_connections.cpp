#include "mainwindow.h"

#include <QComboBox>
#include <QGroupBox>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSysInfo>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QStackedWidget>
#include <QTreeWidget>

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

namespace {
int connectionIndexForRow(const QTableWidget* table, int row) {
    if (!table || row < 0 || row >= table->rowCount()) {
        return -1;
    }
    const QTableWidgetItem* it = table->item(row, 0);
    if (!it) {
        return -1;
    }
    bool ok = false;
    const int idx = it->data(Qt::UserRole).toInt(&ok);
    return ok ? idx : -1;
}

int selectedConnectionRow(const QTableWidget* table) {
    if (!table) {
        return -1;
    }
    int row = table->currentRow();
    if (row >= 0) {
        return connectionIndexForRow(table, row);
    }
    const auto ranges = table->selectedRanges();
    if (!ranges.isEmpty()) {
        return connectionIndexForRow(table, ranges.first().topRow());
    }
    return -1;
}

int rowForConnectionIndex(const QTableWidget* table, int connIdx) {
    if (!table || connIdx < 0) {
        return -1;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        if (connectionIndexForRow(table, row) == connIdx) {
            return row;
        }
    }
    return -1;
}

QString connPersistKeyFromProfiles(const QVector<ConnectionProfile>& profiles, int connIdx) {
    if (connIdx < 0 || connIdx >= profiles.size()) {
        return QString();
    }
    const QString id = profiles[connIdx].id.trimmed();
    if (!id.isEmpty()) {
        return id.toLower();
    }
    return profiles[connIdx].name.trimmed().toLower();
}
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
        rebuildConnectionsTable();
        rebuildDatasetPoolSelectors();
        populateAllPoolsTables();
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = m_profiles.size();
    m_refreshTotal = m_profiles.size();
    m_refreshInProgress = (m_refreshPending > 0);
    updateBusyCursor();

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
    const int idx = selectedConnectionRow(m_connectionsTable);
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = 1;
    m_refreshTotal = 1;
    m_refreshInProgress = true;
    updateBusyCursor();
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
    const int selectedIdx = selectedConnectionRow(m_connectionsTable);
    m_states[idx] = state;
    invalidatePoolDetailsCacheForConnection(idx);
    rebuildConnectionsTable();
    if (selectedIdx >= 0 && m_connectionsTable) {
        const int row = rowForConnectionIndex(m_connectionsTable, selectedIdx);
        if (row >= 0) {
            m_connectionsTable->setCurrentCell(row, 0);
        }
    }
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
    if (m_refreshPending > 0) {
        --m_refreshPending;
    }
    if (m_refreshPending == 0) {
        onAsyncRefreshDone(generation);
    }
}

void MainWindow::onAsyncRefreshDone(int generation) {
    if (generation != m_refreshGeneration) {
        return;
    }
    if (!m_initialRefreshCompleted) {
        m_initialRefreshCompleted = true;
    }
    if (m_connectionsTable && m_connectionsTable->rowCount() > 0 && m_connectionsTable->currentRow() < 0) {
        m_connectionsTable->setCurrentCell(0, 0);
    }
    refreshConnectionNodeDetails();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refresco paralelo finalizado"));
    m_refreshInProgress = false;
    updateBusyCursor();
    if (m_busyOnImportRefresh) {
        m_busyOnImportRefresh = false;
        endUiBusy();
    }
}

void MainWindow::onConnectionSelectionChanged() {
    QWidget* paintRoot = m_poolDetailTabs ? m_poolDetailTabs : static_cast<QWidget*>(m_rightTabs);
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(false);
    }

    if (!m_lastConnectionSelectionKey.isEmpty()) {
        const QStringList oldParts = m_lastConnectionSelectionKey.split('|');
        bool okOld = false;
        const int oldConnIdx = oldParts.value(0).toInt(&okOld);
        if (okOld) {
            saveConnectionNavState(oldConnIdx);
        }
    }
    QString selectionKey;
    if (m_connectionsTable) {
        const int idx = selectedConnectionRow(m_connectionsTable);
        const QString tabKey =
            (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0
             && m_connectionEntityTabs->currentIndex() < m_connectionEntityTabs->count())
                ? m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString()
                : QString();
        selectionKey = QStringLiteral("%1|%2").arg(idx).arg(tabKey);
    }
    if (!selectionKey.isEmpty() && selectionKey == m_lastConnectionSelectionKey) {
        // La tabla se recrea en cada refresh; aunque la clave lógica sea igual,
        // los widgets del detalle pueden haberse vaciado y deben repintarse.
        refreshConnectionNodeDetails();
        updatePoolManagementBoxTitle();
        if (paintRoot) {
            paintRoot->setUpdatesEnabled(true);
            paintRoot->update();
        }
        return;
    }
    m_lastConnectionSelectionKey = selectionKey;
    rebuildConnectionEntityTabs();
    populateAllPoolsTables();
    refreshConnectionNodeDetails();
    updatePoolManagementBoxTitle();
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(true);
        paintRoot->update();
    }
}

void MainWindow::rebuildConnectionEntityTabs() {
    if (!m_connectionEntityTabs || !m_connectionsTable) {
        return;
    }
    const int connIdx = selectedConnectionRow(m_connectionsTable);
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()) {
        m_updatingConnectionEntityTabs = true;
        while (m_connectionEntityTabs->count() > 0) {
            m_connectionEntityTabs->removeTab(m_connectionEntityTabs->count() - 1);
        }
        m_updatingConnectionEntityTabs = false;
        return;
    }
    const QString prevTabKey =
        (m_connectionEntityTabs->currentIndex() >= 0 && m_connectionEntityTabs->currentIndex() < m_connectionEntityTabs->count())
            ? m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString()
            : QString();
    const QString connPersistKey = connPersistKeyFromProfiles(m_profiles, connIdx);
    const ConnectionNavState nav = m_connectionNavStateByConnId.value(connPersistKey);
    QString targetEntityKey = nav.entityTabKey.trimmed();
    if (targetEntityKey.isEmpty()) {
        const QStringList prevParts = prevTabKey.split(':');
        bool prevOk = false;
        if (prevParts.size() >= 2) {
            const int prevConnIdx = prevParts.value(1).toInt(&prevOk);
            if (prevOk && prevConnIdx == connIdx) {
                if (prevParts.value(0) == QStringLiteral("conn")) {
                    targetEntityKey = QStringLiteral("conn");
                } else if (prevParts.value(0) == QStringLiteral("pool") && prevParts.size() >= 3) {
                    targetEntityKey = QStringLiteral("pool:%1").arg(prevParts.value(2).trimmed());
                }
            }
        }
    }
    if (targetEntityKey.isEmpty()) {
        targetEntityKey = QStringLiteral("conn");
    }

    m_updatingConnectionEntityTabs = true;
    while (m_connectionEntityTabs->count() > 0) {
        m_connectionEntityTabs->removeTab(m_connectionEntityTabs->count() - 1);
    }
    const QString connTabText =
        trk(QStringLiteral("t_conn_tab_dyn001"),
            QStringLiteral("Conexión %1"),
            QStringLiteral("Connection %1"),
            QStringLiteral("连接 %1"))
            .arg(m_profiles[connIdx].name);
    const int connTab = m_connectionEntityTabs->addTab(connTabText);
    m_connectionEntityTabs->setTabData(connTab, QStringLiteral("conn:%1").arg(connIdx));

    int targetTab = 0;
    int tabPos = 1;
    const ConnectionRuntimeState& st = m_states[connIdx];
    for (const PoolImported& pool : st.importedPools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        QString poolTabText = poolName
                              + trk(QStringLiteral("t_pool_tab_imp_001"),
                                    QStringLiteral(" [Importado]"),
                                    QStringLiteral(" [Imported]"),
                                    QStringLiteral(" [已导入]"));
        const int t = m_connectionEntityTabs->addTab(poolTabText);
        m_connectionEntityTabs->setTabData(t, QStringLiteral("pool:%1:%2").arg(connIdx).arg(poolName));
        if (targetEntityKey.compare(QStringLiteral("pool:%1").arg(poolName), Qt::CaseInsensitive) == 0) {
            targetTab = tabPos;
        }
        ++tabPos;
    }
    for (const PoolImportable& pool : st.importablePools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        const QString stateUp = pool.state.trimmed().toUpper();
        const QString actionTxt = pool.action.trimmed();
        // No crear tabs para pools no importables.
        if (stateUp != QStringLiteral("ONLINE") || actionTxt.isEmpty()) {
            continue;
        }
        QString poolTabText = poolName;
        poolTabText += trk(QStringLiteral("t_pool_tab_nimp001"),
                           QStringLiteral(" [No importado]"),
                           QStringLiteral(" [Not imported]"),
                           QStringLiteral(" [未导入]"));
        const int t = m_connectionEntityTabs->addTab(poolTabText);
        m_connectionEntityTabs->setTabData(t, QStringLiteral("pool:%1:%2").arg(connIdx).arg(poolName));
        if (targetEntityKey.compare(QStringLiteral("pool:%1").arg(poolName), Qt::CaseInsensitive) == 0) {
            targetTab = tabPos;
        }
        ++tabPos;
    }
    m_connectionEntityTabs->setCurrentIndex(targetTab);
    m_updatingConnectionEntityTabs = false;
}

void MainWindow::onConnectionEntityTabChanged(int idx) {
    if (m_updatingConnectionEntityTabs || !m_connectionEntityTabs || !m_connectionsTable) {
        return;
    }
    if (idx < 0 || idx >= m_connectionEntityTabs->count()) {
        return;
    }
    const QString key = m_connectionEntityTabs->tabData(idx).toString();
    if (key.isEmpty()) {
        return;
    }
    const QStringList parts = key.split(':');
    if (parts.size() < 2) {
        return;
    }
    const int connIdx = parts.value(1).toInt();
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const int row = rowForConnectionIndex(m_connectionsTable, connIdx);
    if (row < 0) {
        return;
    }
    if (m_connectionsTable->currentRow() != row) {
        m_connectionsTable->setCurrentCell(row, 0);
    } else {
        refreshConnectionNodeDetails();
        saveConnectionNavState(connIdx);
    }
}

void MainWindow::saveConnectionNavState(int connIdx) {
    if (!m_connectionEntityTabs || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const QString persistKey = connPersistKeyFromProfiles(m_profiles, connIdx);
    if (persistKey.isEmpty()) {
        return;
    }
    ConnectionNavState& nav = m_connectionNavStateByConnId[persistKey];
    const int tabIdx = m_connectionEntityTabs->currentIndex();
    if (tabIdx >= 0 && tabIdx < m_connectionEntityTabs->count()) {
        const QString tabData = m_connectionEntityTabs->tabData(tabIdx).toString();
        const QStringList parts = tabData.split(':');
        if (parts.size() >= 2 && parts.value(0) == QStringLiteral("conn")) {
            bool ok = false;
            const int tabConnIdx = parts.value(1).toInt(&ok);
            if (ok && tabConnIdx == connIdx) {
                nav.entityTabKey = QStringLiteral("conn");
            }
        } else if (parts.size() >= 3 && parts.value(0) == QStringLiteral("pool")) {
            bool ok = false;
            const int tabConnIdx = parts.value(1).toInt(&ok);
            const QString poolName = parts.value(2).trimmed();
            if (ok && tabConnIdx == connIdx && !poolName.isEmpty()) {
                nav.entityTabKey = QStringLiteral("pool:%1").arg(poolName);
                if (m_poolViewTabBar) {
                    nav.poolSubtabByPoolName[poolName] = m_poolViewTabBar->currentIndex();
                }
            }
        }
    }
}

void MainWindow::restoreConnectionPoolSubtabState(int connIdx, const QString& poolName) {
    if (!m_poolViewTabBar || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const QString persistKey = connPersistKeyFromProfiles(m_profiles, connIdx);
    if (persistKey.isEmpty()) {
        return;
    }
    const ConnectionNavState nav = m_connectionNavStateByConnId.value(persistKey);
    int targetSubtab = nav.poolSubtabByPoolName.value(poolName, 0);
    if (targetSubtab < 0 || targetSubtab >= m_poolViewTabBar->count()) {
        targetSubtab = 0;
    }
    if (m_poolViewTabBar->currentIndex() != targetSubtab) {
        m_poolViewTabBar->setCurrentIndex(targetSubtab);
    }
}

void MainWindow::refreshConnectionNodeDetails() {
    auto setConnectionActionButtonsVisible = [this](bool visible) {
        if (m_connPropsRefreshBtn) {
            m_connPropsRefreshBtn->setVisible(visible);
        }
        if (m_connPropsEditBtn) {
            m_connPropsEditBtn->setVisible(visible);
        }
        if (m_connPropsDeleteBtn) {
            m_connPropsDeleteBtn->setVisible(visible);
        }
    };
    auto setPoolActionButtonsVisible = [this](bool visible) {
        if (m_poolStatusRefreshBtn) {
            m_poolStatusRefreshBtn->setVisible(visible);
        }
        if (m_poolStatusImportBtn) {
            m_poolStatusImportBtn->setVisible(visible);
        }
        if (m_poolStatusExportBtn) {
            m_poolStatusExportBtn->setVisible(visible);
        }
        if (m_poolStatusScrubBtn) {
            m_poolStatusScrubBtn->setVisible(visible);
        }
        if (m_poolStatusDestroyBtn) {
            m_poolStatusDestroyBtn->setVisible(visible);
        }
    };

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
        if (m_connPropsRefreshBtn) {
            m_connPropsRefreshBtn->setProperty("zfsmgr_can_conn_action", false);
            m_connPropsRefreshBtn->setEnabled(false);
        }
        if (m_connPropsEditBtn) {
            m_connPropsEditBtn->setProperty("zfsmgr_can_conn_action", false);
            m_connPropsEditBtn->setEnabled(false);
        }
        if (m_connPropsDeleteBtn) {
            m_connPropsDeleteBtn->setProperty("zfsmgr_can_conn_action", false);
            m_connPropsDeleteBtn->setEnabled(false);
        }
    };

    const int connIdx = selectedConnectionRow(m_connectionsTable);
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        if (!m_connContentToken.isEmpty()) {
            saveConnContentTreeState(m_connContentToken);
            m_connContentToken.clear();
        }
        if (m_poolViewTabBar) {
            m_poolViewTabBar->setVisible(false);
            m_poolViewTabBar->setCurrentIndex(0);
        }
        setConnectionActionButtonsVisible(false);
        setPoolActionButtonsVisible(false);
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

    QString activePoolName;
    bool poolMode = false;
    if (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0) {
        const QString key = m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString();
        const QStringList parts = key.split(':');
        if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
            bool ok = false;
            const int tabConnIdx = parts.value(1).toInt(&ok);
            if (ok && tabConnIdx == connIdx) {
                poolMode = true;
                activePoolName = parts.value(2).trimmed();
            }
        }
    }

    setConnectionActionButtonsVisible(!poolMode);
    setPoolActionButtonsVisible(poolMode);
    if (m_poolViewTabBar) {
        m_poolViewTabBar->setVisible(poolMode);
        if (!poolMode) {
            m_poolViewTabBar->setCurrentIndex(0);
            m_poolViewTabBar->setTabData(0, QVariant());
            m_poolViewTabBar->setTabData(1, QVariant());
        } else {
            m_poolViewTabBar->setTabData(0, QStringLiteral("poolprops:%1:%2").arg(connIdx).arg(activePoolName));
            m_poolViewTabBar->setTabData(1, QStringLiteral("poolcontent:%1:%2").arg(connIdx).arg(activePoolName));
            restoreConnectionPoolSubtabState(connIdx, activePoolName);
        }
    }
    if (!poolMode) {
        if (!m_connContentToken.isEmpty()) {
            saveConnContentTreeState(m_connContentToken);
            m_connContentToken.clear();
        }
        if (m_connPropsStack && m_connPoolPropsPage) {
            m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
        }
        if (m_connBottomStack && m_connStatusPage) {
            m_connBottomStack->setCurrentWidget(m_connStatusPage);
        }
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
                const QString srcIni = QStringLiteral("config.ini");
                addProp(QStringLiteral("id"), p.id, srcIni);
                addProp(QStringLiteral("name"), p.name, srcIni);
                addProp(QStringLiteral("host"), p.host, srcIni);
                addProp(QStringLiteral("port"), QString::number(p.port), srcIni);
                addProp(QStringLiteral("username"), p.username, srcIni);
                addProp(QStringLiteral("password"), p.password.isEmpty() ? QString() : QStringLiteral("[secret]"), srcIni);
                addProp(QStringLiteral("keyPath"), p.keyPath, srcIni);
                addProp(QStringLiteral("connType"), p.connType, srcIni);
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
                QStringList nonImportable;
                for (const PoolImportable& pool : st.importablePools) {
                    const QString poolName = pool.pool.trimmed();
                    if (poolName.isEmpty()) {
                        continue;
                    }
                    const QString stateUp = pool.state.trimmed().toUpper();
                    const QString actionTxt = pool.action.trimmed();
                    if (stateUp == QStringLiteral("ONLINE") && !actionTxt.isEmpty()) {
                        continue;
                    }
                    QString reason = pool.reason.trimmed();
                    if (reason.isEmpty()) {
                        reason = QStringLiteral("-");
                    }
                    nonImportable << QStringLiteral("%1 (%2)").arg(poolName, reason);
                }
                lines << QStringLiteral("Pools no importables: %1")
                             .arg(nonImportable.isEmpty() ? QStringLiteral("(ninguno)")
                                                          : nonImportable.join(QStringLiteral(", ")));
                m_poolStatusText->setPlainText(lines.join(QStringLiteral("\n")));
            }
            resetPoolActionButtons();
            if (m_connPropsRefreshBtn) {
                m_connPropsRefreshBtn->setProperty("zfsmgr_can_conn_action", true);
                m_connPropsRefreshBtn->setEnabled(!actionsLocked());
            }
            if (m_connPropsEditBtn) {
                m_connPropsEditBtn->setProperty("zfsmgr_can_conn_action", true);
                m_connPropsEditBtn->setEnabled(!actionsLocked());
            }
            if (m_connPropsDeleteBtn) {
                m_connPropsDeleteBtn->setProperty("zfsmgr_can_conn_action", true);
                m_connPropsDeleteBtn->setEnabled(!actionsLocked());
            }
        }
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    const QString connName = (connIdx >= 0 && connIdx < m_profiles.size()) ? m_profiles[connIdx].name : QString();
    const QString poolName = activePoolName;
    const QString newConnContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    if (!m_connContentToken.isEmpty() && m_connContentToken != newConnContentToken) {
        saveConnContentTreeState(m_connContentToken);
    }
    if (m_connPropsStack && m_connPoolPropsPage) {
        m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
    }
    if (m_connBottomStack && m_connStatusPage) {
        m_connBottomStack->setCurrentWidget(m_connStatusPage);
    }
    resetPoolActionButtons();
    refreshSelectedPoolDetails(false, true);
    if (connIdx >= 0 && connIdx < m_profiles.size() && m_connContentTree) {
        m_connContentToken = newConnContentToken;
        populateDatasetTree(m_connContentTree, connIdx, poolName, QStringLiteral("conncontent"), true);
        refreshDatasetProperties(QStringLiteral("conncontent"));
    }
    if (m_poolViewTabBar) {
        const int idx = m_poolViewTabBar->currentIndex();
        if (idx == 1) {
            if (m_connPropsStack && m_connContentPage) m_connPropsStack->setCurrentWidget(m_connContentPage);
            if (m_connBottomStack && m_connDatasetPropsPage) m_connBottomStack->setCurrentWidget(m_connDatasetPropsPage);
        }
    }
    updateConnectionActionsState();
    updateConnectionDetailTitlesForCurrentSelection();
}

void MainWindow::updateConnectionDetailTitlesForCurrentSelection() {
    const int connIdx = selectedConnectionRow(m_connectionsTable);
    QString activePoolName;
    bool poolMode = false;
    if (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0) {
        const QString key = m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString();
        const QStringList parts = key.split(':');
        if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
            bool ok = false;
            const int tabConnIdx = parts.value(1).toInt(&ok);
            if (ok && tabConnIdx == connIdx) {
                poolMode = true;
                activePoolName = parts.value(2).trimmed();
            }
        }
    }
    if (m_poolViewTabBar) {
        QString tab0 = trk(QStringLiteral("t_pool_props001"),
                           QStringLiteral("Propiedades"),
                           QStringLiteral("Properties"),
                           QStringLiteral("属性"));
        QString tab1 = trk(QStringLiteral("t_content_node_001"),
                           QStringLiteral("Contenido"),
                           QStringLiteral("Content"),
                           QStringLiteral("内容"));
        if (poolMode) {
            const QString poolName = activePoolName;
            const QString shown = poolName.isEmpty() ? QStringLiteral("-") : poolName;
            tab0 = QStringLiteral("Propiedades %1").arg(shown);
            tab1 = QStringLiteral("Contenido %1").arg(shown);
        }
        m_poolViewTabBar->setTabText(0, tab0);
        m_poolViewTabBar->setTabText(1, tab1);
    }
}

int MainWindow::selectedConnectionIndexForPoolManagement() const {
    if (m_connectionsTable) {
        const int idx = selectedConnectionRow(m_connectionsTable);
        if (idx >= 0 && idx < m_profiles.size()) {
            return idx;
        }
    }
    return -1;
}

void MainWindow::updatePoolManagementBoxTitle() {
    const int idx = selectedConnectionIndexForPoolManagement();
    const QString connText = (idx >= 0 && idx < m_profiles.size())
                                 ? m_profiles[idx].name
                                 : trk(QStringLiteral("t_empty_brkt_01"), QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]"));
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setTitle(
            trk(QStringLiteral("t_pool_mgmt_of01"),
                QStringLiteral("Gestión de Pools de %1"),
                QStringLiteral("Pool Management of %1"),
                QStringLiteral("%1 的池管理"))
                .arg(connText));
    }
    if (m_btnPoolNew) {
        m_btnPoolNew->setEnabled(!actionsLocked() && idx >= 0 && idx < m_profiles.size());
    }
}

void MainWindow::refreshConnectionByIndex(int idx) {
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    m_states[idx] = refreshConnection(m_profiles[idx]);
    invalidatePoolDetailsCacheForConnection(idx);
    rebuildConnectionsTable();
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
}
void MainWindow::loadConnections() {
    QString prevSelectedConnId;
    if (m_connectionsTable) {
        const int prevIdx = selectedConnectionRow(m_connectionsTable);
        if (prevIdx >= 0 && prevIdx < m_profiles.size()) {
            prevSelectedConnId = m_profiles[prevIdx].id.trimmed();
        }
    }

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

    rebuildConnectionsTable();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }

    if (m_connectionsTable && m_connectionsTable->rowCount() > 0) {
        int targetRow = -1;
        if (!prevSelectedConnId.trimmed().isEmpty()) {
            for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
                const int connIdx = connectionIndexForRow(m_connectionsTable, r);
                if (connIdx >= 0
                    && connIdx < m_profiles.size()
                    && m_profiles[connIdx].id.trimmed().compare(prevSelectedConnId, Qt::CaseInsensitive) == 0) {
                    targetRow = r;
                    break;
                }
            }
        }
        if (targetRow < 0 && m_initialRefreshCompleted) {
            targetRow = 0;
        }
        if (targetRow >= 0) {
            m_connectionsTable->setCurrentCell(targetRow, 0);
        }
    }

    rebuildDatasetPoolSelectors();
    syncConnectionLogTabs();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildConnectionsTable() {
    beginUiBusy();
    QString prevSelectedKey;
    {
        const int prevIdx = selectedConnectionRow(m_connectionsTable);
        if (prevIdx >= 0 && prevIdx < m_profiles.size()) {
            prevSelectedKey = m_profiles[prevIdx].id.trimmed().toLower();
            if (prevSelectedKey.isEmpty()) {
                prevSelectedKey = m_profiles[prevIdx].name.trimmed().toLower();
            }
        }
    }
    m_connectionsTable->clear();
    m_connectionsTable->setRowCount(0);
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

        const int row = m_connectionsTable->rowCount();
        m_connectionsTable->insertRow(row);
        auto* it = new QTableWidgetItem(line);
        it->setData(Qt::UserRole, i);
        it->setForeground(QBrush(rowColor));
        it->setToolTip(QStringLiteral("Host: %1\nPort: %2\nEstado: %3\nDetalle: %4")
                           .arg(p.host)
                           .arg(p.port)
                           .arg(s.status)
                           .arg(s.detail));
        m_connectionsTable->setItem(row, 0, it);

    }
    int targetRow = -1;
    const QString preferredKey = !m_userSelectedConnectionKey.trimmed().isEmpty()
                                     ? m_userSelectedConnectionKey.trimmed().toLower()
                                     : prevSelectedKey;
    if (!preferredKey.isEmpty()) {
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            QTableWidgetItem* it = m_connectionsTable->item(r, 0);
            if (!it) {
                continue;
            }
            bool ok = false;
            const int connIdx = it->data(Qt::UserRole).toInt(&ok);
            if (!ok || connIdx < 0 || connIdx >= m_profiles.size()) {
                continue;
            }
            QString key = m_profiles[connIdx].id.trimmed().toLower();
            if (key.isEmpty()) {
                key = m_profiles[connIdx].name.trimmed().toLower();
            }
            if (key == preferredKey) {
                targetRow = r;
                break;
            }
        }
    }
    if (targetRow < 0 && m_connectionsTable->rowCount() > 0) {
        targetRow = 0;
    }
    if (targetRow >= 0) {
        m_connectionsTable->setCurrentCell(targetRow, 0);
    }

    rebuildConnectionEntityTabs();
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
    p.osType = QStringLiteral("Linux");
    p.port = 22;
    dlg.setProfile(p);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile created = dlg.profile();
    {
        const QString newName = created.name.trimmed();
        for (const ConnectionProfile& cp : m_profiles) {
            if (cp.name.trimmed().compare(newName, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_conn_name_unique_01"),
                                         QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                                         QStringLiteral("Connection name already exists. It must be unique."),
                                         QStringLiteral("连接名称已存在，必须唯一。")));
                return;
            }
        }
    }
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
            if (m_connectionsTable) {
                const int row = rowForConnectionIndex(m_connectionsTable, i);
                if (row >= 0) {
                    m_connectionsTable->setCurrentCell(row, 0);
                }
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
    const int idx = selectedConnectionRow(m_connectionsTable);
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
    {
        const QString editedName = edited.name.trimmed();
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (i == idx) {
                continue;
            }
            if (m_profiles[i].name.trimmed().compare(editedName, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_conn_name_unique_01"),
                                         QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                                         QStringLiteral("Connection name already exists. It must be unique."),
                                         QStringLiteral("连接名称已存在，必须唯一。")));
                return;
            }
        }
    }
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
    const int idx = selectedConnectionRow(m_connectionsTable);
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
