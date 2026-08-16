#pragma once

// El modelo de datos de una conexión: lo que la aplicación sabe de cada máquina y de sus
// pools, datasets y permisos.
//
// Estaba declarado DENTRO de `MainWindow`, lo que impedía usarlo en ninguna otra parte:
// ni en un registro de conexiones, ni en un CLI, ni en pruebas que no levanten la
// ventana. Sacarlo es la condición previa de todo eso.
//
// Sigue usando tipos de Qt: este paso desacopla de la CLASE, no todavía de Qt. Ver
// docs/diseno_tecnico_capa_base_sin_qt.md.

#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include "connectionstore.h"

struct PoolImported {
    QString connection;
    QString pool;
    QString action;
};

struct PoolImportable {
    QString connection;
    QString pool;
    QString guid;
    QString state;
    QString reason;
    QString action;
};

struct ConnectionRuntimeState {
    QString status;
    QString detail;
    QString zfsVersion;
    QString machineUuid;
    QString osLine;
    QString connectionMethod;
    QString zfsVersionFull;
    QStringList detectedUnixCommands;
    QStringList missingUnixCommands;
    QString helperPlatformId;
    QString helperPlatformLabel;
    QString helperPackageManagerId;
    QString helperPackageManagerLabel;
    QString helperInstallReason;
    QString helperInstallCommandPreview;
    QStringList helperInstallableCommands;
    QStringList helperUnsupportedCommands;
    QStringList helperInstallPackages;
    bool helperPackageManagerDetected{false};
    bool helperInstallSupported{false};
    QVector<PoolImported> importedPools;
    QVector<PoolImportable> importablePools;
    QMap<QString, QString> poolGuidByName;
    QVector<QPair<QString, QString>> mountedDatasets; // dataset, mountpoint
    QMap<QString, QString> poolStatusByName;
    QString daemonScheduler;
    QString daemonVersion;
    QString daemonApiVersion;
    QString daemonDetail;
    QStringList daemonAttentionReasons;
    bool daemonNeedsAttention{false};
    bool daemonInstalled{false};
    bool daemonActive{false};
    bool daemonNativeBinary{false};
    bool daemonJobsSupported{false};
    // Verbos que el agente declara servir, tal cual los publica en --health. Vacío
    // significa "no lo dice", y entonces manda la tabla estática de zfsmgr::caps.
    QSet<QString> daemonCaps;
    bool daemonZpoolImportUsable{true};
    QString daemonLastSeenZedEvent{QStringLiteral("@")}; // "@" = never polled; "" = polled/no events; "T" = last event
};

struct DatasetRecord {
    QString name;
    QString guid;
    QString used;
    QString compressRatio;
    QString encryption;
    QString creation;
    QString referenced;
    QString mounted;
    QString mountpoint;
    QString canmount;
};

struct PoolDatasetCache {
    bool loaded{false};
    // Un intento de carga que falló también es un resultado y hay que recordarlo.
    // Sin esto, cada reconstrucción del árbol vuelve a intentar la carga remota,
    // y como el fallo hace que el árbol se reconstruya para mostrarlo, se entra
    // en un bucle: en los registros se ven ocho reconstrucciones del mismo árbol
    // en un mismo segundo, cada una con su ida y vuelta por SSH.
    // Se limpia con el resto de la entrada al refrescar la conexión, que es
    // justo cuando tiene sentido reintentar.
    bool loadFailed{false};
    bool autoSnapshotPropsLoaded{false};
    QVector<DatasetRecord> datasets;
    QMap<QString, QStringList> snapshotsByDataset;
    QMap<QString, QString> objectGuidByName;
    QMap<QString, DatasetRecord> recordByName;
    QMap<QString, QString> driveletterByDataset;
    QMap<QString, QMap<QString, QString>> autoSnapshotPropsByDataset;
};

struct PoolDetailsCacheEntry {
    bool loaded{false};
    QVector<QStringList> propsRows; // property,value,source
    QString statusText;
    QString statusPText;
};

struct DatasetPropCacheRow {
    QString prop;
    QString value;
    QString source;
    QString readonly;
};

struct DatasetPermissionGrant {
    QString scope;
    QString targetType;
    QString targetName;
    QStringList permissions;
    bool pending{false};
};

struct DatasetPermissionSet {
    QString name;
    QStringList permissions;
};

struct DatasetPermissionsCacheEntry {
    bool loaded{false};
    QVector<DatasetPermissionGrant> localGrants;
    QVector<DatasetPermissionGrant> descendantGrants;
    QVector<DatasetPermissionGrant> localDescendantGrants;
    QStringList createPermissions;
    QVector<DatasetPermissionSet> permissionSets;
    QVector<DatasetPermissionGrant> originalLocalGrants;
    QVector<DatasetPermissionGrant> originalDescendantGrants;
    QVector<DatasetPermissionGrant> originalLocalDescendantGrants;
    QStringList originalCreatePermissions;
    QVector<DatasetPermissionSet> originalPermissionSets;
    QStringList systemUsers;
    QStringList systemGroups;
    bool dirty{false};
};

struct ConnKey {
    QString connectionId;
};

struct PoolKey {
    ConnKey conn;
    QString poolGuid;
    QString poolName;
};

struct DSKey {
    PoolKey pool;
    QString fullName;
};

enum class LoadState {
    NotLoaded,
    Loading,
    Loaded,
    Stale,
    Error,
};

enum class DSKind {
    Filesystem,
    Volume,
    Snapshot,
    Unknown,
};

struct DSPropertyCapability {
    bool visible{true};
    bool editableInline{false};
    bool editableBySet{false};
    bool editableBySpecialAction{false};
    QString specialActionId;
    bool inheritable{false};
};

struct DSCapabilities {
    bool canMount{false};
    bool canUnmount{false};
    bool canDestroy{false};
    bool canRename{false};
    bool canClone{false};
    bool canManagePermissions{false};
    bool canManageSchedules{false};
    QMap<QString, DSPropertyCapability> propertyCaps;
};

struct DSPropertyEditValue {
    QString value;
    bool inherit{false};
    bool valueDirty{false};
    bool inheritDirty{false};
    bool dirty() const {
        return valueDirty || inheritDirty;
    }
};

struct DSPropertyEditState {
    QMap<QString, DSPropertyEditValue> byName;
    bool dirty() const {
        for (auto it = byName.cbegin(); it != byName.cend(); ++it) {
            if (it->dirty()) {
                return true;
            }
        }
        return false;
    }
    void clear() {
        byName.clear();
    }
};

struct DSPermissionsEditState {
    bool dirty{false};
};

struct DSScheduleEditState {
    bool dirty{false};
};

struct DSEditSession {
    DSKey target;
    DSPropertyEditState propertyEdits;
    DSPermissionsEditState permissionsEdits;
    DSScheduleEditState scheduleEdits;

    bool dirty() const {
        return propertyEdits.dirty() || permissionsEdits.dirty || scheduleEdits.dirty;
    }

    void clear() {
        propertyEdits.clear();
        permissionsEdits = DSPermissionsEditState{};
        scheduleEdits = DSScheduleEditState{};
    }
};

struct ConnectionRuntimeInfo {
    LoadState state{LoadState::NotLoaded};
    QString errorText;
    QDateTime loadedAt;
    ConnectionRuntimeState snapshot;
};

struct PoolRuntimeInfo {
    LoadState detailsState{LoadState::NotLoaded};
    LoadState schedulesState{LoadState::NotLoaded};
    QString errorText;
    QDateTime loadedAt;
    QString poolStatusText;
    QMap<QString, QString> zpoolProperties;
    QVector<QStringList> zpoolPropertyRows;
    QMap<QString, QMap<QString, QString>> autoSnapshotPropsByDataset;
    bool imported{false};
    bool importable{false};
    QString importState;
    QString importReason;
    QString importAction;
};

struct DSRuntimeInfo {
    LoadState propertiesState{LoadState::NotLoaded};
    LoadState permissionsState{LoadState::NotLoaded};
    LoadState schedulesState{LoadState::NotLoaded};
    LoadState holdsState{LoadState::NotLoaded};
    QString errorText;
    QDateTime loadedAt;
    QString datasetType;
    QMap<QString, QString> properties;
    QVector<DatasetPropCacheRow> propertyRows;
    QSet<QString> loadedPropertyNames;
    bool allPropertiesLoaded{false};
    QStringList directSnapshots;
    QVector<QPair<QString, QString>> snapshotHolds;
};

struct DSInfo {
    DSKey key;
    DSKind kind{DSKind::Unknown};
    QString parentFullName;
    QStringList childFullNames;
    DSRuntimeInfo runtime;
    DSCapabilities capabilities;
    DatasetPermissionsCacheEntry permissionsCache;
    DSEditSession editSession;
};

struct PoolInfo {
    PoolKey key;
    PoolRuntimeInfo runtime;
    QMap<QString, DSInfo> objectsByFullName;
    QStringList rootObjectNames;
};

struct ConnInfo {
    ConnKey key;
    int connIdx{-1};
    ConnectionProfile profile;
    ConnectionRuntimeInfo runtime;
    QMap<QString, PoolInfo> poolsByStableId;
};

struct PoolListEntry {
    QString connection;
    QString pool;
    QString guid;
    QString state;
    QString imported;
    QString reason;
    QString action;
};
