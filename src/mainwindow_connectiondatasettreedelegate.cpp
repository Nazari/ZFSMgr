#include "mainwindow_connectiondatasettreedelegate.h"

#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "mainwindow_ui_logic.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QTableWidget>
#include <QVBoxLayout>
#include <functional>

namespace {
constexpr int kIsPoolRootRole = Qt::UserRole + 12;
constexpr int kConnPropRowRole = Qt::UserRole + 13;
constexpr int kConnPropRowKindRole = Qt::UserRole + 16;
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kConnPropGroupNodeRole = Qt::UserRole + 17;
constexpr int kConnPropGroupNameRole = Qt::UserRole + 18;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsConnectionRootRole = Qt::UserRole + 36;
constexpr int kConnSnapshotHoldItemRole = Qt::UserRole + 22;
constexpr int kConnSnapshotHoldTagRole = Qt::UserRole + 23;
constexpr int kConnPermissionsNodeRole = Qt::UserRole + 25;
constexpr int kConnPermissionsKindRole = Qt::UserRole + 26;
constexpr int kConnPermissionsScopeRole = Qt::UserRole + 27;
constexpr int kConnPermissionsTargetTypeRole = Qt::UserRole + 28;
constexpr int kConnPermissionsTargetNameRole = Qt::UserRole + 29;
constexpr int kConnPermissionsEntryNameRole = Qt::UserRole + 30;
constexpr int kConnPoolAutoSnapshotsNodeRole = Qt::UserRole + 34;
constexpr int kConnPoolAutoSnapshotsDatasetRole = Qt::UserRole + 35;
constexpr int kConnSnapshotItemRole = Qt::UserRole + 43;
constexpr char kPoolBlockInfoKey[] = "__pool_block_info__";

QString datasetLeafNameUi(const QString& datasetName) {
    const QString trimmed = datasetName.trimmed();
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return (slash >= 0) ? trimmed.mid(slash + 1) : trimmed;
}

QStringList gsaUserPropsForScheduling() {
    return {
        QStringLiteral("org.fc16.gsa:activado"),
        QStringLiteral("org.fc16.gsa:recursivo"),
        QStringLiteral("org.fc16.gsa:horario"),
        QStringLiteral("org.fc16.gsa:diario"),
        QStringLiteral("org.fc16.gsa:semanal"),
        QStringLiteral("org.fc16.gsa:mensual"),
        QStringLiteral("org.fc16.gsa:anual"),
        QStringLiteral("org.fc16.gsa:nivelar"),
        QStringLiteral("org.fc16.gsa:destino"),
    };
}

bool isGsaProp(const QString& prop) {
    for (const QString& p : gsaUserPropsForScheduling()) {
        if (p.compare(prop, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString gsaDefaultValueForScheduling(const QString& prop) {
    const QString p = prop.trimmed();
    if (p.compare(QStringLiteral("org.fc16.gsa:destino"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:horario"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:diario"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:semanal"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:mensual"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:anual"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("0");
    }
    return QStringLiteral("off");
}

bool gsaValueOn(const QString& raw) {
    const QString v = raw.trimmed().toLower();
    return v == QStringLiteral("on")
           || v == QStringLiteral("yes")
           || v == QStringLiteral("true")
           || v == QStringLiteral("1");
}

QString findCaseInsensitivePropKey(const QMap<QString, QString>& values, const QString& wanted) {
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key().compare(wanted, Qt::CaseInsensitive) == 0) {
            return it.key();
        }
    }
    return QString();
}

bool isMainPropertiesNodeLabel(const QString& text) {
    const QString trimmed = text.trimmed();
    return trimmed == QStringLiteral("Properties")
           || trimmed == QStringLiteral("Dataset properties")
           || trimmed == QStringLiteral("Snapshot properties");
}

QString debugConnTreeNodePath(QTreeWidgetItem* item) {
    if (!item) {
        return QStringLiteral("<null>");
    }
    QStringList parts;
    for (QTreeWidgetItem* p = item; p; p = p->parent()) {
        const QString text = p->text(0).trimmed();
        const QString ds = p->data(0, Qt::UserRole).toString().trimmed();
        const QString snap = p->data(1, Qt::UserRole).toString().trimmed();
        const bool isGroup = p->data(0, kConnPropGroupNodeRole).toBool();
        const QString groupName = p->data(0, kConnPropGroupNameRole).toString().trimmed();
        QString part = text.isEmpty() ? QStringLiteral("<empty>") : text;
        if (!ds.isEmpty()) {
            part += QStringLiteral("{ds=%1}").arg(ds);
        }
        if (!snap.isEmpty()) {
            part += QStringLiteral("{snap=%1}").arg(snap);
        }
        if (isGroup) {
            part += QStringLiteral("{group=%1}").arg(groupName.isEmpty() ? QStringLiteral("<main>") : groupName);
        }
        parts.prepend(part);
    }
    return parts.join(QStringLiteral(" / "));
}

int connectionTableRowForIndex(QTableWidget* table, int connIdx) {
    if (!table || connIdx < 0) {
        return -1;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* item = table->item(row, 0);
        if (!item) {
            continue;
        }
        if (item->data(Qt::UserRole).toInt() == connIdx) {
            return row;
        }
    }
    return -1;
}

void logContextMenuPerf(MainWindow* mw,
                        const QString& phase,
                        const QString& extra,
                        qint64 elapsedMs = -1) {
    if (!mw) {
        return;
    }
    QString msg = QStringLiteral("Context menu %1").arg(phase);
    if (elapsedMs >= 0) {
        msg += QStringLiteral(" (%1 ms)").arg(elapsedMs);
    }
    if (!extra.trimmed().isEmpty()) {
        msg += QStringLiteral(": %1").arg(extra.trimmed());
    }
    mw->debugTrace(msg);
}

}

MainWindowConnectionDatasetTreeDelegate::MainWindowConnectionDatasetTreeDelegate(MainWindow* mainWindow,
                                                                                 QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow) {}

QTreeWidgetItem* MainWindowConnectionDatasetTreeDelegate::ownerItemForNode(QTreeWidgetItem* item) const {
    QTreeWidgetItem* owner = item;
    while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
           && !owner->data(0, kIsPoolRootRole).toBool()) {
        owner = owner->parent();
    }
    return owner;
}

bool MainWindowConnectionDatasetTreeDelegate::isPoolInfoNodeOrInside(QTreeWidgetItem* item) const {
    for (QTreeWidgetItem* p = item; p; p = p->parent()) {
        if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
            return true;
        }
    }
    return false;
}

QString MainWindowConnectionDatasetTreeDelegate::tokenForOwnerItem(QTreeWidgetItem* owner) const {
    if (!m_mainWindow || !owner) {
        return QString();
    }
    const int connIdx = owner->data(0, kConnIdxRole).toInt();
    const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
    if (connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size() || poolName.isEmpty()) {
        return QString();
    }
    return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
}

QString MainWindowConnectionDatasetTreeDelegate::tokenForNode(QTreeWidgetItem* item) const {
    return tokenForOwnerItem(ownerItemForNode(item));
}

QString MainWindowConnectionDatasetTreeDelegate::visualStateTokenForTree(QTreeWidget* tree, const QString& token) const {
    if (!tree) {
        return QString();
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return token.trimmed();
    }
    bool ok = false;
    const int connIdx = token.left(sep).toInt(&ok);
    if (!ok || connIdx < 0) {
        return token.trimmed();
    }
    if (tree == m_mainWindow->m_connContentTree) {
        return QStringLiteral("conn:%1").arg(connIdx);
    }
    return token.trimmed();
}

MainWindowConnectionDatasetTreeDelegate::SelectionSnapshot
MainWindowConnectionDatasetTreeDelegate::currentSelection(QTreeWidget* tree, const QString& token) const {
    SelectionSnapshot snapshot;
    if (!m_mainWindow || !tree || token.isEmpty()) {
        return snapshot;
    }
    const MainWindow::DatasetSelectionContext ctx =
        m_mainWindow->currentConnContentSelection(tree);
    snapshot.valid = ctx.valid;
    snapshot.connIdx = ctx.connIdx;
    snapshot.poolName = ctx.poolName;
    snapshot.datasetName = ctx.datasetName;
    snapshot.snapshotName = ctx.snapshotName;
    return snapshot;
}

void MainWindowConnectionDatasetTreeDelegate::applySelectionToSide(bool isBottom,
                                                                   const SelectionSnapshot& ctx) {
    if (!m_mainWindow) {
        return;
    }
    MainWindow::DatasetSelectionContext mwCtx;
    mwCtx.valid = ctx.valid;
    mwCtx.connIdx = ctx.connIdx;
    mwCtx.poolName = ctx.poolName;
    mwCtx.datasetName = ctx.datasetName;
    mwCtx.snapshotName = ctx.snapshotName;
    if (isBottom) {
        if (mwCtx.valid && !mwCtx.datasetName.isEmpty()) {
            m_mainWindow->setConnectionDestinationSelection(mwCtx);
        } else {
            m_mainWindow->setConnectionDestinationSelection(MainWindow::DatasetSelectionContext{});
        }
    } else {
        if (mwCtx.valid && !mwCtx.datasetName.isEmpty()) {
            m_mainWindow->setConnectionOriginSelection(mwCtx);
        } else {
            m_mainWindow->setConnectionOriginSelection(MainWindow::DatasetSelectionContext{});
        }
    }
}

void MainWindowConnectionDatasetTreeDelegate::refreshTreeForTokenAndDataset(QTreeWidget* tree,
                                                                            const QString& token,
                                                                            const QString& datasetName) {
    if (!m_mainWindow || !tree || token.isEmpty()) {
        return;
    }
    m_mainWindow->syncConnContentPoolColumnsFor(tree, token);
    QTreeWidgetItem* owner = ownerItemForNode(tree->currentItem());
    if (owner && owner->data(0, Qt::UserRole).toString().trimmed() == datasetName) {
        m_mainWindow->refreshConnContentPropertiesFor(tree);
        m_mainWindow->syncConnContentPropertyColumnsFor(tree, token);
    }
}

void MainWindowConnectionDatasetTreeDelegate::refreshAllTreesForTokenAndDataset(const QString& token,
                                                                                const QString& datasetName) {
    refreshTreeForTokenAndDataset(m_mainWindow ? m_mainWindow->m_connContentTree : nullptr, token, datasetName);
}

void MainWindowConnectionDatasetTreeDelegate::refreshPermissionsOwnerNode(QTreeWidget* tree,
                                                                          QTreeWidgetItem* owner,
                                                                          bool forceReload) {
    if (!m_mainWindow || !tree || !owner) {
        return;
    }
    m_mainWindow->populateDatasetPermissionsNode(tree, owner, forceReload);
}

void MainWindowConnectionDatasetTreeDelegate::rehydrateExpandedDatasetNodes(QTreeWidget* tree,
                                                                            const QString& token) {
    if (!m_mainWindow || !tree || token.trimmed().isEmpty()) {
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    bool ok = false;
    const int connIdx = token.left(sep).toInt(&ok);
    if (!ok || connIdx < 0) {
        return;
    }
    const QString scopedToken =
        visualStateTokenForTree(tree, token) + QStringLiteral("|top");
    const auto it = m_mainWindow->m_connContentTreeStateByToken.constFind(scopedToken);
    if (it == m_mainWindow->m_connContentTreeStateByToken.cend()) {
        return;
    }
    const MainWindow::ConnContentTreeState& st = it.value();
    const QSignalBlocker blocker(tree);
    auto pathHasSegmentPrefix = [](const QString& path, const QString& prefix) {
        const QString p = path.trimmed();
        if (p.isEmpty()) {
            return false;
        }
        if (p.startsWith(prefix)) {
            return true;
        }
        const QString needle = QStringLiteral("/%1").arg(prefix);
        return p.contains(needle);
    };
    auto applyStoredSnapshotToItem = [tree, &st](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }
        const QString datasetName = item->data(0, Qt::UserRole).toString().trimmed();
        if (datasetName.isEmpty()) {
            return;
        }
        const QString snapshotName = st.snapshotByDataset.value(datasetName).trimmed();
        if (snapshotName.isEmpty()) {
            item->setData(1, Qt::UserRole, QString());
            return;
        }
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(item, 1))) {
            const int idx = cb->findText(snapshotName);
            if (idx > 0) {
                const QSignalBlocker comboBlocker(cb);
                cb->setCurrentIndex(idx);
            }
        }
        item->setData(1, Qt::UserRole, snapshotName);
    };
    auto findDatasetItemInTree = [tree](int wantedConnIdx, const QString& wantedDatasetName) -> QTreeWidgetItem* {
        if (!tree || wantedDatasetName.trimmed().isEmpty()) {
            return nullptr;
        }
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
            if (!n) {
                return nullptr;
            }
            if (n->data(0, kConnIdxRole).toInt() == wantedConnIdx
                && n->data(0, Qt::UserRole).toString().trimmed() == wantedDatasetName) {
                return n;
            }
            for (int i = 0; i < n->childCount(); ++i) {
                if (QTreeWidgetItem* found = rec(n->child(i))) {
                    return found;
                }
            }
            return nullptr;
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (QTreeWidgetItem* found = rec(tree->topLevelItem(i))) {
                return found;
            }
        }
        return nullptr;
    };
    for (auto childIt = st.expandedChildPathsByDataset.cbegin(); childIt != st.expandedChildPathsByDataset.cend(); ++childIt) {
        const QString datasetName = childIt.key().trimmed();
        if (datasetName.isEmpty()) {
            continue;
        }
        const QStringList paths = childIt.value();
        bool needsProps = false;
        bool needsPerms = false;
        for (const QString& path : paths) {
            if (pathHasSegmentPrefix(path, QStringLiteral("group|"))
                || pathHasSegmentPrefix(path, QStringLiteral("gsa|"))
                || pathHasSegmentPrefix(path, QStringLiteral("holds|"))
                || pathHasSegmentPrefix(path, QStringLiteral("hold|"))) {
                needsProps = true;
            }
            if (pathHasSegmentPrefix(path, QStringLiteral("perm|"))) {
                needsPerms = true;
            }
        }
        if (!needsProps && !needsPerms) {
            continue;
        }
        QTreeWidgetItem* owner = findDatasetItemInTree(connIdx, datasetName);
        if (!owner) {
            continue;
        }
        applyStoredSnapshotToItem(owner);
        tree->setCurrentItem(owner);
        if (needsProps) {
            m_mainWindow->refreshConnContentPropertiesFor(tree);
            m_mainWindow->syncConnContentPropertyColumnsFor(tree, token);
        }
        if (needsPerms) {
            refreshPermissionsOwnerNode(tree, owner, false);
        }
    }
}

void MainWindowConnectionDatasetTreeDelegate::rebuildAndRestoreDatasetNode(QTreeWidget* tree,
                                                                           int connIdx,
                                                                           const QString& poolName,
                                                                           const QString& datasetName,
                                                                           const QString& snapshotName,
                                                                           bool refreshProperties) {
    if (!m_mainWindow || !tree || datasetName.isEmpty() || poolName.isEmpty() || connIdx < 0
        || connIdx >= m_mainWindow->m_profiles.size()) {
        return;
    }
    const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    m_mainWindow->rebuildConnContentTreeFor(tree, token, connIdx, poolName, false);
    if (QTreeWidgetItem* restored = m_mainWindow->findConnContentDatasetItemFor(tree, connIdx, poolName, datasetName)) {
        if (!snapshotName.isEmpty()) {
            restored->setData(1, Qt::UserRole, snapshotName);
        }
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(restored, 1))) {
            const int idx = cb->findText(snapshotName);
            if (idx > 0) {
                const QSignalBlocker blocker(cb);
                cb->setCurrentIndex(idx);
            }
        }
        tree->setCurrentItem(restored);
        if (refreshProperties) {
            m_mainWindow->refreshConnContentPropertiesFor(tree);
            m_mainWindow->syncConnContentPropertyColumnsFor(tree, token);
        }
    }
}

QString MainWindowConnectionDatasetTreeDelegate::deleteLabelForItem(int itemConnIdx,
                                                                    const QString& itemPoolName,
                                                                    QTreeWidgetItem* targetItem) const {
    if (!m_mainWindow) {
        return QStringLiteral("Borrar Dataset");
    }
    QTreeWidgetItem* owner = targetItem;
    while (owner) {
        const QString ownerDatasetName = owner->data(0, Qt::UserRole).toString().trimmed();
        const QString ownerSnapshotName = owner->data(1, Qt::UserRole).toString().trimmed();
        if (!ownerDatasetName.isEmpty() || !ownerSnapshotName.isEmpty()
            || owner->data(0, kIsPoolRootRole).toBool()) {
            break;
        }
        owner = owner->parent();
    }
    const QString snapshotName = owner ? owner->data(1, Qt::UserRole).toString().trimmed() : QString();
    const QString datasetName = owner ? owner->data(0, Qt::UserRole).toString().trimmed() : QString();
    if (!snapshotName.isEmpty() && !datasetName.isEmpty()) {
        return QStringLiteral("%1 %2@%3").arg(
            m_mainWindow->trk(QStringLiteral("t_ctx_delete_snapshot001"),
                              QStringLiteral("Borrar Snapshot")),
            datasetName,
            snapshotName);
    }
    if (!datasetName.isEmpty()) {
        const auto it = m_mainWindow->m_poolDatasetCache.constFind(m_mainWindow->datasetCacheKey(itemConnIdx, itemPoolName));
        if (it != m_mainWindow->m_poolDatasetCache.cend()) {
            const auto recIt = it->recordByName.constFind(datasetName);
            if (recIt != it->recordByName.cend()) {
                const MainWindow::DatasetRecord& rec = recIt.value();
                if (rec.mounted.trimmed() == QStringLiteral("-")
                    && rec.mountpoint.trimmed() == QStringLiteral("-")) {
                    return QStringLiteral("%1 %2").arg(
                        m_mainWindow->trk(QStringLiteral("t_ctx_delete_zvol001"),
                                          QStringLiteral("Borrar ZVol")),
                        datasetName);
                }
            }
        }
        return QStringLiteral("%1 %2").arg(
            m_mainWindow->trk(QStringLiteral("t_ctx_delete_dataset001"),
                              QStringLiteral("Borrar Dataset")),
            datasetName);
    }
    return m_mainWindow->trk(QStringLiteral("t_ctx_delete_dataset001"),
                             QStringLiteral("Borrar Dataset"));
}

MainWindowConnectionDatasetTreeDelegate::PoolRootMenuActions
MainWindowConnectionDatasetTreeDelegate::buildPoolRootMenu(QMenu& menu, QTreeWidget* tree) {
    PoolRootMenuActions actions;
    if (!m_mainWindow) {
        return actions;
    }
    Q_UNUSED(tree);
    actions.update = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_pool_refresh_status001"),
                          QStringLiteral("Actualizar estado"),
                          QStringLiteral("Refresh status"),
                          QStringLiteral("刷新状态")));
    actions.importPool = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_import_btn001"),
                          QStringLiteral("Importar"),
                          QStringLiteral("Import"),
                          QStringLiteral("导入")));
    actions.importRename = menu.addAction(QStringLiteral("Importar renombrando"));
    actions.exportPool = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_export_btn001"),
                          QStringLiteral("Exportar"),
                          QStringLiteral("Export"),
                          QStringLiteral("导出")));
    actions.history = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_pool_history_t1"),
                          QStringLiteral("Historial")));
    QMenu* management = menu.addMenu(QStringLiteral("Gestión"));
    actions.sync = management->addAction(QStringLiteral("Sync"));
    actions.scrub = management->addAction(QStringLiteral("Scrub"));
    actions.upgrade = management->addAction(QStringLiteral("Upgrade"));
    actions.reguid = management->addAction(QStringLiteral("Reguid"));
    actions.trim = management->addAction(QStringLiteral("Trim"));
    actions.initialize = management->addAction(QStringLiteral("Initialize"));
    actions.clear = management->addAction(QStringLiteral("Clear"));
    actions.destroy = management->addAction(QStringLiteral("Destroy"));
    return actions;
}

MainWindowConnectionDatasetTreeDelegate::InlineVisibilityMenuActions
MainWindowConnectionDatasetTreeDelegate::buildInlineVisibilityMenu(QMenu& menu,
                                                                   QTreeWidget* tree,
                                                                   bool includeManage,
                                                                   bool includePoolInfo,
                                                                   bool includeAutoGsa) {
    InlineVisibilityMenuActions actions;
    if (!m_mainWindow) {
        return actions;
    }
    if (includeManage) {
        actions.manage = menu.addAction(
            m_mainWindow->trk(QStringLiteral("t_manage_props_vis001"),
                              QStringLiteral("Gestionar visualización de propiedades")));
    }
    Q_UNUSED(includePoolInfo);
    Q_UNUSED(tree);
    Q_UNUSED(includeAutoGsa);
    return actions;
}

void MainWindowConnectionDatasetTreeDelegate::applyInlineSectionVisibility(QTreeWidget* preferredTree,
                                                                           const QString& preferredToken) {
    if (!m_mainWindow) {
        return;
    }
    ++m_selectionRefreshEpoch;
    m_mainWindow->appLog(QStringLiteral("DEBUG"),
                         QStringLiteral("inline.visibility begin epoch=%1 token=%2")
                             .arg(QString::number(m_selectionRefreshEpoch),
                                  preferredToken.trimmed()));
    QScopedValueRollback<bool> suppressSelectionGuard(m_suppressSelectionRefresh, true);
    auto connTokenFromTreeSelectionBottom = [this](QTreeWidget* tree) -> QString {
        if (!tree) {
            return QString();
        }
        QTreeWidgetItem* owner = tree->currentItem();
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, kIsPoolRootRole).toBool()) {
            owner = owner->parent();
        }
        if (!owner) {
            return QString();
        }
        const int connIdx = owner->data(0, kConnIdxRole).toInt();
        const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
        if (connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size() || poolName.isEmpty()) {
            return QString();
        }
        return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    };
    auto refreshInlinePropsVisualBottom = [this, &connTokenFromTreeSelectionBottom](QTreeWidget* tree, const QString& explicitToken) {
        if (!tree) {
            return;
        }
        {
            QSignalBlocker blocker(tree);
            QTreeWidgetItem* cur = tree->currentItem();
            if (cur && cur->data(0, kConnPropRowRole).toBool() && cur->parent()) {
                tree->setCurrentItem(cur->parent());
            }
        }
        const QString token = explicitToken.trimmed().isEmpty() ? connTokenFromTreeSelectionBottom(tree)
                                                                : explicitToken.trimmed();
        if (token.isEmpty()) {
            return;
        }
        QSignalBlocker blocker(tree);
        m_mainWindow->syncConnContentPropertyColumnsFor(tree, token);
    };
    auto rebuildInlineConnTree = [this](QTreeWidget* tree, const QString& token) {
        if (!tree || token.trimmed().isEmpty()) {
            return;
        }
        if (tree == m_mainWindow->m_connContentTree) {
            m_mainWindow->rebuildConnectionEntityTabs();
            return;
        }
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep <= 0) {
            return;
        }
        bool ok = false;
        const int connIdx = token.left(sep).toInt(&ok);
        const QString poolName = token.mid(sep + 2).trimmed();
        if (!ok || connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size() || poolName.isEmpty()) {
            return;
        }
        m_mainWindow->rebuildConnContentTreeFor(tree, token, connIdx, poolName, true);
    };
    auto alignDetailContextToToken = [this](QTreeWidget* tree, const QString& token) {
        if (!tree) {
            return;
        }
        const QString trimmed = token.trimmed();
        int connIdx = -1;
        bool ok = false;
        const int sep = trimmed.indexOf(QStringLiteral("::"));
        if (sep > 0) {
            connIdx = trimmed.left(sep).toInt(&ok);
        } else if (trimmed.startsWith(QStringLiteral("conn:"))) {
            const QString rest = trimmed.mid(QStringLiteral("conn:").size());
            const int pipe = rest.indexOf(QLatin1Char('|'));
            connIdx = (pipe >= 0 ? rest.left(pipe) : rest).toInt(&ok);
        }
        if (!ok || connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size()) {
            return;
        }
        if (tree == m_mainWindow->m_connContentTree) {
            m_mainWindow->m_topDetailConnIdx = connIdx;
        }
    };
    auto tokenFromTree = [this](QTreeWidget* tree) -> QString {
        return m_mainWindow ? m_mainWindow->connContentTokenForTree(tree) : QString();
    };
    auto refreshExplicitTreeFromToken = [this, &refreshInlinePropsVisualBottom, &rebuildInlineConnTree, &alignDetailContextToToken](QTreeWidget* tree,
                                                                                const QString& token) {
        if (!tree) {
            return;
        }
        const QString scopedToken =
            visualStateTokenForTree(tree, token) + QStringLiteral("|top");
        auto rematerializeVisiblePropertyNodes = [this](QTreeWidget* targetTree, const QString& targetToken) {
            if (!m_mainWindow || !targetTree || targetToken.trimmed().isEmpty()) {
                return;
            }
            QTreeWidgetItem* originalCurrent = targetTree->currentItem();
            std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* item) {
                if (!item) {
                    return;
                }
                const QString datasetName = item->data(0, Qt::UserRole).toString().trimmed();
                if (!datasetName.isEmpty()) {
                    for (int i = 0; i < item->childCount(); ++i) {
                        QTreeWidgetItem* child = item->child(i);
                        if (!child || !child->data(0, kConnPropGroupNodeRole).toBool()) {
                            continue;
                        }
                        const bool isMainPropsNode =
                            child->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                            && isMainPropertiesNodeLabel(child->text(0));
                        if (!isMainPropsNode || !child->isExpanded()) {
                            continue;
                        }
                        {
                            const QSignalBlocker blocker(targetTree);
                            targetTree->setCurrentItem(item);
                        }
                        m_mainWindow->refreshConnContentPropertiesFor(targetTree);
                        m_mainWindow->syncConnContentPropertyColumnsFor(targetTree, targetToken);
                        break;
                    }
                }
                for (int i = 0; i < item->childCount(); ++i) {
                    rec(item->child(i));
                }
            };
            for (int i = 0; i < targetTree->topLevelItemCount(); ++i) {
                rec(targetTree->topLevelItem(i));
            }
            if (originalCurrent) {
                const QSignalBlocker blocker(targetTree);
                targetTree->setCurrentItem(originalCurrent);
            }
        };
        alignDetailContextToToken(tree, token);
        const auto stateItSaved = m_mainWindow->m_connContentTreeStateByToken.constFind(scopedToken);
        const MainWindow::ConnContentTreeState preservedState =
            (stateItSaved != m_mainWindow->m_connContentTreeStateByToken.cend())
                ? stateItSaved.value()
                : MainWindow::ConnContentTreeState{};
        QScopedValueRollback<bool> freezeStateWrites(m_mainWindow->m_connContentTreeStateWriteLocked, true);
        rebuildInlineConnTree(tree, token);
        m_mainWindow->syncConnContentPoolColumnsFor(tree, token);
        rehydrateExpandedDatasetNodes(tree, token);
        refreshInlinePropsVisualBottom(tree, token);
        m_mainWindow->syncConnContentPoolColumnsFor(tree, token);
        rehydrateExpandedDatasetNodes(tree, token);
        rematerializeVisiblePropertyNodes(tree, token);
        if (!preservedState.selectedNodePath.trimmed().isEmpty() || !preservedState.expandedNodePaths.isEmpty()) {
            m_mainWindow->m_connContentTreeStateByToken.insert(scopedToken, preservedState);
        }
        m_mainWindow->restoreConnContentTreeStateFor(tree, token);
    };

    m_mainWindow->saveUiSettings();
    const bool onlyPreferredTree = (preferredTree && !preferredToken.trimmed().isEmpty());
    if (onlyPreferredTree) {
        refreshExplicitTreeFromToken(preferredTree, preferredToken.trimmed());
        m_mainWindow->appLog(QStringLiteral("DEBUG"),
                             QStringLiteral("inline.visibility end epoch=%1 mode=preferred")
                                 .arg(QString::number(m_selectionRefreshEpoch)));
        return;
    }

    if (m_mainWindow->m_connContentTree) {
        const QString token = (preferredTree == m_mainWindow->m_connContentTree && !preferredToken.trimmed().isEmpty())
                                  ? preferredToken.trimmed()
                                  : tokenFromTree(m_mainWindow->m_connContentTree);
        if (!token.isEmpty()) {
            alignDetailContextToToken(m_mainWindow->m_connContentTree, token);
            refreshExplicitTreeFromToken(m_mainWindow->m_connContentTree, token);
        }
    }
    m_mainWindow->appLog(QStringLiteral("DEBUG"),
                         QStringLiteral("inline.visibility end epoch=%1 mode=default")
                             .arg(QString::number(m_selectionRefreshEpoch)));
}

void MainWindowConnectionDatasetTreeDelegate::manageInlinePropsVisualization(QTreeWidget* tree,
                                                                             QTreeWidgetItem* rawItem,
                                                                             bool poolContext) {
    if (!m_mainWindow || !tree || !rawItem) {
        return;
    }
    QTreeWidgetItem* item = rawItem;
    if (item->data(0, kConnPropRowRole).toBool() && item->parent()) {
        item = item->parent();
    }
    const QString clickedGroupName = item->data(0, kConnPropGroupNameRole).toString().trimmed();
    QTreeWidgetItem* owner = item;
    while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
           && !owner->data(0, kIsPoolRootRole).toBool()) {
        owner = owner->parent();
    }
    if (!owner) {
        return;
    }
    const int connIdx = owner->data(0, kConnIdxRole).toInt();
    const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
    if (connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size() || poolName.isEmpty()) {
        return;
    }
    const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    auto normalizeList = [](const QStringList& in) {
        QStringList out;
        QSet<QString> seen;
        for (const QString& raw : in) {
            const QString t = raw.trimmed();
            const QString k = t.toLower();
            if (t.isEmpty() || seen.contains(k)) {
                continue;
            }
            seen.insert(k);
            out.push_back(t);
        }
        return out;
    };
    auto displayedPropsFromNode = [tree, &normalizeList](QTreeWidgetItem* parentNode) {
        QStringList out;
        if (!parentNode) {
            return out;
        }
        std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* node) {
            if (!node) {
                return;
            }
            if (node->data(0, kConnPropRowRole).toBool()
                && node->data(0, kConnPropRowKindRole).toInt() == 1) {
                for (int col = 4; col < tree->columnCount(); ++col) {
                    QString key = node->data(col, kConnPropKeyRole).toString().trimmed();
                    if (key.isEmpty()) {
                        key = node->text(col).trimmed();
                    }
                    if (!key.isEmpty()) {
                        out.push_back(key);
                    }
                }
            }
            for (int i = 0; i < node->childCount(); ++i) {
                collect(node->child(i));
            }
        };
        collect(parentNode);
        return normalizeList(out);
    };

    enum class ManagePropsScope {
        Pool,
        Dataset,
        Snapshot,
    };
    QTreeWidgetItem* snapshotContextNode = nullptr;
    for (QTreeWidgetItem* p = item; p; p = p->parent()) {
        if (p->data(0, kConnSnapshotItemRole).toBool()) {
            snapshotContextNode = p;
            break;
        }
    }
    QStringList allProps;
    QStringList currentVisible;
    QVector<MainWindow::InlinePropGroupConfig> currentGroups;
    ManagePropsScope scope = poolContext ? ManagePropsScope::Pool : ManagePropsScope::Dataset;
    if (poolContext) {
        currentGroups = m_mainWindow->m_poolInlinePropGroups;
        const QString cacheKey = m_mainWindow->poolDetailsCacheKey(connIdx, poolName);
        const auto pit = m_mainWindow->m_poolDetailsCache.constFind(cacheKey);
        if (pit != m_mainWindow->m_poolDetailsCache.cend() && pit->loaded) {
            for (const QStringList& row : pit->propsRows) {
                if (row.isEmpty()) {
                    continue;
                }
                const QString prop = row[0].trimmed();
                if (prop.isEmpty() || prop.startsWith(QStringLiteral("feature@"), Qt::CaseInsensitive)) {
                    continue;
                }
                allProps.push_back(prop);
            }
        }
        allProps = normalizeList(allProps);
        QTreeWidgetItem* infoNode = item;
        while (infoNode && infoNode->data(0, kConnPropKeyRole).toString() != QString::fromLatin1(kPoolBlockInfoKey)) {
            infoNode = infoNode->parent();
        }
        currentVisible = displayedPropsFromNode(infoNode);
        if (!m_mainWindow->m_poolInlinePropsOrder.isEmpty()) {
            currentVisible.clear();
            for (const QString& p : m_mainWindow->m_poolInlinePropsOrder) {
                for (const QString& have : allProps) {
                    if (p.compare(have, Qt::CaseInsensitive) == 0) {
                        currentVisible.push_back(have);
                        break;
                    }
                }
            }
        }
    } else {
        QTreeWidgetItem* contextNode = owner;
        if (snapshotContextNode) {
            contextNode = snapshotContextNode;
        }
        const QString ds = contextNode->data(0, Qt::UserRole).toString().trimmed();
        if (ds.isEmpty()) {
            return;
        }
        const QString snap = snapshotContextNode
                                 ? snapshotContextNode->data(1, Qt::UserRole).toString().trimmed()
                                 : QString();
        if (!snap.isEmpty()) {
            scope = ManagePropsScope::Snapshot;
            currentGroups = m_mainWindow->m_snapshotInlinePropGroups;
        } else {
            currentGroups = m_mainWindow->m_datasetInlinePropGroups;
        }
        const QString objectName = snap.isEmpty() ? ds : QStringLiteral("%1@%2").arg(ds, snap);
        const QString key = QStringLiteral("%1|%2").arg(token.trimmed(), objectName.trimmed());
        const auto vit = m_mainWindow->m_connContentPropValuesByObject.constFind(key);
        if (vit != m_mainWindow->m_connContentPropValuesByObject.cend()) {
            allProps = vit.value().keys();
        } else {
            const QVector<MainWindow::DatasetPropCacheRow> rows =
                m_mainWindow->datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
            for (const MainWindow::DatasetPropCacheRow& row : rows) {
                if (!row.prop.trimmed().isEmpty()) {
                    allProps.push_back(row.prop.trimmed());
                }
            }
        }
        allProps = normalizeList(allProps);
        currentVisible = displayedPropsFromNode(contextNode);
        const QStringList& savedOrder =
            (scope == ManagePropsScope::Snapshot) ? m_mainWindow->m_snapshotInlineVisibleProps
                                                  : m_mainWindow->m_datasetInlinePropsOrder;
        if (!savedOrder.isEmpty()) {
            currentVisible.clear();
            for (const QString& p : savedOrder) {
                for (const QString& have : allProps) {
                    if (p.compare(have, Qt::CaseInsensitive) == 0) {
                        currentVisible.push_back(have);
                        break;
                    }
                }
            }
        }
        if (allProps.isEmpty()) {
            allProps = currentVisible;
        }
    }
    allProps = normalizeList(allProps);
    currentVisible = normalizeList(currentVisible);
    if (poolContext && currentVisible.isEmpty()) {
        currentVisible = allProps;
    }
    if (allProps.isEmpty()) {
        QMessageBox::information(m_mainWindow,
                                 QStringLiteral("ZFSMgr"),
                                 QStringLiteral("No hay propiedades disponibles para gestionar en este nodo."));
        return;
    }
    QStringList selection = currentVisible;
    QVector<MainWindow::InlinePropGroupConfig> editedGroups = currentGroups;
    if (!m_mainWindow->editInlinePropertiesDialog(
            m_mainWindow->trk(QStringLiteral("t_visible_props_title_001"),
                              QStringLiteral("Propiedades visibles"),
                              QStringLiteral("Visible properties"),
                              QStringLiteral("可见属性")),
            m_mainWindow->trk(QStringLiteral("t_visible_props_msg_001"),
                              QStringLiteral("Seleccione las propiedades que desea mostrar en línea y organícelas en grupos."),
                              QStringLiteral("Select the properties you want to show inline and organize them into groups."),
                              QStringLiteral("选择要内联显示的属性并按组组织。")),
                                                  allProps,
                                                  selection,
                                                  editedGroups,
                                                  clickedGroupName)) {
        return;
    }
    selection = normalizeList(selection);
    switch (scope) {
    case ManagePropsScope::Pool:
        m_mainWindow->m_poolInlinePropsOrder = selection;
        m_mainWindow->m_poolInlinePropGroups = editedGroups;
        break;
    case ManagePropsScope::Dataset:
        m_mainWindow->m_datasetInlinePropsOrder = selection;
        m_mainWindow->m_datasetInlinePropGroups = editedGroups;
        break;
    case ManagePropsScope::Snapshot:
        m_mainWindow->m_snapshotInlineVisibleProps = selection;
        m_mainWindow->m_snapshotInlinePropGroups = editedGroups;
        break;
    }

    focusContextItemForInlineVisibility(tree, rawItem);
    applyInlineSectionVisibility(tree, token);
}

void MainWindowConnectionDatasetTreeDelegate::focusContextItemForInlineVisibility(QTreeWidget* tree,
                                                                                  QTreeWidgetItem* rawItem) {
    if (!tree || !rawItem) {
        return;
    }
    QTreeWidgetItem* item = rawItem;
    if (item->data(0, kConnPropRowRole).toBool() && item->parent()) {
        item = item->parent();
    }
    QTreeWidgetItem* focusItem = item;
    const bool itemIsMainPropsNode =
        item->data(0, kConnPropGroupNodeRole).toBool()
        && item->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
        && isMainPropertiesNodeLabel(item->text(0));
    if (!itemIsMainPropsNode) {
        QTreeWidgetItem* probe = item;
        while (probe) {
            const bool probeIsMainPropsNode =
                probe->data(0, kConnPropGroupNodeRole).toBool()
                && probe->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                && isMainPropertiesNodeLabel(probe->text(0));
            if (probeIsMainPropsNode) {
                focusItem = probe;
                break;
            }
            probe = probe->parent();
        }
    }
    QTreeWidgetItem* owner = item;
    while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
           && !owner->data(0, kIsPoolRootRole).toBool()) {
        owner = owner->parent();
    }
    if (!owner) {
        return;
    }
    for (QTreeWidgetItem* p = owner->parent(); p; p = p->parent()) {
        p->setExpanded(true);
    }
    owner->setExpanded(true);
    ++m_selectionRefreshEpoch;
    tree->setCurrentItem(focusItem ? focusItem : owner);
}

bool MainWindowConnectionDatasetTreeDelegate::executeDatasetActionWithStdin(
    const QString& side,
    const QString& actionName,
    int connIdx,
    const QString& poolName,
    const QString& datasetName,
    const QString& snapshotName,
    const QString& cmd,
    const QByteArray& stdinPayload,
    int timeoutMs) {
    if (!m_mainWindow) {
        return false;
    }
    MainWindow::DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = datasetName;
    ctx.snapshotName = snapshotName;
    return m_mainWindow->executeDatasetAction(side, actionName, ctx, cmd, timeoutMs, false, stdinPayload);
}

bool MainWindowConnectionDatasetTreeDelegate::promptNewPassphrase(const QString& title, QString& passphraseOut) {
    if (!m_mainWindow) {
        return false;
    }
    QDialog dlg(m_mainWindow);
    dlg.setModal(true);
    dlg.setWindowTitle(title);
    auto* layout = new QFormLayout(&dlg);
    auto* pass1 = new QLineEdit(&dlg);
    auto* pass2 = new QLineEdit(&dlg);
    pass1->setEchoMode(QLineEdit::Password);
    pass2->setEchoMode(QLineEdit::Password);
    layout->addRow(m_mainWindow->trk(QStringLiteral("t_new_key_lbl_001"),
                                     QStringLiteral("Nueva clave"),
                                     QStringLiteral("New key"),
                                     QStringLiteral("新密钥")),
                   pass1);
    layout->addRow(m_mainWindow->trk(QStringLiteral("t_repeat_key_lbl_001"),
                                     QStringLiteral("Repita la clave"),
                                     QStringLiteral("Repeat key"),
                                     QStringLiteral("重复密钥")),
                   pass2);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    const QString p1 = pass1->text();
    const QString p2 = pass2->text();
    if (p1.isEmpty() || p1 != p2) {
        QMessageBox::warning(m_mainWindow,
                             QStringLiteral("ZFSMgr"),
                             m_mainWindow->trk(QStringLiteral("t_keys_do_not_match_001"),
                                               QStringLiteral("Las claves no coinciden."),
                                               QStringLiteral("Keys do not match."),
                                               QStringLiteral("密钥不匹配。")));
        return false;
    }
    passphraseOut = p1;
    return true;
}

void MainWindowConnectionDatasetTreeDelegate::createSnapshotHold(QTreeWidget* tree, QTreeWidgetItem* rawItem) {
    if (!m_mainWindow || !tree || !rawItem) {
        return;
    }
    Q_UNUSED(tree);
    QTreeWidgetItem* item = rawItem;
    while (item && item->data(0, Qt::UserRole).toString().isEmpty()
           && !item->data(0, kIsPoolRootRole).toBool()) {
        item = item->parent();
    }
    if (!item) {
        return;
    }
    const QString datasetName = item->data(0, Qt::UserRole).toString().trimmed();
    const QString snapshotName = item->data(1, Qt::UserRole).toString().trimmed();
    const int connIdx = item->data(0, kConnIdxRole).toInt();
    const QString poolName = item->data(0, kPoolNameRole).toString().trimmed();
    if (datasetName.isEmpty() || snapshotName.isEmpty() || connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size()
        || poolName.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString holdName = QInputDialog::getText(
        m_mainWindow,
        m_mainWindow->trk(QStringLiteral("t_new_hold_title001"),
                          QStringLiteral("Nuevo Hold")),
        m_mainWindow->trk(QStringLiteral("t_new_hold_prompt001"),
                          QStringLiteral("Nombre del hold")),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (!ok || holdName.isEmpty()) {
        return;
    }
    MainWindow::DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = datasetName;
    ctx.snapshotName = snapshotName;
    const QString objectName = QStringLiteral("%1@%2").arg(datasetName, snapshotName);
    auto shQuote = [](QString s) {
        s.replace('\'', QStringLiteral("'\"'\"'"));
        return QStringLiteral("'%1'").arg(s);
    };
    const QString cmd = QStringLiteral("zfs hold %1 %2").arg(shQuote(holdName), shQuote(objectName));
    ConnectionProfile cp = m_mainWindow->m_profiles[connIdx];
    if (m_mainWindow->isLocalConnection(cp) && !m_mainWindow->isWindowsConnection(cp)) {
        cp.useSudo = true;
        if (!m_mainWindow->ensureLocalSudoCredentials(cp)) {
            m_mainWindow->appLog(QStringLiteral("INFO"), QStringLiteral("Crear hold cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    const QString fullCmd = m_mainWindow->sshExecFromLocal(cp, m_mainWindow->withSudo(cp, cmd));
    const QString connLabel = cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
    QString errorText;
    if (!m_mainWindow->queuePendingShellAction(MainWindow::PendingShellActionDraft{
            QStringLiteral("%1::%2").arg(connLabel, poolName),
            QStringLiteral("Crear hold %1 en %2").arg(holdName, objectName),
            fullCmd,
            45000,
            false,
            {},
            ctx,
            MainWindow::PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
        QMessageBox::warning(m_mainWindow, QStringLiteral("ZFSMgr"), errorText);
        return;
    }
    m_mainWindow->appLog(QStringLiteral("NORMAL"),
                         QStringLiteral("Cambio pendiente añadido: %1::%2  Crear hold %3 en %4")
                             .arg(connLabel, poolName, holdName, objectName));
    m_mainWindow->updateApplyPropsButtonState();
}

void MainWindowConnectionDatasetTreeDelegate::releaseSnapshotHold(QTreeWidget* tree, QTreeWidgetItem* rawItem) {
    if (!m_mainWindow || !tree || !rawItem) {
        return;
    }
    Q_UNUSED(tree);
    QTreeWidgetItem* holdItem = rawItem;
    if (!holdItem->data(0, kConnSnapshotHoldItemRole).toBool()) {
        holdItem = holdItem->parent();
    }
    while (holdItem && !holdItem->data(0, kConnSnapshotHoldItemRole).toBool()) {
        holdItem = holdItem->parent();
    }
    if (!holdItem) {
        return;
    }
    QTreeWidgetItem* snapshotItem = holdItem;
    while (snapshotItem && snapshotItem->data(0, Qt::UserRole).toString().isEmpty()
           && !snapshotItem->data(0, kIsPoolRootRole).toBool()) {
        snapshotItem = snapshotItem->parent();
    }
    if (!snapshotItem) {
        return;
    }
    const QString holdName = holdItem->data(0, kConnSnapshotHoldTagRole).toString().trimmed();
    const QString datasetName = snapshotItem->data(0, Qt::UserRole).toString().trimmed();
    const QString snapshotName = snapshotItem->data(1, Qt::UserRole).toString().trimmed();
    const int connIdx = snapshotItem->data(0, kConnIdxRole).toInt();
    const QString poolName = snapshotItem->data(0, kPoolNameRole).toString().trimmed();
    if (holdName.isEmpty() || datasetName.isEmpty() || snapshotName.isEmpty()
        || connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size() || poolName.isEmpty()) {
        return;
    }
    const auto confirm = QMessageBox::question(
        m_mainWindow,
        m_mainWindow->trk(QStringLiteral("t_release_hold_title001"),
                          QStringLiteral("Release")),
        m_mainWindow->trk(QStringLiteral("t_release_hold_confirm001"),
                          QStringLiteral("¿Liberar hold \"%1\" del snapshot \"%2@%3\"?")
                              .arg(holdName, datasetName, snapshotName)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    MainWindow::DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = datasetName;
    ctx.snapshotName = snapshotName;
    auto shQuote = [](QString s) {
        s.replace('\'', QStringLiteral("'\"'\"'"));
        return QStringLiteral("'%1'").arg(s);
    };
    const QString objectName = QStringLiteral("%1@%2").arg(datasetName, snapshotName);
    const QString cmd = QStringLiteral("zfs release %1 %2").arg(shQuote(holdName), shQuote(objectName));
    ConnectionProfile cp = m_mainWindow->m_profiles[connIdx];
    if (m_mainWindow->isLocalConnection(cp) && !m_mainWindow->isWindowsConnection(cp)) {
        cp.useSudo = true;
        if (!m_mainWindow->ensureLocalSudoCredentials(cp)) {
            m_mainWindow->appLog(QStringLiteral("INFO"), QStringLiteral("Release hold cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    const QString fullCmd = m_mainWindow->sshExecFromLocal(cp, m_mainWindow->withSudo(cp, cmd));
    const QString connLabel = cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
    QString errorText;
    if (!m_mainWindow->queuePendingShellAction(MainWindow::PendingShellActionDraft{
            QStringLiteral("%1::%2").arg(connLabel, poolName),
            QStringLiteral("Release hold %1 en %2").arg(holdName, objectName),
            fullCmd,
            45000,
            false,
            {},
            ctx,
            MainWindow::PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
        QMessageBox::warning(m_mainWindow, QStringLiteral("ZFSMgr"), errorText);
        return;
    }
    m_mainWindow->appLog(QStringLiteral("NORMAL"),
                         QStringLiteral("Cambio pendiente añadido: %1::%2  Release hold %3 en %4")
                             .arg(connLabel, poolName, holdName, objectName));
    m_mainWindow->updateApplyPropsButtonState();
}

void MainWindowConnectionDatasetTreeDelegate::itemClicked(QTreeWidget* tree, QTreeWidgetItem* item) {
    if (!m_mainWindow || !tree || !item) {
        return;
    }
    if (m_contextMenuInProgress) {
        return;
    }
    if (item->data(0, kIsConnectionRootRole).toBool()) {
        m_mainWindow->updatePoolManagementBoxTitle();
        return;
    }
    const bool isPoolInfoNode =
        item->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey);
    if (isPoolInfoNode) {
        if (!m_mainWindow || m_mainWindow->m_closing || !tree || !item) {
            return;
        }
        QTreeWidgetItem* owner = ownerItemForNode(item->parent());
        if (!owner) {
            return;
        }
        const QString token = tokenForOwnerItem(owner);
        if (token.isEmpty()) {
            return;
        }
        m_mainWindow->syncConnContentPoolColumnsFor(tree, token);
        item->setExpanded(true);
        return;
    }
    const bool isLazyPropsNode =
        item->data(0, kConnPropGroupNodeRole).toBool()
        && item->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
        && isMainPropertiesNodeLabel(item->text(0));
    if (isLazyPropsNode) {
        if (!m_mainWindow || m_mainWindow->m_closing || !tree || !item) {
            return;
        }
        QTreeWidgetItem* owner = ownerItemForNode(item->parent());
        if (!owner) {
            return;
        }
        const QString token = tokenForOwnerItem(owner);
        if (token.isEmpty()) {
            return;
        }
        if (item->childCount() == 0) {
            m_mainWindow->refreshConnContentPropertiesFor(tree);
        }
        item->setExpanded(true);
        return;
    }
    if (!item->data(0, kConnPermissionsNodeRole).toBool()
        || item->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("root")) {
        return;
    }
    QTreeWidgetItem* owner = item->parent();
    if (!owner) {
        return;
    }
    const bool wasEmpty = (item->childCount() == 0);
    if (!m_mainWindow || m_mainWindow->m_closing || !tree || !owner || !item) {
        return;
    }
    if (wasEmpty && item->childCount() == 0) {
        m_mainWindow->populateDatasetPermissionsNode(tree, owner, false);
    }
    if (wasEmpty) {
        item->setExpanded(true);
    } else {
        item->setExpanded(!item->isExpanded());
    }
}

void MainWindowConnectionDatasetTreeDelegate::selectionChanged(QTreeWidget* tree, bool isBottom) {
    if (!m_mainWindow || !tree || m_mainWindow->m_syncingConnContentColumns || m_suppressSelectionRefresh) {
        return;
    }
    Q_UNUSED(isBottom);
    if (m_mainWindow->m_rebuildingTopConnContentTree) {
        return;
    }
    QTreeWidgetItem* sel = tree->currentItem();
    const int selectedConnIdx = sel ? sel->data(0, kConnIdxRole).toInt() : -1;
    if (selectedConnIdx >= 0 && selectedConnIdx < m_mainWindow->m_profiles.size()) {
        m_mainWindow->m_topDetailConnIdx = selectedConnIdx;
    }
    if (sel && sel->data(0, kIsConnectionRootRole).toBool()) {
        m_mainWindow->updatePoolManagementBoxTitle();
        m_mainWindow->updateConnectionDetailTitlesForCurrentSelection();
        m_mainWindow->updateConnectionActionsState();
        return;
    }
    const bool isPropRow = sel && sel->data(0, kConnPropRowRole).toBool();
    const bool isGroupNode = sel && sel->data(0, kConnPropGroupNodeRole).toBool();
    const bool isGsaNode = sel && sel->data(0, Qt::UserRole + 33).toBool();
    const bool isHoldItem = sel && sel->data(0, kConnSnapshotHoldItemRole).toBool();
    const bool isPermissionsNode = sel && sel->data(0, kConnPermissionsNodeRole).toBool();
    const bool isAutoSnapshotsSummary =
        sel && (sel->data(0, kConnPoolAutoSnapshotsNodeRole).toBool()
                || !sel->data(0, kConnPoolAutoSnapshotsDatasetRole).toString().trimmed().isEmpty()
                || (sel->parent() && sel->parent()->data(0, kConnPoolAutoSnapshotsNodeRole).toBool()));
    const bool isPoolContext =
        sel && (sel->data(0, kIsPoolRootRole).toBool() || isPoolInfoNodeOrInside(sel));
    const bool isLazyPropsNode =
        sel && isGroupNode && !isPoolContext
        && sel->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
        && isMainPropertiesNodeLabel(sel->text(0))
        && sel->childCount() == 0;
    const bool isLazyPermissionsNode =
        sel && isPermissionsNode
        && sel->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
        && sel->childCount() == 0;
    if ((isPropRow || (isGroupNode && !isGsaNode) || isHoldItem || isPermissionsNode)
        && !isPoolContext && !isLazyPropsNode && !isLazyPermissionsNode) {
        m_mainWindow->updateConnectionDetailTitlesForCurrentSelection();
        m_mainWindow->updateConnectionActionsState();
        return;
    }
    if (isAutoSnapshotsSummary) {
        m_mainWindow->updateConnectionDetailTitlesForCurrentSelection();
        m_mainWindow->updateConnectionActionsState();
        return;
    }
    QTreeWidgetItem* owner = ownerItemForNode(sel);
    const QString token = tokenForOwnerItem(owner);
    if (token.isEmpty()) {
        m_mainWindow->updateConnectionDetailTitlesForCurrentSelection();
        m_mainWindow->updateConnectionActionsState();
        return;
    }
    if (isPoolContext) {
        if (sel && sel->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
            if (!m_contextMenuInProgress) {
                sel->setExpanded(true);
            }
        }
        m_mainWindow->syncConnContentPoolColumnsFor(tree, token);
    } else {
        if (isLazyPermissionsNode) {
            m_mainWindow->populateDatasetPermissionsNode(tree, owner, false);
            if (sel && !m_contextMenuInProgress) {
                sel->setExpanded(true);
            }
        } else {
            const quint64 scheduledEpoch = m_selectionRefreshEpoch;
            if (scheduledEpoch != m_selectionRefreshEpoch) {
                return;
            }
            if (m_suppressSelectionRefresh) {
                return;
            }
            if (m_mainWindow->m_rebuildingTopConnContentTree) {
                return;
            }
            m_mainWindow->refreshConnContentPropertiesFor(tree);
            if (!m_contextMenuInProgress && isLazyPropsNode && sel) {
                sel->setExpanded(true);
            }
            m_mainWindow->syncConnContentPropertyColumnsFor(tree, token);
        }
    }
    m_mainWindow->updateConnectionDetailTitlesForCurrentSelection();
    m_mainWindow->updateConnectionActionsState();
}

void MainWindowConnectionDatasetTreeDelegate::itemChanged(QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, int column) {
    if (!m_mainWindow || !tree || !item) {
        return;
    }
    QTreeWidgetItem* owner = ownerItemForNode(item);
    if (!owner) {
        return;
    }
    const QString token = tokenForOwnerItem(owner);
    if (token.isEmpty()) {
        return;
    }
    m_mainWindow->onDatasetTreeItemChanged(tree, item, column, MainWindow::DatasetTreeContext::ConnectionContent);
    Q_UNUSED(isBottom);
    m_mainWindow->updateConnectionActionsState();
}

void MainWindowConnectionDatasetTreeDelegate::itemExpanded(QTreeWidget* tree, QTreeWidgetItem* item) {
    if (!m_mainWindow || !tree || !item) {
        return;
    }
    if (item->data(0, kConnPropGroupNodeRole).toBool() || item->data(0, kConnPermissionsNodeRole).toBool()) {
        m_mainWindow->appLog(QStringLiteral("DEBUG"),
                             QStringLiteral("tree.itemExpanded path=%1")
                                 .arg(debugConnTreeNodePath(item)));
    }
    if (QTreeWidgetItem* owner = ownerItemForNode(item)) {
        const QString token = tokenForOwnerItem(owner);
        if (!token.isEmpty()) {
            m_mainWindow->saveConnContentTreeStateFor(tree, token);
        }
    }
    m_mainWindow->resizeTreeColumnsToVisibleContent(tree);
    if (!item->data(0, kConnPermissionsNodeRole).toBool()) {
        return;
    }
    if (item->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
        && item->childCount() == 0) {
        if (QTreeWidgetItem* owner = item->parent()) {
            m_mainWindow->populateDatasetPermissionsNode(tree, owner, false);
            m_mainWindow->resizeTreeColumnsToVisibleContent(tree);
        }
    }
}

void MainWindowConnectionDatasetTreeDelegate::itemCollapsed(QTreeWidget* tree, QTreeWidgetItem* item) {
    if (!m_mainWindow || !tree || !item) {
        return;
    }
    if (item->data(0, kConnPropGroupNodeRole).toBool() || item->data(0, kConnPermissionsNodeRole).toBool()) {
        m_mainWindow->appLog(QStringLiteral("DEBUG"),
                             QStringLiteral("tree.itemCollapsed path=%1")
                                 .arg(debugConnTreeNodePath(item)));
    }
    if (QTreeWidgetItem* owner = ownerItemForNode(item)) {
        const QString token = tokenForOwnerItem(owner);
        if (!token.isEmpty()) {
            m_mainWindow->saveConnContentTreeStateFor(tree, token);
        }
    }
    m_mainWindow->resizeTreeColumnsToVisibleContent(tree);
}

void MainWindowConnectionDatasetTreeDelegate::beforeContextMenu(QTreeWidget* tree) {
    Q_UNUSED(tree);
    m_contextMenuInProgress = true;
    if (m_mainWindow) {
        QElapsedTimer timer;
        timer.start();
        m_mainWindow->beginUiBusy();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 25);
        m_mainWindow->updateConnectionActionsState();
        logContextMenuPerf(m_mainWindow,
                           QStringLiteral("beforeContextMenu"),
                           QStringLiteral("busy entered"),
                           timer.elapsed());
    }
}

void MainWindowConnectionDatasetTreeDelegate::afterContextMenu(QTreeWidget* tree) {
    Q_UNUSED(tree);
    m_contextMenuInProgress = false;
}

bool MainWindowConnectionDatasetTreeDelegate::handleAutoSnapshotsMenu(QTreeWidget* tree,
                                                                      QTreeWidgetItem* item,
                                                                      const QPoint& pos) {
    if (!m_mainWindow || !tree || !item) {
        return false;
    }
    const QString autoSnapshotDataset = item->data(0, kConnPoolAutoSnapshotsDatasetRole).toString().trimmed();
    QElapsedTimer timer;
    timer.start();
    bool insideAutoSnapshots = false;
    for (QTreeWidgetItem* p = item; p; p = p->parent()) {
        if (p->data(0, kConnPoolAutoSnapshotsNodeRole).toBool()) {
            insideAutoSnapshots = true;
            break;
        }
    }
    if (!insideAutoSnapshots) {
        logContextMenuPerf(m_mainWindow,
                           QStringLiteral("autoSnapshots.skip"),
                           QStringLiteral("outside auto-snapshots"),
                           timer.elapsed());
        return false;
    }
    if (autoSnapshotDataset.isEmpty()) {
        return true;
    }
    const int connIdx = item->data(0, kConnIdxRole).toInt();
    const QString poolName = item->data(0, kPoolNameRole).toString().trimmed();
    if (connIdx < 0 || connIdx >= m_mainWindow->m_profiles.size() || poolName.isEmpty()) {
        logContextMenuPerf(m_mainWindow,
                           QStringLiteral("autoSnapshots.fallback"),
                           QStringLiteral("invalid pool context"),
                           timer.elapsed());
        return true;
    }
    const QStringList gsaProps = gsaUserPropsForScheduling();
    QMenu autoMenu(m_mainWindow);
    QAction* aDeleteSchedule = autoMenu.addAction(QStringLiteral("Borrar programación"));
    m_mainWindow->endUiBusy();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 25);
    logContextMenuPerf(m_mainWindow,
                       QStringLiteral("autoSnapshots.exec"),
                       QStringLiteral("dataset=%1").arg(autoSnapshotDataset),
                       timer.elapsed());
    QAction* picked = autoMenu.exec(tree->viewport()->mapToGlobal(pos));
    if (picked != aDeleteSchedule) {
        return true;
    }
    const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    for (const QString& prop : gsaProps) {
        m_mainWindow->updateConnContentDraftInherit(token, autoSnapshotDataset, prop, true);
    }
    m_mainWindow->m_propsDirty = true;
    m_mainWindow->updateApplyPropsButtonState();
    refreshAllTreesForTokenAndDataset(token, autoSnapshotDataset);
    return true;
}

bool MainWindowConnectionDatasetTreeDelegate::handlePermissionsMenu(QTreeWidget* tree,
                                                                    bool isBottom,
                                                                    QTreeWidgetItem* item,
                                                                    const QPoint& pos) {
    Q_UNUSED(isBottom);
    QElapsedTimer timer;
    timer.start();
    if (!m_mainWindow || !tree || !item) {
        return false;
    }
    auto permissionNodeItem = [](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        QTreeWidgetItem* node = item;
        while (node && !node->data(0, kConnPermissionsNodeRole).toBool()) {
            node = node->parent();
        }
        return node;
    };
    auto permissionTokensForDataset = [this](const MainWindow::DatasetSelectionContext& ctx) {
        return m_mainWindow->availableDelegablePermissions(ctx.datasetName, ctx.connIdx, ctx.poolName);
    };
    auto promptPermissionGrant = [this, &permissionTokensForDataset](const MainWindow::DatasetSelectionContext& ctx,
                                                                    const QString& title,
                                                                    const QString& initialTargetType,
                                                                    const QString& initialTargetName,
                                                                    const QString& initialScope,
                                                                    const QStringList& initialTokens,
                                                                    bool allowTokenSelection,
                                                                    QString& targetTypeOut,
                                                                    QString& targetNameOut,
                                                                    QString& scopeOut,
                                                                    QStringList& tokensOut) {
        QDialog dlg(m_mainWindow);
        dlg.setModal(true);
        dlg.setWindowTitle(title);
        auto* layout = new QVBoxLayout(&dlg);
        auto* form = new QFormLayout();
        auto* targetType = new QComboBox(&dlg);
        targetType->addItem(QStringLiteral("Usuario"), QStringLiteral("user"));
        targetType->addItem(QStringLiteral("Grupo"), QStringLiteral("group"));
        targetType->addItem(QStringLiteral("Everyone"), QStringLiteral("everyone"));
        auto* targetName = new QComboBox(&dlg);
        targetName->setEditable(true);
        auto* scope = new QComboBox(&dlg);
        scope->addItem(QStringLiteral("Local"), QStringLiteral("local"));
        scope->addItem(QStringLiteral("Descendiente"), QStringLiteral("descendant"));
        scope->addItem(QStringLiteral("Local + descendiente"), QStringLiteral("local_descendant"));
        const auto* cachedPermissions =
            m_mainWindow->datasetPermissionsEntry(ctx.connIdx, ctx.poolName, ctx.datasetName);
        auto reloadTargetNames = [cachedPermissions, targetType, targetName]() {
            targetName->clear();
            if (!cachedPermissions) {
                return;
            }
            const QString kind = targetType->currentData().toString();
            const QStringList names =
                (kind == QStringLiteral("group")) ? cachedPermissions->systemGroups : cachedPermissions->systemUsers;
            targetName->addItems(names);
            targetName->setEnabled(kind != QStringLiteral("everyone"));
        };
        QObject::connect(targetType, &QComboBox::currentIndexChanged, &dlg, reloadTargetNames);
        reloadTargetNames();
        if (!initialTargetType.isEmpty()) {
            const int typeIdx = targetType->findData(initialTargetType);
            if (typeIdx >= 0) {
                targetType->setCurrentIndex(typeIdx);
                reloadTargetNames();
            }
        }
        if (!initialTargetName.isEmpty()) {
            targetName->setCurrentText(initialTargetName);
        }
        if (!initialScope.isEmpty()) {
            const int scopeIdx = scope->findData(initialScope);
            if (scopeIdx >= 0) {
                scope->setCurrentIndex(scopeIdx);
            }
        }
        form->addRow(QStringLiteral("Destino"), targetType);
        form->addRow(QStringLiteral("Nombre"), targetName);
        form->addRow(QStringLiteral("Ámbito"), scope);
        layout->addLayout(form);
        QStringList selectedTokens = initialTokens;
        if (allowTokenSelection) {
            auto* pickTokens = new QPushButton(QStringLiteral("Elegir permisos"), &dlg);
            auto* chosenLabel = new QLabel(selectedTokens.isEmpty() ? QStringLiteral("Ninguno seleccionado")
                                                                    : selectedTokens.join(QStringLiteral(", ")),
                                           &dlg);
            QObject::connect(pickTokens, &QPushButton::clicked, &dlg, [&, chosenLabel]() {
                QStringList picked = selectedTokens;
                if (!m_mainWindow->selectItemsDialog(QStringLiteral("Permisos delegados"),
                                                     QStringLiteral("Seleccione permisos o @sets para la nueva delegación."),
                                                     permissionTokensForDataset(ctx),
                                                     picked)) {
                    return;
                }
                selectedTokens = picked;
                chosenLabel->setText(selectedTokens.isEmpty() ? QStringLiteral("Ninguno seleccionado")
                                                              : selectedTokens.join(QStringLiteral(", ")));
            });
            layout->addWidget(pickTokens);
            layout->addWidget(chosenLabel);
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        targetTypeOut = targetType->currentData().toString();
        targetNameOut = targetName->currentText().trimmed();
        scopeOut = scope->currentData().toString();
        tokensOut = selectedTokens;
        if (allowTokenSelection && tokensOut.isEmpty()) {
            QMessageBox::warning(m_mainWindow, QStringLiteral("ZFSMgr"), QStringLiteral("Debe seleccionar al menos un permiso o set."));
            return false;
        }
        if (targetTypeOut != QStringLiteral("everyone") && targetNameOut.isEmpty()) {
            QMessageBox::warning(m_mainWindow, QStringLiteral("ZFSMgr"), QStringLiteral("Debe indicar el usuario o grupo."));
            return false;
        }
        return true;
    };
    auto promptPermissionSet = [this, &permissionTokensForDataset](const MainWindow::DatasetSelectionContext& ctx,
                                                                   const QString& title,
                                                                   const QString& initialSetName,
                                                                   const QStringList& initialTokens,
                                                                   bool allowEmpty,
                                                                   QString& setNameOut,
                                                                   QStringList& tokensOut) {
        bool ok = false;
        QString setName = QInputDialog::getText(
            m_mainWindow,
            title,
            QStringLiteral("Nombre del set"),
            QLineEdit::Normal,
            initialSetName,
            &ok).trimmed();
        if (!ok || setName.isEmpty()) {
            return false;
        }
        if (!setName.startsWith(QLatin1Char('@'))) {
            setName.prepend(QLatin1Char('@'));
        }
        QStringList picked = initialTokens;
        QStringList available = permissionTokensForDataset(ctx);
        if (!initialSetName.trimmed().isEmpty()) {
            for (int i = available.size() - 1; i >= 0; --i) {
                if (available.at(i).compare(initialSetName.trimmed(), Qt::CaseInsensitive) == 0) {
                    available.removeAt(i);
                }
            }
        }
        if (!m_mainWindow->selectItemsDialog(title,
                                             QStringLiteral("Seleccione permisos o @sets para el nuevo set."),
                                             available,
                                             picked)) {
            return false;
        }
        if (picked.isEmpty() && !allowEmpty) {
            QMessageBox::warning(m_mainWindow, QStringLiteral("ZFSMgr"), QStringLiteral("Debe seleccionar al menos un permiso o set."));
            return false;
        }
        setNameOut = setName;
        tokensOut = picked;
        return true;
    };
    auto permissionTokensFromNode = [](QTreeWidgetItem* permNode) {
        QStringList tokens;
        if (!permNode) {
            return tokens;
        }
        QTreeWidgetItem* owner = permNode;
        const QString kind = permNode->data(0, kConnPermissionsKindRole).toString();
        if ((kind == QStringLiteral("grant_perm") || kind == QStringLiteral("set_perm")) && permNode->parent()) {
            owner = permNode->parent();
        }
        for (int i = 0; i < owner->childCount(); ++i) {
            QTreeWidgetItem* child = owner->child(i);
            if (!child) {
                continue;
            }
            if (child->data(0, kConnPropRowRole).toBool()) {
                if (child->data(0, kConnPropRowKindRole).toInt() != 2) {
                    continue;
                }
                QTreeWidget* childTree = owner->treeWidget();
                if (!childTree) {
                    continue;
                }
                for (int col = 4; col < childTree->columnCount(); ++col) {
                    const QString token = child->data(col, kConnPermissionsEntryNameRole).toString().trimmed();
                    if (token.isEmpty()) {
                        continue;
                    }
                    QWidget* host = childTree->itemWidget(child, col);
                    QCheckBox* cb = host ? host->findChild<QCheckBox*>() : nullptr;
                    if (!cb || !cb->isChecked() || tokens.contains(token, Qt::CaseInsensitive)) {
                        continue;
                    }
                    tokens.push_back(token);
                }
                continue;
            }
            if ((child->flags() & Qt::ItemIsUserCheckable) && child->checkState(0) != Qt::Checked) {
                continue;
            }
            const QString token = child->text(0).trimmed();
            if (!token.isEmpty() && !tokens.contains(token, Qt::CaseInsensitive)) {
                tokens.push_back(token);
            }
        }
        tokens.sort(Qt::CaseInsensitive);
        return tokens;
    };

    QTreeWidgetItem* permNode = permissionNodeItem(item);
    if (!permNode) {
        logContextMenuPerf(m_mainWindow,
                           QStringLiteral("permissions.skip"),
                           QStringLiteral("not a permissions node"),
                           timer.elapsed());
        return false;
    }
    QTreeWidgetItem* owner = ownerItemForNode(item);
    if (!owner) {
        return true;
    }
    const QString token = tokenForOwnerItem(owner);
    SelectionSnapshot ctx = currentSelection(tree, token);
    MainWindow::DatasetSelectionContext mwCtx;
    mwCtx.valid = ctx.valid;
    mwCtx.connIdx = ctx.connIdx;
    mwCtx.poolName = ctx.poolName;
    mwCtx.datasetName = ctx.datasetName;
    mwCtx.snapshotName = ctx.snapshotName;
    if (ctx.valid
        && ctx.snapshotName.isEmpty()
        && !m_mainWindow->isWindowsConnection(ctx.connIdx)
        && permNode->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
        && permNode->childCount() == 0) {
        refreshPermissionsOwnerNode(tree, owner, false);
    }
    if (!ctx.valid || !ctx.snapshotName.isEmpty() || m_mainWindow->isWindowsConnection(ctx.connIdx)) {
        return true;
    }
    auto rebuildPermissionsNodeWithState = [&]() {
        refreshPermissionsOwnerNode(tree, owner, false);
        rehydrateExpandedDatasetNodes(tree, token);
        m_mainWindow->restoreConnContentTreeStateFor(tree, token);
    };
    auto refreshPermissionsNodeWithState = [&]() {
        refreshPermissionsOwnerNode(tree, owner, true);
        rehydrateExpandedDatasetNodes(tree, token);
        m_mainWindow->restoreConnContentTreeStateFor(tree, token);
    };

    QMenu permMenu(m_mainWindow);
    const QString kind = permNode->data(0, kConnPermissionsKindRole).toString();
    QAction* aRefreshPerms = permMenu.addAction(QStringLiteral("Refrescar permisos"));
    QMenu* newMenu = permMenu.addMenu(QStringLiteral("Nuevo"));
    QAction* aNewGrant = newMenu->addAction(QStringLiteral("Delegación"));
    QAction* aNewSet = newMenu->addAction(QStringLiteral("Set de permisos"));
    QAction* aEditGrant = nullptr;
    QAction* aDeleteGrant = nullptr;
    QAction* aRenameSet = nullptr;
    QAction* aDeleteSet = nullptr;
    if (kind == QStringLiteral("grant") || kind == QStringLiteral("grant_perm")) {
        permMenu.addSeparator();
        aEditGrant = permMenu.addAction(QStringLiteral("Editar delegación"));
        aDeleteGrant = permMenu.addAction(QStringLiteral("Eliminar delegación"));
    } else if (kind == QStringLiteral("set") || kind == QStringLiteral("set_perm")) {
        permMenu.addSeparator();
        aRenameSet = permMenu.addAction(QStringLiteral("Renombrar conjunto de permisos"));
        aDeleteSet = permMenu.addAction(QStringLiteral("Eliminar set"));
    }
    m_mainWindow->endUiBusy();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 25);
    logContextMenuPerf(m_mainWindow,
                       QStringLiteral("permissions.exec"),
                       QStringLiteral("kind=%1").arg(kind),
                       timer.elapsed());
    QAction* picked = permMenu.exec(tree->viewport()->mapToGlobal(pos));
    if (!picked) {
        return true;
    }
    if (picked == aRefreshPerms) {
        refreshPermissionsNodeWithState();
        return true;
    }
    if (picked == aNewGrant) {
        QString targetType;
        QString targetName;
        QString scope;
        QStringList tokens;
        if (!promptPermissionGrant(mwCtx,
                                   QStringLiteral("Nueva delegación"),
                                   QString(),
                                   QString(),
                                   QString(),
                                   {},
                                   false,
                                   targetType,
                                   targetName,
                                   scope,
                                   tokens)) {
            return true;
        }
        auto* entry = m_mainWindow->datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
        if (!entry) {
            return true;
        }
        bool exists = false;
        auto appendPending = [&](QVector<MainWindow::DatasetPermissionGrant>& grants) {
            for (const MainWindow::DatasetPermissionGrant& g : grants) {
                if (g.scope == scope && g.targetType == targetType && g.targetName == targetName) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                MainWindow::DatasetPermissionGrant g;
                g.scope = scope;
                g.targetType = targetType;
                g.targetName = targetName;
                g.pending = true;
                grants.push_back(g);
                entry->dirty = true;
                exists = true;
            }
        };
        if (scope == QStringLiteral("local")) {
            appendPending(entry->localGrants);
        } else if (scope == QStringLiteral("descendant")) {
            appendPending(entry->descendantGrants);
        } else {
            appendPending(entry->localDescendantGrants);
        }
        m_mainWindow->mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
        rebuildPermissionsNodeWithState();
        m_mainWindow->updateApplyPropsButtonState();
        for (int i = 0; i < owner->childCount(); ++i) {
            QTreeWidgetItem* child = owner->child(i);
            if (!child || !child->data(0, kConnPermissionsNodeRole).toBool()
                || child->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("root")) {
                continue;
            }
            child->setExpanded(true);
            for (int j = 0; j < child->childCount(); ++j) {
                QTreeWidgetItem* sec = child->child(j);
                if (!sec || sec->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("grants_root")) {
                    continue;
                }
                sec->setExpanded(true);
                for (int k = 0; k < sec->childCount(); ++k) {
                    QTreeWidgetItem* grantNode = sec->child(k);
                    if (!grantNode) {
                        continue;
                    }
                    if (grantNode->data(0, kConnPermissionsScopeRole).toString() == scope
                        && grantNode->data(0, kConnPermissionsTargetTypeRole).toString() == targetType
                        && grantNode->data(0, kConnPermissionsTargetNameRole).toString() == targetName) {
                        grantNode->setExpanded(true);
                        tree->setCurrentItem(grantNode);
                        break;
                    }
                }
            }
        }
        return true;
    }
    if (picked == aEditGrant) {
        QTreeWidgetItem* grantNode = permNode;
        if (kind == QStringLiteral("grant_perm") && permNode->parent()) {
            grantNode = permNode->parent();
        }
        QString targetType = grantNode->data(0, kConnPermissionsTargetTypeRole).toString();
        QString targetName = grantNode->data(0, kConnPermissionsTargetNameRole).toString();
        QString scope = grantNode->data(0, kConnPermissionsScopeRole).toString();
        QStringList tokens = permissionTokensFromNode(grantNode);
        QString newTargetType;
        QString newTargetName;
        QString newScope;
        QStringList newTokens;
        if (!promptPermissionGrant(mwCtx,
                                   QStringLiteral("Editar delegación"),
                                   targetType,
                                   targetName,
                                   scope,
                                   tokens,
                                   false,
                                   newTargetType,
                                   newTargetName,
                                   newScope,
                                   newTokens)) {
            return true;
        }
        auto* entry = m_mainWindow->datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
        if (entry) {
            auto updateGrant = [&](QVector<MainWindow::DatasetPermissionGrant>& grants) {
                for (MainWindow::DatasetPermissionGrant& g : grants) {
                    if (g.scope == scope && g.targetType == targetType && g.targetName == targetName) {
                        g.scope = newScope;
                        g.targetType = newTargetType;
                        g.targetName = newTargetName;
                        entry->dirty = true;
                        return true;
                    }
                }
                return false;
            };
            if (updateGrant(entry->localGrants) || updateGrant(entry->descendantGrants)
                || updateGrant(entry->localDescendantGrants)) {
                m_mainWindow->mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                rebuildPermissionsNodeWithState();
                m_mainWindow->updateApplyPropsButtonState();
            }
        }
        return true;
    }
    if (picked == aNewSet) {
        QString setName;
        QStringList tokens;
        if (!promptPermissionSet(mwCtx,
                                 QStringLiteral("Nuevo set de permisos"),
                                 QString(),
                                 {},
                                 false,
                                 setName,
                                 tokens)) {
            return true;
        }
        auto* entry = m_mainWindow->datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
        if (entry) {
            bool exists = false;
            for (const MainWindow::DatasetPermissionSet& s : entry->permissionSets) {
                if (s.name.compare(setName, Qt::CaseInsensitive) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                MainWindow::DatasetPermissionSet s;
                s.name = setName;
                s.permissions = tokens;
                entry->permissionSets.push_back(s);
                entry->dirty = true;
                m_mainWindow->mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                rebuildPermissionsNodeWithState();
                m_mainWindow->updateApplyPropsButtonState();
            }
        }
        return true;
    }
    if (picked == aRenameSet) {
        QTreeWidgetItem* setNode = permNode;
        if (kind == QStringLiteral("set_perm") && permNode->parent()) {
            setNode = permNode->parent();
        }
        const QString oldSetName = setNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
        bool ok = false;
        QString newSetName = QInputDialog::getText(
            m_mainWindow,
            QStringLiteral("Renombrar conjunto de permisos"),
            QStringLiteral("Nuevo nombre"),
            QLineEdit::Normal,
            oldSetName,
            &ok).trimmed();
        if (!ok || newSetName.isEmpty()) {
            return true;
        }
        if (!newSetName.startsWith(QLatin1Char('@'))) {
            newSetName.prepend(QLatin1Char('@'));
        }
        auto* entry = m_mainWindow->datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
        if (entry) {
            for (MainWindow::DatasetPermissionSet& s : entry->permissionSets) {
                if (s.name.compare(oldSetName, Qt::CaseInsensitive) == 0) {
                    s.name = newSetName;
                    entry->dirty = true;
                    break;
                }
            }
            m_mainWindow->mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
            rebuildPermissionsNodeWithState();
            m_mainWindow->updateApplyPropsButtonState();
        }
        return true;
    }
    if (picked == aDeleteGrant) {
        auto* entry = m_mainWindow->datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
        if (entry) {
            auto removeGrant = [&](QVector<MainWindow::DatasetPermissionGrant>& grants) {
                for (int i = grants.size() - 1; i >= 0; --i) {
                    const MainWindow::DatasetPermissionGrant& g = grants.at(i);
                    if (g.scope == permNode->data(0, kConnPermissionsScopeRole).toString()
                        && g.targetType == permNode->data(0, kConnPermissionsTargetTypeRole).toString()
                        && g.targetName == permNode->data(0, kConnPermissionsTargetNameRole).toString()) {
                        grants.removeAt(i);
                        entry->dirty = true;
                    }
                }
            };
            removeGrant(entry->localGrants);
            removeGrant(entry->descendantGrants);
            removeGrant(entry->localDescendantGrants);
            m_mainWindow->mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
            rebuildPermissionsNodeWithState();
            m_mainWindow->updateApplyPropsButtonState();
        }
        return true;
    }
    if (picked == aDeleteSet) {
        QString setName = permNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
        if (kind == QStringLiteral("set_perm") && permNode->parent()) {
            setName = permNode->parent()->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
        }
        auto* entry = m_mainWindow->datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
        if (entry) {
            for (int i = entry->permissionSets.size() - 1; i >= 0; --i) {
                if (entry->permissionSets.at(i).name.compare(setName, Qt::CaseInsensitive) == 0) {
                    entry->permissionSets.removeAt(i);
                    entry->dirty = true;
                }
            }
            m_mainWindow->mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
            rebuildPermissionsNodeWithState();
            m_mainWindow->updateApplyPropsButtonState();
        }
        return true;
    }
    return true;
}

void MainWindowConnectionDatasetTreeDelegate::showGeneralMenu(QTreeWidget* tree,
                                                              bool isBottom,
                                                              QTreeWidgetItem* item,
                                                              const QPoint& pos) {
    Q_UNUSED(isBottom);
    if (!m_mainWindow || !tree || !item) {
        return;
    }
    QElapsedTimer timer;
    timer.start();
    const auto endBusy = [this]() {
        if (m_mainWindow) {
            m_mainWindow->endUiBusy();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 25);
        }
    };
    int connIdx = item->data(0, kConnIdxRole).toInt();
    const QString poolName = item->data(0, kPoolNameRole).toString().trimmed();

    m_mainWindow->updateConnectionActionsState();

    QMenu menu(m_mainWindow);
    const bool isConnectionRoot = item->data(0, kIsConnectionRootRole).toBool();
    const bool isConnectionAuxChild =
        item->parent()
        && item->parent()->data(0, kIsConnectionRootRole).toBool()
        && item->data(0, Qt::UserRole).toString().trimmed().isEmpty()
        && !item->data(0, kIsPoolRootRole).toBool();
    const bool isPoolRoot = item->data(0, kIsPoolRootRole).toBool();
    const QString menuToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    auto isInfoNodeOrInside = [](QTreeWidgetItem* n) -> bool {
        for (QTreeWidgetItem* p = n; p; p = p->parent()) {
            if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                return true;
            }
        }
        return false;
    };
    const bool isPoolInfoContext = isInfoNodeOrInside(item);

    if (isConnectionRoot || isConnectionAuxChild) {
        if (!isConnectionRoot && item->parent()) {
            connIdx = item->parent()->data(0, kConnIdxRole).toInt();
        }
        logContextMenuPerf(m_mainWindow,
                           QStringLiteral("general.redirect"),
                           QStringLiteral("connection root connIdx=%1").arg(connIdx),
                           timer.elapsed());
        endBusy();
        m_mainWindow->showConnectionContextMenu(connIdx, tree->viewport()->mapToGlobal(pos));
        return;
    }

    int poolRow = -1;
    QString poolAction;
    zfsmgr::uilogic::PoolRootMenuState poolMenuState;
    PoolRootMenuActions poolActions;
    if (isPoolRoot) {
        if (connIdx >= 0 && connIdx < m_mainWindow->m_profiles.size() && !poolName.isEmpty()) {
            poolRow = m_mainWindow->findPoolRow(m_mainWindow->m_profiles[connIdx].name.trimmed(), poolName);
        }
        poolAction =
            (poolRow >= 0 && poolRow < m_mainWindow->m_poolListEntries.size())
                ? m_mainWindow->m_poolListEntries[poolRow].action.trimmed()
                : QString();
        poolMenuState =
            zfsmgr::uilogic::buildPoolRootMenuState(poolAction, QStringLiteral("ONLINE"), poolRow >= 0);
        QMenu* poolMenu = menu.addMenu(QStringLiteral("Pool"));
        poolActions = buildPoolRootMenu(*poolMenu, tree);
        poolActions.update->setEnabled(poolMenuState.canRefresh);
        poolActions.importPool->setEnabled(poolMenuState.canImport);
        poolActions.importRename->setEnabled(poolMenuState.canImport);
        poolActions.exportPool->setEnabled(poolMenuState.canExport);
        poolActions.history->setEnabled(poolMenuState.canHistory);
        poolActions.sync->setEnabled(poolMenuState.canSync);
        poolActions.scrub->setEnabled(poolMenuState.canScrub);
        poolActions.upgrade->setEnabled(poolMenuState.canUpgrade);
        poolActions.reguid->setEnabled(poolMenuState.canReguid);
        poolActions.trim->setEnabled(poolMenuState.canTrim);
        poolActions.initialize->setEnabled(poolMenuState.canInitialize);
        poolActions.clear->setEnabled(poolMenuState.canClear);
        poolActions.destroy->setEnabled(poolMenuState.canDestroy);
    }

    if (isPoolInfoContext) {
        const InlineVisibilityMenuActions inlineActions =
            buildInlineVisibilityMenu(menu, tree, true, true, true);
        logContextMenuPerf(m_mainWindow,
                           QStringLiteral("general.poolInfo.exec"),
                           QStringLiteral("conn=%1 pool=%2").arg(connIdx).arg(poolName),
                           timer.elapsed());
        endBusy();
        QAction* picked = menu.exec(tree->viewport()->mapToGlobal(pos));
        if (picked == inlineActions.manage) {
            manageInlinePropsVisualization(tree, item, true);
        }
        return;
    }

    const bool isSnapshotHoldContext = item->data(0, kConnSnapshotHoldItemRole).toBool()
                                       || (item->parent() && item->parent()->data(0, kConnSnapshotHoldItemRole).toBool());
    QTreeWidgetItem* holdContextItem = item;
    if (holdContextItem && !holdContextItem->data(0, kConnSnapshotHoldItemRole).toBool()) {
        holdContextItem = holdContextItem->parent();
    }
    const QString holdContextName =
        holdContextItem ? holdContextItem->data(0, kConnSnapshotHoldTagRole).toString().trimmed() : QString();

    const InlineVisibilityMenuActions inlineActions =
        buildInlineVisibilityMenu(menu, tree, true, false, false);
    menu.addSeparator();
    QAction* aCreate = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_ctx_create_dsv001"),
                          QStringLiteral("Crear dataset/snapshot/vol"),
                          QStringLiteral("Create dataset/snapshot/vol"),
                          QStringLiteral("创建 dataset/snapshot/vol")));
    QAction* aRename = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_ctx_rename_001"),
                          QStringLiteral("Renombrar"),
                          QStringLiteral("Rename"),
                          QStringLiteral("重命名")));
    QAction* aDelete = menu.addAction(deleteLabelForItem(connIdx, poolName, item));
    QMenu* mEncryption = menu.addMenu(QStringLiteral("Encriptación"));
    QAction* aLoadKey = mEncryption->addAction(
        m_mainWindow->trk(QStringLiteral("t_load_key_001"),
                          QStringLiteral("Cargar clave"),
                          QStringLiteral("Load key"),
                          QStringLiteral("加载密钥")));
    QAction* aUnloadKey = mEncryption->addAction(QStringLiteral("Unload key"));
    QAction* aChangeKey = mEncryption->addAction(QStringLiteral("Change key"));
    menu.addSeparator();
    QAction* aScheduleSnapshots = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_ctx_schedule_auto_snaps_001"),
                          QStringLiteral("Programar snapshots automáticos"),
                          QStringLiteral("Schedule automatic snapshots"),
                          QStringLiteral("计划自动快照")));
    QAction* aRollback = menu.addAction(QStringLiteral("Rollback"));
    QAction* aNewHold = menu.addAction(m_mainWindow->trk(QStringLiteral("t_new_hold_title001"), QStringLiteral("Nuevo Hold")));
    QAction* aReleaseHold = menu.addAction(
        holdContextName.isEmpty()
            ? m_mainWindow->trk(QStringLiteral("t_release_hold_title001"), QStringLiteral("Release"))
            : QStringLiteral("%1 %2").arg(
                  m_mainWindow->trk(QStringLiteral("t_release_hold_title001"), QStringLiteral("Release")),
                  holdContextName));
    menu.addSeparator();
    QAction* aBreakdown = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_breakdown_btn1"), QStringLiteral("Desglosar"), QStringLiteral("Break down"), QStringLiteral("拆分")));
    QAction* aAssemble = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("组装")));
    QAction* aFromDir = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_from_dir_btn1"), QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")));
    QAction* aToDir = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_to_dir_btn_001"), QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")));
    menu.addSeparator();
    QAction* aSelectOrigin = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_ctx_select_as_origin_001"),
                          QStringLiteral("Seleccionar como origen"),
                          QStringLiteral("Select as source"),
                          QStringLiteral("设为源")));
    QAction* aSelectDestination = menu.addAction(
        m_mainWindow->trk(QStringLiteral("t_ctx_select_as_dest_001"),
                          QStringLiteral("Seleccionar como destino"),
                          QStringLiteral("Select as destination"),
                          QStringLiteral("设为目标")));

    const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    auto selectionForMenuContext = [&]() -> SelectionSnapshot {
        SelectionSnapshot snapshot = currentSelection(tree, token);
        if ((!snapshot.valid || snapshot.datasetName.trimmed().isEmpty()) && isPoolRoot
            && connIdx >= 0 && !poolName.isEmpty()) {
            snapshot.valid = true;
            snapshot.connIdx = connIdx;
            snapshot.poolName = poolName;
            snapshot.datasetName = poolName;
            snapshot.snapshotName.clear();
        }
        return snapshot;
    };
    const SelectionSnapshot ctx = selectionForMenuContext();
    MainWindow::DatasetSelectionContext mwSelCtx;
    mwSelCtx.valid = ctx.valid;
    mwSelCtx.connIdx = ctx.connIdx;
    mwSelCtx.poolName = ctx.poolName;
    mwSelCtx.datasetName = ctx.datasetName;
    mwSelCtx.snapshotName = ctx.snapshotName;
    const bool hasConnSel = ctx.valid && !ctx.datasetName.isEmpty();
    const bool hasConnSnap = hasConnSel && !ctx.snapshotName.isEmpty();
    const bool menuOpenedOnDatasetNode =
        !item->data(0, Qt::UserRole).toString().trimmed().isEmpty()
        && !item->data(0, kConnPropRowRole).toBool()
        && !item->data(0, kConnPropGroupNodeRole).toBool()
        && !item->data(0, kConnPermissionsNodeRole).toBool();
    auto datasetPropFromModel = [this](const SelectionSnapshot& c, const QString& prop) -> QString {
        if (!c.valid || c.datasetName.isEmpty() || !c.snapshotName.isEmpty()) {
            return QString();
        }
        const QVector<MainWindow::DatasetPropCacheRow> rows =
            m_mainWindow->datasetPropertyRowsFromModelOrCache(c.connIdx, c.poolName, c.datasetName);
        for (const MainWindow::DatasetPropCacheRow& row : rows) {
            if (row.prop.compare(prop, Qt::CaseInsensitive) == 0) {
                return row.value.trimmed();
            }
        }
        return QString();
    };
    auto effectiveGsaValuesForDataset = [&](const QString& datasetName) -> QMap<QString, QString> {
        QMap<QString, QString> values;
        if (datasetName.trimmed().isEmpty() || !ctx.valid || ctx.connIdx < 0 || ctx.poolName.isEmpty()) {
            return values;
        }
        const QVector<MainWindow::DatasetPropCacheRow> rows =
            m_mainWindow->datasetPropertyRowsFromModelOrCache(ctx.connIdx, ctx.poolName, datasetName);
        for (const MainWindow::DatasetPropCacheRow& row : rows) {
            if (isGsaProp(row.prop)) {
                values[row.prop] = row.value;
            }
        }
        const MainWindow::DatasetPropsDraft draft =
            m_mainWindow->propertyDraftForObject(QStringLiteral("conncontent"), token, datasetName);
        if (draft.dirty) {
            for (auto it = draft.valuesByProp.cbegin(); it != draft.valuesByProp.cend(); ++it) {
                if (isGsaProp(it.key())) {
                    const QString existingKey = findCaseInsensitivePropKey(values, it.key());
                    values[existingKey.isEmpty() ? it.key() : existingKey] = it.value();
                }
            }
            for (auto it = draft.inheritByProp.cbegin(); it != draft.inheritByProp.cend(); ++it) {
                if (!it.value() || !isGsaProp(it.key())) {
                    continue;
                }
                const QString existingKey = findCaseInsensitivePropKey(values, it.key());
                values.remove(existingKey.isEmpty() ? it.key() : existingKey);
            }
        }
        return values;
    };
    auto recursiveGsaAncestorForDataset = [&](const QString& datasetName) -> QString {
        QString parent = datasetName.trimmed();
        while (parent.contains(QLatin1Char('/'))) {
            parent = parent.section(QLatin1Char('/'), 0, -2);
            const QMap<QString, QString> values = effectiveGsaValuesForDataset(parent);
            const QString activeKey = findCaseInsensitivePropKey(values, QStringLiteral("org.fc16.gsa:activado"));
            const QString recursiveKey = findCaseInsensitivePropKey(values, QStringLiteral("org.fc16.gsa:recursivo"));
            if (!activeKey.isEmpty() && !recursiveKey.isEmpty()
                && gsaValueOn(values.value(activeKey))
                && gsaValueOn(values.value(recursiveKey))) {
                return parent;
            }
        }
        return QString();
    };
    const QString encryptionRoot = datasetPropFromModel(ctx, QStringLiteral("encryptionroot"));
    const QString objectType = datasetPropFromModel(ctx, QStringLiteral("type")).toLower();
    const QString keyStatus = datasetPropFromModel(ctx, QStringLiteral("keystatus")).toLower();
    const QString keyLocation = datasetPropFromModel(ctx, QStringLiteral("keylocation")).toLower();
    const QString keyFormat = datasetPropFromModel(ctx, QStringLiteral("keyformat")).toLower();
    const bool isEncryptionRoot =
            hasConnSel && !hasConnSnap
            && !encryptionRoot.isEmpty()
            && encryptionRoot.compare(ctx.datasetName, Qt::CaseInsensitive) == 0;
        const bool keyLoaded = (keyStatus == QStringLiteral("available"));
        aLoadKey->setEnabled(isEncryptionRoot && !keyLoaded);
        aUnloadKey->setEnabled(isEncryptionRoot && keyLoaded);
        aChangeKey->setEnabled(isEncryptionRoot && keyLoaded);
        mEncryption->setEnabled(isEncryptionRoot);
        inlineActions.manage->setEnabled(hasConnSel);
        aRollback->setEnabled(!m_mainWindow->actionsLocked() && hasConnSnap);
        aCreate->setEnabled(!m_mainWindow->actionsLocked() && hasConnSel && !hasConnSnap);
        aRename->setEnabled(!m_mainWindow->actionsLocked() && hasConnSel);
        aNewHold->setEnabled(!m_mainWindow->actionsLocked() && hasConnSnap);
        aReleaseHold->setEnabled(!m_mainWindow->actionsLocked() && hasConnSnap && isSnapshotHoldContext);
    aDelete->setEnabled(!m_mainWindow->actionsLocked() && hasConnSel);
    if (isPoolRoot) {
        aDelete->setEnabled(false);
    }
        aBreakdown->setEnabled(!m_mainWindow->actionsLocked() && m_mainWindow->connAdvancedDatasetActionAllowed(mwSelCtx));
        aAssemble->setEnabled(!m_mainWindow->actionsLocked() && m_mainWindow->connAdvancedDatasetActionAllowed(mwSelCtx));
        aFromDir->setEnabled(!m_mainWindow->actionsLocked() && m_mainWindow->connDirectoryDatasetActionAllowed(mwSelCtx));
        aToDir->setEnabled(!m_mainWindow->actionsLocked() && m_mainWindow->connDirectoryDatasetActionAllowed(mwSelCtx));
        aSelectOrigin->setEnabled(hasConnSel);
        aSelectDestination->setEnabled(hasConnSel);
        bool scheduleEnabled = false;
        if (!m_mainWindow->actionsLocked() && hasConnSel && !hasConnSnap
            && menuOpenedOnDatasetNode
            && objectType != QStringLiteral("volume")) {
            const QMap<QString, QString> selfGsaValues = effectiveGsaValuesForDataset(ctx.datasetName);
            const QString selfActiveKey = findCaseInsensitivePropKey(selfGsaValues, QStringLiteral("org.fc16.gsa:activado"));
            const bool selfAlreadyActive =
                !selfActiveKey.isEmpty() && gsaValueOn(selfGsaValues.value(selfActiveKey));
            const bool ancestorAlreadyActive = !recursiveGsaAncestorForDataset(ctx.datasetName).isEmpty();
            scheduleEnabled = !selfAlreadyActive && !ancestorAlreadyActive;
        }
        aScheduleSnapshots->setEnabled(scheduleEnabled);

        logContextMenuPerf(m_mainWindow,
                           QStringLiteral("general.exec"),
                           QStringLiteral("conn=%1 pool=%2 dataset=%3 snapshot=%4")
                               .arg(connIdx)
                               .arg(poolName)
                               .arg(ctx.datasetName)
                               .arg(ctx.snapshotName),
                           timer.elapsed());
        endBusy();
        QAction* picked = menu.exec(tree->viewport()->mapToGlobal(pos));
        if (!picked) {
            return;
        }
        if (isPoolRoot) {
            if (picked == poolActions.update && poolMenuState.canRefresh) {
                m_mainWindow->refreshPoolStatusNow(connIdx, poolName);
                return;
            }
            if (picked == poolActions.importPool && poolMenuState.canImport && poolRow >= 0) {
                m_mainWindow->importPoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.importRename && poolMenuState.canImport && poolRow >= 0) {
                m_mainWindow->importPoolRenamingFromRow(poolRow);
                return;
            }
            if (picked == poolActions.exportPool && poolMenuState.canExport && poolRow >= 0) {
                m_mainWindow->exportPoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.history && poolMenuState.canHistory && poolRow >= 0) {
                m_mainWindow->showPoolHistoryFromRow(poolRow);
                return;
            }
            if (picked == poolActions.sync && poolMenuState.canSync && poolRow >= 0) {
                m_mainWindow->syncPoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.scrub && poolMenuState.canScrub && poolRow >= 0) {
                m_mainWindow->scrubPoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.upgrade && poolMenuState.canUpgrade && poolRow >= 0) {
                m_mainWindow->upgradePoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.reguid && poolMenuState.canReguid && poolRow >= 0) {
                m_mainWindow->reguidPoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.trim && poolMenuState.canTrim && poolRow >= 0) {
                m_mainWindow->trimPoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.initialize && poolMenuState.canInitialize && poolRow >= 0) {
                m_mainWindow->initializePoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.clear && poolMenuState.canClear && poolRow >= 0) {
                m_mainWindow->clearPoolFromRow(poolRow);
                return;
            }
            if (picked == poolActions.destroy && poolMenuState.canDestroy && poolRow >= 0) {
                m_mainWindow->destroyPoolFromRow(poolRow);
                return;
            }
        }
        if (picked == inlineActions.manage) {
            manageInlinePropsVisualization(tree, item, false);
            return;
        }
        if (picked == aScheduleSnapshots) {
            const SelectionSnapshot actx = selectionForMenuContext();
            if (!actx.valid || actx.datasetName.isEmpty() || !actx.snapshotName.isEmpty()) {
                return;
            }
            for (const QString& prop : gsaUserPropsForScheduling()) {
                const QString value = prop.compare(QStringLiteral("org.fc16.gsa:activado"), Qt::CaseInsensitive) == 0
                                          ? QStringLiteral("on")
                                          : gsaDefaultValueForScheduling(prop);
                m_mainWindow->updateConnContentDraftInherit(token, actx.datasetName, prop, false);
                m_mainWindow->updateConnContentDraftValue(token, actx.datasetName, prop, value);
            }
            m_mainWindow->m_propsDirty = true;
            m_mainWindow->updateApplyPropsButtonState();
            refreshAllTreesForTokenAndDataset(token, actx.datasetName);
            return;
        }
        if (picked == aNewHold) {
            createSnapshotHold(tree, item);
            return;
        }
        if (picked == aReleaseHold) {
            releaseSnapshotHold(tree, item);
            return;
        }
        if (picked == aRollback) {
            const SelectionSnapshot actx = selectionForMenuContext();
            if (!actx.valid || actx.snapshotName.isEmpty()) {
                return;
            }
            const QString snapObj = QStringLiteral("%1@%2").arg(actx.datasetName, actx.snapshotName);
            QDialog dlg(m_mainWindow);
            dlg.setWindowTitle(QStringLiteral("Rollback"));
            dlg.setModal(true);
            auto* vbox = new QVBoxLayout(&dlg);
            auto* intro = new QLabel(
                QStringLiteral("Configure los parámetros para rollback del snapshot:\n%1")
                    .arg(snapObj),
                &dlg);
            intro->setWordWrap(true);
            vbox->addWidget(intro);

            auto* optsForm = new QFormLayout();
            optsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

            QCheckBox* rollbackRecursiveCb = new QCheckBox(QStringLiteral("-r"), &dlg);
            auto* rollbackRecursiveDesc = new QLabel(
                QStringLiteral("Elimina snapshots más recientes del dataset para permitir el rollback."),
                &dlg);
            rollbackRecursiveDesc->setWordWrap(true);
            rollbackRecursiveCb->setToolTip(rollbackRecursiveDesc->text());
            optsForm->addRow(rollbackRecursiveCb, rollbackRecursiveDesc);

            QCheckBox* rollbackRecursiveDestroyCb = new QCheckBox(QStringLiteral("-R"), &dlg);
            auto* rollbackRecursiveDestroyDesc = new QLabel(
                QStringLiteral("Como -r, y además elimina clones y dependencias de esos snapshots."),
                &dlg);
            rollbackRecursiveDestroyDesc->setWordWrap(true);
            rollbackRecursiveDestroyCb->setToolTip(rollbackRecursiveDestroyDesc->text());
            optsForm->addRow(rollbackRecursiveDestroyCb, rollbackRecursiveDestroyDesc);

            QCheckBox* rollbackForceCb = new QCheckBox(QStringLiteral("-f"), &dlg);
            auto* rollbackForceDesc = new QLabel(
                QStringLiteral("Fuerza el desmontaje del filesystem si es necesario antes del rollback."),
                &dlg);
            rollbackForceDesc->setWordWrap(true);
            rollbackForceCb->setToolTip(rollbackForceDesc->text());
            optsForm->addRow(rollbackForceCb, rollbackForceDesc);

            QObject::connect(rollbackRecursiveDestroyCb, &QCheckBox::toggled, &dlg,
                             [rollbackRecursiveCb](bool on) {
                                 if (on) {
                                     rollbackRecursiveCb->setChecked(true);
                                 }
                             });

            vbox->addLayout(optsForm);

            auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            vbox->addWidget(bb);
            if (dlg.exec() != QDialog::Accepted) {
                return;
            }
            m_mainWindow->logUiAction(QStringLiteral("Rollback snapshot (menú Contenido)"));
            QString q = snapObj;
            q.replace('\'', "'\"'\"'");
            QStringList rollbackFlags;
            if (rollbackForceCb->isChecked()) {
                rollbackFlags.push_back(QStringLiteral("-f"));
            }
            if (rollbackRecursiveDestroyCb->isChecked()) {
                rollbackFlags.push_back(QStringLiteral("-R"));
            } else if (rollbackRecursiveCb->isChecked()) {
                rollbackFlags.push_back(QStringLiteral("-r"));
            }
            const QString cmd = rollbackFlags.isEmpty()
                                    ? QStringLiteral("zfs rollback '%1'").arg(q)
                                    : QStringLiteral("zfs rollback %1 '%2'")
                                          .arg(rollbackFlags.join(QLatin1Char(' ')), q);
            MainWindow::DatasetSelectionContext mwActx;
            mwActx.valid = actx.valid;
            mwActx.connIdx = actx.connIdx;
            mwActx.poolName = actx.poolName;
            mwActx.datasetName = actx.datasetName;
            mwActx.snapshotName = actx.snapshotName;
            ConnectionProfile cp = m_mainWindow->m_profiles[actx.connIdx];
            if (m_mainWindow->isLocalConnection(cp) && !m_mainWindow->isWindowsConnection(cp)) {
                cp.useSudo = true;
                if (!m_mainWindow->ensureLocalSudoCredentials(cp)) {
                    m_mainWindow->appLog(QStringLiteral("INFO"), QStringLiteral("Rollback cancelada: faltan credenciales sudo locales"));
                    return;
                }
            }
            const QString fullCmd = m_mainWindow->sshExecFromLocal(cp, m_mainWindow->withSudo(cp, cmd));
            MainWindow::DatasetSelectionContext refreshTarget = mwActx;
            refreshTarget.snapshotName.clear();
            const QString connLabel = cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
            QString errorText;
            if (!m_mainWindow->queuePendingShellAction(MainWindow::PendingShellActionDraft{
                    QStringLiteral("%1::%2").arg(connLabel, actx.poolName),
                    QStringLiteral("Rollback snapshot %1").arg(snapObj),
                    fullCmd,
                    90000,
                    false,
                    {},
                    refreshTarget,
                    MainWindow::PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
                QMessageBox::warning(m_mainWindow, QStringLiteral("ZFSMgr"), errorText);
                return;
            }
            m_mainWindow->appLog(QStringLiteral("NORMAL"),
                                 QStringLiteral("Cambio pendiente añadido: %1::%2  Rollback snapshot %3")
                                     .arg(connLabel, actx.poolName, snapObj));
            m_mainWindow->updateApplyPropsButtonState();
            return;
        }
        if (picked == aCreate) {
            const SelectionSnapshot actx = selectionForMenuContext();
            MainWindow::DatasetSelectionContext mwActx;
            mwActx.valid = actx.valid;
            mwActx.connIdx = actx.connIdx;
            mwActx.poolName = actx.poolName;
            mwActx.datasetName = actx.datasetName;
            mwActx.snapshotName = actx.snapshotName;
            m_mainWindow->logUiAction(QStringLiteral("Crear hijo dataset (menú Contenido)"));
            m_mainWindow->actionCreateChildDataset(QStringLiteral("conncontent"), mwActx);
            return;
        }
        if (picked == aRename) {
            const SelectionSnapshot actx = selectionForMenuContext();
            if (!actx.valid || actx.datasetName.trimmed().isEmpty()) {
                return;
            }
            if (isPoolRoot) {
                QMessageBox::information(
                    m_mainWindow,
                    QStringLiteral("ZFSMgr"),
                    m_mainWindow->trk(QStringLiteral("t_pool_rename_info_001"),
                                      QStringLiteral("Para renombrar un pool hay que exportarlo y volver a importarlo con el nuevo nombre."),
                                      QStringLiteral("To rename a pool, export it and import it again with the new name."),
                                      QStringLiteral("要重命名存储池，必须先导出，再以新名称重新导入。")));
                return;
            }
            const bool isSnapshot = !actx.snapshotName.trimmed().isEmpty();
            const QString currentObject = isSnapshot
                ? QStringLiteral("%1@%2").arg(actx.datasetName.trimmed(), actx.snapshotName.trimmed())
                : actx.datasetName.trimmed();
            const QString currentLeaf = isSnapshot ? actx.snapshotName.trimmed() : datasetLeafNameUi(actx.datasetName);
            bool ok = false;
            const QString newLeaf = QInputDialog::getText(
                m_mainWindow,
                m_mainWindow->trk(QStringLiteral("t_ctx_rename_001"),
                                  QStringLiteral("Renombrar"),
                                  QStringLiteral("Rename"),
                                  QStringLiteral("重命名")),
                m_mainWindow->trk(QStringLiteral("t_new_name_001"),
                                  QStringLiteral("Nuevo nombre"),
                                  QStringLiteral("New name"),
                                  QStringLiteral("新名称")),
                QLineEdit::Normal,
                currentLeaf,
                &ok).trimmed();
            if (!ok || newLeaf.isEmpty() || newLeaf == currentLeaf) {
                return;
            }
            if (newLeaf.contains(QLatin1Char('/')) || newLeaf.contains(QLatin1Char('@'))) {
                QMessageBox::warning(
                    m_mainWindow,
                    QStringLiteral("ZFSMgr"),
                    m_mainWindow->trk(QStringLiteral("t_invalid_ds_name_001"),
                                      QStringLiteral("El nuevo nombre no puede contener '/' ni '@'."),
                                      QStringLiteral("The new name cannot contain '/' or '@'."),
                                      QStringLiteral("新名称不能包含“/”或“@”。")));
                return;
            }
            const QString parentName = mwhelpers::parentDatasetName(actx.datasetName.trimmed());
            if (!isSnapshot && parentName.isEmpty()) {
                QMessageBox::warning(
                    m_mainWindow,
                    QStringLiteral("ZFSMgr"),
                    m_mainWindow->trk(QStringLiteral("t_invalid_ds_target_001"),
                                      QStringLiteral("El nuevo nombre no puede aplicarse a este dataset."),
                                      QStringLiteral("The new name cannot be applied to this dataset."),
                                      QStringLiteral("该新名称无法应用到此数据集。")));
                return;
            }
            const QString targetObject = isSnapshot
                ? QStringLiteral("%1@%2").arg(actx.datasetName.trimmed(), newLeaf)
                : QStringLiteral("%1/%2").arg(parentName, newLeaf);
            QString errorText;
            if (!m_mainWindow->queuePendingDatasetRename(
                    MainWindow::PendingDatasetRenameDraft{actx.connIdx, actx.poolName, currentObject, targetObject},
                    &errorText)) {
                QMessageBox::warning(m_mainWindow, QStringLiteral("ZFSMgr"), errorText);
                return;
            }
            m_mainWindow->updateApplyPropsButtonState();
            return;
        }
        if (picked == aDelete) {
            const SelectionSnapshot actx = selectionForMenuContext();
            MainWindow::DatasetSelectionContext mwActx;
            mwActx.valid = actx.valid;
            mwActx.connIdx = actx.connIdx;
            mwActx.poolName = actx.poolName;
            mwActx.datasetName = actx.datasetName;
            mwActx.snapshotName = actx.snapshotName;
            m_mainWindow->logUiAction(QStringLiteral("Borrar dataset/snapshot (menú Contenido)"));
            m_mainWindow->actionDeleteDatasetOrSnapshot(QStringLiteral("conncontent"), mwActx);
            return;
        }
        if (picked == aSelectOrigin || picked == aSelectDestination) {
            MainWindow::DatasetSelectionContext mwCtx;
            mwCtx.valid = ctx.valid;
            mwCtx.connIdx = ctx.connIdx;
            mwCtx.poolName = ctx.poolName;
            mwCtx.datasetName = ctx.datasetName;
            mwCtx.snapshotName = ctx.snapshotName;
            if (picked == aSelectOrigin) {
                m_mainWindow->m_topDetailConnIdx = ctx.connIdx;
                m_mainWindow->setConnectionOriginSelection(mwCtx);
            } else {
                mwCtx.snapshotName.clear();
                m_mainWindow->setConnectionDestinationSelection(mwCtx);
            }
            m_mainWindow->updatePoolManagementBoxTitle();
            return;
        }
        if (picked == aLoadKey || picked == aUnloadKey || picked == aChangeKey) {
            auto shQuote = [](QString s) {
                s.replace('\'', QStringLiteral("'\"'\"'"));
                return QStringLiteral("'%1'").arg(s);
            };
            QString actionName;
            QString cmd;
            QByteArray stdinPayload;
            if (picked == aLoadKey) {
                actionName = m_mainWindow->trk(QStringLiteral("t_load_key_001"),
                                               QStringLiteral("Cargar clave"),
                                               QStringLiteral("Load key"),
                                               QStringLiteral("加载密钥"));
                cmd = QStringLiteral("zfs load-key %1").arg(shQuote(ctx.datasetName));
            } else if (picked == aUnloadKey) {
                actionName = QStringLiteral("Unload key");
                cmd = QStringLiteral("zfs unload-key %1").arg(shQuote(ctx.datasetName));
            } else {
                actionName = QStringLiteral("Change key");
                cmd = QStringLiteral("zfs change-key -o keylocation=prompt%1 %2")
                          .arg(keyFormat == QStringLiteral("passphrase")
                                   ? QStringLiteral(" -o keyformat=passphrase")
                                   : QString(),
                               shQuote(ctx.datasetName));
            }
            if (picked == aChangeKey) {
                QString newPassphrase;
                if (!promptNewPassphrase(actionName, newPassphrase)) {
                    return;
                }
                stdinPayload = (newPassphrase + QStringLiteral("\n") + newPassphrase + QStringLiteral("\n")).toUtf8();
                executeDatasetActionWithStdin(QStringLiteral("conncontent"),
                                             actionName,
                                             ctx.connIdx,
                                             ctx.poolName,
                                             ctx.datasetName,
                                             ctx.snapshotName,
                                             cmd,
                                             stdinPayload,
                                             90000);
            } else if (picked == aLoadKey && keyLocation == QStringLiteral("prompt")) {
                bool ok = false;
                const QString passphrase = QInputDialog::getText(
                    m_mainWindow,
                    actionName,
                    QStringLiteral("Clave"),
                    QLineEdit::Password,
                    QString(),
                    &ok);
                if (!ok || passphrase.isEmpty()) {
                    return;
                }
                stdinPayload = (passphrase + QStringLiteral("\n")).toUtf8();
                executeDatasetActionWithStdin(QStringLiteral("conncontent"),
                                             actionName,
                                             ctx.connIdx,
                                             ctx.poolName,
                                             ctx.datasetName,
                                             ctx.snapshotName,
                                             cmd,
                                             stdinPayload,
                                             90000);
            } else {
                MainWindow::DatasetSelectionContext mwCtx;
                mwCtx.valid = ctx.valid;
                mwCtx.connIdx = ctx.connIdx;
                mwCtx.poolName = ctx.poolName;
                mwCtx.datasetName = ctx.datasetName;
                mwCtx.snapshotName = ctx.snapshotName;
                m_mainWindow->executeDatasetAction(QStringLiteral("conncontent"), actionName, mwCtx, cmd, 90000);
            }
            return;
        }
        if (picked == aBreakdown) {
            MainWindow::DatasetSelectionContext mwCtx;
            mwCtx.valid = ctx.valid;
            mwCtx.connIdx = ctx.connIdx;
            mwCtx.poolName = ctx.poolName;
            mwCtx.datasetName = ctx.datasetName;
            mwCtx.snapshotName = ctx.snapshotName;
            m_mainWindow->actionAdvancedBreakdown(mwCtx);
            return;
        }
        if (picked == aAssemble) {
            MainWindow::DatasetSelectionContext mwCtx;
            mwCtx.valid = ctx.valid;
            mwCtx.connIdx = ctx.connIdx;
            mwCtx.poolName = ctx.poolName;
            mwCtx.datasetName = ctx.datasetName;
            mwCtx.snapshotName = ctx.snapshotName;
            m_mainWindow->actionAdvancedAssemble(mwCtx);
            return;
        }
        if (picked == aFromDir) {
            MainWindow::DatasetSelectionContext mwCtx;
            mwCtx.valid = ctx.valid;
            mwCtx.connIdx = ctx.connIdx;
            mwCtx.poolName = ctx.poolName;
            mwCtx.datasetName = ctx.datasetName;
            mwCtx.snapshotName = ctx.snapshotName;
            m_mainWindow->actionAdvancedCreateFromDir(mwCtx);
            return;
        }
        if (picked == aToDir) {
            MainWindow::DatasetSelectionContext mwCtx;
            mwCtx.valid = ctx.valid;
            mwCtx.connIdx = ctx.connIdx;
            mwCtx.poolName = ctx.poolName;
            mwCtx.datasetName = ctx.datasetName;
            mwCtx.snapshotName = ctx.snapshotName;
            m_mainWindow->actionAdvancedToDir(mwCtx);
            return;
        }
}
