#pragma once

#include "connectionstore.h"
#include "connectiondialog.h"

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.10.0rc1"
#endif

#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <functional>

class QComboBox;
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

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    struct InlinePropGroupConfig {
        QString name;
        QStringList props;
    };

    explicit MainWindow(const QString& masterPassword, const QString& language, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct PoolImported {
        QString connection;
        QString pool;
        QString action;
    };

    struct PoolImportable {
        QString connection;
        QString pool;
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
        bool unixFromMsysOrMingw{false};
        QString commandsLayer;
        QStringList powershellFallbackCommands;
        QVector<PoolImported> importedPools;
        QVector<PoolImportable> importablePools;
        QVector<QPair<QString, QString>> mountedDatasets; // dataset, mountpoint
        QMap<QString, QString> poolStatusByName;
        QString gsaScheduler;
        QString gsaVersion;
        QString gsaDetail;
        bool gsaInstalled{false};
        bool gsaActive{false};
    };

    struct DatasetRecord {
        QString name;
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
        QVector<DatasetRecord> datasets;
        QMap<QString, QStringList> snapshotsByDataset;
        QMap<QString, DatasetRecord> recordByName;
        QMap<QString, QString> driveletterByDataset;
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
        bool poolRootExpanded{true};
        bool infoExpanded{false};
        QMap<QString, bool> poolRootExpandedByPool;
        QMap<QString, bool> infoExpandedByPool;
        QString selectedDataset;
        QString selectedSnapshot;
        QMap<QString, QString> snapshotByDataset;
        QMap<QString, QStringList> expandedChildPathsByDataset;
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
        QString scopeLabel;
        QString displayLabel;
        QString command;
        int timeoutMs{0};
        bool streamProgress{true};
        DatasetSelectionContext refreshSource;
        DatasetSelectionContext refreshTarget;
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
    struct DatasetPropsCacheEntry {
        bool loaded{false};
        QString objectName;
        QString datasetType;
        QVector<DatasetPropCacheRow> rows;
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
    void installMsysForSelectedConnection();
    void editConnection();
    void deleteConnection();
    void openConnectivityMatrixDialog();
    void repositionConnectivityButton();
    void onConnectionSelectionChanged();
    void updateSecondaryConnectionDetail();
    void rebuildConnectionEntityTabs();
    void onConnectionEntityTabChanged(int idx);
    void onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, const QString& side, const QString& chosen);
    void onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, const QString& side);
    void clearOtherSnapshotSelections(QTreeWidget* tree, QTreeWidgetItem* keepItem);
    void refreshConnectionNodeDetails();
    void updateConnectionDetailTitlesForCurrentSelection();
    void saveTopTreeStateForConnection(int connIdx);
    void saveBottomTreeStateForConnection(int connIdx);
    void restoreTopTreeStateForConnection(int connIdx);
    void restoreBottomTreeStateForConnection(int connIdx);
    void saveConnContentTreeState(const QString& token);
    void restoreConnContentTreeState(const QString& token);
    QString connectionDisplayModeForIndex(int connIdx) const;
    void syncConnectionDisplaySelectors();
    void applyConnectionDisplayMode(int connIdx, const QString& mode);
    void resizeTreeColumnsToVisibleContent(QTreeWidget* tree);
    void syncConnContentPropertyColumns();
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
    bool detectLocalLibzfs(QString* detail = nullptr) const;
    bool localLibzfsMountDataset(const QString& dataset, QString* detail = nullptr) const;
    bool localLibzfsUnmountDataset(const QString& dataset, QString* detail = nullptr) const;
    bool localLibzfsRenameDataset(const QString& oldName, const QString& newName, QString* detail = nullptr) const;
    bool localLibzfsSetProperty(const QString& dataset, const QString& prop, const QString& value, QString* detail = nullptr) const;
    bool localLibzfsInheritProperty(const QString& dataset, const QString& prop, QString* detail = nullptr) const;
    bool listLocalImportedPoolsLibzfs(QStringList& poolsOut, QString* detail = nullptr) const;
    bool listLocalDatasetsLibzfs(const QString& poolName, PoolDatasetCache& cacheOut, QString* detail = nullptr) const;
    bool getLocalDatasetPropsLibzfs(const QString& objectName,
                                    const QStringList& propNames,
                                    QMap<QString, QString>& valuesOut,
                                    QString* detail = nullptr) const;
    bool isWindowsConnection(const ConnectionProfile& p) const;
    bool isWindowsConnection(int connIdx) const;
    QString wrapRemoteCommand(const ConnectionProfile& p,
                              const QString& remoteCmd,
                              WindowsCommandMode windowsMode = WindowsCommandMode::Auto) const;
    QString sshExecFromLocal(const ConnectionProfile& p,
                             const QString& remoteCmd,
                             WindowsCommandMode windowsMode = WindowsCommandMode::Auto) const;
    bool getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut);
    QString effectiveMountPath(int connIdx, const QString& poolName, const QString& datasetName, const QString& mountpointHint, const QString& mountedValue);
    QString datasetCacheKey(int connIdx, const QString& poolName) const;
    QString datasetPropsCachePrefix(int connIdx, const QString& poolName) const;
    QString datasetPropsCacheKey(int connIdx, const QString& poolName, const QString& objectName) const;
    QString datasetPermissionsCacheKey(int connIdx, const QString& poolName, const QString& datasetName) const;
    QString pendingDatasetRenameCommand(const PendingDatasetRenameDraft& draft) const;
    bool queuePendingDatasetRename(const PendingDatasetRenameDraft& draft, QString* errorOut = nullptr);
    QStringList pendingConnContentApplyCommands() const;
    QStringList pendingConnContentApplyDisplayLines() const;
    void activatePendingChangeAtCursor();
    bool focusPendingChangeLine(const QString& line);
    QString poolDetailsCacheKey(int connIdx, const QString& poolName) const;
    bool ensureDatasetsLoaded(int connIdx, const QString& poolName, bool allowRemoteLoadIfMissing = true);
    bool ensureDatasetPermissionsLoaded(int connIdx, const QString& poolName, const QString& datasetName);
    void invalidateDatasetPermissionsCacheForPool(int connIdx, const QString& poolName);
    void populateDatasetPermissionsNode(QTreeWidget* tree, QTreeWidgetItem* datasetItem, bool forceReload = false);
    QStringList availableDelegablePermissions(const QString& datasetName,
                                              int connIdx,
                                              const QString& poolName,
                                              const QString& excludeSetName = QString()) const;
    void populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side, bool allowRemoteLoadIfMissing = true);
    void refreshDatasetProperties(const QString& side);
    void setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName);
    DatasetSelectionContext currentDatasetSelection(const QString& side) const;
    bool executeDatasetAction(const QString& side, const QString& actionName, const DatasetSelectionContext& ctx, const QString& cmd, int timeoutMs = 45000, bool allowWindowsScript = false, const QByteArray& stdinPayload = {});
    bool ensureLocalSudoCredentials(ConnectionProfile& profile);
    bool hasEquivalentLocalSshConnection() const;
    QString diagnoseUmountFailure(const DatasetSelectionContext& ctx);
    void invalidateDatasetCacheForPool(int connIdx, const QString& poolName);
    void invalidatePoolDetailsCacheForConnection(int connIdx);
    void reloadDatasetSide(const QString& side);
    void updateConnectionActionsState();
    bool isTransferVersionAllowed(const DatasetSelectionContext& src,
                                  const DatasetSelectionContext& dst,
                                  QString* reasonOut = nullptr) const;
    bool queuePendingShellAction(const PendingShellActionDraft& draft, QString* errorOut = nullptr);
    QString pendingTransferScopeLabel(const DatasetSelectionContext& src, const DatasetSelectionContext& dst) const;
    void executeConnectionTransferAction(const QString& action);
    void executeConnectionAdvancedAction(const QString& action);
    void setConnectionOriginSelection(const DatasetSelectionContext& ctx);
    void setConnectionDestinationSelection(const DatasetSelectionContext& ctx);
    bool runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs = 0, bool forceConfirmDialog = false, bool streamProgress = false);
    void actionCopySnapshot();
    void actionCloneSnapshot();
    void actionDiffSnapshot();
    void actionLevelSnapshot();
    void actionSyncDatasets();
    void actionAdvancedBreakdown();
    void actionAdvancedAssemble();
    void actionAdvancedCreateFromDir();
    void actionAdvancedToDir();
    bool mountDataset(const QString& side, const DatasetSelectionContext& ctx);
    bool umountDataset(const QString& side, const DatasetSelectionContext& ctx);
    void actionCreateChildDataset(const QString& side);
    void actionDeleteDatasetOrSnapshot(const QString& side);
    bool ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx);
    bool ensureNoMountpointConflictsBeforeMount(const DatasetSelectionContext& ctx, bool includeDescendants);
    void onDatasetPropsCellChanged(int row, int col);
    void applyDatasetPropertyChanges();
    void updateApplyPropsButtonState();
    void clearAllPendingChanges();
    bool removePendingQueuedChangeLine(const QString& line);
    bool executePendingQueuedChangeLine(const QString& line);
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
    void onAsyncRefreshResult(int generation, int idx, const QString& connId, const ConnectionRuntimeState& state);
    void onAsyncRefreshDone(int generation);
    int findConnectionIndexByName(const QString& name) const;
    bool isConnectionRedirectedToLocal(int idx) const;
    QString connectionPersistKey(int idx) const;
    bool isConnectionDisconnected(int idx) const;
    void setConnectionDisconnected(int idx, bool disconnected);
    void refreshConnectionByIndex(int idx);
    struct PoolListEntry {
        QString connection;
        QString pool;
        QString state;
        QString imported;
        QString reason;
        QString action;
    };
    void exportPoolFromRow(int row);
    void importPoolFromRow(int row);
    void importPoolRenamingFromRow(int row);
    void scrubPoolFromRow(int row);
    void reguidPoolFromRow(int row);
    void syncPoolFromRow(int row);
    void trimPoolFromRow(int row);
    void initializePoolFromRow(int row);
    void destroyPoolFromRow(int row);
    void showPoolHistoryFromRow(int row);
    void createPoolForSelectedConnection();
    void refreshSelectedPoolDetails(bool forceRefresh = false, bool allowRemoteLoadIfMissing = true);
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
    bool selectItemsDialog(const QString& title, const QString& intro, const QStringList& items, QStringList& selected);
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

    ConnectionStore m_store;
    QVector<ConnectionProfile> m_profiles;
    QVector<ConnectionRuntimeState> m_states;

    QTableWidget* m_connectionsTable{nullptr};
    QPushButton* m_connectivityMatrixBtn{nullptr};
    QTabWidget* m_leftTabs{nullptr};
    QTabWidget* m_rightTabs{nullptr};

    QGroupBox* m_poolMgmtBox{nullptr};
    QAction* m_menuExitAction{nullptr};
    QGroupBox* m_connActionsBox{nullptr};
    QPushButton* m_btnConnBreakdown{nullptr};
    QPushButton* m_btnConnAssemble{nullptr};
    QPushButton* m_btnConnFromDir{nullptr};
    QPushButton* m_btnConnToDir{nullptr};
    QLabel* m_connOriginSelectionLabel{nullptr};
    QLabel* m_connDestSelectionLabel{nullptr};
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
    QTabBar* m_connectionEntityTabs{nullptr};
    bool m_updatingConnectionEntityTabs{false};
    QString m_lastConnectionSelectionKey;
    QTabBar* m_poolViewTabBar{nullptr};
    QWidget* m_connPropsGroup{nullptr};
    QWidget* m_connBottomGroup{nullptr};
    QSplitter* m_connDetailSplit{nullptr};
    QList<int> m_connSplitSizesProps;
    QList<int> m_connSplitSizesContent;
    int m_connSplitActiveTab{0};
    QTableWidget* m_poolPropsTable{nullptr};
    QPushButton* m_connPropsRefreshBtn{nullptr};
    QPushButton* m_connPropsEditBtn{nullptr};
    QPushButton* m_connPropsDeleteBtn{nullptr};
    QStackedWidget* m_connPropsStack{nullptr};
    QWidget* m_connPoolPropsPage{nullptr};
    QWidget* m_connContentPage{nullptr};
    QTreeWidget* m_connContentTree{nullptr};
    QTableWidget* m_connContentPropsTable{nullptr};
    QStackedWidget* m_connBottomStack{nullptr};
    QWidget* m_connStatusPage{nullptr};
    QWidget* m_connDatasetPropsPage{nullptr};
    QString m_connContentToken;
    QMap<QString, QMap<QString, QString>> m_connContentPropValuesByObject;
    QMap<QString, ConnContentTreeState> m_connContentTreeStateByToken;
    QSet<QString> m_disconnectedConnectionKeys;
    QMap<int, QString> m_pendingRefreshTopTabDataByConn;
    QMap<int, QString> m_pendingRefreshBottomTabDataByConn;
    QMap<int, QSet<QString>> m_savedTopExpandedKeysByConn;
    QMap<int, QString> m_savedTopSelectedKeyByConn;
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
    QTabBar* m_bottomConnectionEntityTabs{nullptr};
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
    QPlainTextEdit* m_pendingChangesView{nullptr};
    QMap<QString, QPointer<QPlainTextEdit>> m_connectionLogViews;
    QSet<QString> m_sshDisableMultiplexKeys;
    QSet<QString> m_loggedSshResolutionKeys;
    QMap<QString, PoolDatasetCache> m_poolDatasetCache;
    QMap<QString, DatasetPermissionsCacheEntry> m_datasetPermissionsCache;
    QMap<QString, PoolDetailsCacheEntry> m_poolDetailsCache;
    QMap<QString, DatasetPropsCacheEntry> m_datasetPropsCache;
    QString m_propsSide;
    QString m_propsDataset;
    QString m_propsToken;
    QMap<QString, QString> m_propsOriginalValues;
    QMap<QString, bool> m_propsOriginalInherit;
    bool m_propsDirty{false};
    QMap<QString, DatasetPropsDraft> m_propsDraftByKey;
    QVector<PendingDatasetRenameDraft> m_pendingDatasetRenameDrafts;
    QVector<PendingShellActionDraft> m_pendingShellActionDrafts;
    bool m_loadingPropsTable{false};
    bool m_loadingDatasetTrees{false};
    QString m_language{QStringLiteral("es")};
    bool m_actionConfirmEnabled{true};
    int m_logMaxSizeMb{10};
    QString m_logLevelSetting{QStringLiteral("normal")};
    int m_logMaxLinesSetting{500};
    bool m_showInlineDatasetProps{true};
    bool m_showInlinePropertyNodes{true};
    bool m_showInlinePermissionsNodes{true};
    bool m_showInlineGsaNode{true};
    bool m_showPoolInfoNode{true};
    bool m_showAutomaticGsaSnapshots{true};
    int m_connPropColumnsSetting{7};
    bool m_pendingChangeActivationInProgress{false};
    QStringList m_datasetInlinePropsOrder;
    QVector<InlinePropGroupConfig> m_datasetInlinePropGroups;
    QStringList m_poolInlinePropsOrder;
    QVector<InlinePropGroupConfig> m_poolInlinePropGroups;
    QStringList m_snapshotInlinePropsOrder;
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
    mutable bool m_localLibzfsChecked{false};
    mutable bool m_localLibzfsAvailable{false};
    mutable QString m_localLibzfsDetail;
    QString m_localSudoUsername;
    QString m_localSudoPassword;
    QString m_localMachineUuid;
    bool m_startupLocalSudoChecked{false};
    bool m_actionsLocked{false};
    bool m_waitCursorActive{false};
    int m_uiBusyDepth{0};
    bool m_connectivityMatrixInProgress{false};
    QStringList m_transientStatusStack;
    bool m_cancelActionRequested{false};
    QProcess* m_activeLocalProcess{nullptr};
    qint64 m_activeLocalPid{-1};
    bool m_busyOnImportRefresh{false};
    QPushButton* m_activeConnActionBtn{nullptr};
    QAction* m_confirmActionsMenuAction{nullptr};
    QString m_activeConnActionName;
    bool m_syncingConnContentColumns{false};
};
