#pragma once

#include "connectionstore.h"

#include <QMainWindow>
#include <QMap>
#include <QVector>

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

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

    void buildUi();
    void loadConnections();
    void rebuildConnectionList();
    void rebuildDatasetPoolSelectors();
    void refreshAllConnections();
    void refreshSelectedConnection();
    void onConnectionSelectionChanged();
    void onOriginPoolChanged();
    void onDestPoolChanged();
    void onOriginTreeSelectionChanged();
    void onDestTreeSelectionChanged();

    ConnectionRuntimeState refreshConnection(const ConnectionProfile& p);
    bool runSsh(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs, QString& out, QString& err, int& rc);
    QString datasetCacheKey(int connIdx, const QString& poolName) const;
    bool ensureDatasetsLoaded(int connIdx, const QString& poolName);
    void populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side);
    void refreshDatasetProperties(const QString& side);
    void setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName);
    void updateStatus(const QString& text);
    void appLog(const QString& level, const QString& msg);
    void populatePoolsForConnection(int idx);

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
    QComboBox* m_originPoolCombo{nullptr};
    QComboBox* m_destPoolCombo{nullptr};
    QTreeWidget* m_originTree{nullptr};
    QTreeWidget* m_destTree{nullptr};
    QTableWidget* m_datasetPropsTable{nullptr};
    QLabel* m_originSelectionLabel{nullptr};
    QLabel* m_destSelectionLabel{nullptr};

    QLabel* m_statusLabel{nullptr};
    QPlainTextEdit* m_logView{nullptr};
    QMap<QString, PoolDatasetCache> m_poolDatasetCache;
    QString m_originSelectedDataset;
    QString m_originSelectedSnapshot;
    QString m_destSelectedDataset;
    QString m_destSelectedSnapshot;
};
