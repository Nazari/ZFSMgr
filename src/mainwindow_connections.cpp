#include "mainwindow.h"

#include <QComboBox>
#include <QGroupBox>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QStackedWidget>
#include <QSplitter>
#include <QTreeWidget>

#include <QtConcurrent/QtConcurrent>

namespace {
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;

struct ConnTreeNavSnapshot {
    QSet<QString> expandedKeys;
    QString selectedKey;
};

QString connTreeNodeKey(QTreeWidgetItem* n) {
    if (!n) {
        return QString();
    }
    const QString pool = n->data(0, kPoolNameRole).toString().trimmed();
    if (n->data(0, kIsPoolRootRole).toBool()) {
        return pool.isEmpty() ? QString() : QStringLiteral("pool:%1").arg(pool);
    }
    const QString marker = n->data(0, kConnPropKeyRole).toString();
    if (marker == QStringLiteral("__pool_block_info__")) {
        return pool.isEmpty() ? QString() : QStringLiteral("info:%1").arg(pool);
    }
    const QString ds = n->data(0, Qt::UserRole).toString().trimmed();
    if (!ds.isEmpty()) {
        const QString snap = n->data(1, Qt::UserRole).toString().trimmed();
        return pool.isEmpty() ? QStringLiteral("ds:%1@%2").arg(ds, snap)
                              : QStringLiteral("ds:%1:%2@%3").arg(pool, ds, snap);
    }
    return QString();
}

QString datasetLeafNameConn(const QString& datasetName) {
    return datasetName.contains('/') ? datasetName.section('/', -1, -1) : datasetName;
}

void applySnapshotVisualStateConn(QTreeWidgetItem* item) {
    if (!item) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString().trimmed();
    if (ds.isEmpty()) {
        return;
    }
    const QString snap = item->data(1, Qt::UserRole).toString().trimmed();
    item->setText(0, snap.isEmpty() ? datasetLeafNameConn(ds)
                                    : QStringLiteral("%1@%2").arg(datasetLeafNameConn(ds), snap));
    const bool hideChildren = !snap.isEmpty();
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* ch = item->child(i);
        if (!ch) {
            continue;
        }
        const bool isDatasetNode = !ch->data(0, Qt::UserRole).toString().trimmed().isEmpty();
        if (isDatasetNode) {
            ch->setHidden(hideChildren);
        }
    }
}

struct SnapshotKeyParts {
    QString pool;
    QString dataset;
    QString snapshot;
};

SnapshotKeyParts parseSnapshotKey(const QString& key) {
    SnapshotKeyParts out;
    if (!key.startsWith(QStringLiteral("ds:"))) {
        return out;
    }
    QString body = key.mid(3);
    const int at = body.indexOf('@');
    out.snapshot = (at >= 0) ? body.mid(at + 1).trimmed() : QString();
    QString left = (at >= 0) ? body.left(at) : body;
    const int c = left.indexOf(':');
    if (c > 0 && !left.left(c).contains('/')) {
        out.pool = left.left(c).trimmed();
        out.dataset = left.mid(c + 1).trimmed();
    } else {
        out.dataset = left.trimmed();
    }
    return out;
}

QTreeWidgetItem* findDatasetNode(QTreeWidget* tree, const QString& pool, const QString& dataset) {
    if (!tree || dataset.isEmpty()) {
        return nullptr;
    }
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) {
        if (!n || found) {
            return;
        }
        const QString ds = n->data(0, Qt::UserRole).toString().trimmed();
        if (!ds.isEmpty() && ds == dataset) {
            const QString nodePool = n->data(0, kPoolNameRole).toString().trimmed();
            if (pool.isEmpty() || pool == nodePool) {
                found = n;
                return;
            }
        }
        for (int i = 0; i < n->childCount(); ++i) {
            rec(n->child(i));
            if (found) {
                return;
            }
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        rec(tree->topLevelItem(i));
        if (found) {
            break;
        }
    }
    return found;
}

ConnTreeNavSnapshot captureConnTreeNavSnapshot(QTreeWidget* tree) {
    ConnTreeNavSnapshot snap;
    if (!tree) {
        return snap;
    }
    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString k = connTreeNodeKey(n);
        if (!k.isEmpty() && n->isExpanded()) {
            snap.expandedKeys.insert(k);
        }
        for (int i = 0; i < n->childCount(); ++i) {
            collect(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        collect(tree->topLevelItem(i));
    }
    if (QTreeWidgetItem* cur = tree->currentItem()) {
        QTreeWidgetItem* owner = cur;
        while (owner && connTreeNodeKey(owner).isEmpty()) {
            owner = owner->parent();
        }
        if (owner) {
            snap.selectedKey = connTreeNodeKey(owner);
        }
    }
    return snap;
}

QTreeWidgetItem* findConnNodeByKey(QTreeWidget* tree, const QString& wantedKey) {
    if (!tree || wantedKey.isEmpty()) {
        return nullptr;
    }
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> recFind = [&](QTreeWidgetItem* n) {
        if (!n || found) {
            return;
        }
        if (connTreeNodeKey(n) == wantedKey) {
            found = n;
            return;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            recFind(n->child(i));
            if (found) {
                return;
            }
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        recFind(tree->topLevelItem(i));
        if (found) {
            break;
        }
    }
    return found;
}

QTreeWidgetItem* findParentNodeForDeletedDataset(QTreeWidget* tree, const QString& selectedKey) {
    if (!tree || !selectedKey.startsWith(QStringLiteral("ds:"))) {
        return nullptr;
    }
    QString rest = selectedKey.mid(3);
    QString poolPrefix;
    const int firstColon = rest.indexOf(':');
    if (firstColon > 0 && rest.left(firstColon).compare(QStringLiteral("pool"), Qt::CaseInsensitive) != 0) {
        const QString maybePool = rest.left(firstColon);
        if (!maybePool.contains('/')) {
            poolPrefix = maybePool;
            rest = rest.mid(firstColon + 1);
        }
    }
    const int at = rest.indexOf('@');
    QString ds = (at >= 0) ? rest.left(at) : rest;
    while (!ds.isEmpty()) {
        const int slash = ds.lastIndexOf('/');
        if (slash <= 0) {
            break;
        }
        ds = ds.left(slash);
        const QString parentKey = poolPrefix.isEmpty()
                                      ? QStringLiteral("ds:%1@").arg(ds)
                                      : QStringLiteral("ds:%1:%2@").arg(poolPrefix, ds);
        if (QTreeWidgetItem* sel = findConnNodeByKey(tree, parentKey)) {
            return sel;
        }
        const QString parentKeyNoAt = poolPrefix.isEmpty()
                                          ? QStringLiteral("ds:%1").arg(ds)
                                          : QStringLiteral("ds:%1:%2").arg(poolPrefix, ds);
        if (QTreeWidgetItem* sel = findConnNodeByKey(tree, parentKeyNoAt)) {
            return sel;
        }
    }
    return nullptr;
}

void restoreConnTreeNavSnapshot(QTreeWidget* tree, const ConnTreeNavSnapshot& snap) {
    if (!tree) {
        return;
    }
    QTreeWidgetItem* selectedItem = nullptr;
    std::function<void(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString k = connTreeNodeKey(n);
        if (!k.isEmpty() && snap.expandedKeys.contains(k)) {
            n->setExpanded(true);
        }
        if (!snap.selectedKey.isEmpty() && k == snap.selectedKey) {
            selectedItem = n;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            apply(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        apply(tree->topLevelItem(i));
    }
    if (!selectedItem) {
        selectedItem = findParentNodeForDeletedDataset(tree, snap.selectedKey);
    }
    if (!selectedItem && tree->topLevelItemCount() > 0) {
        selectedItem = tree->topLevelItem(0);
    }
    if (selectedItem) {
        tree->setCurrentItem(selectedItem);
    }
}

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

QString MainWindow::connectionPersistKey(int idx) const {
    if (idx < 0 || idx >= m_profiles.size()) {
        return QString();
    }
    const QString id = m_profiles[idx].id.trimmed();
    if (!id.isEmpty()) {
        return id.toLower();
    }
    return m_profiles[idx].name.trimmed().toLower();
}

bool MainWindow::isConnectionDisconnected(int idx) const {
    const QString key = connectionPersistKey(idx);
    return !key.isEmpty() && m_disconnectedConnectionKeys.contains(key);
}

void MainWindow::setConnectionDisconnected(int idx, bool disconnected) {
    const QString key = connectionPersistKey(idx);
    if (key.isEmpty()) {
        return;
    }
    if (disconnected) {
        m_disconnectedConnectionKeys.insert(key);
    } else {
        m_disconnectedConnectionKeys.remove(key);
    }
    saveUiSettings();
}

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
    for (int col = table->columnCount() - 1; col >= 0; --col) {
        const QTableWidgetItem* it = table->item(row, col);
        if (!it) {
            continue;
        }
        bool ok = false;
        const int idx = it->data(Qt::UserRole).toInt(&ok);
        if (ok) {
            return idx;
        }
    }
    return -1;
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
        populateAllPoolsTables();
        return;
    }
    const int generation = ++m_refreshGeneration;
    int refreshable = 0;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (!isConnectionDisconnected(i)) {
            ++refreshable;
        }
    }
    m_refreshPending = refreshable;
    m_refreshTotal = refreshable;
    m_refreshInProgress = (m_refreshPending > 0);
    updateBusyCursor();
    if (refreshable <= 0) {
        rebuildConnectionsTable();
        populateAllPoolsTables();
        return;
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        if (isConnectionDisconnected(i)) {
            continue;
        }
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
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    if (m_connectionEntityTabs) {
        const int t = m_connectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_connectionEntityTabs->count()) {
            const QString key = m_connectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshTopTabDataByConn[idx] = key;
            }
        }
    }
    if (m_bottomConnectionEntityTabs && m_bottomConnectionEntityTabs->isVisible()) {
        const int t = m_bottomConnectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_bottomConnectionEntityTabs->count()) {
            const QString key = m_bottomConnectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshBottomTabDataByConn[idx] = key;
            }
        }
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
    if (m_connectionEntityTabs) {
        const QString wantedTop = m_pendingRefreshTopTabDataByConn.value(idx);
        if (!wantedTop.isEmpty()) {
            for (int t = 0; t < m_connectionEntityTabs->count(); ++t) {
                if (m_connectionEntityTabs->tabData(t).toString() == wantedTop) {
                    m_connectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    if (m_bottomConnectionEntityTabs) {
        const QString wantedBottom = m_pendingRefreshBottomTabDataByConn.value(idx);
        if (!wantedBottom.isEmpty()) {
            for (int t = 0; t < m_bottomConnectionEntityTabs->count(); ++t) {
                if (m_bottomConnectionEntityTabs->tabData(t).toString() == wantedBottom) {
                    m_bottomConnectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    m_pendingRefreshTopTabDataByConn.remove(idx);
    m_pendingRefreshBottomTabDataByConn.remove(idx);
    if (selectedIdx >= 0 && m_connectionsTable) {
        const int row = rowForConnectionIndex(m_connectionsTable, selectedIdx);
        if (row >= 0) {
            m_connectionsTable->setCurrentCell(row, 2);
        }
    }
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
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
                m_connectionsTable->setCurrentCell(r, 2);
                break;
            }
        }
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

    QString selectionKey;
    if (m_connectionsTable) {
        int idx = m_topDetailConnIdx;
        if (idx < 0) {
            idx = selectedConnectionRow(m_connectionsTable);
        }
        if (idx >= 0 && isConnectionDisconnected(idx)) {
            idx = -1;
        }
        const QString tabKey =
            (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0
             && m_connectionEntityTabs->currentIndex() < m_connectionEntityTabs->count())
                ? m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString()
                : QString();
        selectionKey = QStringLiteral("%1|%2").arg(idx).arg(tabKey);
    }
    if (!selectionKey.isEmpty() && selectionKey == m_lastConnectionSelectionKey) {
        // Evita reconstrucciones redundantes (y consultas SSH) cuando el usuario
        // vuelve a pinchar la misma conexión/tab ya cargada.
        auto detailLoadedFor = [this](int connIdx, QTreeWidget* tree) -> bool {
            if (connIdx < 0) {
                return true;
            }
            if (!tree) {
                return false;
            }
            return tree->topLevelItemCount() > 0;
        };
        const bool topLoaded = detailLoadedFor(m_topDetailConnIdx, m_connContentTree);
        const bool bottomLoaded = detailLoadedFor(m_bottomDetailConnIdx, m_bottomConnContentTree);
        if (topLoaded && bottomLoaded) {
            updatePoolManagementBoxTitle();
            if (paintRoot) {
                paintRoot->setUpdatesEnabled(true);
                paintRoot->update();
            }
            return;
        }
        // Si falta contenido (p.ej. tras refresh), repoblar una sola vez.
        rebuildConnectionEntityTabs();
        refreshConnectionNodeDetails();
        updateSecondaryConnectionDetail();
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
    updateSecondaryConnectionDetail();
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(true);
        paintRoot->update();
    }
}

void MainWindow::updateSecondaryConnectionDetail() {
    if (!m_bottomConnContentTree) {
        return;
    }
    QScopedValueRollback<bool> rebuildingGuard(m_rebuildingBottomConnContentTree, true);
    const QSignalBlocker blockBottomTree(m_bottomConnContentTree);
    ConnTreeNavSnapshot nav;
    const int pendingConnIdx = m_bottomDetailConnIdx;
    if (pendingConnIdx >= 0 && m_pendingBottomExpandedKeysByConn.contains(pendingConnIdx)) {
        nav.expandedKeys = m_pendingBottomExpandedKeysByConn.take(pendingConnIdx);
        nav.selectedKey = m_pendingBottomSelectedKeyByConn.take(pendingConnIdx);
    } else {
        nav = captureConnTreeNavSnapshot(m_bottomConnContentTree);
    }
    if (pendingConnIdx >= 0 && m_forceRestoreBottomStateConnIdx == pendingConnIdx) {
        nav.expandedKeys = m_savedBottomExpandedKeysByConn.value(pendingConnIdx);
        nav.selectedKey = m_savedBottomSelectedKeyByConn.value(pendingConnIdx);
        m_forceRestoreBottomStateConnIdx = -1;
    } else if (nav.expandedKeys.isEmpty() && nav.selectedKey.isEmpty() && pendingConnIdx >= 0) {
        nav.expandedKeys = m_savedBottomExpandedKeysByConn.value(pendingConnIdx);
        nav.selectedKey = m_savedBottomSelectedKeyByConn.value(pendingConnIdx);
    }
    m_bottomConnContentTree->clear();
    if (m_bottomDetailConnIdx < 0 || m_bottomDetailConnIdx >= m_profiles.size()
        || m_bottomDetailConnIdx >= m_states.size() || isConnectionDisconnected(m_bottomDetailConnIdx)) {
        return;
    }
    const ConnectionRuntimeState& st = m_states[m_bottomDetailConnIdx];
    auto addPoolTree = [this](int connIdx, const QString& poolName, bool allowRemoteLoadIfMissing) {
        QTreeWidget tmp;
        populateDatasetTree(&tmp, connIdx, poolName, QStringLiteral("conncontent_multi"), allowRemoteLoadIfMissing);
        while (tmp.topLevelItemCount() > 0) {
            m_bottomConnContentTree->addTopLevelItem(tmp.takeTopLevelItem(0));
        }
    };
    for (const PoolImported& pool : st.importedPools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        addPoolTree(m_bottomDetailConnIdx, poolName, true);
    }
    for (const PoolImportable& pool : st.importablePools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        const QString stateUp = pool.state.trimmed().toUpper();
        const QString actionTxt = pool.action.trimmed();
        if (stateUp != QStringLiteral("ONLINE") || actionTxt.isEmpty()) {
            continue;
        }
        addPoolTree(m_bottomDetailConnIdx, poolName, false);
    }
    if (m_bottomConnContentTree->topLevelItemCount() == 0) {
        auto* noPools = new QTreeWidgetItem();
        noPools->setText(0, trk(QStringLiteral("t_no_pools_001"),
                                QStringLiteral("Sin Pools"),
                                QStringLiteral("No Pools"),
                                QStringLiteral("无存储池")));
        QFont f = noPools->font(0);
        f.setItalic(true);
        noPools->setFont(0, f);
        noPools->setFlags((noPools->flags() & ~Qt::ItemIsSelectable) & ~Qt::ItemIsEnabled);
        m_bottomConnContentTree->addTopLevelItem(noPools);
    }
    {
        const SnapshotKeyParts sk = parseSnapshotKey(nav.selectedKey);
        if (!sk.snapshot.isEmpty()) {
            if (QTreeWidgetItem* dsNode = findDatasetNode(m_bottomConnContentTree, sk.pool, sk.dataset)) {
                if (QComboBox* cb = qobject_cast<QComboBox*>(m_bottomConnContentTree->itemWidget(dsNode, 1))) {
                    const int idx = cb->findText(sk.snapshot);
                    if (idx > 0) {
                        const QSignalBlocker b(cb);
                        cb->setCurrentIndex(idx);
                        dsNode->setData(1, Qt::UserRole, sk.snapshot);
                        applySnapshotVisualStateConn(dsNode);
                    }
                }
            }
        }
    }
    restoreConnTreeNavSnapshot(m_bottomConnContentTree, nav);
    saveBottomTreeStateForConnection(m_bottomDetailConnIdx);
}

void MainWindow::saveTopTreeStateForConnection(int connIdx) {
    if (connIdx < 0 || !m_connContentTree) {
        return;
    }
    const ConnTreeNavSnapshot nav = captureConnTreeNavSnapshot(m_connContentTree);
    m_savedTopExpandedKeysByConn[connIdx] = nav.expandedKeys;
    m_savedTopSelectedKeyByConn[connIdx] = nav.selectedKey;
}

void MainWindow::saveBottomTreeStateForConnection(int connIdx) {
    if (connIdx < 0 || !m_bottomConnContentTree) {
        return;
    }
    const ConnTreeNavSnapshot nav = captureConnTreeNavSnapshot(m_bottomConnContentTree);
    m_savedBottomExpandedKeysByConn[connIdx] = nav.expandedKeys;
    m_savedBottomSelectedKeyByConn[connIdx] = nav.selectedKey;
}

void MainWindow::rebuildConnectionEntityTabs() {
    if (!m_connContentTree || !m_connectionsTable) {
        return;
    }
    ConnTreeNavSnapshot nav = captureConnTreeNavSnapshot(m_connContentTree);
    if (m_connectionEntityTabs) {
        while (m_connectionEntityTabs->count() > 0) {
            m_connectionEntityTabs->removeTab(m_connectionEntityTabs->count() - 1);
        }
    }
    int connIdx = m_topDetailConnIdx;
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()
        || isConnectionDisconnected(connIdx)) {
        m_connContentTree->clear();
        return;
    }
    if (m_forceRestoreTopStateConnIdx == connIdx) {
        nav.expandedKeys = m_savedTopExpandedKeysByConn.value(connIdx);
        nav.selectedKey = m_savedTopSelectedKeyByConn.value(connIdx);
        m_forceRestoreTopStateConnIdx = -1;
    } else if (nav.expandedKeys.isEmpty() && nav.selectedKey.isEmpty()) {
        nav.expandedKeys = m_savedTopExpandedKeysByConn.value(connIdx);
        nav.selectedKey = m_savedTopSelectedKeyByConn.value(connIdx);
    }
    m_connContentTree->clear();
    const ConnectionRuntimeState& st = m_states[connIdx];
    auto addPoolTree = [this](int cidx, const QString& poolName, bool allowRemoteLoadIfMissing) {
        QTreeWidget tmp;
        populateDatasetTree(&tmp, cidx, poolName, QStringLiteral("conncontent_multi"), allowRemoteLoadIfMissing);
        while (tmp.topLevelItemCount() > 0) {
            m_connContentTree->addTopLevelItem(tmp.takeTopLevelItem(0));
        }
    };
    for (const PoolImported& pool : st.importedPools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        addPoolTree(connIdx, poolName, true);
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
        addPoolTree(connIdx, poolName, false);
    }
    if (m_connContentTree->topLevelItemCount() == 0) {
        auto* noPools = new QTreeWidgetItem();
        noPools->setText(0, trk(QStringLiteral("t_no_pools_001"),
                                QStringLiteral("Sin Pools"),
                                QStringLiteral("No Pools"),
                                QStringLiteral("无存储池")));
        QFont f = noPools->font(0);
        f.setItalic(true);
        noPools->setFont(0, f);
        noPools->setFlags((noPools->flags() & ~Qt::ItemIsSelectable) & ~Qt::ItemIsEnabled);
        m_connContentTree->addTopLevelItem(noPools);
    }
    {
        const SnapshotKeyParts sk = parseSnapshotKey(nav.selectedKey);
        if (!sk.snapshot.isEmpty()) {
            if (QTreeWidgetItem* dsNode = findDatasetNode(m_connContentTree, sk.pool, sk.dataset)) {
                if (QComboBox* cb = qobject_cast<QComboBox*>(m_connContentTree->itemWidget(dsNode, 1))) {
                    const int idx = cb->findText(sk.snapshot);
                    if (idx > 0) {
                        const QSignalBlocker b(cb);
                        cb->setCurrentIndex(idx);
                        dsNode->setData(1, Qt::UserRole, sk.snapshot);
                        applySnapshotVisualStateConn(dsNode);
                    }
                }
            }
        }
    }
    restoreConnTreeNavSnapshot(m_connContentTree, nav);
    saveTopTreeStateForConnection(connIdx);
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
    if (connIdx < 0 || connIdx >= m_profiles.size() || isConnectionDisconnected(connIdx)) {
        return;
    }
    const int row = rowForConnectionIndex(m_connectionsTable, connIdx);
    if (row < 0) {
        return;
    }
    if (m_connectionsTable->currentRow() != row) {
        m_connectionsTable->setCurrentCell(row, 2);
    } else {
        refreshConnectionNodeDetails();
    }
}

void MainWindow::refreshConnectionNodeDetails() {
    auto setConnectionActionButtonsVisible = [this](bool visible) {
        Q_UNUSED(visible);
        if (m_connPropsRefreshBtn) {
            m_connPropsRefreshBtn->setVisible(false);
        }
        if (m_connPropsEditBtn) {
            m_connPropsEditBtn->setVisible(false);
        }
        if (m_connPropsDeleteBtn) {
            m_connPropsDeleteBtn->setVisible(false);
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

    int connIdx = m_topDetailConnIdx;
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

    // Modo sin tabs de pools: el contenido se muestra directamente en el árbol
    // (múltiples raíces de pool). No vaciar ni repoblar aquí por "pool activo".
    if (m_connectionEntityTabs && !m_connectionEntityTabs->isVisible()) {
        if (m_connPropsStack && m_connContentPage) {
            m_connPropsStack->setCurrentWidget(m_connContentPage);
        }
        if (m_connBottomGroup) {
            m_connBottomGroup->setVisible(false);
        }
        if (m_connDetailSplit) {
            m_connDetailSplit->setSizes({1, 0});
        }
        setConnectionActionButtonsVisible(false);
        setPoolActionButtonsVisible(false);
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    QString activePoolName;
    if (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0) {
        const QString key = m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString();
        const QStringList parts = key.split(':');
        if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
            bool ok = false;
            const int tabConnIdx = parts.value(1).toInt(&ok);
            if (ok && tabConnIdx == connIdx) {
                activePoolName = parts.value(2).trimmed();
            }
        }
    }

    const bool hasPoolTabSelected = !activePoolName.isEmpty();
    setConnectionActionButtonsVisible(false);
    setPoolActionButtonsVisible(hasPoolTabSelected);
    if (m_poolViewTabBar) {
        m_poolViewTabBar->setVisible(false);
        m_poolViewTabBar->setCurrentIndex(0);
        m_poolViewTabBar->setTabData(0, QVariant());
        m_poolViewTabBar->setTabData(1, QVariant());
    }
    if (!hasPoolTabSelected) {
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
        if (m_connBottomGroup) {
            m_connBottomGroup->setVisible(true);
        }
        if (m_poolPropsTable) {
            setTablePopulationMode(m_poolPropsTable, true);
            m_poolPropsTable->setRowCount(0);
            setTablePopulationMode(m_poolPropsTable, false);
        }
        if (m_poolStatusText) {
            m_poolStatusText->clear();
        }
        if (m_connContentTree) {
            m_connContentTree->clear();
        }
        if (m_connContentPropsTable) {
            setTablePopulationMode(m_connContentPropsTable, true);
            m_connContentPropsTable->setRowCount(0);
            setTablePopulationMode(m_connContentPropsTable, false);
        }
            resetPoolActionButtons();
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
    if (m_connPropsStack && m_connContentPage) {
        m_connPropsStack->setCurrentWidget(m_connContentPage);
    }
    if (m_connBottomGroup) {
        m_connBottomGroup->setVisible(false);
    }
    resetPoolActionButtons();
    refreshSelectedPoolDetails(false, true);
    if (connIdx >= 0 && connIdx < m_profiles.size() && m_connContentTree) {
        m_connContentToken = newConnContentToken;
        populateDatasetTree(m_connContentTree, connIdx, poolName, QStringLiteral("conncontent"), true);
        syncConnContentPoolColumns();
        refreshDatasetProperties(QStringLiteral("conncontent"));
    }
    updateConnectionActionsState();
    updateConnectionDetailTitlesForCurrentSelection();
}

void MainWindow::updateConnectionDetailTitlesForCurrentSelection() {
    int connIdx = m_topDetailConnIdx;
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
        if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
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
}

void MainWindow::refreshConnectionByIndex(int idx) {
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    if (m_bottomConnContentTree && idx == m_bottomDetailConnIdx) {
        QSet<QString> expandedKeys;
        QString selectedKey;
        std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* n) {
            if (!n) {
                return;
            }
            const QString k = connTreeNodeKey(n);
            if (!k.isEmpty() && n->isExpanded()) {
                expandedKeys.insert(k);
            }
            for (int i = 0; i < n->childCount(); ++i) {
                collect(n->child(i));
            }
        };
        for (int i = 0; i < m_bottomConnContentTree->topLevelItemCount(); ++i) {
            collect(m_bottomConnContentTree->topLevelItem(i));
        }
        if (QTreeWidgetItem* cur = m_bottomConnContentTree->currentItem()) {
            QTreeWidgetItem* owner = cur;
            while (owner && connTreeNodeKey(owner).isEmpty()) {
                owner = owner->parent();
            }
            if (owner) {
                selectedKey = connTreeNodeKey(owner);
            }
        }
        m_pendingBottomExpandedKeysByConn[idx] = expandedKeys;
        m_pendingBottomSelectedKeyByConn[idx] = selectedKey;
    }
    if (m_connectionEntityTabs) {
        const int t = m_connectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_connectionEntityTabs->count()) {
            const QString key = m_connectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshTopTabDataByConn[idx] = key;
            }
        }
    }
    if (m_bottomConnectionEntityTabs) {
        const int t = m_bottomConnectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_bottomConnectionEntityTabs->count()) {
            const QString key = m_bottomConnectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshBottomTabDataByConn[idx] = key;
            }
        }
    }
    if (!m_connContentToken.isEmpty() && m_connContentTree) {
        saveConnContentTreeState(m_connContentToken);
    }
    if (m_bottomConnContentTree && m_bottomConnectionEntityTabs
        && m_bottomConnectionEntityTabs->isVisible()) {
        const int bIdx = m_bottomConnectionEntityTabs->currentIndex();
        if (bIdx >= 0 && bIdx < m_bottomConnectionEntityTabs->count()) {
            const QString key = m_bottomConnectionEntityTabs->tabData(bIdx).toString();
            const QStringList parts = key.split(':');
            if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
                const QString bottomToken = QStringLiteral("%1::%2").arg(parts.value(1), parts.value(2).trimmed());
                const QString prevToken = m_connContentToken;
                QTreeWidget* prevTree = m_connContentTree;
                m_connContentTree = m_bottomConnContentTree;
                m_connContentToken = bottomToken;
                saveConnContentTreeState(bottomToken);
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
            }
        }
    }
    // Al refrescar una conexión, invalidar toda la caché asociada a todos sus pools.
    {
        const QString connPrefix = QStringLiteral("%1::").arg(m_profiles[idx].name.trimmed().toLower());
        auto dsIt = m_poolDatasetCache.begin();
        while (dsIt != m_poolDatasetCache.end()) {
            if (dsIt.key().startsWith(connPrefix)) {
                dsIt = m_poolDatasetCache.erase(dsIt);
            } else {
                ++dsIt;
            }
        }
        // Reutiliza la invalidación existente para detalle de pool + props de dataset.
        invalidatePoolDetailsCacheForConnection(idx);
        // Limpiar caché de propiedades inline del árbol de contenido para esta conexión.
        const QString uiPrefix = QStringLiteral("%1::").arg(QString::number(idx));
        auto valIt = m_connContentPropValuesByObject.begin();
        while (valIt != m_connContentPropValuesByObject.end()) {
            if (valIt.key().startsWith(uiPrefix)) {
                valIt = m_connContentPropValuesByObject.erase(valIt);
            } else {
                ++valIt;
            }
        }
    }
    m_states[idx] = refreshConnection(m_profiles[idx]);
    rebuildConnectionsTable();
    if (m_connectionEntityTabs) {
        const QString wantedTop = m_pendingRefreshTopTabDataByConn.value(idx);
        if (!wantedTop.isEmpty()) {
            for (int t = 0; t < m_connectionEntityTabs->count(); ++t) {
                if (m_connectionEntityTabs->tabData(t).toString() == wantedTop) {
                    m_connectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    if (m_bottomConnectionEntityTabs && m_bottomConnectionEntityTabs->isVisible()) {
        const QString wantedBottom = m_pendingRefreshBottomTabDataByConn.value(idx);
        if (!wantedBottom.isEmpty()) {
            for (int t = 0; t < m_bottomConnectionEntityTabs->count(); ++t) {
                if (m_bottomConnectionEntityTabs->tabData(t).toString() == wantedBottom) {
                    m_bottomConnectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    m_pendingRefreshTopTabDataByConn.remove(idx);
    m_pendingRefreshBottomTabDataByConn.remove(idx);
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
    {
        QSet<QString> validKeys;
        for (int i = 0; i < m_profiles.size(); ++i) {
            const QString key = connectionPersistKey(i);
            if (!key.isEmpty()) {
                validKeys.insert(key);
            }
        }
        for (auto it = m_disconnectedConnectionKeys.begin(); it != m_disconnectedConnectionKeys.end();) {
            if (!validKeys.contains(*it)) {
                it = m_disconnectedConnectionKeys.erase(it);
            } else {
                ++it;
            }
        }
    }
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
            for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
                const int connIdx = connectionIndexForRow(m_connectionsTable, r);
                if (connIdx >= 0 && connIdx < m_profiles.size() && !isConnectionDisconnected(connIdx)) {
                    targetRow = r;
                    break;
                }
            }
        }
        if (targetRow >= 0) {
            m_connectionsTable->setCurrentCell(targetRow, 2);
        }
    }

    syncConnectionLogTabs();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildConnectionsTable() {
    beginUiBusy();
    m_syncConnSelectorChecks = true;
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
    m_connectionsTable->setColumnCount(3);
    m_connectionsTable->setHorizontalHeaderLabels({
        trk(QStringLiteral("t_conn_col_src_01"),
            QStringLiteral("Origen"),
            QStringLiteral("Source"),
            QStringLiteral("来源")),
        trk(QStringLiteral("t_conn_col_dst_01"),
            QStringLiteral("Destino"),
            QStringLiteral("Target"),
            QStringLiteral("目标")),
        trk(QStringLiteral("t_connections_001"),
            QStringLiteral("Conexión"),
            QStringLiteral("Connection"),
            QStringLiteral("连接"))
    });
    m_connectionsTable->setRowCount(0);
    auto buildConnectionStateTooltip = [this](const ConnectionProfile& p, const ConnectionRuntimeState& st) {
        QStringList lines;
        lines << QStringLiteral("Host: %1").arg(p.host);
        lines << QStringLiteral("Port: %1").arg(p.port);
        lines << QStringLiteral("Estado: %1").arg(st.status.trimmed().isEmpty() ? QStringLiteral("-") : st.status.trimmed());
        lines << QStringLiteral("Detalle: %1").arg(st.detail.trimmed().isEmpty() ? QStringLiteral("-") : st.detail.trimmed());
        lines << QStringLiteral("Sistema operativo: %1")
                     .arg(st.osLine.trimmed().isEmpty() ? QStringLiteral("-") : st.osLine.trimmed());
        lines << QStringLiteral("Método de conexión: %1")
                     .arg(st.connectionMethod.trimmed().isEmpty() ? p.connType : st.connectionMethod.trimmed());
        lines << QStringLiteral("OpenZFS: %1")
                     .arg(st.zfsVersionFull.trimmed().isEmpty()
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
            nonImportable << QStringLiteral("%1").arg(poolName);
            nonImportable << QStringLiteral("  Motivo: %1").arg(reason);
        }
        lines << QStringLiteral("Pools no importables:");
        if (nonImportable.isEmpty()) {
            lines << QStringLiteral("  (ninguno)");
        } else {
            lines += nonImportable;
        }
        const QString plain = lines.join(QStringLiteral("\n")).toHtmlEscaped();
        return QStringLiteral("<pre style=\"font-family:monospace; white-space:pre;\">%1</pre>").arg(plain);
    };
    QStringList redirectedToLocalNames;
    auto zfsVersionTooOld = [](const QString& rawVersion) -> bool {
        const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
        const QRegularExpressionMatch m = rx.match(rawVersion.trimmed());
        if (!m.hasMatch()) {
            return false;
        }
        const int major = m.captured(1).toInt();
        const int minor = m.captured(2).toInt();
        const int patch = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
        if (major != 2) return false;
        if (minor < 3) return true;
        if (minor > 3) return false;
        return patch < 3;
    };
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
        const bool disconnected = isConnectionDisconnected(i);
        const QString st = s.status.trimmed().toUpper();
        const bool localConn = isLocalConnection(p);
        const bool redirectedLocal = isConnectionRedirectedToLocal(i);
        if (redirectedLocal) {
            continue;
        }
        if (localConn && !s.machineUuid.trimmed().isEmpty()) {
            m_localMachineUuid = s.machineUuid.trimmed();
        }
        if (disconnected) {
            statusTag = QStringLiteral("[Off]");
            rowColor = QColor("#8b9299");
        } else if (st == QStringLiteral("OK")) {
            statusTag = QStringLiteral("[Ok]");
            rowColor = s.missingUnixCommands.isEmpty() ? QColor("#1f7a1f") : QColor("#c77900");
            if (zfsVersionTooOld(s.zfsVersion)) {
                rowColor = QColor("#a12a2a");
            }
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
        auto* itTop = new QTableWidgetItem(QString());
        itTop->setData(Qt::UserRole, i);
        Qt::ItemFlags topFlags = (itTop->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable;
        if (disconnected) {
            topFlags &= ~Qt::ItemIsEnabled;
        } else {
            topFlags |= Qt::ItemIsEnabled;
        }
        itTop->setFlags(topFlags);
        itTop->setCheckState((i == m_topDetailConnIdx) ? Qt::Checked : Qt::Unchecked);
        itTop->setTextAlignment(Qt::AlignCenter);
        itTop->setToolTip(buildConnectionStateTooltip(p, s));
        m_connectionsTable->setItem(row, 0, itTop);

        auto* itBottom = new QTableWidgetItem(QString());
        itBottom->setData(Qt::UserRole, i);
        Qt::ItemFlags bottomFlags = (itBottom->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable;
        if (disconnected) {
            bottomFlags &= ~Qt::ItemIsEnabled;
        } else {
            bottomFlags |= Qt::ItemIsEnabled;
        }
        itBottom->setFlags(bottomFlags);
        itBottom->setCheckState((i == m_bottomDetailConnIdx) ? Qt::Checked : Qt::Unchecked);
        itBottom->setTextAlignment(Qt::AlignCenter);
        itBottom->setToolTip(buildConnectionStateTooltip(p, s));
        m_connectionsTable->setItem(row, 1, itBottom);

        auto* it = new QTableWidgetItem(line);
        it->setData(Qt::UserRole, i);
        it->setForeground(QBrush(rowColor));
        if (disconnected) {
            QFont f = it->font();
            f.setItalic(true);
            it->setFont(f);
        }
        it->setToolTip(buildConnectionStateTooltip(p, s));
        m_connectionsTable->setItem(row, 2, it);

    }
    auto ensureValidConnIdx = [this](int& idx) {
        if (idx < 0) {
            return false;
        }
        if (isConnectionDisconnected(idx)) {
            return false;
        }
        return rowForConnectionIndex(m_connectionsTable, idx) >= 0;
    };
    const int rowCount = m_connectionsTable->rowCount();
    auto firstConnectedIndex = [this]() -> int {
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
                return idx;
            }
        }
        return -1;
    };
    auto secondConnectedIndex = [this](int firstIdx) -> int {
        bool seenFirst = false;
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
                continue;
            }
            if (!seenFirst && idx == firstIdx) {
                seenFirst = true;
                continue;
            }
            return idx;
        }
        return -1;
    };
    if (!m_connSelectorDefaultsInitialized) {
        if (!ensureValidConnIdx(m_topDetailConnIdx)) {
            m_topDetailConnIdx = (rowCount > 0) ? firstConnectedIndex() : -1;
        }
        if (!ensureValidConnIdx(m_bottomDetailConnIdx)) {
            if (rowCount > 1) {
                // Inicialmente: primera conexión conectada como Origen y segunda conectada como Destino.
                const int second = secondConnectedIndex(m_topDetailConnIdx);
                m_bottomDetailConnIdx = (second >= 0) ? second : m_topDetailConnIdx;
            } else {
                m_bottomDetailConnIdx = m_topDetailConnIdx;
            }
        }
        m_connSelectorDefaultsInitialized = true;
    } else {
        if (!ensureValidConnIdx(m_topDetailConnIdx)) {
            m_topDetailConnIdx = -1;
        }
        if (!ensureValidConnIdx(m_bottomDetailConnIdx)) {
            m_bottomDetailConnIdx = -1;
        }
    }
    for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
        if (QTableWidgetItem* it = m_connectionsTable->item(r, 0)) {
            const int idx = it->data(Qt::UserRole).toInt();
            it->setCheckState((idx == m_topDetailConnIdx) ? Qt::Checked : Qt::Unchecked);
        }
        if (QTableWidgetItem* it = m_connectionsTable->item(r, 1)) {
            const int idx = it->data(Qt::UserRole).toInt();
            it->setCheckState((idx == m_bottomDetailConnIdx) ? Qt::Checked : Qt::Unchecked);
        }
    }
    int targetRow = rowForConnectionIndex(m_connectionsTable, m_topDetailConnIdx);
    const QString preferredKey = !m_userSelectedConnectionKey.trimmed().isEmpty()
                                     ? m_userSelectedConnectionKey.trimmed().toLower()
                                     : prevSelectedKey;
    if (!preferredKey.isEmpty()) {
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            QTableWidgetItem* it = m_connectionsTable->item(r, 2);
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
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
                targetRow = r;
                break;
            }
        }
    }
    if (targetRow >= 0) {
        m_connectionsTable->setCurrentCell(targetRow, 2);
    }
    m_syncConnSelectorChecks = false;

    rebuildConnectionEntityTabs();
    syncConnectionLogTabs();
    endUiBusy();
    refreshConnectionNodeDetails();
    updateSecondaryConnectionDetail();
    updatePoolManagementBoxTitle();
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
                    m_connectionsTable->setCurrentCell(row, 2);
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
