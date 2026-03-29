#include "connectiondatasettreecontroller.h"

#include <QTreeWidget>

ConnectionDatasetTreeController::ConnectionDatasetTreeController(ConnectionDatasetTreePane* pane,
                                                                 Callbacks callbacks,
                                                                 QObject* parent)
    : QObject(parent)
    , m_pane(pane)
    , m_callbacks(std::move(callbacks)) {
    wireSignals();
}

ConnectionDatasetTreePane* ConnectionDatasetTreeController::pane() const {
    return m_pane;
}

QTreeWidget* ConnectionDatasetTreeController::tree() const {
    return m_pane ? m_pane->tree() : nullptr;
}

ConnectionDatasetTreePane::Role ConnectionDatasetTreeController::role() const {
    return m_pane ? m_pane->role() : ConnectionDatasetTreePane::Role::Top;
}

void ConnectionDatasetTreeController::wireSignals() {
    if (!m_pane) {
        return;
    }
    connect(m_pane, &ConnectionDatasetTreePane::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (m_callbacks.itemClicked) {
            m_callbacks.itemClicked(item, column);
        }
    });
    connect(m_pane, &ConnectionDatasetTreePane::selectionChanged, this, [this]() {
        if (m_callbacks.selectionChanged) {
            m_callbacks.selectionChanged();
        }
    });
    connect(m_pane, &ConnectionDatasetTreePane::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_callbacks.itemChanged) {
            m_callbacks.itemChanged(item, column);
        }
    });
    connect(m_pane, &ConnectionDatasetTreePane::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (m_callbacks.itemExpanded) {
            m_callbacks.itemExpanded(item);
        }
    });
    connect(m_pane, &ConnectionDatasetTreePane::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (m_callbacks.itemCollapsed) {
            m_callbacks.itemCollapsed(item);
        }
    });
    connect(m_pane, &ConnectionDatasetTreePane::contextMenuGestureStarted, this, [this](const QPoint& pos, QTreeWidgetItem* item) {
        if (m_callbacks.contextMenuGestureStarted) {
            m_callbacks.contextMenuGestureStarted(pos, item);
        }
    });
    connect(m_pane, &ConnectionDatasetTreePane::contextMenuRequested, this, [this](const QPoint& pos, QTreeWidgetItem* item) {
        if (m_callbacks.contextMenuRequested) {
            m_callbacks.contextMenuRequested(pos, item);
        }
    });
}
