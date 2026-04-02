#pragma once

#include "connectiondatasettreecoordinator.h"

#include <functional>

class QAction;
class MainWindow;
class QMenu;

class MainWindowConnectionDatasetTreeDelegate final : public QObject, public ConnectionDatasetTreeDomainAdapter {
    Q_OBJECT
public:
    explicit MainWindowConnectionDatasetTreeDelegate(MainWindow* mainWindow,
                                                     QObject* parent = nullptr);

    void itemClicked(QTreeWidget* tree, QTreeWidgetItem* item) override;
    void selectionChanged(QTreeWidget* tree, bool isBottom) override;
    void itemChanged(QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, int column) override;
    void itemExpanded(QTreeWidget* tree, QTreeWidgetItem* item) override;
    void itemCollapsed(QTreeWidget* tree, QTreeWidgetItem* item) override;
    void beforeContextMenu(QTreeWidget* tree) override;
    void afterContextMenu(QTreeWidget* tree) override;
    bool handleAutoSnapshotsMenu(QTreeWidget* tree, QTreeWidgetItem* item, const QPoint& pos) override;
    bool handlePermissionsMenu(QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, const QPoint& pos) override;
    void showGeneralMenu(QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, const QPoint& pos) override;

private:
    struct SelectionSnapshot {
        bool valid{false};
        int connIdx{-1};
        QString poolName;
        QString datasetName;
        QString snapshotName;
    };

    struct PoolRootMenuActions {
        QAction* update{nullptr};
        QAction* importPool{nullptr};
        QAction* importRename{nullptr};
        QAction* exportPool{nullptr};
        QAction* history{nullptr};
        QAction* sync{nullptr};
        QAction* scrub{nullptr};
        QAction* upgrade{nullptr};
        QAction* reguid{nullptr};
        QAction* trim{nullptr};
        QAction* initialize{nullptr};
        QAction* clear{nullptr};
        QAction* destroy{nullptr};
        QAction* showAutoGsa{nullptr};
    };

    struct InlineVisibilityMenuActions {
        QAction* manage{nullptr};
        QAction* showInlineProps{nullptr};
        QAction* showInlinePerms{nullptr};
        QAction* showInlineGsa{nullptr};
        QAction* showAutoGsa{nullptr};
    };

    QString deleteLabelForItem(int itemConnIdx, const QString& itemPoolName, QTreeWidgetItem* targetItem) const;
    PoolRootMenuActions buildPoolRootMenu(QMenu& menu, QTreeWidget* tree);
    InlineVisibilityMenuActions buildInlineVisibilityMenu(QMenu& menu,
                                                          QTreeWidget* tree,
                                                          bool includeManage,
                                                          bool includePoolInfo,
                                                          bool includeAutoGsa);
    void applyInlineSectionVisibility(QTreeWidget* preferredTree = nullptr, const QString& preferredToken = QString());
    void manageInlinePropsVisualization(QTreeWidget* tree, QTreeWidgetItem* rawItem, bool poolContext);
    void focusContextItemForInlineVisibility(QTreeWidget* tree, QTreeWidgetItem* rawItem);
    QTreeWidgetItem* ownerItemForNode(QTreeWidgetItem* item) const;
    bool isPoolInfoNodeOrInside(QTreeWidgetItem* item) const;
    QString tokenForOwnerItem(QTreeWidgetItem* owner) const;
    QString tokenForNode(QTreeWidgetItem* item) const;
    QString visualStateTokenForTree(QTreeWidget* tree, const QString& token) const;
    SelectionSnapshot currentSelection(QTreeWidget* tree, const QString& token) const;
    void applySelectionToSide(bool isBottom, const SelectionSnapshot& ctx);
    void refreshTreeForTokenAndDataset(QTreeWidget* tree, const QString& token, const QString& datasetName);
    void refreshAllTreesForTokenAndDataset(const QString& token, const QString& datasetName);
    void refreshPermissionsOwnerNode(QTreeWidget* tree, QTreeWidgetItem* owner, bool forceReload);
    void rehydrateExpandedDatasetNodes(QTreeWidget* tree, const QString& token);
    void rebuildAndRestoreDatasetNode(QTreeWidget* tree,
                                      int connIdx,
                                      const QString& poolName,
                                      const QString& datasetName,
                                      const QString& snapshotName,
                                      bool refreshProperties);
    bool executeDatasetActionWithStdin(const QString& side,
                                       const QString& actionName,
                                       int connIdx,
                                       const QString& poolName,
                                       const QString& datasetName,
                                       const QString& snapshotName,
                                       const QString& cmd,
                                       const QByteArray& stdinPayload,
                                       int timeoutMs = 45000);
    bool promptNewPassphrase(const QString& title, QString& passphraseOut);
    void createSnapshotHold(QTreeWidget* tree, QTreeWidgetItem* rawItem);
    void releaseSnapshotHold(QTreeWidget* tree, QTreeWidgetItem* rawItem);

    MainWindow* m_mainWindow{nullptr};
    bool m_contextMenuInProgress{false};
};
