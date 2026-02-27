#pragma once

#include "connectionstore.h"

#include <QMainWindow>
#include <QVector>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    struct ConnectionRuntimeState {
        QString status;
        QString detail;
    };

    void buildUi();
    void loadConnections();
    void rebuildConnectionList();
    void refreshAllConnections();
    void refreshSelectedConnection();
    void onConnectionSelectionChanged();

    ConnectionRuntimeState refreshConnection(const ConnectionProfile& p);
    void updateStatus(const QString& text);
    void appLog(const QString& level, const QString& msg);
    void populatePoolsForConnection(const ConnectionProfile& p);

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

    QLabel* m_statusLabel{nullptr};
    QPlainTextEdit* m_logView{nullptr};
};
