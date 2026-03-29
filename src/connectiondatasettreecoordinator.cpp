#include "connectiondatasettreecoordinator.h"

#include <QSignalBlocker>
#include <QTreeWidget>

namespace {
constexpr int kConnPropRowRoleCoordinator = Qt::UserRole + 13;
}

ConnectionDatasetTreeCoordinator::ConnectionDatasetTreeCoordinator(ConnectionDatasetTreePane* pane,
                                                                   ConnectionDatasetTreeDomainAdapter* delegate,
                                                                   QObject* parent)
    : QObject(parent)
    , m_pane(pane)
    , m_delegate(delegate) {
    wireController();
}

ConnectionDatasetTreePane* ConnectionDatasetTreeCoordinator::pane() const {
    return m_pane;
}

QTreeWidget* ConnectionDatasetTreeCoordinator::tree() const {
    return m_pane ? m_pane->tree() : nullptr;
}

bool ConnectionDatasetTreeCoordinator::isBottom() const {
    return m_pane && m_pane->role() == ConnectionDatasetTreePane::Role::Bottom;
}

void ConnectionDatasetTreeCoordinator::normalizeContextItem(QTreeWidgetItem*& item) const {
    if (!item) {
        return;
    }
    if (item->data(0, kConnPropRowRoleCoordinator).toBool() && item->parent()) {
        item = item->parent();
    }
    QTreeWidget* tw = tree();
    if (!tw || tw->currentItem() == item) {
        return;
    }
    const QSignalBlocker blocker(tw);
    tw->setCurrentItem(item);
    item = tw->currentItem();
}

void ConnectionDatasetTreeCoordinator::wireController() {
    if (!m_pane) {
        return;
    }
    ConnectionDatasetTreeController::Callbacks callbacks;
    callbacks.itemClicked = [this](QTreeWidgetItem* item, int column) {
        if (m_delegate) {
            m_delegate->itemClicked(tree(), item);
        }
        Q_UNUSED(column);
    };
    callbacks.selectionChanged = [this]() {
        if (m_delegate) {
            m_delegate->selectionChanged(tree(), isBottom());
        }
    };
    callbacks.itemChanged = [this](QTreeWidgetItem* item, int column) {
        if (m_delegate) {
            m_delegate->itemChanged(tree(), isBottom(), item, column);
        }
    };
    callbacks.itemExpanded = [this](QTreeWidgetItem* item) {
        if (m_delegate) {
            m_delegate->itemExpanded(tree(), item);
        }
    };
    callbacks.itemCollapsed = [this](QTreeWidgetItem* item) {
        if (m_delegate) {
            m_delegate->itemCollapsed(tree(), item);
        }
    };
    callbacks.contextMenuGestureStarted = [this](const QPoint&, QTreeWidgetItem* item) {
        if (m_delegate && item) {
            m_delegate->beforeContextMenu(tree());
        }
    };
    callbacks.contextMenuRequested = [this](const QPoint& pos, QTreeWidgetItem* rawItem) {
        if (!rawItem) {
            return;
        }
        QTreeWidgetItem* item = rawItem;
        if (m_delegate && m_delegate->handleAutoSnapshotsMenu(tree(), item, pos)) {
            return;
        }
        normalizeContextItem(item);
        if (!item) {
            return;
        }
        if (m_delegate && m_delegate->handlePermissionsMenu(tree(), isBottom(), item, pos)) {
            return;
        }
        if (m_delegate) {
            m_delegate->showGeneralMenu(tree(), isBottom(), item, pos);
        }
    };
    m_controller = new ConnectionDatasetTreeController(m_pane, callbacks, this);
}
