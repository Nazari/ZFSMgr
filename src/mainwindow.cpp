#include "mainwindow.h"
#include "mainwindow_ui_logic.h"

#include <algorithm>
#include <QComboBox>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>

namespace {
bool zfsmgrTestModeEnabled() {
    const QByteArray value = qgetenv("ZFSMGR_TEST_MODE");
    return !value.isEmpty() && value != "0" && value.compare("false", Qt::CaseInsensitive) != 0;
}

QString canonicalDatasetParentName(const QString& fullName) {
    const QString trimmed = fullName.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const int snapPos = trimmed.indexOf('@');
    if (snapPos > 0) {
        return trimmed.left(snapPos);
    }
    const int slashPos = trimmed.lastIndexOf('/');
    if (slashPos > 0) {
        return trimmed.left(slashPos);
    }
    return QString();
}
}

MainWindow::MainWindow(const QString& masterPassword, const QString& language, QWidget* parent)
    : QMainWindow(parent)
    , m_store(QStringLiteral("ZFSMgr")) {
    setObjectName(QStringLiteral("mainWindow"));
    m_language = language.trimmed().toLower();
    if (m_language.isEmpty()) {
        m_language = QStringLiteral("es");
    }
    loadUiSettings();
    if (!language.trimmed().isEmpty()) {
        m_language = language.trimmed().toLower();
        saveUiSettings();
    }
    m_store.setLanguage(m_language);
    m_store.setMasterPassword(masterPassword);
    initLogPersistence();
    buildUi();
    if (!zfsmgrTestModeEnabled()) {
        loadConnections();
        ensureStartupLocalSudoConnection();
        QTimer::singleShot(0, this, [this]() {
            refreshAllConnections();
        });
    }
    rebuildConnInfoModel();
}

void MainWindow::configureSingleConnectionUiTestState(const ConnectionProfile& profile,
                                                      const QStringList& importedPools,
                                                      const QStringList& importablePools) {
    m_profiles.clear();
    m_states.clear();
    m_profiles.push_back(profile);

    ConnectionRuntimeState state;
    state.status = QStringLiteral("OK");
    state.detail = QStringLiteral("test");
    state.connectionMethod = profile.connType.trimmed();
    for (const QString& poolName : importedPools) {
        const QString trimmed = poolName.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        state.importedPools.push_back(PoolImported{profile.name, trimmed, QStringLiteral("Exportar")});
    }
    for (const QString& poolName : importablePools) {
        const QString trimmed = poolName.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        state.importablePools.push_back(
            PoolImportable{profile.name, trimmed, QString(), QStringLiteral("ONLINE"), QString(), QStringLiteral("Importar")});
    }
    m_states.push_back(state);
    rebuildConnInfoModel();

    rebuildConnectionsTable();
    m_topDetailConnIdx = 0;
    m_bottomDetailConnIdx = 0;
    if (m_connectionsTable && m_connectionsTable->rowCount() > 0) {
        m_connectionsTable->setCurrentCell(0, 0);
    }
}

void MainWindow::rebuildConnectionDetailsForTest() {
    rebuildConnectionEntityTabs();
    updateSecondaryConnectionDetail();
}

void MainWindow::configurePoolDatasetsForTest(int connIdx,
                                              const QString& poolName,
                                              const QVector<UiTestDatasetSeed>& datasets) {
    if (connIdx < 0 || connIdx >= m_profiles.size() || poolName.trimmed().isEmpty()) {
        return;
    }
    PoolDatasetCache cache;
    cache.loaded = true;
    for (const UiTestDatasetSeed& seed : datasets) {
        DatasetRecord record;
        record.name = seed.name.trimmed();
        record.mountpoint = seed.mountpoint.trimmed();
        record.canmount = seed.canmount.trimmed();
        record.mounted = seed.mounted.trimmed();
        if (record.name.isEmpty()) {
            continue;
        }
        cache.datasets.push_back(record);
        cache.recordByName.insert(record.name, record);
        cache.snapshotsByDataset.insert(record.name, seed.snapshots);
    }
    m_poolDatasetCache.insert(datasetCacheKey(connIdx, poolName), cache);
    rebuildConnInfoFor(connIdx);
}

void MainWindow::setShowPoolInfoNodeForTest(bool visible) {
    m_showPoolInfoNodeTop = visible;
    m_showPoolInfoNodeBottom = visible;
    if (m_topDatasetPane) {
        auto options = m_topDatasetPane->visualOptions();
        options.showPoolInfo = visible;
        m_topDatasetPane->setVisualOptions(options);
    }
    if (m_bottomDatasetPane) {
        auto options = m_bottomDatasetPane->visualOptions();
        options.showPoolInfo = visible;
        m_bottomDatasetPane->setVisualOptions(options);
    }
    rebuildConnectionDetailsForTest();
}

void MainWindow::setShowInlineGsaNodeForTest(bool visible) {
    m_showInlineGsaNodeTop = visible;
    m_showInlineGsaNodeBottom = visible;
    if (m_topDatasetPane) {
        auto options = m_topDatasetPane->visualOptions();
        options.showInlineGsa = visible;
        m_topDatasetPane->setVisualOptions(options);
    }
    if (m_bottomDatasetPane) {
        auto options = m_bottomDatasetPane->visualOptions();
        options.showInlineGsa = visible;
        m_bottomDatasetPane->setVisualOptions(options);
    }
    rebuildConnectionDetailsForTest();
}

void MainWindow::setShowAutomaticSnapshotsForTest(bool visible) {
    m_showAutomaticGsaSnapshots = visible;
    rebuildConnectionDetailsForTest();
}

void MainWindow::setConnectionGsaStateForTest(int connIdx, bool installed, bool active, const QString& version) {
    if (connIdx < 0 || connIdx >= m_states.size()) {
        return;
    }
    ConnectionRuntimeState& state = m_states[connIdx];
    state.gsaInstalled = installed;
    state.gsaActive = active;
    if (!version.trimmed().isEmpty()) {
        state.gsaVersion = version.trimmed();
    }
}

void MainWindow::configureDatasetPropertiesForTest(int connIdx,
                                                   const QString& objectName,
                                                   const QString& datasetType,
                                                   const QVector<UiTestPropertySeed>& rows) {
    const QString trimmedObject = objectName.trimmed();
    const QString poolName = trimmedObject.section('/', 0, 0).trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedObject.isEmpty()) {
        return;
    }
    QVector<DatasetPropCacheRow> cacheRows;
    for (const UiTestPropertySeed& seed : rows) {
        const QString prop = seed.prop.trimmed();
        if (prop.isEmpty()) {
            continue;
        }
        DatasetPropCacheRow row;
        row.prop = prop;
        row.value = seed.value;
        row.source = seed.source.trimmed().isEmpty() ? QStringLiteral("local") : seed.source.trimmed();
        row.readonly = seed.readonly.trimmed().isEmpty() ? QStringLiteral("no") : seed.readonly.trimmed();
        cacheRows.push_back(row);
    }
    storeDatasetPropertyRows(connIdx, poolName, trimmedObject, datasetType.trimmed(), cacheRows);
}

QString MainWindow::connStableIdForIndex(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return QStringLiteral("conn-%1").arg(connIdx);
    }
    const ConnectionProfile& profile = m_profiles[connIdx];
    const QString id = profile.id.trimmed();
    if (!id.isEmpty()) {
        return id;
    }
    const QString name = profile.name.trimmed();
    if (!name.isEmpty()) {
        return name;
    }
    return QStringLiteral("conn-%1").arg(connIdx);
}

QString MainWindow::poolStableId(const PoolKey& key) const {
    if (!key.poolGuid.trimmed().isEmpty()) {
        return key.poolGuid.trimmed();
    }
    return key.poolName.trimmed();
}

QString MainWindow::dsStableId(const DSKey& key) const {
    return key.fullName.trimmed();
}

MainWindow::DSKind MainWindow::dsKindFromNames(const QString& fullName, const QString& datasetType) {
    const QString trimmedType = datasetType.trimmed().toLower();
    if (trimmedType == QStringLiteral("filesystem")) {
        return DSKind::Filesystem;
    }
    if (trimmedType == QStringLiteral("volume")) {
        return DSKind::Volume;
    }
    if (trimmedType == QStringLiteral("snapshot")) {
        return DSKind::Snapshot;
    }
    if (fullName.contains('@')) {
        return DSKind::Snapshot;
    }
    return DSKind::Unknown;
}

void MainWindow::rebuildPoolInfoFromCache(PoolInfo& poolInfo,
                                          int connIdx,
                                          const QString& poolName,
                                          const PoolInfo* previousPoolInfo) {
    const QString datasetKey = datasetCacheKey(connIdx, poolName);
    const auto datasetIt = m_poolDatasetCache.constFind(datasetKey);
    if (datasetIt == m_poolDatasetCache.cend() || !datasetIt->loaded) {
        return;
    }

    const PoolDatasetCache& cache = *datasetIt;
    QSet<QString> rootObjects;
    for (const DatasetRecord& record : cache.datasets) {
        const QString fullName = record.name.trimmed();
        if (fullName.isEmpty()) {
            continue;
        }
        DSInfo& dsInfo = poolInfo.objectsByFullName[fullName];
        dsInfo.key = DSKey{poolInfo.key, fullName};
        dsInfo.kind = dsKindFromNames(fullName, QStringLiteral("filesystem"));
        dsInfo.parentFullName = canonicalDatasetParentName(fullName);
        dsInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
        dsInfo.runtime.propertiesState = LoadState::Loaded;
        dsInfo.runtime.datasetType = QStringLiteral("filesystem");
        dsInfo.runtime.properties.insert(QStringLiteral("used"), record.used);
        dsInfo.runtime.properties.insert(QStringLiteral("compressratio"), record.compressRatio);
        dsInfo.runtime.properties.insert(QStringLiteral("encryption"), record.encryption);
        dsInfo.runtime.properties.insert(QStringLiteral("creation"), record.creation);
        dsInfo.runtime.properties.insert(QStringLiteral("referenced"), record.referenced);
        dsInfo.runtime.properties.insert(QStringLiteral("mounted"), record.mounted);
        dsInfo.runtime.properties.insert(QStringLiteral("mountpoint"), record.mountpoint);
        dsInfo.runtime.properties.insert(QStringLiteral("canmount"), record.canmount);
        dsInfo.capabilities.canDestroy = true;
        dsInfo.capabilities.canRename = true;
        dsInfo.capabilities.canManagePermissions = true;
        dsInfo.capabilities.canManageSchedules = true;
        dsInfo.capabilities.canMount = true;
        dsInfo.capabilities.canUnmount = true;
        dsInfo.editSession.target = dsInfo.key;
        if (dsInfo.parentFullName.isEmpty()) {
            rootObjects.insert(fullName);
        } else {
            rootObjects.remove(fullName);
            DSInfo& parentInfo = poolInfo.objectsByFullName[dsInfo.parentFullName];
            if (!parentInfo.childFullNames.contains(fullName)) {
                parentInfo.childFullNames.push_back(fullName);
            }
        }
    }

    for (auto it = cache.snapshotsByDataset.cbegin(); it != cache.snapshotsByDataset.cend(); ++it) {
        const QString datasetName = it.key().trimmed();
        if (datasetName.isEmpty()) {
            continue;
        }
        DSInfo& datasetInfo = poolInfo.objectsByFullName[datasetName];
        if (datasetInfo.key.fullName.isEmpty()) {
            datasetInfo.key = DSKey{poolInfo.key, datasetName};
            datasetInfo.kind = DSKind::Filesystem;
            datasetInfo.parentFullName = canonicalDatasetParentName(datasetName);
            datasetInfo.runtime.datasetType = QStringLiteral("filesystem");
            datasetInfo.editSession.target = datasetInfo.key;
            if (datasetInfo.parentFullName.isEmpty()) {
                rootObjects.insert(datasetName);
            }
        }
        datasetInfo.runtime.directSnapshots = it.value();
        for (const QString& snapNameOnly : it.value()) {
            const QString snapTrimmed = snapNameOnly.trimmed();
            if (snapTrimmed.isEmpty()) {
                continue;
            }
            const QString fullSnapshotName =
                snapTrimmed.startsWith(datasetName + QLatin1Char('@'))
                    ? snapTrimmed
                    : QStringLiteral("%1@%2").arg(datasetName, snapTrimmed);
            DSInfo& snapInfo = poolInfo.objectsByFullName[fullSnapshotName];
            snapInfo.key = DSKey{poolInfo.key, fullSnapshotName};
            snapInfo.kind = DSKind::Snapshot;
            snapInfo.parentFullName = datasetName;
            snapInfo.runtime.datasetType = QStringLiteral("snapshot");
            snapInfo.runtime.propertiesState = LoadState::Loaded;
            snapInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
            snapInfo.editSession.target = snapInfo.key;
            if (!datasetInfo.childFullNames.contains(fullSnapshotName)) {
                datasetInfo.childFullNames.push_back(fullSnapshotName);
            }
        }
    }

    for (auto it = poolInfo.objectsByFullName.begin(); it != poolInfo.objectsByFullName.end(); ++it) {
        auto& children = it->childFullNames;
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
    }
    poolInfo.rootObjectNames = rootObjects.values();
    std::sort(poolInfo.rootObjectNames.begin(), poolInfo.rootObjectNames.end());

    for (auto it = poolInfo.objectsByFullName.begin(); it != poolInfo.objectsByFullName.end(); ++it) {
        if (previousPoolInfo) {
            const auto prevDsIt = previousPoolInfo->objectsByFullName.constFind(it.key());
            if (prevDsIt != previousPoolInfo->objectsByFullName.cend()
                && prevDsIt->runtime.propertiesState == LoadState::Loaded
                && !prevDsIt->runtime.propertyRows.isEmpty()) {
                it->runtime.propertiesState = LoadState::Loaded;
                it->runtime.datasetType =
                    prevDsIt->runtime.datasetType.trimmed().isEmpty()
                        ? it->runtime.datasetType
                        : prevDsIt->runtime.datasetType.trimmed();
                it->runtime.propertyRows = prevDsIt->runtime.propertyRows;
                it->runtime.properties = prevDsIt->runtime.properties;
                it->kind = dsKindFromNames(it.key(), it->runtime.datasetType);
            }
        }

        const QString permsKey = datasetPermissionsCacheKey(connIdx, poolName, it.key());
        const auto permsIt = m_datasetPermissionsCache.constFind(permsKey);
        if (permsIt != m_datasetPermissionsCache.cend() && permsIt->loaded) {
            it->runtime.permissionsState = LoadState::Loaded;
            it->permissionsCache = permsIt.value();
        }
    }
}

void MainWindow::rebuildConnInfoFor(int connIdx) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }

    const ConnInfo* oldConnInfo = findConnInfo(connIdx);

    ConnInfo connInfo;
    connInfo.key.connectionId = connStableIdForIndex(connIdx);
    connInfo.connIdx = connIdx;
    connInfo.profile = m_profiles[connIdx];
    if (connIdx < m_states.size()) {
        connInfo.runtime.state = LoadState::Loaded;
        connInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
        connInfo.runtime.snapshot = m_states[connIdx];
    }

    const ConnectionRuntimeState state = (connIdx < m_states.size()) ? m_states[connIdx] : ConnectionRuntimeState{};
    auto ensurePool = [&](const QString& poolName, const QString& guid) -> PoolInfo& {
        PoolKey key{connInfo.key, guid.trimmed(), poolName.trimmed()};
        const QString stableId = poolStableId(key);
        PoolInfo& poolInfo = connInfo.poolsByStableId[stableId];
        poolInfo.key = key;
        return poolInfo;
    };

    for (const PoolImported& pool : state.importedPools) {
        PoolInfo& poolInfo = ensurePool(pool.pool, QString());
        poolInfo.runtime.imported = true;
        poolInfo.runtime.importAction = pool.action;
        poolInfo.runtime.poolStatusText = state.poolStatusByName.value(pool.pool.trimmed());
        if (!poolInfo.runtime.poolStatusText.trimmed().isEmpty()) {
            poolInfo.runtime.detailsState = LoadState::Loaded;
            poolInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
        }
    }

    for (const PoolImportable& pool : state.importablePools) {
        PoolInfo& poolInfo = ensurePool(pool.pool, pool.guid);
        poolInfo.runtime.importable = true;
        poolInfo.runtime.importState = pool.state;
        poolInfo.runtime.importReason = pool.reason;
        poolInfo.runtime.importAction = pool.action;
    }

    for (auto it = connInfo.poolsByStableId.begin(); it != connInfo.poolsByStableId.end(); ++it) {
        PoolInfo& poolInfo = it.value();
        const QString cacheKey = poolDetailsCacheKey(connIdx, poolInfo.key.poolName);
        const auto poolDetailsIt = m_poolDetailsCache.constFind(cacheKey);
        if (poolDetailsIt != m_poolDetailsCache.cend() && poolDetailsIt->loaded) {
            poolInfo.runtime.detailsState = LoadState::Loaded;
            poolInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
            poolInfo.runtime.poolStatusText = poolDetailsIt->statusText;
            for (const QStringList& row : poolDetailsIt->propsRows) {
                if (row.size() >= 2) {
                    poolInfo.runtime.zpoolProperties.insert(row.value(0).trimmed(), row.value(1));
                }
            }
        }
        const PoolInfo* previousPoolInfo = nullptr;
        if (oldConnInfo) {
            const auto prevPoolIt = oldConnInfo->poolsByStableId.constFind(it.key());
            if (prevPoolIt != oldConnInfo->poolsByStableId.cend()) {
                previousPoolInfo = &prevPoolIt.value();
            }
        }
        rebuildPoolInfoFromCache(poolInfo, connIdx, poolInfo.key.poolName, previousPoolInfo);
    }

    m_connInfoById.insert(connInfo.key.connectionId, connInfo);
}

void MainWindow::rebuildConnInfoModel() {
    m_connInfoById.clear();
    for (int i = 0; i < m_profiles.size(); ++i) {
        rebuildConnInfoFor(i);
    }
}

const MainWindow::ConnInfo* MainWindow::findConnInfo(int connIdx) const {
    const QString stableId = connStableIdForIndex(connIdx);
    const auto it = m_connInfoById.constFind(stableId);
    return (it == m_connInfoById.cend()) ? nullptr : &it.value();
}

MainWindow::ConnInfo* MainWindow::findConnInfo(int connIdx) {
    const QString stableId = connStableIdForIndex(connIdx);
    const auto it = m_connInfoById.find(stableId);
    return (it == m_connInfoById.end()) ? nullptr : &it.value();
}

const MainWindow::PoolInfo* MainWindow::findPoolInfo(int connIdx, const QString& poolName) const {
    const ConnInfo* connInfo = findConnInfo(connIdx);
    if (!connInfo) {
        return nullptr;
    }
    const QString trimmedPool = poolName.trimmed();
    for (auto it = connInfo->poolsByStableId.cbegin(); it != connInfo->poolsByStableId.cend(); ++it) {
        if (it->key.poolName.trimmed() == trimmedPool) {
            return &it.value();
        }
    }
    return nullptr;
}

MainWindow::PoolInfo* MainWindow::findPoolInfo(int connIdx, const QString& poolName) {
    ConnInfo* connInfo = findConnInfo(connIdx);
    if (!connInfo) {
        return nullptr;
    }
    const QString trimmedPool = poolName.trimmed();
    for (auto it = connInfo->poolsByStableId.begin(); it != connInfo->poolsByStableId.end(); ++it) {
        if (it->key.poolName.trimmed() == trimmedPool) {
            return &it.value();
        }
    }
    return nullptr;
}

const MainWindow::DSInfo* MainWindow::findDsInfo(int connIdx, const QString& poolName, const QString& fullName) const {
    const PoolInfo* poolInfo = findPoolInfo(connIdx, poolName);
    if (!poolInfo) {
        return nullptr;
    }
    const auto it = poolInfo->objectsByFullName.constFind(fullName.trimmed());
    return (it == poolInfo->objectsByFullName.cend()) ? nullptr : &it.value();
}

MainWindow::DSInfo* MainWindow::findDsInfo(int connIdx, const QString& poolName, const QString& fullName) {
    PoolInfo* poolInfo = findPoolInfo(connIdx, poolName);
    if (!poolInfo) {
        return nullptr;
    }
    const auto it = poolInfo->objectsByFullName.find(fullName.trimmed());
    return (it == poolInfo->objectsByFullName.end()) ? nullptr : &it.value();
}

QStringList MainWindow::datasetSnapshotsFromModel(int connIdx, const QString& poolName, const QString& datasetName) const {
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return {};
    }
    return dsInfo->runtime.directSnapshots;
}

bool MainWindow::datasetMountedFromModel(int connIdx, const QString& poolName, const QString& datasetName, QString* mountedValueOut) const {
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return false;
    }
    const QString mountedValue = dsInfo->runtime.properties.value(QStringLiteral("mounted")).trimmed();
    if (mountedValueOut) {
        *mountedValueOut = mountedValue;
    }
    return !mountedValue.isEmpty();
}

bool MainWindow::datasetExistsInModel(int connIdx, const QString& poolName, const QString& datasetName) const {
    return findDsInfo(connIdx, poolName, datasetName) != nullptr;
}

QVector<MainWindow::DatasetPropCacheRow> MainWindow::datasetPropertyRowsFromModelOrCache(int connIdx,
                                                                                         const QString& poolName,
                                                                                         const QString& objectName) const {
    if (const DSInfo* objectInfo = findDsInfo(connIdx, poolName, objectName);
        objectInfo && objectInfo->runtime.propertiesState == LoadState::Loaded
        && !objectInfo->runtime.propertyRows.isEmpty()) {
        return objectInfo->runtime.propertyRows;
    }
    return {};
}

void MainWindow::storeDatasetPropertyRows(int connIdx,
                                          const QString& poolName,
                                          const QString& objectName,
                                          const QString& datasetType,
                                          const QVector<DatasetPropCacheRow>& rows) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return;
    }

    if (DSInfo* objectInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        objectInfo->runtime.propertiesState = LoadState::Loaded;
        objectInfo->runtime.datasetType = datasetType.trimmed();
        objectInfo->runtime.propertyRows = rows;
        objectInfo->runtime.properties.clear();
        for (const DatasetPropCacheRow& row : rows) {
            objectInfo->runtime.properties.insert(row.prop.trimmed(), row.value);
        }
        objectInfo->kind = dsKindFromNames(trimmedObject, datasetType.trimmed());
    } else {
        rebuildConnInfoFor(connIdx);
        if (DSInfo* rebuilt = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
            rebuilt->runtime.propertiesState = LoadState::Loaded;
            rebuilt->runtime.datasetType = datasetType.trimmed();
            rebuilt->runtime.propertyRows = rows;
            rebuilt->runtime.properties.clear();
            for (const DatasetPropCacheRow& row : rows) {
                rebuilt->runtime.properties.insert(row.prop.trimmed(), row.value);
            }
            rebuilt->kind = dsKindFromNames(trimmedObject, datasetType.trimmed());
        }
    }
}

void MainWindow::removeDatasetPropertyEntry(int connIdx, const QString& poolName, const QString& objectName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return;
    }
    if (DSInfo* objectInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        objectInfo->runtime.propertiesState = LoadState::NotLoaded;
        objectInfo->runtime.propertyRows.clear();
        objectInfo->runtime.properties.clear();
    }
}

void MainWindow::removeDatasetPropertyEntriesForPool(int connIdx, const QString& poolName) {
    if (PoolInfo* poolInfo = findPoolInfo(connIdx, poolName)) {
        for (auto itDs = poolInfo->objectsByFullName.begin(); itDs != poolInfo->objectsByFullName.end(); ++itDs) {
            itDs->runtime.propertiesState = LoadState::NotLoaded;
            itDs->runtime.propertyRows.clear();
            itDs->runtime.properties.clear();
        }
    }
}

MainWindow::DatasetPropsDraft MainWindow::propertyDraftForObject(const QString& side,
                                                                const QString& token,
                                                                const QString& objectName) const {
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return {};
    }
    bool okConn = false;
    const int connIdx = token.left(sep).toInt(&okConn);
    const QString poolName = token.mid(sep + 2).trimmed();
    if (!okConn || connIdx < 0 || poolName.isEmpty()) {
        return {};
    }
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, objectName);
    if (!dsInfo) {
        return {};
    }

    DatasetPropsDraft draft;
    for (auto itProp = dsInfo->editSession.propertyEdits.byName.cbegin();
         itProp != dsInfo->editSession.propertyEdits.byName.cend();
         ++itProp) {
        if (!itProp->dirty) {
            continue;
        }
        draft.valuesByProp.insert(itProp.key(), itProp->value);
        draft.inheritByProp.insert(itProp.key(), itProp->inherit);
    }
    draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();
    return draft;
}

void MainWindow::storePropertyDraftForObject(const QString& side,
                                            const QString& token,
                                            const QString& objectName,
                                            const DatasetPropsDraft& draftIn) {
    const QString normSide = side.trimmed().toLower();
    const QString normToken = token.trimmed();
    const QString normObject = objectName.trimmed();
    if (normSide.isEmpty() || normToken.isEmpty() || normObject.isEmpty()) {
        return;
    }

    DatasetPropsDraft draft = draftIn;
    draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();
    const int sep = normToken.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    bool okConn = false;
    const int connIdx = normToken.left(sep).toInt(&okConn);
    const QString poolName = normToken.mid(sep + 2).trimmed();
    if (!okConn || connIdx < 0 || poolName.isEmpty()) {
        return;
    }
    DSInfo* dsInfo = findDsInfo(connIdx, poolName, normObject);
    if (!dsInfo) {
        return;
    }

    dsInfo->editSession.target = dsInfo->key;
    dsInfo->editSession.propertyEdits.clear();
    for (auto itProp = draft.valuesByProp.cbegin(); itProp != draft.valuesByProp.cend(); ++itProp) {
        DSPropertyEditValue value;
        value.value = itProp.value();
        value.inherit = draft.inheritByProp.value(itProp.key(), false);
        value.dirty = true;
        dsInfo->editSession.propertyEdits.byName.insert(itProp.key(), value);
    }
    for (auto itInh = draft.inheritByProp.cbegin(); itInh != draft.inheritByProp.cend(); ++itInh) {
        auto existing = dsInfo->editSession.propertyEdits.byName.find(itInh.key());
        if (existing == dsInfo->editSession.propertyEdits.byName.end()) {
            DSPropertyEditValue value;
            value.inherit = itInh.value();
            value.dirty = true;
            dsInfo->editSession.propertyEdits.byName.insert(itInh.key(), value);
        } else {
            existing->inherit = itInh.value();
            existing->dirty = true;
        }
    }
}

QVector<MainWindow::PendingPropertyDraftEntry> MainWindow::pendingConnContentPropertyDraftsFromModel() const {
    QVector<PendingPropertyDraftEntry> drafts;
    for (auto itConn = m_connInfoById.cbegin(); itConn != m_connInfoById.cend(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.cbegin(); itPool != itConn->poolsByStableId.cend(); ++itPool) {
            const QString poolName = itPool->key.poolName.trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            const QString token = QStringLiteral("%1::%2").arg(itConn->connIdx).arg(poolName);
            for (auto itDs = itPool->objectsByFullName.cbegin(); itDs != itPool->objectsByFullName.cend(); ++itDs) {
                const DatasetPropsDraft draft =
                    propertyDraftForObject(QStringLiteral("conncontent"), token, itDs.key());
                if (!draft.dirty) {
                    continue;
                }
                PendingPropertyDraftEntry entry;
                entry.connIdx = itConn->connIdx;
                entry.poolName = poolName;
                entry.token = token;
                entry.objectName = itDs.key();
                entry.draft = draft;
                drafts.push_back(entry);
            }
        }
    }
    return drafts;
}

const MainWindow::DatasetPermissionsCacheEntry* MainWindow::datasetPermissionsEntry(int connIdx,
                                                                                   const QString& poolName,
                                                                                   const QString& datasetName) const {
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (dsInfo && dsInfo->runtime.permissionsState == LoadState::Loaded && dsInfo->permissionsCache.loaded) {
        return &dsInfo->permissionsCache;
    }
    const QString key = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
    const auto it = m_datasetPermissionsCache.constFind(key);
    return (it == m_datasetPermissionsCache.cend()) ? nullptr : &it.value();
}

MainWindow::DatasetPermissionsCacheEntry* MainWindow::datasetPermissionsEntryMutable(int connIdx,
                                                                                    const QString& poolName,
                                                                                    const QString& datasetName) {
    const QString key = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
    auto it = m_datasetPermissionsCache.find(key);
    if (it != m_datasetPermissionsCache.end()) {
        return &it.value();
    }
    DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (dsInfo && dsInfo->runtime.permissionsState == LoadState::Loaded && dsInfo->permissionsCache.loaded) {
        m_datasetPermissionsCache.insert(key, dsInfo->permissionsCache);
        auto inserted = m_datasetPermissionsCache.find(key);
        return (inserted == m_datasetPermissionsCache.end()) ? nullptr : &inserted.value();
    }
    return nullptr;
}

void MainWindow::mirrorDatasetPermissionsEntryToModel(int connIdx, const QString& poolName, const QString& datasetName) {
    const QString key = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
    const auto it = m_datasetPermissionsCache.constFind(key);
    DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return;
    }
    if (it == m_datasetPermissionsCache.cend()) {
        dsInfo->permissionsCache = DatasetPermissionsCacheEntry{};
        dsInfo->runtime.permissionsState = LoadState::NotLoaded;
        return;
    }
    dsInfo->permissionsCache = it.value();
    dsInfo->runtime.permissionsState = it->loaded ? LoadState::Loaded : LoadState::NotLoaded;
}

QVector<MainWindow::PendingPermissionDraftEntry> MainWindow::dirtyDatasetPermissionsEntriesFromModel() const {
    QVector<PendingPermissionDraftEntry> entries;
    for (auto itConn = m_connInfoById.cbegin(); itConn != m_connInfoById.cend(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.cbegin(); itPool != itConn->poolsByStableId.cend(); ++itPool) {
            const QString poolName = itPool->key.poolName.trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            for (auto itDs = itPool->objectsByFullName.cbegin(); itDs != itPool->objectsByFullName.cend(); ++itDs) {
                if (!itDs->permissionsCache.loaded || !itDs->permissionsCache.dirty) {
                    continue;
                }
                PendingPermissionDraftEntry entry;
                entry.connIdx = itConn->connIdx;
                entry.poolName = poolName;
                entry.datasetName = itDs.key();
                entry.entry = itDs->permissionsCache;
                entries.push_back(entry);
            }
        }
    }
    return entries;
}

void MainWindow::removeDatasetPermissionsEntry(int connIdx, const QString& poolName, const QString& datasetName) {
    m_datasetPermissionsCache.remove(datasetPermissionsCacheKey(connIdx, poolName, datasetName));
    mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
}

void MainWindow::removeDatasetPermissionsEntriesForPool(int connIdx, const QString& poolName) {
    const QString prefix = QStringLiteral("%1::%2::").arg(connIdx).arg(poolName.trimmed().toLower());
    for (auto it = m_datasetPermissionsCache.begin(); it != m_datasetPermissionsCache.end();) {
        if (it.key().startsWith(prefix)) {
            it = m_datasetPermissionsCache.erase(it);
        } else {
            ++it;
        }
    }
    if (ConnInfo* connInfo = findConnInfo(connIdx)) {
        for (auto itPool = connInfo->poolsByStableId.begin(); itPool != connInfo->poolsByStableId.end(); ++itPool) {
            if (itPool->key.poolName.trimmed() != poolName.trimmed()) {
                continue;
            }
            for (auto itDs = itPool->objectsByFullName.begin(); itDs != itPool->objectsByFullName.end(); ++itDs) {
                itDs->permissionsCache = DatasetPermissionsCacheEntry{};
                itDs->runtime.permissionsState = LoadState::NotLoaded;
            }
            break;
        }
    }
}

void MainWindow::resetAllDatasetPermissionDrafts() {
    for (auto it = m_datasetPermissionsCache.begin(); it != m_datasetPermissionsCache.end(); ++it) {
        if (!it.value().loaded) {
            continue;
        }
        it.value().dirty = false;
        it.value().localGrants = it.value().originalLocalGrants;
        it.value().descendantGrants = it.value().originalDescendantGrants;
        it.value().localDescendantGrants = it.value().originalLocalDescendantGrants;
        it.value().createPermissions = it.value().originalCreatePermissions;
        it.value().permissionSets = it.value().originalPermissionSets;
    }
    for (auto itConn = m_connInfoById.begin(); itConn != m_connInfoById.end(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.begin(); itPool != itConn->poolsByStableId.end(); ++itPool) {
            for (auto itDs = itPool->objectsByFullName.begin(); itDs != itPool->objectsByFullName.end(); ++itDs) {
                if (!itDs->permissionsCache.loaded) {
                    continue;
                }
                itDs->permissionsCache.dirty = false;
                itDs->permissionsCache.localGrants = itDs->permissionsCache.originalLocalGrants;
                itDs->permissionsCache.descendantGrants = itDs->permissionsCache.originalDescendantGrants;
                itDs->permissionsCache.localDescendantGrants = itDs->permissionsCache.originalLocalDescendantGrants;
                itDs->permissionsCache.createPermissions = itDs->permissionsCache.originalCreatePermissions;
                itDs->permissionsCache.permissionSets = itDs->permissionsCache.originalPermissionSets;
            }
        }
    }
}

bool MainWindow::selectDatasetForTest(const QString& datasetName, bool bottom) {
    QTreeWidget* tree = bottom ? m_bottomConnContentTree : m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty()) {
        return false;
    }
    const QString wanted = datasetName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wanted) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !item; ++i) {
        item = rec(tree->topLevelItem(i));
    }
    if (!item) {
        return false;
    }
    QTreeWidget* prevTree = m_connContentTree;
    const QString prevToken = m_connContentToken;
    if (bottom) {
        const int bIdx = m_bottomConnectionEntityTabs ? m_bottomConnectionEntityTabs->currentIndex() : -1;
        if (bIdx >= 0 && m_bottomConnectionEntityTabs && bIdx < m_bottomConnectionEntityTabs->count()) {
            const QString key = m_bottomConnectionEntityTabs->tabData(bIdx).toString();
            const QStringList parts = key.split(':');
            if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
                m_connContentToken = QStringLiteral("%1::%2").arg(parts.value(1)).arg(parts.value(2).trimmed());
            }
        }
        m_connContentTree = tree;
    }
    tree->setCurrentItem(item);
    refreshDatasetProperties(QStringLiteral("conncontent"));
    m_connContentTree = prevTree;
    m_connContentToken = prevToken;
    return true;
}

bool MainWindow::setDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool expanded, bool bottom) {
    QTreeWidget* tree = bottom ? m_bottomConnContentTree : m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty() || childLabel.trimmed().isEmpty()) {
        return false;
    }
    const QString wantedDataset = datasetName.trimmed();
    const QString wantedChild = childLabel.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> recDataset = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wantedDataset) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = recDataset(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* datasetItem = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !datasetItem; ++i) {
        datasetItem = recDataset(tree->topLevelItem(i));
    }
    if (!datasetItem) {
        return false;
    }
    for (int i = 0; i < datasetItem->childCount(); ++i) {
        QTreeWidgetItem* child = datasetItem->child(i);
        if (child && child->text(0).trimmed() == wantedChild) {
            child->setExpanded(expanded);
            return true;
        }
    }
    return false;
}

bool MainWindow::isDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool bottom) const {
    QTreeWidget* tree = bottom ? m_bottomConnContentTree : m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty() || childLabel.trimmed().isEmpty()) {
        return false;
    }
    const QString wantedDataset = datasetName.trimmed();
    const QString wantedChild = childLabel.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> recDataset = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wantedDataset) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = recDataset(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* datasetItem = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !datasetItem; ++i) {
        datasetItem = recDataset(tree->topLevelItem(i));
    }
    if (!datasetItem) {
        return false;
    }
    for (int i = 0; i < datasetItem->childCount(); ++i) {
        QTreeWidgetItem* child = datasetItem->child(i);
        if (child && child->text(0).trimmed() == wantedChild) {
            return child->isExpanded();
        }
    }
    return false;
}

void MainWindow::rebuildConnContentTreeForTest(const QString& datasetToSelect, bool bottom) {
    QTreeWidget* tree = bottom ? m_bottomConnContentTree : m_connContentTree;
    if (!tree) {
        return;
    }
    const QString token = [&]() -> QString {
        if (!bottom) {
            return m_connContentToken;
        }
        const int bIdx = m_bottomConnectionEntityTabs ? m_bottomConnectionEntityTabs->currentIndex() : -1;
        if (bIdx < 0 || !m_bottomConnectionEntityTabs || bIdx >= m_bottomConnectionEntityTabs->count()) {
            return QString();
        }
        const QString key = m_bottomConnectionEntityTabs->tabData(bIdx).toString();
        const QStringList parts = key.split(':');
        if (parts.size() < 3 || parts.first() != QStringLiteral("pool")) {
            return QString();
        }
        return QStringLiteral("%1::%2").arg(parts.value(1)).arg(parts.value(2).trimmed());
    }();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    bool okConn = false;
    const int connIdx = token.left(sep).toInt(&okConn);
    const QString poolName = token.mid(sep + 2).trimmed();
    if (!okConn || connIdx < 0 || poolName.isEmpty()) {
        return;
    }
    QTreeWidget* prevTree = m_connContentTree;
    const QString prevToken = m_connContentToken;
    m_connContentTree = tree;
    m_connContentToken = token;
    saveConnContentTreeState(token);
    populateDatasetTree(tree, connIdx, poolName, DatasetTreeContext::ConnectionContent, true);
    if (!datasetToSelect.trimmed().isEmpty()) {
        selectDatasetForTest(datasetToSelect, bottom);
    }
    m_connContentTree = prevTree;
    m_connContentToken = prevToken;
}

QStringList MainWindow::topLevelPoolNamesForTest(bool bottom) const {
    QStringList names;
    QTreeWidget* tree = bottom ? m_bottomConnContentTree : m_connContentTree;
    if (!tree) {
        return names;
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* item = tree->topLevelItem(i)) {
            names.push_back(item->text(0).trimmed());
        }
    }
    return names;
}

QStringList MainWindow::childLabelsForDatasetForTest(const QString& datasetName, bool bottom) const {
    QStringList labels;
    QTreeWidget* tree = bottom ? m_bottomConnContentTree : m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty()) {
        return labels;
    }
    const QString wanted = datasetName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wanted) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !item; ++i) {
        item = rec(tree->topLevelItem(i));
    }
    if (!item) {
        return labels;
    }
    for (int i = 0; i < item->childCount(); ++i) {
        if (QTreeWidgetItem* child = item->child(i)) {
            labels.push_back(child->text(0).trimmed());
        }
    }
    return labels;
}

QStringList MainWindow::snapshotNamesForDatasetForTest(const QString& datasetName, bool bottom) const {
    QStringList names;
    QTreeWidget* tree = bottom ? m_bottomConnContentTree : m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty()) {
        return names;
    }
    const QString wanted = datasetName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wanted) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !item; ++i) {
        item = rec(tree->topLevelItem(i));
    }
    if (!item) {
        return names;
    }
    constexpr int kSnapshotListRole = Qt::UserRole + 1;
    names = item->data(1, kSnapshotListRole).toStringList();
    return names;
}

QStringList MainWindow::connectionContextMenuTopLevelLabelsForTest() const {
    const int connIdx = (m_connectionsTable && m_connectionsTable->currentRow() >= 0) ? m_topDetailConnIdx : -1;
    const bool hasConn = (connIdx >= 0 && connIdx < m_profiles.size());
    const bool isDisconnected = hasConn && isConnectionDisconnected(connIdx);
    const bool hasWindowsUnixLayerReady =
        hasConn
        && connIdx < m_states.size()
        && isWindowsConnection(connIdx)
        && m_states[connIdx].unixFromMsysOrMingw
        && m_states[connIdx].missingUnixCommands.isEmpty()
        && !m_states[connIdx].detectedUnixCommands.isEmpty();
    const bool canManageGsa =
        hasConn && !actionsLocked() && !isDisconnected
        && connIdx < m_states.size()
        && gsaMenuLabelForConnection(connIdx).compare(
               trk(QStringLiteral("t_gsa_ok_001"),
                   QStringLiteral("GSA actualizado y funcionando"),
                   QStringLiteral("GSA updated and running"),
                   QStringLiteral("GSA 已更新并运行中")),
               Qt::CaseInsensitive) != 0;
    const bool canUninstallGsa =
        hasConn && !actionsLocked() && !isDisconnected
        && connIdx < m_states.size()
        && m_states[connIdx].gsaInstalled;
    const zfsmgr::uilogic::ConnectionContextMenuState menuState =
        zfsmgr::uilogic::buildConnectionContextMenuState(
            hasConn,
            isDisconnected,
            actionsLocked(),
            hasConn && isLocalConnection(connIdx),
            hasConn && isConnectionRedirectedToLocal(connIdx),
            hasConn && isWindowsConnection(connIdx),
            hasWindowsUnixLayerReady,
            canManageGsa,
            canUninstallGsa);
    Q_UNUSED(menuState);
    return {
        trk(QStringLiteral("t_connect_ctx_001"),
            QStringLiteral("Conectar"),
            QStringLiteral("Connect"),
            QStringLiteral("连接")),
        trk(QStringLiteral("t_disconnect_ctx001"),
            QStringLiteral("Desconectar"),
            QStringLiteral("Disconnect"),
            QStringLiteral("断开连接")),
        trk(QStringLiteral("t_install_msys_ctx001"),
            QStringLiteral("Instalar MSYS2"),
            QStringLiteral("Install MSYS2"),
            QStringLiteral("安装 MSYS2")),
        trk(QStringLiteral("t_install_helpers_ctx001"),
            QStringLiteral("Instalar comandos auxiliares"),
            QStringLiteral("Install helper commands"),
            QStringLiteral("安装辅助命令")),
        trk(QStringLiteral("t_refresh_conn_ctx001"),
            QStringLiteral("Refrescar"),
            QStringLiteral("Refresh"),
            QStringLiteral("刷新")),
        QStringLiteral("GSA"),
        trk(QStringLiteral("t_edit_conn_ctx001"),
            QStringLiteral("Editar"),
            QStringLiteral("Edit"),
            QStringLiteral("编辑")),
        trk(QStringLiteral("t_del_conn_ctx001"),
            QStringLiteral("Borrar"),
            QStringLiteral("Delete"),
            QStringLiteral("删除")),
        trk(QStringLiteral("t_new_conn_ctx001"),
            QStringLiteral("Nueva Conexión"),
            QStringLiteral("New Connection"),
            QStringLiteral("新建连接")),
        trk(QStringLiteral("t_new_pool_ctx_001"),
            QStringLiteral("Nuevo Pool"),
            QStringLiteral("New Pool"),
            QStringLiteral("新建存储池")),
    };
}

QStringList MainWindow::connectionRefreshMenuLabelsForTest() const {
    return {
        trk(QStringLiteral("t_refresh_this_conn_001"),
            QStringLiteral("Esta conexión"),
            QStringLiteral("This connection"),
            QStringLiteral("此连接")),
        trk(QStringLiteral("t_refresh_all_001"),
            QStringLiteral("Todas las conexiones"),
            QStringLiteral("All connections"),
            QStringLiteral("所有连接")),
    };
}

QStringList MainWindow::connectionGsaMenuLabelsForTest() const {
    const int connIdx = m_topDetailConnIdx;
    const bool hasConn = (connIdx >= 0 && connIdx < m_profiles.size());
    const QString manageLabel = hasConn ? gsaMenuLabelForConnection(connIdx)
                                        : trk(QStringLiteral("t_gsa_install_001"),
                                              QStringLiteral("Instalar gestor de snapshots"),
                                              QStringLiteral("Install snapshot manager"),
                                              QStringLiteral("安装快照管理器"));
    return {
        manageLabel,
        trk(QStringLiteral("t_gsa_uninstall_001"),
            QStringLiteral("Desinstalar el GSA"),
            QStringLiteral("Uninstall GSA"),
            QStringLiteral("卸载 GSA")),
    };
}

QStringList MainWindow::poolContextMenuLabelsForTest(const QString& poolName, bool bottom) const {
    const int connIdx = bottom ? m_bottomDetailConnIdx : m_topDetailConnIdx;
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size() || poolName.trimmed().isEmpty()) {
        return {};
    }
    QString poolAction;
    const ConnectionRuntimeState& st = m_states[connIdx];
    for (const PoolImported& pool : st.importedPools) {
        if (pool.pool.trimmed().compare(poolName.trimmed(), Qt::CaseInsensitive) == 0) {
            poolAction = QStringLiteral("Exportar");
            break;
        }
    }
    if (poolAction.isEmpty()) {
        for (const PoolImportable& pool : st.importablePools) {
            if (pool.pool.trimmed().compare(poolName.trimmed(), Qt::CaseInsensitive) == 0) {
                poolAction = pool.action.trimmed();
                break;
            }
        }
    }
    const zfsmgr::uilogic::PoolRootMenuState menuState =
        zfsmgr::uilogic::buildPoolRootMenuState(poolAction, QStringLiteral("ONLINE"), true);
    Q_UNUSED(menuState);
    return {
        trk(QStringLiteral("t_refresh_btn001"),
            QStringLiteral("Actualizar"),
            QStringLiteral("Refresh"),
            QStringLiteral("刷新")),
        trk(QStringLiteral("t_import_btn001"),
            QStringLiteral("Importar"),
            QStringLiteral("Import"),
            QStringLiteral("导入")),
        QStringLiteral("Importar renombrando"),
        trk(QStringLiteral("t_export_btn001"),
            QStringLiteral("Exportar"),
            QStringLiteral("Export"),
            QStringLiteral("导出")),
        trk(QStringLiteral("t_pool_history_t1"),
            QStringLiteral("Historial")),
        QStringLiteral("Sync"),
        QStringLiteral("Scrub"),
        QStringLiteral("Reguid"),
        QStringLiteral("Trim"),
        QStringLiteral("Initialize"),
        QStringLiteral("Destroy"),
        QStringLiteral("Mostrar Información del pool"),
    };
}
