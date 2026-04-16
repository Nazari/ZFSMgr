#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "mainwindow_ui_logic.h"
#include "agentversion.h"

#include <algorithm>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTableWidget>
#include <QTreeWidget>

#include <QtConcurrent/QtConcurrent>

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

QStringList gsaPropertyKeysForModel() {
    return {
        QStringLiteral("org.fc16.gsa:activado"),
        QStringLiteral("org.fc16.gsa:recursivo"),
        QStringLiteral("org.fc16.gsa:horario"),
        QStringLiteral("org.fc16.gsa:diario"),
        QStringLiteral("org.fc16.gsa:semanal"),
        QStringLiteral("org.fc16.gsa:mensual"),
        QStringLiteral("org.fc16.gsa:anual"),
        QStringLiteral("org.fc16.gsa:nivelar"),
        QStringLiteral("org.fc16.gsa:destino"),
    };
}

QString gsaComparableValue(const QString& propName, const QString& rawValue) {
    const QString prop = propName.trimmed();
    const QString value = rawValue.trimmed();
    if (!prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
        return rawValue;
    }
    if (prop.compare(QStringLiteral("org.fc16.gsa:destino"), Qt::CaseInsensitive) == 0) {
        return (value == QStringLiteral("-")) ? QString() : rawValue;
    }
    if (value.isEmpty() || value == QStringLiteral("-")) {
        if (prop.compare(QStringLiteral("org.fc16.gsa:horario"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:diario"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:semanal"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:mensual"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:anual"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("0");
        }
        return QStringLiteral("off");
    }
    return rawValue;
}

QString normalizedPropKey(const QString& propName) {
    return propName.trimmed().toLower();
}

bool mountedStateFromAnyText(const QString& value, bool* mountedOut) {
    const QString s = value.trimmed().toLower();
    if (s == QStringLiteral("montado")
        || s == QStringLiteral("mounted")
        || s == QStringLiteral("已挂载")
        || s == QStringLiteral("on")
        || s == QStringLiteral("yes")
        || s == QStringLiteral("true")
        || s == QStringLiteral("1")) {
        if (mountedOut) {
            *mountedOut = true;
        }
        return true;
    }
    if (s == QStringLiteral("desmontado")
        || s == QStringLiteral("unmounted")
        || s == QStringLiteral("未挂载")
        || s == QStringLiteral("off")
        || s == QStringLiteral("no")
        || s == QStringLiteral("false")
        || s == QStringLiteral("0")) {
        if (mountedOut) {
            *mountedOut = false;
        }
        return true;
    }
    return false;
}

struct PoolAutoSnapshotLoadResult {
    int connIdx{-1};
    QString poolName;
    bool ok{false};
    QString errorText;
    QMap<QString, QMap<QString, QString>> loaded;
};

bool isAllowedGenericZpoolMutationOpClient(const QString& opRaw) {
    const QString op = opRaw.trimmed().toLower();
    static const QSet<QString> allowed = {
        QStringLiteral("create"),
        QStringLiteral("destroy"),
        QStringLiteral("add"),
        QStringLiteral("remove"),
        QStringLiteral("attach"),
        QStringLiteral("detach"),
        QStringLiteral("replace"),
        QStringLiteral("offline"),
        QStringLiteral("online"),
        QStringLiteral("clear"),
        QStringLiteral("export"),
        QStringLiteral("import"),
        QStringLiteral("scrub"),
        QStringLiteral("trim"),
        QStringLiteral("initialize"),
        QStringLiteral("sync"),
        QStringLiteral("upgrade"),
        QStringLiteral("reguid"),
        QStringLiteral("split"),
        QStringLiteral("checkpoint"),
    };
    return allowed.contains(op);
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
        restoreSplitTreeLayoutFromState(m_splitTreeLayoutState);
        for (const ConnectionProfile& p : std::as_const(m_profiles)) {
            if (!isLocalConnection(p) || p.username.trimmed().isEmpty() || p.password.isEmpty()) {
                continue;
            }
            m_localSudoUsername = p.username.trimmed();
            m_localSudoPassword = p.password;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Credenciales sudo locales inyectadas en memoria al arrancar (connLocal)"));
            break;
        }
        ensureStartupLocalSudoConnection();
        refreshAllConnections();
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
    setCurrentConnectionInUi(0);
}

void MainWindow::rebuildConnectionDetailsForTest() {
    rebuildConnectionEntityTabs();
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
        record.guid.clear();
        record.mountpoint = seed.mountpoint.trimmed();
        record.canmount = seed.canmount.trimmed();
        record.mounted = seed.mounted.trimmed();
        if (record.name.isEmpty()) {
            continue;
        }
        cache.datasets.push_back(record);
        cache.recordByName.insert(record.name, record);
        cache.objectGuidByName.insert(record.name, QString());
        cache.snapshotsByDataset.insert(record.name, seed.snapshots);
    }
    m_poolDatasetCache.insert(datasetCacheKey(connIdx, poolName), cache);
    rebuildConnInfoFor(connIdx);
}

void MainWindow::setShowPoolInfoNodeForTest(bool visible) {
    m_showPoolInfoNodeTop = visible;
    if (m_topDatasetPane) {
        auto options = m_topDatasetPane->visualOptions();
        options.showPoolInfo = visible;
        m_topDatasetPane->setVisualOptions(options);
    }
    rebuildConnectionDetailsForTest();
}

void MainWindow::setShowInlineGsaNodeForTest(bool visible) {
    m_showInlineGsaNodeTop = visible;
    if (m_topDatasetPane) {
        auto options = m_topDatasetPane->visualOptions();
        options.showInlineGsa = visible;
        m_topDatasetPane->setVisualOptions(options);
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
    const QString machineUid = profile.machineUid.trimmed();
    if (!machineUid.isEmpty()) {
        return QStringLiteral("%1#%2").arg(machineUid, QString::number(connIdx));
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
        if (!record.guid.trimmed().isEmpty()) {
            dsInfo.runtime.properties.insert(QStringLiteral("guid"), record.guid.trimmed());
        } else {
            const QString guidFromCache = cache.objectGuidByName.value(fullName).trimmed();
            if (!guidFromCache.isEmpty()) {
                dsInfo.runtime.properties.insert(QStringLiteral("guid"), guidFromCache);
            }
        }
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
            const QString datasetGuidFromCache = cache.objectGuidByName.value(datasetName).trimmed();
            if (!datasetGuidFromCache.isEmpty() && datasetGuidFromCache != QStringLiteral("-")) {
                datasetInfo.runtime.properties.insert(QStringLiteral("guid"), datasetGuidFromCache);
            }
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
            const QString snapGuid = cache.objectGuidByName.value(fullSnapshotName).trimmed();
            if (!snapGuid.isEmpty()) {
                snapInfo.runtime.properties.insert(QStringLiteral("guid"), snapGuid);
            }
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
                it->runtime.loadedPropertyNames = prevDsIt->runtime.loadedPropertyNames;
                it->runtime.allPropertiesLoaded = prevDsIt->runtime.allPropertiesLoaded;
                it->runtime.holdsState = prevDsIt->runtime.holdsState;
                it->runtime.snapshotHolds = prevDsIt->runtime.snapshotHolds;
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

    if (cache.autoSnapshotPropsLoaded) {
        poolInfo.runtime.autoSnapshotPropsByDataset = cache.autoSnapshotPropsByDataset;
        poolInfo.runtime.schedulesState = LoadState::Loaded;
    } else if (previousPoolInfo) {
        poolInfo.runtime.autoSnapshotPropsByDataset = previousPoolInfo->runtime.autoSnapshotPropsByDataset;
        poolInfo.runtime.schedulesState = previousPoolInfo->runtime.schedulesState;
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
        const QString poolGuid = state.poolGuidByName.value(pool.pool.trimmed()).trimmed();
        PoolInfo& poolInfo = ensurePool(pool.pool, poolGuid);
        poolInfo.runtime.imported = true;
        poolInfo.runtime.importAction = pool.action;
        poolInfo.runtime.poolStatusText = state.poolStatusByName.value(pool.pool.trimmed());
        if (!poolInfo.runtime.poolStatusText.trimmed().isEmpty()) {
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
            poolInfo.runtime.detailsState =
                (!poolDetailsIt->propsRows.isEmpty() || !poolDetailsIt->statusText.trimmed().isEmpty())
                    ? LoadState::Loaded
                    : LoadState::NotLoaded;
            poolInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
            poolInfo.runtime.poolStatusText = poolDetailsIt->statusText;
            poolInfo.runtime.zpoolPropertyRows = poolDetailsIt->propsRows;
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
                poolInfo.runtime.schedulesState = previousPoolInfo->runtime.schedulesState;
                poolInfo.runtime.autoSnapshotPropsByDataset = previousPoolInfo->runtime.autoSnapshotPropsByDataset;
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

QVector<MainWindow::DatasetPropCacheRow> MainWindow::datasetPropertyRowsForNames(int connIdx,
                                                                                 const QString& poolName,
                                                                                 const QString& objectName,
                                                                                 const QStringList& propNames) const {
    const QVector<DatasetPropCacheRow> rows = datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
    if (propNames.isEmpty() || rows.isEmpty()) {
        return rows;
    }
    QSet<QString> wanted;
    for (const QString& propName : propNames) {
        const QString key = normalizedPropKey(propName);
        if (!key.isEmpty()) {
            wanted.insert(key);
        }
    }
    QVector<DatasetPropCacheRow> filtered;
    for (const DatasetPropCacheRow& row : rows) {
        if (wanted.contains(normalizedPropKey(row.prop))) {
            filtered.push_back(row);
        }
    }
    return filtered;
}

QMap<QString, QString> MainWindow::datasetPropertyValuesForNames(int connIdx,
                                                                 const QString& poolName,
                                                                 const QString& objectName,
                                                                 const QStringList& propNames) const {
    QMap<QString, QString> values;
    for (const DatasetPropCacheRow& row : datasetPropertyRowsForNames(connIdx, poolName, objectName, propNames)) {
        values.insert(row.prop, row.value);
    }
    return values;
}

QMap<QString, QString> MainWindow::datasetGsaPropertyValues(int connIdx,
                                                            const QString& poolName,
                                                            const QString& objectName) const {
    return datasetPropertyValuesForNames(connIdx, poolName, objectName, gsaPropertyKeysForModel());
}

bool MainWindow::ensureDatasetAllPropertiesLoaded(int connIdx,
                                                  const QString& poolName,
                                                  const QString& objectName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return false;
    }
    DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (dsInfo && dsInfo->runtime.propertiesState == LoadState::Loaded && dsInfo->runtime.allPropertiesLoaded) {
        return true;
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    const bool daemonReadApiOk =
        !isWindowsConnection(connIdx)
        && connIdx >= 0
        && connIdx < m_states.size()
        && m_states[connIdx].daemonInstalled
        && m_states[connIdx].daemonActive
        && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    const bool useRemoteScript = !isWindowsConnection(connIdx);
    QString datasetType = trimmedObject.contains(QLatin1Char('@')) ? QStringLiteral("snapshot") : QStringLiteral("filesystem");
    if (dsInfo && !dsInfo->runtime.datasetType.trimmed().isEmpty()) {
        datasetType = dsInfo->runtime.datasetType.trimmed();
    }

    if (datasetType.trimmed().isEmpty()) {
        QString tOut, tErr;
        int tRc = -1;
        const QString typeCmdClassic = withSudo(
            p,
            useRemoteScript
                ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-get-type"), {trimmedObject})
                : mwhelpers::withUnixSearchPathCommand(
                      QStringLiteral("zfs get -H -o value type %1").arg(mwhelpers::shSingleQuote(trimmedObject))));
        const QString typeCmdDaemon = withSudo(
            p, mwhelpers::withUnixSearchPathCommand(
                   QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-get-prop type %1")
                       .arg(mwhelpers::shSingleQuote(trimmedObject))));
        bool typeOk = runSsh(p, (daemonReadApiOk ? typeCmdDaemon : typeCmdClassic), 12000, tOut, tErr, tRc) && tRc == 0;
        if (!typeOk && daemonReadApiOk) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("daemon zfs-get-type fallback %1::%2/%3 -> %4")
                       .arg(p.name, trimmedPool, trimmedObject, mwhelpers::oneLine(tErr.isEmpty() ? tOut : tErr)));
            tOut.clear();
            tErr.clear();
            tRc = -1;
            typeOk = runSsh(p, typeCmdClassic, 12000, tOut, tErr, tRc) && tRc == 0;
        }
        if (typeOk) {
            const QString t = tOut.trimmed().toLower();
            if (!t.isEmpty()) {
                datasetType = t;
            }
        }
    }

    QString out;
    QString err;
    int rc = -1;
    const bool dsWin = isWindowsConnection(connIdx);
    const QString propsCmdClassic = withSudo(
        p,
        (useRemoteScript && !dsWin)
            ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-get-all-json"), {trimmedObject})
            : mwhelpers::withUnixSearchPathCommand(
                  dsWin
                      ? QStringLiteral("zfs get -H -o property,value,source all %1").arg(mwhelpers::shSingleQuote(trimmedObject))
                      : QStringLiteral("zfs get -j all %1").arg(mwhelpers::shSingleQuote(trimmedObject))));
    const QString propsCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(
               QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-get-all %1")
                   .arg(mwhelpers::shSingleQuote(trimmedObject))));
    bool propsOk = runSsh(p, (daemonReadApiOk ? propsCmdDaemon : propsCmdClassic), 20000, out, err, rc) && rc == 0;
    if (!propsOk && daemonReadApiOk) {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("daemon zfs-get-all fallback %1::%2/%3 -> %4")
                   .arg(p.name, trimmedPool, trimmedObject, mwhelpers::oneLine(err.isEmpty() ? out : err)));
        out.clear();
        err.clear();
        rc = -1;
        propsOk = runSsh(p, propsCmdClassic, 20000, out, err, rc) && rc == 0;
    }
    if (!propsOk) {
        if (dsInfo) {
            dsInfo->runtime.propertiesState = LoadState::Error;
            dsInfo->runtime.errorText = err.trimmed();
        }
        return false;
    }

    QVector<DatasetPropCacheRow> rows;
    rows.push_back(DatasetPropCacheRow{QStringLiteral("dataset"), trimmedObject, QString(), QStringLiteral("true")});
    if (dsWin) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            QString prop, val, source, ro;
            const QStringList parts = raw.split('\t');
            if (parts.size() >= 4) {
                prop = parts[0].trimmed(); val = parts[1].trimmed();
                source = parts[2].trimmed(); ro = parts[3].trimmed().toLower();
            } else if (parts.size() >= 3) {
                prop = parts[0].trimmed(); val = parts[1].trimmed();
                source = parts[2].trimmed(); ro.clear();
            } else {
                const QStringList sp = raw.simplified().split(' ');
                if (sp.size() < 3) { continue; }
                prop = sp[0].trimmed(); val = sp[1].trimmed(); source = sp[2].trimmed();
                ro = (sp.size() > 3) ? sp[3].trimmed().toLower() : QString();
            }
            rows.push_back(DatasetPropCacheRow{prop, val, source, ro});
        }
    } else {
        const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
        const QJsonObject datasets = doc.object().value(QStringLiteral("datasets")).toObject();
        const QJsonObject dsObj = datasets.value(trimmedObject).toObject();
        const QJsonObject properties = dsObj.value(QStringLiteral("properties")).toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            const QJsonObject propObj = it.value().toObject();
            const QString val = propObj.value(QStringLiteral("value")).toString().trimmed();
            const QJsonObject sourceObj = propObj.value(QStringLiteral("source")).toObject();
            const QString source = sourceObj.value(QStringLiteral("type")).toString().trimmed();
            rows.push_back(DatasetPropCacheRow{it.key(), val, source, QString()});
        }
    }

    storeDatasetPropertyRows(connIdx, trimmedPool, trimmedObject, datasetType, rows);
    if (DSInfo* refreshed = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        refreshed->runtime.propertiesState = LoadState::Loaded;
        refreshed->runtime.loadedAt = QDateTime::currentDateTimeUtc();
        refreshed->runtime.errorText.clear();
        refreshed->runtime.allPropertiesLoaded = true;
        refreshed->runtime.loadedPropertyNames.clear();
        for (const DatasetPropCacheRow& row : refreshed->runtime.propertyRows) {
            refreshed->runtime.loadedPropertyNames.insert(normalizedPropKey(row.prop));
        }
    }
    return true;
}

bool MainWindow::ensureDatasetPropertySubsetLoaded(int connIdx,
                                                   const QString& poolName,
                                                   const QString& objectName,
                                                   const QStringList& propNames) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return false;
    }
    if (propNames.isEmpty()) {
        return ensureDatasetAllPropertiesLoaded(connIdx, trimmedPool, trimmedObject);
    }
    DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (!dsInfo) {
        return false;
    }
    if (dsInfo->runtime.allPropertiesLoaded) {
        return true;
    }

    QStringList wantedProps;
    QSet<QString> missingKeys;
    for (const QString& propName : propNames) {
        const QString key = normalizedPropKey(propName);
        if (key.isEmpty()) {
            continue;
        }
        wantedProps.push_back(propName.trimmed());
        if (!dsInfo->runtime.loadedPropertyNames.contains(key)) {
            missingKeys.insert(key);
        }
    }
    if (missingKeys.isEmpty()) {
        return true;
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    const bool daemonReadApiOk =
        !isWindowsConnection(connIdx)
        && connIdx >= 0
        && connIdx < m_states.size()
        && m_states[connIdx].daemonInstalled
        && m_states[connIdx].daemonActive
        && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    QString datasetType = dsInfo->runtime.datasetType.trimmed();
    if (datasetType.isEmpty()) {
        datasetType = trimmedObject.contains(QLatin1Char('@')) ? QStringLiteral("snapshot")
                                                               : QStringLiteral("filesystem");
    }
    QString out;
    QString err;
    int rc = -1;
    const bool subWin = isWindowsConnection(connIdx);
    QStringList quotedProps;
    for (const QString& propName : wantedProps) {
        quotedProps.push_back(mwhelpers::shSingleQuote(propName.trimmed()));
    }
    const bool useRemoteScript = !subWin;
    QString propsCmdClassic;
    if (subWin) {
        propsCmdClassic = withSudo(
            p,
            mwhelpers::withUnixSearchPathCommand(
                QStringLiteral("zfs get -H -o property,value,source %1 %2")
                    .arg(quotedProps.join(QLatin1Char(',')),
                         mwhelpers::shSingleQuote(trimmedObject))));
    } else if (useRemoteScript) {
        propsCmdClassic = withSudo(
            p,
            remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-get-json"),
                                {wantedProps.join(QLatin1Char(',')), trimmedObject}));
    } else {
        propsCmdClassic = withSudo(
            p,
            mwhelpers::withUnixSearchPathCommand(
                QStringLiteral("zfs get -j %1 %2")
                    .arg(wantedProps.join(QLatin1Char(',')),
                         mwhelpers::shSingleQuote(trimmedObject))));
    }
    const QString propsCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(
               QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-get-json %1 %2")
                   .arg(mwhelpers::shSingleQuote(wantedProps.join(QLatin1Char(','))),
                        mwhelpers::shSingleQuote(trimmedObject))));
    bool propsOk = runSsh(p, (daemonReadApiOk ? propsCmdDaemon : propsCmdClassic), 20000, out, err, rc) && rc == 0;
    if (!propsOk && daemonReadApiOk) {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("daemon zfs-get-json fallback %1::%2/%3 -> %4")
                   .arg(p.name, trimmedPool, trimmedObject, mwhelpers::oneLine(err.isEmpty() ? out : err)));
        out.clear();
        err.clear();
        rc = -1;
        propsOk = runSsh(p, propsCmdClassic, 20000, out, err, rc) && rc == 0;
    }
    if (!propsOk) {
        dsInfo->runtime.propertiesState = LoadState::Error;
        dsInfo->runtime.errorText = err.trimmed();
        return false;
    }

    QMap<QString, DatasetPropCacheRow> mergedByKey;
    for (const DatasetPropCacheRow& row : dsInfo->runtime.propertyRows) {
        mergedByKey.insert(normalizedPropKey(row.prop), row);
    }
    if (subWin) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            QString prop, val, source;
            const QStringList parts = raw.split('\t');
            if (parts.size() >= 3) {
                prop = parts[0].trimmed(); val = parts[1].trimmed(); source = parts[2].trimmed();
            } else {
                const QStringList sp = raw.simplified().split(' ');
                if (sp.size() < 3) { continue; }
                prop = sp[0].trimmed(); val = sp[1].trimmed();
                source = sp.mid(2).join(QStringLiteral(" ")).trimmed();
            }
            if (prop.isEmpty()) {
                continue;
            }
            val = gsaComparableValue(prop, val);
            if (source == QStringLiteral("-")) {
                source.clear();
            }
            DatasetPropCacheRow row{prop, val, source, QString()};
            mergedByKey.insert(normalizedPropKey(prop), row);
            dsInfo->runtime.properties.insert(prop, val);
            dsInfo->runtime.loadedPropertyNames.insert(normalizedPropKey(prop));
        }
    } else {
        const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
        const QJsonObject datasets = doc.object().value(QStringLiteral("datasets")).toObject();
        const QJsonObject dsObj = datasets.value(trimmedObject).toObject();
        const QJsonObject properties = dsObj.value(QStringLiteral("properties")).toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            const QJsonObject propObj = it.value().toObject();
            QString val = propObj.value(QStringLiteral("value")).toString().trimmed();
            const QJsonObject sourceObj = propObj.value(QStringLiteral("source")).toObject();
            QString source = sourceObj.value(QStringLiteral("type")).toString().trimmed();
            val = gsaComparableValue(it.key(), val);
            if (source == QStringLiteral("-")) { source.clear(); }
            DatasetPropCacheRow newRow{it.key(), val, source, QString()};
            mergedByKey.insert(normalizedPropKey(it.key()), newRow);
            dsInfo->runtime.properties.insert(it.key(), val);
            dsInfo->runtime.loadedPropertyNames.insert(normalizedPropKey(it.key()));
        }
    }

    QVector<DatasetPropCacheRow> mergedRows;
    mergedRows.reserve(mergedByKey.size());
    for (auto it = mergedByKey.cbegin(); it != mergedByKey.cend(); ++it) {
        mergedRows.push_back(it.value());
    }
    dsInfo->runtime.propertyRows = mergedRows;
    dsInfo->runtime.propertiesState = LoadState::Loaded;
    dsInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
    dsInfo->runtime.errorText.clear();
    dsInfo->runtime.datasetType = datasetType;
    return true;
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
        objectInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
        objectInfo->runtime.errorText.clear();
        objectInfo->runtime.datasetType = datasetType.trimmed();
        objectInfo->runtime.propertyRows = rows;
        objectInfo->runtime.properties.clear();
        objectInfo->runtime.loadedPropertyNames.clear();
        for (const DatasetPropCacheRow& row : rows) {
            objectInfo->runtime.properties.insert(row.prop.trimmed(), row.value);
            objectInfo->runtime.loadedPropertyNames.insert(normalizedPropKey(row.prop));
        }
        objectInfo->runtime.allPropertiesLoaded = true;
        objectInfo->kind = dsKindFromNames(trimmedObject, datasetType.trimmed());
    } else {
        rebuildConnInfoFor(connIdx);
        if (DSInfo* rebuilt = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
            rebuilt->runtime.propertiesState = LoadState::Loaded;
            rebuilt->runtime.loadedAt = QDateTime::currentDateTimeUtc();
            rebuilt->runtime.errorText.clear();
            rebuilt->runtime.datasetType = datasetType.trimmed();
            rebuilt->runtime.propertyRows = rows;
            rebuilt->runtime.properties.clear();
            rebuilt->runtime.loadedPropertyNames.clear();
            for (const DatasetPropCacheRow& row : rows) {
                rebuilt->runtime.properties.insert(row.prop.trimmed(), row.value);
                rebuilt->runtime.loadedPropertyNames.insert(normalizedPropKey(row.prop));
            }
            rebuilt->runtime.allPropertiesLoaded = true;
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
        objectInfo->runtime.loadedPropertyNames.clear();
        objectInfo->runtime.allPropertiesLoaded = false;
    }
}

void MainWindow::removeDatasetPropertyEntriesForPool(int connIdx, const QString& poolName) {
    if (PoolInfo* poolInfo = findPoolInfo(connIdx, poolName)) {
        for (auto itDs = poolInfo->objectsByFullName.begin(); itDs != poolInfo->objectsByFullName.end(); ++itDs) {
            itDs->runtime.propertiesState = LoadState::NotLoaded;
            itDs->runtime.propertyRows.clear();
            itDs->runtime.properties.clear();
            itDs->runtime.loadedPropertyNames.clear();
            itDs->runtime.allPropertiesLoaded = false;
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
        if (!itProp->dirty()) {
            continue;
        }
        if (itProp->valueDirty) {
            draft.valuesByProp.insert(itProp.key(), itProp->value);
        }
        if (itProp->inheritDirty) {
            draft.inheritByProp.insert(itProp.key(), itProp->inherit);
        }
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

    const QVector<DatasetPropCacheRow> originalRows = datasetPropertyRowsFromModelOrCache(connIdx, poolName, normObject);
    QMap<QString, QString> originalValues;
    QMap<QString, bool> originalInherit;
    for (const DatasetPropCacheRow& row : originalRows) {
        originalValues.insert(row.prop, gsaComparableValue(row.prop, row.value));
        originalInherit.insert(row.prop, row.source.trimmed().toLower().startsWith(QStringLiteral("inherited")));
    }
    const auto runtimeProps = dsInfo->runtime.properties;
    for (auto it = runtimeProps.cbegin(); it != runtimeProps.cend(); ++it) {
        const QString prop = it.key().trimmed();
        if (prop.isEmpty() || originalValues.contains(prop)) {
            continue;
        }
        originalValues.insert(prop, gsaComparableValue(prop, it.value()));
    }
    auto valueIt = draft.valuesByProp.begin();
    while (valueIt != draft.valuesByProp.end()) {
        const QString originalValue = gsaComparableValue(valueIt.key(), originalValues.value(valueIt.key()));
        const QString currentValue = gsaComparableValue(valueIt.key(), valueIt.value());
        bool erase = false;
        if (valueIt.key().compare(QStringLiteral("mounted"), Qt::CaseInsensitive) == 0) {
            bool currentMounted = false;
            bool originalMounted = false;
            if (mountedStateFromAnyText(currentValue, &currentMounted)
                && mountedStateFromAnyText(originalValue, &originalMounted)
                && currentMounted == originalMounted
                && !draft.inheritByProp.contains(valueIt.key())) {
                erase = true;
            }
        } else if (currentValue == originalValue && !draft.inheritByProp.contains(valueIt.key())) {
            erase = true;
        }
        if (erase) {
            valueIt = draft.valuesByProp.erase(valueIt);
        } else {
            ++valueIt;
        }
    }
    auto inheritIt = draft.inheritByProp.begin();
    while (inheritIt != draft.inheritByProp.end()) {
        const bool originalInherited = originalInherit.value(inheritIt.key(), false);
        if (inheritIt.value() == originalInherited && !draft.valuesByProp.contains(inheritIt.key())) {
            inheritIt = draft.inheritByProp.erase(inheritIt);
        } else {
            ++inheritIt;
        }
    }
    draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();

    dsInfo->editSession.target = dsInfo->key;
    dsInfo->editSession.propertyEdits.clear();
    for (auto itProp = draft.valuesByProp.cbegin(); itProp != draft.valuesByProp.cend(); ++itProp) {
        DSPropertyEditValue value;
        value.value = itProp.value();
        value.valueDirty = true;
        if (draft.inheritByProp.contains(itProp.key())) {
            value.inherit = draft.inheritByProp.value(itProp.key(), false);
            value.inheritDirty = true;
        }
        dsInfo->editSession.propertyEdits.byName.insert(itProp.key(), value);
    }
    for (auto itInh = draft.inheritByProp.cbegin(); itInh != draft.inheritByProp.cend(); ++itInh) {
        auto existing = dsInfo->editSession.propertyEdits.byName.find(itInh.key());
        if (existing == dsInfo->editSession.propertyEdits.byName.end()) {
            DSPropertyEditValue value;
            value.inherit = itInh.value();
            value.inheritDirty = true;
            dsInfo->editSession.propertyEdits.byName.insert(itInh.key(), value);
        } else {
            existing->inherit = itInh.value();
            existing->inheritDirty = true;
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

const MainWindow::DatasetPermissionsCacheEntry* MainWindow::ensureDatasetPermissionsEntryLoaded(int connIdx,
                                                                                               const QString& poolName,
                                                                                               const QString& datasetName) {
    if (!ensureDatasetPermissionsLoaded(connIdx, poolName, datasetName)) {
        return nullptr;
    }
    return datasetPermissionsEntry(connIdx, poolName, datasetName);
}

const MainWindow::PoolDetailsCacheEntry* MainWindow::poolDetailsEntry(int connIdx, const QString& poolName) const {
    const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
    const auto it = m_poolDetailsCache.constFind(cacheKey);
    return (it == m_poolDetailsCache.cend()) ? nullptr : &it.value();
}

bool MainWindow::ensurePoolDetailsLoaded(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return false;
    }
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
        poolInfo && poolInfo->runtime.detailsState == LoadState::Loaded
        && (!poolInfo->runtime.zpoolPropertyRows.isEmpty() || !poolInfo->runtime.poolStatusText.trimmed().isEmpty())) {
        return true;
    }
    const PoolDetailsCacheEntry* cached = poolDetailsEntry(connIdx, trimmedPool);
    if (cached && cached->loaded
        && (!cached->propsRows.isEmpty() || !cached->statusText.trimmed().isEmpty())) {
        return true;
    }
    schedulePoolDetailsLoad(connIdx, trimmedPool);
    return false;
}

void MainWindow::schedulePoolDetailsLoad(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    const QString key = poolDetailsCacheKey(connIdx, trimmedPool);
    if (m_poolDetailsLoadsInFlight.contains(key)) {
        return;
    }
    m_poolDetailsLoadsInFlight.insert(key);
    const ConnectionProfile profile = m_profiles[connIdx];
    const bool daemonReadApiOk =
        !isWindowsConnection(connIdx)
        && connIdx >= 0
        && connIdx < m_states.size()
        && m_states[connIdx].daemonInstalled
        && m_states[connIdx].daemonActive
        && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    (void)QtConcurrent::run([this, profile, connIdx, trimmedPool, daemonReadApiOk]() {
        PoolDetailsCacheEntry fresh;
        QString errorText;
        {
            QString out;
            QString err;
            int rc = -1;
            const bool poolWin = isWindowsConnection(connIdx);
            const bool useRemoteScript = !poolWin;
            const QString propsCmdClassic = withSudo(
                profile,
                (useRemoteScript && !poolWin)
                    ? remoteScriptCommand(profile, QStringLiteral("zfsmgr-zpool-get-all-json"), {trimmedPool})
                    : mwhelpers::withUnixSearchPathCommand(
                          poolWin
                              ? QStringLiteral("zpool get -H -o property,value,source all %1")
                                    .arg(mwhelpers::shSingleQuote(trimmedPool))
                              : QStringLiteral("zpool get -j all %1")
                                    .arg(mwhelpers::shSingleQuote(trimmedPool))));
            const QString propsCmdDaemon = withSudo(
                profile, mwhelpers::withUnixSearchPathCommand(
                             QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-get-all %1")
                                 .arg(mwhelpers::shSingleQuote(trimmedPool))));
            bool propsOk = runSsh(profile, (daemonReadApiOk ? propsCmdDaemon : propsCmdClassic), 20000, out, err, rc) && rc == 0;
            if (!propsOk && daemonReadApiOk) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("daemon zpool-get-all fallback %1::%2 -> %3")
                           .arg(profile.name, trimmedPool, mwhelpers::oneLine(err.isEmpty() ? out : err)));
                out.clear();
                err.clear();
                rc = -1;
                propsOk = runSsh(profile, propsCmdClassic, 20000, out, err, rc) && rc == 0;
            }
            if (propsOk) {
                if (poolWin) {
                    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
                    for (const QString& line : lines) {
                        const QStringList parts = line.split('\t');
                        if (parts.size() < 3) {
                            continue;
                        }
                        fresh.propsRows.push_back(
                            QStringList{parts[0].trimmed(), parts[1].trimmed(), parts[2].trimmed()});
                    }
                } else {
                    const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
                    const QJsonObject pools = doc.object().value(QStringLiteral("pools")).toObject();
                    const QJsonObject poolObj = pools.value(trimmedPool).toObject();
                    const QJsonObject properties = poolObj.value(QStringLiteral("properties")).toObject();
                    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
                        const QJsonObject propObj = it.value().toObject();
                        const QString value = propObj.value(QStringLiteral("value")).toString().trimmed();
                        const QJsonObject sourceObj = propObj.value(QStringLiteral("source")).toObject();
                        const QString source = sourceObj.value(QStringLiteral("type")).toString().trimmed();
                        fresh.propsRows.push_back(QStringList{it.key(), value, source});
                    }
                }
            } else {
                errorText = err.trimmed();
            }

            out.clear();
            err.clear();
            rc = -1;
            const QString stCmdClassic = withSudo(
                profile,
                useRemoteScript
                    ? remoteScriptCommand(profile, QStringLiteral("zfsmgr-zpool-status"), {trimmedPool})
                    : mwhelpers::withUnixSearchPathCommand(
                          QStringLiteral("zpool status -v %1")
                              .arg(mwhelpers::shSingleQuote(trimmedPool))));
            const QString stCmdDaemon = withSudo(
                profile, mwhelpers::withUnixSearchPathCommand(
                             QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-status %1")
                                 .arg(mwhelpers::shSingleQuote(trimmedPool))));
            bool statusOk = runSsh(profile, (daemonReadApiOk ? stCmdDaemon : stCmdClassic), 20000, out, err, rc) && rc == 0;
            if (!statusOk && daemonReadApiOk) {
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("daemon zpool-status fallback %1::%2 -> %3")
                           .arg(profile.name, trimmedPool, mwhelpers::oneLine(err.isEmpty() ? out : err)));
                out.clear();
                err.clear();
                rc = -1;
                statusOk = runSsh(profile, stCmdClassic, 20000, out, err, rc) && rc == 0;
            }
            if (statusOk) {
                fresh.statusText = out.trimmed();
            } else {
                const QString statusErr = err.trimmed();
                fresh.statusText = statusErr;
                if (errorText.isEmpty()) {
                    errorText = statusErr;
                }
            }

            out.clear();
            err.clear();
            rc = -1;
            const QString stPCmdClassic = withSudo(
                profile,
                poolWin
                    ? QStringLiteral("zpool status -P %1")
                          .arg(mwhelpers::shSingleQuote(trimmedPool))
                    : mwhelpers::withUnixSearchPathCommand(
                          QStringLiteral("zpool status -P %1")
                              .arg(mwhelpers::shSingleQuote(trimmedPool))));
            const QString stPCmdDaemon = withSudo(
                profile, mwhelpers::withUnixSearchPathCommand(
                             QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-status-p %1")
                                 .arg(mwhelpers::shSingleQuote(trimmedPool))));
            bool statusPOk = runSsh(profile, (daemonReadApiOk ? stPCmdDaemon : stPCmdClassic), 20000, out, err, rc) && rc == 0;
            if (!statusPOk && daemonReadApiOk) {
                out.clear();
                err.clear();
                rc = -1;
                statusPOk = runSsh(profile, stPCmdClassic, 20000, out, err, rc) && rc == 0;
            }
            if (statusPOk) {
                fresh.statusPText = out.trimmed();
            } else {
                fresh.statusPText.clear();
            }
        }
        fresh.loaded = true;
        const bool ok = errorText.isEmpty() || !fresh.propsRows.isEmpty() || !fresh.statusText.trimmed().isEmpty();
        QMetaObject::invokeMethod(this, [this, connIdx, trimmedPool, ok, fresh, errorText]() {
            applyPoolDetailsLoadResult(connIdx, trimmedPool, ok, fresh, errorText);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::applyPoolDetailsLoadResult(int connIdx,
                                            const QString& poolName,
                                            bool ok,
                                            const PoolDetailsCacheEntry& fresh,
                                            const QString& errorText) {
    const QString trimmedPool = poolName.trimmed();
    const QString key = poolDetailsCacheKey(connIdx, trimmedPool);
    m_poolDetailsLoadsInFlight.remove(key);
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    Q_UNUSED(ok);
    Q_UNUSED(errorText);
    m_poolDetailsCache.insert(key, fresh);
    rebuildConnInfoFor(connIdx);
    applyPoolRootTooltipToVisibleTrees(connIdx, trimmedPool, fresh.statusText);
    const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool);
    if (m_connContentTree) {
        syncConnContentPoolColumnsFor(m_connContentTree, token);
    }
    if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
        syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
    }
    const int row = selectedPoolRowFromTabs();
    if (row >= 0 && row < m_poolListEntries.size()) {
        const auto& pe = m_poolListEntries[row];
        if (findConnectionIndexByName(pe.connection) == connIdx
            && pe.pool.trimmed().compare(trimmedPool, Qt::CaseInsensitive) == 0) {
            refreshSelectedPoolDetails(false, false);
        }
    }
}

bool MainWindow::ensurePoolAutoSnapshotInfoLoaded(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return false;
    }
    PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
    if (poolInfo && poolInfo->runtime.schedulesState == LoadState::Loaded) {
        return true;
    }
    if (poolInfo && poolInfo->runtime.schedulesState == LoadState::Error) {
        return false;
    }
    if (isWindowsConnection(connIdx)) {
        return false;
    }
    if (isPoolSuspended(connIdx, trimmedPool)) {
        if (poolInfo) {
            poolInfo->runtime.schedulesState = LoadState::Error;
            poolInfo->runtime.errorText = QStringLiteral("pool suspended");
        }
        return false;
    }
    if (poolInfo && poolInfo->runtime.schedulesState == LoadState::Loading) {
        return false;
    }
    schedulePoolAutoSnapshotInfoLoad(connIdx, trimmedPool);
    return false;
}

void MainWindow::invalidatePoolAutoSnapshotInfoForConnection(int connIdx) {
    if (ConnInfo* connInfo = findConnInfo(connIdx)) {
        for (auto itPool = connInfo->poolsByStableId.begin(); itPool != connInfo->poolsByStableId.end(); ++itPool) {
            itPool->runtime.schedulesState = LoadState::NotLoaded;
            itPool->runtime.autoSnapshotPropsByDataset.clear();
        }
    }
    const QString prefix = QStringLiteral("%1::").arg(connIdx);
    for (auto it = m_poolAutoSnapshotLoadsInFlight.begin(); it != m_poolAutoSnapshotLoadsInFlight.end();) {
        if (it->startsWith(prefix)) {
            it = m_poolAutoSnapshotLoadsInFlight.erase(it);
        } else {
            ++it;
        }
    }
    m_poolAutoSnapshotPendingLoadsByConn.remove(connIdx);
    m_poolAutoSnapshotDirtyPoolsByConn.remove(connIdx);
    m_poolAutoSnapshotUiDeferByConn.remove(connIdx);
}

void MainWindow::preloadPoolAutoSnapshotInfoForConnection(int connIdx) {
    if (connIdx < 0 || connIdx >= m_profiles.size() || isWindowsConnection(connIdx)) {
        return;
    }
    const ConnectionRuntimeState state =
        (connIdx < m_states.size()) ? m_states[connIdx] : ConnectionRuntimeState{};
    if (!state.gsaInstalled) {
        return;
    }
    int startedLoads = 0;
    for (const PoolImported& pool : state.importedPools) {
        const QString trimmedPool = pool.pool.trimmed();
        if (trimmedPool.isEmpty()) {
            continue;
        }
        if (schedulePoolAutoSnapshotInfoLoad(connIdx, trimmedPool)) {
            ++startedLoads;
        }
    }
    if (startedLoads > 0) {
        m_poolAutoSnapshotUiDeferByConn.insert(connIdx);
    }
}

bool MainWindow::schedulePoolAutoSnapshotInfoLoad(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || isWindowsConnection(connIdx)) {
        return false;
    }
    if (isPoolSuspended(connIdx, trimmedPool)) {
        if (PoolInfo* p = findPoolInfo(connIdx, trimmedPool)) {
            p->runtime.schedulesState = LoadState::Error;
            p->runtime.errorText = QStringLiteral("pool suspended");
        }
        return false;
    }
    PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
    if (poolInfo && (poolInfo->runtime.schedulesState == LoadState::Loaded
                     || poolInfo->runtime.schedulesState == LoadState::Loading)) {
        return false;
    }
    const QString key = QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool);
    if (m_poolAutoSnapshotLoadsInFlight.contains(key)) {
        return false;
    }
    if (!poolInfo) {
        rebuildConnInfoFor(connIdx);
        poolInfo = findPoolInfo(connIdx, trimmedPool);
    }
    if (poolInfo) {
        poolInfo->runtime.schedulesState = LoadState::Loading;
        poolInfo->runtime.errorText.clear();
    }
    m_poolAutoSnapshotLoadsInFlight.insert(key);
    m_poolAutoSnapshotPendingLoadsByConn[connIdx] =
        m_poolAutoSnapshotPendingLoadsByConn.value(connIdx, 0) + 1;
    const ConnectionProfile profile = m_profiles[connIdx];
    const bool daemonReadApiOk =
        !isWindowsConnection(connIdx)
        && connIdx >= 0
        && connIdx < m_states.size()
        && m_states[connIdx].daemonInstalled
        && m_states[connIdx].daemonActive
        && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    (void)QtConcurrent::run([this, profile, connIdx, trimmedPool, daemonReadApiOk]() {
        PoolAutoSnapshotLoadResult result;
        result.connIdx = connIdx;
        result.poolName = trimmedPool;
        const QStringList gsaProps = gsaPropertyKeysForModel();
        QStringList propArgs;
        for (const QString& prop : gsaProps) {
            propArgs << mwhelpers::shSingleQuote(prop);
        }
        const bool useRemoteScript = !isWindowsConnection(profile);
        const QString cmdClassic =
            withSudo(profile,
                     useRemoteScript
                         ? remoteScriptCommand(profile, QStringLiteral("zfsmgr-zfs-get-gsa-raw-recursive"),
                                               {trimmedPool})
                         : mwhelpers::withUnixSearchPathCommand(
                               QStringLiteral("zfs get -H -o name,property,value,source -r %1 %2")
                                   .arg(propArgs.join(QLatin1Char(',')),
                                        mwhelpers::shSingleQuote(trimmedPool))));
        const QString cmdDaemon =
            withSudo(profile,
                     mwhelpers::withUnixSearchPathCommand(
                         QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-get-gsa-raw-recursive %1")
                             .arg(mwhelpers::shSingleQuote(trimmedPool))));
        QString out;
        QString err;
        int rc = -1;
        bool scanOk = runSsh(profile, (daemonReadApiOk ? cmdDaemon : cmdClassic), 20000, out, err, rc) && rc == 0;
        if (!scanOk && daemonReadApiOk) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("daemon gsa recursive fallback %1::%2 -> %3")
                       .arg(profile.name, trimmedPool, mwhelpers::oneLine(err.isEmpty() ? out : err)));
            out.clear();
            err.clear();
            rc = -1;
            scanOk = runSsh(profile, cmdClassic, 20000, out, err, rc) && rc == 0;
        }
        if (!scanOk) {
            result.errorText = err.trimmed();
        } else {
            auto isLocallyConfiguredGsaSource = [](const QString& source) {
                const QString src = source.trimmed().toLower();
                if (src.isEmpty() || src == QStringLiteral("-")) {
                    return false;
                }
                if (src.startsWith(QStringLiteral("inherited")) || src == QStringLiteral("default")) {
                    return false;
                }
                return true;
            };
            for (const QString& line : out.split(QLatin1Char('\n'))) {
                const QString trimmed = line.trimmed();
                if (trimmed.isEmpty()) {
                    continue;
                }
                const QStringList cols = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                                       Qt::SkipEmptyParts);
                if (cols.size() < 4) {
                    continue;
                }
                const QString dsName = cols.at(0).trimmed();
                const QString prop = cols.at(1).trimmed();
                const QString value = cols.at(2).trimmed();
                const QString source = cols.mid(3).join(QStringLiteral(" ")).trimmed();
                if (dsName.isEmpty() || dsName.contains(QLatin1Char('@'))) {
                    continue;
                }
                if (prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)
                    && isLocallyConfiguredGsaSource(source)) {
                    result.loaded[dsName].insert(prop, value);
                }
            }
            result.ok = true;
        }
        QMetaObject::invokeMethod(this, [this, result]() {
            applyPoolAutoSnapshotInfoLoadResult(result.connIdx,
                                                result.poolName,
                                                result.ok,
                                                result.errorText,
                                                result.loaded);
        }, Qt::QueuedConnection);
    });
    return true;
}

void MainWindow::applyPoolAutoSnapshotInfoLoadResult(
    int connIdx,
    const QString& poolName,
    bool ok,
    const QString& errorText,
    const QMap<QString, QMap<QString, QString>>& loaded) {
    const QString trimmedPool = poolName.trimmed();
    const QString key = QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool);
    m_poolAutoSnapshotLoadsInFlight.remove(key);
    int pendingLoads = m_poolAutoSnapshotPendingLoadsByConn.value(connIdx, 0);
    if (pendingLoads > 0) {
        --pendingLoads;
    }
    if (pendingLoads <= 0) {
        m_poolAutoSnapshotPendingLoadsByConn.remove(connIdx);
        pendingLoads = 0;
    } else {
        m_poolAutoSnapshotPendingLoadsByConn[connIdx] = pendingLoads;
    }
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
    if (!poolInfo) {
        rebuildConnInfoFor(connIdx);
        poolInfo = findPoolInfo(connIdx, trimmedPool);
    }
    if (!poolInfo) {
        return;
    }
    if (ok) {
        poolInfo->runtime.autoSnapshotPropsByDataset = loaded;
        poolInfo->runtime.schedulesState = LoadState::Loaded;
        poolInfo->runtime.errorText.clear();
    } else {
        poolInfo->runtime.schedulesState = LoadState::Error;
        poolInfo->runtime.errorText = errorText.trimmed();
    }
    const bool deferUi = m_poolAutoSnapshotUiDeferByConn.contains(connIdx);
    if (deferUi) {
        m_poolAutoSnapshotDirtyPoolsByConn[connIdx].insert(trimmedPool);
        if (pendingLoads > 0) {
            return;
        }
    }

    QSet<QString> poolsToSync;
    if (deferUi) {
        poolsToSync = m_poolAutoSnapshotDirtyPoolsByConn.value(connIdx);
        m_poolAutoSnapshotDirtyPoolsByConn.remove(connIdx);
        m_poolAutoSnapshotUiDeferByConn.remove(connIdx);
    } else {
        poolsToSync.insert(trimmedPool);
    }
    for (const QString& pool : poolsToSync) {
        const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(pool);
        if (m_connContentTree) {
            syncConnContentPoolColumnsFor(m_connContentTree, token);
        }
        if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
            syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
        }
    }
}

bool MainWindow::executePoolCommand(int connIdx,
                                    const QString& poolName,
                                    const QString& actionName,
                                    const QString& remoteCmd,
                                    int timeoutMs,
                                    QString* failureDetailOut,
                                    bool refreshPoolsTable,
                                    bool refreshSelectedPoolDetailsAfter) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedAction = actionName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || trimmedAction.isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid pool command context");
        }
        return false;
    }
    if (isPoolSuspended(connIdx, trimmedPool)) {
        const QString detail = QStringLiteral("pool is suspended");
        if (failureDetailOut) {
            *failureDetailOut = detail;
        }
        appLog(QStringLiteral("WARN"),
               QStringLiteral("Bloqueado %1 sobre %2::%3 (%4)")
                   .arg(trimmedAction, m_profiles[connIdx].name, trimmedPool, detail));
        return false;
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Inicio %1 %2::%3").arg(trimmedAction.toLower(), p.name, trimmedPool));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, timeoutMs, out, err, rc) && rc == 0;
    setActionsLocked(false);
    if (!ok) {
        const QString detail = mwhelpers::oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err);
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error %1 %2::%3 -> %4")
                   .arg(trimmedAction.toLower(), p.name, trimmedPool, detail));
        if (failureDetailOut) {
            *failureDetailOut = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
        }
        return false;
    }

    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Fin %1 %2::%3").arg(trimmedAction.toLower(), p.name, trimmedPool));
    refreshConnectionByIndex(connIdx);
    if (refreshPoolsTable) {
        populateAllPoolsTables();
    }
    if (refreshSelectedPoolDetailsAfter) {
        refreshSelectedPoolDetails(true, true);
    }
    return true;
}

QString MainWindow::daemonizeZpoolMutationCommand(int connIdx, const QString& rawCmd) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return QString();
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    if (isWindowsConnection(p)) {
        return QString();
    }
    if (connIdx < 0 || connIdx >= m_states.size()) {
        return QString();
    }
    const ConnectionRuntimeState& st = m_states[connIdx];
    if (!st.daemonInstalled || !st.daemonActive) {
        return QString();
    }
    if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        return QString();
    }
    const QStringList parts = QProcess::splitCommand(rawCmd.trimmed());
    if (parts.size() < 2 || parts.first().trimmed() != QStringLiteral("zpool")) {
        return QString();
    }
    const QString op = parts.at(1).trimmed();
    if (!isAllowedGenericZpoolMutationOpClient(op)) {
        return QString();
    }
    QJsonArray arr;
    for (int i = 1; i < parts.size(); ++i) {
        arr.push_back(parts.at(i));
    }
    if (arr.isEmpty()) {
        return QString();
    }
    const QString payloadB64 =
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact).toBase64());
    return QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zpool-generic %1")
        .arg(mwhelpers::shSingleQuote(payloadB64));
}

bool MainWindow::fetchPoolCommandOutput(int connIdx,
                                        const QString& poolName,
                                        const QString& actionName,
                                        const QString& remoteCmd,
                                        QString* outputOut,
                                        QString* failureDetailOut,
                                        int timeoutMs) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedAction = actionName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || trimmedAction.isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid pool query context");
        }
        return false;
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Consulta %1 %2::%3").arg(trimmedAction.toLower(), p.name, trimmedPool));
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, remoteCmd, timeoutMs, out, err, rc) || rc != 0) {
        const QString detail = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error %1 %2::%3 -> %4")
                   .arg(trimmedAction.toLower(), p.name, trimmedPool, mwhelpers::oneLine(detail)));
        if (failureDetailOut) {
            *failureDetailOut = detail;
        }
        return false;
    }
    if (outputOut) {
        *outputOut = out;
    }
    return true;
}

bool MainWindow::executeConnectionCommand(int connIdx,
                                          const QString& actionName,
                                          const QString& remoteCmd,
                                          int timeoutMs,
                                          QString* failureDetailOut,
                                          WindowsCommandMode windowsMode,
                                          const QByteArray& stdinPayload) {
    if (connIdx < 0 || connIdx >= m_profiles.size() || actionName.trimmed().isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid connection command context");
        }
        return false;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, timeoutMs, out, err, rc, {}, {}, {}, windowsMode, stdinPayload) && rc == 0;
    if (!ok) {
        QStringList parts;
        if (!err.trimmed().isEmpty()) {
            parts << err.trimmed();
        }
        if (!out.trimmed().isEmpty()) {
            parts << out.trimmed();
        }
        const QString detail = parts.isEmpty() ? QStringLiteral("exit %1").arg(rc) : parts.join(QStringLiteral("\n\n"));
        appLog(QStringLiteral("WARN"),
               QStringLiteral("Fallo comando conexión [%1] en \"%2\" (rc=%3): %4")
                   .arg(actionName.trimmed().isEmpty() ? QStringLiteral("?") : actionName.trimmed(),
                        p.name,
                        QString::number(rc),
                        mwhelpers::oneLine(detail)));
        if (failureDetailOut) {
            *failureDetailOut = detail;
        }
    }
    return ok;
}

bool MainWindow::fetchConnectionCommandOutput(int connIdx,
                                              const QString& actionName,
                                              const QString& remoteCmd,
                                              QString* outputOut,
                                              QString* failureDetailOut,
                                              int timeoutMs,
                                              WindowsCommandMode windowsMode) {
    if (connIdx < 0 || connIdx >= m_profiles.size() || actionName.trimmed().isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid connection query context");
        }
        return false;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, timeoutMs, out, err, rc, {}, {}, {}, windowsMode) && rc == 0;
    if (!ok) {
        if (failureDetailOut) {
            QStringList parts;
            if (!err.trimmed().isEmpty()) {
                parts << err.trimmed();
            }
            if (!out.trimmed().isEmpty()) {
                parts << out.trimmed();
            }
            *failureDetailOut = parts.isEmpty() ? QStringLiteral("exit %1").arg(rc) : parts.join(QStringLiteral("\n\n"));
        }
        return false;
    }
    if (outputOut) {
        *outputOut = out;
    }
    return true;
}

bool MainWindow::fetchConnectionProbeOutput(int sourceConnIdx,
                                            const QString& actionName,
                                            const QString& remoteCmd,
                                            QString* mergedOutputOut,
                                            QString* failureDetailOut,
                                            int timeoutMs) {
    if (sourceConnIdx < 0 || sourceConnIdx >= m_profiles.size() || actionName.trimmed().isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid probe context");
        }
        return false;
    }
    const ConnectionProfile& src = m_profiles[sourceConnIdx];
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(src, remoteCmd, timeoutMs, out, err, rc);
    const QString merged = (out + QStringLiteral("\n") + err).trimmed();
    if (mergedOutputOut) {
        *mergedOutputOut = merged;
    }
    if (!ok || rc != 0) {
        if (failureDetailOut) {
            *failureDetailOut = merged.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : merged;
        }
        return false;
    }
    if (failureDetailOut) {
        failureDetailOut->clear();
    }
    return true;
}

QMap<QString, QMap<QString, QString>> MainWindow::poolAutoSnapshotPropsByDataset(int connIdx, const QString& poolName) const {
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, poolName)) {
        return poolInfo->runtime.autoSnapshotPropsByDataset;
    }
    return {};
}

bool MainWindow::ensureDatasetSnapshotHoldsLoaded(int connIdx, const QString& poolName, const QString& objectName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return false;
    }
    DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (dsInfo && dsInfo->runtime.holdsState == LoadState::Loaded) {
        return true;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    QString out;
    QString err;
    int rc = -1;
    const QString cmd = withSudo(
        p,
        QStringLiteral("zfs holds -H %1").arg(mwhelpers::shSingleQuote(trimmedObject)));
    if (!runSsh(p, cmd, 20000, out, err, rc) || rc != 0) {
        if (dsInfo) {
            dsInfo->runtime.holdsState = LoadState::Error;
            dsInfo->runtime.errorText = err.trimmed();
        }
        return false;
    }

    QVector<QPair<QString, QString>> holds;
    QSet<QString> seen;
    for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = line.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString tag = parts.value(1).trimmed();
        const QString timestamp = parts.value(2).trimmed();
        const QString key = tag.toLower();
        if (tag.isEmpty() || seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        holds.push_back(qMakePair(tag, timestamp));
    }
    if (dsInfo) {
        dsInfo->runtime.snapshotHolds = holds;
        dsInfo->runtime.holdsState = LoadState::Loaded;
        dsInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
        dsInfo->runtime.errorText.clear();
    }
    return true;
}

QVector<QPair<QString, QString>> MainWindow::datasetSnapshotHolds(int connIdx, const QString& poolName, const QString& objectName) const {
    if (const DSInfo* dsInfo = findDsInfo(connIdx, poolName, objectName)) {
        return dsInfo->runtime.snapshotHolds;
    }
    return {};
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
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
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
    const int connIdx = item->data(0, Qt::UserRole + 10).toInt();
    const QString poolName = item->data(0, Qt::UserRole + 11).toString().trimmed();
    const QString token = (connIdx >= 0 && !poolName.isEmpty())
                              ? QStringLiteral("%1::%2").arg(connIdx).arg(poolName)
                              : QString();
    Q_UNUSED(token);
    tree->setCurrentItem(item);
    refreshConnContentPropertiesFor(tree);
    return true;
}

bool MainWindow::setDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool expanded, bool bottom) {
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
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
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
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
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree) {
        return;
    }
    const QString token = connContentTokenForTree(tree);
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
    populateDatasetTree(tree, connIdx, poolName, DatasetTreeContext::ConnectionContent, true);
    if (!datasetToSelect.trimmed().isEmpty()) {
        selectDatasetForTest(datasetToSelect, bottom);
    }
}

QStringList MainWindow::topLevelPoolNamesForTest(bool bottom) const {
    QStringList names;
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
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
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
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
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
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
    const int connIdx = m_topDetailConnIdx;
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
        trk(QStringLiteral("t_refresh_conn_ctx001"),
            QStringLiteral("Refrescar"),
            QStringLiteral("Refresh"),
            QStringLiteral("刷新")),
        QString(),
        trk(QStringLiteral("t_new_conn_ctx001"),
            QStringLiteral("Nueva Conexión"),
            QStringLiteral("New Connection"),
            QStringLiteral("新建连接")),
        trk(QStringLiteral("t_edit_conn_ctx001"),
            QStringLiteral("Editar"),
            QStringLiteral("Edit"),
            QStringLiteral("编辑")),
        trk(QStringLiteral("t_del_conn_ctx001"),
            QStringLiteral("Borrar"),
            QStringLiteral("Delete"),
            QStringLiteral("删除")),
        QString(),
        QStringLiteral("GSA"),
        QString(),
        trk(QStringLiteral("t_new_pool_ctx_001"),
            QStringLiteral("Nuevo Pool"),
            QStringLiteral("New Pool"),
            QStringLiteral("新建存储池")),
        QString(),
        trk(QStringLiteral("t_install_msys_ctx001"),
            QStringLiteral("Instalar MSYS2"),
            QStringLiteral("Install MSYS2"),
            QStringLiteral("安装 MSYS2")),
        trk(QStringLiteral("t_install_helpers_ctx001"),
            QStringLiteral("Instalar comandos auxiliares"),
            QStringLiteral("Install helper commands"),
            QStringLiteral("安装辅助命令")),
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
    Q_UNUSED(bottom);
    const int connIdx = m_topDetailConnIdx;
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
        trk(QStringLiteral("t_pool_refresh_status001"),
            QStringLiteral("Actualizar estado"),
            QStringLiteral("Refresh status"),
            QStringLiteral("刷新状态")),
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
        QStringLiteral("Upgrade"),
        QStringLiteral("Reguid"),
        QStringLiteral("Trim"),
        QStringLiteral("Initialize"),
        QStringLiteral("Clear"),
        QStringLiteral("Destroy"),
        QStringLiteral("Mostrar Datasets programados"),
    };
}
