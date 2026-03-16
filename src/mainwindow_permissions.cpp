#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QCheckBox>
#include <QColor>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSet>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWidget>

namespace {
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;

constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kConnPermissionsNodeRole = Qt::UserRole + 25;
constexpr int kConnPermissionsKindRole = Qt::UserRole + 26;
constexpr int kConnPermissionsScopeRole = Qt::UserRole + 27;
constexpr int kConnPermissionsTargetTypeRole = Qt::UserRole + 28;
constexpr int kConnPermissionsTargetNameRole = Qt::UserRole + 29;
constexpr int kConnPermissionsEntryNameRole = Qt::UserRole + 30;
constexpr int kConnPermissionsPendingRole = Qt::UserRole + 31;
constexpr int kConnPropRowRole = Qt::UserRole + 13;
constexpr int kConnPropRowKindRole = Qt::UserRole + 16;
constexpr int kConnInlineCellUsedRole = Qt::UserRole + 32;

enum class PermissionsSection {
    None,
    Local,
    Descendant,
    LocalDescendant,
    Create,
    Sets,
};

struct ParsedPermissionGrant {
    QString scope;
    QString targetType;
    QString targetName;
    QStringList permissions;
};

struct ParsedPermissionSet {
    QString name;
    QStringList permissions;
};

struct ParsedPermissionsEntry {
    QStringList createPermissions;
    QVector<ParsedPermissionGrant> localGrants;
    QVector<ParsedPermissionGrant> descendantGrants;
    QVector<ParsedPermissionGrant> localDescendantGrants;
    QVector<ParsedPermissionSet> permissionSets;
};

QStringList splitPermissionTokens(const QString& raw) {
    QStringList out;
    QSet<QString> seen;
    for (const QString& part : raw.split(',', Qt::SkipEmptyParts)) {
        const QString t = part.trimmed();
        const QString k = t.toLower();
        if (t.isEmpty() || seen.contains(k)) {
            continue;
        }
        seen.insert(k);
        out.push_back(t);
    }
    return out;
}

QString remainingAfterTokens(const QStringList& parts, int skipCount) {
    if (parts.size() <= skipCount) {
        return QString();
    }
    QStringList rest;
    for (int i = skipCount; i < parts.size(); ++i) {
        rest.push_back(parts.at(i));
    }
    return rest.join(' ');
}

QString accountListCommand(const QString& kind, const ConnectionProfile& p, const QString& osLine) {
    const QString merged = (p.osType + QStringLiteral(" ") + osLine).toLower();
    if (merged.contains(QStringLiteral("darwin")) || merged.contains(QStringLiteral("mac"))) {
        if (kind == QStringLiteral("user")) {
            return QStringLiteral("(dscl . -list /Users 2>/dev/null || cut -d: -f1 /etc/passwd 2>/dev/null)");
        }
        return QStringLiteral("(dscl . -list /Groups 2>/dev/null || cut -d: -f1 /etc/group 2>/dev/null)");
    }
    if (kind == QStringLiteral("user")) {
        return QStringLiteral("((getent passwd 2>/dev/null || cat /etc/passwd 2>/dev/null) | cut -d: -f1)");
    }
    return QStringLiteral("((getent group 2>/dev/null || cat /etc/group 2>/dev/null) | cut -d: -f1)");
}

ParsedPermissionsEntry parsePermissionsOutput(const QString& out) {
    ParsedPermissionsEntry entry;
    PermissionsSection section = PermissionsSection::None;

    for (const QString& rawLine : out.split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1Char('-'))) {
            continue;
        }
        const QString lower = line.toLower();
        if (lower.contains(QStringLiteral("local permissions:"))) {
            section = PermissionsSection::Local;
            continue;
        }
        if (lower.contains(QStringLiteral("descendent permissions:"))
            || lower.contains(QStringLiteral("descendant permissions:"))) {
            if (lower.contains(QStringLiteral("local+"))) {
                section = PermissionsSection::LocalDescendant;
            } else {
                section = PermissionsSection::Descendant;
            }
            continue;
        }
        if (lower.contains(QStringLiteral("create time permissions:"))) {
            section = PermissionsSection::Create;
            continue;
        }
        if (lower.contains(QStringLiteral("permission sets:"))) {
            section = PermissionsSection::Sets;
            continue;
        }

        if (section == PermissionsSection::Create) {
            for (const QString& token : splitPermissionTokens(line)) {
                if (!entry.createPermissions.contains(token, Qt::CaseInsensitive)) {
                    entry.createPermissions.push_back(token);
                }
            }
            continue;
        }

        if (section == PermissionsSection::Sets) {
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (parts.isEmpty()) {
                continue;
            }
            ParsedPermissionSet set;
            set.name = parts.first().trimmed();
            if (!set.name.startsWith(QLatin1Char('@'))) {
                continue;
            }
            set.permissions = splitPermissionTokens(remainingAfterTokens(parts, 1));
            entry.permissionSets.push_back(set);
            continue;
        }

        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            continue;
        }
        ParsedPermissionGrant grant;
        if (parts.first().compare(QStringLiteral("everyone"), Qt::CaseInsensitive) == 0) {
            grant.targetType = QStringLiteral("everyone");
            grant.targetName = QStringLiteral("everyone");
            grant.permissions = splitPermissionTokens(remainingAfterTokens(parts, 1));
        } else if (parts.size() >= 3
                   && (parts.first().compare(QStringLiteral("user"), Qt::CaseInsensitive) == 0
                       || parts.first().compare(QStringLiteral("group"), Qt::CaseInsensitive) == 0)) {
            grant.targetType = parts.first().trimmed().toLower();
            grant.targetName = parts.at(1).trimmed();
            grant.permissions = splitPermissionTokens(remainingAfterTokens(parts, 2));
        }
        if (grant.targetType.isEmpty() || grant.permissions.isEmpty()) {
            continue;
        }
        switch (section) {
        case PermissionsSection::Local:
            grant.scope = QStringLiteral("local");
            entry.localGrants.push_back(grant);
            break;
        case PermissionsSection::Descendant:
            grant.scope = QStringLiteral("descendant");
            entry.descendantGrants.push_back(grant);
            break;
        case PermissionsSection::LocalDescendant:
            grant.scope = QStringLiteral("local_descendant");
            entry.localDescendantGrants.push_back(grant);
            break;
        default:
            break;
        }
    }

    entry.createPermissions.removeDuplicates();
    return entry;
}

QString extractPermissionsOutputForDataset(const QString& out, const QString& datasetName) {
    const QString wanted = datasetName.trimmed();
    if (wanted.isEmpty()) {
        return out;
    }
    const QRegularExpression headerRe(
        QStringLiteral("^\\s*-+\\s*Permissions on\\s+([^\\s]+)\\s*-+\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList lines = out.split(QLatin1Char('\n'));
    bool sawHeader = false;
    bool inWantedBlock = false;
    QStringList filtered;
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        const QRegularExpressionMatch m = headerRe.match(line);
        if (m.hasMatch()) {
            sawHeader = true;
            inWantedBlock = (m.captured(1).trimmed() == wanted);
            continue;
        }
        if (inWantedBlock) {
            filtered.push_back(rawLine);
        }
    }
    if (!sawHeader) {
        return out;
    }
    return filtered.join(QLatin1Char('\n'));
}

QTreeWidgetItem* ensurePermissionsSectionNode(QTreeWidgetItem* parent,
                                              const QString& title,
                                              const QString& kind,
                                              int count,
                                              int connIdx,
                                              const QString& poolName) {
    auto* node = new QTreeWidgetItem(parent);
    node->setText(0, QStringLiteral("%1 (%2)").arg(title).arg(count));
    node->setData(0, kConnPermissionsNodeRole, true);
    node->setData(0, kConnPermissionsKindRole, kind);
    node->setData(0, kConnIdxRole, connIdx);
    node->setData(0, kPoolNameRole, poolName);
    node->setExpanded(false);
    node->setFlags(node->flags() & ~Qt::ItemIsUserCheckable);
    return node;
}

QString targetLabel(const QString& targetType, const QString& targetName) {
    if (targetType == QStringLiteral("user")) {
        return QStringLiteral("Usuario %1").arg(targetName);
    }
    if (targetType == QStringLiteral("group")) {
        return QStringLiteral("Grupo %1").arg(targetName);
    }
    return QStringLiteral("everyone");
}

QString grantScopeLabel(const QString& scope) {
    const QString s = scope.trimmed().toLower();
    if (s == QStringLiteral("local")) {
        return QStringLiteral("Local");
    }
    if (s == QStringLiteral("descendant")) {
        return QStringLiteral("Desc.");
    }
    return QStringLiteral("Local y Desc.");
}

void styleInlinePermissionCheckHost(QWidget* host, const QTreeWidget* tree) {
    if (!host || !tree) {
        return;
    }
    const QPalette pal = tree->palette();
    const QColor vBorder = pal.color(QPalette::Mid).darker(118);
    const QColor hBorder = pal.color(QPalette::Mid).darker(108);
    const QColor bg = pal.color(QPalette::Base);
    host->setAutoFillBackground(true);
    host->setStyleSheet(QStringLiteral(
        "background: %1;"
        "border-left: 1px solid %2;"
        "border-right: 1px solid %2;"
        "border-bottom: 1px solid %3;")
                            .arg(bg.name(QColor::HexRgb),
                                 vBorder.name(QColor::HexRgb),
                                 hBorder.name(QColor::HexRgb)));
}

QFont inlinePermissionLabelFont(const QTreeWidget* tree) {
    QFont f = tree ? tree->font() : QFont();
    const qreal current = f.pointSizeF() > 0.0 ? f.pointSizeF() : 10.0;
    f.setPointSizeF(std::max<qreal>(7.0, current - 1.0));
    return f;
}

QString permissionNodeStableId(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    const QString kind = node->data(0, kConnPermissionsKindRole).toString();
    if (kind == QStringLiteral("grant") || kind == QStringLiteral("grant_perm")) {
        return QStringLiteral("%1|%2|%3|%4")
            .arg(kind,
                 node->data(0, kConnPermissionsScopeRole).toString(),
                 node->data(0, kConnPermissionsTargetTypeRole).toString(),
                 node->data(0, kConnPermissionsTargetNameRole).toString());
    }
    if (kind == QStringLiteral("set") || kind == QStringLiteral("set_perm")
        || kind == QStringLiteral("create_perm")) {
        return QStringLiteral("%1|%2")
            .arg(kind, node->data(0, kConnPermissionsEntryNameRole).toString());
    }
    return QStringLiteral("%1|%2").arg(kind, node->text(0));
}

QString permissionPath(QTreeWidgetItem* root, QTreeWidgetItem* node) {
    if (!root || !node) {
        return QString();
    }
    QStringList parts;
    for (QTreeWidgetItem* cur = node; cur && cur != root; cur = cur->parent()) {
        parts.prepend(permissionNodeStableId(cur));
    }
    return parts.join(QStringLiteral("/"));
}

void normalizePermissionTokens(QStringList& tokens) {
    QSet<QString> seen;
    QStringList normalized;
    for (const QString& token : tokens) {
        const QString t = token.trimmed();
        const QString key = t.toLower();
        if (t.isEmpty() || seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        normalized.push_back(t);
    }
    normalized.sort(Qt::CaseInsensitive);
    tokens = normalized;
}

bool permissionTokenListsEqual(QStringList a, QStringList b) {
    normalizePermissionTokens(a);
    normalizePermissionTokens(b);
    return a == b;
}

QSet<QString> collectExpandedPermissionPaths(QTreeWidgetItem* root) {
    QSet<QString> expanded;
    if (!root) {
        return expanded;
    }
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        if (node != root && node->isExpanded()) {
            const QString path = permissionPath(root, node);
            if (!path.isEmpty()) {
                expanded.insert(path);
            }
        }
        for (int i = 0; i < node->childCount(); ++i) {
            rec(node->child(i));
        }
    };
    rec(root);
    return expanded;
}

void restoreExpandedPermissionPaths(QTreeWidgetItem* root, const QSet<QString>& expanded) {
    if (!root || expanded.isEmpty()) {
        return;
    }
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        if (node != root) {
            const QString path = permissionPath(root, node);
            if (expanded.contains(path)) {
                node->setExpanded(true);
            }
        }
        for (int i = 0; i < node->childCount(); ++i) {
            rec(node->child(i));
        }
    };
    rec(root);
}

QTreeWidgetItem* findDatasetItemByIdentity(QTreeWidget* tree,
                                           int connIdx,
                                           const QString& poolName,
                                           const QString& datasetName) {
    if (!tree) {
        return nullptr;
    }
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == datasetName
            && node->data(0, kConnIdxRole).toInt() == connIdx
            && node->data(0, kPoolNameRole).toString().trimmed() == poolName) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(node->child(i))) {
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
} // namespace

QString MainWindow::datasetPermissionsCacheKey(int connIdx, const QString& poolName, const QString& datasetName) const {
    return QStringLiteral("%1::%2::%3")
        .arg(connIdx)
        .arg(poolName.trimmed().toLower(), datasetName.trimmed().toLower());
}

QStringList MainWindow::availableDelegablePermissions(const QString& datasetName,
                                                      int connIdx,
                                                      const QString& poolName,
                                                      const QString& excludeSetName) const {
    QString datasetType = QStringLiteral("filesystem");
    const auto cacheIt = m_poolDatasetCache.constFind(datasetCacheKey(connIdx, poolName));
    if (cacheIt != m_poolDatasetCache.cend()) {
        const auto recIt = cacheIt->recordByName.constFind(datasetName);
        if (recIt != cacheIt->recordByName.cend()) {
            const DatasetRecord& rec = recIt.value();
            if (rec.mounted.trimmed() == QStringLiteral("-") && rec.mountpoint.trimmed() == QStringLiteral("-")) {
                datasetType = QStringLiteral("volume");
            }
        }
    }

    QStringList perms = {
        QStringLiteral("allow"),
        QStringLiteral("bookmark"),
        QStringLiteral("clone"),
        QStringLiteral("create"),
        QStringLiteral("destroy"),
        QStringLiteral("diff"),
        QStringLiteral("hold"),
        QStringLiteral("load-key"),
        QStringLiteral("mount"),
        QStringLiteral("promote"),
        QStringLiteral("receive"),
        QStringLiteral("release"),
        QStringLiteral("rename"),
        QStringLiteral("rollback"),
        QStringLiteral("send"),
        QStringLiteral("snapshot"),
        QStringLiteral("share")
    };
    if (datasetType == QStringLiteral("filesystem")) {
        perms << QStringLiteral("aclinherit")
              << QStringLiteral("acltype")
              << QStringLiteral("atime")
              << QStringLiteral("canmount")
              << QStringLiteral("checksum")
              << QStringLiteral("compression")
              << QStringLiteral("copies")
              << QStringLiteral("devices")
              << QStringLiteral("exec")
              << QStringLiteral("mountpoint")
              << QStringLiteral("nbmand")
              << QStringLiteral("normalization")
              << QStringLiteral("overlay")
              << QStringLiteral("primarycache")
              << QStringLiteral("quota")
              << QStringLiteral("readonly")
              << QStringLiteral("recordsize")
              << QStringLiteral("refquota")
              << QStringLiteral("refreservation")
              << QStringLiteral("reservation")
              << QStringLiteral("secondarycache")
              << QStringLiteral("setuid")
              << QStringLiteral("sharenfs")
              << QStringLiteral("sharesmb")
              << QStringLiteral("snapdir")
              << QStringLiteral("sync")
              << QStringLiteral("xattr");
    } else {
        perms << QStringLiteral("checksum")
              << QStringLiteral("compression")
              << QStringLiteral("copies")
              << QStringLiteral("logbias")
              << QStringLiteral("primarycache")
              << QStringLiteral("readonly")
              << QStringLiteral("refreservation")
              << QStringLiteral("reservation")
              << QStringLiteral("secondarycache")
              << QStringLiteral("snapdev")
              << QStringLiteral("sync")
              << QStringLiteral("volblocksize")
              << QStringLiteral("volmode")
              << QStringLiteral("volsize");
    }

    const auto permsCacheIt = m_datasetPermissionsCache.constFind(
        datasetPermissionsCacheKey(connIdx, poolName, datasetName));
    if (permsCacheIt != m_datasetPermissionsCache.cend()) {
        const QString excludeKey = excludeSetName.trimmed().toLower();
        for (const DatasetPermissionSet& set : permsCacheIt->permissionSets) {
            const QString setName = set.name.trimmed();
            if (setName.isEmpty()) {
                continue;
            }
            if (!excludeKey.isEmpty() && setName.compare(excludeSetName.trimmed(), Qt::CaseInsensitive) == 0) {
                continue;
            }
            perms.push_back(setName);
        }
    }

    perms.removeDuplicates();
    perms.sort(Qt::CaseInsensitive);
    return perms;
}

void MainWindow::invalidateDatasetPermissionsCacheForPool(int connIdx, const QString& poolName) {
    const QString prefix = QStringLiteral("%1::%2::").arg(connIdx).arg(poolName.trimmed().toLower());
    for (auto it = m_datasetPermissionsCache.begin(); it != m_datasetPermissionsCache.end();) {
        if (it.key().startsWith(prefix)) {
            it = m_datasetPermissionsCache.erase(it);
        } else {
            ++it;
        }
    }
}

bool MainWindow::ensureDatasetPermissionsLoaded(int connIdx, const QString& poolName, const QString& datasetName) {
    if (connIdx < 0 || connIdx >= m_profiles.size() || poolName.trimmed().isEmpty() || datasetName.trimmed().isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    if (isWindowsConnection(p)) {
        return false;
    }
    const QString key = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
    const auto cached = m_datasetPermissionsCache.constFind(key);
    if (cached != m_datasetPermissionsCache.cend() && cached->loaded) {
        return true;
    }

    QString out;
    QString err;
    int rc = -1;
    const QString cmd = withSudo(p, QStringLiteral("zfs allow %1").arg(shSingleQuote(datasetName)));
    if (!runSsh(p, cmd, 30000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("No se pudieron cargar permisos ZFS para %1: %2")
                   .arg(datasetName, oneLine(err)));
        return false;
    }

    const QString filteredOut = extractPermissionsOutputForDataset(out, datasetName);
    const ParsedPermissionsEntry parsed = parsePermissionsOutput(filteredOut);
    DatasetPermissionsCacheEntry entry;
    entry.loaded = true;
    for (const ParsedPermissionGrant& grant : parsed.localGrants) {
        DatasetPermissionGrant g;
        g.scope = grant.scope;
        g.targetType = grant.targetType;
        g.targetName = grant.targetName;
        g.permissions = grant.permissions;
        entry.localGrants.push_back(g);
        entry.originalLocalGrants.push_back(g);
    }
    for (const ParsedPermissionGrant& grant : parsed.descendantGrants) {
        DatasetPermissionGrant g;
        g.scope = grant.scope;
        g.targetType = grant.targetType;
        g.targetName = grant.targetName;
        g.permissions = grant.permissions;
        entry.descendantGrants.push_back(g);
        entry.originalDescendantGrants.push_back(g);
    }
    for (const ParsedPermissionGrant& grant : parsed.localDescendantGrants) {
        DatasetPermissionGrant g;
        g.scope = grant.scope;
        g.targetType = grant.targetType;
        g.targetName = grant.targetName;
        g.permissions = grant.permissions;
        entry.localDescendantGrants.push_back(g);
        entry.originalLocalDescendantGrants.push_back(g);
    }
    entry.createPermissions = parsed.createPermissions;
    entry.originalCreatePermissions = parsed.createPermissions;
    QSet<QString> seenSetNames;
    for (const ParsedPermissionSet& set : parsed.permissionSets) {
        const QString setKey = set.name.trimmed().toLower();
        if (setKey.isEmpty() || seenSetNames.contains(setKey)) {
            continue;
        }
        seenSetNames.insert(setKey);
        DatasetPermissionSet ps;
        ps.name = set.name;
        ps.permissions = set.permissions;
        entry.permissionSets.push_back(ps);
        entry.originalPermissionSets.push_back(ps);
    }
    const QString osLine = (connIdx >= 0 && connIdx < m_states.size()) ? m_states[connIdx].osLine : QString();
    auto queryAccounts = [this, &p, &osLine](const QString& kind) {
        QString listOut;
        QString listErr;
        int listRc = -1;
        const QString listCmd = accountListCommand(kind, p, osLine);
        if (!runSsh(p, listCmd, 15000, listOut, listErr, listRc) || listRc != 0) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("No se pudo enumerar %1 remotos: %2")
                       .arg(kind, oneLine(listErr)));
            return QStringList{};
        }
        QStringList names;
        QSet<QString> seen;
        for (const QString& line : listOut.split('\n', Qt::SkipEmptyParts)) {
            const QString t = line.trimmed();
            const QString k = t.toLower();
            if (t.isEmpty() || seen.contains(k)) {
                continue;
            }
            seen.insert(k);
            names.push_back(t);
        }
        names.sort(Qt::CaseInsensitive);
        return names;
    };
    entry.systemUsers = queryAccounts(QStringLiteral("user"));
    entry.systemGroups = queryAccounts(QStringLiteral("group"));
    m_datasetPermissionsCache.insert(key, entry);
    return true;
}

void MainWindow::populateDatasetPermissionsNode(QTreeWidget* tree, QTreeWidgetItem* datasetItem, bool forceReload) {
    if (!tree || !datasetItem) {
        return;
    }
    beginTransientUiBusy(QStringLiteral("Leyendo permisos..."));
    struct BusyGuard final {
        MainWindow* self;
        ~BusyGuard() {
            if (self) {
                self->endTransientUiBusy();
            }
        }
    } busyGuard{this};
    const QSignalBlocker blocker(tree);
    const QString datasetName = datasetItem->data(0, Qt::UserRole).toString().trimmed();
    const QString snapshotName = datasetItem->data(1, Qt::UserRole).toString().trimmed();
    const int connIdx = datasetItem->data(0, kConnIdxRole).toInt();
    const QString poolName = datasetItem->data(0, kPoolNameRole).toString().trimmed();
    if (datasetName.isEmpty() || !snapshotName.isEmpty() || connIdx < 0 || poolName.isEmpty()) {
        return;
    }

    QTreeWidgetItem* permissionsNode = nullptr;
    for (int i = 0; i < datasetItem->childCount(); ++i) {
        QTreeWidgetItem* child = datasetItem->child(i);
        if (child && child->data(0, kConnPermissionsNodeRole).toBool()
            && child->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")) {
            permissionsNode = child;
            break;
        }
    }
    if (!permissionsNode) {
        return;
    }

    updateStatus(QStringLiteral("Leyendo permisos de %1").arg(datasetName));

    if (forceReload) {
        m_datasetPermissionsCache.remove(datasetPermissionsCacheKey(connIdx, poolName, datasetName));
    }

    const bool rootExpanded = permissionsNode->isExpanded();
    const QSet<QString> expandedPaths = collectExpandedPermissionPaths(permissionsNode);

    while (permissionsNode->childCount() > 0) {
        delete permissionsNode->takeChild(0);
    }

    if (isWindowsConnection(connIdx)) {
        permissionsNode->setHidden(true);
        return;
    }
    permissionsNode->setHidden(false);

    if (!ensureDatasetPermissionsLoaded(connIdx, poolName, datasetName)) {
        return;
    }
    const auto it = m_datasetPermissionsCache.constFind(datasetPermissionsCacheKey(connIdx, poolName, datasetName));
    if (it == m_datasetPermissionsCache.cend()) {
        return;
    }
    const DatasetPermissionsCacheEntry& entry = it.value();

    const QStringList allSetTokens = availableDelegablePermissions(datasetName, connIdx, poolName);
    const int propCols = qBound(5, m_connPropColumnsSetting, 10);
    const QColor nameRowBg(232, 240, 250);
    const QFont inlineLabelFont = inlinePermissionLabelFont(tree);
    QVector<DatasetPermissionGrant> allGrants = entry.localGrants;
    allGrants += entry.descendantGrants;
    allGrants += entry.localDescendantGrants;
    auto* grantsNode = ensurePermissionsSectionNode(
        permissionsNode,
        QStringLiteral("Delegaciones"),
        QStringLiteral("grants_root"),
        allGrants.size(),
        connIdx,
        poolName);
    for (const DatasetPermissionGrant& grant : allGrants) {
        auto* targetNode = new QTreeWidgetItem(grantsNode);
        QString who = QStringLiteral("Everyone");
        if (grant.targetType == QStringLiteral("user")) {
            who = QStringLiteral("Usuario %1").arg(grant.targetName);
        } else if (grant.targetType == QStringLiteral("group")) {
            who = QStringLiteral("Grupo %1").arg(grant.targetName);
        }
        targetNode->setText(0, QStringLiteral("%1 Ámbito %2").arg(who, grantScopeLabel(grant.scope)));
        targetNode->setData(0, kConnPermissionsNodeRole, true);
        targetNode->setData(0, kConnPermissionsKindRole, QStringLiteral("grant"));
        targetNode->setData(0, kConnPermissionsScopeRole, grant.scope);
        targetNode->setData(0, kConnPermissionsTargetTypeRole, grant.targetType);
        targetNode->setData(0, kConnPermissionsTargetNameRole, grant.targetName);
        targetNode->setData(0, kConnPermissionsPendingRole, grant.pending);
        targetNode->setData(0, kConnIdxRole, connIdx);
        targetNode->setData(0, kPoolNameRole, poolName);
        targetNode->setExpanded(false);
        for (int base = 0; base < allSetTokens.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem(targetNode);
            rowNames->setData(0, kConnPropRowRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            auto* rowValues = new QTreeWidgetItem(targetNode);
            rowValues->setData(0, kConnPropRowRole, true);
            rowValues->setData(0, kConnPropRowKindRole, 2);
            rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
            rowValues->setText(0, QString());
            rowValues->setSizeHint(0, QSize(0, 24));
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= allSetTokens.size()) {
                    break;
                }
                const QString perm = allSetTokens.at(idx);
                const int col = 4 + off;
                rowNames->setData(col, kConnInlineCellUsedRole, true);
                rowValues->setData(col, kConnInlineCellUsedRole, true);
                rowNames->setBackground(col, QBrush(nameRowBg));
                rowNames->setText(col, perm);
                rowNames->setFont(col, inlineLabelFont);
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                rowNames->setData(col, kConnPermissionsEntryNameRole, perm);
                rowValues->setData(col, kConnPermissionsEntryNameRole, perm);
                auto* boxHost = new QWidget(tree);
                styleInlinePermissionCheckHost(boxHost, tree);
                auto* layout = new QHBoxLayout(boxHost);
                layout->setContentsMargins(0, 2, 0, 0);
                layout->setAlignment(Qt::AlignCenter);
                auto* cb = new QCheckBox(boxHost);
                cb->setChecked(grant.permissions.contains(perm, Qt::CaseInsensitive));
                cb->setFocusPolicy(Qt::NoFocus);
                layout->addWidget(cb);
                tree->setItemWidget(rowValues, col, boxHost);
                QObject::connect(cb, &QCheckBox::toggled, tree,
                                 [this, tree, connIdx, poolName, datasetName,
                                  grantScope = grant.scope,
                                  grantTargetType = grant.targetType,
                                  grantTargetName = grant.targetName](bool) {
                    auto collectCheckedTokens = [tree](QTreeWidgetItem* grantNode) {
                        QStringList checkedTokens;
                        if (!tree || !grantNode) {
                            return checkedTokens;
                        }
                        for (int i = 0; i < grantNode->childCount(); ++i) {
                            QTreeWidgetItem* row = grantNode->child(i);
                            if (!row || row->data(0, kConnPropRowKindRole).toInt() != 2) {
                                continue;
                            }
                            for (int col = 4; col < tree->columnCount(); ++col) {
                                const QString token = row->data(col, kConnPermissionsEntryNameRole).toString().trimmed();
                                if (token.isEmpty() || checkedTokens.contains(token, Qt::CaseInsensitive)) {
                                    continue;
                                }
                                QWidget* host = tree->itemWidget(row, col);
                                QCheckBox* rowCb = host ? host->findChild<QCheckBox*>() : nullptr;
                                if (rowCb && rowCb->isChecked()) {
                                    checkedTokens.push_back(token);
                                }
                            }
                        }
                        return checkedTokens;
                    };
                    auto findGrantNode = [&](QTreeWidgetItem* ownerNode) -> QTreeWidgetItem* {
                        if (!ownerNode) {
                            return nullptr;
                        }
                        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
                            if (!node) {
                                return nullptr;
                            }
                            if (node->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("grant")
                                && node->data(0, kConnPermissionsScopeRole).toString() == grantScope
                                && node->data(0, kConnPermissionsTargetTypeRole).toString() == grantTargetType
                                && node->data(0, kConnPermissionsTargetNameRole).toString() == grantTargetName) {
                                return node;
                            }
                            for (int i = 0; i < node->childCount(); ++i) {
                                if (QTreeWidgetItem* found = rec(node->child(i))) {
                                    return found;
                                }
                            }
                            return nullptr;
                        };
                        return rec(ownerNode);
                    };
                    QTreeWidgetItem* ownerNode = findDatasetItemByIdentity(tree, connIdx, poolName, datasetName);
                    QTreeWidgetItem* grantTreeNode = findGrantNode(ownerNode);
                    if (!ownerNode || !grantTreeNode) {
                        return;
                    }
                    QStringList checkedTokens = collectCheckedTokens(grantTreeNode);
                    normalizePermissionTokens(checkedTokens);
                    const QString cacheKey = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
                    auto cacheIt = m_datasetPermissionsCache.find(cacheKey);
                    if (cacheIt == m_datasetPermissionsCache.end()) {
                        return;
                    }
                    auto updateGrantList = [&](QVector<DatasetPermissionGrant>& grants) -> bool {
                        for (DatasetPermissionGrant& g : grants) {
                            if (g.scope == grantScope
                                && g.targetType == grantTargetType
                                && g.targetName == grantTargetName) {
                                g.permissions = checkedTokens;
                                cacheIt->dirty = true;
                                return true;
                            }
                        }
                        return false;
                    };
                    if (!updateGrantList(cacheIt->localGrants)
                        && !updateGrantList(cacheIt->descendantGrants)
                        && !updateGrantList(cacheIt->localDescendantGrants)) {
                        return;
                    }
                    updateApplyPropsButtonState();
                });
            }
        }
    }
    auto* createNode = ensurePermissionsSectionNode(
        permissionsNode,
        QStringLiteral("Permisos para nuevos subdatasets"),
        QStringLiteral("create_root"),
        entry.createPermissions.size(),
        connIdx,
        poolName);
    createNode->setToolTip(
        0,
        QStringLiteral("Permisos que recibirá automáticamente quien cree nuevos subdatasets debajo de este dataset."));
    for (int base = 0; base < allSetTokens.size(); base += propCols) {
        auto* rowNames = new QTreeWidgetItem(createNode);
        rowNames->setData(0, kConnPropRowRole, true);
        rowNames->setData(0, kConnPropRowKindRole, 1);
        rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
        auto* rowValues = new QTreeWidgetItem(createNode);
        rowValues->setData(0, kConnPropRowRole, true);
        rowValues->setData(0, kConnPropRowKindRole, 2);
        rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
        rowValues->setText(0, QString());
        rowValues->setSizeHint(0, QSize(0, 24));
        for (int off = 0; off < propCols; ++off) {
            const int idx = base + off;
            if (idx >= allSetTokens.size()) {
                break;
            }
            const QString perm = allSetTokens.at(idx);
            const int col = 4 + off;
            rowNames->setData(col, kConnInlineCellUsedRole, true);
            rowValues->setData(col, kConnInlineCellUsedRole, true);
            rowNames->setBackground(col, QBrush(nameRowBg));
            rowNames->setText(col, perm);
            rowNames->setFont(col, inlineLabelFont);
            rowNames->setTextAlignment(col, Qt::AlignCenter);
            rowNames->setData(col, kConnPermissionsEntryNameRole, perm);
            rowValues->setData(col, kConnPermissionsEntryNameRole, perm);
            auto* boxHost = new QWidget(tree);
            styleInlinePermissionCheckHost(boxHost, tree);
            auto* layout = new QHBoxLayout(boxHost);
            layout->setContentsMargins(0, 2, 0, 0);
            layout->setAlignment(Qt::AlignCenter);
            auto* cb = new QCheckBox(boxHost);
            cb->setChecked(entry.createPermissions.contains(perm, Qt::CaseInsensitive));
            cb->setFocusPolicy(Qt::NoFocus);
            layout->addWidget(cb);
            tree->setItemWidget(rowValues, col, boxHost);
            QObject::connect(cb, &QCheckBox::toggled, tree,
                             [this, tree, connIdx, poolName, datasetName](bool) {
                auto collectCheckedTokens = [tree](QTreeWidgetItem* createRootNode) {
                    QStringList checkedTokens;
                    if (!tree || !createRootNode) {
                        return checkedTokens;
                    }
                    for (int i = 0; i < createRootNode->childCount(); ++i) {
                        QTreeWidgetItem* row = createRootNode->child(i);
                        if (!row || row->data(0, kConnPropRowKindRole).toInt() != 2) {
                            continue;
                        }
                        for (int col = 4; col < tree->columnCount(); ++col) {
                            const QString token = row->data(col, kConnPermissionsEntryNameRole).toString().trimmed();
                            if (token.isEmpty() || checkedTokens.contains(token, Qt::CaseInsensitive)) {
                                continue;
                            }
                            QWidget* host = tree->itemWidget(row, col);
                            QCheckBox* rowCb = host ? host->findChild<QCheckBox*>() : nullptr;
                            if (rowCb && rowCb->isChecked()) {
                                checkedTokens.push_back(token);
                            }
                        }
                    }
                    return checkedTokens;
                };
                auto findCreateRootNode = [&](QTreeWidgetItem* ownerNode) -> QTreeWidgetItem* {
                    if (!ownerNode) {
                        return nullptr;
                    }
                    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
                        if (!node) {
                            return nullptr;
                        }
                        if (node->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("create_root")) {
                            return node;
                        }
                        for (int i = 0; i < node->childCount(); ++i) {
                            if (QTreeWidgetItem* found = rec(node->child(i))) {
                                return found;
                            }
                        }
                        return nullptr;
                    };
                    return rec(ownerNode);
                };
                QTreeWidgetItem* ownerNode = findDatasetItemByIdentity(tree, connIdx, poolName, datasetName);
                QTreeWidgetItem* createRootNode = findCreateRootNode(ownerNode);
                if (!ownerNode || !createRootNode) {
                    return;
                }
                QStringList checkedTokens = collectCheckedTokens(createRootNode);
                normalizePermissionTokens(checkedTokens);
                const QString cacheKey = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
                auto cacheIt = m_datasetPermissionsCache.find(cacheKey);
                if (cacheIt == m_datasetPermissionsCache.end()) {
                    return;
                }
                cacheIt->createPermissions = checkedTokens;
                cacheIt->dirty = true;
                updateApplyPropsButtonState();
            });
        }
    }

    auto* setsNode = ensurePermissionsSectionNode(
        permissionsNode,
        QStringLiteral("Sets de permisos"),
        QStringLiteral("sets_root"),
        entry.permissionSets.size(),
        connIdx,
        poolName);
    for (const DatasetPermissionSet& set : entry.permissionSets) {
        auto* setNode = new QTreeWidgetItem(setsNode);
        setNode->setText(0, set.name);
        setNode->setData(0, kConnPermissionsNodeRole, true);
        setNode->setData(0, kConnPermissionsKindRole, QStringLiteral("set"));
        setNode->setData(0, kConnPermissionsEntryNameRole, set.name);
        setNode->setData(0, kConnIdxRole, connIdx);
        setNode->setData(0, kPoolNameRole, poolName);
        setNode->setExpanded(false);
        const QStringList setAssignableTokens =
            availableDelegablePermissions(datasetName, connIdx, poolName, set.name);
        for (int base = 0; base < setAssignableTokens.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem(setNode);
            rowNames->setData(0, kConnPropRowRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            auto* rowValues = new QTreeWidgetItem(setNode);
            rowValues->setData(0, kConnPropRowRole, true);
            rowValues->setData(0, kConnPropRowKindRole, 2);
            rowValues->setFlags(rowValues->flags() & ~Qt::ItemIsUserCheckable);
            rowValues->setText(0, QString());
            rowValues->setSizeHint(0, QSize(0, 24));
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= setAssignableTokens.size()) {
                    break;
                }
                const QString perm = setAssignableTokens.at(idx);
                const int col = 4 + off;
                rowNames->setData(col, kConnInlineCellUsedRole, true);
                rowValues->setData(col, kConnInlineCellUsedRole, true);
                rowNames->setBackground(col, QBrush(nameRowBg));
                rowNames->setText(col, perm);
                rowNames->setFont(col, inlineLabelFont);
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                rowNames->setData(col, kConnPermissionsEntryNameRole, perm);
                rowValues->setData(col, kConnPermissionsEntryNameRole, perm);
                auto* boxHost = new QWidget(tree);
                styleInlinePermissionCheckHost(boxHost, tree);
                auto* layout = new QHBoxLayout(boxHost);
                layout->setContentsMargins(0, 2, 0, 0);
                layout->setAlignment(Qt::AlignCenter);
                auto* cb = new QCheckBox(boxHost);
                cb->setChecked(set.permissions.contains(perm, Qt::CaseInsensitive));
                cb->setFocusPolicy(Qt::NoFocus);
                layout->addWidget(cb);
                tree->setItemWidget(rowValues, col, boxHost);
                QObject::connect(cb, &QCheckBox::toggled, tree,
                                 [this, tree, connIdx, poolName, datasetName, setName = set.name](bool) {
                    auto stableId = [](QTreeWidgetItem* node) {
                        if (!node) {
                            return QString();
                        }
                        if (node->data(0, kConnPermissionsNodeRole).toBool()) {
                            return QStringLiteral("perm|%1|%2|%3")
                                .arg(node->data(0, kConnPermissionsKindRole).toString(),
                                     node->data(0, kConnPermissionsEntryNameRole).toString().trimmed(),
                                     node->text(0).trimmed());
                        }
                        return QStringLiteral("text|%1").arg(node->text(0).trimmed());
                    };
                    auto collectExpandedPaths = [&](QTreeWidgetItem* datasetNode) {
                        QStringList paths;
                        QSet<QString> seen;
                        std::function<void(QTreeWidgetItem*, QStringList)> rec = [&](QTreeWidgetItem* node, QStringList parts) {
                            if (!node) {
                                return;
                            }
                            if (node != datasetNode) {
                                const QString id = stableId(node);
                                if (!id.isEmpty()) {
                                    parts.push_back(id);
                                    if (node->isExpanded()) {
                                        const QString path = parts.join(QStringLiteral("/"));
                                        if (!seen.contains(path)) {
                                            seen.insert(path);
                                            paths.push_back(path);
                                        }
                                    }
                                }
                            }
                            for (int i = 0; i < node->childCount(); ++i) {
                                rec(node->child(i), parts);
                            }
                        };
                        rec(datasetNode, {});
                        return paths;
                    };
                    auto restoreExpandedPaths = [&](QTreeWidgetItem* datasetNode, const QStringList& paths) {
                        if (!datasetNode || paths.isEmpty()) {
                            return;
                        }
                        const QSet<QString> wanted(paths.cbegin(), paths.cend());
                        std::function<void(QTreeWidgetItem*, QStringList)> rec = [&](QTreeWidgetItem* node, QStringList parts) {
                            if (!node) {
                                return;
                            }
                            if (node != datasetNode) {
                                const QString id = stableId(node);
                                if (!id.isEmpty()) {
                                    parts.push_back(id);
                                    if (wanted.contains(parts.join(QStringLiteral("/")))) {
                                        node->setExpanded(true);
                                    }
                                }
                            }
                            for (int i = 0; i < node->childCount(); ++i) {
                                rec(node->child(i), parts);
                            }
                        };
                        rec(datasetNode, {});
                    };
                    auto collectCheckedTokens = [tree](QTreeWidgetItem* setTreeNode) {
                        QStringList checkedTokens;
                        if (!tree || !setTreeNode) {
                            return checkedTokens;
                        }
                        for (int i = 0; i < setTreeNode->childCount(); ++i) {
                            QTreeWidgetItem* row = setTreeNode->child(i);
                            if (!row || row->data(0, kConnPropRowKindRole).toInt() != 2) {
                                continue;
                            }
                            for (int col = 4; col < tree->columnCount(); ++col) {
                                const QString token = row->data(col, kConnPermissionsEntryNameRole).toString().trimmed();
                                if (token.isEmpty() || checkedTokens.contains(token, Qt::CaseInsensitive)) {
                                    continue;
                                }
                                QWidget* host = tree->itemWidget(row, col);
                                QCheckBox* rowCb = host ? host->findChild<QCheckBox*>() : nullptr;
                                if (rowCb && rowCb->isChecked()) {
                                    checkedTokens.push_back(token);
                                }
                            }
                        }
                        return checkedTokens;
                    };
                    auto findSetNode = [&](QTreeWidgetItem* ownerNode) -> QTreeWidgetItem* {
                        if (!ownerNode) {
                            return nullptr;
                        }
                        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
                            if (!node) {
                                return nullptr;
                            }
                            if (node->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("set")
                                && node->data(0, kConnPermissionsEntryNameRole).toString().trimmed() == setName) {
                                return node;
                            }
                            for (int i = 0; i < node->childCount(); ++i) {
                                if (QTreeWidgetItem* found = rec(node->child(i))) {
                                    return found;
                                }
                            }
                            return nullptr;
                        };
                        return rec(ownerNode);
                    };

                    QTreeWidgetItem* ownerNode = findDatasetItemByIdentity(tree, connIdx, poolName, datasetName);
                    QTreeWidgetItem* setTreeNode = findSetNode(ownerNode);
                    if (!ownerNode || !setTreeNode) {
                        return;
                    }
                    const bool ownerExpanded = ownerNode->isExpanded();
                    const QStringList ownerExpandedPaths = collectExpandedPaths(ownerNode);
                    appLog(QStringLiteral("DEBUG"),
                           QStringLiteral("setCheckbox before dataset=%1 set=%2 ownerExpanded=%3 ownerPaths=%4")
                               .arg(datasetName,
                                    setName,
                                    ownerExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                                    ownerExpandedPaths.join(QStringLiteral(" || "))));
                    QStringList checkedTokens = collectCheckedTokens(setTreeNode);
                    normalizePermissionTokens(checkedTokens);
                    const QString cacheKey = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
                    auto cacheIt = m_datasetPermissionsCache.find(cacheKey);
                    if (cacheIt == m_datasetPermissionsCache.end()) {
                        return;
                    }
                    for (DatasetPermissionSet& cachedSet : cacheIt->permissionSets) {
                        if (cachedSet.name.compare(setName, Qt::CaseInsensitive) == 0) {
                            cachedSet.permissions = checkedTokens;
                            cacheIt->dirty = true;
                            break;
                        }
                    }
                    appLog(QStringLiteral("DEBUG"),
                           QStringLiteral("setCheckbox after draft-only dataset=%1 set=%2 ownerExpanded=%3")
                               .arg(datasetName,
                                    setName,
                                    ownerExpanded ? QStringLiteral("1") : QStringLiteral("0")));
                    Q_UNUSED(ownerExpandedPaths);
                    updateApplyPropsButtonState();
                });
            }
        }
    }

    permissionsNode->setExpanded(rootExpanded);
    restoreExpandedPermissionPaths(permissionsNode, expandedPaths);
}
