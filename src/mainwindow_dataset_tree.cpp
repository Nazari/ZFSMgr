#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QCoreApplication>
#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWheelEvent>

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
constexpr int kConnPropRowKindRole = Qt::UserRole + 16; // 1=name, 2=value
constexpr char kPoolBlockInfoKey[] = "__pool_block_info__";

QColor connPropVerticalBorderColor(const QPalette& palette) {
    return palette.color(QPalette::Mid).darker(118);
}

QColor connPropHorizontalBorderColor(const QPalette& palette) {
    return palette.color(QPalette::Mid).darker(108);
}

class TreeScrollComboBox final : public QComboBox {
public:
    explicit TreeScrollComboBox(QTreeWidget* tree, QWidget* parent = nullptr)
        : QComboBox(parent), m_tree(tree) {}

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (view()->isVisible() || !m_tree || !m_tree->viewport()) {
            QComboBox::wheelEvent(event);
            return;
        }
        event->ignore();
        QCoreApplication::sendEvent(m_tree->viewport(), event);
    }

private:
    QPointer<QTreeWidget> m_tree;
};

class InlinePropNameWidget final : public QWidget {
public:
    InlinePropNameWidget(const QString& name, bool inheritable, QWidget* parent = nullptr)
        : QWidget(parent), m_inheritable(inheritable) {
        auto* layout = new QHBoxLayout(this);
        // Reserve 1px for left/top/right borders and add a small top inset so
        // each name row reads as a new block under the previous value row.
        layout->setContentsMargins(7, 5, 7, 2);
        layout->setSpacing(8);
        auto* left = new QLabel(name, this);
        left->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        left->setStyleSheet(QStringLiteral("background: transparent;"));
        layout->addWidget(left, 1);
        if (m_inheritable) {
            auto* right = new QLabel(QStringLiteral("Inh."), this);
            right->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            right->setStyleSheet(QStringLiteral("background: transparent; color:#5f6b76;"));
            layout->addWidget(right, 0);
        }
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QWidget::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor vBorder = connPropVerticalBorderColor(palette());
        const QColor hBorder = connPropHorizontalBorderColor(palette());
        const QRect r = rect();
        painter.fillRect(0, 0, 1, r.height(), vBorder);
        painter.fillRect(qMax(0, r.width() - 1), 0, 1, r.height(), vBorder);
        painter.fillRect(0, 0, r.width(), 1, hBorder);
    }

private:
    bool m_inheritable{false};
};

class InlinePropValueWidget final : public QWidget {
public:
    explicit InlinePropValueWidget(QWidget* parent = nullptr)
        : QWidget(parent) {}

    QHBoxLayout* ensureLayout() {
        if (auto* lay = qobject_cast<QHBoxLayout*>(layout())) {
            return lay;
        }
        auto* lay = new QHBoxLayout(this);
        // Reserve 1px for left/right/bottom borders and keep editors slightly
        // away from the bottom edge so adjacent property groups read cleaner.
        lay->setContentsMargins(3, 2, 3, 6);
        lay->setSpacing(4);
        return lay;
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QWidget::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor vBorder = connPropVerticalBorderColor(palette());
        const QColor hBorder = connPropHorizontalBorderColor(palette());
        const QRect r = rect();
        painter.fillRect(0, 0, 1, r.height(), vBorder);
        painter.fillRect(qMax(0, r.width() - 1), 0, 1, r.height(), vBorder);
        painter.fillRect(0, qMax(0, r.height() - 1), r.width(), 1, hBorder);
    }
};

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

QString datasetLeafName(const QString& datasetName) {
    return datasetName.contains('/') ? datasetName.section('/', -1, -1) : datasetName;
}

void applySnapshotVisualState(QTreeWidgetItem* item) {
    if (!item) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString().trimmed();
    if (ds.isEmpty()) {
        return;
    }
    const QString snap = item->data(1, Qt::UserRole).toString().trimmed();
    const QString leaf = datasetLeafName(ds);
    item->setText(0, snap.isEmpty() ? leaf : QStringLiteral("%1@%2").arg(leaf, snap));

    const bool hideDatasetChildren = !snap.isEmpty();
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* ch = item->child(i);
        if (!ch) {
            continue;
        }
        const bool isPropRow = ch->data(0, kConnPropRowRole).toBool();
        const bool isDatasetNode = !ch->data(0, Qt::UserRole).toString().trimmed().isEmpty();
        if (isDatasetNode && !isPropRow) {
            ch->setHidden(hideDatasetChildren);
        }
    }
}

void refreshDatasetExpansionIndicators(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            rec(n->child(i));
        }

        if (n->data(0, kIsPoolRootRole).toBool() || n->data(0, kConnPropRowRole).toBool()) {
            return;
        }
        const QString marker = n->data(0, kConnPropKeyRole).toString();
        if (marker == QString::fromLatin1(kPoolBlockInfoKey)) {
            return;
        }
        const QString ds = n->data(0, Qt::UserRole).toString();
        if (ds.isEmpty()) {
            return;
        }

        bool hasDatasetChildren = false;
        bool hasAnyChildren = (n->childCount() > 0);
        for (int i = 0; i < n->childCount(); ++i) {
            QTreeWidgetItem* c = n->child(i);
            if (!c) {
                continue;
            }
            if (c->data(0, kConnPropRowRole).toBool()) {
                continue;
            }
            if (c->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                continue;
            }
            if (!c->data(0, Qt::UserRole).toString().isEmpty()) {
                hasDatasetChildren = true;
                break;
            }
        }
        n->setChildIndicatorPolicy((hasDatasetChildren || hasAnyChildren)
                                       ? QTreeWidgetItem::ShowIndicator
                                       : QTreeWidgetItem::DontShowIndicator);
    };

    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        rec(tree->topLevelItem(i));
    }
}

void resizeTreeColumnsToVisibleContent(QTreeWidget* tree) {
    if (!tree || !tree->headerItem()) {
        return;
    }
    for (int col = 0; col < tree->columnCount(); ++col) {
        if (tree->isColumnHidden(col)) {
            continue;
        }
        tree->resizeColumnToContents(col);
    }
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
    QTreeWidget* const tree = m_connContentTree;
    const int propCols = qBound(5, m_connPropColumnsSetting, 10);
    if (m_syncingConnContentColumns) {
        return;
    }
    m_syncingConnContentColumns = true;
    const QSignalBlocker blocker(tree);

    QStringList headers = {
        trk(QStringLiteral("t_dataset_001"), QStringLiteral("Dataset"), QStringLiteral("Dataset"), QStringLiteral("数据集")),
        trk(QStringLiteral("t_snapshot_col01"), QStringLiteral("Snapshot"), QStringLiteral("Snapshot"), QStringLiteral("快照")),
        trk(QStringLiteral("t_montado_a97484"), QStringLiteral("Montado"), QStringLiteral("Mounted"), QStringLiteral("已挂载")),
        trk(QStringLiteral("t_mountpoint_001"), QStringLiteral("Mountpoint"), QStringLiteral("Mountpoint"), QStringLiteral("挂载点"))
    };
    for (int i = 0; i < propCols; ++i) {
        headers << QStringLiteral("C%1").arg(i + 1);
    }
    tree->setColumnCount(headers.size());
    tree->setHeaderLabels(headers);
    if (QTreeWidgetItem* hh = tree->headerItem()) {
        for (int col = 0; col < headers.size(); ++col) {
            hh->setTextAlignment(col, Qt::AlignCenter);
        }
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    for (int col = 4; col < tree->columnCount(); ++col) {
        tree->header()->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    // En Contenido de Pool, Snapshot/Montado/Mountpoint se gestionan dentro de "Prop.".
    tree->setColumnHidden(0, false);
    if (tree->columnCount() > 1) tree->setColumnHidden(1, true);
    if (tree->columnCount() > 2) tree->setColumnHidden(2, true);
    if (tree->columnCount() > 3) tree->setColumnHidden(3, true);
    for (int col = 4; col < tree->columnCount(); ++col) {
        tree->setColumnHidden(col, false);
    }
    for (int col = 4; col < tree->columnCount(); ++col) {
        if (tree->columnWidth(col) < 32) {
            tree->setColumnWidth(col, 96);
        }
    }

    auto clearPropRowsRec = [&](auto&& self, QTreeWidgetItem* n) -> void {
        if (!n) {
            return;
        }
        const QString marker = n->data(0, kConnPropKeyRole).toString();
        if (marker == QString::fromLatin1(kPoolBlockInfoKey)) {
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
        for (int col = 4; col < tree->columnCount(); ++col) {
            if (QWidget* w = tree->itemWidget(n, col)) {
                tree->removeItemWidget(n, col);
                w->deleteLater();
            }
            n->setText(col, QString());
            n->setData(col, kConnPropKeyRole, QVariant());
            n->setData(col, kConnPropEditableRole, false);
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        clearPropRowsRec(clearPropRowsRec, tree->topLevelItem(i));
    }

    if (!m_showInlineDatasetProps) {
        refreshDatasetExpansionIndicators(tree);
        m_syncingConnContentColumns = false;
        return;
    }

    QTreeWidgetItem* sel = tree->currentItem();
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
    const bool objectIsSnapshot = !snap.trimmed().isEmpty();
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
    if (!m_datasetInlinePropsOrder.isEmpty()) {
        QStringList filtered;
        for (const QString& wanted : m_datasetInlinePropsOrder) {
            for (const QString& have : props) {
                if (wanted.compare(have, Qt::CaseInsensitive) == 0) {
                    filtered.push_back(have);
                    break;
                }
            }
        }
        props = filtered;
    }
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
    auto isInheritableProp = [this, &propsKey, &isReadonlyFlag, objectIsSnapshot](const QString& prop) -> bool {
        if (objectIsSnapshot) {
            return false;
        }
        const QString p = prop.trimmed();
        if (p.isEmpty()) {
            return false;
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
    auto isCurrentlyInheritedProp = [this, &propsKey](const QString& prop) -> bool {
        const auto itCache = m_datasetPropsCache.constFind(propsKey);
        if (itCache == m_datasetPropsCache.cend() || !itCache->loaded) {
            return false;
        }
        for (const DatasetPropCacheRow& row : itCache->rows) {
            if (row.prop.compare(prop, Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString src = row.source.trimmed().toLower();
            if (src.isEmpty() || src == QStringLiteral("-")) {
                return false;
            }
            if (src == QStringLiteral("local")
                || src == QStringLiteral("default")
                || src == QStringLiteral("received")
                || src == QStringLiteral("temporary")) {
                return false;
            }
            return src.startsWith(QStringLiteral("inherited"));
        }
        return false;
    };
    auto findPropsTableRowByProp = [this](const QString& prop) -> int {
        if (!m_connContentPropsTable) {
            return -1;
        }
        for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
            QTableWidgetItem* k = m_connContentPropsTable->item(r, 0);
            if (!k) {
                continue;
            }
            const QString key = k->data(Qt::UserRole + 777).toString().trimmed().isEmpty()
                                    ? k->text().trimmed()
                                    : k->data(Qt::UserRole + 777).toString().trimmed();
            if (key.compare(prop, Qt::CaseInsensitive) == 0) {
                return r;
            }
        }
        return -1;
    };
    int insertAt = 0;
    for (int base = 0; base < props.size(); base += propCols) {
        auto* rowNames = new QTreeWidgetItem();
        rowNames->setData(0, kConnPropRowRole, true);
        rowNames->setData(0, kConnPropRowKindRole, 1);
        rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
        rowNames->setText(0, (base == 0)
                                 ? trk(QStringLiteral("t_props_lbl_001"),
                                       QStringLiteral("Propiedades"),
                                       QStringLiteral("Properties"),
                                       QStringLiteral("属性"))
                                 : QString());
        auto* rowValues = new QTreeWidgetItem();
        rowValues->setData(0, kConnPropRowRole, true);
        rowValues->setData(0, kConnPropRowKindRole, 2);
        rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
        rowValues->setText(0, QString());
        const QColor nameRowBg(232, 240, 250);
        for (int col = 0; col < tree->columnCount(); ++col) {
            rowNames->setBackground(col, QBrush(nameRowBg));
        }
        sel->insertChild(insertAt++, rowNames);
        sel->insertChild(insertAt++, rowValues);

        for (int off = 0; off < propCols; ++off) {
            const int idx = base + off;
            if (idx >= props.size()) {
                break;
            }
            const QString& prop = props.at(idx);
            const int col = 4 + off;
            const QString value = it->value(prop);
            const QString propLower = prop.trimmed().toLower();
            const bool inheritable = isInheritableProp(prop);
            const QString propLabel =
                (propLower == QStringLiteral("estado"))
                    ? trk(QStringLiteral("t_montado_a97484"),
                          QStringLiteral("Montado"),
                          QStringLiteral("Mounted"),
                          QStringLiteral("已挂载"))
                    : prop;
            if (inheritable) {
                tree->setItemWidget(rowNames, col, new InlinePropNameWidget(propLabel, true, tree));
            } else {
                rowNames->setText(col, propLabel);
            }
            rowNames->setTextAlignment(col, Qt::AlignCenter);
            rowNames->setData(col, kConnPropKeyRole, prop);
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
                auto* combo = new TreeScrollComboBox(tree, tree);
                combo->setMinimumHeight(24);
                combo->setMaximumHeight(24);
                combo->setFont(tree->font());
                combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 5px; margin:0px;}"));
                combo->addItems(options);
                const QString currentSnap = sel->data(1, Qt::UserRole).toString().trimmed();
                const QString currentLabel = currentSnap.isEmpty() ? options.first() : currentSnap;
                combo->setCurrentText(currentLabel);
                tree->setItemWidget(rowValues, col, combo);
                QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, sel, rowValues, col](const QString& txt) {
                    if (!sel || !rowValues) {
                        return;
                    }
                    rowValues->setText(col, txt);
                    onSnapshotComboChanged(tree, sel, QStringLiteral("conncontent"), txt);
                });
                rowValues->setData(col, kConnPropEditableRole, true);
                continue;
            }
            if (propLower == QStringLiteral("estado") && !objectIsSnapshot) {
                auto* combo = new TreeScrollComboBox(tree, tree);
                combo->setMinimumHeight(22);
                combo->setMaximumHeight(22);
                combo->setFont(tree->font());
                combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 5px; margin:0px;}"));
                combo->addItem(QStringLiteral("off"));
                combo->addItem(QStringLiteral("on"));
                combo->setCurrentText(sel->checkState(2) == Qt::Checked ? QStringLiteral("on") : QStringLiteral("off"));
                tree->setItemWidget(rowValues, col, combo);
                rowValues->setData(col, kConnPropEditableRole, true);
                QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, sel, rowValues, col, combo](const QString& txt) {
                    if (!sel || !rowValues || !combo) {
                        return;
                    }
                    const bool on = (txt.trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0);
                    const Qt::CheckState desired = on ? Qt::Checked : Qt::Unchecked;
                    const QSignalBlocker blocker(combo);
                    sel->setCheckState(2, desired);
                    onDatasetTreeItemChanged(tree, sel, 2, QStringLiteral("conncontent"));
                    combo->setCurrentText(sel->checkState(2) == Qt::Checked ? QStringLiteral("on") : QStringLiteral("off"));
                    rowValues->setText(col, sel->checkState(2) == Qt::Checked ? QStringLiteral("on") : QStringLiteral("off"));
                });
                continue;
            }
            if (editable) {
                rowValues->setFlags(rowValues->flags() | Qt::ItemIsEditable);
                if (inheritable) {
                    auto* cell = new InlinePropValueWidget(tree);
                    auto* lay = cell->ensureLayout();
                    QWidget* valueEditor = nullptr;
                    TreeScrollComboBox* inheritCombo = new TreeScrollComboBox(tree, cell);
                    inheritCombo->setMinimumHeight(22);
                    inheritCombo->setMaximumHeight(22);
                    inheritCombo->setFont(tree->font());
                    inheritCombo->setStyleSheet(QStringLiteral("QComboBox{padding:0 5px; margin:0px;}"));
                    inheritCombo->addItem(QStringLiteral("off"));
                    inheritCombo->addItem(QStringLiteral("on"));
                    int propRow = findPropsTableRowByProp(prop);
                    bool inheritChecked = isCurrentlyInheritedProp(prop);
                    if (propRow >= 0) {
                        if (QTableWidgetItem* pi = m_connContentPropsTable->item(propRow, 2)) {
                            inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable)
                                             && pi->checkState() == Qt::Checked;
                        }
                    }
                    inheritCombo->setCurrentText(inheritChecked ? QStringLiteral("on") : QStringLiteral("off"));
                    const auto eIt = enumValues.constFind(propLower);
                    if (eIt != enumValues.cend()) {
                        auto* combo = new TreeScrollComboBox(tree, cell);
                        combo->setMinimumHeight(22);
                        combo->setMaximumHeight(22);
                        combo->setFont(tree->font());
                        combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 5px; margin:0px;}"));
                        QStringList opts = eIt.value();
                        if (!value.isEmpty() && !opts.contains(value)) {
                            opts.prepend(value);
                        }
                        combo->addItems(opts);
                        combo->setCurrentText(value);
                        combo->setEnabled(!inheritChecked);
                        valueEditor = combo;
                        lay->addWidget(combo, 1);
                        QObject::connect(combo, &QComboBox::currentTextChanged, tree,
                                         [this, tree, rowValues, col, inheritCombo](const QString& txt) {
                            if (!rowValues) {
                                return;
                            }
                            rowValues->setText(col, txt);
                            if (inheritCombo && inheritCombo->currentText() != QStringLiteral("off")) {
                                const QSignalBlocker blocker(inheritCombo);
                                inheritCombo->setCurrentText(QStringLiteral("off"));
                            }
                            onDatasetTreeItemChanged(tree, rowValues, col, QStringLiteral("conncontent"));
                        });
                    } else {
                        auto* edit = new QLineEdit(cell);
                        edit->setText(value);
                        edit->setMinimumHeight(22);
                        edit->setMaximumHeight(22);
                        edit->setFont(tree->font());
                        edit->setStyleSheet(QStringLiteral("QLineEdit{padding:0 5px; margin:0px;}"));
                        edit->setEnabled(!inheritChecked);
                        valueEditor = edit;
                        lay->addWidget(edit, 1);
                        QObject::connect(edit, &QLineEdit::editingFinished, tree,
                                         [this, tree, rowValues, col, edit, inheritCombo]() {
                            if (!rowValues || !edit) {
                                return;
                            }
                            rowValues->setText(col, edit->text());
                            if (inheritCombo && inheritCombo->currentText() != QStringLiteral("off")) {
                                const QSignalBlocker blocker(inheritCombo);
                                inheritCombo->setCurrentText(QStringLiteral("off"));
                            }
                            onDatasetTreeItemChanged(tree, rowValues, col, QStringLiteral("conncontent"));
                        });
                    }
                    lay->addWidget(inheritCombo, 0);
                    tree->setItemWidget(rowValues, col, cell);
                    QObject::connect(inheritCombo, &QComboBox::currentTextChanged, tree,
                                     [this, valueEditor, prop, inheritCombo](const QString& txt) {
                        const bool inheritOn = (txt.trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0);
                        if (valueEditor) {
                            valueEditor->setEnabled(!inheritOn);
                        }
                        if (!m_connContentPropsTable) {
                            return;
                        }
                        for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
                            QTableWidgetItem* k = m_connContentPropsTable->item(r, 0);
                            QTableWidgetItem* pi = m_connContentPropsTable->item(r, 2);
                            if (!k || !pi) {
                                continue;
                            }
                            const QString key = k->data(Qt::UserRole + 777).toString().trimmed().isEmpty()
                                                    ? k->text().trimmed()
                                                    : k->data(Qt::UserRole + 777).toString().trimmed();
                            if (key.compare(prop, Qt::CaseInsensitive) != 0) {
                                continue;
                            }
                            if (pi->flags() & Qt::ItemIsUserCheckable) {
                                pi->setCheckState(inheritOn ? Qt::Checked : Qt::Unchecked);
                                onDatasetPropsCellChanged(r, 2);
                            }
                            break;
                        }
                    });
                    continue;
                }
                const auto eIt = enumValues.constFind(propLower);
                if (eIt != enumValues.cend()) {
                    auto* combo = new TreeScrollComboBox(tree, tree);
                    combo->setMinimumHeight(22);
                    combo->setMaximumHeight(22);
                    combo->setFont(tree->font());
                    combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 5px; margin:0px;}"));
                    QStringList opts = eIt.value();
                    if (!value.isEmpty() && !opts.contains(value)) {
                        opts.prepend(value);
                    }
                    combo->addItems(opts);
                    combo->setCurrentText(value);
                    tree->setItemWidget(rowValues, col, combo);
                    QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, rowValues, col](const QString& txt) {
                        if (!rowValues) {
                            return;
                        }
                        rowValues->setText(col, txt);
                        onDatasetTreeItemChanged(tree, rowValues, col, QStringLiteral("conncontent"));
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
    refreshDatasetExpansionIndicators(tree);
    sel->setExpanded(true);
    resizeTreeColumnsToVisibleContent(tree);
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
    const int propCols = qBound(5, m_connPropColumnsSetting, 10);
    QStringList headers;
    headers << trk(QStringLiteral("t_pool_title001"), QStringLiteral("Pool"), QStringLiteral("Pool"), QStringLiteral("存储池"))
            << trk(QStringLiteral("t_snapshot_col01"), QStringLiteral("Snapshot"), QStringLiteral("Snapshot"), QStringLiteral("快照"))
            << trk(QStringLiteral("t_montado_a97484"), QStringLiteral("Montado"), QStringLiteral("Mounted"), QStringLiteral("已挂载"))
            << trk(QStringLiteral("t_mountpoint_001"), QStringLiteral("Mountpoint"), QStringLiteral("Mountpoint"), QStringLiteral("挂载点"));
    for (int i = 0; i < propCols; ++i) {
        headers << QStringLiteral("C%1").arg(i + 1);
    }
    m_connContentTree->setColumnCount(headers.size());
    m_connContentTree->setHeaderLabels(headers);
    if (QTreeWidgetItem* hh = m_connContentTree->headerItem()) {
        for (int col = 0; col < headers.size(); ++col) {
            hh->setTextAlignment(col, Qt::AlignCenter);
        }
    }
    m_connContentTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    for (int col = 1; col < m_connContentTree->columnCount(); ++col) {
        m_connContentTree->header()->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    if (m_connContentTree->columnCount() > 1) m_connContentTree->setColumnHidden(1, true);
    if (m_connContentTree->columnCount() > 2) m_connContentTree->setColumnHidden(2, true);
    if (m_connContentTree->columnCount() > 3) m_connContentTree->setColumnHidden(3, true);
    for (int col = 4; col < m_connContentTree->columnCount(); ++col) {
        m_connContentTree->setColumnHidden(col, false);
    }
    for (int col = 4; col < (4 + propCols) && col < m_connContentTree->columnCount(); ++col) {
        if (m_connContentTree->columnWidth(col) < 32) {
            m_connContentTree->setColumnWidth(col, 96);
        }
    }

    QTreeWidgetItem* root = nullptr;
    int wantedConnIdx = -1;
    QString wantedPoolName;
    const QString token = m_connContentToken.trimmed();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep > 0) {
        bool ok = false;
        wantedConnIdx = token.left(sep).toInt(&ok);
        wantedPoolName = token.mid(sep + 2).trimmed();
        if (!ok) {
            wantedConnIdx = -1;
            wantedPoolName.clear();
        }
    }
    if (wantedConnIdx >= 0 && !wantedPoolName.isEmpty()) {
        for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* n = m_connContentTree->topLevelItem(i);
            if (!n || !n->data(0, kIsPoolRootRole).toBool()) {
                continue;
            }
            if (n->data(0, kConnIdxRole).toInt() == wantedConnIdx
                && n->data(0, kPoolNameRole).toString().trimmed() == wantedPoolName) {
                root = n;
                break;
            }
        }
    }
    if (!root) {
        for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* n = m_connContentTree->topLevelItem(i);
            if (n && n->data(0, kIsPoolRootRole).toBool()) {
                root = n;
                break;
            }
        }
    }
    if (!root) {
        m_syncingConnContentColumns = false;
        return;
    }

    const int connIdx = root->data(0, kConnIdxRole).toInt();
    const QString poolName = root->data(0, kPoolNameRole).toString();
    const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
    auto pit = m_poolDetailsCache.constFind(cacheKey);
    if (pit == m_poolDetailsCache.cend() || !pit->loaded) {
        // Carga ad-hoc para el panel inferior/superior sin depender de la selección
        // del tab de propiedades del pool.
        if (connIdx >= 0 && connIdx < m_profiles.size() && !poolName.trimmed().isEmpty()) {
            const ConnectionProfile& p = m_profiles[connIdx];
            PoolDetailsCacheEntry fresh;

            QString out;
            QString err;
            int rc = -1;
            const QString propsCmd = withSudo(
                p, QStringLiteral("zpool get -H -o property,value,source all %1").arg(mwhelpers::shSingleQuote(poolName)));
            if (runSsh(p, propsCmd, 20000, out, err, rc) && rc == 0) {
                const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
                for (const QString& line : lines) {
                    const QStringList parts = line.split('\t');
                    if (parts.size() < 3) {
                        continue;
                    }
                    fresh.propsRows.push_back(
                        QStringList{parts[0].trimmed(), parts[1].trimmed(), parts[2].trimmed()});
                }
            }

            out.clear();
            err.clear();
            rc = -1;
            const QString stCmd = withSudo(
                p, QStringLiteral("zpool status -v %1").arg(mwhelpers::shSingleQuote(poolName)));
            if (runSsh(p, stCmd, 20000, out, err, rc) && rc == 0) {
                fresh.statusText = out.trimmed();
            } else {
                fresh.statusText = err.trimmed();
            }
            fresh.loaded = true;
            m_poolDetailsCache.insert(cacheKey, fresh);
            pit = m_poolDetailsCache.constFind(cacheKey);
        }
        if (pit == m_poolDetailsCache.cend() || !pit->loaded) {
            m_syncingConnContentColumns = false;
            return;
        }
    }
    {
        const QString tt = pit->statusText.toHtmlEscaped();
        root->setToolTip(
            0,
            QStringLiteral("<pre style=\"font-family:monospace; white-space:pre;\">%1</pre>").arg(tt));
    }
    auto clearNodeRows = [](QTreeWidgetItem* parent) {
        if (!parent) {
            return;
        }
        for (int i = parent->childCount() - 1; i >= 0; --i) {
            delete parent->takeChild(i);
        }
    };
    auto clearDatasetNodeRec = [&](auto&& self, QTreeWidgetItem* n) -> void {
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
        // Limpiar solo columnas dinámicas de propiedades; no tocar estado base del dataset
        // (snapshot/check de montado/mountpoint), que se reutiliza al volver a selección de dataset.
        for (int col = 4; col < m_connContentTree->columnCount(); ++col) {
            if (QWidget* w = m_connContentTree->itemWidget(n, col)) {
                m_connContentTree->removeItemWidget(n, col);
                w->deleteLater();
            }
            n->setText(col, QString());
        }
    };
    auto blockForKey = [root](const QString& key) -> QTreeWidgetItem* {
        for (int i = 0; i < root->childCount(); ++i) {
            QTreeWidgetItem* c = root->child(i);
            if (!c) {
                continue;
            }
            if (c->data(0, kConnPropKeyRole).toString() == key) {
                return c;
            }
        }
        return nullptr;
    };
    for (int i = root->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* c = root->child(i);
        if (!c) {
            continue;
        }
        const QString marker = c->data(0, kConnPropKeyRole).toString();
        if (marker == QString::fromLatin1(kPoolBlockInfoKey)) {
            clearNodeRows(c);
            continue;
        }
        if (c->data(0, kConnPropRowRole).toBool()) {
            delete root->takeChild(i);
        } else {
            clearDatasetNodeRec(clearDatasetNodeRec, c);
        }
    }
    QStringList props;
    QMap<QString, QString> values;
    QStringList featureEnabled;
    QStringList featureDisabled;
    const QString featurePrefix = QStringLiteral("feature@");
    for (const QStringList& row : pit->propsRows) {
        if (row.size() < 2) {
            continue;
        }
        const QString prop = row[0].trimmed();
        const QString value = row[1].trimmed();
        if (prop.isEmpty()) {
            continue;
        }
        if (prop.startsWith(featurePrefix, Qt::CaseInsensitive)) {
            const QString featureName = prop.mid(featurePrefix.size());
            const QString v = value.toLower();
            if (v == QStringLiteral("enabled")) {
                featureEnabled.push_back(featureName);
            } else if (v == QStringLiteral("disabled")) {
                featureDisabled.push_back(featureName);
            }
            // Todas las feature@ se excluyen del bloque "Propiedades".
            continue;
        }
        props.push_back(prop);
        values[prop] = value;
    }
    if (!m_poolInlinePropsOrder.isEmpty()) {
        QStringList filtered;
        for (const QString& wanted : m_poolInlinePropsOrder) {
            for (const QString& have : props) {
                if (wanted.compare(have, Qt::CaseInsensitive) == 0) {
                    filtered.push_back(have);
                    break;
                }
            }
        }
        props = filtered;
    }
    const QColor nameRowBg(232, 240, 250);
    auto addSectionRows = [&](QTreeWidgetItem* parent,
                                const QString& title,
                                const QStringList& names,
                                const QMap<QString, QString>* valuesByName,
                                bool namesOnly) {
        if (!parent) {
            return;
        }
        if (names.isEmpty()) {
            return;
        }
        auto* titleRow = new QTreeWidgetItem(parent);
        titleRow->setData(0, kConnPropRowRole, true);
        titleRow->setData(0, kConnPropRowKindRole, 0);
        titleRow->setFlags(titleRow->flags() & ~Qt::ItemIsUserCheckable);
        titleRow->setText(0, title);
        for (int col = 0; col < m_connContentTree->columnCount(); ++col) {
            titleRow->setBackground(col, QBrush(nameRowBg));
        }
        titleRow->setTextAlignment(0, Qt::AlignLeft | Qt::AlignVCenter);
        for (int base = 0; base < names.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem(parent);
            rowNames->setData(0, kConnPropRowRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            for (int col = 0; col < m_connContentTree->columnCount(); ++col) {
                rowNames->setBackground(col, QBrush(nameRowBg));
            }
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= names.size()) {
                    break;
                }
                const QString& name = names.at(idx);
                const int col = 4 + off;
                rowNames->setText(col, name);
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                rowNames->setData(col, kConnPropKeyRole, name);
            }
            if (namesOnly) {
                continue;
            }
            auto* rowValues = new QTreeWidgetItem(parent);
            rowValues->setData(0, kConnPropRowRole, true);
            rowValues->setData(0, kConnPropRowKindRole, 2);
            rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= names.size()) {
                    break;
                }
                const QString& name = names.at(idx);
                const int col = 4 + off;
                rowValues->setText(col, valuesByName ? valuesByName->value(name) : QString());
                rowValues->setTextAlignment(col, Qt::AlignCenter);
                rowValues->setData(col, kConnPropKeyRole, name);
            }
        }
    };
    QTreeWidgetItem* infoNode = blockForKey(QString::fromLatin1(kPoolBlockInfoKey));
    if (!infoNode) {
        infoNode = new QTreeWidgetItem();
        infoNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kPoolBlockInfoKey));
        infoNode->setFlags(infoNode->flags() & ~Qt::ItemIsUserCheckable);
        infoNode->setExpanded(true);
        root->insertChild(0, infoNode);
    }
    infoNode->setText(0, trk(QStringLiteral("t_pool_info_001"),
                             QStringLiteral("Información"),
                             QStringLiteral("Information"),
                             QStringLiteral("信息")));

    addSectionRows(infoNode,
                   trk(QStringLiteral("t_props_lbl_001"),
                       QStringLiteral("Propiedades"),
                       QStringLiteral("Properties"),
                       QStringLiteral("属性")),
                   props,
                   &values,
                   false);
    addSectionRows(infoNode,
                   trk(QStringLiteral("t_pool_caps_on001"),
                       QStringLiteral("Capacidades activas"),
                       QStringLiteral("Enabled features"),
                       QStringLiteral("已启用能力")),
                   featureEnabled,
                   nullptr,
                   true);
    addSectionRows(infoNode,
                   trk(QStringLiteral("t_pool_caps_off01"),
                       QStringLiteral("Capacidades deshabilitadas"),
                       QStringLiteral("Disabled features"),
                       QStringLiteral("已禁用能力")),
                   featureDisabled,
                   nullptr,
                   true);
    root->setExpanded(true);
    resizeTreeColumnsToVisibleContent(m_connContentTree);
    m_syncingConnContentColumns = false;
}

void MainWindow::saveConnContentTreeState(const QString& token) {
    if (token.isEmpty() || !m_connContentTree) {
        return;
    }
    const QString scopedToken =
        token + ((m_connContentTree == m_bottomConnContentTree) ? QStringLiteral("|bottom")
                                                                 : QStringLiteral("|top"));
    ConnContentTreeState st;
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        if (n->data(0, kIsPoolRootRole).toBool()) {
            st.poolRootExpanded = n->isExpanded();
        }
        if (n->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
            st.infoExpanded = n->isExpanded();
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
    m_connContentTreeStateByToken[scopedToken] = st;
}

void MainWindow::restoreConnContentTreeState(const QString& token) {
    if (token.isEmpty() || !m_connContentTree) {
        return;
    }
    const QString scopedToken =
        token + ((m_connContentTree == m_bottomConnContentTree) ? QStringLiteral("|bottom")
                                                                 : QStringLiteral("|top"));
    const auto it = m_connContentTreeStateByToken.constFind(scopedToken);
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
    for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_connContentTree->topLevelItem(i);
        if (!top) {
            continue;
        }
        if (top->data(0, kIsPoolRootRole).toBool()) {
            top->setExpanded(st.poolRootExpanded);
            for (int c = 0; c < top->childCount(); ++c) {
                QTreeWidgetItem* ch = top->child(c);
                if (!ch) {
                    continue;
                }
                if (ch->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                    ch->setExpanded(st.infoExpanded);
                    break;
                }
            }
            break;
        }
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
                applySnapshotVisualState(item);
            }
        }
    }

    if (!st.selectedDataset.isEmpty()) {
        QTreeWidgetItem* sel = findDatasetItem(m_connContentTree, st.selectedDataset);
        if (!sel) {
            QString parent = parentDatasetName(st.selectedDataset);
            while (!parent.isEmpty() && !sel) {
                sel = findDatasetItem(m_connContentTree, parent);
                if (sel) {
                    break;
                }
                parent = parentDatasetName(parent);
            }
        }
        if (sel) {
            if (!st.selectedSnapshot.isEmpty()) {
                if (QComboBox* cb = qobject_cast<QComboBox*>(m_connContentTree->itemWidget(sel, 1))) {
                    const int idx = cb->findText(st.selectedSnapshot);
                    if (idx > 0) {
                        const QSignalBlocker blocker(cb);
                        cb->setCurrentIndex(idx);
                        sel->setData(1, Qt::UserRole, st.selectedSnapshot);
                        applySnapshotVisualState(sel);
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

void MainWindow::populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side, bool allowRemoteLoadIfMissing) {
    if (!tree) {
        return;
    }
    beginUiBusy();
    m_loadingDatasetTrees = true;
    tree->clear();
    const bool isConnContentInteractive = (side == QStringLiteral("conncontent"));
    const bool isConnContent = (side == QStringLiteral("conncontent")
                                || side == QStringLiteral("conncontent_multi"));
    auto poolRootTitle = [&]() -> QString {
        QString connName = (connIdx >= 0 && connIdx < m_profiles.size()) ? m_profiles[connIdx].name : QStringLiteral("?");
        bool imported = false;
        if (connIdx >= 0 && connIdx < m_states.size()) {
            const ConnectionRuntimeState& st = m_states[connIdx];
            for (const PoolImported& p : st.importedPools) {
                if (p.pool.trimmed() == poolName.trimmed()) {
                    imported = true;
                    break;
                }
            }
        }
        if (imported) {
            return QStringLiteral("%1::%2").arg(connName, poolName);
        }
        const QString stateText = trk(QStringLiteral("t_pool_impable_001"),
                                      QStringLiteral("Importable"),
                                      QStringLiteral("Importable"),
                                      QStringLiteral("可导入"));
        return QStringLiteral("%1::%2 [%3]").arg(connName, poolName, stateText);
    };
    auto addPoolRootOnlyForConnContent = [&]() {
        if (!isConnContent) {
            return;
        }
        auto* poolRoot = new QTreeWidgetItem();
        poolRoot->setText(0, poolRootTitle());
        {
            QFont f = tree->font();
            const qreal base = (f.pointSizeF() > 0.0) ? f.pointSizeF() : 9.0;
            f.setPointSizeF(base + 0.5);
            f.setBold(false);
            poolRoot->setFont(0, f);
        }
        poolRoot->setFlags(poolRoot->flags() & ~Qt::ItemIsUserCheckable);
        poolRoot->setData(0, kIsPoolRootRole, true);
        poolRoot->setData(0, kConnIdxRole, connIdx);
        poolRoot->setData(0, kPoolNameRole, poolName);
        auto* infoNode = new QTreeWidgetItem(poolRoot);
        infoNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kPoolBlockInfoKey));
        infoNode->setFlags(infoNode->flags() & ~Qt::ItemIsUserCheckable);
        infoNode->setText(0, trk(QStringLiteral("t_pool_info_001"),
                                 QStringLiteral("Información"),
                                 QStringLiteral("Information"),
                                 QStringLiteral("信息")));
        infoNode->setExpanded(true);
        tree->addTopLevelItem(poolRoot);
        poolRoot->setExpanded(true);
    };
    if (!ensureDatasetsLoaded(connIdx, poolName, allowRemoteLoadIfMissing)) {
        addPoolRootOnlyForConnContent();
        if (isConnContentInteractive) {
            syncConnContentPoolColumns();
        }
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
        {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
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
    if (isConnContent) {
        auto* poolRoot = new QTreeWidgetItem();
        poolRoot->setText(0, poolRootTitle());
        {
            QFont f = tree->font();
            const qreal base = (f.pointSizeF() > 0.0) ? f.pointSizeF() : 9.0;
            f.setPointSizeF(base + 0.5);
            f.setBold(false);
            poolRoot->setFont(0, f);
        }
        poolRoot->setFlags(poolRoot->flags() & ~Qt::ItemIsUserCheckable);
        poolRoot->setData(0, kIsPoolRootRole, true);
        poolRoot->setData(0, kConnIdxRole, connIdx);
        poolRoot->setData(0, kPoolNameRole, poolName);
        auto* infoNode = new QTreeWidgetItem(poolRoot);
        infoNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kPoolBlockInfoKey));
        infoNode->setFlags(infoNode->flags() & ~Qt::ItemIsUserCheckable);
        infoNode->setText(0, trk(QStringLiteral("t_pool_info_001"),
                                 QStringLiteral("Información"),
                                 QStringLiteral("Information"),
                                 QStringLiteral("信息")));
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
            auto* combo = new TreeScrollComboBox(tree, tree);
            combo->addItems(options);
            combo->setCurrentIndex(0);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
            combo->setMinimumHeight(22);
            combo->setMaximumHeight(22);
            combo->setFont(tree->font());
            combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 5px; margin:0px;}"));
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
    refreshDatasetExpansionIndicators(tree);

    if (isConnContentInteractive) {
        syncConnContentPropertyColumns();
        restoreConnContentTreeState(m_connContentToken);
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
        applySnapshotVisualState(n);
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
    applySnapshotVisualState(item);
    refreshDatasetExpansionIndicators(tree);
    if (side == QStringLiteral("conncontent")) {
        const bool changedSelection = (tree->currentItem() != item);
        if (changedSelection) {
            tree->setCurrentItem(item);
        } else {
            refreshDatasetProperties(QStringLiteral("conncontent"));
        }
        // Garantiza que, tras recrear filas de propiedades, la selección final
        // siga en el dataset/snapshot y no en una fila auxiliar "Prop.".
        if (tree->currentItem() != item) {
            QPointer<QTreeWidget> safeTree(tree);
            const QString dsKeep = ds;
            const QString snapKeep = snap;
            QTimer::singleShot(0, this, [safeTree, dsKeep, snapKeep]() {
                if (!safeTree || dsKeep.isEmpty()) {
                    return;
                }
                if (QTreeWidgetItem* keep = findDatasetItem(safeTree, dsKeep)) {
                    keep->setData(1, Qt::UserRole, snapKeep);
                    applySnapshotVisualState(keep);
                    safeTree->setCurrentItem(keep);
                }
            });
        }
        updateConnectionActionsState();
        return;
    }
    tree->setCurrentItem(item);
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
        if (m_connActionOrigin.valid) {
            token = QStringLiteral("%1::%2").arg(m_connActionOrigin.connIdx).arg(m_connActionOrigin.poolName);
        }
    } else if (side == QStringLiteral("dest")) {
        if (m_connActionDest.valid) {
            token = QStringLiteral("%1::%2").arg(m_connActionDest.connIdx).arg(m_connActionDest.poolName);
        }
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

    if (side == QStringLiteral("conncontent")) {
        const bool willBeMounted = (desired == Qt::Checked);
        const QString mountedLabel =
            willBeMounted
                ? trk(QStringLiteral("t_montado_a97484"),
                      QStringLiteral("Montado"),
                      QStringLiteral("Mounted"),
                      QStringLiteral("已挂载"))
                : trk(QStringLiteral("t_desmontado001"),
                      QStringLiteral("Desmontado"),
                      QStringLiteral("Unmounted"),
                      QStringLiteral("未挂载"));

        // Reflejar el borrador en la tabla auxiliar de propiedades.
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
                if (key.compare(QStringLiteral("estado"), Qt::CaseInsensitive) != 0) {
                    continue;
                }
                if (v->text() != mountedLabel) {
                    v->setText(mountedLabel);
                }
                break;
            }
        }

        const QString objectName = ctx.snapshotName.isEmpty()
                                       ? ctx.datasetName
                                       : QStringLiteral("%1@%2").arg(ctx.datasetName, ctx.snapshotName);
        QMap<QString, QString> vals = m_connContentPropValuesByObject.value(
            QStringLiteral("%1|%2").arg(m_connContentToken.trimmed().toLower(),
                                        objectName.trimmed().toLower()));
        vals[QStringLiteral("estado")] = mountedLabel;
        updateConnContentPropertyValues(m_connContentToken, objectName, vals);

        m_propsDirty = true;
        updateApplyPropsButtonState();
        updateConnectionActionsState();
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
    if (!ok) {
        if (safeItem) {
            m_loadingDatasetTrees = true;
            safeItem->setCheckState(2, desired == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            m_loadingDatasetTrees = false;
        }
        return;
    }

    // Montar/Desmontar en "conncontent" dispara refresco asíncrono de conexión.
    // No tocar más el árbol aquí para evitar operar sobre estructura en transición.
    if (side == QStringLiteral("conncontent")) {
        updateConnectionActionsState();
        return;
    }

    // Reflejar inmediatamente el estado visual tras una operación correcta.
    if (safeItem) {
        m_loadingDatasetTrees = true;
        safeItem->setCheckState(2, desired);
        m_loadingDatasetTrees = false;
    }
}

void MainWindow::setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName) {
    if (side == QStringLiteral("origin")) {
        DatasetSelectionContext ctx;
        if (!datasetName.isEmpty() && m_connActionOrigin.valid) {
            ctx = m_connActionOrigin;
            ctx.datasetName = datasetName;
            ctx.snapshotName = snapshotName;
            ctx.valid = true;
        } else if (!datasetName.isEmpty()) {
            ctx.valid = true;
            ctx.datasetName = datasetName;
            ctx.snapshotName = snapshotName;
        }
        setConnectionOriginSelection(ctx);
        refreshDatasetProperties(QStringLiteral("origin"));
        return;
    }
    DatasetSelectionContext ctx;
    if (!datasetName.isEmpty() && m_connActionDest.valid) {
        ctx = m_connActionDest;
        ctx.datasetName = datasetName;
        ctx.snapshotName = snapshotName;
        ctx.valid = true;
    } else if (!datasetName.isEmpty()) {
        ctx.valid = true;
        ctx.datasetName = datasetName;
        ctx.snapshotName = snapshotName;
    }
    setConnectionDestinationSelection(ctx);
    refreshDatasetProperties(QStringLiteral("dest"));
}
