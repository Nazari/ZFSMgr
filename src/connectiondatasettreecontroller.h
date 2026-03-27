#pragma once

#include "connectiondatasettreepane.h"

#include <QObject>
#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QPoint;

class ConnectionDatasetTreeController final : public QObject {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<void(QTreeWidgetItem* item, int column)> itemClicked;
        std::function<void()> selectionChanged;
        std::function<void(QTreeWidgetItem* item, int column)> itemChanged;
        std::function<void(QTreeWidgetItem* item)> itemExpanded;
        std::function<void(QTreeWidgetItem* item)> itemCollapsed;
        std::function<void(const QPoint& pos, QTreeWidgetItem* item)> contextMenuRequested;
    };

    explicit ConnectionDatasetTreeController(ConnectionDatasetTreePane* pane,
                                             Callbacks callbacks,
                                             QObject* parent = nullptr);

    ConnectionDatasetTreePane* pane() const;
    QTreeWidget* tree() const;
    ConnectionDatasetTreePane::Role role() const;

private:
    void wireSignals();

    ConnectionDatasetTreePane* m_pane{nullptr};
    Callbacks m_callbacks;
};
