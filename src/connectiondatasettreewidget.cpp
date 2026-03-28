#include "connectiondatasettreewidget.h"

#include <QVBoxLayout>

ConnectionDatasetTreeWidget::ConnectionDatasetTreeWidget(const Config& config,
                                                         DomainAdapter* adapter,
                                                         QWidget* parent)
    : QWidget(parent)
    , m_config(config) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_pane = new ConnectionDatasetTreePane(m_config.role, this);
    layout->addWidget(m_pane, 1);

    if (!m_config.treeName.trimmed().isEmpty()) {
        setObjectName(m_config.treeName.trimmed());
    }
    if (!m_config.primaryColumnTitle.trimmed().isEmpty()) {
        m_pane->setPrimaryColumnTitle(m_config.primaryColumnTitle.trimmed());
    }
    m_pane->setVisualOptions(m_config.visualOptions);

    m_coordinator = new ConnectionDatasetTreeCoordinator(m_pane, adapter, this);
}

const ConnectionDatasetTreeWidget::Config& ConnectionDatasetTreeWidget::config() const {
    return m_config;
}

QTreeWidget* ConnectionDatasetTreeWidget::tree() const {
    return m_pane ? m_pane->tree() : nullptr;
}

ConnectionDatasetTreePane* ConnectionDatasetTreeWidget::pane() const {
    return m_pane;
}

ConnectionDatasetTreeCoordinator* ConnectionDatasetTreeWidget::coordinator() const {
    return m_coordinator;
}

void ConnectionDatasetTreeWidget::setPrimaryColumnTitle(const QString& title) {
    m_config.primaryColumnTitle = title;
    if (m_pane) {
        m_pane->setPrimaryColumnTitle(title);
    }
}

void ConnectionDatasetTreeWidget::setVisualOptions(const ConnectionDatasetTreePane::VisualOptions& options) {
    m_config.visualOptions = options;
    if (m_pane) {
        m_pane->setVisualOptions(options);
    }
}

ConnectionDatasetTreePane::VisualOptions ConnectionDatasetTreeWidget::visualOptions() const {
    return m_pane ? m_pane->visualOptions() : m_config.visualOptions;
}
