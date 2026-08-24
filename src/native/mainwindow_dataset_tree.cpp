#include "mainwindow.h"

#include "zfsprops.h"
#include "commands/gsa.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "i18nmanager.h"
#include "agentversion.h"

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
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QJsonDocument>
#include <QJsonObject>
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
constexpr int kIsConnectionRootRole = Qt::UserRole + 36;
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
constexpr int kConnPermissionsScopeRole = Qt::UserRole + 27;
constexpr int kConnPermissionsTargetTypeRole = Qt::UserRole + 28;
constexpr int kConnPermissionsTargetNameRole = Qt::UserRole + 29;
constexpr int kConnPermissionsEntryNameRole = Qt::UserRole + 30;

constexpr int kConnPoolAutoSnapshotsNodeRole = Qt::UserRole + 34;
constexpr int kConnPoolAutoSnapshotsDatasetRole = Qt::UserRole + 35;
constexpr int kConnRootSectionRole = Qt::UserRole + 37;
constexpr int kConnRootInlineFieldRole = Qt::UserRole + 38;
constexpr int kConnRootInlineEditableRole = Qt::UserRole + 39;
constexpr int kConnRootInlineRawValueRole = Qt::UserRole + 40;
constexpr int kConnSnapshotsNodeRole = Qt::UserRole + 41;
constexpr int kConnSnapshotGroupNodeRole = Qt::UserRole + 42;
constexpr int kConnSnapshotItemRole = Qt::UserRole + 43;
constexpr int kConnStatePartRole = Qt::UserRole + 44;
constexpr int kConnConnectionStableIdRole = Qt::UserRole + 45;
constexpr int kConnPoolGuidRole = Qt::UserRole + 46;
constexpr int kConnDatasetGuidRole = Qt::UserRole + 47;
constexpr int kConnSnapshotGuidRole = Qt::UserRole + 48;
constexpr int kConnSnapshotGroupIdRole = Qt::UserRole + 49;
constexpr int kIsSplitRootRole = Qt::UserRole + 50;
constexpr int kConnDebugBaseTextRole = Qt::UserRole + 51;
constexpr int kConnDebugLastIdRole = Qt::UserRole + 52;
constexpr int kConnFileBrowserNodeRole = Qt::UserRole + 53;
constexpr int kConnFileBrowserPathRole = Qt::UserRole + 54;
constexpr int kConnFileBrowserIsDirRole = Qt::UserRole + 55;
constexpr int kConnFileBrowserLoadedRole = Qt::UserRole + 56;
constexpr char kPoolBlockInfoKey[] = "__pool_block_info__";


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

// La familia de plataforma, el soporte por plataforma y la editabilidad viven ahora en
// `base/zfsprops`, sin Qt. Estaban DUPLICADAS letra por letra en este fichero y en el
// otro, y el servidor web necesita la misma regla para saber qué celda pinta con una caja
// de edición. Esto es solo el puente entre los QString de aquí y las cadenas de allí.
using DatasetPlatformFamily = zfsmgr::base::zfsprops::Platform;

DatasetPlatformFamily datasetPlatformFamilyFromStrings(const QString& osType, const QString& osLine) {
    return zfsmgr::base::zfsprops::platformOf(osType.toStdString(), osLine.toStdString());
}

bool isDatasetPropertySupportedOnPlatform(const QString& propName, DatasetPlatformFamily platform) {
    return zfsmgr::base::zfsprops::isSupportedOn(propName.toStdString(), platform);
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

QString gsaUserPropertyLabel(const QString& prop, const QString& language) {
    const QString p = prop.trimmed();
    if (p.compare(QStringLiteral("org.fc16.gsa:activado"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_enabled_001"),
                                                    QStringLiteral("Activado"), QStringLiteral("Enabled"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:recursivo"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_recursive_001"),
                                                    QStringLiteral("Recursivo"), QStringLiteral("Recursive"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:horario"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_hourly_001"),
                                                    QStringLiteral("Horario"), QStringLiteral("Hourly"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:diario"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_daily_001"),
                                                    QStringLiteral("Diario"), QStringLiteral("Daily"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:semanal"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_weekly_001"),
                                                    QStringLiteral("Semanal"), QStringLiteral("Weekly"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:mensual"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_monthly_001"),
                                                    QStringLiteral("Mensual"), QStringLiteral("Monthly"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:anual"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_yearly_001"),
                                                    QStringLiteral("Anual"), QStringLiteral("Yearly"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:nivelar"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_level_001"),
                                                    QStringLiteral("Nivelar"), QStringLiteral("Level"));
    }
    if (p.compare(QStringLiteral("org.fc16.gsa:destino"), Qt::CaseInsensitive) == 0) {
        return I18nManager::instance().translateKey(language, QStringLiteral("t_gsa_prop_target_001"),
                                                    QStringLiteral("Destino"), QStringLiteral("Target"));
    }
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



struct PoolDeviceStatusNode {
    QString name;
    QString state;
    QVector<PoolDeviceStatusNode> children;
};

bool poolDeviceStateToken(const QString& raw) {
    const QString s = raw.trimmed().toUpper();
    static const QSet<QString> kStates = {
        QStringLiteral("ONLINE"),
        QStringLiteral("OFFLINE"),
        QStringLiteral("UNAVAIL"),
        QStringLiteral("UNAVAILABLE"),
        QStringLiteral("DEGRADED"),
        QStringLiteral("FAULTED"),
        QStringLiteral("REMOVED"),
        QStringLiteral("AVAIL")
    };
    return kStates.contains(s);
}


QVector<PoolDeviceStatusNode> parsePoolDeviceHierarchyFromStatus(const QString& poolName,
                                                                 const QString& statusPText,
                                                                 const QString& fallbackStatusText) {
    const QString text = !statusPText.trimmed().isEmpty() ? statusPText : fallbackStatusText;
    if (text.trimmed().isEmpty()) {
        return {};
    }
    const QStringList lines = text.split('\n');
    int configLine = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed().startsWith(QStringLiteral("config:"), Qt::CaseInsensitive)) {
            configLine = i;
            break;
        }
    }
    if (configLine < 0) {
        return {};
    }

    struct ParsedLine {
        int indent{0};
        QString name;
        QString state;
    };
    QVector<ParsedLine> parsed;
    bool inConfig = false;
    for (int i = configLine + 1; i < lines.size(); ++i) {
        const QString raw = lines.at(i);
        const QString trimmed = raw.trimmed();
        if (!inConfig) {
            if (trimmed.isEmpty()) {
                continue;
            }
            if (trimmed.startsWith(QStringLiteral("NAME"), Qt::CaseInsensitive)) {
                inConfig = true;
            }
            continue;
        }
        if (trimmed.isEmpty()) {
            break;
        }
        const QString lower = trimmed.toLower();
        if (lower.startsWith(QStringLiteral("errors:"))
            || lower.startsWith(QStringLiteral("scan:"))
            || lower.startsWith(QStringLiteral("state:"))
            || lower.startsWith(QStringLiteral("action:"))
            || lower.startsWith(QStringLiteral("status:"))
            || lower.startsWith(QStringLiteral("see:"))
            || lower.startsWith(QStringLiteral("pool:"))) {
            break;
        }
        const int indent = raw.size() - raw.trimmed().size();
        const QStringList toks = trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (toks.isEmpty()) {
            continue;
        }
        ParsedLine pl;
        pl.indent = indent;
        pl.name = toks.first().trimmed();
        if (toks.size() >= 2 && poolDeviceStateToken(toks.at(1))) {
            pl.state = toks.at(1).trimmed().toUpper();
        }
        parsed.push_back(pl);
    }
    if (parsed.isEmpty()) {
        return {};
    }

    QVector<PoolDeviceStatusNode> top;
    QVector<QPair<int, PoolDeviceStatusNode*>> stack;
    for (const ParsedLine& pl : parsed) {
        PoolDeviceStatusNode node;
        node.name = pl.name;
        node.state = pl.state;
        while (!stack.isEmpty() && pl.indent <= stack.last().first) {
            stack.removeLast();
        }
        if (stack.isEmpty()) {
            top.push_back(node);
            stack.push_back({pl.indent, &top.last()});
        } else if (PoolDeviceStatusNode* parent = stack.last().second) {
            parent->children.push_back(node);
            stack.push_back({pl.indent, &parent->children.last()});
        }
    }
    if (!top.isEmpty()) {
        const QString poolLower = poolName.trimmed().toLower();
        if (!poolLower.isEmpty() && top.first().name.trimmed().toLower() == poolLower) {
            return top.first().children;
        }
    }
    return top;
}


QIcon snapshotsNodeIcon() {
    const QIcon themed = QIcon::fromTheme(QStringLiteral("camera-photo"));
    if (!themed.isNull()) {
        return themed;
    }
    QStyle* style = QApplication::style();
    return style ? style->standardIcon(QStyle::SP_FileDialogContentsView) : QIcon();
}

QTreeWidgetItem* findSnapshotItemInDatasetNode(QTreeWidgetItem* datasetNode, const QString& snapshotName) {
    if (!datasetNode || snapshotName.trimmed().isEmpty()) {
        return nullptr;
    }
    const QString wanted = snapshotName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
        if (!n) {
            return nullptr;
        }
        if (n->data(0, kConnSnapshotItemRole).toBool()
            && n->data(1, Qt::UserRole).toString().trimmed() == wanted) {
            return n;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(n->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    return rec(datasetNode);
}

QTreeWidgetItem* findSnapshotItemInDatasetNodeByGuid(QTreeWidgetItem* datasetNode, const QString& snapshotGuid) {
    if (!datasetNode || snapshotGuid.trimmed().isEmpty()) {
        return nullptr;
    }
    const QString wanted = snapshotGuid.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
        if (!n) {
            return nullptr;
        }
        if (n->data(0, kConnSnapshotItemRole).toBool()
            && n->data(0, kConnSnapshotGuidRole).toString().trimmed() == wanted) {
            return n;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(n->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    return rec(datasetNode);
}

bool gsaBoolOn(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    return normalized == QStringLiteral("on")
           || normalized == QStringLiteral("yes")
           || normalized == QStringLiteral("true")
           || normalized == QStringLiteral("1");
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





QString escapeConnStatePart(QString value) {
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral(";"), QStringLiteral("\\;"));
    return value;
}

QString unescapeConnStatePart(const QString& value) {
    QString out;
    out.reserve(value.size());
    bool esc = false;
    for (const QChar ch : value) {
        if (esc) {
            out.push_back(ch);
            esc = false;
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            esc = true;
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

QStringList splitEscapedConnStatePath(const QString& rawPath) {
    QStringList out;
    QString cur;
    bool esc = false;
    for (const QChar ch : rawPath) {
        if (esc) {
            cur.push_back(ch);
            esc = false;
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            esc = true;
            continue;
        }
        if (ch == QLatin1Char(';')) {
            out.push_back(unescapeConnStatePart(cur));
            cur.clear();
            continue;
        }
        cur.push_back(ch);
    }
    out.push_back(unescapeConnStatePart(cur));
    return out;
}

QString connectionStableIdFromNode(QTreeWidgetItem* node) {
    for (QTreeWidgetItem* cur = node; cur; cur = cur->parent()) {
        const QString stable = cur->data(0, kConnConnectionStableIdRole).toString().trimmed();
        if (!stable.isEmpty()) {
            return stable;
        }
    }
    return QString();
}

QString connContentNodeLocalStableId(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    const QString explicitPart = node->data(0, kConnStatePartRole).toString().trimmed();
    if (!explicitPart.isEmpty()) {
        return explicitPart;
    }
    if (node->data(0, kIsConnectionRootRole).toBool()) {
        QString connId = node->data(0, kConnConnectionStableIdRole).toString().trimmed();
        if (connId.isEmpty()) {
            connId = QStringLiteral("conn-%1").arg(node->data(0, kConnIdxRole).toInt());
        }
        return QStringLiteral("conn:%1").arg(connId);
    }
    if (node->data(0, kIsPoolRootRole).toBool()) {
        QString poolId = node->data(0, kConnPoolGuidRole).toString().trimmed();
        if (!poolId.isEmpty()) {
            return QStringLiteral("pool:%1").arg(poolId);
        }
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("pooltmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("pooltmp:0");
    }
    if (node->data(0, kConnSnapshotsNodeRole).toBool()) {
        return QStringLiteral("syn:snapshots_root");
    }
    if (node->data(0, kConnSnapshotGroupNodeRole).toBool()) {
        QString groupId = node->data(0, kConnSnapshotGroupIdRole).toString().trimmed().toLower();
        if (!groupId.isEmpty()) {
            return QStringLiteral("syn:snapshot_group:%1").arg(groupId);
        }
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("syn:snapshot_group_tmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("syn:snapshot_group_tmp:0");
    }
    if (node->data(0, kConnSnapshotItemRole).toBool()) {
        QString snapId = node->data(0, kConnSnapshotGuidRole).toString().trimmed();
        if (!snapId.isEmpty()) {
            return QStringLiteral("snap:%1").arg(snapId);
        }
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("snaptmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("snaptmp:0");
    }
    if (node->data(0, kConnPoolAutoSnapshotsNodeRole).toBool()) {
        return QStringLiteral("syn:scheduled_datasets_root");
    }
    const QString scheduledDataset = node->data(0, kConnPoolAutoSnapshotsDatasetRole).toString().trimmed();
    if (!scheduledDataset.isEmpty()) {
        QString dsId = node->data(0, kConnDatasetGuidRole).toString().trimmed();
        if (!dsId.isEmpty()) {
            return QStringLiteral("ds:%1").arg(dsId);
        }
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("dstmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("dstmp:0");
    }
    if (node->data(0, kConnSnapshotHoldsNodeRole).toBool()) {
        return QStringLiteral("syn:snapshot_holds");
    }
    if (node->data(0, kConnSnapshotHoldItemRole).toBool()) {
        return QStringLiteral("hold:%1").arg(node->data(0, kConnSnapshotHoldTagRole).toString().trimmed());
    }
    if (node->data(0, kConnPermissionsNodeRole).toBool()) {
        const QString kind = node->data(0, kConnPermissionsKindRole).toString();
        const QString entry = node->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
        if (kind == QStringLiteral("grant") || kind == QStringLiteral("grant_perm")) {
            return QStringLiteral("perm:%1:%2:%3:%4")
                .arg(kind,
                     node->data(0, kConnPermissionsScopeRole).toString(),
                     node->data(0, kConnPermissionsTargetTypeRole).toString(),
                     node->data(0, kConnPermissionsTargetNameRole).toString());
        }
        if (kind == QStringLiteral("set") || kind == QStringLiteral("set_perm")
            || kind == QStringLiteral("create_perm")) {
            return QStringLiteral("perm:%1:%2").arg(kind, entry);
        }
        return QStringLiteral("perm:%1").arg(kind);
    }
    const QString section = node->data(0, kConnRootSectionRole).toString().trimmed();
    if (!section.isEmpty()) {
        return QStringLiteral("syn:conn_section:%1").arg(section);
    }
    if (node->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
        return QStringLiteral("syn:pool_information");
    }
    if (node->data(0, kConnPropGroupNodeRole).toBool()) {
        const QString groupName = node->data(0, kConnPropGroupNameRole).toString().trimmed();
        if (groupName.isEmpty()) {
            const bool isSnapshotProps = node->parent()
                                         && node->parent()->data(0, kConnSnapshotItemRole).toBool();
            return isSnapshotProps ? QStringLiteral("syn:snap_prop")
                                   : QStringLiteral("syn:ds_prop");
        }
        return QStringLiteral("syn:prop_group:%1").arg(groupName.toLower());
    }
    const QString ds = node->data(0, Qt::UserRole).toString().trimmed();
    if (!ds.isEmpty()) {
        QString dsId = node->data(0, kConnDatasetGuidRole).toString().trimmed();
        if (!dsId.isEmpty()) {
            return QStringLiteral("ds:%1").arg(dsId);
        }
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("dstmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("dstmp:0");
    }
    if (QTreeWidgetItem* parent = node->parent()) {
        return QStringLiteral("idx:%1").arg(parent->indexOfChild(node));
    }
    return QStringLiteral("idx:0");
}

QString connContentNodeStablePath(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    QStringList parts;
    bool hasConnRoot = false;
    for (QTreeWidgetItem* cur = node; cur; cur = cur->parent()) {
        const QString localId = connContentNodeLocalStableId(cur).trimmed();
        if (!localId.isEmpty()) {
            parts.prepend(escapeConnStatePart(localId));
        }
        if (cur->data(0, kIsConnectionRootRole).toBool()) {
            hasConnRoot = true;
            break;
        }
    }
    if (!hasConnRoot) {
        const QString connId = connectionStableIdFromNode(node);
        if (!connId.isEmpty()) {
            parts.prepend(escapeConnStatePart(QStringLiteral("conn:%1").arg(connId)));
        }
    }
    return parts.join(QStringLiteral(";"));
}

QString connContentNodeLocalTemporaryId(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    if (node->data(0, kIsConnectionRootRole).toBool()) {
        QString connId = node->data(0, kConnConnectionStableIdRole).toString().trimmed();
        if (connId.isEmpty()) {
            connId = QStringLiteral("conn-%1").arg(node->data(0, kConnIdxRole).toInt());
        }
        return QStringLiteral("conn:%1").arg(connId);
    }
    if (node->data(0, kIsPoolRootRole).toBool()) {
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("pooltmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("pooltmp:0");
    }
    if (node->data(0, kConnSnapshotItemRole).toBool()) {
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("snaptmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("snaptmp:0");
    }
    const QString ds = node->data(0, Qt::UserRole).toString().trimmed();
    if (!ds.isEmpty()) {
        if (QTreeWidgetItem* parent = node->parent()) {
            return QStringLiteral("dstmp:%1").arg(parent->indexOfChild(node));
        }
        return QStringLiteral("dstmp:0");
    }
    return connContentNodeLocalStableId(node);
}

QString connContentNodeTemporaryPath(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    QStringList parts;
    bool hasConnRoot = false;
    for (QTreeWidgetItem* cur = node; cur; cur = cur->parent()) {
        const QString localId = connContentNodeLocalTemporaryId(cur).trimmed();
        if (!localId.isEmpty()) {
            parts.prepend(escapeConnStatePart(localId));
        }
        if (cur->data(0, kIsConnectionRootRole).toBool()) {
            hasConnRoot = true;
            break;
        }
    }
    if (!hasConnRoot) {
        const QString connId = connectionStableIdFromNode(node);
        if (!connId.isEmpty()) {
            parts.prepend(escapeConnStatePart(QStringLiteral("conn:%1").arg(connId)));
        }
    }
    return parts.join(QStringLiteral(";"));
}

QString connContentChildStableId(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    const QString explicitPart = node->data(0, kConnStatePartRole).toString().trimmed();
    if (!explicitPart.isEmpty()) {
        return explicitPart;
    }
    if (node->data(0, kConnPermissionsNodeRole).toBool()) {
        const QString kind = node->data(0, kConnPermissionsKindRole).toString();
        const QString entry = node->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
        if (kind == QStringLiteral("grant") || kind == QStringLiteral("grant_perm")) {
            return QStringLiteral("perm|%1|%2|%3|%4")
                .arg(kind,
                     node->data(0, kConnPermissionsScopeRole).toString(),
                     node->data(0, kConnPermissionsTargetTypeRole).toString(),
                     node->data(0, kConnPermissionsTargetNameRole).toString());
        }
        if (kind == QStringLiteral("set") || kind == QStringLiteral("set_perm")
            || kind == QStringLiteral("create_perm")) {
            return QStringLiteral("perm|%1|%2").arg(kind, entry);
        }
        if (!kind.isEmpty()) {
            return QStringLiteral("perm|%1").arg(kind);
        }
    }
    if (node->data(0, kConnPropGroupNodeRole).toBool()) {
        const QString groupName = node->data(0, kConnPropGroupNameRole).toString().trimmed();
        if (groupName.isEmpty()) {
            const bool isSnapshotProps = node->parent()
                                         && node->parent()->data(0, kConnSnapshotItemRole).toBool();
            return isSnapshotProps ? QStringLiteral("syn:snap_prop")
                                   : QStringLiteral("syn:ds_prop");
        }
        return QStringLiteral("syn:prop_group:%1").arg(groupName.toLower());
    }
    if (node->data(0, kConnSnapshotHoldsNodeRole).toBool()) {
        return QStringLiteral("syn:snapshot_holds");
    }
    if (node->data(0, kConnSnapshotHoldItemRole).toBool()) {
        return QStringLiteral("hold|%1").arg(node->data(0, kConnSnapshotHoldTagRole).toString().trimmed());
    }
    if (node->data(0, kConnSnapshotsNodeRole).toBool()) {
        return QStringLiteral("syn:snapshots_root");
    }
    if (node->data(0, kConnSnapshotGroupNodeRole).toBool()) {
        const QString groupId = node->data(0, kConnSnapshotGroupIdRole).toString().trimmed().toLower();
        return groupId.isEmpty() ? QString() : QStringLiteral("syn:snapshot_group:%1").arg(groupId);
    }
    if (node->data(0, kConnSnapshotItemRole).toBool()) {
        const QString snapGuid = node->data(0, kConnSnapshotGuidRole).toString().trimmed();
        return snapGuid.isEmpty() ? QString() : QStringLiteral("snap:%1").arg(snapGuid);
    }
    const QString ds = node->data(0, Qt::UserRole).toString().trimmed();
    if (!ds.isEmpty()) {
        const QString dsGuid = node->data(0, kConnDatasetGuidRole).toString().trimmed();
        if (!dsGuid.isEmpty()) {
            return QStringLiteral("ds:%1").arg(dsGuid);
        }
        return QString();
    }
    if (QTreeWidgetItem* parent = node->parent()) {
        return QStringLiteral("idx:%1").arg(parent->indexOfChild(node));
    }
    return QStringLiteral("idx:0");
}

QString normalizeConnContentChildPathForCompat(const QString& path) {
    QString normalized = path.trimmed();
    normalized.replace(QStringLiteral("group||mainprops"), QStringLiteral("syn:ds_prop"));
    normalized.replace(QStringLiteral("group|||Propiedades"), QStringLiteral("group||mainprops"));
    normalized.replace(QStringLiteral("group|||Properties"), QStringLiteral("group||mainprops"));
    normalized.replace(QStringLiteral("group|||Dataset properties"), QStringLiteral("group||mainprops"));
    normalized.replace(QStringLiteral("group|||Snapshot properties"), QStringLiteral("group||mainprops"));
    normalized.replace(QStringLiteral("group|||Propiedades"), QStringLiteral("syn:ds_prop"));
    normalized.replace(QStringLiteral("group|||Properties"), QStringLiteral("syn:ds_prop"));
    normalized.replace(QStringLiteral("group|||Dataset properties"), QStringLiteral("syn:ds_prop"));
    normalized.replace(QStringLiteral("group|||Snapshot properties"), QStringLiteral("syn:snap_prop"));
    normalized.replace(QStringLiteral("snaproot|@"), QStringLiteral("syn:snapshots_root"));
    normalized.replace(QStringLiteral("holds|Holds"), QStringLiteral("syn:snapshot_holds"));
    normalized.replace(QStringLiteral("holds|Retenciones"), QStringLiteral("syn:snapshot_holds"));
    if (normalized.startsWith(QStringLiteral("perm|"))) {
        const QStringList parts = normalized.split(QLatin1Char('|'));
        if (parts.size() == 4
            && (parts.at(1) == QStringLiteral("root")
                || parts.at(1) == QStringLiteral("grants_root")
                || parts.at(1) == QStringLiteral("create_root")
                || parts.at(1) == QStringLiteral("sets_root"))) {
            normalized = QStringLiteral("perm|%1").arg(parts.at(1));
        }
    }
    return normalized;
}

QString normalizedConnContentStateToken(QTreeWidget* tree, const QString& token, int connIdx) {
    if (!tree) {
        return token.trimmed();
    }
    // Split trees keep their pool-specific token as-is so their state is stored
    // under a distinct key and does not collide with the main tree's "conn:X" key.
    if (tree->property("zfsmgr.isSplitTree").toBool()) {
        return token.trimmed();
    }
    const QString trimmed = token.trimmed();
    const int sep = trimmed.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return trimmed;
    }
    if (connIdx < 0) {
        return trimmed;
    }
    return QStringLiteral("conn:%1").arg(connIdx);
}

bool treeGroupsPoolsByConnectionRoots(const QTreeWidget* tree) {
    return tree && tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool();
}


template <typename Fn>
void forEachPoolRootItem(QTreeWidget* tree, Fn&& fn) {
    if (!tree) {
        return;
    }
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }
        if (item->data(0, kIsPoolRootRole).toBool()) {
            fn(item);
        }
        for (int i = 0; i < item->childCount(); ++i) {
            rec(item->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        rec(tree->topLevelItem(i));
    }
}

QTreeWidgetItem* findConnectionRootItem(QTreeWidget* tree, int connIdx) {
    if (!tree || connIdx < 0) {
        return nullptr;
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        if (!item) {
            continue;
        }
        if (item->data(0, kIsConnectionRootRole).toBool()
            && item->data(0, kConnIdxRole).toInt() == connIdx) {
            return item;
        }
    }
    return nullptr;
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
    QSet<QString> wanted;
    for (const QString& path : paths) {
        wanted.insert(normalizeConnContentChildPathForCompat(path));
    }
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
    return zfsmgr::base::zfsprops::isUserProperty(prop.toStdString());
}

bool isDatasetPropertyEditableInline(const QString& propName,
                                     const QString& datasetType,
                                     const QString& source,
                                     const QString& readonly,
                                     DatasetPlatformFamily platform) {
    return zfsmgr::base::zfsprops::isInlineEditable(propName.toStdString(),
                                                   datasetType.toStdString(),
                                                   source.toStdString(),
                                                   readonly.toStdString(), platform);
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

QWidget* primaryInlineEditor(QWidget* host) {
    if (!host || !host->layout() || host->layout()->count() <= 0) {
        return nullptr;
    }
    QWidget* editor = host->layout()->itemAt(0) ? host->layout()->itemAt(0)->widget() : nullptr;
    return (editor && editor->focusPolicy() != Qt::NoFocus) ? editor : nullptr;
}

void rebuildInlineEditorTabOrder(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    QList<QWidget*> editors;
    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }
        if (item->data(0, kConnPropRowRole).toBool()
            && item->data(0, kConnPropRowKindRole).toInt() == 2) {
            for (int col = 4; col < tree->columnCount(); ++col) {
                QWidget* host = tree->itemWidget(item, col);
                QWidget* editor = primaryInlineEditor(host);
                if (editor && editor->isEnabled() && editor->isVisible()) {
                    editors.push_back(editor);
                }
            }
        }
        for (int i = 0; i < item->childCount(); ++i) {
            collect(item->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        collect(tree->topLevelItem(i));
    }
    for (int i = 0; i + 1 < editors.size(); ++i) {
        QWidget::setTabOrder(editors.at(i), editors.at(i + 1));
    }
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

QIcon grayTreeIcon(const QIcon& base) {
    if (base.isNull()) {
        return QIcon();
    }
    QIcon gray;
    const QColor tint(QStringLiteral("#7a7f85"));
    const QList<QSize> sizes = {
        QSize(16, 16),
        QSize(24, 24),
        QSize(32, 32),
    };
    for (const QSize& size : sizes) {
        QPixmap src = base.pixmap(size);
        if (src.isNull()) {
            continue;
        }
        QPixmap dst(src.size());
        dst.fill(Qt::transparent);
        QPainter painter(&dst);
        painter.drawPixmap(0, 0, src);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(dst.rect(), tint);
        painter.end();
        gray.addPixmap(dst);
    }
    return gray.isNull() ? base : gray;
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

QIcon contentNodeIcon() {
    const QIcon themed = QIcon::fromTheme(QStringLiteral("folder-open"));
    if (!themed.isNull()) {
        return grayTreeIcon(themed);
    }
    return grayTreeIcon(treeStandardIcon(QStyle::SP_DirOpenIcon));
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
    // Solo el nombre. Llevaba «Dataset» delante cuando en el árbol convivían datasets con
    // nodos de propiedades, permisos, contenido y snapshots, y hacía falta decir qué era
    // cada fila. Ahora dentro solo hay pools y datasets, y el icono ya los distingue.
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

        if ((n->data(0, kIsPoolRootRole).toBool()
             && n->data(0, Qt::UserRole).toString().trimmed().isEmpty())
            || n->data(0, kConnPropRowRole).toBool()) {
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
    const int colCount = tree->columnCount();
    if (colCount <= 0) {
        return;
    }
    constexpr int kMinWidthCol0 = 180;
    constexpr int kMinWidthOther = 72;
    constexpr int kMinWidthProp = 88;
    constexpr int kMaxWidthProp = 240;
    constexpr int kTreeDecorPadding = 28;
    constexpr int kCellPadding = 18;

    QVector<int> widths(colCount, 0);
    const QFontMetrics fm(tree->font());
    for (int col = 0; col < colCount; ++col) {
        if (tree->isColumnHidden(col)) {
            continue;
        }
        const QString headerText = tree->headerItem()->text(col);
        widths[col] = qMax(widths[col], fm.horizontalAdvance(headerText) + kCellPadding);
    }

    std::function<void(QTreeWidgetItem*, int)> measureVisible;
    measureVisible = [&](QTreeWidgetItem* item, int depth) {
        if (!item) {
            return;
        }
        for (int col = 0; col < colCount; ++col) {
            if (tree->isColumnHidden(col)) {
                continue;
            }
            QWidget* cellWidget = tree->itemWidget(item, col);
            const QString text = item->text(col);
            const bool inlineUsed = item->data(col, kConnInlineCellUsedRole).toBool();
            const bool considerCell = (col == 0) || inlineUsed || !text.trimmed().isEmpty() || (cellWidget != nullptr);
            if (!considerCell) {
                continue;
            }

            int candidate = 0;
            if (!text.isEmpty()) {
                candidate = qMax(candidate, fm.horizontalAdvance(text));
            }
            if (cellWidget) {
                QSize hint = cellWidget->sizeHint();
                if (!hint.isValid() || hint.width() <= 0) {
                    hint = cellWidget->minimumSizeHint();
                }
                if (hint.width() > 0) {
                    candidate = qMax(candidate, hint.width());
                }
            }

            if (col == 0) {
                candidate += depth * tree->indentation();
                if (!item->icon(0).isNull()) {
                    candidate += tree->iconSize().width() + 6;
                }
                candidate += kTreeDecorPadding;
            } else {
                candidate += kCellPadding;
            }
            widths[col] = qMax(widths[col], candidate);
        }

        if (!item->isExpanded()) {
            return;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            measureVisible(item->child(i), depth + 1);
        }
    };

    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        measureVisible(tree->topLevelItem(i), 0);
    }

    for (int col = 0; col < colCount; ++col) {
        if (tree->isColumnHidden(col)) {
            continue;
        }
        int width = widths[col];
        if (col == 0) {
            width = qMax(width, kMinWidthCol0);
        } else if (col >= 4) {
            width = qBound(kMinWidthProp, width, kMaxWidthProp);
        } else {
            width = qMax(width, kMinWidthOther);
        }
        tree->setColumnWidth(col, width);
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

    {
        int connIdx = -1;
        QString poolName;
        if (splitConnToken(t, connIdx, poolName)) {
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

    {
        int connIdx = -1;
        QString poolName;
        if (splitConnToken(t, connIdx, poolName)) {
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

// Los datasets de un pool que tienen programación ACTIVADA, contando lo que aún está en
// el borrador sin aplicar. Es el contenido del nodo «Datasets programados» y, a la vez, lo
// que comprueba la prueba: si el borrador se guarda con un testigo y se relee con otro,
// esto devuelve la lista vacía y el nodo desaparece sin decir nada.
QStringList MainWindow::scheduledDatasetsForPool(int connIdx,
                                                 const QString& poolName,
                                                 QMap<QString, QMap<QString, QString>>* propsOut) const {
    QMap<QString, QMap<QString, QString>> porDataset;
    const_cast<MainWindow*>(this)->ensurePoolAutoSnapshotInfoLoaded(connIdx, poolName);
    porDataset = poolAutoSnapshotPropsByDataset(connIdx, poolName);
    for (const PendingPropertyDraftEntry& item : pendingConnContentPropertyDraftsFromModel()) {
        if (item.connIdx != connIdx || item.poolName != poolName) {
            continue;
        }
        const QString datasetName = item.objectName.trimmed();
        if (datasetName.isEmpty() || datasetName.contains(QLatin1Char('@'))) {
            continue;
        }
        for (auto vit = item.draft.valuesByProp.cbegin(); vit != item.draft.valuesByProp.cend(); ++vit) {
            if (vit.key().trimmed().startsWith(QString::fromLatin1(zfsmgr::base::gsa::kPropertyPrefix),
                                               Qt::CaseInsensitive)) {
                porDataset[datasetName].insert(vit.key().trimmed(), vit.value());
            }
        }
        for (auto iit = item.draft.inheritByProp.cbegin(); iit != item.draft.inheritByProp.cend(); ++iit) {
            if (iit.value()
                && iit.key().trimmed().startsWith(QString::fromLatin1(zfsmgr::base::gsa::kPropertyPrefix),
                                                  Qt::CaseInsensitive)) {
                porDataset[datasetName].remove(iit.key().trimmed());
            }
        }
    }
    QStringList programados;
    for (auto it = porDataset.cbegin(); it != porDataset.cend(); ++it) {
        const QString enabledKey =
            findCaseInsensitiveMapKey(it.value(), QStringLiteral("org.fc16.gsa:activado"));
        if (!enabledKey.isEmpty() && gsaBoolOn(it.value().value(enabledKey))) {
            programados.push_back(it.key());
        }
    }
    std::sort(programados.begin(), programados.end(), [](const QString& a, const QString& b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    if (propsOut) {
        *propsOut = porDataset;
    }
    return programados;
}

void MainWindow::stageGsaDraftForTest(int connIdx,
                                      const QString& poolName,
                                      const QString& datasetName,
                                      const QMap<QString, QString>& valuesByProp) {
    const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), poolName);
    for (auto it = valuesByProp.cbegin(); it != valuesByProp.cend(); ++it) {
        updateConnContentDraftInherit(token, datasetName, it.key(), false);
        updateConnContentDraftValue(token, datasetName, it.key(), it.value());
    }
}

QStringList MainWindow::scheduledDatasetsForTest(int connIdx, const QString& poolName) const {
    return scheduledDatasetsForPool(connIdx, poolName, nullptr);
}

namespace {

// Suelta los widgets de celda de las columnas que van a desaparecer.
//
// Las propiedades en línea se pintan con setItemWidget, y cada widget mantiene un índice
// PERSISTENTE sobre su columna. Si la columna desaparece bajo ellos, ese índice queda
// colgando dentro del modelo y el primer borrado de filas posterior lo delata:
//
//   QAbstractItemModel::endRemoveRows: Invalid index ( 0 , 12 )
//
// Reproducido bajando las columnas de propiedades de 10 a 8. Qt se recupera, pero un
// índice inválido vivo en el modelo no es sitio donde dejar nada.
void dropItemWidgetsBeyondColumn(QTreeWidget* tree, int keepColumns) {
    if (!tree || keepColumns >= tree->columnCount()) {
        return;
    }
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        for (int col = keepColumns; col < tree->columnCount(); ++col) {
            if (QWidget* w = tree->itemWidget(node, col)) {
                tree->removeItemWidget(node, col);
                w->deleteLater();
            }
        }
        for (int i = 0; i < node->childCount(); ++i) {
            walk(node->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        walk(tree->topLevelItem(i));
    }
}

}  // namespace

void MainWindow::syncConnContentPropertyColumnsFor(QTreeWidget* tree, const QString& token) {
    Q_UNUSED(token);
    syncConnContentPropertyColumns(tree);
}

// Sin columnas C1...C10 y sin nodos de propiedades dentro del árbol.
//
// Esta función construía las dos cosas: ampliaba el árbol a 4+N columnas —una por
// propiedad visible— y colgaba del item marcado los nodos «Dataset properties»,
// «Permisos» y «Holds», cada uno con sus valores tumbados en esas columnas, en pares de
// filas: una de nombres y otra de valores. Eran 1.100 líneas para enseñar pares
// nombre/valor de lado, y con más de cuatro propiedades había que leerlas en zigzag.
//
// Las tres cosas viven ahora en el frame de detalle de cada panel, cada una con la forma
// que le pega: tabla para las propiedades, árbol con casillas para los permisos y tabla
// para los holds. El árbol vuelve a ser un árbol.
//
// Las cuatro columnas se quedan. La 0 es el nombre y las otras tres están ocultas desde
// siempre, pero NO están vacías: la 1 guarda el nombre del snapshot en `Qt::UserRole` y
// medio programa lo lee de ahí. Reducir el número de columnas se llevaría ese dato por
// delante sin que nada lo avise.
void MainWindow::syncConnContentPropertyColumns(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    if (tree->columnCount() > 4) {
        dropItemWidgetsBeyondColumn(tree, 4);
        tree->setColumnCount(4);
    }
    refreshDatasetExpansionIndicators(tree);
}

void MainWindow::syncConnContentPropertyColumns() {
    syncConnContentPropertyColumns(m_connContentTree);
}

// Lo mismo para el pool: construía el nodo «Pool Information» con las propiedades del
// pool en esas mismas columnas. Ahora las enseña la página de pool del detalle.
void MainWindow::syncConnContentPoolColumns(QTreeWidget* tree, const QString& token) {
    Q_UNUSED(token);
    syncConnContentPropertyColumns(tree);
}

void MainWindow::syncConnContentPoolColumnsFor(QTreeWidget* tree, const QString& token) {
    syncConnContentPoolColumns(tree, token);
}

void MainWindow::syncConnContentPoolColumns() {
    syncConnContentPoolColumns(m_connContentTree, QString());
}

void MainWindow::saveConnContentTreeState(QTreeWidget* tree, const QString& token) {
    if (m_connContentTreeStateWriteLocked || token.isEmpty() || !tree) {
        return;
    }
    const QString normalizedToken = normalizedConnContentStateToken(tree, token, connIdxFromToken(token.left(token.indexOf(QStringLiteral("::")))));
    int scopedConnIdx = -1;
    if (normalizedToken.startsWith(QStringLiteral("conn:"))) {
        bool okScoped = false;
        const QString rawConn = normalizedToken.mid(QStringLiteral("conn:").size());
        const int pipePos = rawConn.indexOf(QLatin1Char('|'));
        const QString connPart = (pipePos >= 0 ? rawConn.left(pipePos) : rawConn).trimmed();
        const int parsed = connPart.toInt(&okScoped);
        if (okScoped && parsed >= 0) {
            scopedConnIdx = parsed;
        }
    }
    const QString scopedToken =
        normalizedToken + QStringLiteral("|top");
    ConnContentTreeState st;
    auto poolStateKey = [this](QTreeWidgetItem* n) -> QString {
        if (!n || !n->data(0, kIsPoolRootRole).toBool()) {
            return QString();
        }
        return QStringLiteral("%1::%2")
            .arg(connToken(n->data(0, kConnIdxRole).toInt()),
                 n->data(0, kPoolNameRole).toString().trimmed());
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
        const bool eligibleDatasetStateNode =
            !ds.isEmpty()
            && !n->data(0, kIsConnectionRootRole).toBool()
            && !n->data(0, kConnSnapshotItemRole).toBool()
            && !n->data(0, kConnPoolAutoSnapshotsDatasetRole).toBool();
        if (eligibleDatasetStateNode) {
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
    forEachPoolRootItem(tree, [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        if (scopedConnIdx >= 0 && n->data(0, kConnIdxRole).toInt() != scopedConnIdx) {
            return;
        }
        rec(n);
    });
    {
        QSet<QString> seenExpandedNodePaths;
        std::function<void(QTreeWidgetItem*)> recExpanded = [&](QTreeWidgetItem* n) {
            if (!n) {
                return;
            }
            if (n->childCount() > 0 && n->isExpanded()) {
                const QString path = connContentNodeStablePath(n);
                const QString tmpPath = connContentNodeTemporaryPath(n);
                if (!path.isEmpty() && !seenExpandedNodePaths.contains(path)) {
                    seenExpandedNodePaths.insert(path);
                    st.expandedNodePaths.push_back(path);
                }
                if (path.isEmpty() && !tmpPath.isEmpty() && !seenExpandedNodePaths.contains(tmpPath)) {
                    seenExpandedNodePaths.insert(tmpPath);
                    st.expandedNodePaths.push_back(tmpPath);
                }
                if (path.isEmpty()
                    && (tmpPath.contains(QStringLiteral("pooltmp:"))
                        || tmpPath.contains(QStringLiteral("dstmp:"))
                        || tmpPath.contains(QStringLiteral("snaptmp:")))) {
                    appLog(QStringLiteral("WARN"),
                           QStringLiteral("Tree state fallback ID used path=%1 node=%2 ds=%3 snap=%4 conn=%5 pool=%6")
                               .arg(tmpPath,
                                    n->text(0).trimmed(),
                                    n->data(0, Qt::UserRole).toString().trimmed(),
                                    n->data(1, Qt::UserRole).toString().trimmed(),
                                    QString::number(n->data(0, kConnIdxRole).toInt()),
                                    n->data(0, kPoolNameRole).toString().trimmed()));
                }
            }
            for (int i = 0; i < n->childCount(); ++i) {
                recExpanded(n->child(i));
            }
        };
        if (scopedConnIdx >= 0) {
            if (QTreeWidgetItem* connRoot = findConnectionRootItem(tree, scopedConnIdx)) {
                recExpanded(connRoot);
            }
        } else {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                recExpanded(tree->topLevelItem(i));
            }
        }
    }
    QTreeWidgetItem* selectedItem = tree->currentItem();
    if (!selectedItem) {
        const auto selected = tree->selectedItems();
        if (!selected.isEmpty()) {
            selectedItem = selected.first();
        }
    }
    if (selectedItem && scopedConnIdx >= 0) {
        QTreeWidgetItem* selOwner = selectedItem;
        while (selOwner && !selOwner->data(0, kIsConnectionRootRole).toBool()
               && !selOwner->data(0, kIsPoolRootRole).toBool()) {
            selOwner = selOwner->parent();
        }
        if (!selOwner || selOwner->data(0, kConnIdxRole).toInt() != scopedConnIdx) {
            selectedItem = nullptr;
        }
    }
    if (selectedItem) {
        st.selectedDataset = selectedItem->data(0, Qt::UserRole).toString();
        st.selectedSnapshot = selectedItem->data(1, Qt::UserRole).toString();
        if (st.selectedDataset.trimmed().isEmpty()) {
            for (QTreeWidgetItem* p = selectedItem->parent(); p; p = p->parent()) {
                const QString ds = p->data(0, Qt::UserRole).toString().trimmed();
                if (ds.isEmpty()) {
                    continue;
                }
                st.selectedDataset = ds;
                const QString snap = p->data(1, Qt::UserRole).toString().trimmed();
                if (!snap.isEmpty()) {
                    st.selectedSnapshot = snap;
                } else {
                    for (QTreeWidgetItem* pp = p->parent(); pp; pp = pp->parent()) {
                        const QString ancSnap = pp->data(1, Qt::UserRole).toString().trimmed();
                        if (!ancSnap.isEmpty()) {
                            st.selectedSnapshot = ancSnap;
                            break;
                        }
                    }
                }
                break;
            }
        }
        st.selectedNodePath = connContentNodeStablePath(selectedItem);
        if (st.selectedNodePath.isEmpty()) {
            const QString selectedNodeTemporaryPath = connContentNodeTemporaryPath(selectedItem);
            if (!selectedNodeTemporaryPath.isEmpty()) {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("Tree state fallback selected path=%1 node=%2 ds=%3 snap=%4 conn=%5 pool=%6")
                           .arg(selectedNodeTemporaryPath,
                                selectedItem->text(0).trimmed(),
                                selectedItem->data(0, Qt::UserRole).toString().trimmed(),
                                selectedItem->data(1, Qt::UserRole).toString().trimmed(),
                                QString::number(selectedItem->data(0, kConnIdxRole).toInt()),
                                selectedItem->data(0, kPoolNameRole).toString().trimmed()));
            }
        }
    }
    if (!st.selectedDataset.trimmed().isEmpty() && !st.selectedSnapshot.trimmed().isEmpty()) {
        st.snapshotByDataset.insert(st.selectedDataset.trimmed(), st.selectedSnapshot.trimmed());
    }
    ConnectionDatasetTreePane* pane = m_topDatasetPane;
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
           QStringLiteral("saveConnContentTreeState token=%1 poolExpanded=%2 infoExpanded=%3 pools=%4 selected=%5 snapshot=%6 selectedPath=%7 expandedDatasets=%8 childPathDatasets=%9 expandedNodePaths=%10 vscroll=%11 hscroll=%12")
               .arg(scopedToken,
                    st.poolRootExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    st.infoExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    poolKeys.join(QStringLiteral(",")),
                    st.selectedDataset,
                    st.selectedSnapshot,
                    st.selectedNodePath,
                    st.expandedDatasets.join(QStringLiteral(",")),
                    childPathDatasets.join(QStringLiteral(",")),
                    QString::number(st.expandedNodePaths.size()),
                    QString::number(st.verticalScrollValue),
                    QString::number(st.horizontalScrollValue)));
}



void MainWindow::applyDebugNodeIdsToTree(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    const bool debugMode = m_logLevelSetting.trimmed().compare(QStringLiteral("debug"), Qt::CaseInsensitive) == 0;
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        QString currentText = node->text(0);
        const QString lastId = node->data(0, kConnDebugLastIdRole).toString().trimmed();
        if (!lastId.isEmpty()) {
            const QString suffix = QStringLiteral(" (%1)").arg(lastId);
            if (currentText.endsWith(suffix)) {
                currentText.chop(suffix.size());
            }
        }
        node->setData(0, kConnDebugBaseTextRole, currentText);

        if (debugMode) {
            QString nodeId = connContentNodeStablePath(node).trimmed();
            if (nodeId.isEmpty()) {
                nodeId = connContentNodeTemporaryPath(node).trimmed();
            }
            if (!nodeId.isEmpty()) {
                node->setText(0, QStringLiteral("%1 (%2)").arg(currentText, nodeId));
                node->setData(0, kConnDebugLastIdRole, nodeId);
            } else {
                node->setText(0, currentText);
                node->setData(0, kConnDebugLastIdRole, QString());
            }
        } else {
            node->setText(0, currentText);
            node->setData(0, kConnDebugLastIdRole, QString());
        }

        for (int i = 0; i < node->childCount(); ++i) {
            rec(node->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        rec(tree->topLevelItem(i));
    }
}

void MainWindow::saveConnContentTreeStateFor(QTreeWidget* tree, const QString& token) {
    saveConnContentTreeState(tree, token);
}

void MainWindow::saveConnContentTreeState(const QString& token) {
    saveConnContentTreeState(m_connContentTree, token);
}

void MainWindow::restoreConnContentTreeStateFor(QTreeWidget* tree, const QString& token) {
    restoreConnContentTreeState(tree, token);
    applyDebugNodeIdsToTree(tree);
}

void MainWindow::rebuildConnContentTreeFor(QTreeWidget* tree,
                                           const QString& token,
                                           int connIdx,
                                           const QString& poolName,
                                           bool restoreState) {
    if (!tree || connIdx < 0 || poolName.isEmpty()) {
        return;
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
    const QString normalizedToken = normalizedConnContentStateToken(tree, token, connIdxFromToken(token.left(token.indexOf(QStringLiteral("::")))));
    int scopedConnIdx = -1;
    if (normalizedToken.startsWith(QStringLiteral("conn:"))) {
        bool okScoped = false;
        const QString rawConn = normalizedToken.mid(QStringLiteral("conn:").size());
        const int pipePos = rawConn.indexOf(QLatin1Char('|'));
        const QString connPart = (pipePos >= 0 ? rawConn.left(pipePos) : rawConn).trimmed();
        const int parsed = connPart.toInt(&okScoped);
        if (okScoped && parsed >= 0) {
            scopedConnIdx = parsed;
        }
    }
    const QString scopedToken =
        normalizedToken + QStringLiteral("|top");
    const auto it = m_connContentTreeStateByToken.constFind(scopedToken);
    if (it == m_connContentTreeStateByToken.cend()) {
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("restoreConnContentTreeState token=%1 no-state").arg(scopedToken));
        return;
    }
    // Copy, not a reference into m_connContentTreeStateByToken: the
    // refreshConnContentPropertiesFor() calls below reach runSsh(), which pumps the
    // event loop, and tree handlers can rewrite this map meanwhile.
    const ConnContentTreeState st = it.value();
    auto logSnapshotPhase = [this, tree, &st](const QString& phase) {
        if (!tree) {
            return;
        }
        QTreeWidgetItem* dsItem = nullptr;
        QTreeWidgetItem* snapItem = nullptr;
        if (!st.selectedDataset.trimmed().isEmpty() && !st.selectedSnapshot.trimmed().isEmpty()) {
            dsItem = findDatasetItem(tree, st.selectedDataset.trimmed());
            snapItem = dsItem ? findSnapshotItemInDatasetNode(dsItem, st.selectedSnapshot.trimmed())
                              : nullptr;
        }
        if (!snapItem && !st.selectedNodePath.trimmed().isEmpty()) {
            std::function<QTreeWidgetItem*(QTreeWidgetItem*)> recFind = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
                if (!n) {
                    return nullptr;
                }
                if (connContentNodeStablePath(n) == st.selectedNodePath.trimmed()) {
                    return n;
                }
                for (int i = 0; i < n->childCount(); ++i) {
                    if (QTreeWidgetItem* f = recFind(n->child(i))) {
                        return f;
                    }
                }
                return nullptr;
            };
            for (int i = 0; i < tree->topLevelItemCount() && !snapItem; ++i) {
                QTreeWidgetItem* selectedByPath = recFind(tree->topLevelItem(i));
                for (QTreeWidgetItem* p = selectedByPath; p; p = p->parent()) {
                    if (p->data(0, kConnSnapshotItemRole).toBool()) {
                        snapItem = p;
                        break;
                    }
                }
            }
        }
        QTreeWidgetItem* propsNode = nullptr;
        if (snapItem) {
            for (int i = 0; i < snapItem->childCount(); ++i) {
                QTreeWidgetItem* child = snapItem->child(i);
                if (!child) {
                    continue;
                }
                const bool isMainPropsNode =
                    child->data(0, kConnPropGroupNodeRole).toBool()
                    && child->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                    && (child->data(0, kConnStatePartRole).toString().trimmed() == QStringLiteral("syn:snap_prop")
                        || child->data(0, kConnStatePartRole).toString().trimmed().isEmpty());
                if (isMainPropsNode) {
                    propsNode = child;
                    break;
                }
            }
        }
        const QString currentPath = tree->currentItem() ? connContentNodeStablePath(tree->currentItem())
                                                         : QString();
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("restoreConnContentTreeState phase=%1 snapExpanded=%2 propsExpanded=%3 currentPath=%4")
                   .arg(phase,
                        (snapItem && snapItem->isExpanded()) ? QStringLiteral("1") : QStringLiteral("0"),
                        (propsNode && propsNode->isExpanded()) ? QStringLiteral("1") : QStringLiteral("0"),
                        currentPath));
    };
    const QSet<QString> expandedSet(st.expandedDatasets.cbegin(), st.expandedDatasets.cend());
    const QStringList childPathDatasets = st.expandedChildPathsByDataset.keys();
    auto poolStateKey = [this](QTreeWidgetItem* n) -> QString {
        if (!n || !n->data(0, kIsPoolRootRole).toBool()) {
            return QString();
        }
        return QStringLiteral("%1::%2")
            .arg(connToken(n->data(0, kConnIdxRole).toInt()),
                 n->data(0, kPoolNameRole).toString().trimmed());
    };
    appLog(QStringLiteral("DEBUG"),
           QStringLiteral("restoreConnContentTreeState begin token=%1 poolExpanded=%2 infoExpanded=%3 pools=%4 selected=%5 snapshot=%6 selectedPath=%7 expandedDatasets=%8 childPathDatasets=%9 expandedNodePaths=%10 vscroll=%11 hscroll=%12")
               .arg(scopedToken,
                    st.poolRootExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    st.infoExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                    st.poolRootExpandedByPool.keys().join(QStringLiteral(",")),
                    st.selectedDataset,
                    st.selectedSnapshot,
                    st.selectedNodePath,
                    st.expandedDatasets.join(QStringLiteral(",")),
                    childPathDatasets.join(QStringLiteral(",")),
                    QString::number(st.expandedNodePaths.size()),
                    QString::number(st.verticalScrollValue),
                    QString::number(st.horizontalScrollValue)));
    logSnapshotPhase(QStringLiteral("begin"));

    std::function<void(QTreeWidgetItem*)> applyExpand = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString ds = n->data(0, Qt::UserRole).toString();
        const bool eligibleDatasetStateNode =
            !ds.isEmpty()
            && !n->data(0, kIsConnectionRootRole).toBool()
            && !n->data(0, kConnSnapshotItemRole).toBool()
            && !n->data(0, kConnPoolAutoSnapshotsDatasetRole).toBool();
        if (eligibleDatasetStateNode) {
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
    if (scopedConnIdx >= 0) {
        if (QTreeWidgetItem* connRoot = findConnectionRootItem(tree, scopedConnIdx)) {
            applyExpand(connRoot);
        }
    } else {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            applyExpand(tree->topLevelItem(i));
        }
    }
    logSnapshotPhase(QStringLiteral("after-applyExpand"));
    forEachPoolRootItem(tree, [&](QTreeWidgetItem* top) {
        if (!top) {
            return;
        }
        if (scopedConnIdx >= 0 && top->data(0, kConnIdxRole).toInt() != scopedConnIdx) {
            return;
        }
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
    });
    logSnapshotPhase(QStringLiteral("after-pool-info"));

    auto applyStoredSnapshotToItem = [&st](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }
        const QString datasetName = item->data(0, Qt::UserRole).toString().trimmed();
        if (datasetName.isEmpty()) {
            return;
        }
        item->setData(1, Qt::UserRole, st.snapshotByDataset.value(datasetName).trimmed());
    };

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
    auto needsExpandedPropertyMaterialization = [&pathHasSegmentPrefix](const QStringList& paths) {
        for (const QString& path : paths) {
            if (pathHasSegmentPrefix(path, QStringLiteral("group|"))
                || pathHasSegmentPrefix(path, QStringLiteral("gsa|"))
                || pathHasSegmentPrefix(path, QStringLiteral("holds|"))
                || pathHasSegmentPrefix(path, QStringLiteral("hold|"))) {
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
        if (item->data(0, kIsConnectionRootRole).toBool()
            || item->data(0, kConnSnapshotItemRole).toBool()
            || item->data(0, kConnPoolAutoSnapshotsDatasetRole).toBool()) {
            continue;
        }
        bool needsDatasetMainPropsMaterialization = false;
        QSet<QString> snapshotGuidsToMaterialize;
        for (const QString& childPath : childIt.value()) {
            const bool hasDsPropsMarker = childPath.contains(QStringLiteral("syn:ds_prop"));
            const bool hasSnapPropsMarker = childPath.contains(QStringLiteral("syn:snap_prop"));
            if (!(hasDsPropsMarker || hasSnapPropsMarker)) {
                continue;
            }
            const bool pathTargetsSnapshot = childPath.contains(QStringLiteral("snap:"));
            if (!pathTargetsSnapshot) {
                needsDatasetMainPropsMaterialization = true;
                continue;
            }
            const QRegularExpression reGuid(QStringLiteral("(?:^|/)snap:([^/]+)"));
            const QRegularExpressionMatch matchGuid = reGuid.match(childPath);
            if (matchGuid.hasMatch()) {
                const QString snapGuid = matchGuid.captured(1).trimmed();
                if (!snapGuid.isEmpty()) {
                    snapshotGuidsToMaterialize.insert(snapGuid);
                    continue;
                }
            }
        }
        if (needsDatasetMainPropsMaterialization) {
            applyStoredSnapshotToItem(item);
            {
                const QSignalBlocker blocker(tree);
                tree->setCurrentItem(item);
            }
            refreshConnContentPropertiesFor(tree);
            syncConnContentPropertyColumnsFor(tree, token);
        }
        for (const QString& snapGuid : snapshotGuidsToMaterialize) {
            QTreeWidgetItem* snapItem = findSnapshotItemInDatasetNodeByGuid(item, snapGuid);
            if (!snapItem) {
                continue;
            }
            {
                const QSignalBlocker blocker(tree);
                tree->setCurrentItem(snapItem);
            }
            refreshConnContentPropertiesFor(tree);
            syncConnContentPropertyColumnsFor(tree, token);
        }
    }
    logSnapshotPhase(QStringLiteral("after-materialize"));

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
                if (QTreeWidgetItem* snapItem = findSnapshotItemInDatasetNode(sel, st.selectedSnapshot)) {
                    sel = snapItem;
                }
            }
            for (QTreeWidgetItem* p = sel->parent(); p; p = p->parent()) {
                p->setExpanded(true);
            }
            const QSignalBlocker blocker(tree);
            tree->setCurrentItem(sel);
        }
    }
    logSnapshotPhase(QStringLiteral("after-selected"));
    if (!st.expandedNodePaths.isEmpty() || !st.selectedNodePath.trimmed().isEmpty()) {
        QHash<QString, QTreeWidgetItem*> nodeByPath;
        std::function<void(QTreeWidgetItem*)> buildPathMap = [&](QTreeWidgetItem* node) {
            if (!node) {
                return;
            }
            const QString path = connContentNodeStablePath(node);
            if (!path.isEmpty()) {
                nodeByPath.insert(path, node);
            }
            for (int i = 0; i < node->childCount(); ++i) {
                buildPathMap(node->child(i));
            }
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            buildPathMap(tree->topLevelItem(i));
        }

        QTreeWidgetItem* fallbackSelectedNode = nullptr;
        const QString wantedSelectedPath = st.selectedNodePath.trimmed();
        const bool wantsMainSnapshotProps =
            wantedSelectedPath.endsWith(QStringLiteral(";syn:snap_prop"))
            || wantedSelectedPath == QStringLiteral("syn:snap_prop")
            || wantedSelectedPath.endsWith(QStringLiteral(";syn:main_properties"))
            || wantedSelectedPath == QStringLiteral("syn:main_properties");
        if (wantsMainSnapshotProps
            && !st.selectedDataset.trimmed().isEmpty()
            && !st.selectedSnapshot.trimmed().isEmpty()) {
            QTreeWidgetItem* dsItem = findDatasetItem(tree, st.selectedDataset.trimmed());
            if (dsItem) {
                QTreeWidgetItem* snapItem =
                    findSnapshotItemInDatasetNode(dsItem, st.selectedSnapshot.trimmed());
                if (snapItem) {
                    for (QTreeWidgetItem* p = snapItem; p; p = p->parent()) {
                        p->setExpanded(true);
                    }
                    for (int i = 0; i < snapItem->childCount(); ++i) {
                        QTreeWidgetItem* child = snapItem->child(i);
                        if (!child) {
                            continue;
                        }
                        const bool isMainPropsNode =
                            child->data(0, kConnPropGroupNodeRole).toBool()
                            && child->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                            && (child->data(0, kConnStatePartRole).toString().trimmed() == QStringLiteral("syn:snap_prop")
                                || child->data(0, kConnStatePartRole).toString().trimmed().isEmpty());
                        if (!isMainPropsNode) {
                            continue;
                        }
                        child->setExpanded(true);
                        fallbackSelectedNode = child;
                        const QString stablePath = connContentNodeStablePath(child);
                        const QString tempPath = connContentNodeTemporaryPath(child);
                        if (!stablePath.isEmpty()) {
                            nodeByPath.insert(stablePath, child);
                        }
                        if (!tempPath.isEmpty()) {
                            nodeByPath.insert(tempPath, child);
                        }
                        appLog(QStringLiteral("DEBUG"),
                               QStringLiteral("restoreConnContentTreeState fallback main snapshot props path=%1")
                                   .arg(stablePath));
                        break;
                    }
                }
            }
        }

        QSet<QString> wantedExpanded;
        for (const QString& raw : st.expandedNodePaths) {
            const QString trimmed = raw.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            const QStringList parts = splitEscapedConnStatePath(trimmed);
            if (parts.isEmpty()) {
                continue;
            }
            QStringList prefixParts;
            for (const QString& part : parts) {
                prefixParts.push_back(part);
                QStringList escapedPrefixParts;
                escapedPrefixParts.reserve(prefixParts.size());
                for (const QString& p : prefixParts) {
                    escapedPrefixParts.push_back(escapeConnStatePart(p));
                }
                wantedExpanded.insert(escapedPrefixParts.join(QStringLiteral(";")));
            }
        }
        for (auto itNode = nodeByPath.cbegin(); itNode != nodeByPath.cend(); ++itNode) {
            QTreeWidgetItem* node = itNode.value();
            if (!node || node->childCount() == 0) {
                continue;
            }
            if (wantedExpanded.contains(itNode.key())) {
                node->setExpanded(true);
            }
        }
        const QString selPath = st.selectedNodePath.trimmed();
        if (!selPath.isEmpty()) {
            QTreeWidgetItem* sel = nodeByPath.value(selPath, nullptr);
            if (!sel) {
                sel = nodeByPath.value(splitEscapedConnStatePath(selPath).join(QStringLiteral(";")), nullptr);
            }
            if (!sel) {
                sel = fallbackSelectedNode;
            }
            if (sel) {
                for (QTreeWidgetItem* p = sel->parent(); p; p = p->parent()) {
                    p->setExpanded(true);
                }
                const QSignalBlocker blocker(tree);
                tree->setCurrentItem(sel);
            }
        }
    }
    logSnapshotPhase(QStringLiteral("after-nodepath"));
    ConnectionDatasetTreePane* pane = m_topDatasetPane;
    if (pane) {
        ConnectionDatasetTreePane::VisualState visualState;
        visualState.headerState = st.headerState;
        visualState.verticalScroll = st.verticalScrollValue;
        visualState.horizontalScroll = st.horizontalScrollValue;
        pane->restoreVisualState(visualState);
    } else {
        if (tree->verticalScrollBar()) {
            tree->verticalScrollBar()->setValue(st.verticalScrollValue);
        }
        if (tree->horizontalScrollBar()) {
            tree->horizontalScrollBar()->setValue(st.horizontalScrollValue);
        }
    }
    QStringList finalPoolStates;
    forEachPoolRootItem(tree, [&](QTreeWidgetItem* top) {
        finalPoolStates.push_back(QStringLiteral("%1=%2")
                                      .arg(poolStateKey(top),
                                           top->isExpanded() ? QStringLiteral("1")
                                                             : QStringLiteral("0")));
    });
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
    return options;
}

bool MainWindow::applyConnectionInlineFieldValue(int connIdx,
                                                 const QString& fieldKey,
                                                 const QString& rawValue,
                                                 QString* normalizedOut,
                                                 QString* errorOut) {
    if (normalizedOut) {
        normalizedOut->clear();
    }
    if (errorOut) {
        errorOut->clear();
    }
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Índice de conexión inválido.");
        }
        return false;
    }
    const QString key = fieldKey.trimmed().toLower();
    if (key.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Campo vacío.");
        }
        return false;
    }

    ConnectionProfile updated = m_conns.profiles[connIdx];
    auto boolFromText = [](const QString& text, bool* okOut) -> bool {
        const QString v = text.trimmed().toLower();
        if (v == QStringLiteral("1") || v == QStringLiteral("on") || v == QStringLiteral("yes")
            || v == QStringLiteral("true") || v == QStringLiteral("si") || v == QStringLiteral("sí")) {
            if (okOut) {
                *okOut = true;
            }
            return true;
        }
        if (v == QStringLiteral("0") || v == QStringLiteral("off") || v == QStringLiteral("no")
            || v == QStringLiteral("false")) {
            if (okOut) {
                *okOut = true;
            }
            return false;
        }
        if (okOut) {
            *okOut = false;
        }
        return false;
    };
    QString normalized = rawValue;
    if (key == QStringLiteral("id")) {
        updated.id = rawValue.trimmed();
        normalized = updated.id;
    } else if (key == QStringLiteral("name")) {
        updated.name = rawValue.trimmed();
        normalized = updated.name;
    } else if (key == QStringLiteral("machine_uid")) {
        updated.machineUid = rawValue.trimmed();
        normalized = updated.machineUid;
    } else if (key == QStringLiteral("conn_type")) {
        updated.connType = rawValue.trimmed();
        normalized = updated.connType;
    } else if (key == QStringLiteral("os_type")) {
        updated.osType = rawValue.trimmed();
        normalized = updated.osType;
    } else if (key == QStringLiteral("host")) {
        updated.host = rawValue.trimmed();
        normalized = updated.host;
    } else if (key == QStringLiteral("port")) {
        bool okPort = false;
        const int port = rawValue.trimmed().toInt(&okPort);
        if (!okPort || port <= 0) {
            if (errorOut) {
                *errorOut = QStringLiteral("Puerto inválido: %1").arg(rawValue.trimmed());
            }
            return false;
        }
        updated.port = port;
        normalized = QString::number(updated.port);
    } else if (key == QStringLiteral("ssh_address_family")) {
        const QString family = rawValue.trimmed().toLower();
        if (family == QStringLiteral("ipv4")
            || family == QStringLiteral("ipv6")
            || family == QStringLiteral("auto")) {
            updated.sshAddressFamily = family;
            normalized = family;
        } else {
            updated.sshAddressFamily = QStringLiteral("auto");
            normalized = QStringLiteral("auto");
        }
    } else if (key == QStringLiteral("username")) {
        updated.username = rawValue;
        normalized = updated.username;
    } else if (key == QStringLiteral("password")) {
        updated.password = rawValue;
        normalized = updated.password;
    } else if (key == QStringLiteral("key_path")) {
        updated.keyPath = rawValue.trimmed();
        normalized = updated.keyPath;
    } else if (key == QStringLiteral("use_sudo")) {
        bool okBool = false;
        const bool useSudo = boolFromText(rawValue, &okBool);
        if (!okBool) {
            if (errorOut) {
                *errorOut = QStringLiteral("Valor booleano inválido para use_sudo: %1").arg(rawValue.trimmed());
            }
            return false;
        }
        updated.useSudo = useSudo;
        normalized = updated.useSudo ? QStringLiteral("true") : QStringLiteral("false");
    } else {
        if (errorOut) {
            *errorOut = QStringLiteral("Campo de conexión no soportado: %1").arg(fieldKey);
        }
        return false;
    }

    QString saveError;
    if (!m_conns.store.upsertConnection(updated, saveError)) {
        if (errorOut) {
            *errorOut = saveError.trimmed().isEmpty() ? QStringLiteral("No se pudo guardar la conexión.") : saveError;
        }
        return false;
    }
    m_conns.profiles[connIdx] = updated;
    if (normalizedOut) {
        *normalizedOut = normalized;
    }
    return true;
}

void MainWindow::ensureConnectionRootAuxNodes(QTreeWidget* tree, QTreeWidgetItem* connRoot, int connIdx) {
    if (!tree || !connRoot || connIdx < 0 || connIdx >= m_conns.profiles.size() || connIdx >= m_conns.states.size()) {
        return;
    }
    if (isConnectionDisconnected(connIdx)) {
        // Regla estricta: conexión desconectada => nodo raíz sin hijos.
        while (connRoot->childCount() > 0) {
            delete connRoot->takeChild(0);
        }
        connRoot->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
        return;
    }
    const int propCols = qBound(4, m_connPropColumnsSetting, 16);
    auto findSection = [&](const QString& sectionId) -> QTreeWidgetItem* {
        for (int i = 0; i < connRoot->childCount(); ++i) {
            QTreeWidgetItem* child = connRoot->child(i);
            if (!child) {
                continue;
            }
            if (child->data(0, kConnRootSectionRole).toString() == sectionId) {
                return child;
            }
        }
        return nullptr;
    };
    auto ensureSection = [&](const QString& sectionId,
                             const QString& title,
                             QStyle::StandardPixmap icon) -> QTreeWidgetItem* {
        QTreeWidgetItem* section = findSection(sectionId);
        if (!section) {
            section = new QTreeWidgetItem();
            section->setFlags(section->flags() & ~Qt::ItemIsUserCheckable);
            section->setData(0, kConnRootSectionRole, sectionId);
            section->setData(0, kConnIdxRole, connIdx);
            section->setData(0, kConnContentNodeRole, true);
            connRoot->addChild(section);
        }
        section->setText(0, title);
        section->setIcon(0, treeStandardIcon(icon));
        section->setData(0, kConnIdxRole, connIdx);
        return section;
    };

    const bool hadPropsNode = (findSection(QStringLiteral("connection_properties")) != nullptr);
    QTreeWidgetItem* propsNode =
        ensureSection(QStringLiteral("connection_properties"),
                      QStringLiteral("Properties"),
                      QStyle::SP_FileDialogDetailedView);
    const bool propsNodeWasExpanded = propsNode ? propsNode->isExpanded() : false;
    while (propsNode->childCount() > 0) {
        delete propsNode->takeChild(0);
    }

    const ConnectionProfile cp = m_conns.profiles[connIdx];
    const bool localConnection = isLocalConnection(connIdx);
    const bool osIsWindows = cp.osType.trimmed().toLower().contains(QStringLiteral("windows"));
    const QVector<QPair<QString, QString>> fields = {
        {QStringLiteral("name"), cp.name},
        {QStringLiteral("machine_uid"), cp.machineUid},
        {QStringLiteral("conn_type"), cp.connType},
        {QStringLiteral("os_type"), cp.osType},
        {QStringLiteral("host"), cp.host},
        {QStringLiteral("port"), QString::number(cp.port)},
        {QStringLiteral("username"), cp.username},
        {QStringLiteral("password"), cp.password},
        {QStringLiteral("key_path"), cp.keyPath},
        {QStringLiteral("use_sudo"), cp.useSudo ? QStringLiteral("true") : QStringLiteral("false")}
    };
    const QColor nameRowBg(232, 240, 250);
    for (int base = 0; base < fields.size(); base += propCols) {
        auto* rowNames = new QTreeWidgetItem();
        rowNames->setData(0, kConnRootInlineFieldRole, true);
        rowNames->setData(0, kConnPropRowKindRole, 1);
        rowNames->setData(0, kConnIdxRole, connIdx);
        rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
        rowNames->setText(0, QString());
        auto* rowValues = new QTreeWidgetItem();
        rowValues->setData(0, kConnRootInlineFieldRole, true);
        rowValues->setData(0, kConnPropRowKindRole, 2);
        rowValues->setData(0, kConnIdxRole, connIdx);
        rowValues->setFlags((rowValues->flags() | Qt::ItemIsEditable) & ~Qt::ItemIsUserCheckable);
        rowValues->setText(0, QString());
        rowValues->setSizeHint(0, QSize(0, 33));
        propsNode->addChild(rowNames);
        propsNode->addChild(rowValues);

        for (int off = 0; off < propCols; ++off) {
            const int idx = base + off;
            if (idx >= fields.size()) {
                break;
            }
            const int col = 4 + off;
            if (col >= tree->columnCount()) {
                break;
            }
            const QString key = fields.at(idx).first;
            const QString value = fields.at(idx).second;
            rowNames->setBackground(col, QBrush(nameRowBg));
            rowNames->setData(col, kConnInlineCellUsedRole, true);
            rowValues->setData(col, kConnInlineCellUsedRole, true);
            tree->setItemWidget(rowNames, col, new InlinePropNameWidget(key, false, tree));
            rowNames->setTextAlignment(col, Qt::AlignCenter);
            const bool isPassword = (key == QStringLiteral("password"));
            const QString maskedValue = isPassword
                                            ? (value.isEmpty() ? QString() : QStringLiteral("********"))
                                            : value;
            rowValues->setText(col, maskedValue);
            rowValues->setTextAlignment(col, Qt::AlignCenter);
            rowValues->setData(col, kConnRootInlineFieldRole, key);
            rowValues->setData(col, kConnRootInlineRawValueRole, value);
            bool editable = true;
            if (key == QStringLiteral("machine_uid") || key == QStringLiteral("os_type")) {
                editable = false;
            }
            if (key == QStringLiteral("conn_type") && !osIsWindows) {
                editable = false;
            }
            if (localConnection && key != QStringLiteral("username") && key != QStringLiteral("password")) {
                editable = false;
            }
            rowValues->setData(col, kConnRootInlineEditableRole, editable);
            if (key == QStringLiteral("use_sudo")) {
                const QString boolText = value.trimmed().toLower();
                const bool checked = (boolText == QStringLiteral("1")
                                      || boolText == QStringLiteral("on")
                                      || boolText == QStringLiteral("yes")
                                      || boolText == QStringLiteral("true")
                                      || boolText == QStringLiteral("si")
                                      || boolText == QStringLiteral("sí"));
                rowValues->setData(col, kConnRootInlineRawValueRole, checked ? QStringLiteral("true")
                                                                              : QStringLiteral("false"));
                rowValues->setText(col, checked ? QStringLiteral("true") : QStringLiteral("false"));
                auto* check = new QCheckBox(tree);
                check->setChecked(checked);
                check->setEnabled(editable);
                tree->setItemWidget(rowValues, col, wrapInlineCellEditor(check, tree));
                if (!editable) {
                    rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsEditable);
                    continue;
                }
                QObject::connect(check, &QCheckBox::toggled, tree, [this, tree, rowValues, col](bool on) {
                    if (!rowValues) {
                        return;
                    }
                    const QString boolValue = on ? QStringLiteral("true") : QStringLiteral("false");
                    rowValues->setData(col, kConnRootInlineRawValueRole, boolValue);
                    rowValues->setText(col, boolValue);
                    onDatasetTreeItemChanged(tree, rowValues, col, DatasetTreeContext::ConnectionContent);
                });
                continue;
            }
            auto* edit = new QLineEdit(tree);
            edit->setText(value);
            if (isPassword) {
                edit->setEchoMode(QLineEdit::Password);
            }
            edit->setReadOnly(!editable);
            edit->setMinimumHeight(22);
            edit->setMaximumHeight(22);
            edit->setFont(tree->font());
            edit->setStyleSheet(editable
                                    ? QStringLiteral("QLineEdit{padding:0 5px; margin:0px;}")
                                    : QStringLiteral("QLineEdit{padding:0 5px; margin:0px; background:#f3f5f7; color:#5f6b76;}"));
            tree->setItemWidget(rowValues, col, wrapInlineCellEditor(edit, tree));
            if (!editable) {
                rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsEditable);
                continue;
            }
            QObject::connect(edit, &QLineEdit::editingFinished, tree, [this, tree, rowValues, col, edit, isPassword]() {
                if (!rowValues || !edit) {
                    return;
                }
                const QString rawValue = edit->text();
                rowValues->setData(col, kConnRootInlineRawValueRole, rawValue);
                rowValues->setText(col, isPassword ? (rawValue.isEmpty() ? QString() : QStringLiteral("********"))
                                                   : rawValue);
                onDatasetTreeItemChanged(tree, rowValues, col, DatasetTreeContext::ConnectionContent);
            });
        }
    }
    propsNode->setExpanded(hadPropsNode ? propsNodeWasExpanded : false);

    const bool hadInfoNode = (findSection(QStringLiteral("connection_info")) != nullptr);
    QTreeWidgetItem* infoNode =
        ensureSection(QStringLiteral("connection_info"),
                      QStringLiteral("Info"),
                      QStyle::SP_MessageBoxInformation);
    const bool infoNodeWasExpanded = infoNode ? infoNode->isExpanded() : false;
    QMap<QString, bool> infoChildExpandedById;
    if (infoNode) {
        for (int i = 0; i < infoNode->childCount(); ++i) {
            QTreeWidgetItem* child = infoNode->child(i);
            if (!child) {
                continue;
            }
            const QString childId = child->data(0, kConnStatePartRole).toString().trimmed();
            if (!childId.isEmpty()) {
                infoChildExpandedById.insert(childId, child->isExpanded());
            }
        }
    }
    while (infoNode->childCount() > 0) {
        delete infoNode->takeChild(0);
    }

    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    auto appendReadOnlyInlineProps = [&](QTreeWidgetItem* parent,
                                         const QVector<QPair<QString, QString>>& props) {
        if (!parent || props.isEmpty()) {
            return;
        }
        for (int base = 0; base < props.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem();
            rowNames->setData(0, kConnRootInlineFieldRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setData(0, kConnIdxRole, connIdx);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            rowNames->setText(0, QString());
            auto* rowValues = new QTreeWidgetItem();
            rowValues->setData(0, kConnRootInlineFieldRole, true);
            rowValues->setData(0, kConnPropRowKindRole, 2);
            rowValues->setData(0, kConnIdxRole, connIdx);
            rowValues->setFlags((rowValues->flags() & ~Qt::ItemIsEditable) & ~Qt::ItemIsUserCheckable);
            rowValues->setText(0, QString());
            rowValues->setSizeHint(0, QSize(0, 33));
            parent->addChild(rowNames);
            parent->addChild(rowValues);
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= props.size()) {
                    break;
                }
                const int col = 4 + off;
                if (col >= tree->columnCount()) {
                    break;
                }
                const QString key = props.at(idx).first;
                const QString value = props.at(idx).second;
                rowNames->setData(col, kConnInlineCellUsedRole, true);
                rowValues->setData(col, kConnInlineCellUsedRole, true);
                tree->setItemWidget(rowNames, col, new InlinePropNameWidget(key, false, tree));
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                rowValues->setText(col, value);
                rowValues->setTextAlignment(col, Qt::AlignCenter);
                auto* edit = new QLineEdit(tree);
                edit->setText(value);
                edit->setReadOnly(true);
                edit->setMinimumHeight(22);
                edit->setMaximumHeight(22);
                edit->setFont(tree->font());
                edit->setStyleSheet(QStringLiteral("QLineEdit{padding:0 5px; margin:0px; background:#f3f5f7; color:#5f6b76;}"));
                tree->setItemWidget(rowValues, col, wrapInlineCellEditor(edit, tree));
            }
        }
    };

    auto listOrNone = [&](const QStringList& values) -> QString {
        return values.isEmpty() ? trk(QStringLiteral("t_none_001"),
                                      QStringLiteral("(ninguno)"),
                                      QStringLiteral("(none)"))
                                : values.join(QStringLiteral(", "));
    };
    const QString statusText = st.status.trimmed().isEmpty() ? QStringLiteral("-") : st.status.trimmed();
    const QString colorReason = connectionStateColorReason(connIdx).trimmed();
    const QString osText = st.osLine.trimmed().isEmpty() ? QStringLiteral("-") : st.osLine.trimmed();
    const QString methodText = st.connectionMethod.trimmed().isEmpty()
                                   ? m_conns.profiles[connIdx].connType.trimmed()
                                   : st.connectionMethod.trimmed();
    const QString zfsText = st.zfsVersionFull.trimmed().isEmpty()
                                ? (st.zfsVersion.trimmed().isEmpty()
                                       ? QStringLiteral("-")
                                       : QStringLiteral("OpenZFS %1").arg(st.zfsVersion.trimmed()))
                                : st.zfsVersionFull.trimmed();
    const QString packageMgrText = st.helperPackageManagerLabel.trimmed().isEmpty()
                                       ? QStringLiteral("-")
                                       : QStringLiteral("%1%2")
                                             .arg(st.helperPackageManagerLabel.trimmed(),
                                                  st.helperPackageManagerDetected
                                                      ? trk(QStringLiteral("t_detected_001"),
                                                            QStringLiteral(" (detectado)"),
                                                            QStringLiteral(" (detected)"))
                                                      : trk(QStringLiteral("t_not_detected_001"),
                                                            QStringLiteral(" (no detectado)"),
                                                            QStringLiteral(" (not detected)")));
    QVector<QPair<QString, QString>> infoProps = {
        {trk(QStringLiteral("t_status_001"),
             QStringLiteral("Estado"),
             QStringLiteral("Status")),
         statusText},
        {trk(QStringLiteral("t_os_001"),
             QStringLiteral("Sistema operativo"),
             QStringLiteral("Operating system")),
         osText},
        {trk(QStringLiteral("t_conn_method_001"),
             QStringLiteral("Método de conexión"),
             QStringLiteral("Connection method")),
         methodText},
        {QStringLiteral("OpenZFS"), zfsText},
        {trk(QStringLiteral("t_helper_platform_001"),
             QStringLiteral("Plataforma instalación auxiliar"),
             QStringLiteral("Helper install platform")),
         st.helperPlatformLabel.trimmed().isEmpty() ? QStringLiteral("-") : st.helperPlatformLabel.trimmed()},
        {trk(QStringLiteral("t_package_manager_001"),
             QStringLiteral("Gestor de paquetes"),
             QStringLiteral("Package manager")),
         packageMgrText},
        {trk(QStringLiteral("t_assisted_install_001"),
             QStringLiteral("Instalación asistida"),
             QStringLiteral("Assisted install")),
         st.helperInstallSupported
             ? trk(QStringLiteral("t_yes_001"),
                   QStringLiteral("sí"),
                   QStringLiteral("yes"))
             : trk(QStringLiteral("t_no_001"),
                   QStringLiteral("no"),
                   QStringLiteral("no"))},
        {trk(QStringLiteral("t_installable_commands_001"),
             QStringLiteral("Comandos instalables desde ZFSMgr"),
             QStringLiteral("Commands installable from ZFSMgr")),
         listOrNone(st.helperInstallableCommands)},
        {trk(QStringLiteral("t_unsupported_installer_commands_001"),
             QStringLiteral("Comandos no soportados por instalador"),
             QStringLiteral("Commands not supported by installer")),
         listOrNone(st.helperUnsupportedCommands)}
    };
    if (!colorReason.isEmpty()) {
        infoProps.insert(1,
                         {trk(QStringLiteral("t_color_reason_001"),
                              QStringLiteral("Motivo del color"),
                              QStringLiteral("Color reason")),
                          colorReason});
    }
    if (!st.helperInstallReason.trimmed().isEmpty()) {
        infoProps.push_back({trk(QStringLiteral("t_assisted_install_reason_001"),
                                 QStringLiteral("Motivo instalación asistida"),
                                 QStringLiteral("Assisted install reason")),
                             st.helperInstallReason.trimmed()});
    }
    auto* generalNode = new QTreeWidgetItem(infoNode);
    generalNode->setFlags(generalNode->flags() & ~Qt::ItemIsUserCheckable);
    generalNode->setData(0, kConnContentNodeRole, true);
    generalNode->setData(0, kConnIdxRole, connIdx);
    generalNode->setData(0, kConnStatePartRole, QStringLiteral("syn:general"));
    generalNode->setText(0, trk(QStringLiteral("t_conn_general_001"),
                                QStringLiteral("General"),
                                QStringLiteral("General")));
    generalNode->setIcon(0, treeStandardIcon(QStyle::SP_FileDialogDetailedView));
    appendReadOnlyInlineProps(generalNode, infoProps);
    generalNode->setExpanded(infoChildExpandedById.value(QStringLiteral("syn:general"), true));


    auto* agentNode = new QTreeWidgetItem(infoNode);
    agentNode->setFlags(agentNode->flags() & ~Qt::ItemIsUserCheckable);
    agentNode->setData(0, kConnContentNodeRole, true);
    agentNode->setData(0, kConnIdxRole, connIdx);
    agentNode->setData(0, kConnStatePartRole, QStringLiteral("syn:agent"));
    agentNode->setText(0, trk(QStringLiteral("t_conn_agent_001"),
                              QStringLiteral("Daemon"),
                              QStringLiteral("Daemon")));
    agentNode->setIcon(0, treeStandardIcon(QStyle::SP_ComputerIcon));
    const QString agentStatus = !st.daemonInstalled
                                    ? trk(QStringLiteral("t_conn_agent_not_installed_001"),
                                          QStringLiteral("no instalado"),
                                          QStringLiteral("not installed"))
                                    : QStringLiteral("%1 | %2 | %3")
                                          .arg(st.daemonVersion.trimmed().isEmpty() ? QStringLiteral("-")
                                                                                    : st.daemonVersion.trimmed(),
                                               st.daemonScheduler.trimmed().isEmpty() ? QStringLiteral("-")
                                                                                      : st.daemonScheduler.trimmed(),
                                               st.daemonActive
                                                   ? trk(QStringLiteral("t_conn_agent_active_001"),
                                                         QStringLiteral("activo"),
                                                         QStringLiteral("active"))
                                                   : trk(QStringLiteral("t_conn_agent_inactive_001"),
                                                         QStringLiteral("inactivo"),
                                                         QStringLiteral("inactive")));
    QVector<QPair<QString, QString>> agentProps = {
        {trk(QStringLiteral("t_conn_agent_label_001"),
             QStringLiteral("Daemon"),
             QStringLiteral("Daemon")),
         agentStatus},
        {trk(QStringLiteral("t_conn_agent_api_label_001"),
             QStringLiteral("API"),
             QStringLiteral("API")),
         st.daemonApiVersion.trimmed().isEmpty() ? QStringLiteral("-") : st.daemonApiVersion.trimmed()},
    };
    if (!st.daemonDetail.trimmed().isEmpty()) {
        agentProps.push_back(
            {trk(QStringLiteral("t_conn_agent_detail_001"),
                 QStringLiteral("Detalle"),
                 QStringLiteral("Detail")),
             st.daemonDetail.trimmed()});
    }
    if (st.daemonNeedsAttention && !st.daemonAttentionReasons.isEmpty()) {
        agentProps.push_back(
            {trk(QStringLiteral("t_conn_agent_attention_001"),
                 QStringLiteral("Atención daemon"),
                 QStringLiteral("Daemon attention")),
             st.daemonAttentionReasons.join(QStringLiteral(", "))});
    }
    appendReadOnlyInlineProps(agentNode, agentProps);
    agentNode->setExpanded(infoChildExpandedById.value(QStringLiteral("syn:agent"), false));

    auto* commandsNode = new QTreeWidgetItem(infoNode);
    commandsNode->setFlags(commandsNode->flags() & ~Qt::ItemIsUserCheckable);
    commandsNode->setData(0, kConnContentNodeRole, true);
    commandsNode->setData(0, kConnIdxRole, connIdx);
    commandsNode->setData(0, kConnStatePartRole, QStringLiteral("syn:commands"));
    commandsNode->setText(0, trk(QStringLiteral("t_conn_commands_001"),
                                 QStringLiteral("Comandos"),
                                 QStringLiteral("Commands")));
    {
        const QIcon terminalIcon = QIcon::fromTheme(QStringLiteral("utilities-terminal"));
        commandsNode->setIcon(0, terminalIcon.isNull()
                                     ? treeStandardIcon(QStyle::SP_CommandLink)
                                     : terminalIcon);
    }
    auto appendCommandRows = [&](QTreeWidgetItem* parent,
                                 const QVector<QPair<QString, bool>>& entries) {
        if (!parent || entries.isEmpty()) {
            return;
        }
        for (int base = 0; base < entries.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem(parent);
            rowNames->setData(0, kConnRootInlineFieldRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setData(0, kConnIdxRole, connIdx);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            rowNames->setText(0, QString());
            auto* rowValues = new QTreeWidgetItem(parent);
            rowValues->setData(0, kConnRootInlineFieldRole, true);
            rowValues->setData(0, kConnPropRowKindRole, 2);
            rowValues->setData(0, kConnIdxRole, connIdx);
            rowValues->setFlags((rowValues->flags() & ~Qt::ItemIsEditable) & ~Qt::ItemIsUserCheckable);
            rowValues->setText(0, QString());
            rowValues->setSizeHint(0, QSize(0, 33));
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= entries.size()) {
                    break;
                }
                const int col = 4 + off;
                if (col >= tree->columnCount()) {
                    break;
                }
                const QString cmd = entries.at(idx).first;
                const bool detected = entries.at(idx).second;
                rowNames->setData(col, kConnInlineCellUsedRole, true);
                rowValues->setData(col, kConnInlineCellUsedRole, true);
                tree->setItemWidget(rowNames, col, new InlinePropNameWidget(cmd, false, tree));
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                const QString valueText = detected
                                              ? trk(QStringLiteral("t_yes_001"),
                                                    QStringLiteral("sí"),
                                                    QStringLiteral("yes"))
                                              : trk(QStringLiteral("t_no_001"),
                                                    QStringLiteral("no"),
                                                    QStringLiteral("no"));
                rowValues->setText(col, valueText);
                rowValues->setTextAlignment(col, Qt::AlignCenter);
                const QString valueColor = detected ? QStringLiteral("#1b8f3a")
                                                    : QStringLiteral("#b00020");
                auto* edit = new QLineEdit(tree);
                edit->setText(valueText);
                edit->setReadOnly(true);
                edit->setMinimumHeight(22);
                edit->setMaximumHeight(22);
                edit->setFont(tree->font());
                edit->setStyleSheet(QStringLiteral("QLineEdit{padding:0 5px; margin:0px; background:#f3f5f7; color:%1;}").arg(valueColor));
                tree->setItemWidget(rowValues, col, wrapInlineCellEditor(edit, tree));
            }
        }
    };
    QVector<QPair<QString, bool>> commandEntries;
    for (const QString& cmd : st.detectedUnixCommands) {
        const QString trimmed = cmd.trimmed();
        if (!trimmed.isEmpty()) {
            commandEntries.push_back({trimmed, true});
        }
    }
    for (const QString& cmd : st.missingUnixCommands) {
        const QString trimmed = cmd.trimmed();
        if (!trimmed.isEmpty()) {
            commandEntries.push_back({trimmed, false});
        }
    }
    if (commandEntries.isEmpty()) {
        commandEntries.push_back({trk(QStringLiteral("t_conn_commands_001"),
                                      QStringLiteral("Comandos"),
                                      QStringLiteral("Commands")),
                                  true});
    }
    appendCommandRows(commandsNode, commandEntries);
    if (st.detectedUnixCommands.isEmpty() && st.missingUnixCommands.isEmpty()) {
        for (int i = 0; i < commandsNode->childCount(); ++i) {
            QTreeWidgetItem* child = commandsNode->child(i);
            if (!child) {
                continue;
            }
            if (child->data(0, kConnPropRowKindRole).toInt() == 2) {
                for (int col = 4; col < tree->columnCount(); ++col) {
                    if (!child->data(col, kConnInlineCellUsedRole).toBool()) {
                        continue;
                    }
                    const QString noneText =
                        trk(QStringLiteral("t_none_001"),
                            QStringLiteral("(ninguno)"),
                            QStringLiteral("(none)"));
                    child->setText(col, noneText);
                    child->setForeground(col, QBrush());
                    auto* edit = new QLineEdit(tree);
                    edit->setText(noneText);
                    edit->setReadOnly(true);
                    edit->setMinimumHeight(22);
                    edit->setMaximumHeight(22);
                    edit->setFont(tree->font());
                    edit->setStyleSheet(QStringLiteral("QLineEdit{padding:0 5px; margin:0px; background:#f3f5f7; color:#5f6b76;}"));
                    tree->setItemWidget(child, col, wrapInlineCellEditor(edit, tree));
                }
                break;
            }
        }
    }
    const bool expandedLegacyDetected = infoChildExpandedById.value(QStringLiteral("syn:detected_commands"), false);
    const bool expandedLegacyMissing = infoChildExpandedById.value(QStringLiteral("syn:missing_commands"), false);
    commandsNode->setExpanded(infoChildExpandedById.value(QStringLiteral("syn:commands"),
                                                          expandedLegacyDetected || expandedLegacyMissing));

    QVector<QPair<QString, QString>> nonImportablePools;
    for (const PoolImportable& pool : st.importablePools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        const QString stateUp = pool.state.trimmed().toUpper();
        const QString actionTxt = pool.action.trimmed();
        if (stateUp == QStringLiteral("ONLINE") && !actionTxt.isEmpty()) {
            continue;
        }
        nonImportablePools.push_back({poolName, pool.reason.trimmed()});
    }
    if (!nonImportablePools.isEmpty()) {
        auto* poolsNode = new QTreeWidgetItem(infoNode);
        poolsNode->setFlags(poolsNode->flags() & ~Qt::ItemIsUserCheckable);
        poolsNode->setData(0, kConnContentNodeRole, true);
        poolsNode->setData(0, kConnIdxRole, connIdx);
        poolsNode->setData(0, kConnStatePartRole, QStringLiteral("syn:non_importable_pools"));
        poolsNode->setText(0, trk(QStringLiteral("t_non_importable_pools_001"),
                                  QStringLiteral("Pools no importables"),
                                  QStringLiteral("Non-importable pools")));
        poolsNode->setExpanded(infoChildExpandedById.value(QStringLiteral("syn:non_importable_pools"), true));
        for (const auto& pool : nonImportablePools) {
            auto* poolItem = new QTreeWidgetItem(poolsNode);
            poolItem->setFlags(poolItem->flags() & ~Qt::ItemIsUserCheckable);
            poolItem->setData(0, kConnContentNodeRole, true);
            poolItem->setData(0, kConnIdxRole, connIdx);
            poolItem->setData(0, kConnStatePartRole,
                              QStringLiteral("syn:non_importable_pool:%1").arg(pool.first.trimmed().toLower()));
            poolItem->setText(0, pool.first);
            const QString reason = pool.second.trimmed().isEmpty() ? QStringLiteral("-") : pool.second.trimmed();
            poolItem->setToolTip(
                0,
                trk(QStringLiteral("t_reason_001"),
                    QStringLiteral("Motivo: %1"),
                    QStringLiteral("Reason: %1"))
                    .arg(reason));
        }
    }
    infoNode->setExpanded(hadInfoNode ? infoNodeWasExpanded : false);
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
    QPointer<QTreeWidget> safeTree(tree);
    const bool poolImported = [&]() -> bool {
        if (connIdx < 0 || connIdx >= m_conns.states.size()) {
            return false;
        }
        const ConnectionRuntimeState& st = m_conns.states[connIdx];
        for (const PoolImported& p : st.importedPools) {
            if (p.pool.trimmed() == poolName.trimmed()) {
                return true;
            }
        }
        return false;
    }();
    auto poolRootTitle = [&]() -> QString {
        // Solo el nombre del pool. Llevaba delante el de la conexión —«Pool Local::fc16»—
        // porque el árbol de destino no la enseñaba en ningún otro sitio; ahora la dice el
        // desplegable de encima del panel y repetirla en cada raíz solo gastaba ancho.
        const bool poolSuspended = isPoolSuspended(connIdx, poolName);
        const QString poolPrefix =
            trk(QStringLiteral("t_tree_pool_prefix_001"),
                QStringLiteral("Pool"),
                QStringLiteral("Pool"));
        if (poolImported) {
            QString title = QStringLiteral("%1 %2").arg(poolPrefix, poolName);
            if (poolSuspended) {
                title += QStringLiteral(" (Suspended)");
            }
            return title;
        }
        const QString stateText = trk(QStringLiteral("t_pool_impable_001"),
                                      QStringLiteral("Importable"),
                                      QStringLiteral("Importable"));
        return QStringLiteral("%1 %2 [%3]").arg(poolPrefix, poolName, stateText);
    };
    auto connectionRootTitle = [&]() -> QString {
        QString connName = (connIdx >= 0 && connIdx < m_conns.profiles.size()) ? m_conns.profiles[connIdx].name : QStringLiteral("?");
        if (connIdx >= 0 && connIdx < m_conns.states.size() && m_conns.states[connIdx].daemonNeedsAttention) {
            connName += QStringLiteral(" (*)");
        }
        const QString daemonBackoff = daemonRpcBackoffTextForConnection(connIdx);
        if (!daemonBackoff.isEmpty()) {
            connName += QStringLiteral(" [%1]").arg(daemonBackoff);
        }
        const QString connPrefix =
            trk(QStringLiteral("t_tree_connection_prefix_001"),
                QStringLiteral("Conexión"),
                QStringLiteral("Connection"));
        return QStringLiteral("%1 %2").arg(connPrefix, connName);
    };
    auto connectionRootColor = [&]() -> QColor {
        return connectionStateRowColor(connIdx);
    };
    auto ensureConnectionRoot = [&]() -> QTreeWidgetItem* {
        QTreeWidget* liveTree = safeTree.data();
        if (!liveTree || !treeGroupsPoolsByConnectionRoots(liveTree)) {
            return nullptr;
        }
        QTreeWidgetItem* connRoot = findConnectionRootItem(liveTree, connIdx);
        bool createdConnRoot = false;
        if (!connRoot) {
            connRoot = new QTreeWidgetItem();
            connRoot->setData(0, kIsConnectionRootRole, true);
            connRoot->setData(0, kConnIdxRole, connIdx);
            connRoot->setData(0, kConnConnectionStableIdRole, connStableIdForIndex(connIdx));
            connRoot->setFlags(connRoot->flags() & ~Qt::ItemIsUserCheckable);
            connRoot->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            liveTree->addTopLevelItem(connRoot);
            connRoot->setExpanded(true);
            createdConnRoot = true;
        }
        connRoot->setText(0, connectionRootTitle());
        connRoot->setData(0, kConnConnectionStableIdRole, connStableIdForIndex(connIdx));
        connRoot->setBackground(0, QBrush(connectionRootColor()));
        connRoot->setToolTip(0, QString());
        QFont connFont = connRoot->font(0);
        connFont.setBold(true);
        if (isConnectionDisconnected(connIdx)) {
            connFont.setItalic(true);
        } else {
            connFont.setItalic(false);
        }
        connRoot->setFont(0, connFont);
        bool hasAuxSections = false;
        for (int i = 0; i < connRoot->childCount(); ++i) {
            const QString sectionId = connRoot->child(i)->data(0, kConnRootSectionRole).toString();
            if (sectionId == QStringLiteral("connection_properties")
                || sectionId == QStringLiteral("connection_info")) {
                hasAuxSections = true;
                break;
            }
        }
        if (createdConnRoot || !hasAuxSections) {
            ensureConnectionRootAuxNodes(liveTree, connRoot, connIdx);
        }
        return connRoot;
    };
    auto addPoolRootOnlyForConnContent = [&]() {
        QTreeWidget* liveTree = safeTree.data();
        if (!liveTree) {
            return;
        }
        if (!options.includePoolRoot) {
            return;
        }
        auto* poolRoot = new QTreeWidgetItem();
        poolRoot->setText(0, poolRootTitle());
        poolRoot->setIcon(0, treeStandardIcon(QStyle::SP_DriveHDIcon));
        {
            QFont f = poolRoot->font(0);
            f.setBold(true);
            poolRoot->setFont(0, f);
        }
        poolRoot->setFlags(poolRoot->flags() & ~Qt::ItemIsUserCheckable);
        poolRoot->setData(0, kIsPoolRootRole, true);
        poolRoot->setData(0, kConnIdxRole, connIdx);
        poolRoot->setData(0, kPoolNameRole, poolName);
        poolRoot->setData(0, kConnConnectionStableIdRole, connStableIdForIndex(connIdx));
        poolRoot->setData(0, kConnPoolGuidRole, QString());
        poolRoot->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        const QString tooltipHtml = cachedPoolStatusTooltipHtml(connIdx, poolName);
        poolRoot->setToolTip(0, tooltipHtml.isEmpty() ? poolRoot->text(0) : tooltipHtml);
        if (QTreeWidgetItem* connRoot = ensureConnectionRoot()) {
            connRoot->addChild(poolRoot);
        } else {
            liveTree->addTopLevelItem(poolRoot);
        }
        poolRoot->setExpanded(true);
    };
    if (!ensureDatasetsLoaded(connIdx, poolName, allowRemoteLoadIfMissing)) {
        if (!safeTree) {
            return;
        }
        addPoolRootOnlyForConnContent();
        return;
    }
    if (!safeTree) {
        return;
    }
    PoolInfo* poolInfo = findPoolInfo(connIdx, poolName);
    if (!poolInfo) {
        if (!safeTree) {
            return;
        }
        addPoolRootOnlyForConnContent();
        return;
    }
    auto ensurePoolAndObjectGuids = [&]() -> bool {
        if (!poolInfo) {
            return false;
        }
        const QString trimmedPool = poolName.trimmed();
        if (trimmedPool.isEmpty() || connIdx < 0 || connIdx >= m_conns.profiles.size()) {
            return false;
        }
        ConnectionProfile p = m_conns.profiles[connIdx];
        const bool daemonReadApiOk =
            connIdx >= 0
            && connIdx < m_conns.states.size()
            && m_conns.states[connIdx].daemonInstalled
            && m_conns.states[connIdx].daemonActive
            && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
        bool changed = false;
        if (poolInfo->key.poolGuid.trimmed().isEmpty()) {
            QString out;
            QString err;
            int rc = -1;
            // Por argv cuando hay daemon: la orden no pasa por ninguna cadena de shell.
            const QStringList cmdDaemonArgv = mwhelpers::argvQt(zfsmgr::commands::requests::poolGuid(trimmedPool.toStdString()));
            if (daemonReadApiOk) {
                (void)runAgentCommand(p, cmdDaemonArgv, 12000, out, err, rc);
            }
            // Unconditional re-look up: runSsh() pumped the event loop, so
            // rebuildConnInfoFor() may have replaced the ConnInfo that owns poolInfo.
            // This used to happen only on the success path, leaving the pointer stale
            // whenever the command failed.
            poolInfo = findPoolInfo(connIdx, poolName);
            if (rc == 0) {
                const QString guid = out.section('\n', 0, 0).trimmed();
                if (!guid.isEmpty() && guid != QStringLiteral("-")) {
                    if (connIdx >= 0 && connIdx < m_conns.states.size()) {
                        m_conns.states[connIdx].poolGuidByName.insert(trimmedPool, guid);
                    }
                    if (poolInfo && poolInfo->key.poolGuid.trimmed().isEmpty()) {
                        poolInfo->key.poolGuid = guid;
                    }
                    changed = true;
                }
            }
        }
        if (!poolInfo) {
            return false;
        }
        QSet<QString> missingNames;
        for (auto it = poolInfo->objectsByFullName.cbegin(); it != poolInfo->objectsByFullName.cend(); ++it) {
            if (it->kind == DSKind::Unknown) {
                continue;
            }
            const QString guid = it->runtime.properties.value(QStringLiteral("guid")).trimmed();
            if (guid.isEmpty() || guid == QStringLiteral("-")) {
                missingNames.insert(it.key());
            }
        }
        if (!missingNames.isEmpty()) {
            QString out;
            QString err;
            int rc = -1;
            // Por argv cuando hay daemon: la orden no pasa por ninguna cadena de shell.
            const QStringList cmdDaemonArgv = mwhelpers::argvQt(zfsmgr::commands::requests::guidMap(trimmedPool.toStdString()));
            if (daemonReadApiOk) {
                (void)runAgentCommand(p, cmdDaemonArgv, 25000, out, err, rc);
            }
            if (rc == 0) {
                const QString cacheKey = datasetCacheKey(connIdx, trimmedPool);
                PoolDatasetCache* cache = nullptr;
                auto cacheIt = m_conns.poolDatasetCache.find(cacheKey);
                if (cacheIt != m_conns.poolDatasetCache.end()) {
                    cache = &cacheIt.value();
                }
                const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
                for (const QString& line : lines) {
                    const QStringList cols = line.split('\t');
                    if (cols.size() < 2) {
                        continue;
                    }
                    const QString name = cols[0].trimmed();
                    const QString guid = cols[1].trimmed();
                    if (name.isEmpty() || guid.isEmpty() || guid == QStringLiteral("-")) {
                        continue;
                    }
                    if (cache) {
                        cache->objectGuidByName.insert(name, guid);
                    }
                    if (DSInfo* ds = findDsInfo(connIdx, trimmedPool, name)) {
                        ds->runtime.properties.insert(QStringLiteral("guid"), guid);
                    }
                }
                changed = true;
            }
        }
        if (changed) {
            rebuildConnInfoFor(connIdx);
        }
        // Unconditional re-look up: the runSsh() above pumped the event loop, so the
        // ConnInfo owning poolInfo may have been replaced even when nothing changed.
        poolInfo = findPoolInfo(connIdx, poolName);
        if (!poolInfo || poolInfo->key.poolGuid.trimmed().isEmpty()) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Abort tree build: missing stable pool GUID conn=%1 pool=%2")
                       .arg(QString::number(connIdx), poolName));
            return false;
        }
        for (auto it = poolInfo->objectsByFullName.cbegin(); it != poolInfo->objectsByFullName.cend(); ++it) {
            if (it->kind == DSKind::Unknown) {
                continue;
            }
            const QString guid = it->runtime.properties.value(QStringLiteral("guid")).trimmed();
            if (guid.isEmpty() || guid == QStringLiteral("-")) {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("Abort tree build: missing stable object GUID conn=%1 pool=%2 object=%3")
                           .arg(QString::number(connIdx), poolName, it.key()));
                return false;
            }
        }
        return true;
    };
    if (!ensurePoolAndObjectGuids()) {
        return;
    }
    if (!safeTree) {
        return;
    }
    // Requisito funcional: antes de pintar datasets en el árbol, cargar permisos
    // dataset por dataset (zfs allow no ofrece listado global de todo el pool).
    if (!isWindowsConnection(connIdx)) {
        QStringList permissionObjectsToPreload;
        permissionObjectsToPreload.reserve(poolInfo->objectsByFullName.size());
        for (auto it = poolInfo->objectsByFullName.cbegin(); it != poolInfo->objectsByFullName.cend(); ++it) {
            if (it->kind == DSKind::Snapshot) {
                continue;
            }
            const QString objectName = it.key().trimmed();
            if (!objectName.isEmpty()) {
                permissionObjectsToPreload.push_back(objectName);
            }
        }
        if (!ensureDatasetPermissionsLoadedBatch(connIdx, poolName, permissionObjectsToPreload)) {
            for (const QString& objectName : permissionObjectsToPreload) {
                ensureDatasetPermissionsLoaded(connIdx, poolName, objectName);
            }
        }
        if (!safeTree) {
            return;
        }
        poolInfo = findPoolInfo(connIdx, poolName);
        if (!poolInfo) {
            return;
        }
    }
    auto objectGuidFromCache = [this, connIdx, &poolName](const QString& objectName) -> QString {
        const QVector<DatasetPropCacheRow> rows =
            datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
        for (const DatasetPropCacheRow& row : rows) {
            if (row.prop.trimmed().compare(QStringLiteral("guid"), Qt::CaseInsensitive) == 0) {
                return row.value.trimmed();
            }
        }
        return QString();
    };
    // By value, not by reference into poolInfo->objectsByFullName: effectiveMountPath()
    // below reaches runSsh() on Windows connections, and rebuildConnInfoFor() can
    // replace the whole ConnInfo while the event loop is pumped, which would dangle
    // both this parameter and the caller's iterator.
    auto buildDatasetItem = [&](const DSInfo dsInfo, auto&& buildDatasetItemRef) -> QTreeWidgetItem* {
        auto* item = new QTreeWidgetItem();
        const QString fullName = dsInfo.key.fullName.trimmed();
        const QString displayName = fullName.contains('/')
                                        ? fullName.section('/', -1, -1)
                                        : fullName;
        // Sin «Dataset» delante: ver el comentario de `applySnapshotVisualState()`.
        item->setText(0, displayName);
        item->setIcon(0, treeStandardIcon(QStyle::SP_DirIcon));
        {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        // Las instantáneas automáticas se ven SIEMPRE (punto 6 del backlog).
        //
        // Aquí se filtraban cuando la casilla estaba desmarcada. La casilla se ha retirado,
        // y ocultarlas nunca fue buena idea: la lista decía «(ninguno)» sobre un dataset
        // que tenía copias, solo que programadas.
        const QStringList snaps = dsInfo.runtime.directSnapshots;
        item->setText(1, snaps.isEmpty() ? QString()
                                         : trk(QStringLiteral("t_none_001"),
                                               QStringLiteral("(ninguno)"),
                                               QStringLiteral("(none)")));
        item->setData(1, Qt::UserRole, QString());
        item->setData(0, Qt::UserRole, fullName);
        item->setData(1, kSnapshotListRole, snaps);
        item->setData(0, kConnIdxRole, connIdx);
        item->setData(0, kPoolNameRole, poolName);
        item->setData(0, kConnConnectionStableIdRole, connStableIdForIndex(connIdx));
        item->setData(0, kConnPoolGuidRole, poolInfo->key.poolGuid.trimmed());
        QString datasetGuid = dsInfo.runtime.properties.value(QStringLiteral("guid")).trimmed();
        if (datasetGuid.isEmpty()) {
            datasetGuid = objectGuidFromCache(fullName);
        }
        item->setData(0, kConnDatasetGuidRole, datasetGuid);
        if (datasetGuid.isEmpty()) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("GUID fallback: dataset node without GUID conn=%1 pool=%2 ds=%3")
                       .arg(QString::number(connIdx), poolName, fullName));
        }
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const QString mounted = dsInfo.runtime.properties.value(QStringLiteral("mounted")).trimmed().toLower();
        // El estado de montaje sale de DOS sitios, y hay que mirar los dos.
        //
        // `mounted` viene del listado de datasets, que se cachea y NO se rehace al
        // refrescar la conexión. Si el dataset se monta por fuera —o desde otra ventana—
        // el árbol se queda con el valor de la última vez que se listó. Visto en Windows:
        // el disco montado y visible en el Explorador, las propiedades diciendo
        // montados=yes, y el árbol insistiendo en «Contenido no disponible: el dataset no
        // está montado».
        //
        // El refresco sí consulta los montajes de verdad (--dump-zfs-mount) y los guarda
        // en el estado de la conexión, así que esa lista manda cuando dice que sí.
        bool isMounted = (mounted == QStringLiteral("yes")
                          || mounted == QStringLiteral("on")
                          || mounted == QStringLiteral("true")
                          || mounted == QStringLiteral("1"));
        if (!isMounted && connIdx >= 0 && connIdx < m_conns.states.size()) {
            for (const QPair<QString, QString>& row : m_conns.states[connIdx].mountedDatasets) {
                if (row.first.trimmed() == fullName) {
                    isMounted = true;
                    break;
                }
            }
        }
        item->setCheckState(2, isMounted ? Qt::Checked : Qt::Unchecked);
        const QString mountpoint = dsInfo.runtime.properties.value(QStringLiteral("mountpoint")).trimmed();
        const QString effectiveMp = effectiveMountPath(connIdx, poolName, fullName, mountpoint, mounted);
        item->setText(3, effectiveMp.isEmpty() ? mountpoint : effectiveMp);
        DatasetPermissionsCacheEntry perms = dsInfo.permissionsCache;
        if (const DatasetPermissionsCacheEntry* cachedPerms =
                datasetPermissionsEntry(connIdx, poolName, fullName);
            cachedPerms && cachedPerms->loaded) {
            perms = *cachedPerms;
        }
        // El nodo «@» con los snapshots agrupados vivía aquí, y debajo de cada uno otro
        // nivel más. Eran cuatro despliegues por encima de un dataset para llegar a una
        // instantánea, en la misma columna donde se navegan los datasets. Ahora es la
        // pestaña «Snapshots» del detalle, con sus mismos grupos y sus mismas acciones.
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
    // Copy the name list: buildDatasetItem() can yield, and iterating a member of
    // *poolInfo across that would dangle if the ConnInfo gets rebuilt.
    const QStringList rootObjectNames = poolInfo->rootObjectNames;
    for (const QString& rootName : rootObjectNames) {
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
        {
            QFont f = poolRoot->font(0);
            f.setBold(true);
            poolRoot->setFont(0, f);
        }
        poolRoot->setFlags(poolRoot->flags() & ~Qt::ItemIsUserCheckable);
        poolRoot->setData(0, kIsPoolRootRole, true);
        poolRoot->setData(0, kConnIdxRole, connIdx);
        poolRoot->setData(0, kPoolNameRole, poolName);
        poolRoot->setData(0, kConnConnectionStableIdRole, connStableIdForIndex(connIdx));
        poolRoot->setData(0, kConnPoolGuidRole, poolInfo->key.poolGuid.trimmed());
        poolRoot->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        const QString tooltipHtml = cachedPoolStatusTooltipHtml(connIdx, poolName);
        poolRoot->setToolTip(0, tooltipHtml.isEmpty() ? poolRoot->text(0) : tooltipHtml);
        QTreeWidgetItem* mergedRootDataset = nullptr;
        QVector<QTreeWidgetItem*> remainingTopLevelItems;
        for (QTreeWidgetItem* top : logicalTopLevelItems) {
            if (top) {
                if (!mergedRootDataset
                    && top->data(0, Qt::UserRole).toString().trimmed().compare(poolName, Qt::CaseInsensitive) == 0) {
                    mergedRootDataset = top;
                } else {
                    remainingTopLevelItems.push_back(top);
                }
            }
        }
        if (mergedRootDataset) {
            poolRoot->setFlags(poolRoot->flags() | Qt::ItemIsUserCheckable);
            poolRoot->setData(0, Qt::UserRole, mergedRootDataset->data(0, Qt::UserRole));
            poolRoot->setData(1, Qt::UserRole, mergedRootDataset->data(1, Qt::UserRole));
            poolRoot->setData(1, kSnapshotListRole, mergedRootDataset->data(1, kSnapshotListRole));
            poolRoot->setData(0, kConnDatasetGuidRole, mergedRootDataset->data(0, kConnDatasetGuidRole));
            poolRoot->setCheckState(2, mergedRootDataset->checkState(2));
            poolRoot->setText(1, mergedRootDataset->text(1));
            poolRoot->setText(3, mergedRootDataset->text(3));
            while (mergedRootDataset->childCount() > 0) {
                poolRoot->addChild(mergedRootDataset->takeChild(0));
            }
            delete mergedRootDataset;
        }
        for (QTreeWidgetItem* top : remainingTopLevelItems) {
            if (top) {
                poolRoot->addChild(top);
            }
        }
        if (QTreeWidgetItem* connRoot = ensureConnectionRoot()) {
            connRoot->addChild(poolRoot);
        } else {
            tree->addTopLevelItem(poolRoot);
        }
    } else {
        for (QTreeWidgetItem* top : logicalTopLevelItems) {
            if (top) {
                tree->addTopLevelItem(top);
            }
        }
    }
}

// Enraíza el árbol en un pool —o en un dataset de dentro— en lugar de enseñar la
// conexión entera. Es lo que hace el desplegable de pool de cada panel.
void MainWindow::appendPaneTreeRootedAtPool(QTreeWidget* tree, int connIdx,
                                            const QString& poolName,
                                            const QString& rootDataset,
                                            const QString& displayRoot) {
    if (!tree || connIdx < 0 || connIdx >= m_conns.profiles.size() || poolName.trimmed().isEmpty()) {
        return;
    }

    const QString trimmedRoot = rootDataset.trimmed();
    const QString trimmedPool = poolName.trimmed();
    const bool isPoolRoot = (trimmedRoot.compare(trimmedPool, Qt::CaseInsensitive) == 0);

    // Build using the same code path as the main tree so node structure is identical
    const DatasetTreeRenderOptions opts =
        datasetTreeRenderOptionsForTree(tree, DatasetTreeContext::ConnectionContent);
    appendDatasetTreeForPool(tree, connIdx, poolName, DatasetTreeContext::ConnectionContent, opts,
                             false);

    if (isPoolRoot) {
        // Find the pool root item and mark/rename it as split root
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (item->data(0, kIsPoolRootRole).toBool()) {
                item->setData(0, kIsSplitRootRole, true);
                item->setText(0, displayRoot);
                break;
            }
        }
    } else {
        // Dataset-rooted: find the matching dataset item and promote it to top level
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findDataset;
        findDataset = [&](QTreeWidgetItem* parent) -> QTreeWidgetItem* {
            for (int i = 0; i < parent->childCount(); ++i) {
                QTreeWidgetItem* child = parent->child(i);
                if (child->data(0, Qt::UserRole).toString().trimmed().compare(
                        trimmedRoot, Qt::CaseInsensitive) == 0) {
                    return child;
                }
                if (QTreeWidgetItem* found = findDataset(child)) {
                    return found;
                }
            }
            return nullptr;
        };

        QTreeWidgetItem* datasetItem = nullptr;
        for (int i = 0; i < tree->topLevelItemCount() && !datasetItem; ++i) {
            QTreeWidgetItem* top = tree->topLevelItem(i);
            if (top->data(0, Qt::UserRole).toString().trimmed().compare(
                    trimmedRoot, Qt::CaseInsensitive) == 0) {
                datasetItem = top;
            } else {
                datasetItem = findDataset(top);
            }
        }

        if (datasetItem) {
            // Detach from parent before clearing the tree
            if (QTreeWidgetItem* parent = datasetItem->parent()) {
                parent->takeChild(parent->indexOfChild(datasetItem));
            } else {
                tree->takeTopLevelItem(tree->indexOfTopLevelItem(datasetItem));
            }
            datasetItem->setData(0, kIsSplitRootRole, true);
            datasetItem->setText(0, displayRoot);
            tree->clear();
            tree->addTopLevelItem(datasetItem);
            datasetItem->setExpanded(true);
        }
    }
}

void MainWindow::attachDatasetTreeSnapshotCombos(QTreeWidget* tree, DatasetTreeContext side) {
    Q_UNUSED(tree);
    Q_UNUSED(side);
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
        const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), poolName);
        restoreConnContentTreeState(tree, token);
    }

    applyDebugNodeIdsToTree(tree);

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


void MainWindow::onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, DatasetTreeContext side) {
    if (!tree || !item || m_loadingDatasetTrees || actionsLocked()) {
        return;
    }
    if (side == DatasetTreeContext::ConnectionContent && col >= 4) {
        if (item->data(0, kConnRootInlineFieldRole).toBool()) {
            const QString fieldKey = item->data(col, kConnRootInlineFieldRole).toString().trimmed();
            if (fieldKey.isEmpty() || !item->data(col, kConnRootInlineEditableRole).toBool()) {
                return;
            }
            int connIdx = item->data(0, kConnIdxRole).toInt();
            if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
                QTreeWidgetItem* p = item->parent();
                while (p && (connIdx < 0 || connIdx >= m_conns.profiles.size())) {
                    connIdx = p->data(0, kConnIdxRole).toInt();
                    p = p->parent();
                }
            }
            if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
                return;
            }
            QString requestedValue = item->data(col, kConnRootInlineRawValueRole).toString();
            if (requestedValue.isNull()) {
                requestedValue = item->text(col);
            }
            QString normalizedValue;
            QString errorText;
            if (!applyConnectionInlineFieldValue(connIdx,
                                                 fieldKey,
                                                 requestedValue,
                                                 &normalizedValue,
                                                 &errorText)) {
                const QString fallbackRawValue = [this, connIdx, fieldKey]() -> QString {
                    const ConnectionProfile cp = m_conns.profiles[connIdx];
                    const QString k = fieldKey.toLower();
                    if (k == QStringLiteral("name")) return cp.name;
                    if (k == QStringLiteral("machine_uid")) return cp.machineUid;
                    if (k == QStringLiteral("conn_type")) return cp.connType;
                    if (k == QStringLiteral("os_type")) return cp.osType;
                    if (k == QStringLiteral("host")) return cp.host;
                    if (k == QStringLiteral("port")) return QString::number(cp.port);
                    if (k == QStringLiteral("username")) return cp.username;
                    if (k == QStringLiteral("password")) return cp.password;
                    if (k == QStringLiteral("key_path")) return cp.keyPath;
                    if (k == QStringLiteral("use_sudo")) return cp.useSudo ? QStringLiteral("true") : QStringLiteral("false");
                    return QString();
                }();
                const bool isPassword = (fieldKey.compare(QStringLiteral("password"), Qt::CaseInsensitive) == 0);
                const QString fallbackDisplay = isPassword
                                                    ? (fallbackRawValue.isEmpty() ? QString() : QStringLiteral("********"))
                                                    : fallbackRawValue;
                const QSignalBlocker blocker(tree);
                item->setData(col, kConnRootInlineRawValueRole, fallbackRawValue);
                item->setText(col, fallbackDisplay);
                if (QWidget* host = tree->itemWidget(item, col)) {
                    if (QLineEdit* edit = host->findChild<QLineEdit*>()) {
                        edit->setText(fallbackRawValue);
                    } else if (QCheckBox* check = host->findChild<QCheckBox*>()) {
                        const QString v = fallbackRawValue.trimmed().toLower();
                        const bool checked = (v == QStringLiteral("1")
                                              || v == QStringLiteral("on")
                                              || v == QStringLiteral("yes")
                                              || v == QStringLiteral("true")
                                              || v == QStringLiteral("si")
                                              || v == QStringLiteral("sí"));
                        check->setChecked(checked);
                    }
                }
                if (!errorText.trimmed().isEmpty()) {
                    appLog(QStringLiteral("ERROR"),
                           QStringLiteral("No se pudo guardar '%1' para la conexión %2: %3")
                               .arg(fieldKey,
                                    m_conns.profiles[connIdx].name.trimmed().isEmpty()
                                        ? m_conns.profiles[connIdx].id.trimmed()
                                        : m_conns.profiles[connIdx].name.trimmed(),
                                    errorText));
                }
                return;
            }
            {
                const bool isPassword = (fieldKey.compare(QStringLiteral("password"), Qt::CaseInsensitive) == 0);
                const QString displayValue = isPassword
                                                 ? (normalizedValue.isEmpty() ? QString() : QStringLiteral("********"))
                                                 : normalizedValue;
                const QSignalBlocker blocker(tree);
                item->setData(col, kConnRootInlineRawValueRole, normalizedValue);
                item->setText(col, displayValue);
                if (QWidget* host = tree->itemWidget(item, col)) {
                    if (QLineEdit* edit = host->findChild<QLineEdit*>()) {
                        edit->setText(normalizedValue);
                    } else if (QCheckBox* check = host->findChild<QCheckBox*>()) {
                        const QString v = normalizedValue.trimmed().toLower();
                        const bool checked = (v == QStringLiteral("1")
                                              || v == QStringLiteral("on")
                                              || v == QStringLiteral("yes")
                                              || v == QStringLiteral("true")
                                              || v == QStringLiteral("si")
                                              || v == QStringLiteral("sí"));
                        check->setChecked(checked);
                    }
                }
            }
            if (QTreeWidgetItem* connRoot = findConnectionRootItem(tree, connIdx)) {
                QString connName = m_conns.profiles[connIdx].name.trimmed().isEmpty()
                                       ? m_conns.profiles[connIdx].id.trimmed()
                                       : m_conns.profiles[connIdx].name.trimmed();
                if (connIdx >= 0 && connIdx < m_conns.states.size() && m_conns.states[connIdx].daemonNeedsAttention) {
                    connName += QStringLiteral(" (*)");
                }
                const QString connPrefix =
                    trk(QStringLiteral("t_tree_connection_prefix_001"),
                        QStringLiteral("Conexión"),
                        QStringLiteral("Connection"));
                connRoot->setText(0, QStringLiteral("%1 %2").arg(connPrefix, connName));
                connRoot->setToolTip(0, QString());
                ensureConnectionRootAuxNodes(tree, connRoot, connIdx);
                applyDebugNodeIdsToTree(tree);
            }
            updateConnectionDetailTitlesForCurrentSelection();
            updateConnectionActionsState();
            return;
        }
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
                                  .arg(connToken(owner->data(0, kConnIdxRole).toInt()),
                                       owner->data(0, kPoolNameRole).toString());
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
    const QString ds = item->data(0, Qt::UserRole).toString();
    if (item->data(0, kConnPropRowRole).toBool()
        || (item->data(0, kIsPoolRootRole).toBool() && ds.trimmed().isEmpty())) {
        return;
    }
    if (ds.isEmpty()) {
        return;
    }
    const Qt::CheckState desired = item->checkState(2);
    QString token;
    if (side == DatasetTreeContext::Origin) {
        if (m_connActionOrigin.valid) {
            token = QStringLiteral("%1::%2").arg(connToken(m_connActionOrigin.connIdx), m_connActionOrigin.poolName);
        }
    } else if (side == DatasetTreeContext::Destination) {
        if (m_connActionDest.valid) {
            token = QStringLiteral("%1::%2").arg(connToken(m_connActionDest.connIdx), m_connActionDest.poolName);
        }
    } else if (side == DatasetTreeContext::ConnectionContent) {
        const int itemConnIdx = item->data(0, kConnIdxRole).toInt();
        const QString itemPool = item->data(0, kPoolNameRole).toString();
        if (itemConnIdx >= 0 && !itemPool.isEmpty()) {
            token = QStringLiteral("%1::%2").arg(connToken(itemConnIdx), itemPool);
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
    ctx.connIdx = connIdxFromToken(token.left(sep));
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
                      QStringLiteral("Mounted"))
                : trk(QStringLiteral("t_desmontado001"),
                      QStringLiteral("Desmontado"),
                      QStringLiteral("Unmounted"));

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
                      QStringLiteral("Mounted"))
                : trk(QStringLiteral("t_desmontado001"),
                      QStringLiteral("Desmontado"),
                      QStringLiteral("Unmounted"));

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
    auto fillCtxFromSideTree = [&](DatasetSelectionContext& ctx, const QTreeWidget* tree) {
        if (!tree || ctx.valid) {
            return;
        }
        QTreeWidgetItem* item = tree->currentItem();
        if (!item) {
            const QList<QTreeWidgetItem*> selected = tree->selectedItems();
            if (!selected.isEmpty()) {
                item = selected.first();
            }
        }
        while (item && item->data(0, Qt::UserRole).toString().trimmed().isEmpty() && item->parent()) {
            item = item->parent();
        }
        if (!item) {
            return;
        }
        QString itemDataset = item->data(0, Qt::UserRole).toString().trimmed();
        const QString itemPool = item->data(0, kPoolNameRole).toString().trimmed();
        if (itemDataset.isEmpty() && item->data(0, kIsPoolRootRole).toBool()) {
            itemDataset = itemPool;
        }
        const int itemConnIdx = item->data(0, kConnIdxRole).toInt();
        if (itemConnIdx < 0 || itemPool.isEmpty() || itemDataset.isEmpty()) {
            return;
        }
        ctx.valid = true;
        ctx.connIdx = itemConnIdx;
        ctx.poolName = itemPool;
        ctx.datasetName = itemDataset;
        ctx.snapshotName = item->data(1, Qt::UserRole).toString().trimmed();
    };
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
        fillCtxFromSideTree(ctx, m_topDatasetTreeWidget ? m_topDatasetTreeWidget->tree() : nullptr);
        ctx = normalizeDatasetSelectionContext(
            ctx,
            m_topDatasetTreeWidget ? m_topDatasetTreeWidget->tree() : nullptr);
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
    fillCtxFromSideTree(ctx, m_bottomDatasetTreeWidget ? m_bottomDatasetTreeWidget->tree() : nullptr);
    ctx = normalizeDatasetSelectionContext(
        ctx,
        m_bottomDatasetTreeWidget ? m_bottomDatasetTreeWidget->tree() : nullptr);
    setConnectionDestinationSelection(ctx);
    refreshDatasetProperties(QStringLiteral("dest"));
}


static QString userExpandedTreePrefix(QTreeWidget* tree) {
    if (!tree || !tree->property("zfsmgr.isSplitTree").toBool()) {
        return QStringLiteral("main");
    }
    const QString splitKey = tree->property("zfsmgr.splitTreeKey").toString().trimmed();
    return QStringLiteral("split:") + (splitKey.isEmpty() ? QStringLiteral("?") : splitKey);
}

QString MainWindow::userExpandedKey(QTreeWidget* tree, QTreeWidgetItem* item) const {
    const QString path = connContentNodeStablePath(item).trimmed();
    if (path.isEmpty()) {
        return QString();
    }
    return userExpandedTreePrefix(tree) + QStringLiteral(":") + path;
}

void MainWindow::applyUserExpandedState(QTreeWidget* tree) {
    if (!tree || m_userNodeExpanded.isEmpty()) {
        return;
    }
    const QString prefix = userExpandedTreePrefix(tree) + QStringLiteral(":");
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString path = connContentNodeStablePath(n).trimmed();
        if (!path.isEmpty()) {
            const auto it = m_userNodeExpanded.constFind(prefix + path);
            if (it != m_userNodeExpanded.cend()) {
                n->setExpanded(it.value());
            }
        }
        for (int i = 0; i < n->childCount(); ++i) {
            rec(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        rec(tree->topLevelItem(i));
    }
}

void MainWindow::loadUserExpandedState() {
    m_userNodeExpanded.clear();
    const QJsonObject root = m_conns.store.loadConfigJson();
    const QJsonObject map = root[QStringLiteral("tree_expanded")].toObject();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.value().isBool()) {
            m_userNodeExpanded.insert(it.key(), it.value().toBool());
        }
    }
}

void MainWindow::saveUserExpandedState() {
    if (!m_userExpandedSaveTimer) {
        m_userExpandedSaveTimer = new QTimer(this);
        m_userExpandedSaveTimer->setSingleShot(true);
        connect(m_userExpandedSaveTimer, &QTimer::timeout, this, [this]() {
            QJsonObject root = m_conns.store.loadConfigJson();
            QJsonObject map;
            for (auto it = m_userNodeExpanded.cbegin(); it != m_userNodeExpanded.cend(); ++it) {
                map.insert(it.key(), QJsonValue(it.value()));
            }
            root[QStringLiteral("tree_expanded")] = map;
            m_conns.store.saveConfigJson(root);
        });
    }
    m_userExpandedSaveTimer->start(500);
}
