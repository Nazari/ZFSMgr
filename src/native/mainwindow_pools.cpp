#include "mainwindow.h"

#include "pools.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "daemonpayload.h"
#include "mainwindow_ui_logic.h"
#include "agentversion.h"

#include <QBrush>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;
using mwhelpers::sshUserHostPort;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;

} // namespace

QString MainWindow::formatPoolStatusTooltipHtml(const QString& statusText) const {
    const QString trimmed = statusText.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    return QStringLiteral("<pre style=\"font-family:'SF Mono','Menlo','Monaco','Consolas','Liberation Mono',monospace; white-space:pre;\">%1</pre>")
        .arg(trimmed.toHtmlEscaped());
}

QString MainWindow::cachedPoolStatusTooltipHtml(int connIdx, const QString& poolName) const {
    const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
    if (cacheKey.isEmpty()) {
        return QString();
    }
    const auto it = m_conns.poolDetailsCache.constFind(cacheKey);
    if (it == m_conns.poolDetailsCache.cend()) {
        return QString();
    }
    return formatPoolStatusTooltipHtml(it->statusText);
}

bool MainWindow::isPoolSuspendedByStatusText(const QString& statusText) const {
    const QString t = statusText.trimmed().toLower();
    if (t.isEmpty()) {
        return false;
    }
    if (t.contains(QStringLiteral("pool i/o is currently suspended"))) {
        return true;
    }
    if (t.contains(QStringLiteral("state: suspended"))) {
        return true;
    }
    return t.contains(QStringLiteral("suspended"));
}

bool MainWindow::isPoolSuspended(int connIdx, const QString& poolName) const {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return false;
    }
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool)) {
        if (isPoolSuspendedByStatusText(poolInfo->runtime.poolStatusText)) {
            return true;
        }
    }
    if (connIdx >= 0 && connIdx < m_conns.states.size()) {
        if (isPoolSuspendedByStatusText(m_conns.states[connIdx].poolStatusByName.value(trimmedPool))) {
            return true;
        }
    }
    const QString cacheKey = poolDetailsCacheKey(connIdx, trimmedPool);
    if (!cacheKey.isEmpty()) {
        const auto it = m_conns.poolDetailsCache.constFind(cacheKey);
        if (it != m_conns.poolDetailsCache.cend() && isPoolSuspendedByStatusText(it->statusText)) {
            return true;
        }
    }
    return false;
}

void MainWindow::applyPoolRootTooltipForTree(QTreeWidget* tree,
                                             int connIdx,
                                             const QString& poolName,
                                             const QString& statusText) const {
    if (!tree || connIdx < 0 || poolName.trimmed().isEmpty()) {
        return;
    }
    const QString poolKey = poolName.trimmed();
    const QString tooltipHtml = formatPoolStatusTooltipHtml(statusText);
    const QString fallbackText = QStringLiteral("%1::%2")
        .arg((connIdx >= 0 && connIdx < m_conns.profiles.size()) ? m_conns.profiles[connIdx].name : QString(),
             poolKey);
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = tree->topLevelItem(i);
        if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
            continue;
        }
        const int rootConnIdx = root->data(0, kConnIdxRole).toInt();
        const QString rootPool = root->data(0, kPoolNameRole).toString().trimmed();
        if (rootConnIdx != connIdx || rootPool != poolKey) {
            continue;
        }
        root->setToolTip(0, tooltipHtml.isEmpty() ? fallbackText : tooltipHtml);
        break;
    }
}

void MainWindow::applyPoolRootTooltipToVisibleTrees(int connIdx,
                                                    const QString& poolName,
                                                    const QString& statusText) const {
    applyPoolRootTooltipForTree(m_connContentTree, connIdx, poolName, statusText);
}

void MainWindow::cachePoolStatusTextsForConnection(int connIdx, const ConnectionRuntimeState& state) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }
    for (auto it = state.poolStatusByName.cbegin(); it != state.poolStatusByName.cend(); ++it) {
        const QString poolName = it.key().trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
        if (cacheKey.isEmpty()) {
            continue;
        }
        PoolDetailsCacheEntry entry = m_conns.poolDetailsCache.value(cacheKey);
        entry.statusText = it.value().trimmed();
        m_conns.poolDetailsCache.insert(cacheKey, entry);
        rebuildConnInfoFor(connIdx);
        applyPoolRootTooltipToVisibleTrees(connIdx, poolName, entry.statusText);
    }
}

int MainWindow::findPoolRow(const QString& connection, const QString& pool) const {
    const QString connKey = connection.trimmed();
    const QString poolKey = pool.trimmed();
    for (int i = 0; i < m_conns.poolListEntries.size(); ++i) {
        const auto& e = m_conns.poolListEntries[i];
        if (e.connection.compare(connKey, Qt::CaseInsensitive) == 0
            && e.pool.compare(poolKey, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

QString MainWindow::poolDetailsCacheKey(int connIdx, const QString& poolName) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return QString();
    }
    return QStringLiteral("%1::%2")
        .arg(m_conns.profiles[connIdx].name.trimmed().toLower(), poolName.trimmed().toLower());
}

void MainWindow::invalidatePoolDetailsCacheForConnection(int connIdx) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }
    const QString prefix = QStringLiteral("%1::").arg(m_conns.profiles[connIdx].name.trimmed().toLower());
    if (prefix.isEmpty()) {
        return;
    }
    auto it = m_conns.poolDetailsCache.begin();
    while (it != m_conns.poolDetailsCache.end()) {
        if (it.key().startsWith(prefix)) {
            it = m_conns.poolDetailsCache.erase(it);
        } else {
            ++it;
        }
    }
    auto inFlight = m_poolDetailsLoadsInFlight.begin();
    while (inFlight != m_poolDetailsLoadsInFlight.end()) {
        if (inFlight->startsWith(prefix)) {
            inFlight = m_poolDetailsLoadsInFlight.erase(inFlight);
        } else {
            ++inFlight;
        }
    }
    if (const ConnInfo* connInfo = findConnInfo(connIdx)) {
        for (auto itPool = connInfo->poolsByStableId.cbegin(); itPool != connInfo->poolsByStableId.cend(); ++itPool) {
            removeDatasetPropertyEntriesForPool(connIdx, itPool->key.poolName);
        }
    }
}

int MainWindow::selectedPoolRowFromTabs() const {
    return -1;
}

void MainWindow::refreshPoolStatusNow(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return;
    }

    beginTransientUiBusy(trk(QStringLiteral("t_pool_refresh_status_busy001"),
                             QStringLiteral("Actualizando estado del pool..."),
                             QStringLiteral("Refreshing pool status...")));
    struct BusyGuard final {
        MainWindow* self{nullptr};
        ~BusyGuard() {
            if (self) {
                self->endTransientUiBusy();
            }
        }
    } busyGuard{this};

    const ConnectionProfile profile = m_conns.profiles[connIdx];
    QString out;
    QString errText;
    if (!readPoolStatus(connIdx, trimmedPool, out, &errText)) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("No se pudo refrescar estado de pool %1 en %2: %3")
                   .arg(trimmedPool, profile.name, errText));
        QMessageBox::warning(this,
                             trk(QStringLiteral("t_pool_refresh_status_fail_t1"),
                                 QStringLiteral("Refresh status"),
                                 QStringLiteral("Refresh status")),
                             trk(QStringLiteral("t_pool_refresh_status_fail_q1"),
                                 QStringLiteral("No se pudo obtener el estado del pool %1.\n%2"),
                                 QStringLiteral("Could not get status for pool %1.\n%2"))
                                 .arg(trimmedPool, errText));
        return;
    }

    PoolDetailsCacheEntry entry = m_conns.poolDetailsCache.value(poolDetailsCacheKey(connIdx, trimmedPool));
    entry.loaded = true;
    entry.statusText = out.trimmed();
    m_conns.poolDetailsCache.insert(poolDetailsCacheKey(connIdx, trimmedPool), entry);
    if (connIdx >= 0 && connIdx < m_conns.states.size()) {
        m_conns.states[connIdx].poolStatusByName.insert(trimmedPool, entry.statusText);
    }
    rebuildConnInfoFor(connIdx);
    applyPoolRootTooltipToVisibleTrees(connIdx, trimmedPool, entry.statusText);
    const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool);
    if (m_connContentTree) {
        syncConnContentPoolColumnsFor(m_connContentTree, token);
    }
    if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
        syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
    }
}

void MainWindow::exportPoolFromRow(int row) {
    // The shell's `export` on the pool of that row.
    if (row < 0 || row >= m_conns.poolListEntries.size()) {
        return;
    }
    const auto& pe = m_conns.poolListEntries[row];
    const int idx = findConnectionIndexByName(pe.connection);
    if (idx >= 0 && !pe.pool.isEmpty()) {
        cliPool(idx, pe.pool, QStringLiteral("export"));
    }
}

void MainWindow::importPoolFromRow(int row) {
    // The shell's `import` of the pool of that row.
    if (row < 0 || row >= m_conns.poolListEntries.size()) {
        return;
    }
    const auto& pe = m_conns.poolListEntries[row];
    const int idx = findConnectionIndexByName(pe.connection);
    if (idx >= 0 && !pe.pool.isEmpty()) {
        cliImportPool(idx, pe.pool, QString(), false);
    }
}

void MainWindow::scrubPoolFromRow(int row) {
    // The shell's `scrub` on the pool of that row.
    if (row < 0 || row >= m_conns.poolListEntries.size()) {
        return;
    }
    const auto& pe = m_conns.poolListEntries[row];
    const int idx = findConnectionIndexByName(pe.connection);
    if (idx >= 0 && !pe.pool.isEmpty()) {
        cliPool(idx, pe.pool, QStringLiteral("scrub"));
    }
}

void MainWindow::destroyPoolFromRow(int row) {
    // The shell's `destroy` on the pool of that row.
    if (row < 0 || row >= m_conns.poolListEntries.size()) {
        return;
    }
    const auto& pe = m_conns.poolListEntries[row];
    const int idx = findConnectionIndexByName(pe.connection);
    if (idx >= 0 && !pe.pool.isEmpty()) {
        cliPool(idx, pe.pool, QStringLiteral("destroy"));
    }
}

void MainWindow::populateAllPoolsTables() {
    if (m_uiRebuildDeferred > 0) {
        m_uiRebuildPendingPools = true;
        return;
    }
    ++m_uiRebuildCounts.pools;
    m_conns.poolListEntries.clear();
    for (int i = 0; i < m_conns.states.size(); ++i) {
        if (i < m_conns.profiles.size() && isConnectionDisconnected(i)) {
            continue;
        }
        if (i < m_conns.profiles.size()) {
            const bool redirectedLocal = isConnectionRedirectedToLocal(i);
            if (redirectedLocal) {
                continue;
            }
        }
        const auto& st = m_conns.states[i];
        for (const PoolImported& pool : st.importedPools) {
            PoolListEntry e;
            e.connection = pool.connection;
            e.pool = pool.pool;
            e.state = QStringLiteral("ONLINE");
            e.imported = QStringLiteral("Sí");
            e.reason.clear();
            e.action = QStringLiteral("Exportar");
            m_conns.poolListEntries.push_back(std::move(e));
        }
        for (const PoolImportable& pool : st.importablePools) {
            const QString up = pool.state.trimmed().toUpper();
            PoolListEntry e;
            e.connection = pool.connection;
            e.pool = pool.pool;
            e.guid = pool.guid;
            e.state = pool.state;
            e.imported = QStringLiteral("No");
            e.reason = pool.reason;
            e.action = (up == QStringLiteral("ONLINE")) ? pool.action : QString();
            m_conns.poolListEntries.push_back(std::move(e));
        }
    }
    refreshSelectedPoolDetails(false, false);
    updatePoolManagementBoxTitle();
}


void MainWindow::refreshSelectedPoolDetails(bool forceRefresh, bool allowRemoteLoadIfMissing) {
    if (!m_poolPropsTable || !m_poolStatusText) {
        return;
    }
    setTablePopulationMode(m_poolPropsTable, true);
    m_poolPropsTable->setRowCount(0);
    m_poolStatusText->clear();
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

    const int row = selectedPoolRowFromTabs();
    if (row < 0 || row >= m_conns.poolListEntries.size()) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const auto& pe = m_conns.poolListEntries[row];
    const QString connName = pe.connection;
    const QString poolName = pe.pool;
    const QString poolState = pe.state.trimmed().toUpper();
    const QString action = pe.action;
    const bool canExport = (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0);
    const bool canImport = (action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0
                            && poolState == QStringLiteral("ONLINE"));
    const bool canScrub = canExport && poolState == QStringLiteral("ONLINE");
    const bool canDestroy = canExport;
    if (m_poolStatusRefreshBtn) {
        m_poolStatusRefreshBtn->setProperty("zfsmgr_can_refresh", canExport);
        m_poolStatusRefreshBtn->setEnabled(!actionsLocked() && canExport);
    }
    if (m_poolStatusExportBtn) {
        m_poolStatusExportBtn->setEnabled(!actionsLocked() && canExport);
    }
    if (m_poolStatusImportBtn) {
        m_poolStatusImportBtn->setEnabled(!actionsLocked() && canImport);
    }
    if (m_poolStatusScrubBtn) {
        m_poolStatusScrubBtn->setEnabled(!actionsLocked() && canScrub);
    }
    if (m_poolStatusDestroyBtn) {
        m_poolStatusDestroyBtn->setEnabled(!actionsLocked() && canDestroy);
    }
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    if (!canExport) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0 || idx >= m_conns.profiles.size()) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const ConnectionProfile p = m_conns.profiles[idx];
    const QString cacheKey = poolDetailsCacheKey(idx, poolName);

    auto loadFromCache = [this, &cacheKey, idx, poolName]() -> bool {
        const auto it = m_conns.poolDetailsCache.constFind(cacheKey);
        if (it == m_conns.poolDetailsCache.constEnd() || !it->loaded) {
            return false;
        }
        const PoolDetailsCacheEntry& cached = it.value();
        for (const QStringList& row : cached.propsRows) {
            if (row.size() < 3) {
                continue;
            }
            const int r = m_poolPropsTable->rowCount();
            m_poolPropsTable->insertRow(r);
            m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(row.value(0)));
            m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(row.value(1)));
            m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(row.value(2)));
        }
        m_poolStatusText->setPlainText(cached.statusText);
        applyPoolRootTooltipToVisibleTrees(idx, poolName, cached.statusText);
        return true;
    };

    const bool cacheHit = loadFromCache();

    if (!forceRefresh && cacheHit) {
        auto syncTreeIfPoolRootSelected = [this](QTreeWidget* tree) {
            if (!tree) {
                return;
            }
            QTreeWidgetItem* sel = tree->currentItem();
            if (sel && sel->data(0, kIsPoolRootRole).toBool()) {
                syncConnContentPoolColumnsFor(tree, connContentTokenForTree(tree));
            }
        };
        syncTreeIfPoolRootSelected(m_connContentTree);
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    if (!allowRemoteLoadIfMissing) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }

    if (!ensurePoolDetailsLoaded(idx, poolName)) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const PoolDetailsCacheEntry* loaded = poolDetailsEntry(idx, poolName);
    if (!loaded) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    for (const QStringList& row : loaded->propsRows) {
        if (row.size() < 3) {
            continue;
        }
        const int r = m_poolPropsTable->rowCount();
        m_poolPropsTable->insertRow(r);
        m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(row.value(0)));
        m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(row.value(1)));
        m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(row.value(2)));
    }
    m_poolStatusText->setPlainText(loaded->statusText);
    applyPoolRootTooltipToVisibleTrees(idx, poolName, loaded->statusText);
    auto syncTreeIfPoolRootSelected = [this](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        QTreeWidgetItem* sel = tree->currentItem();
        if (sel && sel->data(0, kIsPoolRootRole).toBool()) {
            syncConnContentPoolColumnsFor(tree, connContentTokenForTree(tree));
        }
    };
    syncTreeIfPoolRootSelected(m_connContentTree);
    setTablePopulationMode(m_poolPropsTable, false);
}
