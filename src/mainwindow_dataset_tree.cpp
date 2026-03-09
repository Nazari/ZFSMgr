#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <functional>

namespace {
using mwhelpers::parentDatasetName;
constexpr int kSnapshotListRole = Qt::UserRole + 1;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;

QTreeWidgetItem* findDatasetItem(QTreeWidget* tree, const QString& datasetName) {
    if (!tree || datasetName.isEmpty()) {
        return nullptr;
    }
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
        if (!n) {
            return nullptr;
        }
        if (n->data(0, Qt::UserRole).toString() == datasetName) {
            return n;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            if (QTreeWidgetItem* f = rec(n->child(i))) {
                return f;
            }
        }
        return nullptr;
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* f = rec(tree->topLevelItem(i))) {
            return f;
        }
    }
    return nullptr;
}
} // namespace

void MainWindow::saveConnContentTreeState(const QString& token) {
    if (token.isEmpty() || !m_connContentTree) {
        return;
    }
    ConnContentTreeState st;
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString ds = n->data(0, Qt::UserRole).toString();
        if (!ds.isEmpty()) {
            if (n->isExpanded()) {
                st.expandedDatasets.push_back(ds);
            }
            const QString snap = n->data(1, Qt::UserRole).toString();
            if (!snap.isEmpty()) {
                st.snapshotByDataset.insert(ds, snap);
            }
        }
        for (int i = 0; i < n->childCount(); ++i) {
            rec(n->child(i));
        }
    };
    for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
        rec(m_connContentTree->topLevelItem(i));
    }
    const auto selected = m_connContentTree->selectedItems();
    if (!selected.isEmpty()) {
        st.selectedDataset = selected.first()->data(0, Qt::UserRole).toString();
        st.selectedSnapshot = selected.first()->data(1, Qt::UserRole).toString();
    }
    m_connContentTreeStateByToken[token] = st;
}

void MainWindow::restoreConnContentTreeState(const QString& token) {
    if (token.isEmpty() || !m_connContentTree) {
        return;
    }
    const auto it = m_connContentTreeStateByToken.constFind(token);
    if (it == m_connContentTreeStateByToken.cend()) {
        return;
    }
    const ConnContentTreeState& st = it.value();
    const QSet<QString> expandedSet(st.expandedDatasets.cbegin(), st.expandedDatasets.cend());

    std::function<void(QTreeWidgetItem*)> applyExpand = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString ds = n->data(0, Qt::UserRole).toString();
        if (!ds.isEmpty()) {
            n->setExpanded(expandedSet.contains(ds));
        }
        for (int i = 0; i < n->childCount(); ++i) {
            applyExpand(n->child(i));
        }
    };
    for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
        applyExpand(m_connContentTree->topLevelItem(i));
    }

    for (auto sit = st.snapshotByDataset.cbegin(); sit != st.snapshotByDataset.cend(); ++sit) {
        QTreeWidgetItem* item = findDatasetItem(m_connContentTree, sit.key());
        if (!item) {
            continue;
        }
        if (QComboBox* cb = qobject_cast<QComboBox*>(m_connContentTree->itemWidget(item, 1))) {
            const int idx = cb->findText(sit.value());
            if (idx > 0) {
                const QSignalBlocker blocker(cb);
                cb->setCurrentIndex(idx);
                item->setData(1, Qt::UserRole, sit.value());
            }
        }
    }

    if (!st.selectedDataset.isEmpty()) {
        QTreeWidgetItem* sel = findDatasetItem(m_connContentTree, st.selectedDataset);
        if (sel) {
            if (!st.selectedSnapshot.isEmpty()) {
                if (QComboBox* cb = qobject_cast<QComboBox*>(m_connContentTree->itemWidget(sel, 1))) {
                    const int idx = cb->findText(st.selectedSnapshot);
                    if (idx > 0) {
                        const QSignalBlocker blocker(cb);
                        cb->setCurrentIndex(idx);
                        sel->setData(1, Qt::UserRole, st.selectedSnapshot);
                    }
                }
            }
            for (QTreeWidgetItem* p = sel->parent(); p; p = p->parent()) {
                p->setExpanded(true);
            }
            m_connContentTree->setCurrentItem(sel);
            m_connContentTree->scrollToItem(sel, QAbstractItemView::PositionAtCenter);
        }
    }
}

void MainWindow::onOriginPoolChanged() {
    m_originSelectedDataset.clear();
    m_originSelectedSnapshot.clear();
    const QString token = m_originPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_originTree->clear();
        m_originSelectionLabel->setText(trk(QStringLiteral("t_no_sel_001"),
                                            QStringLiteral("(sin selección)"),
                                            QStringLiteral("(no selection)"),
                                            QStringLiteral("（未选择）")));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_originTree, connIdx, poolName, QStringLiteral("origin"));
    refreshDatasetProperties(QStringLiteral("origin"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onDestPoolChanged() {
    m_destSelectedDataset.clear();
    m_destSelectedSnapshot.clear();
    const QString token = m_destPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_destTree->clear();
        m_destSelectionLabel->setText(trk(QStringLiteral("t_no_sel_001"),
                                          QStringLiteral("(sin selección)"),
                                          QStringLiteral("(no selection)"),
                                          QStringLiteral("（未选择）")));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_destTree, connIdx, poolName, QStringLiteral("dest"));
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onAdvancedPoolChanged() {
    QString prevDataset;
    QString prevSnapshot;
    if (m_advTree) {
        const auto selected = m_advTree->selectedItems();
        if (!selected.isEmpty()) {
            prevDataset = selected.first()->data(0, Qt::UserRole).toString();
            prevSnapshot = selected.first()->data(1, Qt::UserRole).toString();
        }
    }

    const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        if (m_advTree) {
            m_advTree->clear();
        }
        updateAdvancedSelectionUi(QString(), QString());
        refreshDatasetProperties(QStringLiteral("advanced"));
        updateTransferButtonsState();
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_advTree, connIdx, poolName, QStringLiteral("advanced"));

    QTreeWidgetItem* restored = nullptr;
    if (m_advTree && !prevDataset.isEmpty()) {
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findMatch = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
            if (!node) {
                return nullptr;
            }
            const QString ds = node->data(0, Qt::UserRole).toString();
            const QString snap = node->data(1, Qt::UserRole).toString();
            if (ds == prevDataset && (prevSnapshot.isEmpty() || snap == prevSnapshot)) {
                return node;
            }
            for (int i = 0; i < node->childCount(); ++i) {
                if (auto* found = findMatch(node->child(i))) {
                    return found;
                }
            }
            return nullptr;
        };
        for (int i = 0; i < m_advTree->topLevelItemCount() && !restored; ++i) {
            restored = findMatch(m_advTree->topLevelItem(i));
        }
    }

    if (m_advTree && restored) {
        for (QTreeWidgetItem* p = restored->parent(); p; p = p->parent()) {
            m_advTree->expandItem(p);
        }
        m_advTree->setCurrentItem(restored);
        m_advTree->scrollToItem(restored, QAbstractItemView::PositionAtCenter);
    } else {
        updateAdvancedSelectionUi(QString(), QString());
    }
    refreshDatasetProperties(QStringLiteral("advanced"));
    updateTransferButtonsState();
}

void MainWindow::onOriginTreeSelectionChanged() {
    const auto selected = m_originTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("origin"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("origin"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onDestTreeSelectionChanged() {
    const auto selected = m_destTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("dest"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("dest"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onOriginTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    Q_UNUSED(item);
    Q_UNUSED(col);
}

void MainWindow::onDestTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    Q_UNUSED(item);
    Q_UNUSED(col);
}

void MainWindow::populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side, bool allowRemoteLoadIfMissing) {
    if (!tree) {
        return;
    }
    beginUiBusy();
    m_loadingDatasetTrees = true;
    tree->clear();
    if (!ensureDatasetsLoaded(connIdx, poolName, allowRemoteLoadIfMissing)) {
        m_loadingDatasetTrees = false;
        endUiBusy();
        return;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    const PoolDatasetCache& cache = m_poolDatasetCache[key];

    QMap<QString, QTreeWidgetItem*> byName;
    for (const DatasetRecord& rec : cache.datasets) {
        auto* item = new QTreeWidgetItem();
        const QString displayName = rec.name.contains('/')
                                        ? rec.name.section('/', -1, -1)
                                        : rec.name;
        item->setText(0, displayName);
        const QStringList snaps = cache.snapshotsByDataset.value(rec.name);
        item->setText(1, snaps.isEmpty() ? QString() : QStringLiteral("(ninguno)"));
        item->setData(1, Qt::UserRole, QString());
        item->setData(0, Qt::UserRole, rec.name);
        item->setData(1, kSnapshotListRole, snaps);
        item->setData(0, kConnIdxRole, connIdx);
        item->setData(0, kPoolNameRole, poolName);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const QString mounted = rec.mounted.trimmed().toLower();
        const bool isMounted = (mounted == QStringLiteral("yes")
                                || mounted == QStringLiteral("on")
                                || mounted == QStringLiteral("true")
                                || mounted == QStringLiteral("1"));
        item->setCheckState(2, isMounted ? Qt::Checked : Qt::Unchecked);
        const QString effectiveMp = effectiveMountPath(connIdx, poolName, rec.name, rec.mountpoint, rec.mounted);
        item->setText(3, effectiveMp.isEmpty() ? rec.mountpoint.trimmed() : effectiveMp);
        byName.insert(rec.name, item);
    }

    for (const DatasetRecord& rec : cache.datasets) {
        QTreeWidgetItem* item = byName.value(rec.name, nullptr);
        if (!item) {
            continue;
        }
        const QString parent = parentDatasetName(rec.name);
        QTreeWidgetItem* parentItem = byName.value(parent, nullptr);
        if (parentItem) {
            parentItem->addChild(item);
        } else {
            tree->addTopLevelItem(item);
        }
    }
    tree->expandToDepth(0);
    // Dropdown embebido en celda Snapshot, sin seleccionar ninguno al inicio.
    std::function<void(QTreeWidgetItem*)> attachCombos = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QStringList snaps = n->data(1, kSnapshotListRole).toStringList();
        if (!snaps.isEmpty()) {
            QStringList options;
            options << QStringLiteral("(ninguno)");
            options += snaps;
            auto* combo = new QComboBox(tree);
            combo->addItems(options);
            combo->setCurrentIndex(0);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
            combo->setMinimumHeight(22);
            combo->setMaximumHeight(22);
            combo->setFont(tree->font());
            combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 2px; margin:0px;}"));
            tree->setItemWidget(n, 1, combo);
            QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, n, side](const QString& txt) {
                onSnapshotComboChanged(tree, n, side, txt);
            });
        } else {
            tree->setItemWidget(n, 1, nullptr);
            n->setText(1, QString());
            n->setData(1, Qt::UserRole, QString());
        }
        for (int i = 0; i < n->childCount(); ++i) {
            attachCombos(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        attachCombos(tree->topLevelItem(i));
    }

    if (side == QStringLiteral("conncontent")) {
        restoreConnContentTreeState(m_connContentToken);
    }

    if (side == QStringLiteral("origin")) {
        m_originSelectionLabel->setText(trk(QStringLiteral("t_no_sel_001"),
                                            QStringLiteral("(sin selección)"),
                                            QStringLiteral("(no selection)"),
                                            QStringLiteral("（未选择）")));
    } else if (side == QStringLiteral("dest")) {
        m_destSelectionLabel->setText(trk(QStringLiteral("t_no_sel_001"),
                                          QStringLiteral("(sin selección)"),
                                          QStringLiteral("(no selection)"),
                                          QStringLiteral("（未选择）")));
    }
    m_loadingDatasetTrees = false;
    endUiBusy();
}

void MainWindow::clearOtherSnapshotSelections(QTreeWidget* tree, QTreeWidgetItem* keepItem) {
    std::function<void(QTreeWidgetItem*)> clearRec = [&](QTreeWidgetItem* n) {
        if (!n || n == keepItem) {
            return;
        }
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(n, 1))) {
            QSignalBlocker b(cb);
            cb->setCurrentIndex(0);
        }
        n->setData(1, Qt::UserRole, QString());
        for (int i = 0; i < n->childCount(); ++i) {
            clearRec(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        clearRec(tree->topLevelItem(i));
    }
}

void MainWindow::onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, const QString& side, const QString& chosen) {
    if (!tree || !item || m_loadingDatasetTrees) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString();
    const QString snap = (chosen == QStringLiteral("(ninguno)")) ? QString() : chosen.trimmed();
    if (!snap.isEmpty()) {
        clearOtherSnapshotSelections(tree, item);
    }
    item->setData(1, Qt::UserRole, snap);
    tree->setCurrentItem(item);
    if (side == QStringLiteral("advanced")) {
        updateAdvancedSelectionUi(ds, snap);
        refreshDatasetProperties(QStringLiteral("advanced"));
        updateTransferButtonsState();
        return;
    }
    if (side == QStringLiteral("conncontent")) {
        refreshDatasetProperties(QStringLiteral("conncontent"));
        updateConnectionActionsState();
        return;
    }
    setSelectedDataset(side, ds, snap);
}

void MainWindow::onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, const QString& side) {
    if (!tree || !item || m_loadingDatasetTrees || actionsLocked()) {
        return;
    }
    if (col != 2) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const Qt::CheckState desired = item->checkState(2);
    QString token;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();
    } else if (side == QStringLiteral("conncontent")) {
        const int itemConnIdx = item->data(0, kConnIdxRole).toInt();
        const QString itemPool = item->data(0, kPoolNameRole).toString();
        if (itemConnIdx >= 0 && !itemPool.isEmpty()) {
            token = QStringLiteral("%1::%2").arg(itemConnIdx).arg(itemPool);
        } else {
            token = m_connContentToken;
        }
    } else {
        token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        m_loadingDatasetTrees = true;
        item->setCheckState(2, desired == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        m_loadingDatasetTrees = false;
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = token.left(sep).toInt();
    ctx.poolName = token.mid(sep + 2);
    ctx.datasetName = ds;
    ctx.snapshotName = item->data(1, Qt::UserRole).toString();
    if (!ctx.snapshotName.isEmpty()) {
        m_loadingDatasetTrees = true;
        item->setCheckState(2, desired == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        m_loadingDatasetTrees = false;
        return;
    }

    bool ok = false;
    m_loadingDatasetTrees = true;
    if (desired == Qt::Checked) {
        ok = mountDataset(side, ctx);
    } else {
        ok = umountDataset(side, ctx);
    }
    m_loadingDatasetTrees = false;
    if (!ok) {
        auto findByDataset = [&](auto&& self, QTreeWidgetItem* n, const QString& name) -> QTreeWidgetItem* {
            if (!n) {
                return nullptr;
            }
            if (n->data(0, Qt::UserRole).toString() == name) {
                return n;
            }
            for (int i = 0; i < n->childCount(); ++i) {
                if (QTreeWidgetItem* f = self(self, n->child(i), name)) {
                    return f;
                }
            }
            return nullptr;
        };
        QTreeWidgetItem* safeItem = nullptr;
        for (int i = 0; i < tree->topLevelItemCount() && !safeItem; ++i) {
            safeItem = findByDataset(findByDataset, tree->topLevelItem(i), ds);
        }
        if (safeItem) {
            m_loadingDatasetTrees = true;
            safeItem->setCheckState(2, desired == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            m_loadingDatasetTrees = false;
        }
    }
}

void MainWindow::setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName) {
    if (side == QStringLiteral("origin")) {
        m_originSelectedDataset = datasetName;
        m_originSelectedSnapshot = snapshotName;
        if (datasetName.isEmpty()) {
            m_originSelectionLabel->setText(trk(QStringLiteral("t_no_sel_001"),
                                                QStringLiteral("(sin selección)"),
                                                QStringLiteral("(no selection)"),
                                                QStringLiteral("（未选择）")));
        } else if (snapshotName.isEmpty()) {
            m_originSelectionLabel->setText(datasetName);
        } else {
            m_originSelectionLabel->setText(QStringLiteral("%1@%2").arg(datasetName, snapshotName));
        }
        refreshDatasetProperties(QStringLiteral("origin"));
        refreshTransferSelectionLabels();
        updateTransferButtonsState();
        return;
    }
    m_destSelectedDataset = datasetName;
    m_destSelectedSnapshot = snapshotName;
    if (datasetName.isEmpty()) {
        m_destSelectionLabel->setText(trk(QStringLiteral("t_no_sel_001"),
                                          QStringLiteral("(sin selección)"),
                                          QStringLiteral("(no selection)"),
                                          QStringLiteral("（未选择）")));
    } else if (snapshotName.isEmpty()) {
        m_destSelectionLabel->setText(datasetName);
    } else {
        m_destSelectionLabel->setText(QStringLiteral("%1@%2").arg(datasetName, snapshotName));
    }
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::refreshTransferSelectionLabels() {
    QString originText;
    if (!m_originSelectedDataset.isEmpty()) {
        if (!m_originSelectedSnapshot.isEmpty()) {
            originText = QStringLiteral("%1@%2").arg(m_originSelectedDataset, m_originSelectedSnapshot);
        } else {
            originText = m_originSelectedDataset;
        }
    } else {
        originText = trk(QStringLiteral("t_no_sel_001"),
                         QStringLiteral("(sin selección)"),
                         QStringLiteral("(no selection)"),
                         QStringLiteral("（未选择）"));
    }
    if (m_transferOriginLabel) {
        m_transferOriginLabel->setText(originText);
    }
    if (m_originSelectionLabel) {
        m_originSelectionLabel->setText(originText);
    }

    QString destText;
    if (!m_destSelectedDataset.isEmpty()) {
        if (!m_destSelectedSnapshot.isEmpty()) {
            destText = QStringLiteral("%1@%2").arg(m_destSelectedDataset, m_destSelectedSnapshot);
        } else {
            destText = m_destSelectedDataset;
        }
    } else {
        destText = trk(QStringLiteral("t_no_sel_001"),
                       QStringLiteral("(sin selección)"),
                       QStringLiteral("(no selection)"),
                       QStringLiteral("（未选择）"));
    }
    if (m_transferDestLabel) {
        m_transferDestLabel->setText(destText);
    }
    if (m_destSelectionLabel) {
        m_destSelectionLabel->setText(destText);
    }

    if (m_transferBox) {
        const QString emptyToken = trk(QStringLiteral("t_empty_tag_001"),
                                       QStringLiteral("[vacío]"),
                                       QStringLiteral("[empty]"),
                                       QStringLiteral("[空]"));
        const QString originTitle = m_originSelectedDataset.isEmpty() ? emptyToken : originText;
        const QString destTitle = m_destSelectedDataset.isEmpty() ? emptyToken : destText;
        m_transferBox->setTitle(
            trk(QStringLiteral("t_action_from_to1"),
                QStringLiteral("Acción desde %1 hacia %2"),
                QStringLiteral("Action from %1 to %2"),
                QStringLiteral("从 %1 到 %2 的操作"))
                .arg(originTitle, destTitle));
    }
}

void MainWindow::updateAdvancedSelectionUi(const QString& datasetName, const QString& snapshotName) {
    QString text;
    if (datasetName.isEmpty()) {
        text = trk(QStringLiteral("t_no_sel_001"),
                   QStringLiteral("(sin selección)"),
                   QStringLiteral("(no selection)"),
                   QStringLiteral("（未选择）"));
    } else if (snapshotName.isEmpty()) {
        text = datasetName;
    } else {
        text = QStringLiteral("%1@%2").arg(datasetName, snapshotName);
    }
    if (m_advSelectionLabel) {
        m_advSelectionLabel->setText(text);
    }
    if (m_advCommandsBox) {
        const QString emptyTitle = trk(QStringLiteral("t_empty_tag_001"),
                                       QStringLiteral("[vacío]"),
                                       QStringLiteral("[empty]"),
                                       QStringLiteral("[空]"));
        const QString selectedTitle = datasetName.isEmpty() ? emptyTitle : text;
        m_advCommandsBox->setTitle(
            trk(QStringLiteral("t_action_on_sel01"),
                QStringLiteral("Acción sobre %1"),
                QStringLiteral("Action on %1"),
                QStringLiteral("对 %1 的操作"))
                .arg(selectedTitle));
    }
}
