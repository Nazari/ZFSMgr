#pragma once

#include "connectionstore.h"
#include "connectiondialog.h"

#include <QMainWindow>
#include <QMap>
#include <QVector>

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPoint;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString& masterPassword, QWidget* parent = nullptr);

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
        QVector<PoolImported> importedPools;
        QVector<PoolImportable> importablePools;
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
    void onOriginPoolChanged();
    void onDestPoolChanged();
    void onOriginTreeSelectionChanged();
    void onDestTreeSelectionChanged();
    void onOriginTreeItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onDestTreeItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onOriginTreeContextMenuRequested(const QPoint& pos);
    void onDestTreeContextMenuRequested(const QPoint& pos);
    void onConnectionListContextMenuRequested(const QPoint& pos);
    void onImportedPoolsContextMenuRequested(const QPoint& pos);
    void onImportablePoolsContextMenuRequested(const QPoint& pos);

    ConnectionRuntimeState refreshConnection(const ConnectionProfile& p);
    bool runSsh(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs, QString& out, QString& err, int& rc);
    QString withSudo(const ConnectionProfile& p, const QString& cmd) const;
    bool getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut);
    QString datasetCacheKey(int connIdx, const QString& poolName) const;
    bool ensureDatasetsLoaded(int connIdx, const QString& poolName);
    void populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side);
    void refreshDatasetProperties(const QString& side);
    void setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName);
    DatasetSelectionContext currentDatasetSelection(const QString& side) const;
    void showDatasetContextMenu(const QString& side, QTreeWidget* tree, const QPoint& pos);
    bool executeDatasetAction(const QString& side, const QString& actionName, const DatasetSelectionContext& ctx, const QString& cmd, int timeoutMs = 45000);
    void invalidateDatasetCacheForPool(int connIdx, const QString& poolName);
    void reloadDatasetSide(const QString& side);
    void updateTransferButtonsState();
    void refreshTransferSelectionLabels();
    bool runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs = 0);
    void actionCopySnapshot();
    void actionLevelSnapshot();
    void actionSyncDatasets();
    void actionAdvancedBreakdown();
    void actionAdvancedAssemble();
    void actionMountDataset(const QString& side);
    void actionUmountDataset(const QString& side);
    void actionCreateChildDataset(const QString& side);
    void actionDeleteDatasetOrSnapshot(const QString& side);
    void onDatasetPropsCellChanged(int row, int col);
    void applyDatasetPropertyChanges();
    void updateApplyPropsButtonState();
    void initLogPersistence();
    void rotateLogIfNeeded();
    void appendLogToFile(const QString& line);
    void clearAppLog();
    void copyAppLogToClipboard();
    int findConnectionIndexByName(const QString& name) const;
    void refreshConnectionByIndex(int idx);
    void exportPoolFromRow(int row);
    void importPoolFromRow(int row);
    void updateStatus(const QString& text);
    void appLog(const QString& level, const QString& msg);
    void populateAllPoolsTables();

    ConnectionStore m_store;
    QVector<ConnectionProfile> m_profiles;
    QVector<ConnectionRuntimeState> m_states;

    QListWidget* m_connectionsList{nullptr};
    QTabWidget* m_leftTabs{nullptr};
    QTabWidget* m_rightTabs{nullptr};

    QPushButton* m_btnNew{nullptr};
    QPushButton* m_btnRefreshAll{nullptr};
    QPushButton* m_btnRefreshSelected{nullptr};

    QTableWidget* m_importedPoolsTable{nullptr};
    QTableWidget* m_importablePoolsTable{nullptr};
    QStackedWidget* m_rightStack{nullptr};
    QComboBox* m_originPoolCombo{nullptr};
    QComboBox* m_destPoolCombo{nullptr};
    QTreeWidget* m_originTree{nullptr};
    QTreeWidget* m_destTree{nullptr};
    QTableWidget* m_datasetPropsTable{nullptr};
    QPushButton* m_btnApplyDatasetProps{nullptr};
    QLabel* m_transferOriginLabel{nullptr};
    QLabel* m_transferDestLabel{nullptr};
    QPushButton* m_btnCopy{nullptr};
    QPushButton* m_btnLevel{nullptr};
    QPushButton* m_btnSync{nullptr};
    QComboBox* m_advPoolCombo{nullptr};
    QTreeWidget* m_advTree{nullptr};
    QLabel* m_advSelectionLabel{nullptr};
    QPushButton* m_btnAdvancedBreakdown{nullptr};
    QPushButton* m_btnAdvancedAssemble{nullptr};
    QLabel* m_originSelectionLabel{nullptr};
    QLabel* m_destSelectionLabel{nullptr};

    QLabel* m_statusLabel{nullptr};
    QLabel* m_lastSshLineLabel{nullptr};
    QComboBox* m_logLevelCombo{nullptr};
    QPushButton* m_logClearBtn{nullptr};
    QPushButton* m_logCopyBtn{nullptr};
    QPlainTextEdit* m_logView{nullptr};
    QMap<QString, PoolDatasetCache> m_poolDatasetCache;
    QString m_originSelectedDataset;
    QString m_originSelectedSnapshot;
    QString m_destSelectedDataset;
    QString m_destSelectedSnapshot;
    QString m_propsSide;
    QString m_propsDataset;
    QMap<QString, QString> m_propsOriginalValues;
    bool m_propsDirty{false};
    bool m_loadingPropsTable{false};
    QString m_appLogPath;
};
