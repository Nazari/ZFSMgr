#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <functional>

namespace {
using mwhelpers::parentDatasetName;
constexpr int kSnapshotListRole = Qt::UserRole + 1;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;
constexpr int kConnPropRowRole = Qt::UserRole + 13;
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kConnPropEditableRole = Qt::UserRole + 15;
constexpr int kConnPropCols = 10;

QMap<QString, QStringList> connContentEnumValues() {
    return {
        {QStringLiteral("atime"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("readonly"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("compression"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("lz4"), QStringLiteral("zstd"), QStringLiteral("gzip"), QStringLiteral("zle"), QStringLiteral("lzjb")}},
        {QStringLiteral("checksum"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("fletcher2"), QStringLiteral("fletcher4"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr"), QStringLiteral("blake3")}},
        {QStringLiteral("sync"), {QStringLiteral("standard"), QStringLiteral("always"), QStringLiteral("disabled")}},
        {QStringLiteral("logbias"), {QStringLiteral("latency"), QStringLiteral("throughput")}},
        {QStringLiteral("primarycache"), {QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("secondarycache"), {QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("dedup"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("verify"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr"), QStringLiteral("blake3")}},
        {QStringLiteral("copies"), {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}},
        {QStringLiteral("acltype"), {QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), {QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("xattr"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa"), QStringLiteral("dir")}},
        {QStringLiteral("normalization"), {QStringLiteral("none"), QStringLiteral("formC"), QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")}},
        {QStringLiteral("casesensitivity"), {QStringLiteral("sensitive"), QStringLiteral("insensitive"), QStringLiteral("mixed")}},
        {QStringLiteral("utf8only"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("canmount"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("noauto")}},
        {QStringLiteral("snapdir"), {QStringLiteral("hidden"), QStringLiteral("visible")}},
        {QStringLiteral("exec"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("setuid"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("devices"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("snapdev"), {QStringLiteral("hidden"), QStringLiteral("visible")}},
        {QStringLiteral("volmode"), {QStringLiteral("default"), QStringLiteral("full"), QStringLiteral("dev"), QStringLiteral("none"), QStringLiteral("geom")}},
    };
}

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

void MainWindow::updateConnContentPropertyValues(const QString& token,
                                                 const QString& objectName,
                                                 const QMap<QString, QString>& valuesByProp) {
    const QString t = token.trimmed().toLower();
    const QString o = objectName.trimmed().toLower();
    if (t.isEmpty() || o.isEmpty()) {
        return;
    }
    const QString key = QStringLiteral("%1|%2").arg(t, o);
    if (valuesByProp.isEmpty()) {
        m_connContentPropValuesByObject.remove(key);
    } else {
        m_connContentPropValuesByObject.insert(key, valuesByProp);
    }
}

void MainWindow::syncConnContentPropertyColumns() {
    if (!m_connContentTree) {
        return;
    }
    if (m_syncingConnContentColumns) {
        return;
    }
    m_syncingConnContentColumns = true;
    const QSignalBlocker blocker(m_connContentTree);

    QStringList headers = {
        trk(QStringLiteral("t_dataset_001"), QStringLiteral("Dataset"), QStringLiteral("Dataset"), QStringLiteral("数据集")),
        trk(QStringLiteral("t_snapshot_col01"), QStringLiteral("Snapshot"), QStringLiteral("Snapshot"), QStringLiteral("快照")),
        trk(QStringLiteral("t_montado_a97484"), QStringLiteral("Montado"), QStringLiteral("Mounted"), QStringLiteral("已挂载")),
        trk(QStringLiteral("t_mountpoint_001"), QStringLiteral("Mountpoint"), QStringLiteral("Mountpoint"), QStringLiteral("挂载点"))
    };
    for (int i = 0; i < kConnPropCols; ++i) {
        headers << trk(QStringLiteral("t_prop_hdr_001"), QStringLiteral("Prop."), QStringLiteral("Prop."), QStringLiteral("属性"));
    }
    m_connContentTree->setColumnCount(headers.size());
    m_connContentTree->setHeaderLabels(headers);
    m_connContentTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_connContentTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_connContentTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_connContentTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    for (int col = 4; col < m_connContentTree->columnCount(); ++col) {
        m_connContentTree->header()->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    // En Contenido de Pool, Snapshot/Montado/Mountpoint se gestionan dentro de "Prop.".
    m_connContentTree->setColumnHidden(0, false);
    if (m_connContentTree->columnCount() > 1) m_connContentTree->setColumnHidden(1, true);
    if (m_connContentTree->columnCount() > 2) m_connContentTree->setColumnHidden(2, true);
    if (m_connContentTree->columnCount() > 3) m_connContentTree->setColumnHidden(3, true);
    for (int col = 4; col < m_connContentTree->columnCount(); ++col) {
        m_connContentTree->setColumnHidden(col, false);
    }
    if (m_connContentTree->columnWidth(4) <= 0) {
        for (int col = 4; col < m_connContentTree->columnCount(); ++col) {
            m_connContentTree->setColumnWidth(col, 96);
        }
    }

    QTreeWidgetItem* sel = m_connContentTree->currentItem();
    if (!sel) {
        m_syncingConnContentColumns = false;
        return;
    }
    while (sel && sel->data(0, Qt::UserRole).toString().isEmpty()) {
        sel = sel->parent();
    }
    if (!sel) {
        m_syncingConnContentColumns = false;
        return;
    }
    const QString ds = sel->data(0, Qt::UserRole).toString();
    const QString snap = sel->data(1, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        m_syncingConnContentColumns = false;
        return;
    }
    const QString obj = snap.isEmpty() ? ds : QStringLiteral("%1@%2").arg(ds, snap);
    const QString key = QStringLiteral("%1|%2").arg(m_connContentToken.trimmed().toLower(),
                                                    obj.trimmed().toLower());
    const auto it = m_connContentPropValuesByObject.constFind(key);
    if (it == m_connContentPropValuesByObject.cend() || it->isEmpty()) {
        m_syncingConnContentColumns = false;
        return;
    }

    auto clearPropRowsRec = [&](auto&& self, QTreeWidgetItem* n) -> void {
        if (!n) {
            return;
        }
        for (int i = n->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* c = n->child(i);
            if (!c) {
                continue;
            }
            if (c->data(0, kConnPropRowRole).toBool()) {
                delete n->takeChild(i);
                continue;
            }
            self(self, c);
        }
        for (int col = 4; col < m_connContentTree->columnCount(); ++col) {
            if (QWidget* w = m_connContentTree->itemWidget(n, col)) {
                m_connContentTree->removeItemWidget(n, col);
                w->deleteLater();
            }
            n->setText(col, QString());
            n->setData(col, kConnPropKeyRole, QVariant());
            n->setData(col, kConnPropEditableRole, false);
        }
    };
    for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
        clearPropRowsRec(clearPropRowsRec, m_connContentTree->topLevelItem(i));
    }

    QStringList props = it->keys();
    props.sort(Qt::CaseInsensitive);
    QStringList ordered;
    const QStringList pinned = {QStringLiteral("snapshot"), QStringLiteral("estado"), QStringLiteral("mountpoint")};
    for (const QString& p : pinned) {
        for (const QString& k : props) {
            if (k.compare(p, Qt::CaseInsensitive) == 0) {
                ordered.push_back(k);
                break;
            }
        }
    }
    for (const QString& k : props) {
        bool already = false;
        for (const QString& p : ordered) {
            if (p.compare(k, Qt::CaseInsensitive) == 0) {
                already = true;
                break;
            }
        }
        if (!already) {
            ordered.push_back(k);
        }
    }
    props = ordered;
    const auto enumValues = connContentEnumValues();
    const int itemConnIdx = sel->data(0, kConnIdxRole).toInt();
    const QString itemPool = sel->data(0, kPoolNameRole).toString();
    const QString propsKey = datasetPropsCacheKey(itemConnIdx, itemPool, obj);
    auto isReadonlyFlag = [](const QString& v) {
        const QString s = v.trimmed().toLower();
        return s == QStringLiteral("true") || s == QStringLiteral("on")
               || s == QStringLiteral("yes") || s == QStringLiteral("1");
    };
    auto isEditableProp = [this, &propsKey, &isReadonlyFlag](const QString& prop) -> bool {
        if (m_connContentPropsTable) {
            for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
                QTableWidgetItem* k = m_connContentPropsTable->item(r, 0);
                if (!k) {
                    continue;
                }
                const QString key = k->data(Qt::UserRole + 777).toString().trimmed().isEmpty()
                                        ? k->text().trimmed()
                                        : k->data(Qt::UserRole + 777).toString().trimmed();
                if (key.compare(prop, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                if (m_connContentPropsTable->cellWidget(r, 1)) {
                    return true;
                }
                QTableWidgetItem* v = m_connContentPropsTable->item(r, 1);
                if (v && (v->flags() & Qt::ItemIsEditable)) {
                    return true;
                }
                break;
            }
        }
        if (prop.contains(':')) {
            return true;
        }
        const auto itCache = m_datasetPropsCache.constFind(propsKey);
        if (itCache == m_datasetPropsCache.cend() || !itCache->loaded) {
            return false;
        }
        for (const DatasetPropCacheRow& row : itCache->rows) {
            if (row.prop.compare(prop, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (isReadonlyFlag(row.readonly)) {
                return false;
            }
            if (row.source.trimmed() == QStringLiteral("-")) {
                return false;
            }
            return true;
        }
        return false;
    };
    int insertAt = 0;
    for (int base = 0; base < props.size(); base += kConnPropCols) {
        auto* rowNames = new QTreeWidgetItem();
        rowNames->setData(0, kConnPropRowRole, true);
        rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
        rowNames->setText(0, (base == 0)
                                 ? trk(QStringLiteral("t_props_lbl_001"),
                                       QStringLiteral("Propiedades"),
                                       QStringLiteral("Properties"),
                                       QStringLiteral("属性"))
                                 : QString());
        auto* rowValues = new QTreeWidgetItem();
        rowValues->setData(0, kConnPropRowRole, true);
        rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
        rowValues->setText(0, QString());
        const QColor nameRowBg(232, 240, 250);
        for (int col = 0; col < m_connContentTree->columnCount(); ++col) {
            rowNames->setBackground(col, QBrush(nameRowBg));
        }
        sel->insertChild(insertAt++, rowNames);
        sel->insertChild(insertAt++, rowValues);

        for (int off = 0; off < kConnPropCols; ++off) {
            const int idx = base + off;
            if (idx >= props.size()) {
                break;
            }
            const QString& prop = props.at(idx);
            const int col = 4 + off;
            const QString value = it->value(prop);
            const QString propLower = prop.trimmed().toLower();
            rowNames->setText(col, prop);
            rowNames->setTextAlignment(col, Qt::AlignCenter);
            rowValues->setText(col, value);
            rowValues->setTextAlignment(col, Qt::AlignCenter);
            rowValues->setData(col, kConnPropKeyRole, prop);
            const bool enumProp = enumValues.contains(propLower);
            const bool editable = isEditableProp(prop) || enumProp;
            rowValues->setData(col, kConnPropEditableRole, editable);
            if (propLower == QStringLiteral("snapshot")) {
                QStringList snaps = sel->data(1, kSnapshotListRole).toStringList();
                QStringList options;
                options << trk(QStringLiteral("t_none_paren_001"),
                               QStringLiteral("(ninguno)"),
                               QStringLiteral("(none)"),
                               QStringLiteral("（无）"));
                options += snaps;
                auto* combo = new QComboBox(m_connContentTree);
                combo->setMinimumHeight(22);
                combo->setMaximumHeight(22);
                combo->setFont(m_connContentTree->font());
                combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 2px; margin:0px;}"));
                combo->addItems(options);
                const QString currentSnap = sel->data(1, Qt::UserRole).toString().trimmed();
                const QString currentLabel = currentSnap.isEmpty() ? options.first() : currentSnap;
                combo->setCurrentText(currentLabel);
                m_connContentTree->setItemWidget(rowValues, col, combo);
                QObject::connect(combo, &QComboBox::currentTextChanged, m_connContentTree, [this, sel, rowValues, col](const QString& txt) {
                    if (!sel || !rowValues) {
                        return;
                    }
                    rowValues->setText(col, txt);
                    onSnapshotComboChanged(m_connContentTree, sel, QStringLiteral("conncontent"), txt);
                });
                rowValues->setData(col, kConnPropEditableRole, true);
                continue;
            }
            if (propLower == QStringLiteral("estado")) {
                auto* boxHost = new QWidget(m_connContentTree);
                auto* lay = new QHBoxLayout(boxHost);
                lay->setContentsMargins(0, 0, 0, 0);
                lay->setSpacing(0);
                auto* chk = new QCheckBox(boxHost);
                chk->setTristate(false);
                chk->setChecked(sel->checkState(2) == Qt::Checked);
                lay->addStretch(1);
                lay->addWidget(chk);
                lay->addStretch(1);
                m_connContentTree->setItemWidget(rowValues, col, boxHost);
                rowValues->setData(col, kConnPropEditableRole, true);
                QObject::connect(chk, &QCheckBox::toggled, m_connContentTree, [this, sel, rowValues, col, chk](bool on) {
                    if (!sel || !rowValues || !chk) {
                        return;
                    }
                    const Qt::CheckState desired = on ? Qt::Checked : Qt::Unchecked;
                    const QSignalBlocker blocker(chk);
                    sel->setCheckState(2, desired);
                    onDatasetTreeItemChanged(m_connContentTree, sel, 2, QStringLiteral("conncontent"));
                    chk->setChecked(sel->checkState(2) == Qt::Checked);
                    rowValues->setText(
                        col,
                        (sel->checkState(2) == Qt::Checked)
                            ? trk(QStringLiteral("t_montado_a97484"),
                                  QStringLiteral("Montado"),
                                  QStringLiteral("Mounted"),
                                  QStringLiteral("已挂载"))
                            : trk(QStringLiteral("t_desmontado_bbceae"),
                                  QStringLiteral("Desmontado"),
                                  QStringLiteral("Unmounted"),
                                  QStringLiteral("未挂载")));
                });
                continue;
            }
            if (editable) {
                rowValues->setFlags(rowValues->flags() | Qt::ItemIsEditable);
                const auto eIt = enumValues.constFind(propLower);
                if (eIt != enumValues.cend()) {
                    auto* combo = new QComboBox(m_connContentTree);
                    combo->setMinimumHeight(22);
                    combo->setMaximumHeight(22);
                    combo->setFont(m_connContentTree->font());
                    combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 2px; margin:0px;}"));
                    QStringList opts = eIt.value();
                    if (!value.isEmpty() && !opts.contains(value)) {
                        opts.prepend(value);
                    }
                    combo->addItems(opts);
                    combo->setCurrentText(value);
                    m_connContentTree->setItemWidget(rowValues, col, combo);
                    QObject::connect(combo, &QComboBox::currentTextChanged, m_connContentTree, [this, rowValues, col](const QString& txt) {
                        if (!rowValues) {
                            return;
                        }
                        rowValues->setText(col, txt);
                        onDatasetTreeItemChanged(m_connContentTree, rowValues, col, QStringLiteral("conncontent"));
                    });
                }
                if (propLower == QStringLiteral("normalization")) {
                    appLog(QStringLiteral("DEBUG"),
                           QStringLiteral("conncontent Prop. normalization editable=%1 enum=%2 combo=%3")
                               .arg(editable ? QStringLiteral("1") : QStringLiteral("0"),
                                    enumProp ? QStringLiteral("1") : QStringLiteral("0"),
                                    (eIt != enumValues.cend()) ? QStringLiteral("1") : QStringLiteral("0")));
                }
            }
        }
    }
    sel->setExpanded(true);
    m_syncingConnContentColumns = false;
}

void MainWindow::syncConnContentPoolColumns() {
    if (!m_connContentTree) {
        return;
    }
    if (m_syncingConnContentColumns) {
        return;
    }
    m_syncingConnContentColumns = true;
    const QSignalBlocker blocker(m_connContentTree);
    QStringList headers;
    headers << trk(QStringLiteral("t_pool_title001"), QStringLiteral("Pool"), QStringLiteral("Pool"), QStringLiteral("存储池"));
    for (int i = 0; i < kConnPropCols; ++i) {
        headers << trk(QStringLiteral("t_prop_hdr_001"), QStringLiteral("Prop."), QStringLiteral("Prop."), QStringLiteral("属性"));
    }
    m_connContentTree->setColumnCount(headers.size());
    m_connContentTree->setHeaderLabels(headers);
    m_connContentTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    for (int col = 1; col < m_connContentTree->columnCount(); ++col) {
        m_connContentTree->header()->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    if (m_connContentTree->columnWidth(1) <= 0) {
        for (int col = 1; col <= kConnPropCols; ++col) {
            m_connContentTree->setColumnWidth(col, 96);
        }
    }

    QTreeWidgetItem* root = nullptr;
    for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* n = m_connContentTree->topLevelItem(i);
        if (n && n->data(0, kIsPoolRootRole).toBool()) {
            root = n;
            break;
        }
    }
    if (!root) {
        m_syncingConnContentColumns = false;
        return;
    }

    const int connIdx = root->data(0, kConnIdxRole).toInt();
    const QString poolName = root->data(0, kPoolNameRole).toString();
    const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
    const auto pit = m_poolDetailsCache.constFind(cacheKey);
    if (pit == m_poolDetailsCache.cend() || !pit->loaded) {
        m_syncingConnContentColumns = false;
        return;
    }
    for (int i = root->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* c = root->child(i);
        if (c && c->data(0, kConnPropRowRole).toBool()) {
            delete root->takeChild(i);
        } else if (c) {
            for (int col = 1; col < m_connContentTree->columnCount(); ++col) {
                c->setText(col, QString());
            }
        }
    }
    QStringList props;
    QMap<QString, QString> values;
    for (const QStringList& row : pit->propsRows) {
        if (row.size() < 2) {
            continue;
        }
        const QString prop = row[0].trimmed();
        if (prop.isEmpty()) {
            continue;
        }
        props.push_back(prop);
        values[prop] = row[1].trimmed();
    }
    int insertAt = 0;
    for (int base = 0; base < props.size(); base += kConnPropCols) {
        auto* rowNames = new QTreeWidgetItem();
        rowNames->setData(0, kConnPropRowRole, true);
        rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
        rowNames->setText(0, (base == 0)
                                 ? trk(QStringLiteral("t_props_lbl_001"),
                                       QStringLiteral("Propiedades"),
                                       QStringLiteral("Properties"),
                                       QStringLiteral("属性"))
                                 : QString());
        auto* rowValues = new QTreeWidgetItem();
        rowValues->setData(0, kConnPropRowRole, true);
        rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
        rowValues->setText(0, QString());
        const QColor nameRowBg(232, 240, 250);
        for (int col = 0; col < m_connContentTree->columnCount(); ++col) {
            rowNames->setBackground(col, QBrush(nameRowBg));
        }
        for (int off = 0; off < kConnPropCols; ++off) {
            const int idx = base + off;
            if (idx >= props.size()) {
                break;
            }
            const QString& prop = props.at(idx);
            const int col = 1 + off;
            rowNames->setText(col, prop);
            rowNames->setTextAlignment(col, Qt::AlignCenter);
            rowValues->setText(col, values.value(prop));
            rowValues->setTextAlignment(col, Qt::AlignCenter);
        }
        root->insertChild(insertAt++, rowNames);
        root->insertChild(insertAt++, rowValues);
    }
    root->setExpanded(true);
    m_syncingConnContentColumns = false;
}

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

    QVector<QTreeWidgetItem*> logicalTopLevelItems;
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
            logicalTopLevelItems.push_back(item);
        }
    }
    if (side == QStringLiteral("conncontent")) {
        auto* poolRoot = new QTreeWidgetItem();
        poolRoot->setText(0, QStringLiteral("Pool"));
        poolRoot->setFlags(poolRoot->flags() & ~Qt::ItemIsUserCheckable);
        poolRoot->setData(0, kIsPoolRootRole, true);
        poolRoot->setData(0, kConnIdxRole, connIdx);
        poolRoot->setData(0, kPoolNameRole, poolName);
        for (QTreeWidgetItem* top : logicalTopLevelItems) {
            if (top) {
                poolRoot->addChild(top);
            }
        }
        tree->addTopLevelItem(poolRoot);
    } else {
        for (QTreeWidgetItem* top : logicalTopLevelItems) {
            if (top) {
                tree->addTopLevelItem(top);
            }
        }
    }
    tree->expandToDepth(0);
    // Dropdown embebido en celda Snapshot, sin seleccionar ninguno al inicio.
    std::function<void(QTreeWidgetItem*)> attachCombos = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        if (n->data(0, kIsPoolRootRole).toBool() || n->data(0, kConnPropRowRole).toBool()) {
            for (int i = 0; i < n->childCount(); ++i) {
                attachCombos(n->child(i));
            }
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
        syncConnContentPropertyColumns();
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
    if (side == QStringLiteral("conncontent") && col >= 4) {
        if (!item->data(0, kConnPropRowRole).toBool()) {
            return;
        }
        const QString prop = item->data(col, kConnPropKeyRole).toString().trimmed();
        if (prop.isEmpty() || !item->data(col, kConnPropEditableRole).toBool()) {
            return;
        }
        QTreeWidgetItem* owner = item->parent();
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()) {
            owner = owner->parent();
        }
        if (!owner) {
            return;
        }
        const QString ds = owner->data(0, Qt::UserRole).toString();
        const QString snap = owner->data(1, Qt::UserRole).toString();
        const QString objectName = snap.isEmpty() ? ds : QStringLiteral("%1@%2").arg(ds, snap);
        const QString value = item->text(col);

        if (m_connContentPropsTable) {
            for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
                QTableWidgetItem* k = m_connContentPropsTable->item(r, 0);
                QTableWidgetItem* v = m_connContentPropsTable->item(r, 1);
                if (!k || !v) {
                    continue;
                }
                const QString key = k->data(Qt::UserRole + 777).toString().trimmed().isEmpty()
                                        ? k->text().trimmed()
                                        : k->data(Qt::UserRole + 777).toString().trimmed();
                if (key.compare(prop, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                if (v->text() != value) {
                    v->setText(value);
                    onDatasetPropsCellChanged(r, 1);
                }
                break;
            }
        }
        QMap<QString, QString> vals = m_connContentPropValuesByObject.value(
            QStringLiteral("%1|%2").arg(m_connContentToken.trimmed().toLower(),
                                        objectName.trimmed().toLower()));
        vals[prop] = value;
        updateConnContentPropertyValues(m_connContentToken, objectName, vals);
        return;
    }
    if (col != 2) {
        return;
    }
    if (item->data(0, kConnPropRowRole).toBool() || item->data(0, kIsPoolRootRole).toBool()) {
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
        token.clear();
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
