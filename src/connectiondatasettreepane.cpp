#include "connectiondatasettreepane.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollBar>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString defaultPrimaryTitle(ConnectionDatasetTreePane::Role role) {
    if (role == ConnectionDatasetTreePane::Role::Bottom) {
        return QStringLiteral("Destino:");
    }
    if (role == ConnectionDatasetTreePane::Role::Unified) {
        return QStringLiteral("Conexiones:");
    }
    return QStringLiteral("Origen:");
}
}

ConnectionDatasetTreePane::ConnectionDatasetTreePane(Role role, QWidget* parent)
    : QWidget(parent)
    , m_role(role) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tree = new QTreeWidget(this);
    layout->addWidget(m_tree, 1);

    configureTree();

    connect(m_tree, &QTreeWidget::itemClicked, this, &ConnectionDatasetTreePane::itemClicked);
    connect(m_tree, &QTreeWidget::itemChanged, this, &ConnectionDatasetTreePane::itemChanged);
    connect(m_tree, &QTreeWidget::itemExpanded, this, &ConnectionDatasetTreePane::itemExpanded);
    connect(m_tree, &QTreeWidget::itemCollapsed, this, &ConnectionDatasetTreePane::itemCollapsed);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &ConnectionDatasetTreePane::selectionChanged);
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem*) {
        QPointer<QTreeWidget> tree = m_tree;
        QTimer::singleShot(0, this, [tree]() {
            if (!tree) {
                return;
            }
            for (int col = 0; col < tree->columnCount(); ++col) {
                if (!tree->isColumnHidden(col)) {
                    tree->resizeColumnToContents(col);
                }
            }
        });
    });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        m_contextMenuGestureActive = false;
        Q_EMIT contextMenuRequested(pos, m_tree ? m_tree->itemAt(pos) : nullptr);
    });
    if (m_tree->viewport()) {
        m_tree->viewport()->installEventFilter(this);
    }
    if (QHeaderView* header = m_tree->header()) {
        connect(header, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            const int logicalIndex = (m_tree && m_tree->header()) ? m_tree->header()->logicalIndexAt(pos) : -1;
            Q_EMIT headerContextMenuRequested(pos, logicalIndex);
        });
    }
}

bool ConnectionDatasetTreePane::eventFilter(QObject* watched, QEvent* event) {
    if (m_tree && watched == m_tree->viewport() && event) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me && me->button() == Qt::LeftButton) {
                const QPoint pos = me->position().toPoint();
                const QModelIndex idx = m_tree->indexAt(pos);
                if (idx.isValid() && idx.column() != 0) {
                    QTreeWidgetItem* item = m_tree->itemAt(pos);
                    if (!item || !m_tree->itemWidget(item, idx.column())) {
                        return true;
                    }
                }
            }
            if (me && me->button() == Qt::RightButton) {
                m_contextMenuGestureActive = true;
                const QPoint pos = me->position().toPoint();
                Q_EMIT contextMenuGestureStarted(pos, m_tree->itemAt(pos));
            }
        } else if (event->type() == QEvent::ContextMenu) {
            auto* ce = static_cast<QContextMenuEvent*>(event);
            if (ce && !m_contextMenuGestureActive) {
                const QPoint pos = ce->pos();
                Q_EMIT contextMenuGestureStarted(pos, m_tree->itemAt(pos));
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

ConnectionDatasetTreePane::Role ConnectionDatasetTreePane::role() const {
    return m_role;
}

QTreeWidget* ConnectionDatasetTreePane::tree() const {
    return m_tree;
}

void ConnectionDatasetTreePane::setPrimaryColumnTitle(const QString& title) {
    if (!m_tree || title.trimmed().isEmpty()) {
        return;
    }
    QStringList headers;
    headers.reserve(m_tree->columnCount());
    for (int i = 0; i < m_tree->columnCount(); ++i) {
        headers.push_back(m_tree->headerItem() ? m_tree->headerItem()->text(i) : QString());
    }
    if (headers.isEmpty()) {
        updateHeaders();
        headers = {title, QStringLiteral("Snapshot"), QStringLiteral("Montado"), QStringLiteral("Mountpoint")};
    } else {
        headers[0] = title;
    }
    m_tree->setHeaderLabels(headers);
}

ConnectionDatasetTreePane::VisualOptions ConnectionDatasetTreePane::visualOptions() const {
    return m_visualOptions;
}

void ConnectionDatasetTreePane::setVisualOptions(const VisualOptions& options) {
    m_visualOptions = options;
}

ConnectionDatasetTreePane::VisualState ConnectionDatasetTreePane::captureVisualState() const {
    VisualState state;
    if (!m_tree) {
        return state;
    }
    if (QHeaderView* header = m_tree->header()) {
        state.headerState = header->saveState();
    }
    if (QScrollBar* scroll = m_tree->verticalScrollBar()) {
        state.verticalScroll = scroll->value();
    }
    if (QScrollBar* scroll = m_tree->horizontalScrollBar()) {
        state.horizontalScroll = scroll->value();
    }
    return state;
}

void ConnectionDatasetTreePane::restoreVisualState(const VisualState& state) {
    if (!m_tree) {
        return;
    }
    if (QHeaderView* header = m_tree->header(); header && !state.headerState.isEmpty()) {
        header->restoreState(state.headerState);
    }
    if (QScrollBar* scroll = m_tree->verticalScrollBar()) {
        scroll->setValue(state.verticalScroll);
    }
    if (QScrollBar* scroll = m_tree->horizontalScrollBar()) {
        scroll->setValue(state.horizontalScroll);
    }
}

void ConnectionDatasetTreePane::configureTree() {
    if (!m_tree) {
        return;
    }
    m_tree->setObjectName((m_role == Role::Bottom) ? QStringLiteral("connContentTreeBottom")
                                                   : ((m_role == Role::Unified)
                                                          ? QStringLiteral("connContentTreeUnified")
                                                          : QStringLiteral("connContentTreeTop")));
    m_tree->setColumnCount(4);
    updateHeaders();
    if (QHeaderView* header = m_tree->header()) {
        header->setSectionResizeMode(0, QHeaderView::Interactive);
        header->setSectionResizeMode(1, QHeaderView::Interactive);
        header->setSectionResizeMode(2, QHeaderView::Interactive);
        header->setSectionResizeMode(3, QHeaderView::Interactive);
        header->setStretchLastSection(false);
        header->setFont(QApplication::font());
        header->setContextMenuPolicy(Qt::CustomContextMenu);
    }
    m_tree->setColumnWidth(0, (m_role == Role::Bottom) ? 230 : ((m_role == Role::Unified) ? 280 : 250));
    m_tree->setColumnWidth(1, 90);
    m_tree->setColumnWidth(2, 72);
    m_tree->setColumnWidth(3, (m_role == Role::Bottom) ? 170 : 180);
    m_tree->setColumnHidden(1, true);
    m_tree->setColumnHidden(2, true);
    m_tree->setColumnHidden(3, true);
    m_tree->setFont(QApplication::font());
    m_tree->setUniformRowHeights(false);
    m_tree->setRootIsDecorated(true);
    m_tree->setItemsExpandable(true);
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    if (QScrollBar* vscroll = m_tree->verticalScrollBar()) {
        vscroll->setSingleStep(12);
    }
    m_tree->setStyleSheet(QStringLiteral(
        "QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"
        "QTreeWidget::indicator { width: 8px; height: 8px; margin: 2px; }"));
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_tree->setStyle(fusion);
    }
#endif
}

void ConnectionDatasetTreePane::updateHeaders() {
    if (!m_tree) {
        return;
    }
    m_tree->setHeaderLabels({defaultPrimaryTitle(m_role) + QStringLiteral("Dataset"),
                             QStringLiteral("Snapshot"),
                             QStringLiteral("Montado"),
                             QStringLiteral("Mountpoint")});
}
