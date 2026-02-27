#pragma once

#include "connectionstore.h"

#include <QMainWindow>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QTabWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildUi();
    void loadConnections();
    void updateStatus(const QString& text);

    ConnectionStore m_store;
    QListWidget* m_connectionsList{nullptr};
    QTabWidget* m_leftTabs{nullptr};
    QTabWidget* m_rightTabs{nullptr};
    QLabel* m_statusLabel{nullptr};
    QPlainTextEdit* m_logView{nullptr};
};
