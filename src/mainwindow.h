#pragma once

#include "connectionstore.h"
#include "connectiondialog.h"
#include "connectiondatasettreepane.h"
#include "connectiondatasettreecoordinator.h"
#include "connectiondatasettreewidget.h"
#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.10.0rc1"
#endif

#include <QMainWindow>
#include <QDateTime>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <functional>

class QTimer;
class QComboBox;
class QColor;
class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPoint;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QAction;
class QCloseEvent;
class QTableWidget;
class QTabBar;
class QTabWidget;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;
class QTextEdit;
class QByteArray;
class MainWindowConnectionDatasetTreeDelegate;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    enum class DatasetTreeContext {
        Origin,
        Destination,
        ConnectionContent,
        ConnectionContentMulti,
    };
    enum class PendingItemStatus { Pending, Running, Success, Failed };
    struct InlinePropGroupConfig {
        QString name;
        QStringList props;
    };
    struct UiTestDatasetSeed {
        QString name;
        QString mountpoint;
        QString canmount{QStringLiteral("on")};
        QString mounted{QStringLiteral("yes")};
        QStringList snapshots;
    };
    struct UiTestPropertySeed {
        QString prop;
        QString value;
        QString source{QStringLiteral("local")};
        QString readonly{QStringLiteral("no")};
    };

    explicit MainWindow(const QString& masterPassword, const QString& language, QWidget* parent = nullptr);
    ~MainWindow() override;
    void configureSingleConnectionUiTestState(const ConnectionProfile& profile,
                                              const QStringList& importedPools,
                                              const QStringList& importablePools);
    void configurePoolDatasetsForTest(int connIdx,
                                      const QString& poolName,
                                      const QVector<UiTestDatasetSeed>& datasets);
    void rebuildConnectionDetailsForTest();
    void setShowPoolInfoNodeForTest(bool visible);
    void setShowInlineGsaNodeForTest(bool visible);
    void setShowAutomaticSnapshotsForTest(bool visible);
    void setConnectionGsaStateForTest(int connIdx, bool installed, bool active, const QString& version = QString());
    void configureDatasetPropertiesForTest(int connIdx,
                                           const QString& objectName,
                                           const QString& datasetType,
                                           const QVector<UiTestPropertySeed>& rows);
    void debugTrace(const QString& msg);
    QStringList topLevelPoolNamesForTest(bool bottom = false) const;
    QStringList childLabelsForDatasetForTest(const QString& datasetName, bool bottom = false) const;
    QStringList snapshotNamesForDatasetForTest(const QString& datasetName, bool bottom = false) const;
    bool selectDatasetForTest(const QString& datasetName, bool bottom = false);
    bool setDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool expanded, bool bottom = false);
    bool isDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool bottom = false) const;
    void rebuildConnContentTreeForTest(const QString& datasetToSelect, bool bottom = false);
    QStringList connectionContextMenuTopLevelLabelsForTest() const;
    QStringList connectionRefreshMenuLabelsForTest() const;
    QStringList connectionGsaMenuLabelsForTest() const;
    QStringList poolContextMenuLabelsForTest(const QString& poolName, bool bottom = false) const;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class MainWindowConnectionDatasetTreeDelegate;
    struct DatasetSelectionContext;

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
        bool unixFromMsysOrMingw{false};
        QString commandsLayer;
        QStringList powershellFallbackCommands;
        bool helperPackageManagerDetected{false};
        bool helperInstallSupported{false};
        QVector<PoolImported> importedPools;
        QVector<PoolImportable> importablePools;
        QMap<QString, QString> poolGuidByName;
        QVector<QPair<QString, QString>> mountedDatasets; // dataset, mountpoint
        QMap<QString, QString> poolStatusByName;
        QString gsaScheduler;
        QString gsaVersion;
        QString gsaDetail;
        QStringList gsaKnownConnections;
        QStringList gsaRequiredConnections;
        QStringList gsaAttentionReasons;
        bool gsaNeedsAttention{false};
        bool gsaInstalled{false};
        bool gsaActive{false};
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
        bool autoSnapshotPropsLoaded{false};
        QVector<DatasetRecord> datasets;
        QMap<QString, QStringList> snapshotsByDataset;
        QMap<QString, QString> objectGuidByName;
        QMap<QString, DatasetRecord> recordByName;
        QMap<QString, QString> driveletterByDataset;
        QMap<QString, QMap<QString, QString>> autoSnapshotPropsByDataset;
    };

    struct DatasetSelectionContext {
        bool valid{false};
        int connIdx{-1};
        QString poolName;
        QString datasetName;
        QString snapshotName;
    };

    struct ConnContentTreeState {
        QStringList expandedDatasets;
        QStringList expandedNodePaths;
        bool poolRootExpanded{true};
        bool infoExpanded{false};
        QMap<QString, bool> poolRootExpandedByPool;
        QMap<QString, bool> infoExpandedByPool;
        QString selectedDataset;
        QString selectedSnapshot;
        QString selectedNodePath;
        QMap<QString, QString> snapshotByDataset;
        QMap<QString, QStringList> expandedChildPathsByDataset;
        QByteArray headerState;
        int verticalScrollValue{0};
        int horizontalScrollValue{0};
    };

    struct DatasetPropsDraft {
        QMap<QString, QString> valuesByProp;
        QMap<QString, bool> inheritByProp;
        bool dirty{false};
    };

    struct PendingDatasetRenameDraft {
        int connIdx{-1};
        QString poolName;
        QString sourceName;
        QString targetName;
    };

    struct PendingShellActionDraft {
        enum class RefreshScope {
            None,
            TargetOnly,
            SourceAndTarget
        };
        QString scopeLabel;
        QString displayLabel;
        QString command;
        int timeoutMs{0};
        bool streamProgress{true};
        DatasetSelectionContext refreshSource;
        DatasetSelectionContext refreshTarget;
        RefreshScope refreshScope{RefreshScope::SourceAndTarget};
    };
    struct PendingPropertyDraftEntry {
        int connIdx{-1};
        QString poolName;
        QString token;
        QString objectName;
        DatasetPropsDraft draft;
    };

    struct PoolDetailsCacheEntry {
        bool loaded{false};
        QVector<QStringList> propsRows; // property,value,source
        QString statusText;
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
    struct PendingPermissionDraftEntry {
        int connIdx{-1};
        QString poolName;
        QString datasetName;
        DatasetPermissionsCacheEntry entry;
    };
    struct PendingChange {
        enum class Kind {
            Property,
            Permission,
            Rename,
            ShellAction,
        };
        Kind kind{Kind::Property};
        QString stableId;
        QString displayLine;
        QString commandLine;
        int connIdx{-1};
        QString poolName;
        QString objectName;
        QString propertyName;
        bool focusPermissionsNode{false};
        bool removableIndividually{false};
        bool executableIndividually{false};
        PendingDatasetRenameDraft renameDraft;
        PendingShellActionDraft shellDraft;
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

    enum class WindowsCommandMode {
        Auto,
        PowerShellNative,
        UnixShell,
    };

    void buildUi();
    void loadConnections();
    void ensureStartupLocalSudoConnection();
    void rebuildConnectionsTable();
    int connectionIndexByNameOrId(const QString& value) const;
    bool connectionsReferToSameMachine(int a, int b) const;
    int equivalentSshForLocal(int localIdx) const;
    bool canSshBetweenConnections(int rowIdx, int colIdx, QString* errorOut = nullptr, int* effectiveDstIdxOut = nullptr);
    void refreshAllConnections();
    void refreshSelectedConnection();
    void createConnection();
    void installHelperCommandsForSelectedConnection();
    void installMsysForSelectedConnection();
    void editConnection();
    void deleteConnection();
    int currentConnectionIndexFromUnifiedTree() const;
    int currentConnectionIndexFromUi() const;
    void setCurrentConnectionInUi(int connIdx);
    QColor connectionStateRowColor(int connIdx) const;
    QString connectionStateColorReason(int connIdx) const;
    QString connectionStateTooltipHtml(int connIdx) const;
    void openConnectivityMatrixDialog();
    void showConnectionContextMenu(int connIdx, const QPoint& globalPos, QTreeWidget* sourceTree = nullptr);
    void onConnectionSelectionChanged();
    void updateSecondaryConnectionDetail();
    void rebuildConnectionEntityTabs();
    struct DatasetTreeRenderOptions {
        bool includePoolRoot{false};
        bool interactiveConnContent{false};
        bool showInlinePropertyNodes{true};
        bool showInlinePermissionsNodes{true};
        bool showInlineGsaNode{true};
        bool showAutomaticSnapshots{true};
    };
    DatasetTreeRenderOptions datasetTreeRenderOptionsForTree(const QTreeWidget* tree,
                                                             DatasetTreeContext side) const;
    void appendDatasetTreeForPool(QTreeWidget* tree,
                                  int connIdx,
                                  const QString& poolName,
                                  DatasetTreeContext side,
                                  const DatasetTreeRenderOptions& options,
                                  bool allowRemoteLoadIfMissing = true);
    void ensureConnectionRootAuxNodes(QTreeWidget* tree, QTreeWidgetItem* connRoot, int connIdx);
    bool applyConnectionInlineFieldValue(int connIdx,
                                         const QString& fieldKey,
                                         const QString& rawValue,
                                         QString* normalizedOut = nullptr,
                                         QString* errorOut = nullptr);
    void attachDatasetTreeSnapshotCombos(QTreeWidget* tree, DatasetTreeContext side);
    void populateConnectionPoolsIntoTree(QTreeWidget* tree,
                                         int connIdx,
                                         const ConnectionRuntimeState& st);
    void onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, DatasetTreeContext side, const QString& chosen);
    void onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, DatasetTreeContext side);
    void clearOtherSnapshotSelections(QTreeWidget* tree, QTreeWidgetItem* keepItem);
    void refreshConnectionNodeDetails();
    void updateConnectionDetailTitlesForCurrentSelection();
    void rebuildConnContentDetailTree(QTreeWidget* tree,
                                      int connIdx,
                                      bool& rebuildingFlag,
                                      int* forceRestoreConnIdx,
                                      const std::function<void(int)>& saveTreeState,
                                      const std::function<void()>& clearPendingState = {});
    void saveTopTreeStateForConnection(int connIdx);
    void saveBottomTreeStateForConnection(int connIdx);
    void restoreTopTreeStateForConnection(int connIdx);
    void restoreBottomTreeStateForConnection(int connIdx);
    void saveConnContentTreeState(QTreeWidget* tree, const QString& token);
    void saveConnContentTreeStateFor(QTreeWidget* tree, const QString& token);
    void saveConnContentTreeState(const QString& token);
    void setConnContentTreeStateWriteLocked(bool locked);
    bool connContentTreeStateWriteLocked() const;
    void restoreConnContentTreeState(QTreeWidget* tree, const QString& token);
    void restoreConnContentTreeStateFor(QTreeWidget* tree, const QString& token);
    void restoreConnContentTreeState(const QString& token);
    void rebuildConnContentTreeFor(QTreeWidget* tree,
                                   const QString& token,
                                   int connIdx,
                                   const QString& poolName,
                                   bool restoreState = true);
    QTreeWidgetItem* findConnContentDatasetItemFor(QTreeWidget* tree,
                                                   int connIdx,
                                                   const QString& poolName,
                                                   const QString& datasetName) const;
    QString connectionDisplayModeForIndex(int connIdx) const;
    void syncConnectionDisplaySelectors();
    void applyConnectionDisplayMode(int connIdx, const QString& mode);
    void resizeTreeColumnsToVisibleContent(QTreeWidget* tree);
    int propColumnCountForTree(const QTreeWidget* tree) const;
    void syncConnContentPropertyColumns(QTreeWidget* tree);
    void syncConnContentPropertyColumnsFor(QTreeWidget* tree, const QString& token);
    void syncConnContentPropertyColumns();
    void syncConnContentPoolColumns(QTreeWidget* tree, const QString& token);
    void syncConnContentPoolColumnsFor(QTreeWidget* tree, const QString& token);
    void syncConnContentPoolColumns();
    void updateConnContentPropertyValues(const QString& token,
                                         const QString& objectName,
                                         const QMap<QString, QString>& valuesByProp);
    void updateConnContentDraftValue(const QString& token,
                                     const QString& objectName,
                                     const QString& prop,
                                     const QString& value);
    void updateConnContentDraftInherit(const QString& token,
                                       const QString& objectName,
                                       const QString& prop,
                                       bool inherit);
    QString gsaMenuLabelForConnection(int connIdx) const;
    bool installOrUpdateGsaForConnection(int connIdx);
    bool uninstallGsaForConnection(int connIdx);
    bool validatePendingGsaDrafts(QString* errorOut = nullptr);
    bool showAutomaticSnapshots() const;

    ConnectionRuntimeState refreshConnection(const ConnectionProfile& p);
    bool runSsh(const ConnectionProfile& p,
                const QString& remoteCmd,
                int timeoutMs,
                QString& out,
                QString& err,
                int& rc,
                const std::function<void(const QString&)>& onStdoutLine = {},
                const std::function<void(const QString&)>& onStderrLine = {},
                const std::function<void(int)>& onIdleTimeoutRemaining = {},
                WindowsCommandMode windowsMode = WindowsCommandMode::Auto,
                const QByteArray& stdinPayload = {});
    void closeAllSshControlMasters();
    QString withSudo(const ConnectionProfile& p, const QString& cmd) const;
    QString withSudoStreamInput(const ConnectionProfile& p, const QString& cmd) const;
    bool isLocalConnection(const ConnectionProfile& p) const;
    bool isLocalConnection(int connIdx) const;
    bool isWindowsConnection(const ConnectionProfile& p) const;
    bool isWindowsConnection(int connIdx) const;
    bool supportsAlternateDatasetMount(int connIdx) const;
    QString wrapRemoteCommand(const ConnectionProfile& p,
                              const QString& remoteCmd,
                              WindowsCommandMode windowsMode = WindowsCommandMode::Auto) const;
    QString sshExecFromLocal(const ConnectionProfile& p,
                             const QString& remoteCmd,
                             WindowsCommandMode windowsMode = WindowsCommandMode::Auto) const;
    bool getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut);
    QString effectiveMountPath(int connIdx, const QString& poolName, const QString& datasetName, const QString& mountpointHint, const QString& mountedValue);
    QString datasetCacheKey(int connIdx, const QString& poolName) const;
    QString datasetPermissionsCacheKey(int connIdx, const QString& poolName, const QString& datasetName) const;
    QString connectionAccountCacheKey(int connIdx) const;
    QString pendingDatasetRenameCommand(const PendingDatasetRenameDraft& draft) const;
    bool queuePendingDatasetRename(const PendingDatasetRenameDraft& draft, QString* errorOut = nullptr);
    QVector<PendingChange> pendingChanges() const;
    bool findPendingChangeByDisplayLine(const QString& line, PendingChange* out) const;
    QStringList pendingConnContentApplyCommands() const;
    QStringList pendingConnContentApplyDisplayLines() const;
    void activatePendingChangeAtCursor();
    bool focusPendingChangeLine(const QString& line);
    void updatePendingChangesList();
    void startPendingApplyAnimation();
    void finishPendingApplyAnimation();
    void splitAndRootConnContent(Qt::Orientation orientation, bool insertBefore, int connIdx,
                                  const QString& poolName, const QString& rootDataset,
                                  QTreeWidget* sourceTree = nullptr);
    void closeSplitTree(QTreeWidget* tree);
    void rebuildAllSplitTrees();
    QString serializeSplitTreeLayoutState() const;
    void restoreSplitTreeLayoutFromState(const QString& state);
    void appendSplitDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName,
                                 const QString& rootDataset, const QString& displayRoot);
    void appendSplitDatasetTreeForConnection(QTreeWidget* tree, int connIdx);
    void installConnContentTreeHeaderContextMenu(QTreeWidget* tree);
    QString poolDetailsCacheKey(int connIdx, const QString& poolName) const;
    bool ensureDatasetsLoaded(int connIdx, const QString& poolName, bool allowRemoteLoadIfMissing = true);
    bool ensureDatasetPermissionsLoaded(int connIdx, const QString& poolName, const QString& datasetName);
    bool ensureDatasetPermissionsLoadedBatch(int connIdx, const QString& poolName, const QStringList& datasetNames);
    bool ensurePoolDetailsLoaded(int connIdx, const QString& poolName);
    const PoolDetailsCacheEntry* poolDetailsEntry(int connIdx, const QString& poolName) const;
    void schedulePoolDetailsLoad(int connIdx, const QString& poolName);
    void applyPoolDetailsLoadResult(int connIdx,
                                    const QString& poolName,
                                    bool ok,
                                    const PoolDetailsCacheEntry& fresh,
                                    const QString& errorText);
    bool ensurePoolAutoSnapshotInfoLoaded(int connIdx, const QString& poolName);
    QMap<QString, QMap<QString, QString>> poolAutoSnapshotPropsByDataset(int connIdx, const QString& poolName) const;
    void invalidatePoolAutoSnapshotInfoForConnection(int connIdx);
    void preloadPoolAutoSnapshotInfoForConnection(int connIdx);
    void schedulePoolAutoSnapshotInfoLoad(int connIdx, const QString& poolName);
    void applyPoolAutoSnapshotInfoLoadResult(int connIdx,
                                             const QString& poolName,
                                             bool ok,
                                             const QString& errorText,
                                             const QMap<QString, QMap<QString, QString>>& loaded);
    bool ensureDatasetSnapshotHoldsLoaded(int connIdx, const QString& poolName, const QString& objectName);
    QVector<QPair<QString, QString>> datasetSnapshotHolds(int connIdx, const QString& poolName, const QString& objectName) const;
    void invalidateDatasetPermissionsCacheForPool(int connIdx, const QString& poolName);
    void populateDatasetPermissionsNode(QTreeWidget* tree, QTreeWidgetItem* datasetItem, bool forceReload = false);
    QStringList availableDelegablePermissions(const QString& datasetName,
                                              int connIdx,
                                              const QString& poolName,
                                              const QString& excludeSetName = QString()) const;
    void populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, DatasetTreeContext side, bool allowRemoteLoadIfMissing = true);
    void refreshDatasetProperties(const QString& side);
    void refreshDatasetProperties(const QString& side, QTreeWidget* connContentTree);
    void refreshConnContentPropertiesFor(QTreeWidget* tree);
    void setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName);
    DatasetSelectionContext currentDatasetSelection(const QString& side) const;
    DatasetSelectionContext currentConnContentSelection(const QTreeWidget* tree) const;
    DatasetSelectionContext normalizeDatasetSelectionContext(const DatasetSelectionContext& ctx,
                                                            const QTreeWidget* treeHint = nullptr) const;
    bool executeDatasetAction(const QString& side,
                              const QString& actionName,
                              const DatasetSelectionContext& ctx,
                              const QString& cmd,
                              int timeoutMs = 45000,
                              bool allowWindowsScript = false,
                              const QByteArray& stdinPayload = {},
                              bool invalidatePoolCache = true,
                              const std::function<void()>& onSuccessRefresh = {});
    bool executePendingDatasetRenameDraft(const PendingDatasetRenameDraft& draft,
                                          bool interactiveErrorDialog = true,
                                          QString* failureDetailOut = nullptr);
    bool executePoolCommand(int connIdx,
                            const QString& poolName,
                            const QString& actionName,
                            const QString& remoteCmd,
                            int timeoutMs,
                            QString* failureDetailOut = nullptr,
                            bool refreshPoolsTable = false,
                            bool refreshSelectedPoolDetailsAfter = false);
    bool fetchPoolCommandOutput(int connIdx,
                                const QString& poolName,
                                const QString& actionName,
                                const QString& remoteCmd,
                                QString* outputOut,
                                QString* failureDetailOut = nullptr,
                                int timeoutMs = 45000);
    bool executeConnectionCommand(int connIdx,
                                  const QString& actionName,
                                  const QString& remoteCmd,
                                  int timeoutMs,
                                  QString* failureDetailOut = nullptr,
                                  WindowsCommandMode windowsMode = WindowsCommandMode::Auto);
    bool fetchConnectionCommandOutput(int connIdx,
                                      const QString& actionName,
                                      const QString& remoteCmd,
                                      QString* outputOut,
                                      QString* failureDetailOut = nullptr,
                                      int timeoutMs = 45000,
                                      WindowsCommandMode windowsMode = WindowsCommandMode::Auto);
    bool fetchConnectionProbeOutput(int sourceConnIdx,
                                    const QString& actionName,
                                    const QString& remoteCmd,
                                    QString* mergedOutputOut,
                                    QString* failureDetailOut = nullptr,
                                    int timeoutMs = 12000);
    bool ensureLocalSudoCredentials(ConnectionProfile& profile);
    bool hasEquivalentLocalSshConnection() const;
    QString diagnoseUmountFailure(const DatasetSelectionContext& ctx);
    void invalidateDatasetCacheEntry(int connIdx, const QString& poolName, const QString& objectName, bool invalidatePermissions = true);
    void invalidateDatasetSubtreeCacheEntries(int connIdx,
                                              const QString& poolName,
                                              const QString& datasetName,
                                              bool invalidatePermissions = true);
    void invalidatePoolDatasetListingCache(int connIdx, const QString& poolName);
    void invalidateDatasetCacheForPool(int connIdx, const QString& poolName);
    void invalidatePoolDetailsCacheForConnection(int connIdx);
    bool shouldRefreshSizePropsForCommand(const QString& actionLabel, const QString& command) const;
    bool refreshDatasetAndPoolSizeProperties(int connIdx,
                                             const QString& poolName,
                                             const QString& datasetName);
    void reloadConnContentPool(int connIdx, const QString& poolName);
    void reloadDatasetSide(const QString& side);
    void refreshPendingShellActionDraft(const PendingShellActionDraft& draft);
    void updateConnectionActionsState();
    bool isTransferVersionAllowed(const DatasetSelectionContext& src,
                                  const DatasetSelectionContext& dst,
                                  QString* reasonOut = nullptr) const;
    bool queuePendingShellAction(const PendingShellActionDraft& draft, QString* errorOut = nullptr);
    QString pendingTransferScopeLabel(const DatasetSelectionContext& src, const DatasetSelectionContext& dst) const;
    void executeConnectionTransferAction(const QString& action);
    void setConnectionOriginSelection(const DatasetSelectionContext& ctx);
    void setConnectionDestinationSelection(const DatasetSelectionContext& ctx);
    bool connAdvancedDatasetActionAllowed(const DatasetSelectionContext& ctx) const;
    bool connDirectoryDatasetActionAllowed(const DatasetSelectionContext& ctx) const;
    QString connContentTokenForTree(const QTreeWidget* tree) const;
    void withConnContentContext(QTreeWidget* tree,
                                const QString& token,
                                const std::function<void()>& fn);
    bool runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs = 0, bool forceConfirmDialog = false, bool streamProgress = false);
    void actionCopySnapshot();
    void actionCloneSnapshot();
    void actionDiffSnapshot();
    void actionLevelSnapshot();
    void actionSyncDatasets();
    void actionAdvancedBreakdown();
    void actionAdvancedBreakdown(const DatasetSelectionContext& explicitCtx);
    void actionAdvancedAssemble();
    void actionAdvancedAssemble(const DatasetSelectionContext& explicitCtx);
    void actionAdvancedCreateFromDir();
    void actionAdvancedCreateFromDir(const DatasetSelectionContext& explicitCtx);
    void actionAdvancedToDir();
    void actionAdvancedToDir(const DatasetSelectionContext& explicitCtx);
    bool showInlinePropertyNodesForTree(const QTreeWidget* tree) const;
    bool showInlinePermissionsNodesForTree(const QTreeWidget* tree) const;
    bool showPoolInfoNodeForTree(const QTreeWidget* tree) const;
    bool showInlineGsaNodeForTree(const QTreeWidget* tree) const;
    void setShowInlinePropertyNodesForTree(QTreeWidget* tree, bool visible);
    void setShowInlinePermissionsNodesForTree(QTreeWidget* tree, bool visible);
    void setShowPoolInfoNodeForTree(const QTreeWidget* tree, bool visible);
    void setShowInlineGsaNodeForTree(QTreeWidget* tree, bool visible);
    bool mountDataset(const QString& side, const DatasetSelectionContext& ctx);
    bool umountDataset(const QString& side, const DatasetSelectionContext& ctx);
    void actionCreateChildDataset(const QString& side);
    void actionCreateChildDataset(const QString& side, const DatasetSelectionContext& explicitCtx);
    void actionDeleteDatasetOrSnapshot(const QString& side);
    void actionDeleteDatasetOrSnapshot(const QString& side, const DatasetSelectionContext& explicitCtx);
    bool ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx);
    bool ensureNoMountpointConflictsBeforeMount(const DatasetSelectionContext& ctx, bool includeDescendants);
    void onDatasetPropsCellChanged(int row, int col);
    void applyDatasetPropertyChanges();
    void updateApplyPropsButtonState();
    void clearAllPendingChanges();
    bool removePendingQueuedChangeLine(const QString& line);
    bool executePendingQueuedChangeLine(const QString& line);
    bool executePendingChange(const PendingChange& change);
    void initLogPersistence();
    void rotateLogIfNeeded();
    void appendLogToFile(const QString& line);
    void appendLogToNative(const QString& level, const QString& line);
    void clearAppLog();
    void copyAppLogToClipboard();
    int maxLogLines() const;
    void trimLogWidget(QPlainTextEdit* widget);
    void syncConnectionLogTabs();
    void appendConnectionLog(const QString& connId, const QString& line);
    void refreshConnectionGsaLogAsync(int idx);
    void onAsyncRefreshResult(int generation, int idx, const QString& connId, const ConnectionRuntimeState& state);
    void onAsyncRefreshDone(int generation);
    int findConnectionIndexByName(const QString& name) const;
    bool isConnectionRedirectedToLocal(int idx) const;
    QString connectionPersistKey(int idx) const;
    bool isConnectionDisconnected(int idx) const;
    void setConnectionDisconnected(int idx, bool disconnected);
    void refreshConnectionByIndex(int idx);
    bool installOrUpdateGsaForConnectionInternal(int idx, bool interactive);
    void refreshInstalledGsaAfterConnectionChange(const QString& changedConnectionName);
    struct PoolListEntry {
        QString connection;
        QString pool;
        QString guid;
        QString state;
        QString imported;
        QString reason;
        QString action;
    };
    void exportPoolFromRow(int row);
    void importPoolFromRow(int row);
    void importPoolRenamingFromRow(int row);
    void scrubPoolFromRow(int row);
    void upgradePoolFromRow(int row);
    void reguidPoolFromRow(int row);
    void syncPoolFromRow(int row);
    void trimPoolFromRow(int row);
    void initializePoolFromRow(int row);
    void clearPoolFromRow(int row);
    void destroyPoolFromRow(int row);
    void showPoolHistoryFromRow(int row);
    void createPoolForSelectedConnection();
    void refreshSelectedPoolDetails(bool forceRefresh = false, bool allowRemoteLoadIfMissing = true);
    void refreshPoolStatusNow(int connIdx, const QString& poolName);
    int findPoolRow(const QString& connection, const QString& pool) const;
    int selectedPoolRowFromTabs() const;
    int selectedConnectionIndexForPoolManagement() const;
    void updatePoolManagementBoxTitle();
    void updateStatus(const QString& text);
    void beginUiBusy();
    void endUiBusy();
    void beginTransientUiBusy(const QString& statusText);
    void endTransientUiBusy();
    void updateBusyCursor();
    QString defaultStatusTextForCurrentState() const;
    void updateConnectivityMatrixButtonState();
    void setActionsLocked(bool locked);
    bool actionsLocked() const;
    void requestCancelRunningAction();
    void terminateProcessTree(qint64 rootPid);
    void loadUiSettings();
    void saveUiSettings() const;
    void applyLanguageLive();
    void openHelpTopic(const QString& topicId, const QString& titleOverride = QString());
    QString loadHelpTopicMarkdown(const QString& topicId) const;
    bool selectItemsDialog(const QString& title,
                           const QString& intro,
                           const QStringList& items,
                           QStringList& selected,
                           const QString& detail = QString(),
                           const QMap<QString, QString>& invalidItems = {});
    bool editInlinePropertiesDialog(const QString& title,
                                    const QString& intro,
                                    const QStringList& items,
                                    QStringList& selected,
                                    QVector<InlinePropGroupConfig>& groups,
                                    const QString& initialGroupName = QString());
    bool confirmActionExecution(const QString& actionName, const QStringList& commands, bool forceDialog = false);
    QString buildSshPreviewCommand(const ConnectionProfile& p, const QString& remoteCmd) const;
    QString trk(const QString& key,
                const QString& es = QString(),
                const QString& en = QString(),
                const QString& zh = QString()) const;
    QString maskSecrets(const QString& text) const;
    void logUiAction(const QString& action);
    void appLog(const QString& level, const QString& msg);
    void appendAppLogLineToView(const QString& fullLine);
    void loadPersistedAppLogToView();
    void populateAllPoolsTables();
    void enableSortableHeader(QTableWidget* table);
    void setTablePopulationMode(QTableWidget* table, bool populating);
    QString formatPoolStatusTooltipHtml(const QString& statusText) const;
    QString cachedPoolStatusTooltipHtml(int connIdx, const QString& poolName) const;
    void applyPoolRootTooltipForTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& statusText) const;
    void applyPoolRootTooltipToVisibleTrees(int connIdx, const QString& poolName, const QString& statusText) const;
    void cachePoolStatusTextsForConnection(int connIdx, const ConnectionRuntimeState& state);
    QString connStableIdForIndex(int connIdx) const;
    QString poolStableId(const PoolKey& key) const;
    QString dsStableId(const DSKey& key) const;
    void rebuildConnInfoModel();
    void rebuildConnInfoFor(int connIdx);
    void rebuildPoolInfoFromCache(PoolInfo& poolInfo, int connIdx, const QString& poolName, const PoolInfo* previousPoolInfo = nullptr);
    static DSKind dsKindFromNames(const QString& fullName, const QString& datasetType);
    const ConnInfo* findConnInfo(int connIdx) const;
    ConnInfo* findConnInfo(int connIdx);
    const PoolInfo* findPoolInfo(int connIdx, const QString& poolName) const;
    PoolInfo* findPoolInfo(int connIdx, const QString& poolName);
    const DSInfo* findDsInfo(int connIdx, const QString& poolName, const QString& fullName) const;
    DSInfo* findDsInfo(int connIdx, const QString& poolName, const QString& fullName);
    QStringList datasetSnapshotsFromModel(int connIdx, const QString& poolName, const QString& datasetName) const;
    bool datasetMountedFromModel(int connIdx, const QString& poolName, const QString& datasetName, QString* mountedValueOut = nullptr) const;
    bool datasetExistsInModel(int connIdx, const QString& poolName, const QString& datasetName) const;
    bool ensureObjectGuidLoaded(int connIdx, const QString& poolName, const QString& objectName, QString* guidOut = nullptr);
    QVector<DatasetPropCacheRow> datasetPropertyRowsFromModelOrCache(int connIdx, const QString& poolName, const QString& objectName) const;
    QVector<DatasetPropCacheRow> datasetPropertyRowsForNames(int connIdx,
                                                             const QString& poolName,
                                                             const QString& objectName,
                                                             const QStringList& propNames) const;
    QMap<QString, QString> datasetPropertyValuesForNames(int connIdx,
                                                         const QString& poolName,
                                                         const QString& objectName,
                                                         const QStringList& propNames) const;
    QMap<QString, QString> datasetGsaPropertyValues(int connIdx,
                                                    const QString& poolName,
                                                    const QString& objectName) const;
    bool ensureDatasetAllPropertiesLoaded(int connIdx, const QString& poolName, const QString& objectName);
    bool ensureDatasetPropertySubsetLoaded(int connIdx,
                                           const QString& poolName,
                                           const QString& objectName,
                                           const QStringList& propNames);
    void storeDatasetPropertyRows(int connIdx, const QString& poolName, const QString& objectName, const QString& datasetType, const QVector<DatasetPropCacheRow>& rows);
    void removeDatasetPropertyEntry(int connIdx, const QString& poolName, const QString& objectName);
    void removeDatasetPropertyEntriesForPool(int connIdx, const QString& poolName);
    DatasetPropsDraft propertyDraftForObject(const QString& side, const QString& token, const QString& objectName) const;
    void storePropertyDraftForObject(const QString& side, const QString& token, const QString& objectName, const DatasetPropsDraft& draft);
    QVector<PendingPropertyDraftEntry> pendingConnContentPropertyDraftsFromModel() const;
    const DatasetPermissionsCacheEntry* datasetPermissionsEntry(int connIdx, const QString& poolName, const QString& datasetName) const;
    const DatasetPermissionsCacheEntry* ensureDatasetPermissionsEntryLoaded(int connIdx,
                                                                            const QString& poolName,
                                                                            const QString& datasetName);
    DatasetPermissionsCacheEntry* datasetPermissionsEntryMutable(int connIdx, const QString& poolName, const QString& datasetName);
    void mirrorDatasetPermissionsEntryToModel(int connIdx, const QString& poolName, const QString& datasetName);
    QVector<PendingPermissionDraftEntry> dirtyDatasetPermissionsEntriesFromModel() const;
    void removeDatasetPermissionsEntry(int connIdx, const QString& poolName, const QString& datasetName);
    void removeDatasetPermissionsEntriesForPool(int connIdx, const QString& poolName);
    void resetAllDatasetPermissionDrafts();

    ConnectionStore m_store;
    QVector<ConnectionProfile> m_profiles;
    QVector<ConnectionRuntimeState> m_states;
    QMap<QString, ConnInfo> m_connInfoById;

    QTableWidget* m_connectionsTable{nullptr};
    QAction* m_connectivityMatrixAction{nullptr};
    QTabWidget* m_rightTabs{nullptr};

    QGroupBox* m_poolMgmtBox{nullptr};
    QAction* m_menuExitAction{nullptr};
    QGroupBox* m_connActionsBox{nullptr};
    QLabel* m_connOriginSelectionLabel{nullptr};
    QPushButton* m_btnConnCopy{nullptr};
    QPushButton* m_btnConnClone{nullptr};
    QPushButton* m_btnConnDiff{nullptr};
    QPushButton* m_btnConnLevel{nullptr};
    QPushButton* m_btnConnSync{nullptr};
    DatasetSelectionContext m_connActionOrigin;
    DatasetSelectionContext m_connActionDest;
    bool m_transferSelectionOverrideActive{false};
    DatasetSelectionContext m_transferSelectionOverrideOrigin;
    DatasetSelectionContext m_transferSelectionOverrideDest;

    QVector<PoolListEntry> m_poolListEntries;
    QWidget* m_poolDetailTabs{nullptr};
    bool m_updatingConnectionEntityTabs{false};
    QString m_lastConnectionSelectionKey;
    QWidget* m_connPropsGroup{nullptr};
    QSplitter* m_topMainSplit{nullptr};
    QSplitter* m_rightMainSplit{nullptr};
    QSplitter* m_verticalMainSplit{nullptr};
    QSplitter* m_bottomInfoSplit{nullptr};
    QTableWidget* m_poolPropsTable{nullptr};
    QStackedWidget* m_connPropsStack{nullptr};
    QWidget* m_connPoolPropsPage{nullptr};
    QWidget* m_connContentPage{nullptr};
    ConnectionDatasetTreeWidget* m_topDatasetTreeWidget{nullptr};
    ConnectionDatasetTreePane* m_topDatasetPane{nullptr};
    MainWindowConnectionDatasetTreeDelegate* m_topConnContentDelegate{nullptr};
    ConnectionDatasetTreeCoordinator* m_topConnContentCoordinator{nullptr};
    QTreeWidget* m_connContentTree{nullptr};
    QTableWidget* m_connContentPropsTable{nullptr};
    QString m_connContentToken;
    QMap<QString, QMap<QString, QString>> m_connContentPropValuesByObject;
    QMap<QString, ConnContentTreeState> m_connContentTreeStateByToken;
    bool m_connContentTreeStateWriteLocked{false};
    QSet<QString> m_disconnectedConnectionKeys;
    QByteArray m_mainWindowGeometryState;
    QByteArray m_topMainSplitState;
    QByteArray m_rightMainSplitState;
    QByteArray m_verticalMainSplitState;
    QByteArray m_bottomInfoSplitState;
    QString m_splitTreeLayoutState;
    QMap<int, QSet<QString>> m_savedBottomExpandedKeysByConn;
    QMap<int, QString> m_savedBottomSelectedKeyByConn;
    int m_forceRestoreTopStateConnIdx{-1};
    int m_forceRestoreBottomStateConnIdx{-1};
    QMap<int, QSet<QString>> m_pendingBottomExpandedKeysByConn;
    QMap<int, QString> m_pendingBottomSelectedKeyByConn;
    QString m_userSelectedConnectionKey;
    QString m_persistedTopDetailConnectionKey;
    QString m_persistedBottomDetailConnectionKey;
    int m_topDetailConnIdx{-1};
    int m_bottomDetailConnIdx{-1};
    bool m_connSelectorDefaultsInitialized{false};
    bool m_syncConnSelectorChecks{false};
    bool m_rebuildingTopConnContentTree{false};
    bool m_rebuildingBottomConnContentTree{false};
    ConnectionDatasetTreeWidget* m_bottomDatasetTreeWidget{nullptr};
    ConnectionDatasetTreePane* m_bottomDatasetPane{nullptr};
    MainWindowConnectionDatasetTreeDelegate* m_bottomConnContentDelegate{nullptr};
    ConnectionDatasetTreeCoordinator* m_bottomConnContentCoordinator{nullptr};
    QTreeWidget* m_bottomConnContentTree{nullptr};
    QPlainTextEdit* m_poolStatusText{nullptr};
    QPushButton* m_poolStatusRefreshBtn{nullptr};
    QPushButton* m_poolStatusImportBtn{nullptr};
    QPushButton* m_poolStatusExportBtn{nullptr};
    QPushButton* m_poolStatusScrubBtn{nullptr};
    QPushButton* m_poolStatusDestroyBtn{nullptr};
    QStackedWidget* m_rightStack{nullptr};
    QPushButton* m_btnApplyConnContentProps{nullptr};
    QPushButton* m_btnDiscardPendingChanges{nullptr};
    QPushButton* m_btnConnMove{nullptr};
    QPushButton* m_btnAdvancedBreakdown{nullptr};
    QPushButton* m_btnAdvancedAssemble{nullptr};
    QPushButton* m_btnAdvancedFromDir{nullptr};
    QPushButton* m_btnAdvancedToDir{nullptr};

    QTextEdit* m_statusText{nullptr};
    QTextEdit* m_lastDetailText{nullptr};
    QTabWidget* m_logsTabs{nullptr};
    QPlainTextEdit* m_logView{nullptr};
    QListWidget* m_pendingChangesList{nullptr};
    QMap<QString, PendingItemStatus> m_pendingItemStatus;
    QStringList m_pendingOrderedDisplayLines;
    QTimer* m_pendingSpinnerTimer{nullptr};
    int m_pendingSpinnerFrame{0};
    bool m_pendingApplyInProgress{false};
    bool m_pendingApplyFinishSuppressed{false};
    struct SplitTreeEntry {
        int connIdx{-1};
        QString poolName;
        QString rootDataset;
        QString displayRoot;
        ConnectionDatasetTreeWidget* treeWidget{nullptr};
        MainWindowConnectionDatasetTreeDelegate* delegate{nullptr};
    };
    QList<SplitTreeEntry> m_splitTrees;
    QSplitter* m_connContentTreeSplitter{nullptr};
    QMap<QString, QPointer<QPlainTextEdit>> m_connectionLogViews;
    QMap<QString, QPointer<QPlainTextEdit>> m_connectionGsaLogViews;
    QMap<QString, QPointer<QWidget>> m_connectionLogTabs;
    struct ConnCompactState {
        bool valid{false};
        QString date, time, conn, level;
    };
    QMap<QString, ConnCompactState> m_connCompactState;
    QMap<QString, ConnCompactState> m_connGsaCompactState;
    QSet<QString> m_sshDisableMultiplexKeys;
    QSet<QString> m_loggedSshResolutionKeys;
    mutable QMutex m_sshRuntimeSetsMutex;
    QMap<QString, PoolDatasetCache> m_poolDatasetCache;
    QMap<QString, DatasetPermissionsCacheEntry> m_datasetPermissionsCache;
    QMap<QString, PoolDetailsCacheEntry> m_poolDetailsCache;
    QMap<QString, QStringList> m_connSystemUsersCacheByKey;
    QMap<QString, QStringList> m_connSystemGroupsCacheByKey;
    QSet<QString> m_connSystemUsersLoadedKeys;
    QSet<QString> m_connSystemGroupsLoadedKeys;
    QString m_propsSide;
    QString m_propsDataset;
    QString m_propsToken;
    QMap<QString, QString> m_propsOriginalValues;
    QMap<QString, bool> m_propsOriginalInherit;
    bool m_propsDirty{false};
    QVector<PendingChange> m_pendingChangesModel;
    mutable QMap<QString, int> m_pendingChangeOrderByStableId;
    mutable int m_nextPendingChangeOrder{0};
    bool m_loadingPropsTable{false};
    bool m_loadingDatasetTrees{false};
    QString m_language{QStringLiteral("es")};
    bool m_actionConfirmEnabled{true};
    int m_logMaxSizeMb{10};
    QString m_logLevelSetting{QStringLiteral("normal")};
    int m_logMaxLinesSetting{500};
    bool m_showInlineDatasetProps{true};
    bool m_showInlinePropertyNodesTop{true};
    bool m_showInlinePropertyNodesBottom{true};
    bool m_showInlinePermissionsNodesTop{true};
    bool m_showInlinePermissionsNodesBottom{true};
    bool m_showInlineGsaNodeTop{true};
    bool m_showInlineGsaNodeBottom{true};
    bool m_showPoolInfoNodeTop{true};
    bool m_showPoolInfoNodeBottom{true};
    bool m_showAutomaticGsaSnapshots{true};
    int m_connPropColumnsSetting{7};
    bool m_pendingChangeActivationInProgress{false};
    QStringList m_datasetInlinePropsOrder;
    QVector<InlinePropGroupConfig> m_datasetInlinePropGroups;
    QStringList m_poolInlinePropsOrder;
    QVector<InlinePropGroupConfig> m_poolInlinePropGroups;
    QStringList m_snapshotInlineVisibleProps;
    QVector<InlinePropGroupConfig> m_snapshotInlinePropGroups;
    QString m_appLogPath;
    bool m_compactPrevValid{false};
    QString m_compactPrevDate;
    QString m_compactPrevTime;
    QString m_compactPrevConn;
    QString m_compactPrevLevel;
    int m_refreshGeneration{0};
    int m_refreshPending{0};
    int m_refreshTotal{0};
    bool m_refreshInProgress{false};
    bool m_initialRefreshCompleted{false};
    QString m_localSudoUsername;
    QString m_localSudoPassword;
    QString m_localMachineUuid;
    bool m_startupLocalSudoChecked{false};
    bool m_actionsLocked{false};
    bool m_waitCursorActive{false};
    int m_uiBusyDepth{0};
    bool m_connectivityMatrixInProgress{false};
    bool m_closing{false};
    QStringList m_transientStatusStack;
    bool m_cancelActionRequested{false};
    QProcess* m_activeLocalProcess{nullptr};
    qint64 m_activeLocalPid{-1};
    bool m_busyOnImportRefresh{false};
    QPushButton* m_activeConnActionBtn{nullptr};
    QAction* m_confirmActionsMenuAction{nullptr};
    QString m_activeConnActionName;
    bool m_syncingConnContentColumns{false};
    QSet<QString> m_poolDetailsLoadsInFlight;
    QSet<QString> m_poolAutoSnapshotLoadsInFlight;
};
