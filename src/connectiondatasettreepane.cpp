#include "connectiondatasettreepane.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHeaderView>
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
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        Q_EMIT contextMenuRequested(pos, m_tree ? m_tree->itemAt(pos) : nullptr);
    });
    if (QHeaderView* header = m_tree->header()) {
        connect(header, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            const int logicalIndex = (m_tree && m_tree->header()) ? m_tree->header()->logicalIndexAt(pos) : -1;
            Q_EMIT headerContextMenuRequested(pos, logicalIndex);
        });
    }
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
    QPointer<QTreeWidget> safeTree(m_tree);
    const int vscroll = state.verticalScroll;
    const int hscroll = state.horizontalScroll;
    QTimer::singleShot(0, m_tree, [safeTree, vscroll, hscroll]() {
        if (!safeTree) {
            return;
        }
        if (QScrollBar* scroll = safeTree->verticalScrollBar()) {
            scroll->setValue(vscroll);
        }
        if (QScrollBar* scroll = safeTree->horizontalScrollBar()) {
            scroll->setValue(hscroll);
        }
    });
    QTimer::singleShot(40, m_tree, [safeTree, vscroll, hscroll]() {
        if (!safeTree) {
            return;
        }
        if (QScrollBar* scroll = safeTree->verticalScrollBar()) {
            scroll->setValue(vscroll);
        }
        if (QScrollBar* scroll = safeTree->horizontalScrollBar()) {
            scroll->setValue(hscroll);
        }
    });
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
