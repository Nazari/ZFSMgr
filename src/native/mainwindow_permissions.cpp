#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "agentversion.h"

#include <QCheckBox>
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSet>
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
constexpr int kConnPropGroupNodeRole = Qt::UserRole + 17;
constexpr int kConnPropGroupNameRole = Qt::UserRole + 18;
constexpr int kConnInlineCellUsedRole = Qt::UserRole + 32;
constexpr int kConnStatePartRole = Qt::UserRole + 44;

class InlinePermissionCheckHost final : public QWidget {
public:
    explicit InlinePermissionCheckHost(QWidget* parent = nullptr)
        : QWidget(parent) {}

    void setCheckbox(QCheckBox* checkbox) {
        m_checkbox = checkbox;
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            if (m_checkbox && m_checkbox->isEnabled()) {
                m_checkbox->click();
                event->accept();
                return;
            }
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QCheckBox* m_checkbox{nullptr};
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
    node->setData(0, kConnStatePartRole, QStringLiteral("perm:%1").arg(kind));
    node->setData(0, kConnIdxRole, connIdx);
    node->setData(0, kPoolNameRole, poolName);
    node->setExpanded(false);
    node->setFlags(node->flags() & ~Qt::ItemIsUserCheckable);
    return node;
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
    host->setAutoFillBackground(false);
    host->setAttribute(Qt::WA_StyledBackground, false);
    host->setStyleSheet(QStringLiteral("background: transparent; border: 0px;"));
}

QFont inlinePermissionLabelFont(const QTreeWidget* tree) {
    return tree ? tree->font() : QFont();
}

QString permissionNodeStableId(QTreeWidgetItem* node) {
    if (!node) {
        return QString();
    }
    const QString explicitPart = node->data(0, kConnStatePartRole).toString().trimmed();
    if (!explicitPart.isEmpty()) {
        return explicitPart;
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
    if (!kind.isEmpty()) {
        return QStringLiteral("perm:%1").arg(kind);
    }
    if (QTreeWidgetItem* parent = node->parent()) {
        return QStringLiteral("idx:%1").arg(parent->indexOfChild(node));
    }
    return QStringLiteral("idx:0");
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
        .arg(connToken(connIdx))
        .arg(poolName.trimmed().toLower(), datasetName.trimmed().toLower());
}

QString MainWindow::connectionAccountCacheKey(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return QString();
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    QString key = p.id.trimmed().toLower();
    if (key.isEmpty()) {
        key = p.name.trimmed().toLower();
    }
    if (key.isEmpty()) {
        key = QStringLiteral("idx:%1").arg(connIdx);
    }
    return key;
}

QStringList MainWindow::availableDelegablePermissions(const QString& datasetName,
                                                      int connIdx,
                                                      const QString& poolName,
                                                      const QString& excludeSetName) const {
    QString datasetType = QStringLiteral("filesystem");
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (dsInfo) {
        const QString mounted = dsInfo->runtime.properties.value(QStringLiteral("mounted")).trimmed();
        const QString mountpoint = dsInfo->runtime.properties.value(QStringLiteral("mountpoint")).trimmed();
        if (mounted == QStringLiteral("-") && mountpoint == QStringLiteral("-")) {
            datasetType = QStringLiteral("volume");
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

    auto appendPermissionSets = [&](const QString& dsName) {
        if (dsName.trimmed().isEmpty()) {
            return;
        }
        if (const_cast<MainWindow*>(this)->ensureDatasetPermissionsEntryLoaded(connIdx, poolName, dsName);
            const DatasetPermissionsCacheEntry* permsCacheIt = datasetPermissionsEntry(connIdx, poolName, dsName)) {
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
    };

    appendPermissionSets(datasetName);
    QString ancestorName = datasetName.trimmed();
    while (!ancestorName.isEmpty()) {
        const int slashPos = ancestorName.lastIndexOf(QLatin1Char('/'));
        if (slashPos <= 0) {
            break;
        }
        ancestorName = ancestorName.left(slashPos).trimmed();
        appendPermissionSets(ancestorName);
    }

    {
        const QString excludeKey = excludeSetName.trimmed().toLower();
        if (!excludeKey.isEmpty()) {
            for (int i = perms.size() - 1; i >= 0; --i) {
                if (perms.at(i).trimmed().compare(excludeSetName.trimmed(), Qt::CaseInsensitive) == 0) {
                    perms.removeAt(i);
                }
            }
        }
    }

    perms.removeDuplicates();
    perms.sort(Qt::CaseInsensitive);
    return perms;
}

void MainWindow::invalidateDatasetPermissionsCacheForPool(int connIdx, const QString& poolName) {
    removeDatasetPermissionsEntriesForPool(connIdx, poolName);
    rebuildConnInfoFor(connIdx);
}

bool MainWindow::ensureDatasetPermissionsLoaded(int connIdx, const QString& poolName, const QString& datasetName) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || poolName.trimmed().isEmpty() || datasetName.trimmed().isEmpty()) {
        return false;
    }
    if (!featureAvailable(connIdx, zfsmgr::caps::Feature::DatasetPermissions)) {
        return false;
    }
    if (const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
        dsInfo && dsInfo->runtime.permissionsState == LoadState::Loaded && dsInfo->permissionsCache.loaded) {
        return true;
    }
    const DatasetPermissionsCacheEntry* cached = datasetPermissionsEntry(connIdx, poolName, datasetName);
    if (cached && cached->loaded) {
        return true;
    }
    DatasetPermissionsCacheEntry entry;
    QString error;
    if (!readPermissionsEntry(connIdx, datasetName, entry, &error)) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("No se pudieron cargar permisos ZFS para %1: %2")
                   .arg(datasetName, oneLine(error)));
        return false;
    }
    entry.systemUsers = readAccounts(connIdx, false);
    entry.systemGroups = readAccounts(connIdx, true);
    if (auto* mutableEntry = datasetPermissionsEntryMutable(connIdx, poolName, datasetName)) {
        *mutableEntry = entry;
    } else {
        m_conns.datasetPermissionsCache.insert(
            datasetPermissionsCacheKey(connIdx, poolName, datasetName), entry);
    }
    mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
    rebuildConnInfoFor(connIdx);
    return true;
}

bool MainWindow::ensureDatasetPermissionsLoadedBatch(int connIdx,
                                                     const QString& poolName,
                                                     const QStringList& datasetNames) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || poolName.trimmed().isEmpty()) {
        return false;
    }
    if (!featureAvailable(connIdx, zfsmgr::caps::Feature::DatasetPermissions)) {
        return false;
    }
    // One `ls #permissions` per dataset that is not loaded yet.
    bool anyLoaded = false;
    QSet<QString> seen;
    for (const QString& raw : datasetNames) {
        const QString datasetName = raw.trimmed();
        if (datasetName.isEmpty() || seen.contains(datasetName.toLower())) {
            continue;
        }
        seen.insert(datasetName.toLower());
        if (const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
            dsInfo && dsInfo->runtime.permissionsState == LoadState::Loaded
            && dsInfo->permissionsCache.loaded) {
            continue;
        }
        if (const DatasetPermissionsCacheEntry* cached =
                datasetPermissionsEntry(connIdx, poolName, datasetName);
            cached && cached->loaded) {
            continue;
        }
        DatasetPermissionsCacheEntry entry;
        if (!readPermissionsEntry(connIdx, datasetName, entry)) {
            continue;
        }
        entry.systemUsers = readAccounts(connIdx, false);
        entry.systemGroups = readAccounts(connIdx, true);
        if (auto* mutableEntry = datasetPermissionsEntryMutable(connIdx, poolName, datasetName)) {
            *mutableEntry = entry;
        } else {
            m_conns.datasetPermissionsCache.insert(
                datasetPermissionsCacheKey(connIdx, poolName, datasetName), entry);
        }
        mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
        anyLoaded = true;
    }
    if (anyLoaded) {
        rebuildConnInfoFor(connIdx);
    }
    return anyLoaded;
}

// The delegations of a dataset as the shell's `ls #permissions` lists them —parsed by
// base/zfsallow, with zfs's own section titles—, into the editor's entry.
bool MainWindow::readPermissionsEntry(int connIdx, const QString& datasetName,
                                      DatasetPermissionsCacheEntry& entry, QString* error) {
    entry = DatasetPermissionsCacheEntry{};
    QJsonObject r;
    if (!cliReadJson(connIdx,
                     QStringLiteral("ls %1#permissions").arg(zfsmUrl(connIdx, datasetName)), r,
                     error, 30000)) {
        return false;
    }
    entry.loaded = true;
    QSet<QString> conjuntosVistos;
    for (const QJsonValue& v : r.value(QStringLiteral("permissions")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString seccion = o.value(QStringLiteral("scope")).toString().toLower();
        const QString clase = o.value(QStringLiteral("kind")).toString().toLower();
        const QString quien = o.value(QStringLiteral("who")).toString().trimmed();
        const QStringList permisos =
            splitPermissionTokens(o.value(QStringLiteral("permissions")).toString());
        if (seccion.startsWith(QStringLiteral("create time"))) {
            for (const QString& pm : permisos) {
                if (!entry.createPermissions.contains(pm, Qt::CaseInsensitive)) {
                    entry.createPermissions.push_back(pm);
                }
            }
            continue;
        }
        if (seccion.startsWith(QStringLiteral("permission set"))) {
            QString nombre = quien;
            if (!nombre.startsWith(QLatin1Char('@'))) {
                nombre.prepend(QLatin1Char('@'));
            }
            if (conjuntosVistos.contains(nombre.toLower())) {
                continue;
            }
            conjuntosVistos.insert(nombre.toLower());
            DatasetPermissionSet ps;
            ps.name = nombre;
            ps.permissions = permisos;
            entry.permissionSets.push_back(ps);
            continue;
        }
        if (permisos.isEmpty() || clase.isEmpty()) {
            continue;
        }
        DatasetPermissionGrant g;
        g.targetType = clase;
        g.targetName = clase == QStringLiteral("everyone") ? QStringLiteral("everyone") : quien;
        g.permissions = permisos;
        if (seccion.startsWith(QStringLiteral("local+"))) {
            g.scope = QStringLiteral("local_descendant");
            entry.localDescendantGrants.push_back(g);
        } else if (seccion.startsWith(QStringLiteral("local"))) {
            g.scope = QStringLiteral("local");
            entry.localGrants.push_back(g);
        } else if (seccion.startsWith(QStringLiteral("descend"))) {
            g.scope = QStringLiteral("descendant");
            entry.descendantGrants.push_back(g);
        }
    }
    entry.originalLocalGrants = entry.localGrants;
    entry.originalDescendantGrants = entry.descendantGrants;
    entry.originalLocalDescendantGrants = entry.localDescendantGrants;
    entry.originalCreatePermissions = entry.createPermissions;
    entry.originalPermissionSets = entry.permissionSets;
    return true;
}

// The machine's users or groups, from the shell's `accounts`, once per connection.
QStringList MainWindow::readAccounts(int connIdx, bool groups) {
    const QString accountKey = connectionAccountCacheKey(connIdx);
    auto& cache = groups ? m_connSystemGroupsCacheByKey : m_connSystemUsersCacheByKey;
    auto& cargadas = groups ? m_connSystemGroupsLoadedKeys : m_connSystemUsersLoadedKeys;
    if (!accountKey.isEmpty() && cargadas.contains(accountKey)) {
        return cache.value(accountKey);
    }
    QJsonObject r;
    QStringList nombres;
    QString error;
    if (cliReadJson(connIdx,
                    QStringLiteral("accounts%1 --on %2")
                        .arg(groups ? QStringLiteral(" --groups") : QString(), zfsmUrl(connIdx)),
                    r, &error, 20000)) {
        for (const QJsonValue& v :
             r.value(groups ? QStringLiteral("groups") : QStringLiteral("users")).toArray()) {
            nombres.push_back(v.toObject().value(QStringLiteral("name")).toString());
        }
        nombres.sort(Qt::CaseInsensitive);
    } else {
        appLog(QStringLiteral("WARN"), QStringLiteral("accounts: %1").arg(oneLine(error)));
    }
    if (!accountKey.isEmpty()) {
        cache.insert(accountKey, nombres);
        cargadas.insert(accountKey);
    }
    return nombres;
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

    QTreeWidgetItem* propsNode = nullptr;
    QTreeWidgetItem* permissionsNode = nullptr;
    for (int i = 0; i < datasetItem->childCount(); ++i) {
        QTreeWidgetItem* child = datasetItem->child(i);
        if (child && child->data(0, kConnPropGroupNodeRole).toBool()
            && child->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()) {
            propsNode = child;
        }
        if (child && child->data(0, kConnPermissionsNodeRole).toBool()
            && child->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")) {
            permissionsNode = child;
        }
    }
    if (propsNode) {
        for (int i = propsNode->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* child = propsNode->child(i);
            if (child
                && child->data(0, kConnPermissionsNodeRole).toBool()
                && child->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("create_root")) {
                delete propsNode->takeChild(i);
            }
        }
    }

    updateStatus(QStringLiteral("Leyendo permisos de %1").arg(datasetName));

    if (forceReload) {
        removeDatasetPermissionsEntry(connIdx, poolName, datasetName);
        rebuildConnInfoFor(connIdx);
    }

    bool rootExpanded = false;
    QSet<QString> expandedPaths;
    if (permissionsNode) {
        rootExpanded = permissionsNode->isExpanded();
        expandedPaths = collectExpandedPermissionPaths(permissionsNode);
        while (permissionsNode->childCount() > 0) {
            delete permissionsNode->takeChild(0);
        }
    }

    if (!featureAvailable(connIdx, zfsmgr::caps::Feature::DatasetPermissions)) {
        if (permissionsNode) {
            permissionsNode->setHidden(true);
        }
        return;
    }
    if (permissionsNode) {
        permissionsNode->setHidden(false);
    }

    const DatasetPermissionsCacheEntry* entryPtr =
        ensureDatasetPermissionsEntryLoaded(connIdx, poolName, datasetName);
    if (!entryPtr) {
        return;
    }
    const DatasetPermissionsCacheEntry& entry = *entryPtr;

    const QStringList allSetTokens = availableDelegablePermissions(datasetName, connIdx, poolName);
    const int propCols = qBound(5, m_connPropColumnsSetting, 10);
    const QColor nameRowBg(232, 240, 250);
    const QFont inlineLabelFont = inlinePermissionLabelFont(tree);
    QVector<DatasetPermissionGrant> allGrants = entry.localGrants;
    allGrants += entry.descendantGrants;
    allGrants += entry.localDescendantGrants;
    if (permissionsNode && !allGrants.isEmpty()) {
        auto* grantsNode = ensurePermissionsSectionNode(
            permissionsNode,
            trk(QStringLiteral("t_perm_grants_root_001"),
                QStringLiteral("Delegación"),
                QStringLiteral("Delegation")),
            QStringLiteral("grants_root"),
            allGrants.size(),
            connIdx,
            poolName);
        for (const DatasetPermissionGrant& grant : allGrants) {
            auto* targetNode = new QTreeWidgetItem(grantsNode);
            QString who = trk(QStringLiteral("t_everyone_001"),
                              QStringLiteral("Everyone"),
                              QStringLiteral("Everyone"));
            if (grant.targetType == QStringLiteral("user")) {
                who = trk(QStringLiteral("t_user_with_name_001"),
                          QStringLiteral("Usuario %1"),
                          QStringLiteral("User %1"))
                          .arg(grant.targetName);
            } else if (grant.targetType == QStringLiteral("group")) {
                who = trk(QStringLiteral("t_group_with_name_001"),
                          QStringLiteral("Grupo %1"),
                          QStringLiteral("Group %1"))
                          .arg(grant.targetName);
            }
            targetNode->setText(
                0,
                trk(QStringLiteral("t_perm_scope_row_001"),
                    QStringLiteral("%1 Ámbito %2"),
                    QStringLiteral("%1 Scope %2"))
                    .arg(who, grantScopeLabel(grant.scope)));
            targetNode->setData(0, kConnPermissionsNodeRole, true);
            targetNode->setData(0, kConnPermissionsKindRole, QStringLiteral("grant"));
            targetNode->setData(0, kConnStatePartRole,
                                QStringLiteral("perm:grant:%1:%2:%3")
                                    .arg(grant.scope, grant.targetType, grant.targetName));
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
                    auto* boxHost = new InlinePermissionCheckHost(tree);
                    styleInlinePermissionCheckHost(boxHost, tree);
                    auto* layout = new QHBoxLayout(boxHost);
                    layout->setContentsMargins(1, 1, 1, 1);
                    layout->setAlignment(Qt::AlignCenter);
                    auto* cb = new QCheckBox(boxHost);
                    cb->setChecked(grant.permissions.contains(perm, Qt::CaseInsensitive));
                    cb->setFocusPolicy(Qt::NoFocus);
                    boxHost->setCheckbox(cb);
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
                        auto* cacheIt = datasetPermissionsEntryMutable(connIdx, poolName, datasetName);
                        if (!cacheIt) {
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
                        mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
                        updateApplyPropsButtonState();
                    });
                }
            }
        }
    }
    QTreeWidgetItem* createOwnerNode = propsNode ? propsNode : permissionsNode;
    if (createOwnerNode) {
    auto* createNode = ensurePermissionsSectionNode(
        createOwnerNode,
        trk(QStringLiteral("t_perm_new_children_root_001"),
            QStringLiteral("Permisos por defecto"),
            QStringLiteral("Default permissions")),
        QStringLiteral("create_root"),
        entry.createPermissions.size(),
        connIdx,
        poolName);
    createNode->setToolTip(
        0,
        trk(QStringLiteral("t_perm_new_children_tt_001"),
            QStringLiteral("Permisos que recibirá automáticamente quien cree nuevos subdatasets debajo de este dataset."),
            QStringLiteral("Permissions automatically granted to whoever creates new child datasets below this dataset.")));
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
            auto* boxHost = new InlinePermissionCheckHost(tree);
            styleInlinePermissionCheckHost(boxHost, tree);
            auto* layout = new QHBoxLayout(boxHost);
            layout->setContentsMargins(1, 1, 1, 1);
            layout->setAlignment(Qt::AlignCenter);
            auto* cb = new QCheckBox(boxHost);
            cb->setChecked(entry.createPermissions.contains(perm, Qt::CaseInsensitive));
            cb->setFocusPolicy(Qt::NoFocus);
            boxHost->setCheckbox(cb);
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
                auto* cacheIt = datasetPermissionsEntryMutable(connIdx, poolName, datasetName);
                if (!cacheIt) {
                    return;
                }
                cacheIt->createPermissions = checkedTokens;
                cacheIt->dirty = true;
                mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
                updateApplyPropsButtonState();
            });
        }
    }

    }

    if (permissionsNode && !entry.permissionSets.isEmpty()) {
        auto* setsNode = ensurePermissionsSectionNode(
            permissionsNode,
            trk(QStringLiteral("t_perm_sets_root_001"),
                QStringLiteral("Conjuntos"),
                QStringLiteral("Sets")),
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
            setNode->setData(0, kConnStatePartRole, QStringLiteral("perm:set:%1").arg(set.name.trimmed()));
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
                    auto* boxHost = new InlinePermissionCheckHost(tree);
                    styleInlinePermissionCheckHost(boxHost, tree);
                    auto* layout = new QHBoxLayout(boxHost);
                    layout->setContentsMargins(1, 1, 1, 1);
                    layout->setAlignment(Qt::AlignCenter);
                    auto* cb = new QCheckBox(boxHost);
                    cb->setChecked(set.permissions.contains(perm, Qt::CaseInsensitive));
                    cb->setFocusPolicy(Qt::NoFocus);
                    boxHost->setCheckbox(cb);
                    layout->addWidget(cb);
                    tree->setItemWidget(rowValues, col, boxHost);
                    QObject::connect(cb, &QCheckBox::toggled, tree,
                                     [this, tree, connIdx, poolName, datasetName, setName = set.name](bool) {
                    auto stableId = [](QTreeWidgetItem* node) {
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
                            return entry.isEmpty() ? QStringLiteral("perm:%1").arg(kind)
                                                   : QStringLiteral("perm:%1:%2").arg(kind, entry);
                        }
                        if (QTreeWidgetItem* parent = node->parent()) {
                            return QStringLiteral("idx:%1").arg(parent->indexOfChild(node));
                        }
                        return QStringLiteral("idx:0");
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
                    auto* cacheIt = datasetPermissionsEntryMutable(connIdx, poolName, datasetName);
                    if (!cacheIt) {
                        return;
                    }
                    for (DatasetPermissionSet& cachedSet : cacheIt->permissionSets) {
                        if (cachedSet.name.compare(setName, Qt::CaseInsensitive) == 0) {
                            cachedSet.permissions = checkedTokens;
                            cacheIt->dirty = true;
                            break;
                        }
                    }
                    mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
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
    }

    if (permissionsNode) {
        permissionsNode->setExpanded(rootExpanded);
        restoreExpandedPermissionPaths(permissionsNode, expandedPaths);
    }
    applyDebugNodeIdsToTree(tree);
}
