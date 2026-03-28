#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QCoreApplication>
#include <QApplication>
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
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
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
constexpr int kConnInlineCellUsedRole = Qt::UserRole + 32;
constexpr int kConnPropGroupNodeRole = Qt::UserRole + 17;
constexpr int kConnPropGroupNameRole = Qt::UserRole + 18;
constexpr int kConnContentNodeRole = Qt::UserRole + 19;
constexpr int kConnSnapshotHoldsNodeRole = Qt::UserRole + 21;
constexpr int kConnSnapshotHoldItemRole = Qt::UserRole + 22;
constexpr int kConnSnapshotHoldTagRole = Qt::UserRole + 23;
constexpr int kConnSnapshotHoldTimestampRole = Qt::UserRole + 24;
constexpr int kConnPermissionsNodeRole = Qt::UserRole + 25;
constexpr int kConnPermissionsKindRole = Qt::UserRole + 26;
constexpr int kConnPermissionsEntryNameRole = Qt::UserRole + 30;
constexpr int kConnGsaNodeRole = Qt::UserRole + 33;
constexpr int kConnPoolAutoSnapshotsNodeRole = Qt::UserRole + 34;
constexpr int kConnPoolAutoSnapshotsDatasetRole = Qt::UserRole + 35;
constexpr char kPoolBlockInfoKey[] = "__pool_block_info__";
constexpr char kGsaBlockInfoKey[] = "__gsa_block_info__";

using DatasetTreeContext = MainWindow::DatasetTreeContext;

bool isConnContentContext(DatasetTreeContext side) {
    return side == DatasetTreeContext::ConnectionContent
           || side == DatasetTreeContext::ConnectionContentMulti;
}

bool isInteractiveConnContentContext(DatasetTreeContext side) {
    return side == DatasetTreeContext::ConnectionContent;
}

QString selectionSideString(DatasetTreeContext side) {
    switch (side) {
    case DatasetTreeContext::Origin:
        return QStringLiteral("origin");
    case DatasetTreeContext::Destination:
        return QStringLiteral("dest");
    case DatasetTreeContext::ConnectionContent:
    case DatasetTreeContext::ConnectionContentMulti:
        return QStringLiteral("conncontent");
    }
    return QString();
}

enum class DatasetPlatformFamily {
    Linux,
    MacOS,
    FreeBSD,
    Windows,
    Other,
};

DatasetPlatformFamily datasetPlatformFamilyFromStrings(const QString& osType, const QString& osLine) {
    const QString merged = (osType + QStringLiteral(" ") + osLine).trimmed().toLower();
    if (merged.contains(QStringLiteral("windows"))) {
        return DatasetPlatformFamily::Windows;
    }
    if (merged.contains(QStringLiteral("darwin")) || merged.contains(QStringLiteral("mac"))) {
        return DatasetPlatformFamily::MacOS;
    }
    if (merged.contains(QStringLiteral("freebsd"))) {
        return DatasetPlatformFamily::FreeBSD;
    }
    if (merged.contains(QStringLiteral("linux"))) {
        return DatasetPlatformFamily::Linux;
    }
    return DatasetPlatformFamily::Other;
}

bool isDatasetPropertySupportedOnPlatform(const QString& propName, DatasetPlatformFamily platform) {
    const QString prop = propName.trimmed().toLower();
    if (prop.isEmpty()) {
        return false;
    }
    if (prop == QStringLiteral("vscan")) {
        return false;
    }
    if (prop == QStringLiteral("jailed")) {
        return platform == DatasetPlatformFamily::FreeBSD;
    }
    if (prop == QStringLiteral("zoned")) {
        return platform == DatasetPlatformFamily::Linux;
    }
    if (prop == QStringLiteral("sharesmb")) {
        return platform != DatasetPlatformFamily::MacOS;
    }
    if (prop == QStringLiteral("nbmand")) {
        return platform == DatasetPlatformFamily::Linux;
    }
    return true;
}

QStringList gsaUserProps() {
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

bool isGsaUserProperty(const QString& prop) {
    for (const QString& key : gsaUserProps()) {
        if (key.compare(prop.trimmed(), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString gsaUserPropertyLabel(const QString& prop) {
    const QString p = prop.trimmed();
    if (p.compare(QStringLiteral("org.fc16.gsa:activado"), Qt::CaseInsensitive) == 0) return QStringLiteral("Activado");
    if (p.compare(QStringLiteral("org.fc16.gsa:recursivo"), Qt::CaseInsensitive) == 0) return QStringLiteral("Recursivo");
    if (p.compare(QStringLiteral("org.fc16.gsa:horario"), Qt::CaseInsensitive) == 0) return QStringLiteral("Horario");
    if (p.compare(QStringLiteral("org.fc16.gsa:diario"), Qt::CaseInsensitive) == 0) return QStringLiteral("Diario");
    if (p.compare(QStringLiteral("org.fc16.gsa:semanal"), Qt::CaseInsensitive) == 0) return QStringLiteral("Semanal");
    if (p.compare(QStringLiteral("org.fc16.gsa:mensual"), Qt::CaseInsensitive) == 0) return QStringLiteral("Mensual");
    if (p.compare(QStringLiteral("org.fc16.gsa:anual"), Qt::CaseInsensitive) == 0) return QStringLiteral("Anual");
    if (p.compare(QStringLiteral("org.fc16.gsa:nivelar"), Qt::CaseInsensitive) == 0) return QStringLiteral("Nivelar");
    if (p.compare(QStringLiteral("org.fc16.gsa:destino"), Qt::CaseInsensitive) == 0) return QStringLiteral("Destino");
    return p;
}

QString gsaUserPropertyDefaultValue(const QString& prop) {
    const QString p = prop.trimmed();
    if (p.compare(QStringLiteral("org.fc16.gsa:destino"), Qt::CaseInsensitive) == 0) return QString();
    if (p.compare(QStringLiteral("org.fc16.gsa:horario"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:diario"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:semanal"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:mensual"), Qt::CaseInsensitive) == 0
        || p.compare(QStringLiteral("org.fc16.gsa:anual"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("0");
    }
    return QStringLiteral("off");
}

bool isGsaOnOffProperty(const QString& prop) {
    const QString p = prop.trimmed();
    return p.compare(QStringLiteral("org.fc16.gsa:activado"), Qt::CaseInsensitive) == 0
           || p.compare(QStringLiteral("org.fc16.gsa:recursivo"), Qt::CaseInsensitive) == 0
           || p.compare(QStringLiteral("org.fc16.gsa:nivelar"), Qt::CaseInsensitive) == 0;
}

bool isAutomaticGsaSnapshotName(const QString& snap) {
    return snap.trimmed().startsWith(QStringLiteral("GSA-"), Qt::CaseInsensitive);
}

QString findCaseInsensitiveMapKey(const QMap<QString, QString>& map, const QString& wanted) {
    const QString target = wanted.trimmed();
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        const QString key = it.key().trimmed();
        if (key.compare(target, Qt::CaseInsensitive) == 0) {
            return it.key();
        }
    }
    return QString();
}

QString findCaseInsensitiveMapKey(const QMap<QString, bool>& map, const QString& wanted) {
    const QString target = wanted.trimmed();
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.key().trimmed().compare(target, Qt::CaseInsensitive) == 0) {
            return it.key();
        }
    }
    return QString();
}

QString connContentChildStableId(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    if (node->data(0, kConnPermissionsNodeRole).toBool()) {
        const QString kind = node->data(0, kConnPermissionsKindRole).toString();
        const QString entry = node->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
        if (!kind.isEmpty()) {
            return QStringLiteral("perm|%1|%2|%3")
                .arg(kind,
                     entry,
                     node->text(0).trimmed());
        }
    }
    if (node->data(0, kConnPropGroupNodeRole).toBool()) {
        const QString groupName = node->data(0, kConnPropGroupNameRole).toString().trimmed();
        const QString propKey = node->data(0, kConnPropKeyRole).toString().trimmed();
        return QStringLiteral("group|%1|%2|%3")
            .arg(groupName,
                 propKey,
                 node->text(0).trimmed());
    }
    if (node->data(0, kConnSnapshotHoldsNodeRole).toBool()) {
        return QStringLiteral("holds|%1").arg(node->text(0).trimmed());
    }
    if (node->data(0, kConnSnapshotHoldItemRole).toBool()) {
        return QStringLiteral("hold|%1").arg(node->data(0, kConnSnapshotHoldTagRole).toString().trimmed());
    }
    if (node->data(0, kConnGsaNodeRole).toBool()) {
        return QStringLiteral("gsa|%1").arg(node->text(0).trimmed());
    }
    return QStringLiteral("text|%1").arg(node->text(0).trimmed());
}

QString normalizedConnContentStateToken(QTreeWidget* tree, const QString& token) {
    if (!tree) {
        return token.trimmed();
    }
    const QString trimmed = token.trimmed();
    const int sep = trimmed.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return trimmed;
    }
    bool ok = false;
    const int connIdx = trimmed.left(sep).toInt(&ok);
    if (!ok || connIdx < 0) {
        return trimmed;
    }
    return QStringLiteral("conn:%1").arg(connIdx);
}

QString connContentChildPath(QTreeWidgetItem* datasetNode, QTreeWidgetItem* node) {
    if (!datasetNode || !node || datasetNode == node) {
        return QString();
    }
    QStringList parts;
    for (QTreeWidgetItem* cur = node; cur && cur != datasetNode; cur = cur->parent()) {
        const QString id = connContentChildStableId(cur);
        if (id.isEmpty()) {
            return QString();
        }
        parts.prepend(id);
    }
    return parts.join(QStringLiteral("/"));
}

QStringList collectExpandedConnContentChildPaths(QTreeWidgetItem* datasetNode) {
    QStringList paths;
    if (!datasetNode) {
        return paths;
    }
    QSet<QString> seen;
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        if (node != datasetNode && node->isExpanded()) {
            const QString path = connContentChildPath(datasetNode, node);
            if (!path.isEmpty() && !seen.contains(path)) {
                seen.insert(path);
                paths.push_back(path);
            }
        }
        for (int i = 0; i < node->childCount(); ++i) {
            rec(node->child(i));
        }
    };
    rec(datasetNode);
    return paths;
}

void restoreExpandedConnContentChildPaths(QTreeWidgetItem* datasetNode, const QStringList& paths) {
    if (!datasetNode || paths.isEmpty()) {
        return;
    }
    const QSet<QString> wanted(paths.cbegin(), paths.cend());
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        if (node != datasetNode) {
            const QString path = connContentChildPath(datasetNode, node);
            if (wanted.contains(path)) {
                node->setExpanded(true);
            }
        }
        for (int i = 0; i < node->childCount(); ++i) {
            rec(node->child(i));
        }
    };
    rec(datasetNode);
}

bool isUserProperty(const QString& prop) {
    return prop.contains(':');
}

bool isDatasetPropertyEditableInline(const QString& propName,
                                     const QString& datasetType,
                                     const QString& source,
                                     const QString& readonly,
                                     DatasetPlatformFamily platform) {
    const QString prop = propName.trimmed().toLower();
    const QString dsType = datasetType.trimmed().toLower();
    const QString src = source.trimmed();
    const QString ro = readonly.trimmed().toLower();
    if (prop.isEmpty()) {
        return false;
    }
    if (!isDatasetPropertySupportedOnPlatform(propName, platform)) {
        return false;
    }
    if (ro == QStringLiteral("true") || ro == QStringLiteral("on") || ro == QStringLiteral("yes") || ro == QStringLiteral("1")) {
        return false;
    }
    if (src == QStringLiteral("-")) {
        return false;
    }
    if (isUserProperty(prop)) {
        return true;
    }

    static const QSet<QString> common = {
        QStringLiteral("atime"), QStringLiteral("relatime"), QStringLiteral("readonly"), QStringLiteral("compression"),
        QStringLiteral("checksum"), QStringLiteral("sync"), QStringLiteral("logbias"), QStringLiteral("primarycache"),
        QStringLiteral("secondarycache"), QStringLiteral("dedup"), QStringLiteral("copies"), QStringLiteral("acltype"),
        QStringLiteral("aclinherit"), QStringLiteral("xattr"), QStringLiteral("normalization"),
        QStringLiteral("casesensitivity"), QStringLiteral("utf8only"), QStringLiteral("keylocation"), QStringLiteral("comment")
    };
    static const QSet<QString> fs = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("mountpoint"), QStringLiteral("canmount"), QStringLiteral("recordsize"), QStringLiteral("quota"),
            QStringLiteral("reservation"), QStringLiteral("refquota"), QStringLiteral("refreservation"),
            QStringLiteral("snapdir"), QStringLiteral("exec"), QStringLiteral("setuid"), QStringLiteral("devices"),
            QStringLiteral("driveletter"), QStringLiteral("sharesmb"), QStringLiteral("sharenfs"),
            QStringLiteral("nbmand"), QStringLiteral("overlay"), QStringLiteral("jailed"),
            QStringLiteral("zoned")
        });
        return s;
    }();
    static const QSet<QString> vol = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("volsize"), QStringLiteral("volblocksize"), QStringLiteral("reservation"),
            QStringLiteral("refreservation"), QStringLiteral("snapdev"), QStringLiteral("volmode")
        });
        return s;
    }();

    if (dsType == QStringLiteral("filesystem")) {
        return fs.contains(prop);
    }
    if (dsType == QStringLiteral("volume")) {
        return vol.contains(prop);
    }
    if (dsType == QStringLiteral("snapshot")) {
        return false;
    }
    return fs.contains(prop) || vol.contains(prop);
}

template <typename Rows>
bool encryptionDisabledForRows(const Rows& rows) {
    for (const auto& row : rows) {
        if (row.prop.compare(QStringLiteral("encryption"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        return row.value.trimmed().compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0;
    }
    return false;
}

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
        const QFont baseFont = parent ? parent->font() : font();
        setFont(baseFont);
        auto* layout = new QHBoxLayout(this);
        // Reserve 1px for left/top/right borders and add a small top inset so
        // each name row reads as a new block under the previous value row.
        layout->setContentsMargins(7, 5, 7, 2);
        layout->setSpacing(8);
        auto* left = new QLabel(name, this);
        left->setFont(baseFont);
        left->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        left->setStyleSheet(QStringLiteral("background: transparent;"));
        layout->addWidget(left, 1);
        auto* right = new QLabel(m_inheritable ? QStringLiteral("Inh.") : QString(), this);
        right->setFont(baseFont);
        right->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        right->setStyleSheet(QStringLiteral("background: transparent; color:#5f6b76;"));
        right->setMinimumWidth(right->fontMetrics().horizontalAdvance(QStringLiteral("Inh.")));
        right->setVisible(m_inheritable);
        layout->addWidget(right, 0);
    }

protected:
    bool event(QEvent* event) override {
        const bool handled = QWidget::event(event);
        if (!event || event->type() != QEvent::Paint) {
            return handled;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor vBorder = connPropVerticalBorderColor(palette());
        const QColor hBorder = connPropHorizontalBorderColor(palette());
        const QRect r = rect();
        painter.fillRect(0, 0, 1, r.height(), vBorder);
        painter.fillRect(qMax(0, r.width() - 1), 0, 1, r.height(), vBorder);
        painter.fillRect(0, 0, r.width(), 1, hBorder);
        return handled;
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
    bool event(QEvent* event) override {
        const bool handled = QWidget::event(event);
        if (!event || event->type() != QEvent::Paint) {
            return handled;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor vBorder = connPropVerticalBorderColor(palette());
        const QColor hBorder = connPropHorizontalBorderColor(palette());
        const QRect r = rect();
        painter.fillRect(0, 0, 1, r.height(), vBorder);
        painter.fillRect(qMax(0, r.width() - 1), 0, 1, r.height(), vBorder);
        painter.fillRect(0, qMax(0, r.height() - 1), r.width(), 1, hBorder);
        return handled;
    }
};

QWidget* wrapInlineCellEditor(QWidget* editor, QTreeWidget* tree) {
    if (!editor || !tree) {
        return editor;
    }
    auto* host = new InlinePropValueWidget(tree);
    host->setFont(tree->font());
    editor->setFont(tree->font());
    QHBoxLayout* layout = host->ensureLayout();
    layout->setContentsMargins(3, 1, 3, 3);
    layout->setSpacing(0);
    layout->addWidget(editor);
    return host;
}

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

QTreeWidgetItem* findDatasetItemByIdentity(QTreeWidget* tree,
                                           int connIdx,
                                           const QString& poolName,
                                           const QString& datasetName) {
    if (!tree || connIdx < 0 || poolName.isEmpty() || datasetName.isEmpty()) {
        return nullptr;
    }
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
        if (!n) {
            return nullptr;
        }
        if (n->data(0, Qt::UserRole).toString().trimmed() == datasetName
            && n->data(0, kConnIdxRole).toInt() == connIdx
            && n->data(0, kPoolNameRole).toString().trimmed() == poolName) {
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
}

QString datasetLeafName(const QString& datasetName) {
    return datasetName.contains('/') ? datasetName.section('/', -1, -1) : datasetName;
}

QIcon treeStandardIcon(QStyle::StandardPixmap sp) {
    QStyle* style = QApplication::style();
    return style ? style->standardIcon(sp) : QIcon();
}

QIcon datasetNodeIcon(QTreeWidgetItem* item) {
    if (!item) {
        return QIcon();
    }
    const QString snap = item->data(1, Qt::UserRole).toString().trimmed();
    if (!snap.isEmpty()) {
        return treeStandardIcon(QStyle::SP_FileIcon);
    }
    return treeStandardIcon(QStyle::SP_DirIcon);
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
    item->setIcon(0, datasetNodeIcon(item));

    const bool hideDatasetChildren = !snap.isEmpty();
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* ch = item->child(i);
        if (!ch) {
            continue;
        }
        const bool isPropRow = ch->data(0, kConnPropRowRole).toBool();
        const bool isContainerNode = ch->data(0, kConnContentNodeRole).toBool();
        const bool isPermissionsNode = ch->data(0, kConnPermissionsNodeRole).toBool();
        const bool isDatasetNode = !ch->data(0, Qt::UserRole).toString().trimmed().isEmpty();
        if ((isDatasetNode || isContainerNode || isPermissionsNode) && !isPropRow) {
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

} // namespace

void MainWindow::resizeTreeColumnsToVisibleContent(QTreeWidget* tree) {
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

void MainWindow::updateConnContentPropertyValues(const QString& token,
                                                 const QString& objectName,
                                                 const QMap<QString, QString>& valuesByProp) {
    const QString t = token.trimmed();
    const QString o = objectName.trimmed();
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

void MainWindow::updateConnContentDraftValue(const QString& token,
                                             const QString& objectName,
                                             const QString& prop,
                                             const QString& value) {
    const QString t = token.trimmed();
    const QString o = objectName.trimmed();
    QString p = prop.trimmed();
    QString normalizedValue = value;
    if (t.isEmpty() || o.isEmpty() || p.isEmpty()) {
        return;
    }
    if (p.compare(QStringLiteral("estado"), Qt::CaseInsensitive) == 0) {
        p = QStringLiteral("mounted");
        const QString s = value.trimmed().toLower();
        const bool mountedOn = (s == QStringLiteral("montado")
                                || s == QStringLiteral("mounted")
                                || s == QStringLiteral("已挂载")
                                || s == QStringLiteral("on")
                                || s == QStringLiteral("yes")
                                || s == QStringLiteral("true")
                                || s == QStringLiteral("1"));
        normalizedValue = mountedOn ? QStringLiteral("yes") : QStringLiteral("no");
    }
    DatasetPropsDraft draft = propertyDraftForObject(QStringLiteral("conncontent"), t, o);
    if (p.compare(QStringLiteral("snapshot"), Qt::CaseInsensitive) == 0) {
        draft.valuesByProp.remove(p);
        draft.inheritByProp.remove(p);
        storePropertyDraftForObject(QStringLiteral("conncontent"), t, o, draft);
        return;
    }
    draft.valuesByProp[p] = normalizedValue;

    const int sep = t.indexOf(QStringLiteral("::"));
    if (sep > 0) {
        bool okConn = false;
        const int connIdx = t.left(sep).toInt(&okConn);
        const QString poolName = t.mid(sep + 2);
        if (okConn && connIdx >= 0 && !poolName.isEmpty()) {
            const QVector<DatasetPropCacheRow> rows =
                datasetPropertyRowsFromModelOrCache(connIdx, poolName, o);
            for (const DatasetPropCacheRow& row : rows) {
                if (row.prop.compare(p, Qt::CaseInsensitive) == 0 && row.value == normalizedValue) {
                    draft.valuesByProp.remove(p);
                    break;
                }
            }
        }
    }

    storePropertyDraftForObject(QStringLiteral("conncontent"), t, o, draft);
}

void MainWindow::updateConnContentDraftInherit(const QString& token,
                                               const QString& objectName,
                                               const QString& prop,
                                               bool inherit) {
    const QString t = token.trimmed();
    const QString o = objectName.trimmed();
    const QString p = prop.trimmed();
    if (t.isEmpty() || o.isEmpty() || p.isEmpty()) {
        return;
    }
    DatasetPropsDraft draft = propertyDraftForObject(QStringLiteral("conncontent"), t, o);
    draft.inheritByProp[p] = inherit;

    const int sep = t.indexOf(QStringLiteral("::"));
    if (sep > 0) {
        bool okConn = false;
        const int connIdx = t.left(sep).toInt(&okConn);
        const QString poolName = t.mid(sep + 2);
        if (okConn && connIdx >= 0 && !poolName.isEmpty()) {
            const QVector<DatasetPropCacheRow> rows =
                datasetPropertyRowsFromModelOrCache(connIdx, poolName, o);
            for (const DatasetPropCacheRow& row : rows) {
                if (row.prop.compare(p, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                const bool original = row.source.trimmed().toLower().startsWith(QStringLiteral("inherited"));
                if (original == inherit) {
                    draft.inheritByProp.remove(p);
                }
                break;
            }
        }
    }

    storePropertyDraftForObject(QStringLiteral("conncontent"), t, o, draft);
}

bool MainWindow::showAutomaticSnapshots() const {
    return m_showAutomaticGsaSnapshots;
}

void MainWindow::syncConnContentPropertyColumnsFor(QTreeWidget* tree, const QString& token) {
    Q_UNUSED(token);
    syncConnContentPropertyColumns(tree);
}

void MainWindow::syncConnContentPropertyColumns(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    const int propCols = qBound(6, m_connPropColumnsSetting, 20);
    if (m_syncingConnContentColumns) {
        return;
    }
    m_syncingConnContentColumns = true;
    const QSignalBlocker blocker(tree);

    QStringList headers = {
        (tree && tree == m_bottomConnContentTree)
            ? trk(QStringLiteral("t_target_dataset_col001"),
                  QStringLiteral("Destino:Dataset"),
                  QStringLiteral("Destination:Dataset"),
                  QStringLiteral("目标:数据集"))
            : trk(QStringLiteral("t_origin_dataset_col001"),
                  QStringLiteral("Origen:Dataset"),
                  QStringLiteral("Origin:Dataset"),
                  QStringLiteral("源:数据集")),
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
            if (c->data(0, kConnPropGroupNodeRole).toBool()) {
                const bool isMainPropsNode =
                    c->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                    && c->text(0).trimmed() == trk(QStringLiteral("t_props_lbl_001"),
                                                   QStringLiteral("Propiedades"),
                                                   QStringLiteral("Properties"),
                                                   QStringLiteral("属性"));
                const bool isGsaNode = c->data(0, kConnGsaNodeRole).toBool()
                                       || c->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kGsaBlockInfoKey);
                if (isMainPropsNode || isGsaNode) {
                    self(self, c);
                } else {
                    delete n->takeChild(i);
                }
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
    const bool showInlineDatasetProps = m_showInlineDatasetProps;
    const bool showInlinePropertyNodes = showInlinePropertyNodesForTree(tree);
    const bool showInlinePermissionsNodes = showInlinePermissionsNodesForTree(tree);
    const bool showInlineGsaNode = showInlineGsaNodeForTree(tree);
    if (!showInlineDatasetProps) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            clearPropRowsRec(clearPropRowsRec, tree->topLevelItem(i));
        }
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
    clearPropRowsRec(clearPropRowsRec, sel);
    const QString obj = snap.isEmpty() ? ds : QStringLiteral("%1@%2").arg(ds, snap);
    const int itemConnIdx = sel->data(0, kConnIdxRole).toInt();
    const QString itemPool = sel->data(0, kPoolNameRole).toString();
    const QString draftToken = QStringLiteral("%1::%2").arg(itemConnIdx).arg(itemPool);
    const QString key = QStringLiteral("%1|%2").arg(draftToken.trimmed(),
                                                    obj.trimmed());
    const auto it = m_connContentPropValuesByObject.constFind(key);
    QMap<QString, QString> displayValues;
    if (it != m_connContentPropValuesByObject.cend() && !it->isEmpty()) {
        displayValues = it.value();
    } else {
        const QVector<DatasetPropCacheRow> fallbackRows =
            datasetPropertyRowsFromModelOrCache(itemConnIdx, itemPool, obj);
        for (const DatasetPropCacheRow& row : fallbackRows) {
            if (!row.prop.trimmed().isEmpty()) {
                displayValues.insert(row.prop, row.value);
            }
        }
        if (displayValues.isEmpty()) {
            m_syncingConnContentColumns = false;
            return;
        }
        updateConnContentPropertyValues(draftToken, obj, displayValues);
    }
    const DatasetPropsDraft objectDraftValue =
        propertyDraftForObject(QStringLiteral("conncontent"), draftToken, obj);
    const DatasetPropsDraft* objectDraft = objectDraftValue.dirty ? &objectDraftValue : nullptr;
    if (objectDraft) {
        for (auto vit = objectDraft->valuesByProp.cbegin(); vit != objectDraft->valuesByProp.cend(); ++vit) {
            const QString existingKey = findCaseInsensitiveMapKey(displayValues, vit.key());
            if (!existingKey.isEmpty()) {
                displayValues[existingKey] = vit.value();
            } else {
                displayValues[vit.key()] = vit.value();
            }
        }
    }

    QStringList props = displayValues.keys();
    for (int i = props.size() - 1; i >= 0; --i) {
        if (isGsaUserProperty(props.at(i))) {
            props.removeAt(i);
        }
    }
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
    const QString fixedSnapshotProp = QStringLiteral("snapshot");
    auto filterPropsByWanted = [](const QStringList& available, const QStringList& wanted) {
        QStringList filtered;
        for (const QString& desired : wanted) {
            for (const QString& have : available) {
                if (desired.compare(have, Qt::CaseInsensitive) == 0) {
                    filtered.push_back(have);
                    break;
                }
            }
        }
        return filtered;
    };
    auto normalizePropsList = [](const QStringList& in) {
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
    const QStringList* savedOrder = &m_datasetInlinePropsOrder;
    const QVector<InlinePropGroupConfig>* savedGroups = &m_datasetInlinePropGroups;
    if (objectIsSnapshot) {
        savedOrder = &m_snapshotInlinePropsOrder;
        savedGroups = &m_snapshotInlinePropGroups;
    }
    auto querySnapshotHolds = [this](int connIdx, const QString& poolName, const QString& objectName) {
        if (!ensureDatasetSnapshotHoldsLoaded(connIdx, poolName, objectName)) {
            return QVector<QPair<QString, QString>>{};
        }
        return datasetSnapshotHolds(connIdx, poolName, objectName);
    };
    QStringList mainProps = props;
    if (!savedOrder->isEmpty()) {
        mainProps = filterPropsByWanted(props, *savedOrder);
    }
    if (objectIsSnapshot) {
        QString canonicalSnapshot;
        for (const QString& p : props) {
            if (p.compare(fixedSnapshotProp, Qt::CaseInsensitive) == 0) {
                canonicalSnapshot = p;
                break;
            }
        }
        if (!canonicalSnapshot.isEmpty()) {
            mainProps.removeAll(canonicalSnapshot);
            mainProps.prepend(canonicalSnapshot);
        }
    }
    mainProps = normalizePropsList(mainProps);
    auto enumValues = connContentEnumValues();
    enumValues.insert(QStringLiteral("org.fc16.gsa:activado"), QStringList{QStringLiteral("off"), QStringLiteral("on")});
    enumValues.insert(QStringLiteral("org.fc16.gsa:recursivo"), QStringList{QStringLiteral("off"), QStringLiteral("on")});
    enumValues.insert(QStringLiteral("org.fc16.gsa:nivelar"), QStringList{QStringLiteral("off"), QStringLiteral("on")});
    const QVector<DatasetPropCacheRow> objectRows =
        datasetPropertyRowsFromModelOrCache(itemConnIdx, itemPool, obj);
    QString objectDatasetType = objectIsSnapshot ? QStringLiteral("snapshot") : QString();
    if (const DSInfo* objectInfo = findDsInfo(itemConnIdx, itemPool, obj);
        objectInfo && !objectInfo->runtime.datasetType.trimmed().isEmpty()) {
        objectDatasetType = objectInfo->runtime.datasetType.trimmed();
    }
    auto gsaBoolOn = [](const QString& raw) {
        const QString v = raw.trimmed().toLower();
        return v == QStringLiteral("on")
               || v == QStringLiteral("yes")
               || v == QStringLiteral("true")
               || v == QStringLiteral("1");
    };
    auto effectiveGsaValuesForDataset = [this, itemConnIdx, itemPool](const QString& datasetName) {
        QMap<QString, QString> values;
        if (itemConnIdx < 0 || itemPool.trimmed().isEmpty() || datasetName.trimmed().isEmpty()) {
            return values;
        }
        ensureDatasetPropertySubsetLoaded(itemConnIdx, itemPool, datasetName, gsaUserProps());
        const QVector<DatasetPropCacheRow> rows =
            datasetPropertyRowsForNames(itemConnIdx, itemPool, datasetName, gsaUserProps());
        for (const DatasetPropCacheRow& row : rows) {
            if (isGsaUserProperty(row.prop)) {
                values[row.prop] = row.value;
            }
        }
        const QString token = QStringLiteral("%1::%2").arg(itemConnIdx).arg(itemPool);
        const QString liveKey = QStringLiteral("%1|%2").arg(token, datasetName);
        const auto liveIt = m_connContentPropValuesByObject.constFind(liveKey);
        if (liveIt != m_connContentPropValuesByObject.cend()) {
            for (auto it = liveIt->cbegin(); it != liveIt->cend(); ++it) {
                if (isGsaUserProperty(it.key())) {
                    const QString key = findCaseInsensitiveMapKey(values, it.key());
                    values[key.isEmpty() ? it.key() : key] = it.value();
                }
            }
        }
        const DatasetPropsDraft draft =
            propertyDraftForObject(QStringLiteral("conncontent"), token, datasetName);
        if (draft.dirty) {
            for (auto it = draft.valuesByProp.cbegin(); it != draft.valuesByProp.cend(); ++it) {
                if (isGsaUserProperty(it.key())) {
                    const QString key = findCaseInsensitiveMapKey(values, it.key());
                    values[key.isEmpty() ? it.key() : key] = it.value();
                }
            }
            for (auto it = draft.inheritByProp.cbegin(); it != draft.inheritByProp.cend(); ++it) {
                if (it.value() && isGsaUserProperty(it.key())) {
                    const QString key = findCaseInsensitiveMapKey(values, it.key());
                    values.remove(key.isEmpty() ? it.key() : key);
                }
            }
        }
        return values;
    };
    auto recursiveGsaAncestorForDataset = [&](const QString& datasetName) {
        QString parent = parentDatasetName(datasetName);
        while (!parent.trimmed().isEmpty()) {
            const QMap<QString, QString> values = effectiveGsaValuesForDataset(parent);
            if (gsaBoolOn(values.value(findCaseInsensitiveMapKey(values, QStringLiteral("org.fc16.gsa:activado"))))
                && gsaBoolOn(values.value(findCaseInsensitiveMapKey(values, QStringLiteral("org.fc16.gsa:recursivo"))))) {
                return parent;
            }
            parent = parentDatasetName(parent);
        }
        return QString();
    };
    const QString recursiveGsaAncestor = objectIsSnapshot ? QString() : recursiveGsaAncestorForDataset(obj);
    const DatasetPlatformFamily platform =
        datasetPlatformFamilyFromStrings(
            (itemConnIdx >= 0 && itemConnIdx < m_profiles.size()) ? m_profiles[itemConnIdx].osType : QString(),
            (itemConnIdx >= 0 && itemConnIdx < m_states.size()) ? m_states[itemConnIdx].osLine : QString());
    auto isReadonlyFlag = [](const QString& v) {
        const QString s = v.trimmed().toLower();
        return s == QStringLiteral("true") || s == QStringLiteral("on")
               || s == QStringLiteral("yes") || s == QStringLiteral("1");
    };
    auto isEditableProp = [this, &objectRows, &objectDatasetType, &isReadonlyFlag, platform](const QString& prop) -> bool {
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
        if (objectRows.isEmpty()) {
            return false;
        }
        const bool encryptionOff = encryptionDisabledForRows(objectRows);
        for (const DatasetPropCacheRow& row : objectRows) {
            if (row.prop.compare(prop, Qt::CaseInsensitive) != 0) {
                continue;
            }
            return isDatasetPropertyEditableInline(row.prop,
                                                   objectDatasetType,
                                                   row.source,
                                                   row.readonly,
                                                   platform)
                   && !(row.prop.compare(QStringLiteral("keylocation"), Qt::CaseInsensitive) == 0 && encryptionOff);
        }
        return false;
    };
    auto isInheritableProp = [this, &objectRows, &objectDatasetType, &isReadonlyFlag, objectIsSnapshot, platform](const QString& prop) -> bool {
        if (objectIsSnapshot) {
            return false;
        }
        const QString p = prop.trimmed();
        if (p.isEmpty()) {
            return false;
        }
        if (p.compare(QStringLiteral("canmount"), Qt::CaseInsensitive) == 0) {
            return false;
        }
        if (isGsaUserProperty(p) || p.contains(QLatin1Char(':'))) {
            return false;
        }
        if (objectRows.isEmpty()) {
            return false;
        }
        const bool encryptionOff = encryptionDisabledForRows(objectRows);
        for (const DatasetPropCacheRow& row : objectRows) {
            if (row.prop.compare(prop, Qt::CaseInsensitive) != 0) {
                continue;
            }
            return isDatasetPropertyEditableInline(row.prop,
                                                   objectDatasetType,
                                                   row.source,
                                                   row.readonly,
                                                   platform)
                   && !(row.prop.compare(QStringLiteral("keylocation"), Qt::CaseInsensitive) == 0 && encryptionOff);
        }
        return false;
    };
    auto isCurrentlyInheritedProp = [&objectRows](const QString& prop) -> bool {
        if (objectRows.isEmpty()) {
            return false;
        }
        for (const DatasetPropCacheRow& row : objectRows) {
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
    auto draftInheritForProp = [objectDraft](const QString& prop, bool* found = nullptr) -> bool {
        if (found) {
            *found = false;
        }
        if (!objectDraft) {
            return false;
        }
        const QString key = findCaseInsensitiveMapKey(objectDraft->inheritByProp, prop);
        if (key.isEmpty()) {
            return false;
        }
        if (found) {
            *found = true;
        }
        return objectDraft->inheritByProp.value(key);
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
    const QColor unsupportedColor = tree->palette().color(QPalette::Disabled, QPalette::Text);
    const QString unsupportedReason =
        trk(QStringLiteral("t_prop_unsupported_platform_001"),
            QStringLiteral("Propiedad no soportada en este sistema operativo."));
    auto appendPropRows = [&](QTreeWidgetItem* parentNode,
                              const QString& title,
                              const QStringList& groupProps,
                              bool titleInFirstRow,
                              int insertAt) {
        for (int base = 0; base < groupProps.size(); base += propCols) {
        auto* rowNames = new QTreeWidgetItem();
        rowNames->setData(0, kConnPropRowRole, true);
        rowNames->setData(0, kConnPropRowKindRole, 1);
        rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
        rowNames->setText(0, (base == 0 && titleInFirstRow) ? title : QString());
        auto* rowValues = new QTreeWidgetItem();
        rowValues->setData(0, kConnPropRowRole, true);
        rowValues->setData(0, kConnPropRowKindRole, 2);
        rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
        rowValues->setText(0, QString());
        rowValues->setSizeHint(0, QSize(0, 33));
        const QColor nameRowBg(232, 240, 250);
        if (base == 0 && titleInFirstRow) {
            rowNames->setBackground(0, QBrush(nameRowBg));
        }
            if (insertAt >= 0) {
                parentNode->insertChild(insertAt++, rowNames);
                parentNode->insertChild(insertAt++, rowValues);
            } else {
                parentNode->addChild(rowNames);
                parentNode->addChild(rowValues);
            }

            for (int off = 0; off < propCols; ++off) {
            const int idx = base + off;
            if (idx >= groupProps.size()) {
                break;
            }
            const QString& prop = groupProps.at(idx);
            const int col = 4 + off;
            const QString value = displayValues.value(prop);
            const QString propLower = prop.trimmed().toLower();
            const bool inheritable = isInheritableProp(prop);
            rowNames->setData(col, kConnInlineCellUsedRole, true);
            rowValues->setData(col, kConnInlineCellUsedRole, true);
            rowNames->setBackground(col, QBrush(nameRowBg));
            const QString propLabel =
                (propLower == QStringLiteral("estado"))
                    ? trk(QStringLiteral("t_montado_a97484"),
                          QStringLiteral("Montado"),
                          QStringLiteral("Mounted"),
                          QStringLiteral("已挂载"))
                    : (isGsaUserProperty(prop) ? gsaUserPropertyLabel(prop) : prop);
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
            const bool editable = isEditableProp(prop);
            const bool supported = isDatasetPropertySupportedOnPlatform(prop, platform);
            rowValues->setData(col, kConnPropEditableRole, editable);
            if (!supported) {
                rowNames->setForeground(col, unsupportedColor);
                rowValues->setForeground(col, unsupportedColor);
                rowNames->setToolTip(col, unsupportedReason);
                rowValues->setToolTip(col, unsupportedReason);
            }
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
                tree->setItemWidget(rowValues, col, wrapInlineCellEditor(combo, tree));
                QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, sel, rowValues, col](const QString& txt) {
                    if (!sel || !rowValues) {
                        return;
                    }
                    rowValues->setText(col, txt);
                    onSnapshotComboChanged(tree, sel, DatasetTreeContext::ConnectionContent, txt);
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
                tree->setItemWidget(rowValues, col, wrapInlineCellEditor(combo, tree));
                rowValues->setData(col, kConnPropEditableRole, true);
                QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, sel, rowValues, col, combo](const QString& txt) {
                    if (!sel || !rowValues || !combo) {
                        return;
                    }
                    const bool on = (txt.trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0);
                    const Qt::CheckState desired = on ? Qt::Checked : Qt::Unchecked;
                    const QSignalBlocker blocker(combo);
                    sel->setCheckState(2, desired);
                    onDatasetTreeItemChanged(tree, sel, 2, DatasetTreeContext::ConnectionContent);
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
                    int propRow =
                        (m_propsToken.trimmed() == draftToken.trimmed()
                         && m_propsDataset.trimmed() == obj)
                            ? findPropsTableRowByProp(prop)
                            : -1;
                    bool inheritChecked = isCurrentlyInheritedProp(prop);
                    bool draftInheritFound = false;
                    const bool draftInherit = draftInheritForProp(prop, &draftInheritFound);
                    if (draftInheritFound) {
                        inheritChecked = draftInherit;
                    }
                    if (propRow >= 0) {
                        if (QTableWidgetItem* pi = m_connContentPropsTable->item(propRow, 2)) {
                            inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable)
                                             && pi->checkState() == Qt::Checked;
                        }
                    }
                    inheritCombo->setCurrentText(inheritChecked ? QStringLiteral("on") : QStringLiteral("off"));
                    auto applyInheritedVisualState = [](QWidget* editor, bool inheritOn) {
                        if (!editor) {
                            return;
                        }
                        editor->setEnabled(!inheritOn);
                        editor->setProperty("inheritedDisabled", inheritOn);
                        if (qobject_cast<QLineEdit*>(editor)) {
                            editor->setStyleSheet(inheritOn
                                                      ? QStringLiteral("QLineEdit{padding:0 5px; margin:0px; background:#f3f5f7; color:#7a8691;}")
                                                      : QStringLiteral("QLineEdit{padding:0 5px; margin:0px;}"));
                        } else if (qobject_cast<QComboBox*>(editor)) {
                            editor->setStyleSheet(inheritOn
                                                      ? QStringLiteral("QComboBox{padding:0 5px; margin:0px; background:#f3f5f7; color:#7a8691;}")
                                                      : QStringLiteral("QComboBox{padding:0 5px; margin:0px;}"));
                        }
                    };
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
                        valueEditor = combo;
                        lay->addWidget(combo, 1);
                        QObject::connect(combo, &QComboBox::currentTextChanged, tree,
                                         [this, tree, rowValues, col, inheritCombo, draftToken, obj](const QString& txt) {
                            if (!rowValues) {
                                return;
                            }
                            rowValues->setText(col, txt);
                            if (inheritCombo && inheritCombo->currentText() != QStringLiteral("off")) {
                                const QSignalBlocker blocker(inheritCombo);
                                inheritCombo->setCurrentText(QStringLiteral("off"));
                            }
                            updateConnContentDraftInherit(draftToken,
                                                          obj,
                                                          rowValues->data(col, kConnPropKeyRole).toString(),
                                                          false);
                            onDatasetTreeItemChanged(tree, rowValues, col, DatasetTreeContext::ConnectionContent);
                        });
                    } else {
                        auto* edit = new QLineEdit(cell);
                        edit->setText(value);
                        edit->setMinimumHeight(22);
                        edit->setMaximumHeight(22);
                        edit->setFont(tree->font());
                        edit->setStyleSheet(QStringLiteral("QLineEdit{padding:0 5px; margin:0px;}"));
                        valueEditor = edit;
                        lay->addWidget(edit, 1);
                        QObject::connect(edit, &QLineEdit::editingFinished, tree,
                                         [this, tree, rowValues, col, edit, inheritCombo, draftToken, obj]() {
                            if (!rowValues || !edit) {
                                return;
                            }
                            rowValues->setText(col, edit->text());
                            if (inheritCombo && inheritCombo->currentText() != QStringLiteral("off")) {
                                const QSignalBlocker blocker(inheritCombo);
                                inheritCombo->setCurrentText(QStringLiteral("off"));
                            }
                            updateConnContentDraftInherit(draftToken,
                                                          obj,
                                                          rowValues->data(col, kConnPropKeyRole).toString(),
                                                          false);
                            onDatasetTreeItemChanged(tree, rowValues, col, DatasetTreeContext::ConnectionContent);
                        });
                    }
                    applyInheritedVisualState(valueEditor, inheritChecked);
                    lay->addWidget(inheritCombo, 0);
                    tree->setItemWidget(rowValues, col, cell);
                    QObject::connect(inheritCombo, &QComboBox::currentTextChanged, tree,
                                     [this, valueEditor, prop, inheritCombo, draftToken, obj, applyInheritedVisualState](const QString& txt) {
                        const bool inheritOn = (txt.trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0);
                        applyInheritedVisualState(valueEditor, inheritOn);
                        updateConnContentDraftInherit(draftToken, obj, prop, inheritOn);
                        if (!m_connContentPropsTable
                            || m_propsToken.trimmed() != draftToken.trimmed()
                            || m_propsDataset.trimmed() != obj) {
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
                    tree->setItemWidget(rowValues, col, wrapInlineCellEditor(combo, tree));
                    QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, rowValues, col](const QString& txt) {
                        if (!rowValues) {
                            return;
                        }
                        rowValues->setText(col, txt);
                        onDatasetTreeItemChanged(tree, rowValues, col, DatasetTreeContext::ConnectionContent);
                    });
                }
                if (propLower == QStringLiteral("normalization")) {
                    appLog(QStringLiteral("DEBUG"),
                           QStringLiteral("conncontent Prop. normalization editable=%1 enum=%2 combo=%3")
                               .arg(editable ? QStringLiteral("1") : QStringLiteral("0"),
                                    (eIt != enumValues.cend()) ? QStringLiteral("1") : QStringLiteral("0"),
                                    (eIt != enumValues.cend()) ? QStringLiteral("1") : QStringLiteral("0")));
                }
            } else if (!supported) {
                rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsEditable);
            }
            }
        }
        return insertAt;
    };
    auto appendStaticPropRows = [&](QTreeWidgetItem* parentNode,
                                    const QMap<QString, QString>& valuesByName,
                                    int insertAt) {
        const QStringList groupProps = valuesByName.keys();
        const QColor nameRowBg(232, 240, 250);
        for (int base = 0; base < groupProps.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem();
            rowNames->setData(0, kConnPropRowRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            auto* rowValues = new QTreeWidgetItem();
            rowValues->setData(0, kConnPropRowRole, true);
            rowValues->setData(0, kConnPropRowKindRole, 2);
            rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
            rowValues->setText(0, QString());
            rowValues->setSizeHint(0, QSize(0, 33));
            for (int col = 0; col < tree->columnCount(); ++col) {
                rowNames->setBackground(col, QBrush(nameRowBg));
            }
            if (insertAt >= 0) {
                parentNode->insertChild(insertAt++, rowNames);
                parentNode->insertChild(insertAt++, rowValues);
            } else {
                parentNode->addChild(rowNames);
                parentNode->addChild(rowValues);
            }
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= groupProps.size()) {
                    break;
                }
                const QString& prop = groupProps.at(idx);
                const int col = 4 + off;
                rowNames->setText(col, prop);
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                rowNames->setData(col, kConnPropKeyRole, prop);
                rowValues->setText(col, valuesByName.value(prop));
                rowValues->setTextAlignment(col, Qt::AlignCenter);
                rowValues->setData(col, kConnPropKeyRole, prop);
                rowValues->setData(col, kConnPropEditableRole, false);
            }
        }
        return insertAt;
    };
    auto appendSnapshotHoldsNode = [&](QTreeWidgetItem* parentNode, int insertPos) {
        if (!parentNode || !objectIsSnapshot) {
            return insertPos;
        }
        const QVector<QPair<QString, QString>> snapshotHolds = querySnapshotHolds(itemConnIdx, itemPool, obj);
        auto* holdsNode = new QTreeWidgetItem();
        holdsNode->setText(0,
                           trk(QStringLiteral("t_holds_node_001"),
                               QStringLiteral("Holds"),
                               QStringLiteral("Holds"),
                               QStringLiteral("Holds"))
                               + QStringLiteral(" (%1)").arg(snapshotHolds.size()));
        holdsNode->setIcon(0, treeStandardIcon(QStyle::SP_FileDialogContentsView));
        holdsNode->setData(0, kConnPropGroupNodeRole, true);
        holdsNode->setData(0, kConnSnapshotHoldsNodeRole, true);
        holdsNode->setData(0, kConnIdxRole, itemConnIdx);
        holdsNode->setData(0, kPoolNameRole, itemPool);
        holdsNode->setExpanded(false);
        parentNode->insertChild(insertPos++, holdsNode);
        for (const auto& holdEntry : snapshotHolds) {
            auto* holdItem = new QTreeWidgetItem(holdsNode);
            holdItem->setText(0, holdEntry.first);
            holdItem->setIcon(0, treeStandardIcon(QStyle::SP_FileIcon));
            holdItem->setData(0, kConnSnapshotHoldItemRole, true);
            holdItem->setData(0, kConnSnapshotHoldTagRole, holdEntry.first);
            holdItem->setData(0, kConnSnapshotHoldTimestampRole, holdEntry.second);
            holdItem->setData(0, kConnIdxRole, itemConnIdx);
            holdItem->setData(0, kPoolNameRole, itemPool);
            holdItem->setFlags(holdItem->flags() & ~Qt::ItemIsUserCheckable);
            appendStaticPropRows(holdItem,
                                 QMap<QString, QString>{
                                     {QStringLiteral("TimeStamp"), holdEntry.second}
                                 },
                                 -1);
        }
        return insertPos;
    };
    auto clearNodeChildrenAndCells = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        while (node->childCount() > 0) {
            delete node->takeChild(0);
        }
        for (int col = 4; col < tree->columnCount(); ++col) {
            if (QWidget* w = tree->itemWidget(node, col)) {
                tree->removeItemWidget(node, col);
                w->deleteLater();
            }
            node->setText(col, QString());
            node->setData(col, kConnPropKeyRole, QVariant());
            node->setData(col, kConnPropEditableRole, false);
        }
    };
    int insertAt = 0;
    if (!showInlineDatasetProps) {
        for (int i = sel->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* child = sel->child(i);
            if (!child || !child->data(0, kConnPropGroupNodeRole).toBool()) {
                continue;
            }
            delete sel->takeChild(i);
        }
        insertAt = appendSnapshotHoldsNode(sel, insertAt);
        refreshDatasetExpansionIndicators(tree);
        sel->setExpanded(true);
        resizeTreeColumnsToVisibleContent(tree);
        m_syncingConnContentColumns = false;
        return;
    }
    QTreeWidgetItem* propsNode = nullptr;
    bool propsNodeWasExpanded = false;
    bool shouldExpandPropsNode = false;
    QMap<QString, bool> propGroupExpandedByName;
    QTreeWidgetItem* gsaNode = nullptr;
    bool gsaNodeWasExpanded = false;
    for (int i = sel->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* child = sel->child(i);
        if (!child || !child->data(0, kConnPropGroupNodeRole).toBool()) {
            continue;
        }
        if (child->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
            && child->text(0).trimmed() == trk(QStringLiteral("t_props_lbl_001"),
                                               QStringLiteral("Propiedades"),
                                               QStringLiteral("Properties"),
                                               QStringLiteral("属性"))) {
            if (!propsNode) {
                propsNode = child;
                propsNodeWasExpanded = child->isExpanded();
                for (int c = 0; c < child->childCount(); ++c) {
                    QTreeWidgetItem* groupChild = child->child(c);
                    if (!groupChild || !groupChild->data(0, kConnPropGroupNodeRole).toBool()) {
                        continue;
                    }
                    const QString groupName = groupChild->data(0, kConnPropGroupNameRole).toString().trimmed();
                    if (!groupName.isEmpty()) {
                        propGroupExpandedByName.insert(groupName.toLower(), groupChild->isExpanded());
                    }
                }
            } else {
                delete sel->takeChild(i);
            }
            continue;
        }
        if (child->data(0, kConnGsaNodeRole).toBool()
            || child->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kGsaBlockInfoKey)) {
            if (!gsaNode) {
                gsaNode = child;
                gsaNodeWasExpanded = child->isExpanded();
            } else {
                delete sel->takeChild(i);
            }
        }
    }
    if (showInlinePropertyNodes && !propsNode) {
        propsNode = new QTreeWidgetItem();
        propsNode->setText(0, trk(QStringLiteral("t_props_lbl_001"),
                                  QStringLiteral("Propiedades"),
                                  QStringLiteral("Properties"),
                                  QStringLiteral("属性")));
        propsNode->setData(0, kConnPropGroupNodeRole, true);
        propsNode->setData(0, kConnPropGroupNameRole, QString());
        propsNode->setData(0, kConnIdxRole, itemConnIdx);
        propsNode->setData(0, kPoolNameRole, itemPool);
        sel->insertChild(insertAt++, propsNode);
    }
    if (showInlinePropertyNodes && propsNode) {
        shouldExpandPropsNode = propsNodeWasExpanded || tree->currentItem() == propsNode;
        clearNodeChildrenAndCells(propsNode);
        if (!mainProps.isEmpty()) {
            appendPropRows(propsNode,
                           QString(),
                           mainProps,
                           false,
                           -1);
        }
        for (const InlinePropGroupConfig& cfg : *savedGroups) {
            QStringList wantedProps = cfg.props;
            if (objectIsSnapshot) {
                for (int i = wantedProps.size() - 1; i >= 0; --i) {
                    if (wantedProps.at(i).compare(fixedSnapshotProp, Qt::CaseInsensitive) == 0) {
                        wantedProps.removeAt(i);
                    }
                }
            }
            const QStringList groupProps = normalizePropsList(filterPropsByWanted(props, wantedProps));
            if (groupProps.isEmpty()) {
                continue;
            }
            auto* groupNode = new QTreeWidgetItem();
            groupNode->setText(0, cfg.name);
            groupNode->setData(0, kConnPropGroupNodeRole, true);
            groupNode->setData(0, kConnPropGroupNameRole, cfg.name);
            groupNode->setData(0, kConnIdxRole, itemConnIdx);
            groupNode->setData(0, kPoolNameRole, itemPool);
            propsNode->addChild(groupNode);
            appendPropRows(groupNode, cfg.name, groupProps, false, -1);
            const auto expandedIt = propGroupExpandedByName.constFind(cfg.name.trimmed().toLower());
            const bool expandGroup =
                (expandedIt != propGroupExpandedByName.cend())
                    ? (expandedIt.value() || (shouldExpandPropsNode && mainProps.isEmpty()))
                    : (shouldExpandPropsNode && mainProps.isEmpty());
            groupNode->setExpanded(expandGroup);
        }
    } else if (propsNode) {
        delete sel->takeChild(sel->indexOfChild(propsNode));
        propsNode = nullptr;
    }
    const QString mountpointForGsa = displayValues.value(QStringLiteral("mountpoint")).trimmed();
    const bool datasetSupportsGsa = !objectIsSnapshot && !mountpointForGsa.isEmpty() && mountpointForGsa != QStringLiteral("-");
    if (datasetSupportsGsa && showInlineGsaNode) {
        if (!gsaNode) {
            gsaNode = new QTreeWidgetItem();
            gsaNode->setText(0, QStringLiteral("Programar snapshots"));
            gsaNode->setIcon(0, treeStandardIcon(QStyle::SP_BrowserReload));
            gsaNode->setData(0, kConnGsaNodeRole, true);
            gsaNode->setData(0, kConnPropGroupNodeRole, true);
            gsaNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kGsaBlockInfoKey));
            gsaNode->setData(0, kConnIdxRole, itemConnIdx);
            gsaNode->setData(0, kPoolNameRole, itemPool);
            gsaNode->setFlags(gsaNode->flags() & ~Qt::ItemIsUserCheckable);
            sel->insertChild(insertAt++, gsaNode);
        }
        clearNodeChildrenAndCells(gsaNode);
        const bool gsaInstalled = (itemConnIdx >= 0
                                   && itemConnIdx < m_states.size()
                                   && m_states[itemConnIdx].gsaInstalled);
        if (!gsaInstalled) {
            auto* msgItem = new QTreeWidgetItem(gsaNode);
            msgItem->setText(0,
                             QStringLiteral("Por favor instale el GSA en esta conexión desde la tabla Conexiones"));
            msgItem->setFlags(msgItem->flags() & ~Qt::ItemIsUserCheckable);
            msgItem->setFirstColumnSpanned(true);
            msgItem->setToolTip(0, msgItem->text(0));
        } else if (!recursiveGsaAncestor.isEmpty()) {
            auto* msgItem = new QTreeWidgetItem(gsaNode);
            msgItem->setText(0, QStringLiteral("Programación gestionada desde ancestro"));
            msgItem->setFlags(msgItem->flags() & ~Qt::ItemIsUserCheckable);
            msgItem->setFirstColumnSpanned(true);
            msgItem->setToolTip(0, QStringLiteral("Programación gestionada desde ancestro: %1").arg(recursiveGsaAncestor));
        } else {
            QMap<QString, QString> gsaValues;
            for (const QString& gsaProp : gsaUserProps()) {
                const QString existingKey = findCaseInsensitiveMapKey(displayValues, gsaProp);
                gsaValues[gsaProp] = existingKey.isEmpty() ? gsaUserPropertyDefaultValue(gsaProp)
                                                           : displayValues.value(existingKey);
            }
            if (objectDraft) {
                for (auto itDraft = objectDraft->valuesByProp.cbegin(); itDraft != objectDraft->valuesByProp.cend(); ++itDraft) {
                    if (isGsaUserProperty(itDraft.key())) {
                        const QString existingKey = findCaseInsensitiveMapKey(gsaValues, itDraft.key());
                        gsaValues[existingKey.isEmpty() ? itDraft.key() : existingKey] = itDraft.value();
                    }
                }
                for (auto itDraft = objectDraft->inheritByProp.cbegin(); itDraft != objectDraft->inheritByProp.cend(); ++itDraft) {
                    if (itDraft.value() && isGsaUserProperty(itDraft.key())) {
                        const QString existingKey = findCaseInsensitiveMapKey(gsaValues, itDraft.key());
                        const QString key = existingKey.isEmpty() ? itDraft.key() : existingKey;
                        gsaValues[key] = gsaUserPropertyDefaultValue(key);
                    }
                }
            }
            const QStringList gsaProps = gsaUserProps();
            const QMap<QString, QString> savedDisplayValues = displayValues;
            displayValues = gsaValues;
            appendPropRows(gsaNode, QString(), gsaProps, false, -1);
            displayValues = savedDisplayValues;
        }
        gsaNode->setExpanded(gsaNodeWasExpanded || tree->currentItem() == gsaNode);
    } else if (gsaNode) {
        delete sel->takeChild(sel->indexOfChild(gsaNode));
        gsaNode = nullptr;
    }
    insertAt = appendSnapshotHoldsNode(sel, insertAt);
    if (propsNode) {
        propsNode->setExpanded(shouldExpandPropsNode);
    }
    const QString activeTreeToken = connContentTokenForTree(tree);
    if (!activeTreeToken.trimmed().isEmpty()) {
        const QString normalizedToken = normalizedConnContentStateToken(tree, activeTreeToken);
        const QString scopedToken =
            normalizedToken
            + ((tree == m_bottomConnContentTree) ? QStringLiteral("|bottom")
                                                 : QStringLiteral("|top"));
        const auto stateIt = m_connContentTreeStateByToken.constFind(scopedToken);
        const QString selectedDatasetName = sel->data(0, Qt::UserRole).toString().trimmed();
        if (stateIt != m_connContentTreeStateByToken.cend() && !selectedDatasetName.isEmpty()) {
            const auto childIt = stateIt->expandedChildPathsByDataset.constFind(selectedDatasetName);
            if (childIt != stateIt->expandedChildPathsByDataset.cend()) {
                restoreExpandedConnContentChildPaths(sel, childIt.value());
            }
        }
    }
    auto refreshVisiblePermissionsNodes = [&](auto&& self, QTreeWidgetItem* node) -> void {
        if (!node) {
            return;
        }
        const QString datasetName = node->data(0, Qt::UserRole).toString().trimmed();
        const QString snapshotName = node->data(1, Qt::UserRole).toString().trimmed();
        if (!datasetName.isEmpty() && snapshotName.isEmpty()) {
            QTreeWidgetItem* permissionsNode = nullptr;
            for (int i = 0; i < node->childCount(); ++i) {
                QTreeWidgetItem* child = node->child(i);
                if (child && child->data(0, kConnPermissionsNodeRole).toBool()
                    && child->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")) {
                    permissionsNode = child;
                    break;
                }
            }
            if (permissionsNode && !permissionsNode->isHidden()
                && (permissionsNode->isExpanded() || permissionsNode->childCount() > 0)) {
                populateDatasetPermissionsNode(tree, node, false);
            }
        }
        for (int i = 0; i < node->childCount(); ++i) {
            self(self, node->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        refreshVisiblePermissionsNodes(refreshVisiblePermissionsNodes, tree->topLevelItem(i));
    }
    refreshDatasetExpansionIndicators(tree);
    sel->setExpanded(true);
    resizeTreeColumnsToVisibleContent(tree);
    m_syncingConnContentColumns = false;
}

void MainWindow::syncConnContentPropertyColumns() {
    syncConnContentPropertyColumns(m_connContentTree);
}

void MainWindow::syncConnContentPoolColumns(QTreeWidget* tree, const QString& token) {
    if (!tree) {
        return;
    }
    const bool rebuildingCurrentTree =
        (tree == m_bottomConnContentTree) ? m_rebuildingBottomConnContentTree
                                          : m_rebuildingTopConnContentTree;
    if (rebuildingCurrentTree) {
        return;
    }
    if (m_syncingConnContentColumns) {
        return;
    }
    QPointer<QTreeWidget> safeTree(tree);
    m_syncingConnContentColumns = true;
    const QSignalBlocker blocker(tree);
    const bool sortingWasEnabled = tree->isSortingEnabled();
    tree->setSortingEnabled(false);
    const auto restoreSorting = qScopeGuard([&]() {
        if (safeTree) {
            tree->setSortingEnabled(sortingWasEnabled);
        }
    });
    const int propCols = qBound(6, m_connPropColumnsSetting, 20);
    QStringList headers;
    headers << ((tree == m_bottomConnContentTree)
                    ? trk(QStringLiteral("t_target_pool_col001"),
                          QStringLiteral("Destino:Pool"),
                          QStringLiteral("Destination:Pool"),
                          QStringLiteral("目标:存储池"))
                    : trk(QStringLiteral("t_origin_pool_col001"),
                          QStringLiteral("Origen:Pool"),
                          QStringLiteral("Origin:Pool"),
                          QStringLiteral("源:存储池")))
            << trk(QStringLiteral("t_snapshot_col01"), QStringLiteral("Snapshot"), QStringLiteral("Snapshot"), QStringLiteral("快照"))
            << trk(QStringLiteral("t_montado_a97484"), QStringLiteral("Montado"), QStringLiteral("Mounted"), QStringLiteral("已挂载"))
            << trk(QStringLiteral("t_mountpoint_001"), QStringLiteral("Mountpoint"), QStringLiteral("Mountpoint"), QStringLiteral("挂载点"));
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
    for (int col = 1; col < tree->columnCount(); ++col) {
        tree->header()->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    if (tree->columnCount() > 1) tree->setColumnHidden(1, true);
    if (tree->columnCount() > 2) tree->setColumnHidden(2, true);
    if (tree->columnCount() > 3) tree->setColumnHidden(3, true);
    for (int col = 4; col < tree->columnCount(); ++col) {
        tree->setColumnHidden(col, false);
    }
    for (int col = 4; col < (4 + propCols) && col < tree->columnCount(); ++col) {
        if (tree->columnWidth(col) < 32) {
            tree->setColumnWidth(col, 96);
        }
    }

    int wantedConnIdx = -1;
    QString wantedPoolName;
    const QString resolvedToken = token.trimmed().isEmpty() ? connContentTokenForTree(tree).trimmed()
                                                            : token.trimmed();
    const int sep = resolvedToken.indexOf(QStringLiteral("::"));
    if (sep > 0) {
        bool ok = false;
        wantedConnIdx = resolvedToken.left(sep).toInt(&ok);
        wantedPoolName = resolvedToken.mid(sep + 2).trimmed();
        if (!ok) {
            wantedConnIdx = -1;
            wantedPoolName.clear();
        }
    }
    auto poolRoots = [&]() {
        QVector<QTreeWidgetItem*> roots;
        if (!safeTree) {
            return roots;
        }
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* n = tree->topLevelItem(i);
            if (n && n->data(0, kIsPoolRootRole).toBool()) {
                roots.push_back(n);
            }
        }
        if (wantedConnIdx >= 0 && !wantedPoolName.isEmpty()) {
            std::stable_sort(roots.begin(), roots.end(), [&](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                const bool aWanted = a && a->data(0, kConnIdxRole).toInt() == wantedConnIdx
                                     && a->data(0, kPoolNameRole).toString().trimmed() == wantedPoolName;
                const bool bWanted = b && b->data(0, kConnIdxRole).toInt() == wantedConnIdx
                                     && b->data(0, kPoolNameRole).toString().trimmed() == wantedPoolName;
                return aWanted && !bWanted;
            });
        }
        return roots;
    };

    QVector<QTreeWidgetItem*> roots = poolRoots();
    if (roots.isEmpty()) {
        m_syncingConnContentColumns = false;
        return;
    }
    if (!showPoolInfoNodeForTree(tree)) {
        for (QTreeWidgetItem* root : roots) {
            if (!root) {
                continue;
            }
            for (int i = root->childCount() - 1; i >= 0; --i) {
                QTreeWidgetItem* c = root->child(i);
                if (c && c->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                    delete root->takeChild(i);
                }
            }
        }
        resizeTreeColumnsToVisibleContent(tree);
        m_syncingConnContentColumns = false;
        return;
    }

    auto filterPropsByWanted = [](const QStringList& available, const QStringList& wanted) {
        QStringList filtered;
        for (const QString& desired : wanted) {
            for (const QString& have : available) {
                if (desired.compare(have, Qt::CaseInsensitive) == 0) {
                    filtered.push_back(have);
                    break;
                }
            }
        }
        return filtered;
    };
    const QColor nameRowBg(232, 240, 250);
    auto addSectionRows = [&](QTreeWidgetItem* parent,
                              const QString& title,
                              const QStringList& names,
                              const QMap<QString, QString>* valuesByName,
                              bool namesOnly,
                              bool collapsible = false,
                              bool expanded = true) {
        if (!parent) {
            return;
        }
        if (names.isEmpty()) {
            return;
        }
        QTreeWidgetItem* sectionParent = parent;
        QTreeWidgetItem* titleRow = new QTreeWidgetItem(parent);
        titleRow->setData(0, kConnPropRowRole, true);
        titleRow->setData(0, kConnPropRowKindRole, 0);
        titleRow->setFlags(titleRow->flags() & ~Qt::ItemIsUserCheckable);
        titleRow->setText(0, title);
        for (int col = 0; col < tree->columnCount(); ++col) {
            titleRow->setBackground(col, QBrush(nameRowBg));
        }
        titleRow->setTextAlignment(0, Qt::AlignLeft | Qt::AlignVCenter);
        if (collapsible) {
            titleRow->setData(0, kConnContentNodeRole, true);
            titleRow->setExpanded(expanded);
            sectionParent = titleRow;
        }
        for (int base = 0; base < names.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem(sectionParent);
            rowNames->setData(0, kConnPropRowRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= names.size()) {
                    break;
                }
                const QString& name = names.at(idx);
                const int col = 4 + off;
                rowNames->setData(col, kConnInlineCellUsedRole, true);
                rowNames->setBackground(col, QBrush(nameRowBg));
                rowNames->setText(col, name);
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                rowNames->setData(col, kConnPropKeyRole, name);
            }
            if (namesOnly) {
                continue;
            }
            auto* rowValues = new QTreeWidgetItem(sectionParent);
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
                rowValues->setData(col, kConnInlineCellUsedRole, true);
                rowValues->setText(col, valuesByName ? valuesByName->value(name) : QString());
                rowValues->setTextAlignment(col, Qt::AlignCenter);
                rowValues->setData(col, kConnPropKeyRole, name);
            }
        }
    };
    auto isLocallyConfiguredGsaSource = [](const QString& source) {
        const QString src = source.trimmed().toLower();
        if (src.isEmpty() || src == QStringLiteral("-")) {
            return false;
        }
        if (src.startsWith(QStringLiteral("inherited")) || src == QStringLiteral("default")) {
            return false;
        }
        return true;
    };
    auto gsaBoolOn = [](const QString& raw) {
        const QString v = raw.trimmed().toLower();
        return v == QStringLiteral("on")
               || v == QStringLiteral("yes")
               || v == QStringLiteral("true")
               || v == QStringLiteral("1");
    };
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
        for (int col = 4; col < tree->columnCount(); ++col) {
            if (QWidget* w = tree->itemWidget(n, col)) {
                tree->removeItemWidget(n, col);
                w->deleteLater();
            }
            n->setText(col, QString());
        }
    };
    for (QTreeWidgetItem* root : roots) {
        if (!root) {
            continue;
        }

        const int connIdx = root->data(0, kConnIdxRole).toInt();
        const QString poolName = root->data(0, kPoolNameRole).toString();
        if (!ensurePoolDetailsLoaded(connIdx, poolName)) {
            continue;
        }
        const PoolDetailsCacheEntry* pit = poolDetailsEntry(connIdx, poolName);
        if (!pit || !pit->loaded) {
            continue;
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
                continue;
            }
            props.push_back(prop);
            values[prop] = value;
        }
        QStringList mainProps = props;
        if (!m_poolInlinePropsOrder.isEmpty()) {
            mainProps = filterPropsByWanted(props, m_poolInlinePropsOrder);
        }

        QMap<QString, QMap<QString, QString>> autoSnapshotPropsByDatasetForPool;
        ensurePoolAutoSnapshotInfoLoaded(connIdx, poolName);
        autoSnapshotPropsByDatasetForPool = poolAutoSnapshotPropsByDataset(connIdx, poolName);
        for (const PendingPropertyDraftEntry& item : pendingConnContentPropertyDraftsFromModel()) {
            if (item.connIdx != connIdx || item.poolName != poolName) {
                continue;
            }
            const QString datasetName = item.objectName.trimmed();
            if (datasetName.isEmpty() || datasetName.contains(QLatin1Char('@'))) {
                continue;
            }
            for (auto vit = item.draft.valuesByProp.cbegin(); vit != item.draft.valuesByProp.cend(); ++vit) {
                if (vit.key().trimmed().startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
                    autoSnapshotPropsByDatasetForPool[datasetName].insert(vit.key().trimmed(), vit.value());
                }
            }
            for (auto iit = item.draft.inheritByProp.cbegin(); iit != item.draft.inheritByProp.cend(); ++iit) {
                if (iit.value() && iit.key().trimmed().startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
                    autoSnapshotPropsByDatasetForPool[datasetName].remove(iit.key().trimmed());
                }
            }
        }
        const QString tt = pit->statusText.toHtmlEscaped();
        root->setToolTip(
            0,
            QStringLiteral("<pre style=\"font-family:'SF Mono','Menlo','Monaco','Consolas','Liberation Mono',monospace; white-space:pre;\">%1</pre>").arg(tt));

        auto blockForKey = [root](const QString& key) -> QTreeWidgetItem* {
            if (!root) {
                return nullptr;
            }
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
        QTreeWidgetItem* infoNode = blockForKey(QString::fromLatin1(kPoolBlockInfoKey));
        if (!infoNode) {
            infoNode = new QTreeWidgetItem();
            infoNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kPoolBlockInfoKey));
            infoNode->setFlags(infoNode->flags() & ~Qt::ItemIsUserCheckable);
            infoNode->setExpanded(true);
            root->insertChild(0, infoNode);
        }
        infoNode->setText(0, trk(QStringLiteral("t_info_lbl_001"),
                                 QStringLiteral("Información del pool"),
                                 QStringLiteral("Information"),
                                 QStringLiteral("信息")));

        addSectionRows(infoNode,
                       trk(QStringLiteral("t_props_lbl_001"),
                           QStringLiteral("Propiedades"),
                           QStringLiteral("Properties"),
                           QStringLiteral("属性")),
                       mainProps,
                       &values,
                       false,
                       true,
                       true);
        for (const InlinePropGroupConfig& cfg : m_poolInlinePropGroups) {
            if (cfg.name.trimmed().isEmpty()
                || cfg.name.trimmed().compare(QStringLiteral("__all__"), Qt::CaseInsensitive) == 0) {
                continue;
            }
            addSectionRows(infoNode,
                           cfg.name,
                           filterPropsByWanted(props, cfg.props),
                           &values,
                           false,
                           true,
                           false);
        }
        addSectionRows(infoNode,
                       trk(QStringLiteral("t_pool_caps_on001"),
                           QStringLiteral("Capacidades activas"),
                           QStringLiteral("Enabled features"),
                           QStringLiteral("已启用能力")),
                       featureEnabled,
                       nullptr,
                       true,
                       true,
                       false);
        addSectionRows(infoNode,
                       trk(QStringLiteral("t_pool_caps_off01"),
                           QStringLiteral("Capacidades deshabilitadas"),
                           QStringLiteral("Disabled features"),
                           QStringLiteral("已禁用能力")),
                       featureDisabled,
                       nullptr,
                       true,
                       true,
                       true);
        QStringList autoSnapshotDatasets;
        for (auto it = autoSnapshotPropsByDatasetForPool.cbegin(); it != autoSnapshotPropsByDatasetForPool.cend(); ++it) {
            const QString enabledKey = findCaseInsensitiveMapKey(it.value(), QStringLiteral("org.fc16.gsa:activado"));
            if (!enabledKey.isEmpty() && gsaBoolOn(it.value().value(enabledKey))) {
                autoSnapshotDatasets.push_back(it.key());
            }
        }
        std::sort(autoSnapshotDatasets.begin(), autoSnapshotDatasets.end(), [](const QString& a, const QString& b) {
            return QString::compare(a, b, Qt::CaseInsensitive) < 0;
        });
        QTreeWidgetItem* autoSnapsNode = nullptr;
        for (int i = 0; i < infoNode->childCount(); ++i) {
            QTreeWidgetItem* child = infoNode->child(i);
            if (child && child->data(0, kConnPoolAutoSnapshotsNodeRole).toBool()) {
                autoSnapsNode = child;
                break;
            }
        }
        if (autoSnapshotDatasets.isEmpty()) {
            if (autoSnapsNode) {
                delete infoNode->takeChild(infoNode->indexOfChild(autoSnapsNode));
            }
        } else {
            if (!autoSnapsNode) {
                autoSnapsNode = new QTreeWidgetItem(infoNode);
                autoSnapsNode->setData(0, kConnPoolAutoSnapshotsNodeRole, true);
                autoSnapsNode->setFlags(autoSnapsNode->flags() & ~Qt::ItemIsUserCheckable);
                autoSnapsNode->setIcon(0, treeStandardIcon(QStyle::SP_BrowserReload));
            } else {
                while (autoSnapsNode->childCount() > 0) {
                    delete autoSnapsNode->takeChild(0);
                }
            }
            autoSnapsNode->setText(0, QStringLiteral("Snapshots automáticos"));
            autoSnapsNode->setExpanded(false);
            for (const QString& datasetName : autoSnapshotDatasets) {
                auto* dsItem = new QTreeWidgetItem(autoSnapsNode);
                dsItem->setText(0, datasetName);
                dsItem->setToolTip(0, datasetName);
                dsItem->setData(0, kConnPoolAutoSnapshotsDatasetRole, datasetName);
                dsItem->setData(0, kConnIdxRole, connIdx);
                dsItem->setData(0, kPoolNameRole, poolName);
                dsItem->setFlags(dsItem->flags() & ~Qt::ItemIsUserCheckable);
                const auto propsMap = autoSnapshotPropsByDatasetForPool.value(datasetName);
                QStringList propNames;
                for (const QString& wantedProp : gsaUserProps()) {
                    const QString existingKey = findCaseInsensitiveMapKey(propsMap, wantedProp);
                    if (!existingKey.isEmpty()) {
                        propNames.push_back(existingKey);
                    }
                }
                for (int base = 0; base < propNames.size(); base += propCols) {
                    auto* rowNames = new QTreeWidgetItem(dsItem);
                    rowNames->setData(0, kConnPropRowRole, true);
                    rowNames->setData(0, kConnPropRowKindRole, 1);
                    rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
                    auto* rowValues = new QTreeWidgetItem(dsItem);
                    rowValues->setData(0, kConnPropRowRole, true);
                    rowValues->setData(0, kConnPropRowKindRole, 2);
                    rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
                    for (int off = 0; off < propCols; ++off) {
                        const int idx = base + off;
                        if (idx >= propNames.size()) {
                            break;
                        }
                        const QString& propName = propNames.at(idx);
                        const int col = 4 + off;
                        rowNames->setData(col, kConnInlineCellUsedRole, true);
                        rowValues->setData(col, kConnInlineCellUsedRole, true);
                        rowNames->setBackground(col, QBrush(nameRowBg));
                        rowNames->setText(col, gsaUserPropertyLabel(propName));
                        rowNames->setTextAlignment(col, Qt::AlignCenter);
                        rowNames->setData(col, kConnPropKeyRole, propName);
                        rowNames->setToolTip(col, propName);
                        rowValues->setText(col, propsMap.value(propName));
                        rowValues->setTextAlignment(col, Qt::AlignCenter);
                        rowValues->setData(col, kConnPropKeyRole, propName);
                        rowValues->setToolTip(col, propsMap.value(propName));
                    }
                }
            }
            root->setExpanded(true);
        }
    }
    resizeTreeColumnsToVisibleContent(tree);
    m_syncingConnContentColumns = false;
}

void MainWindow::syncConnContentPoolColumnsFor(QTreeWidget* tree, const QString& token) {
    syncConnContentPoolColumns(tree, token);
}

void MainWindow::syncConnContentPoolColumns() {
    syncConnContentPoolColumns(m_connContentTree, QString());
}

void MainWindow::saveConnContentTreeState(QTreeWidget* tree, const QString& token) {
    if (token.isEmpty() || !tree) {
        return;
    }
    const QString normalizedToken = normalizedConnContentStateToken(tree, token);
    const QString scopedToken =
        normalizedToken + ((tree == m_bottomConnContentTree) ? QStringLiteral("|bottom")
                                                             : QStringLiteral("|top"));
    ConnContentTreeState st;
    auto poolStateKey = [](QTreeWidgetItem* n) -> QString {
        if (!n || !n->data(0, kIsPoolRootRole).toBool()) {
            return QString();
        }
        return QStringLiteral("%1::%2")
            .arg(n->data(0, kConnIdxRole).toInt())
            .arg(n->data(0, kPoolNameRole).toString().trimmed());
    };
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        if (n->data(0, kIsPoolRootRole).toBool()) {
            st.poolRootExpanded = n->isExpanded();
            const QString key = poolStateKey(n);
            if (!key.isEmpty()) {
                st.poolRootExpandedByPool.insert(key, n->isExpanded());
            }
        }
        if (n->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
            st.infoExpanded = n->isExpanded();
            QTreeWidgetItem* p = n->parent();
            const QString key = poolStateKey(p);
            if (!key.isEmpty()) {
                st.infoExpandedByPool.insert(key, n->isExpanded());
            }
        }
        const QString ds = n->data(0, Qt::UserRole).toString();
        if (!ds.isEmpty()) {
            if (n->isExpanded()) {
                st.expandedDatasets.push_back(ds);
            }
            const QStringList childPaths = collectExpandedConnContentChildPaths(n);
            if (!childPaths.isEmpty()) {
                st.expandedChildPathsByDataset.insert(ds, childPaths);
                appLog(QStringLiteral("DEBUG"),
                       QStringLiteral("saveConnContentTreeState childPaths dataset=%1 paths=%2")
                           .arg(ds, childPaths.join(QStringLiteral(" || "))));
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
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        rec(tree->topLevelItem(i));
    }
    QTreeWidgetItem* selectedItem = tree->currentItem();
    if (!selectedItem) {
        const auto selected = tree->selectedItems();
        if (!selected.isEmpty()) {
            selectedItem = selected.first();
        }
    }
    if (selectedItem) {
        st.selectedDataset = selectedItem->data(0, Qt::UserRole).toString();
        st.selectedSnapshot = selectedItem->data(1, Qt::UserRole).toString();
    }
    ConnectionDatasetTreePane* pane =
        (tree == m_bottomConnContentTree) ? m_bottomDatasetPane : m_topDatasetPane;
    if (pane) {
        const ConnectionDatasetTreePane::VisualState visualState = pane->captureVisualState();
        st.headerState = visualState.headerState;
        st.verticalScrollValue = visualState.verticalScroll;
        st.horizontalScrollValue = visualState.horizontalScroll;
    } else {
        if (tree->verticalScrollBar()) {
            st.verticalScrollValue = tree->verticalScrollBar()->value();
        }
        if (tree->horizontalScrollBar()) {
            st.horizontalScrollValue = tree->horizontalScrollBar()->value();
        }
    }
    if ((!st.selectedDataset.isEmpty() || !st.expandedDatasets.isEmpty()
         || !st.expandedChildPathsByDataset.isEmpty())
        && !st.poolRootExpanded) {
        st.poolRootExpanded = true;
    }
    m_connContentTreeStateByToken[scopedToken] = st;
    const QStringList childPathDatasets = st.expandedChildPathsByDataset.keys();
    const QStringList poolKeys = st.poolRootExpandedByPool.keys();
    appLog(QStringLiteral("DEBUG"),
           QStringLiteral("saveConnContentTreeState token=%1 poolExpanded=%2 infoExpanded=%3 pools=%4 selected=%5 snapshot=%6 expandedDatasets=%7 childPathDatasets=%8 vscroll=%9 hscroll=%10")
               .arg(scopedToken,
                    st.poolRootExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    st.infoExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    poolKeys.join(QStringLiteral(",")),
                    st.selectedDataset,
                    st.selectedSnapshot,
                    st.expandedDatasets.join(QStringLiteral(",")),
                    childPathDatasets.join(QStringLiteral(",")),
                    QString::number(st.verticalScrollValue),
                    QString::number(st.horizontalScrollValue)));
}

void MainWindow::saveConnContentTreeStateFor(QTreeWidget* tree, const QString& token) {
    saveConnContentTreeState(tree, token);
}

void MainWindow::saveConnContentTreeState(const QString& token) {
    saveConnContentTreeState(m_connContentTree, token);
}

void MainWindow::restoreConnContentTreeStateFor(QTreeWidget* tree, const QString& token) {
    restoreConnContentTreeState(tree, token);
}

void MainWindow::rebuildConnContentTreeFor(QTreeWidget* tree,
                                           const QString& token,
                                           int connIdx,
                                           const QString& poolName,
                                           bool restoreState) {
    if (!tree || connIdx < 0 || poolName.isEmpty()) {
        return;
    }
    auto treeMatchesToken = [tree, connIdx, &poolName]() -> bool {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* root = tree->topLevelItem(i);
            if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
                continue;
            }
            const int rootConnIdx = root->data(0, kConnIdxRole).toInt();
            const QString rootPoolName = root->data(0, kPoolNameRole).toString().trimmed();
            if (rootConnIdx == connIdx && rootPoolName == poolName) {
                return true;
            }
        }
        return tree->topLevelItemCount() == 0;
    };
    if (treeMatchesToken()) {
        saveConnContentTreeStateFor(tree, token);
    }
    populateDatasetTree(tree, connIdx, poolName, DatasetTreeContext::ConnectionContent, true);
    if (restoreState) {
        restoreConnContentTreeStateFor(tree, token);
    }
}

QTreeWidgetItem* MainWindow::findConnContentDatasetItemFor(QTreeWidget* tree,
                                                           int connIdx,
                                                           const QString& poolName,
                                                           const QString& datasetName) const {
    return findDatasetItemByIdentity(tree, connIdx, poolName, datasetName);
}

void MainWindow::restoreConnContentTreeState(QTreeWidget* tree, const QString& token) {
    if (token.isEmpty() || !tree) {
        return;
    }
    const QString normalizedToken = normalizedConnContentStateToken(tree, token);
    const QString scopedToken =
        normalizedToken + ((tree == m_bottomConnContentTree) ? QStringLiteral("|bottom")
                                                             : QStringLiteral("|top"));
    const auto it = m_connContentTreeStateByToken.constFind(scopedToken);
    if (it == m_connContentTreeStateByToken.cend()) {
        if (tree == m_bottomConnContentTree) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* top = tree->topLevelItem(i);
                if (top && top->data(0, kIsPoolRootRole).toBool()) {
                    top->setExpanded(true);
                }
            }
        }
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("restoreConnContentTreeState token=%1 no-state").arg(scopedToken));
        return;
    }
    const ConnContentTreeState& st = it.value();
    const QSet<QString> expandedSet(st.expandedDatasets.cbegin(), st.expandedDatasets.cend());
    const QStringList childPathDatasets = st.expandedChildPathsByDataset.keys();
    auto poolStateKey = [](QTreeWidgetItem* n) -> QString {
        if (!n || !n->data(0, kIsPoolRootRole).toBool()) {
            return QString();
        }
        return QStringLiteral("%1::%2")
            .arg(n->data(0, kConnIdxRole).toInt())
            .arg(n->data(0, kPoolNameRole).toString().trimmed());
    };
    appLog(QStringLiteral("DEBUG"),
           QStringLiteral("restoreConnContentTreeState begin token=%1 poolExpanded=%2 infoExpanded=%3 pools=%4 selected=%5 snapshot=%6 expandedDatasets=%7 childPathDatasets=%8 vscroll=%9 hscroll=%10")
               .arg(scopedToken,
                    st.poolRootExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    st.infoExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    st.poolRootExpandedByPool.keys().join(QStringLiteral(",")),
                    st.selectedDataset,
                    st.selectedSnapshot,
                    st.expandedDatasets.join(QStringLiteral(",")),
                    childPathDatasets.join(QStringLiteral(",")),
                    QString::number(st.verticalScrollValue),
                    QString::number(st.horizontalScrollValue)));

    std::function<void(QTreeWidgetItem*)> applyExpand = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString ds = n->data(0, Qt::UserRole).toString();
        if (!ds.isEmpty()) {
            n->setExpanded(expandedSet.contains(ds));
            const auto childIt = st.expandedChildPathsByDataset.constFind(ds);
            if (childIt != st.expandedChildPathsByDataset.cend()) {
                QStringList availablePaths;
                std::function<void(QTreeWidgetItem*)> collectAvailable = [&](QTreeWidgetItem* child) {
                    if (!child) {
                        return;
                    }
                    if (child != n) {
                        const QString path = connContentChildPath(n, child);
                        if (!path.isEmpty()) {
                            availablePaths.push_back(path);
                        }
                    }
                    for (int i = 0; i < child->childCount(); ++i) {
                        collectAvailable(child->child(i));
                    }
                };
                collectAvailable(n);
                appLog(QStringLiteral("DEBUG"),
                       QStringLiteral("restoreConnContentTreeState childPaths dataset=%1 wanted=%2 available=%3")
                           .arg(ds,
                                childIt.value().join(QStringLiteral(" || ")),
                                availablePaths.join(QStringLiteral(" || "))));
                restoreExpandedConnContentChildPaths(n, childIt.value());
            }
        }
        for (int i = 0; i < n->childCount(); ++i) {
            applyExpand(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        applyExpand(tree->topLevelItem(i));
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree->topLevelItem(i);
        if (!top) {
            continue;
        }
        if (top->data(0, kIsPoolRootRole).toBool()) {
            const QString key = poolStateKey(top);
            const bool poolExpanded = key.isEmpty()
                                          ? st.poolRootExpanded
                                          : st.poolRootExpandedByPool.value(key, st.poolRootExpanded);
            top->setExpanded(poolExpanded);
            for (int c = 0; c < top->childCount(); ++c) {
                QTreeWidgetItem* ch = top->child(c);
                if (!ch) {
                    continue;
                }
                if (ch->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                    const bool infoExpanded = key.isEmpty()
                                                  ? st.infoExpanded
                                                  : st.infoExpandedByPool.value(key, st.infoExpanded);
                    ch->setExpanded(infoExpanded);
                    break;
                }
            }
        }
    }

    for (auto sit = st.snapshotByDataset.cbegin(); sit != st.snapshotByDataset.cend(); ++sit) {
        QTreeWidgetItem* item = findDatasetItem(tree, sit.key());
        if (!item) {
            continue;
        }
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(item, 1))) {
            const int idx = cb->findText(sit.value());
            if (idx > 0) {
                const QSignalBlocker blocker(cb);
                cb->setCurrentIndex(idx);
                item->setData(1, Qt::UserRole, sit.value());
                applySnapshotVisualState(item);
            }
        }
    }

    auto applyStoredSnapshotToItem = [this, &st, tree](QTreeWidgetItem* item) {
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
                const QSignalBlocker blocker(cb);
                cb->setCurrentIndex(idx);
                item->setData(1, Qt::UserRole, snapshotName);
                applySnapshotVisualState(item);
                return;
            }
        }
        item->setData(1, Qt::UserRole, snapshotName);
    };

    auto needsExpandedPropertyMaterialization = [](const QStringList& paths) {
        for (const QString& path : paths) {
            if (path.startsWith(QStringLiteral("group|"))
                || path.startsWith(QStringLiteral("gsa|"))
                || path.startsWith(QStringLiteral("holds|"))
                || path.startsWith(QStringLiteral("hold|"))) {
                return true;
            }
        }
        return false;
    };
    for (auto childIt = st.expandedChildPathsByDataset.cbegin(); childIt != st.expandedChildPathsByDataset.cend(); ++childIt) {
        if (!needsExpandedPropertyMaterialization(childIt.value())) {
            continue;
        }
        QTreeWidgetItem* item = findDatasetItem(tree, childIt.key());
        if (!item) {
            continue;
        }
        applyStoredSnapshotToItem(item);
        {
            const QSignalBlocker blocker(tree);
            tree->setCurrentItem(item);
        }
        refreshConnContentPropertiesFor(tree);
        syncConnContentPropertyColumnsFor(tree, token);
    }

    if (!st.selectedDataset.isEmpty()) {
        QTreeWidgetItem* sel = findDatasetItem(tree, st.selectedDataset);
        if (!sel) {
            QString parent = parentDatasetName(st.selectedDataset);
            while (!parent.isEmpty() && !sel) {
                sel = findDatasetItem(tree, parent);
                if (sel) {
                    break;
                }
                parent = parentDatasetName(parent);
            }
        }
        if (sel) {
            if (!st.selectedSnapshot.isEmpty()) {
                if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(sel, 1))) {
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
            const QSignalBlocker blocker(tree);
            tree->setCurrentItem(sel);
        }
    }
    ConnectionDatasetTreePane* pane =
        (tree == m_bottomConnContentTree) ? m_bottomDatasetPane : m_topDatasetPane;
    if (pane) {
        ConnectionDatasetTreePane::VisualState visualState;
        visualState.headerState = st.headerState;
        visualState.verticalScroll = st.verticalScrollValue;
        visualState.horizontalScroll = st.horizontalScrollValue;
        pane->restoreVisualState(visualState);
    } else {
        QPointer<QTreeWidget> safeTree(tree);
        const int vscroll = st.verticalScrollValue;
        const int hscroll = st.horizontalScrollValue;
        QTimer::singleShot(0, tree, [safeTree, vscroll, hscroll]() {
            if (!safeTree) {
                return;
            }
            if (safeTree->verticalScrollBar()) {
                safeTree->verticalScrollBar()->setValue(vscroll);
            }
            if (safeTree->horizontalScrollBar()) {
                safeTree->horizontalScrollBar()->setValue(hscroll);
            }
        });
        QTimer::singleShot(40, tree, [safeTree, vscroll, hscroll]() {
            if (!safeTree) {
                return;
            }
            if (safeTree->verticalScrollBar()) {
                safeTree->verticalScrollBar()->setValue(vscroll);
            }
            if (safeTree->horizontalScrollBar()) {
                safeTree->horizontalScrollBar()->setValue(hscroll);
            }
        });
    }
    QStringList finalPoolStates;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree->topLevelItem(i);
        if (top && top->data(0, kIsPoolRootRole).toBool()) {
            finalPoolStates.push_back(QStringLiteral("%1=%2")
                                          .arg(poolStateKey(top),
                                               top->isExpanded() ? QStringLiteral("1")
                                                                 : QStringLiteral("0")));
        }
    }
    appLog(QStringLiteral("DEBUG"),
           QStringLiteral("restoreConnContentTreeState end token=%1 finalPools=%2 currentDataset=%3")
               .arg(scopedToken,
                    finalPoolStates.join(QStringLiteral(",")),
                    tree->currentItem()
                        ? tree->currentItem()->data(0, Qt::UserRole).toString()
                        : QString()));
}

void MainWindow::restoreConnContentTreeState(const QString& token) {
    restoreConnContentTreeState(m_connContentTree, token);
}

MainWindow::DatasetTreeRenderOptions MainWindow::datasetTreeRenderOptionsForTree(const QTreeWidget* tree,
                                                                                 DatasetTreeContext side) const {
    DatasetTreeRenderOptions options;
    options.includePoolRoot = isConnContentContext(side);
    options.interactiveConnContent = isInteractiveConnContentContext(side);
    options.showInlinePropertyNodes = showInlinePropertyNodesForTree(tree);
    options.showInlinePermissionsNodes = showInlinePermissionsNodesForTree(tree);
    options.showInlineGsaNode = showInlineGsaNodeForTree(tree);
    options.showAutomaticSnapshots = showAutomaticSnapshots();
    return options;
}

void MainWindow::appendDatasetTreeForPool(QTreeWidget* tree,
                                          int connIdx,
                                          const QString& poolName,
                                          DatasetTreeContext side,
                                          const DatasetTreeRenderOptions& options,
                                          bool allowRemoteLoadIfMissing) {
    if (!tree) {
        return;
    }
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
        if (!options.includePoolRoot) {
            return;
        }
        auto* poolRoot = new QTreeWidgetItem();
        poolRoot->setText(0, poolRootTitle());
        poolRoot->setIcon(0, treeStandardIcon(QStyle::SP_DriveHDIcon));
        poolRoot->setFlags(poolRoot->flags() & ~Qt::ItemIsUserCheckable);
        poolRoot->setData(0, kIsPoolRootRole, true);
        poolRoot->setData(0, kConnIdxRole, connIdx);
        poolRoot->setData(0, kPoolNameRole, poolName);
        poolRoot->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        const QString tooltipHtml = cachedPoolStatusTooltipHtml(connIdx, poolName);
        poolRoot->setToolTip(0, tooltipHtml.isEmpty() ? poolRoot->text(0) : tooltipHtml);
        if (showPoolInfoNodeForTree(tree)) {
            auto* infoNode = new QTreeWidgetItem(poolRoot);
            infoNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kPoolBlockInfoKey));
            infoNode->setFlags(infoNode->flags() & ~Qt::ItemIsUserCheckable);
            infoNode->setText(0, trk(QStringLiteral("t_info_lbl_001"),
                                     QStringLiteral("Información del pool"),
                                     QStringLiteral("Pool information"),
                                     QStringLiteral("存储池信息")));
            infoNode->setIcon(0, treeStandardIcon(QStyle::SP_MessageBoxInformation));
            infoNode->setExpanded(true);
        }
        tree->addTopLevelItem(poolRoot);
        poolRoot->setExpanded(true);
    };
    if (!ensureDatasetsLoaded(connIdx, poolName, allowRemoteLoadIfMissing)) {
        addPoolRootOnlyForConnContent();
        return;
    }
    const PoolInfo* poolInfo = findPoolInfo(connIdx, poolName);
    if (!poolInfo) {
        addPoolRootOnlyForConnContent();
        return;
    }
    auto isFilesystemObject = [](const DSInfo& dsInfo) -> bool {
        const QString mp = dsInfo.runtime.properties.value(QStringLiteral("mountpoint")).trimmed();
        const QString cm = dsInfo.runtime.properties.value(QStringLiteral("canmount")).trimmed();
        const QString mounted = dsInfo.runtime.properties.value(QStringLiteral("mounted")).trimmed();
        return (mp != QStringLiteral("-") && !mp.isEmpty())
               || (cm != QStringLiteral("-") && !cm.isEmpty())
               || (mounted != QStringLiteral("-") && !mounted.isEmpty());
    };
    auto buildDatasetItem = [&](const DSInfo& dsInfo, auto&& buildDatasetItemRef) -> QTreeWidgetItem* {
        auto* item = new QTreeWidgetItem();
        const QString fullName = dsInfo.key.fullName.trimmed();
        const QString displayName = fullName.contains('/')
                                        ? fullName.section('/', -1, -1)
                                        : fullName;
        item->setText(0, displayName);
        item->setIcon(0, treeStandardIcon(QStyle::SP_DirIcon));
        {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        QStringList snaps = dsInfo.runtime.directSnapshots;
        if (!options.showAutomaticSnapshots) {
            QStringList filtered;
            for (const QString& snapName : snaps) {
                if (!isAutomaticGsaSnapshotName(snapName)) {
                    filtered.push_back(snapName);
                }
            }
            snaps = filtered;
        }
        item->setText(1, snaps.isEmpty() ? QString() : QStringLiteral("(ninguno)"));
        item->setData(1, Qt::UserRole, QString());
        item->setData(0, Qt::UserRole, fullName);
        item->setData(1, kSnapshotListRole, snaps);
        item->setData(0, kConnIdxRole, connIdx);
        item->setData(0, kPoolNameRole, poolName);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const QString mounted = dsInfo.runtime.properties.value(QStringLiteral("mounted")).trimmed().toLower();
        const bool isMounted = (mounted == QStringLiteral("yes")
                                || mounted == QStringLiteral("on")
                                || mounted == QStringLiteral("true")
                                || mounted == QStringLiteral("1"));
        item->setCheckState(2, isMounted ? Qt::Checked : Qt::Unchecked);
        const QString mountpoint = dsInfo.runtime.properties.value(QStringLiteral("mountpoint")).trimmed();
        const QString effectiveMp = effectiveMountPath(connIdx, poolName, fullName, mountpoint, mounted);
        item->setText(3, effectiveMp.isEmpty() ? mountpoint : effectiveMp);
        if (options.showInlinePropertyNodes) {
            auto* propsNode = new QTreeWidgetItem(item);
            propsNode->setText(0, trk(QStringLiteral("t_props_lbl_001"),
                                      QStringLiteral("Propiedades"),
                                      QStringLiteral("Properties"),
                                      QStringLiteral("属性")));
            propsNode->setIcon(0, treeStandardIcon(QStyle::SP_FileDialogDetailedView));
            propsNode->setData(0, kConnPropGroupNodeRole, true);
            propsNode->setData(0, kConnPropGroupNameRole, QString());
            propsNode->setData(0, kConnIdxRole, connIdx);
            propsNode->setData(0, kPoolNameRole, poolName);
            propsNode->setFlags(propsNode->flags() & ~Qt::ItemIsUserCheckable);
        }
        if (options.showInlinePermissionsNodes) {
            auto* permissionsNode = new QTreeWidgetItem(item);
            permissionsNode->setText(0, QStringLiteral("Permisos"));
            permissionsNode->setIcon(0, treeStandardIcon(QStyle::SP_DialogYesButton));
            permissionsNode->setData(0, kConnPermissionsNodeRole, true);
            permissionsNode->setData(0, kConnPermissionsKindRole, QStringLiteral("root"));
            permissionsNode->setData(0, kConnIdxRole, connIdx);
            permissionsNode->setData(0, kPoolNameRole, poolName);
            permissionsNode->setFlags(permissionsNode->flags() & ~Qt::ItemIsUserCheckable);
            permissionsNode->setExpanded(false);
        }
        if (isFilesystemObject(dsInfo) && options.showInlineGsaNode) {
            auto* gsaNode = new QTreeWidgetItem(item);
            gsaNode->setText(0, QStringLiteral("Programar snapshots"));
            gsaNode->setIcon(0, treeStandardIcon(QStyle::SP_BrowserReload));
            gsaNode->setData(0, kConnGsaNodeRole, true);
            gsaNode->setData(0, kConnPropGroupNodeRole, true);
            gsaNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kGsaBlockInfoKey));
            gsaNode->setData(0, kConnIdxRole, connIdx);
            gsaNode->setData(0, kPoolNameRole, poolName);
            gsaNode->setFlags(gsaNode->flags() & ~Qt::ItemIsUserCheckable);
            gsaNode->setExpanded(false);
        }
        for (const QString& childName : dsInfo.childFullNames) {
            const auto childIt = poolInfo->objectsByFullName.constFind(childName);
            if (childIt == poolInfo->objectsByFullName.cend() || childIt->kind == DSKind::Snapshot) {
                continue;
            }
            item->addChild(buildDatasetItemRef(childIt.value(), buildDatasetItemRef));
        }
        return item;
    };

    QVector<QTreeWidgetItem*> logicalTopLevelItems;
    for (const QString& rootName : poolInfo->rootObjectNames) {
        const auto it = poolInfo->objectsByFullName.constFind(rootName);
        if (it == poolInfo->objectsByFullName.cend() || it->kind == DSKind::Snapshot) {
            continue;
        }
        logicalTopLevelItems.push_back(buildDatasetItem(it.value(), buildDatasetItem));
    }
    if (options.includePoolRoot) {
        auto* poolRoot = new QTreeWidgetItem();
        poolRoot->setText(0, poolRootTitle());
        poolRoot->setIcon(0, treeStandardIcon(QStyle::SP_DriveHDIcon));
        poolRoot->setFlags(poolRoot->flags() & ~Qt::ItemIsUserCheckable);
        poolRoot->setData(0, kIsPoolRootRole, true);
        poolRoot->setData(0, kConnIdxRole, connIdx);
        poolRoot->setData(0, kPoolNameRole, poolName);
        poolRoot->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        const QString tooltipHtml = cachedPoolStatusTooltipHtml(connIdx, poolName);
        poolRoot->setToolTip(0, tooltipHtml.isEmpty() ? poolRoot->text(0) : tooltipHtml);
        if (showPoolInfoNodeForTree(tree)) {
            auto* infoNode = new QTreeWidgetItem(poolRoot);
            infoNode->setData(0, kConnPropKeyRole, QString::fromLatin1(kPoolBlockInfoKey));
            infoNode->setFlags(infoNode->flags() & ~Qt::ItemIsUserCheckable);
            infoNode->setText(0, trk(QStringLiteral("t_info_lbl_001"),
                                     QStringLiteral("Información del pool"),
                                     QStringLiteral("Pool information"),
                                     QStringLiteral("存储池信息")));
            infoNode->setIcon(0, treeStandardIcon(QStyle::SP_MessageBoxInformation));
        }
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
}

void MainWindow::attachDatasetTreeSnapshotCombos(QTreeWidget* tree, DatasetTreeContext side) {
    if (!tree) {
        return;
    }
    std::function<void(QTreeWidgetItem*)> attachCombos = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        if (n->data(0, kIsPoolRootRole).toBool()
            || n->data(0, kConnPropRowRole).toBool()
            || n->data(0, kConnContentNodeRole).toBool()) {
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
}

void MainWindow::populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, DatasetTreeContext side, bool allowRemoteLoadIfMissing) {
    if (!tree) {
        return;
    }
    beginUiBusy();
    m_loadingDatasetTrees = true;
    tree->clear();
    const DatasetTreeRenderOptions options = datasetTreeRenderOptionsForTree(tree, side);
    appendDatasetTreeForPool(tree, connIdx, poolName, side, options, allowRemoteLoadIfMissing);
    tree->expandToDepth(0);
    attachDatasetTreeSnapshotCombos(tree, side);
    refreshDatasetExpansionIndicators(tree);

    if (options.interactiveConnContent) {
        syncConnContentPropertyColumns();
        const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
        restoreConnContentTreeState(tree, token);
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

void MainWindow::onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, DatasetTreeContext side, const QString& chosen) {
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
    if (side == DatasetTreeContext::ConnectionContent) {
        const bool changedSelection = (tree->currentItem() != item);
        if (changedSelection) {
            tree->setCurrentItem(item);
        } else {
            refreshConnContentPropertiesFor(tree);
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
    setSelectedDataset(selectionSideString(side), ds, snap);
}

void MainWindow::onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, DatasetTreeContext side) {
    if (!tree || !item || m_loadingDatasetTrees || actionsLocked()) {
        return;
    }
    if (side == DatasetTreeContext::ConnectionContent && col >= 4) {
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
        const QString token = QStringLiteral("%1::%2")
                                  .arg(owner->data(0, kConnIdxRole).toInt())
                                  .arg(owner->data(0, kPoolNameRole).toString());
        const QString objectName = snap.isEmpty() ? ds : QStringLiteral("%1@%2").arg(ds, snap);
        const QString value = item->text(col);

        if (m_connContentPropsTable
            && m_propsToken.trimmed() == token.trimmed()
            && m_propsDataset.trimmed() == objectName) {
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
            QStringLiteral("%1|%2").arg(token.trimmed(),
                                        objectName.trimmed()));
        vals[prop] = value;
        updateConnContentPropertyValues(token, objectName, vals);
        updateConnContentDraftValue(token, objectName, prop, value);
        updateApplyPropsButtonState();
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
    if (side == DatasetTreeContext::Origin) {
        if (m_connActionOrigin.valid) {
            token = QStringLiteral("%1::%2").arg(m_connActionOrigin.connIdx).arg(m_connActionOrigin.poolName);
        }
    } else if (side == DatasetTreeContext::Destination) {
        if (m_connActionDest.valid) {
            token = QStringLiteral("%1::%2").arg(m_connActionDest.connIdx).arg(m_connActionDest.poolName);
        }
    } else if (side == DatasetTreeContext::ConnectionContent) {
        const int itemConnIdx = item->data(0, kConnIdxRole).toInt();
        const QString itemPool = item->data(0, kPoolNameRole).toString();
        if (itemConnIdx >= 0 && !itemPool.isEmpty()) {
            token = QStringLiteral("%1::%2").arg(itemConnIdx).arg(itemPool);
        } else {
            token = connContentTokenForTree(tree);
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

    if (side == DatasetTreeContext::ConnectionContent) {
        const bool willBeMounted = (desired == Qt::Checked);
        const QString objectName = ctx.snapshotName.isEmpty()
                                       ? ctx.datasetName
                                       : QStringLiteral("%1@%2").arg(ctx.datasetName, ctx.snapshotName);
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
        if (m_connContentPropsTable
            && m_propsToken.trimmed() == token.trimmed()
            && m_propsDataset.trimmed() == objectName) {
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
        QMap<QString, QString> vals = m_connContentPropValuesByObject.value(
            QStringLiteral("%1|%2").arg(token.trimmed(),
                                        objectName.trimmed()));
        vals[QStringLiteral("mounted")] = willBeMounted ? QStringLiteral("yes") : QStringLiteral("no");
        updateConnContentPropertyValues(token, objectName, vals);
        updateConnContentDraftValue(token,
                                    objectName,
                                    QStringLiteral("mounted"),
                                    willBeMounted ? QStringLiteral("yes") : QStringLiteral("no"));
        updateApplyPropsButtonState();
        updateConnectionActionsState();
        return;
    }

    if (side == DatasetTreeContext::Origin || side == DatasetTreeContext::Destination) {
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

        if (m_connContentPropsTable
            && m_propsToken.trimmed() == token.trimmed()
            && m_propsDataset.trimmed() == ctx.datasetName.trimmed()) {
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
                    onDatasetPropsCellChanged(r, 1);
                }
                break;
            }
        }
        updateConnContentDraftValue(token,
                                    ctx.datasetName,
                                    QStringLiteral("mounted"),
                                    willBeMounted ? QStringLiteral("yes") : QStringLiteral("no"));
        updateApplyPropsButtonState();
        updateConnectionActionsState();
        return;
    }

    bool ok = false;
    m_loadingDatasetTrees = true;
    if (desired == Qt::Checked) {
        ok = mountDataset(selectionSideString(side), ctx);
    } else {
        ok = umountDataset(selectionSideString(side), ctx);
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
    if (side == DatasetTreeContext::ConnectionContent) {
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
