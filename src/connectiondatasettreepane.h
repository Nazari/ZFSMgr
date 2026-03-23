#pragma once

#include <QTreeWidget>
#include <QWidget>

class ConnectionDatasetTreePane final : public QWidget {
    Q_OBJECT
public:
    enum class Role {
        Top,
        Bottom
    };

    struct VisualOptions {
        bool showInlineProperties{true};
        bool showInlinePermissions{true};
        bool showInlineGsa{true};
    };

    struct VisualState {
        QByteArray headerState;
        int verticalScroll{0};
        int horizontalScroll{0};
    };

    explicit ConnectionDatasetTreePane(Role role, QWidget* parent = nullptr);

    Role role() const;
    QTreeWidget* tree() const;

    void setPrimaryColumnTitle(const QString& title);
    VisualOptions visualOptions() const;
    void setVisualOptions(const VisualOptions& options);
    VisualState captureVisualState() const;
    void restoreVisualState(const VisualState& state);

Q_SIGNALS:
    void itemClicked(QTreeWidgetItem* item, int column);
    void itemChanged(QTreeWidgetItem* item, int column);
    void itemExpanded(QTreeWidgetItem* item);
    void itemCollapsed(QTreeWidgetItem* item);
    void selectionChanged();
    void contextMenuRequested(const QPoint& pos, QTreeWidgetItem* item);
    void headerContextMenuRequested(const QPoint& pos, int logicalColumn);

private:
    void configureTree();
    void updateHeaders();

    Role m_role;
    QTreeWidget* m_tree{nullptr};
    VisualOptions m_visualOptions;
};
