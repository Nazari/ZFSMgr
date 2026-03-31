#include <QByteArray>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QVector>
#include <QDebug>
#include <QLibrary>
#include <libssh/libssh.h>

#include <algorithm>
#include <cstring>

#include "../src/daemon_rpc_protocol.h"

static constexpr int kDefaultPort = 32099;

static QString jsonResult(const QString& requestId,
                          int code,
                          const QString& message,
                          const QJsonObject& payload = {},
                          const QString& fallback = {}) {
    zfsmgr::daemonrpc::RpcResponse response;
    response.id = requestId;
    response.result.code = code;
    response.result.message = message;
    response.result.payload = payload;
    response.result.fallback = fallback;
    return QString::fromUtf8(response.toJson());
}

namespace {

struct LibZfsBackend {
    using InitFn = void* (*)();
    using FiniFn = void (*)(void*);
    using IterCb = int (*)(void*, void*);
    using ZpoolIterFn = int (*)(void*, IterCb, void*);
    using ZpoolGetNameFn = const char* (*)(void*);
    using ZpoolCloseFn = void (*)(void*);
    using ZfsIterRootFn = int (*)(void*, IterCb, void*);
    using ZfsIterFilesystemsFn = int (*)(void*, IterCb, void*);
    using ZfsIterSnapshotsFn = int (*)(void*, IterCb, void*, bool);
    using ZfsGetNameFn = const char* (*)(void*);
    using ZfsIsMountedFn = int (*)(void*, char**);
    using ZfsCloseFn = void (*)(void*);
    using ZfsNameToPropFn = int (*)(const char*);
    using ZfsPropGetFn = int (*)(void*, int, char*, size_t, int*, char*, size_t, int);

    QLibrary lib;
    QString candidate;
    InitFn initFn{nullptr};
    FiniFn finiFn{nullptr};
    ZpoolIterFn zpoolIterFn{nullptr};
    ZpoolGetNameFn zpoolGetNameFn{nullptr};
    ZpoolCloseFn zpoolCloseFn{nullptr};
    ZfsIterRootFn zfsIterRootFn{nullptr};
    ZfsIterFilesystemsFn zfsIterFilesystemsFn{nullptr};
    ZfsIterSnapshotsFn zfsIterSnapshotsFn{nullptr};
    ZfsGetNameFn zfsGetNameFn{nullptr};
    ZfsIsMountedFn zfsIsMountedFn{nullptr};
    ZfsCloseFn zfsCloseFn{nullptr};
    ZfsNameToPropFn zfsNameToPropFn{nullptr};
    ZfsPropGetFn zfsPropGetFn{nullptr};

    bool load(QString* detail) {
#if defined(Q_OS_WIN)
        if (detail) {
            *detail = QStringLiteral("libzfs backend not available on Windows");
        }
        return false;
#else
        QStringList candidates;
#if defined(Q_OS_MACOS)
        candidates << QStringLiteral("/usr/local/zfs/lib/libzfs.dylib")
                   << QStringLiteral("libzfs.dylib");
#else
        candidates << QStringLiteral("libzfs.so.7")
                   << QStringLiteral("libzfs.so.6")
                   << QStringLiteral("libzfs.so.5")
                   << QStringLiteral("libzfs.so.4")
                   << QStringLiteral("libzfs.so");
#endif
        QString localDetail = QStringLiteral("no loadable libzfs library found");
        for (const QString& cand : candidates) {
            lib.setFileName(cand);
            if (!lib.load()) {
                continue;
            }
            candidate = cand;
            initFn = reinterpret_cast<InitFn>(lib.resolve("libzfs_init"));
            finiFn = reinterpret_cast<FiniFn>(lib.resolve("libzfs_fini"));
            zpoolIterFn = reinterpret_cast<ZpoolIterFn>(lib.resolve("zpool_iter"));
            zpoolGetNameFn = reinterpret_cast<ZpoolGetNameFn>(lib.resolve("zpool_get_name"));
            zpoolCloseFn = reinterpret_cast<ZpoolCloseFn>(lib.resolve("zpool_close"));
            zfsIterRootFn = reinterpret_cast<ZfsIterRootFn>(lib.resolve("zfs_iter_root"));
            zfsIterFilesystemsFn =
                reinterpret_cast<ZfsIterFilesystemsFn>(lib.resolve("zfs_iter_filesystems"));
            zfsIterSnapshotsFn =
                reinterpret_cast<ZfsIterSnapshotsFn>(lib.resolve("zfs_iter_snapshots"));
            zfsGetNameFn = reinterpret_cast<ZfsGetNameFn>(lib.resolve("zfs_get_name"));
            zfsIsMountedFn = reinterpret_cast<ZfsIsMountedFn>(lib.resolve("zfs_is_mounted"));
            zfsCloseFn = reinterpret_cast<ZfsCloseFn>(lib.resolve("zfs_close"));
            zfsNameToPropFn = reinterpret_cast<ZfsNameToPropFn>(lib.resolve("zfs_name_to_prop"));
            zfsPropGetFn = reinterpret_cast<ZfsPropGetFn>(lib.resolve("zfs_prop_get"));
            if (!initFn || !finiFn || !zpoolIterFn || !zpoolGetNameFn || !zpoolCloseFn
                || !zfsIterRootFn || !zfsIterFilesystemsFn || !zfsIterSnapshotsFn
                || !zfsGetNameFn || !zfsCloseFn || !zfsNameToPropFn || !zfsPropGetFn) {
                localDetail = QStringLiteral("%1 loaded but required symbols are missing").arg(cand);
                lib.unload();
                clear();
                continue;
            }
            if (detail) {
                *detail = QStringLiteral("%1 loaded").arg(cand);
            }
            return true;
        }
        if (detail) {
            *detail = localDetail;
        }
        clear();
        return false;
#endif
    }

    void clear() {
        candidate.clear();
        initFn = nullptr;
        finiFn = nullptr;
        zpoolIterFn = nullptr;
        zpoolGetNameFn = nullptr;
        zpoolCloseFn = nullptr;
        zfsIterRootFn = nullptr;
        zfsIterFilesystemsFn = nullptr;
        zfsIterSnapshotsFn = nullptr;
        zfsGetNameFn = nullptr;
        zfsIsMountedFn = nullptr;
        zfsCloseFn = nullptr;
        zfsNameToPropFn = nullptr;
        zfsPropGetFn = nullptr;
    }
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

struct ParsedPermissionsEntry {
    QStringList createPermissions;
    QVector<zfsmgr::daemonrpc::PermissionGrantJson> localGrants;
    QVector<zfsmgr::daemonrpc::PermissionGrantJson> descendantGrants;
    QVector<zfsmgr::daemonrpc::PermissionGrantJson> localDescendantGrants;
    QVector<zfsmgr::daemonrpc::PermissionSetJson> permissionSets;
};

enum class PermissionsSection {
    None,
    Local,
    Descendant,
    LocalDescendant,
    Create,
    Sets,
};

ParsedPermissionsEntry parsePermissionsOutput(const QString& out) {
    ParsedPermissionsEntry entry;
    PermissionsSection section = PermissionsSection::None;

    for (const QString& rawLine : out.split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('-'))) {
            continue;
        }
        const QString lower = line.toLower();
        if (lower.contains(QStringLiteral("local permissions:"))) {
            section = PermissionsSection::Local;
            continue;
        }
        if (lower.contains(QStringLiteral("descendent permissions:"))
            || lower.contains(QStringLiteral("descendant permissions:"))) {
            section = lower.contains(QStringLiteral("local+")) ? PermissionsSection::LocalDescendant
                                                               : PermissionsSection::Descendant;
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
            zfsmgr::daemonrpc::PermissionSetJson set;
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
        zfsmgr::daemonrpc::PermissionGrantJson grant;
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

QString libzfsStringProp(LibZfsBackend& backend, void* handle, const QString& propName) {
    if (!backend.zfsNameToPropFn || !backend.zfsPropGetFn || !handle) {
        return {};
    }
    const int propId = backend.zfsNameToPropFn(propName.toUtf8().constData());
    if (propId < 0) {
        return {};
    }
    char value[4096];
    char statbuf[4096];
    std::memset(value, 0, sizeof(value));
    std::memset(statbuf, 0, sizeof(statbuf));
    int src = 0;
    if (backend.zfsPropGetFn(handle, propId, value, sizeof(value), &src, statbuf, sizeof(statbuf), 1) != 0) {
        return {};
    }
    return QString::fromUtf8(value).trimmed();
}

QString inferDatasetType(const QString& name) {
    return name.contains(QLatin1Char('@')) ? QStringLiteral("snapshot") : QStringLiteral("filesystem");
}

QString poolNameFromDataset(const QString& datasetName) {
    const int slash = datasetName.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        return datasetName.section(QLatin1Char('@'), 0, 0);
    }
    return datasetName.left(slash);
}

QVector<zfsmgr::daemonrpc::DSInfoJson> listDatasetsForPool(const QString& poolName, QString* detail);

QJsonObject buildDatasetProps(LibZfsBackend& backend, void* zh, const QString& datasetName) {
    QJsonObject props;
    const QStringList names = {
        QStringLiteral("used"),
        QStringLiteral("compressratio"),
        QStringLiteral("encryption"),
        QStringLiteral("creation"),
        QStringLiteral("referenced"),
        QStringLiteral("mounted"),
        QStringLiteral("mountpoint"),
        QStringLiteral("canmount"),
    };
    for (const QString& propName : names) {
        const QString value = libzfsStringProp(backend, zh, propName);
        if (!value.isEmpty()) {
            props.insert(propName, value);
        }
    }
    QString mountedValue = props.value(QStringLiteral("mounted")).toString();
    QString mountpoint = props.value(QStringLiteral("mountpoint")).toString();
    if (backend.zfsIsMountedFn) {
        char* where = nullptr;
        const int mounted = backend.zfsIsMountedFn(zh, &where);
        if (mounted >= 0) {
            mountedValue = mounted ? QStringLiteral("yes") : QStringLiteral("no");
            props.insert(QStringLiteral("mounted"), mountedValue);
            if (mounted && where && *where) {
                mountpoint = QString::fromUtf8(where);
                props.insert(QStringLiteral("mountpoint"), mountpoint);
            }
        }
    }
    const bool mountpointVisible =
        !mountpoint.trimmed().isEmpty() && mountpoint.trimmed() != QStringLiteral("none")
        && mountpoint.trimmed() != QStringLiteral("legacy")
        && mountpoint.trimmed() != QStringLiteral("-");
    props.insert(QStringLiteral("mountpointVisible"), mountpointVisible);
    props.insert(QStringLiteral("dataset"), datasetName);
    return props;
}

QMap<QString, QJsonObject> loadGsaPropsByDataset(const QString& rootName) {
    QMap<QString, QJsonObject> out;
    const QString zfsExe = QStandardPaths::findExecutable(QStringLiteral("zfs"));
    if (zfsExe.isEmpty()) {
        return out;
    }
    QProcess proc;
    proc.setProgram(zfsExe);
    proc.setArguments({
        QStringLiteral("get"),
        QStringLiteral("-H"),
        QStringLiteral("-o"),
        QStringLiteral("name,property,value"),
        QStringLiteral("-r"),
        QStringLiteral("org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:nivelar,org.fc16.gsa:destino,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual"),
        rootName,
    });
    proc.start();
    if (!proc.waitForFinished(5000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        proc.kill();
        proc.waitForFinished(1000);
        return out;
    }
    const QStringList lines = QString::fromUtf8(proc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList fields = line.split('\t');
        if (fields.size() < 3) {
            continue;
        }
        const QString dataset = fields.value(0).trimmed();
        const QString property = fields.value(1).trimmed();
        const QString value = fields.value(2).trimmed();
        if (!dataset.isEmpty() && property.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
            out[dataset].insert(property, value);
        }
    }
    return out;
}

zfsmgr::daemonrpc::GsaPlanJson gsaPlanFromProps(const QJsonObject& props) {
    zfsmgr::daemonrpc::GsaPlanJson plan;
    const auto isOn = [](const QString& value) {
        const QString low = value.trimmed().toLower();
        return low == QStringLiteral("on") || low == QStringLiteral("yes") || low == QStringLiteral("true");
    };
    plan.enabled = isOn(props.value(QStringLiteral("org.fc16.gsa:activado")).toString());
    plan.recursive = isOn(props.value(QStringLiteral("org.fc16.gsa:recursivo")).toString());
    const auto addClass = [&](const QString& key, const QString& name) {
        bool ok = false;
        const int value = props.value(key).toString().toInt(&ok);
        if (ok && value > 0) {
            plan.classes.push_back(name);
        }
    };
    addClass(QStringLiteral("org.fc16.gsa:horario"), QStringLiteral("hourly"));
    addClass(QStringLiteral("org.fc16.gsa:diario"), QStringLiteral("daily"));
    addClass(QStringLiteral("org.fc16.gsa:semanal"), QStringLiteral("weekly"));
    addClass(QStringLiteral("org.fc16.gsa:mensual"), QStringLiteral("monthly"));
    addClass(QStringLiteral("org.fc16.gsa:anual"), QStringLiteral("yearly"));
    const QString dest = props.value(QStringLiteral("org.fc16.gsa:destino")).toString().trimmed();
    if (!dest.isEmpty()) {
        const QString connection = dest.section(QStringLiteral("::"), 0, 0);
        const QString rest = dest.section(QStringLiteral("::"), 1);
        const QString pool = rest.section(QLatin1Char('/'), 0, 0);
        const QString dataset = rest.section(QLatin1Char('/'), 1);
        if (!connection.isEmpty() && !pool.isEmpty() && !dataset.isEmpty()) {
            plan.destinations.push_back({connection, pool, dataset});
        }
    }
    return plan;
}

QVector<zfsmgr::daemonrpc::PoolInfoJson> listPools(QString* detail) {
    QVector<zfsmgr::daemonrpc::PoolInfoJson> pools;
    LibZfsBackend backend;
    if (!backend.load(detail)) {
        return pools;
    }
    void* h = backend.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(backend.candidate);
        }
        backend.lib.unload();
        backend.clear();
        return pools;
    }
    struct PoolCtx {
        QVector<zfsmgr::daemonrpc::PoolInfoJson>* pools;
        LibZfsBackend* backend;
    } ctx{&pools, &backend};
    auto cb = [](void* poolHandle, void* opaque) -> int {
        auto* c = static_cast<PoolCtx*>(opaque);
        if (!c || !poolHandle || !c->backend || !c->backend->zpoolGetNameFn) {
            return 0;
        }
        const char* rawName = c->backend->zpoolGetNameFn(poolHandle);
        const QString poolName = rawName ? QString::fromUtf8(rawName).trimmed() : QString();
        if (poolName.isEmpty()) {
            return 0;
        }
        zfsmgr::daemonrpc::PoolInfoJson info;
        info.pool = poolName;
        info.state = QStringLiteral("ONLINE");
        info.version = 0;
        info.guid = poolName;
        info.importable = false;
        info.comment = QStringLiteral("imported via libzfs");
        const QVector<zfsmgr::daemonrpc::DSInfoJson> datasets = listDatasetsForPool(poolName, nullptr);
        QSet<QString> childSet;
        for (const auto& ds : datasets) {
            if (ds.dataset.contains(QLatin1Char('@'))) {
                continue;
            }
            if (!ds.dataset.startsWith(poolName + QLatin1Char('/'))) {
                continue;
            }
            const QString rel = ds.dataset.mid(poolName.size() + 1);
            if (!rel.isEmpty() && !rel.contains(QLatin1Char('/'))) {
                childSet.insert(ds.dataset);
            }
        }
        info.children = childSet.values();
        std::sort(info.children.begin(), info.children.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
        c->pools->push_back(info);
        c->backend->zpoolCloseFn(poolHandle);
        return 0;
    };
    (void)backend.zpoolIterFn(h, cb, &ctx);
    backend.finiFn(h);
    backend.lib.unload();
    backend.clear();
    return pools;
}

struct DatasetWalkCtx {
    QVector<zfsmgr::daemonrpc::DSInfoJson>* datasets{nullptr};
    QMap<QString, int> indexByName;
    QSet<QString> seen;
    LibZfsBackend* backend{nullptr};
    QString pool;
    QMap<QString, QJsonObject> gsaPropsByDataset;
};

static int walkDatasetHandle(void* zh, void* opaque) {
    auto* c = static_cast<DatasetWalkCtx*>(opaque);
    if (!c || !zh || !c->backend || !c->backend->zfsGetNameFn) {
        return 0;
    }
    const char* rawName = c->backend->zfsGetNameFn(zh);
    const QString datasetName = rawName ? QString::fromUtf8(rawName).trimmed() : QString();
    if (datasetName.isEmpty() || c->seen.contains(datasetName)) {
        if (c->backend->zfsCloseFn) {
            c->backend->zfsCloseFn(zh);
        }
        return 0;
    }
    const QString dsPool = poolNameFromDataset(datasetName);
    if (!c->pool.isEmpty() && dsPool != c->pool) {
        if (c->backend->zfsCloseFn) {
            c->backend->zfsCloseFn(zh);
        }
        return 0;
    }
    c->seen.insert(datasetName);

    zfsmgr::daemonrpc::DSInfoJson info;
    info.dataset = datasetName;
    info.type = inferDatasetType(datasetName);
    info.properties = buildDatasetProps(*c->backend, zh, datasetName);
    const auto gsaIt = c->gsaPropsByDataset.constFind(datasetName);
    if (gsaIt != c->gsaPropsByDataset.cend()) {
        for (auto propIt = gsaIt->begin(); propIt != gsaIt->end(); ++propIt) {
            info.properties.insert(propIt.key(), propIt.value());
        }
    }
    info.mountpointVisible = info.properties.value(QStringLiteral("mountpointVisible")).toBool();
    info.mountpoint = info.properties.value(QStringLiteral("mountpoint")).toString();
    info.canmount = info.properties.value(QStringLiteral("canmount")).toString();
    info.mounted = info.properties.value(QStringLiteral("mounted")).toString();
    info.encryption = info.properties.value(QStringLiteral("encryption")).toString();
    c->indexByName.insert(datasetName, c->datasets->size());
    c->datasets->push_back(info);

    if (c->backend->zfsIterFilesystemsFn) {
        c->backend->zfsIterFilesystemsFn(zh, walkDatasetHandle, opaque);
    }
    if (c->backend->zfsIterSnapshotsFn) {
        c->backend->zfsIterSnapshotsFn(zh, walkDatasetHandle, opaque, false);
    }
    if (datasetName.contains(QLatin1Char('@'))) {
        const QString parent = datasetName.section(QLatin1Char('@'), 0, 0);
        if (c->indexByName.contains(parent)) {
            (*c->datasets)[c->indexByName.value(parent)].snapshots.push_back(datasetName);
            (*c->datasets)[c->indexByName.value(parent)].snapshots.removeDuplicates();
        }
    }
    if (c->backend->zfsCloseFn) {
        c->backend->zfsCloseFn(zh);
    }
    return 0;
}

QVector<zfsmgr::daemonrpc::DSInfoJson> listDatasetsForPool(const QString& poolName, QString* detail) {
    QVector<zfsmgr::daemonrpc::DSInfoJson> datasets;
    if (poolName.trimmed().isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("missing pool");
        }
        return datasets;
    }
    LibZfsBackend backend;
    if (!backend.load(detail)) {
        return datasets;
    }
    void* h = backend.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(backend.candidate);
        }
        backend.lib.unload();
        backend.clear();
        return datasets;
    }
    DatasetWalkCtx ctx;
    ctx.datasets = &datasets;
    ctx.backend = &backend;
    ctx.pool = poolName;
    ctx.gsaPropsByDataset = loadGsaPropsByDataset(poolName);
    (void)backend.zfsIterRootFn(h, walkDatasetHandle, &ctx);
    for (auto& ds : datasets) {
        ds.snapshots.removeDuplicates();
    }
    backend.finiFn(h);
    backend.lib.unload();
    backend.clear();
    std::sort(datasets.begin(), datasets.end(), [](const auto& a, const auto& b) {
        return a.dataset.compare(b.dataset, Qt::CaseInsensitive) < 0;
    });
    return datasets;
}

QVector<zfsmgr::daemonrpc::GsaPlanJson> listGsaPlansForPool(const QString& poolName, QString* detail) {
    QVector<zfsmgr::daemonrpc::GsaPlanJson> plans;
    const QMap<QString, QJsonObject> propsByDataset = loadGsaPropsByDataset(poolName);
    for (auto it = propsByDataset.cbegin(); it != propsByDataset.cend(); ++it) {
        const QString dataset = it.key();
        if (!poolName.isEmpty() && !dataset.startsWith(poolName + QLatin1Char('/')) && dataset != poolName) {
            continue;
        }
        const zfsmgr::daemonrpc::GsaPlanJson plan = gsaPlanFromProps(it.value());
        if (plan.enabled || plan.recursive || !plan.classes.isEmpty() || !plan.destinations.isEmpty()) {
            plans.push_back(plan);
        }
    }
    if (plans.isEmpty() && detail) {
        *detail = QStringLiteral("no GSA properties found");
    }
    return plans;
}

QJsonObject datasetPropertiesPayload(const QString& datasetName, QString* detail) {
    QJsonObject payload;
    LibZfsBackend backend;
    if (!backend.load(detail)) {
        return payload;
    }
    void* h = backend.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(backend.candidate);
        }
        backend.lib.unload();
        backend.clear();
        return payload;
    }
    void* zh = backend.zfsOpenFn(h, datasetName.toUtf8().constData(), 0xFFFF);
    if (!zh) {
        backend.finiFn(h);
        backend.lib.unload();
        backend.clear();
        if (detail) {
            *detail = QStringLiteral("zfs_open(%1) failed").arg(datasetName);
        }
        return payload;
    }

    QJsonObject props = buildDatasetProps(backend, zh, datasetName);
    const QJsonObject gsa = loadGsaPropsByDataset(poolNameFromDataset(datasetName)).value(datasetName);
    for (auto it = gsa.begin(); it != gsa.end(); ++it) {
        props.insert(it.key(), it.value());
    }
    payload.insert(QStringLiteral("object"), datasetName);
    payload.insert(QStringLiteral("datasetType"), inferDatasetType(datasetName));
    payload.insert(QStringLiteral("properties"), props);
    payload.insert(QStringLiteral("gsaStatus"), gsa.isEmpty() ? QJsonValue::Null : QJsonValue(gsa));

    backend.zfsCloseFn(zh);
    backend.finiFn(h);
    backend.lib.unload();
    backend.clear();
    return payload;
}

zfsmgr::daemonrpc::PoolDetailsJson poolDetailsPayload(const QString& poolName, QString* detail) {
    zfsmgr::daemonrpc::PoolDetailsJson payload;
    payload.pool = poolName.trimmed();
    const QString zpoolExe = QStandardPaths::findExecutable(QStringLiteral("zpool"));
    if (zpoolExe.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("zpool executable not found");
        }
        return payload;
    }

    QProcess propsProc;
    propsProc.setProgram(zpoolExe);
    propsProc.setArguments({
        QStringLiteral("get"),
        QStringLiteral("-H"),
        QStringLiteral("-o"),
        QStringLiteral("property,value,source"),
        QStringLiteral("all"),
        poolName,
    });
    propsProc.start();
    if (propsProc.waitForFinished(5000) && propsProc.exitStatus() == QProcess::NormalExit && propsProc.exitCode() == 0) {
        const QStringList lines = QString::fromUtf8(propsProc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList fields = line.split('\t');
            if (fields.size() < 3) {
                continue;
            }
            payload.propsRows.push_back(QStringList{fields.value(0).trimmed(), fields.value(1).trimmed(), fields.value(2).trimmed()});
        }
    } else if (detail) {
        *detail = QStringLiteral("failed to run zpool get");
    }

    QProcess statusProc;
    statusProc.setProgram(zpoolExe);
    statusProc.setArguments({
        QStringLiteral("status"),
        QStringLiteral("-v"),
        poolName,
    });
    statusProc.start();
    if (statusProc.waitForFinished(5000) && statusProc.exitStatus() == QProcess::NormalExit && statusProc.exitCode() == 0) {
        payload.statusText = QString::fromUtf8(statusProc.readAllStandardOutput()).trimmed();
    } else if (payload.statusText.trimmed().isEmpty()) {
        payload.statusText = QStringLiteral("failed to run zpool status");
    }
    return payload;
}

zfsmgr::daemonrpc::PermissionsJson permissionsPayload(const QString& datasetName, QString* detail) {
    zfsmgr::daemonrpc::PermissionsJson payload;
    payload.dataset = datasetName.trimmed();
    const QString zfsExe = QStandardPaths::findExecutable(QStringLiteral("zfs"));
    if (zfsExe.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("zfs executable not found");
        }
        return payload;
    }

    QProcess proc;
    proc.setProgram(zfsExe);
    proc.setArguments({QStringLiteral("allow"), QStringLiteral("-H"), datasetName});
    proc.start();
    if (!proc.waitForFinished(15000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (detail) {
            *detail = QStringLiteral("failed to run zfs allow");
        }
        return payload;
    }

    const QString rawOut = QString::fromUtf8(proc.readAllStandardOutput());
    const ParsedPermissionsEntry parsed = parsePermissionsOutput(rawOut);
    for (const auto& grant : parsed.localGrants) {
        payload.localGrants.push_back(grant);
    }
    for (const auto& grant : parsed.descendantGrants) {
        payload.descendantGrants.push_back(grant);
    }
    for (const auto& grant : parsed.localDescendantGrants) {
        payload.localDescendantGrants.push_back(grant);
    }
    payload.createPermissions = parsed.createPermissions;
    payload.permissionSets = parsed.permissionSets;

    QProcess usersProc;
    usersProc.setProgram(QStringLiteral("sh"));
    usersProc.setArguments({
        QStringLiteral("-lc"),
        QStringLiteral("((getent passwd 2>/dev/null || cat /etc/passwd 2>/dev/null) | cut -d: -f1)")
    });
    usersProc.start();
    if (usersProc.waitForFinished(5000) && usersProc.exitStatus() == QProcess::NormalExit && usersProc.exitCode() == 0) {
        payload.systemUsers = QString::fromUtf8(usersProc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        for (QString& user : payload.systemUsers) {
            user = user.trimmed();
        }
        payload.systemUsers.removeAll(QString());
        payload.systemUsers.removeDuplicates();
        std::sort(payload.systemUsers.begin(), payload.systemUsers.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
    }
    QProcess groupsProc;
    groupsProc.setProgram(QStringLiteral("sh"));
    groupsProc.setArguments({
        QStringLiteral("-lc"),
        QStringLiteral("((getent group 2>/dev/null || cat /etc/group 2>/dev/null) | cut -d: -f1)")
    });
    groupsProc.start();
    if (groupsProc.waitForFinished(5000) && groupsProc.exitStatus() == QProcess::NormalExit && groupsProc.exitCode() == 0) {
        payload.systemGroups = QString::fromUtf8(groupsProc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        for (QString& group : payload.systemGroups) {
            group = group.trimmed();
        }
        payload.systemGroups.removeAll(QString());
        payload.systemGroups.removeDuplicates();
        std::sort(payload.systemGroups.begin(), payload.systemGroups.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
    }
    return payload;
}

zfsmgr::daemonrpc::HoldsJson holdsPayload(const QString& objectName, QString* detail) {
    zfsmgr::daemonrpc::HoldsJson payload;
    payload.dataset = objectName.trimmed();
    const QString zfsExe = QStandardPaths::findExecutable(QStringLiteral("zfs"));
    if (zfsExe.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("zfs executable not found");
        }
        return payload;
    }

    QProcess proc;
    proc.setProgram(zfsExe);
    proc.setArguments({QStringLiteral("holds"), QStringLiteral("-H"), objectName});
    proc.start();
    if (!proc.waitForFinished(5000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (detail) {
            *detail = QStringLiteral("failed to run zfs holds");
        }
        return payload;
    }
    const QStringList lines = QString::fromUtf8(proc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    QSet<QString> seen;
    for (const QString& line : lines) {
        const QStringList fields = line.split('\t');
        if (fields.size() < 2) {
            continue;
        }
        const QString tag = fields.value(1).trimmed();
        const QString timestamp = fields.value(2).trimmed();
        const QString key = tag.toLower();
        if (tag.isEmpty() || seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        payload.holds.push_back({tag, timestamp});
    }
    return payload;
}

} // namespace

static bool handleRequest(const QByteArray& buffer, QString& response) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(buffer, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject obj = doc.object();
    const QString id = obj.value(QStringLiteral("id")).toString();
    const QString method = obj.value(QStringLiteral("method")).toString();
    const QJsonObject params = obj.value(QStringLiteral("params")).toObject();

    if (method == QStringLiteral("/health")) {
        response = jsonResult(
            id,
            200,
            QStringLiteral("ok"),
            QJsonObject{
                {QStringLiteral("status"), QStringLiteral("healthy")},
                {QStringLiteral("protocolVersion"), 1},
                {QStringLiteral("supportedMethods"), QJsonArray{
                     QStringLiteral("/health"),
                     QStringLiteral("/exec"),
                     QStringLiteral("/pools"),
                     QStringLiteral("/datasets"),
                     QStringLiteral("/datasets/properties"),
                     QStringLiteral("/gsa"),
                 }},
            });
        return true;
    }

    if (method == QStringLiteral("/exec")) {
        response = jsonResult(
            id,
            200,
            QStringLiteral("executed"),
            QJsonObject{
                {QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("command"), params.value(QStringLiteral("cmd")).toString()},
            });
        return true;
    }

    if (method == QStringLiteral("/pools")) {
        const QVector<zfsmgr::daemonrpc::PoolInfoJson> pools = listPools(nullptr);
        QJsonArray arr;
        for (const auto& pool : pools) {
            arr.push_back(pool.toJsonObject());
        }
        response = jsonResult(id, 200, QStringLiteral("ok"), QJsonObject{{QStringLiteral("pools"), arr}});
        return true;
    }

    if (method == QStringLiteral("/datasets")) {
        QString poolName = params.value(QStringLiteral("pool")).toString().trimmed();
        if (poolName.isEmpty()) {
            poolName = poolNameFromDataset(params.value(QStringLiteral("dataset")).toString().trimmed());
        }
        const QVector<zfsmgr::daemonrpc::DSInfoJson> datasets = listDatasetsForPool(poolName, nullptr);
        QJsonArray arr;
        for (const auto& ds : datasets) {
            arr.push_back(ds.toJsonObject());
        }
        response = jsonResult(id, 200, QStringLiteral("ok"), QJsonObject{{QStringLiteral("datasets"), arr}});
        return true;
    }

    if (method == QStringLiteral("/datasets/properties")) {
        const QString datasetName = params.value(QStringLiteral("dataset")).toString().trimmed();
        if (datasetName.isEmpty()) {
            response = jsonResult(id, 400, QStringLiteral("missing dataset"));
            return true;
        }
        QString detail;
        const QJsonObject payload = datasetPropertiesPayload(datasetName, &detail);
        if (payload.isEmpty()) {
            response = jsonResult(id, 404, detail.isEmpty() ? QStringLiteral("dataset not found") : detail);
            return true;
        }
        response = jsonResult(id, 200, QStringLiteral("ok"), payload);
        return true;
    }

    if (method == QStringLiteral("/gsa")) {
        QString poolName = params.value(QStringLiteral("pool")).toString().trimmed();
        if (poolName.isEmpty()) {
            poolName = poolNameFromDataset(params.value(QStringLiteral("dataset")).toString().trimmed());
        }
        const QVector<zfsmgr::daemonrpc::GsaPlanJson> plans = listGsaPlansForPool(poolName, nullptr);
        QJsonArray arr;
        for (const auto& plan : plans) {
            arr.push_back(plan.toJsonObject());
        }
        response = jsonResult(id, 200, QStringLiteral("ok"), QJsonObject{{QStringLiteral("plans"), arr}});
        return true;
    }

    if (method == QStringLiteral("/permissions")) {
        const QString datasetName = params.value(QStringLiteral("dataset")).toString().trimmed();
        if (datasetName.isEmpty()) {
            response = jsonResult(id, 400, QStringLiteral("missing dataset"));
            return true;
        }
        QString detail;
        const zfsmgr::daemonrpc::PermissionsJson payload = permissionsPayload(datasetName, &detail);
        response = jsonResult(id, 200, detail.isEmpty() ? QStringLiteral("ok") : detail, payload.toJsonObject());
        return true;
    }

    if (method == QStringLiteral("/holds")) {
        const QString objectName = params.value(QStringLiteral("dataset")).toString().trimmed();
        if (objectName.isEmpty()) {
            response = jsonResult(id, 400, QStringLiteral("missing dataset"));
            return true;
        }
        QString detail;
        const zfsmgr::daemonrpc::HoldsJson payload = holdsPayload(objectName, &detail);
        response = jsonResult(id, 200, detail.isEmpty() ? QStringLiteral("ok") : detail, payload.toJsonObject());
        return true;
    }

    if (method == QStringLiteral("/pools/details")) {
        const QString poolName = params.value(QStringLiteral("pool")).toString().trimmed();
        if (poolName.isEmpty()) {
            response = jsonResult(id, 400, QStringLiteral("missing pool"));
            return true;
        }
        QString detail;
        const zfsmgr::daemonrpc::PoolDetailsJson payload = poolDetailsPayload(poolName, &detail);
        response = jsonResult(id, 200, detail.isEmpty() ? QStringLiteral("ok") : detail, payload.toJsonObject());
        return true;
    }

    response = jsonResult(id, 404, QStringLiteral("not implemented"));
    return true;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"port", "Port to listen on", "port", QString::number(kDefaultPort)});
    parser.addOption({"hostkey", "Path to host key", "hostkey", "/etc/zfsmgr/host_rsa"});
    parser.addOption({"fingerprint", "Allowed fingerprint base64", "fingerprint", QStringLiteral("")});
    parser.process(app);

    const int port = parser.value("port").toInt();
    const QString hostKey = parser.value("hostkey");
    const QString allowedFingerprint = parser.value("fingerprint").toLower();
    if (hostKey.isEmpty() || allowedFingerprint.isEmpty()) {
        qWarning() << "hostkey and fingerprint are required";
        return 1;
    }

    ssh_bind bind = ssh_bind_new();
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, "0.0.0.0");
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_RSAKEY, hostKey.toUtf8().constData());
    if (ssh_bind_listen(bind) != SSH_OK) {
        qWarning() << "unable to bind" << ssh_get_error(bind);
        ssh_bind_free(bind);
        return 1;
    }

    while (true) {
        ssh_session session = ssh_new();
        if (ssh_bind_accept(bind, session) != SSH_OK) {
            qWarning() << "accept failed" << ssh_get_error(bind);
            ssh_free(session);
            continue;
        }
        if (ssh_handle_key_exchange(session) != SSH_OK) {
            qWarning() << "key exchange failed" << ssh_get_error(session);
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }
        unsigned char* hash = nullptr;
        size_t hlen = 0;
        ssh_key pubkey = ssh_get_publickey(session);
        bool authorized = false;
        if (pubkey && ssh_get_publickey_hash(pubkey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == SSH_OK) {
            const QByteArray fingerprint = QByteArray::fromRawData(reinterpret_cast<char*>(hash), hlen).toBase64();
            ssh_clean_pubkey_hash(&hash);
            authorized = fingerprint.toLower() == allowedFingerprint;
        }
        if (!authorized) {
            qWarning() << "unauthorized fingerprint";
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }
        ssh_channel channel = ssh_channel_new(session);
        if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
            qWarning() << "channel open failed" << ssh_get_error(session);
            if (channel) {
                ssh_channel_free(channel);
            }
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }
        QByteArray payload;
        char buffer[512];
        int n;
        while ((n = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            payload.append(buffer, n);
        }
        QString response;
        if (!handleRequest(payload, response)) {
            response = jsonResult(QStringLiteral("-"), 400, QStringLiteral("bad request"));
        }
        ssh_channel_write(channel, response.toUtf8().constData(), response.size());
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
    }
    ssh_bind_free(bind);
    return 0;
}
