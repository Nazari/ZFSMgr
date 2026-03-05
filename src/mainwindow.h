#pragma once

#include "connectionstore.h"
#include "connectiondialog.h"

#include <QMainWindow>
#include <QMap>
#include <QVector>

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
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;
class QTextEdit;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString& masterPassword, const QString& language, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

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
        QString osLine;
        QString connectionMethod;
        QString zfsVersionFull;
        QStringList detectedUnixCommands;
        QStringList missingUnixCommands;
        bool unixFromMsysOrMingw{false};
        QStringList powershellFallbackCommands;
        QVector<PoolImported> importedPools;
        QVector<PoolImportable> importablePools;
        QVector<QPair<QString, QString>> mountedDatasets; // dataset, mountpoint
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

    void buildUi();
    void loadConnections();
    void rebuildConnectionList();
    void rebuildDatasetPoolSelectors();
    void refreshAllConnections();
    void refreshSelectedConnection();
    void createConnection();
    void editConnection();
    void deleteConnection();
    void onConnectionSelectionChanged();
    void onPoolsSelectionChanged();
    void onOriginPoolChanged();
    void onDestPoolChanged();
    void onAdvancedPoolChanged();
    void onOriginTreeSelectionChanged();
    void onDestTreeSelectionChanged();
    void onOriginTreeItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onDestTreeItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onOriginTreeContextMenuRequested(const QPoint& pos);
    void onDestTreeContextMenuRequested(const QPoint& pos);
    void onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, const QString& side, const QString& chosen);
    void onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, const QString& side);
    void clearOtherSnapshotSelections(QTreeWidget* tree, QTreeWidgetItem* keepItem);
    void onConnectionListContextMenuRequested(const QPoint& pos);

    ConnectionRuntimeState refreshConnection(const ConnectionProfile& p);
    bool runSsh(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs, QString& out, QString& err, int& rc);
    QString withSudo(const ConnectionProfile& p, const QString& cmd) const;
    QString withSudoStreamInput(const ConnectionProfile& p, const QString& cmd) const;
    bool isWindowsConnection(const ConnectionProfile& p) const;
    bool isWindowsConnection(int connIdx) const;
    QString wrapRemoteCommand(const ConnectionProfile& p, const QString& remoteCmd) const;
    QString sshExecFromLocal(const ConnectionProfile& p, const QString& remoteCmd) const;
    bool getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut);
    QString effectiveMountPath(int connIdx, const QString& poolName, const QString& datasetName, const QString& mountpointHint, const QString& mountedValue);
    QString datasetCacheKey(int connIdx, const QString& poolName) const;
    bool ensureDatasetsLoaded(int connIdx, const QString& poolName);
    void populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side);
    void refreshDatasetProperties(const QString& side);
    void setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName);
    DatasetSelectionContext currentDatasetSelection(const QString& side) const;
    void showDatasetContextMenu(const QString& side, QTreeWidget* tree, const QPoint& pos);
    bool executeDatasetAction(const QString& side, const QString& actionName, const DatasetSelectionContext& ctx, const QString& cmd, int timeoutMs = 45000, bool allowWindowsScript = false);
    QString diagnoseUmountFailure(const DatasetSelectionContext& ctx);
    void invalidateDatasetCacheForPool(int connIdx, const QString& poolName);
    void reloadDatasetSide(const QString& side);
    void updateTransferButtonsState();
    void refreshTransferSelectionLabels();
    void updateAdvancedSelectionUi(const QString& datasetName, const QString& snapshotName);
    bool runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs = 0, bool forceConfirmDialog = false, bool streamProgress = false);
    void actionCopySnapshot();
    void actionLevelSnapshot();
    void actionSyncDatasets();
    void actionAdvancedBreakdown();
    void actionAdvancedAssemble();
    void actionAdvancedCreateFromDir();
    void actionAdvancedToDir();
    void actionMountDataset(const QString& side);
    void actionMountDatasetWithChildren(const QString& side);
    void actionUmountDataset(const QString& side);
    bool mountDataset(const QString& side, const DatasetSelectionContext& ctx);
    bool umountDataset(const QString& side, const DatasetSelectionContext& ctx);
    void actionCreateChildDataset(const QString& side);
    void actionDeleteDatasetOrSnapshot(const QString& side);
    void actionDeleteAllSnapshots(const QString& side);
    bool ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx);
    bool ensureNoMountpointConflictsBeforeMount(const DatasetSelectionContext& ctx, bool includeDescendants);
    void onDatasetPropsCellChanged(int row, int col);
    void applyDatasetPropertyChanges();
    void onAdvancedPropsCellChanged(int row, int col);
    void applyAdvancedDatasetPropertyChanges();
    void updateApplyPropsButtonState();
    void initLogPersistence();
    void rotateLogIfNeeded();
    void appendLogToFile(const QString& line);
    void clearAppLog();
    void copyAppLogToClipboard();
    int maxLogLines() const;
    void trimLogWidget(QPlainTextEdit* widget);
    void syncConnectionLogTabs();
    void appendConnectionLog(const QString& connId, const QString& line);
    void onAsyncRefreshResult(int generation, int idx, const ConnectionRuntimeState& state);
    void onAsyncRefreshDone(int generation);
    int findConnectionIndexByName(const QString& name) const;
    void refreshConnectionByIndex(int idx);
    void exportPoolFromRow(int row);
    void importPoolFromRow(int row);
    void scrubPoolFromRow(int row);
    void destroyPoolFromRow(int row);
    void createPoolForSelectedConnection();
    void refreshSelectedPoolDetails();
    int selectedConnectionIndexForPoolManagement() const;
    void updatePoolManagementBoxTitle();
    void updateStatus(const QString& text);
    void beginUiBusy();
    void endUiBusy();
    void updateBusyCursor();
    void setActionsLocked(bool locked);
    bool actionsLocked() const;
    void requestCancelRunningAction();
    void terminateProcessTree(qint64 rootPid);
    void loadUiSettings();
    void saveUiSettings() const;
    void applyLanguageLive();
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
    void populateAllPoolsTables();
    void populateMountedDatasetsTables();
    void enableSortableHeader(QTableWidget* table);
    void setTablePopulationMode(QTableWidget* table, bool populating);

    ConnectionStore m_store;
    QVector<ConnectionProfile> m_profiles;
    QVector<ConnectionRuntimeState> m_states;

    QTreeWidget* m_connectionsList{nullptr};
    QTabWidget* m_leftTabs{nullptr};
    QTabWidget* m_rightTabs{nullptr};

    QPushButton* m_btnNew{nullptr};
    QPushButton* m_btnRefreshAll{nullptr};
    QGroupBox* m_poolMgmtBox{nullptr};
    QPushButton* m_btnPoolNew{nullptr};

    QTableWidget* m_importedPoolsTable{nullptr};
    QTableWidget* m_importablePoolsTable{nullptr};
    QTabWidget* m_poolDetailTabs{nullptr};
    QTableWidget* m_poolPropsTable{nullptr};
    QPlainTextEdit* m_poolStatusText{nullptr};
    QPushButton* m_poolStatusRefreshBtn{nullptr};
    QPushButton* m_poolStatusImportBtn{nullptr};
    QPushButton* m_poolStatusExportBtn{nullptr};
    QPushButton* m_poolStatusScrubBtn{nullptr};
    QPushButton* m_poolStatusDestroyBtn{nullptr};
    QStackedWidget* m_rightStack{nullptr};
    QComboBox* m_originPoolCombo{nullptr};
    QComboBox* m_destPoolCombo{nullptr};
    QGroupBox* m_transferBox{nullptr};
    QTreeWidget* m_originTree{nullptr};
    QTreeWidget* m_destTree{nullptr};
    QTableWidget* m_datasetPropsTable{nullptr};
    QPushButton* m_btnApplyDatasetProps{nullptr};
    QTableWidget* m_advPropsTable{nullptr};
    QPushButton* m_btnApplyAdvancedProps{nullptr};
    QLabel* m_transferOriginLabel{nullptr};
    QLabel* m_transferDestLabel{nullptr};
    QPushButton* m_btnCopy{nullptr};
    QPushButton* m_btnLevel{nullptr};
    QPushButton* m_btnSync{nullptr};
    QComboBox* m_advPoolCombo{nullptr};
    QTreeWidget* m_advTree{nullptr};
    QGroupBox* m_advCommandsBox{nullptr};
    QLabel* m_advSelectionLabel{nullptr};
    QPushButton* m_btnAdvancedBreakdown{nullptr};
    QPushButton* m_btnAdvancedAssemble{nullptr};
    QPushButton* m_btnAdvancedFromDir{nullptr};
    QPushButton* m_btnAdvancedToDir{nullptr};
    QLabel* m_originSelectionLabel{nullptr};
    QLabel* m_destSelectionLabel{nullptr};
    QTableWidget* m_mountedDatasetsTableLeft{nullptr};
    QTableWidget* m_mountedDatasetsTableAdv{nullptr};

    QTextEdit* m_statusText{nullptr};
    QTextEdit* m_lastDetailText{nullptr};
    QTabWidget* m_logsTabs{nullptr};
    QComboBox* m_logLevelCombo{nullptr};
    QComboBox* m_logMaxLinesCombo{nullptr};
    QPushButton* m_logClearBtn{nullptr};
    QPushButton* m_logCopyBtn{nullptr};
    QPushButton* m_logCancelBtn{nullptr};
    QPlainTextEdit* m_logView{nullptr};
    QMap<QString, QPlainTextEdit*> m_connectionLogViews;
    QMap<QString, PoolDatasetCache> m_poolDatasetCache;
    QString m_originSelectedDataset;
    QString m_originSelectedSnapshot;
    QString m_destSelectedDataset;
    QString m_destSelectedSnapshot;
    QString m_propsSide;
    QString m_propsDataset;
    QMap<QString, QString> m_propsOriginalValues;
    QMap<QString, bool> m_propsOriginalInherit;
    bool m_propsDirty{false};
    QString m_advPropsDataset;
    QMap<QString, QString> m_advPropsOriginalValues;
    QMap<QString, bool> m_advPropsOriginalInherit;
    bool m_advPropsDirty{false};
    bool m_loadingPropsTable{false};
    bool m_loadingDatasetTrees{false};
    QString m_language{QStringLiteral("es")};
    bool m_actionConfirmEnabled{true};
    int m_logMaxSizeMb{10};
    QString m_appLogPath;
    int m_refreshGeneration{0};
    int m_refreshPending{0};
    int m_refreshTotal{0};
    bool m_refreshInProgress{false};
    bool m_syncingConnectionFromPoolSelection{false};
    bool m_actionsLocked{false};
    bool m_waitCursorActive{false};
    int m_uiBusyDepth{0};
    bool m_cancelActionRequested{false};
    QProcess* m_activeLocalProcess{nullptr};
    qint64 m_activeLocalPid{-1};
};
