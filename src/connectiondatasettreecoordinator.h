#pragma once

#include "connectiondatasettreepane.h"
#include "connectiondatasettreecontroller.h"

#include <QObject>

class QTreeWidget;
class QTreeWidgetItem;
class QPoint;

class ConnectionDatasetTreeDomainAdapter {
public:
    virtual ~ConnectionDatasetTreeDomainAdapter() = default;

    virtual void itemClicked(QTreeWidget* tree, QTreeWidgetItem* item) = 0;
    virtual void selectionChanged(QTreeWidget* tree, bool isBottom) = 0;
    virtual void itemChanged(QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, int column) = 0;
    virtual void itemExpanded(QTreeWidget* tree, QTreeWidgetItem* item) = 0;
    virtual void itemCollapsed(QTreeWidget* tree, QTreeWidgetItem* item) = 0;
    virtual void beforeContextMenu(QTreeWidget* tree) = 0;
    virtual bool handleAutoSnapshotsMenu(QTreeWidget* tree, QTreeWidgetItem* item, const QPoint& pos) = 0;
    virtual bool handlePermissionsMenu(QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, const QPoint& pos) = 0;
    virtual void showGeneralMenu(QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, const QPoint& pos) = 0;
};

using ConnectionDatasetTreeDelegate = ConnectionDatasetTreeDomainAdapter;

class ConnectionDatasetTreeCoordinator final : public QObject {
    Q_OBJECT
public:

    explicit ConnectionDatasetTreeCoordinator(ConnectionDatasetTreePane* pane,
                                              ConnectionDatasetTreeDomainAdapter* delegate,
                                              QObject* parent = nullptr);

    ConnectionDatasetTreePane* pane() const;
    QTreeWidget* tree() const;
    bool isBottom() const;

private:
    void normalizeContextItem(QTreeWidgetItem*& item) const;
    void wireController();

    ConnectionDatasetTreePane* m_pane{nullptr};
    ConnectionDatasetTreeDomainAdapter* m_delegate{nullptr};
    ConnectionDatasetTreeController* m_controller{nullptr};
};
