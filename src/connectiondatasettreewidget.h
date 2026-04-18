#pragma once

#include "connectiondatasettreepane.h"
#include "connectiondatasettreecoordinator.h"

#include <QWidget>

class ConnectionDatasetTreeWidget final : public QWidget {
    Q_OBJECT
public:
    using DomainAdapter = ConnectionDatasetTreeDomainAdapter;

    struct Config {
        QString treeName;
        QString primaryColumnTitle;
        ConnectionDatasetTreePane::Role role{ConnectionDatasetTreePane::Role::Top};
        ConnectionDatasetTreePane::VisualOptions visualOptions;
        bool groupPoolsByConnectionRoots{false};
    };

    explicit ConnectionDatasetTreeWidget(const Config& config,
                                         DomainAdapter* adapter,
                                         QWidget* parent = nullptr);

    const Config& config() const;
    QTreeWidget* tree() const;
    ConnectionDatasetTreePane* pane() const;
    ConnectionDatasetTreeCoordinator* coordinator() const;

    void setPrimaryColumnTitle(const QString& title);
    void setVisualOptions(const ConnectionDatasetTreePane::VisualOptions& options);
    ConnectionDatasetTreePane::VisualOptions visualOptions() const;

private:
    Config m_config;
    ConnectionDatasetTreePane* m_pane{nullptr};
    ConnectionDatasetTreeCoordinator* m_coordinator{nullptr};
};
