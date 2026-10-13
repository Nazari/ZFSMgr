#include "mainwindow.h"
#include "agentversion.h"
#include "mainwindow_helpers.h"
#include "listings.h"
#include "requests.h"
#include "daemonpayload.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::oneLine;
using mwhelpers::parentDatasetName;
using mwhelpers::shSingleQuote;

QString normalizeHostTokenLocal(QString host) {
    host = host.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']') && host.size() > 2) {
        host = host.mid(1, host.size() - 2);
    }
    while (host.endsWith('.')) {
        host.chop(1);
    }
    return host;
}

bool isLocalHostTokenLocal(const QString& host) {
    const QString h = normalizeHostTokenLocal(host);
    if (h.isEmpty()) {
        return false;
    }
    if (h == QStringLiteral("localhost") || h == QStringLiteral("127.0.0.1") || h == QStringLiteral("::1")) {
        return true;
    }
    static const QSet<QString> aliases = []() {
        QSet<QString> s;
        s.insert(QStringLiteral("localhost"));
        s.insert(QStringLiteral("127.0.0.1"));
        s.insert(QStringLiteral("::1"));
        const QString local = normalizeHostTokenLocal(QSysInfo::machineHostName());
        if (!local.isEmpty()) {
            s.insert(local);
            s.insert(local + QStringLiteral(".local"));
            const int dot = local.indexOf('.');
            if (dot > 0) {
                const QString shortName = local.left(dot);
                s.insert(shortName);
                s.insert(shortName + QStringLiteral(".local"));
            }
        }
        return s;
    }();
    return aliases.contains(h);
}

// Tercera copia retirada: ahora es `currentLocalMachineUid()` de connectionstore.h, que es
// la única que lee el registro en Windows. Esta caía a `QSysInfo` allí, así que comparaba
// contra un identificador distinto del que se había guardado.
QString currentLocalMachineUidFallback() {
    return currentLocalMachineUid();
}

QStringList datasetSizePropertyNames() {
    return {
        QStringLiteral("used"),
        QStringLiteral("available"),
        QStringLiteral("referenced"),
        QStringLiteral("logicalused"),
        QStringLiteral("logicalreferenced"),
        QStringLiteral("written"),
        QStringLiteral("quota"),
        QStringLiteral("refquota"),
        QStringLiteral("reservation"),
        QStringLiteral("refreservation"),
        QStringLiteral("volsize"),
    };
}

QStringList poolSizePropertyNames() {
    return {
        QStringLiteral("size"),
        QStringLiteral("alloc"),
        QStringLiteral("free"),
        QStringLiteral("cap"),
        QStringLiteral("fragmentation"),
    };
}

bool containsAnyToken(const QString& haystackLower, const QStringList& needles) {
    for (const QString& token : needles) {
        if (haystackLower.contains(token)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool MainWindow::ensureLocalSudoCredentials(ConnectionProfile& profile) {
    if (!isLocalConnection(profile) || isWindowsConnection(profile) || !profile.useSudo) {
        return true;
    }

    QString localUid = m_localMachineUuid.trimmed();
    if (localUid.isEmpty()) {
        localUid = currentLocalMachineUidFallback();
    }

    for (int i = 0; i < m_conns.profiles.size(); ++i) {
        if (isLocalConnection(i) || i >= m_conns.states.size()) {
            continue;
        }
        const ConnectionProfile candidate = m_conns.profiles[i];
        if (!isConnectionRedirectedToLocal(i)) {
            continue;
        }
        if (candidate.username.trimmed().isEmpty() || candidate.password.isEmpty()) {
            continue;
        }
        profile.username = candidate.username;
        profile.password = candidate.password;
        m_localSudoUsername = candidate.username;
        m_localSudoPassword = candidate.password;
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Credenciales sudo locales tomadas de conexión redirigida: %1").arg(candidate.name));
        return true;
    }

    for (int i = 0; i < m_conns.profiles.size(); ++i) {
        const ConnectionProfile candidate = m_conns.profiles[i];
        if (!isLocalConnection(candidate)) {
            continue;
        }
        const QString candUid = candidate.machineUid.trimmed();
        if (!localUid.isEmpty() && !candUid.isEmpty() && candUid.compare(localUid, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (candidate.username.trimmed().isEmpty() || candidate.password.isEmpty()) {
            continue;
        }
        profile.username = candidate.username;
        profile.password = candidate.password;
        m_localSudoUsername = candidate.username;
        m_localSudoPassword = candidate.password;
        appLog(QStringLiteral("INFO"), QStringLiteral("Credenciales sudo locales tomadas de config.json (Local)"));
        return true;
    }

    bool hasConfiguredRedirect = false;
    for (int i = 0; i < m_conns.profiles.size(); ++i) {
        const ConnectionProfile candidate = m_conns.profiles[i];
        if (isLocalConnection(candidate)) {
            continue;
        }
        const bool byUid = !localUid.isEmpty() && !candidate.machineUid.trimmed().isEmpty()
                           && candidate.machineUid.trimmed().compare(localUid, Qt::CaseInsensitive) == 0;
        const bool byHost = isLocalHostTokenLocal(candidate.host);
        if (!byUid && !byHost) {
            continue;
        }
        hasConfiguredRedirect = true;
        if (!candidate.username.trimmed().isEmpty() && !candidate.password.isEmpty()) {
            profile.username = candidate.username;
            profile.password = candidate.password;
            m_localSudoUsername = candidate.username;
            m_localSudoPassword = candidate.password;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Credenciales sudo locales tomadas de redirección configurada: %1").arg(candidate.name));
            return true;
        }
    }

    if (hasConfiguredRedirect) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_local_sudo_cfg1"),
                QStringLiteral("Hay una conexión redirigida a Local en config.json, pero no tiene usuario/password.\n"
                               "Edite esa conexión o complete sus credenciales."),
                QStringLiteral("There is a connection redirected to Local in config.json, but it has no user/password.\n"
                               "Edit that connection or complete its credentials.")));
        appLog(QStringLiteral("WARN"), QStringLiteral("Credenciales sudo locales no disponibles: redirección configurada sin credenciales"));
        return false;
    }

    if (!m_localSudoUsername.trimmed().isEmpty() && !m_localSudoPassword.isEmpty()) {
        profile.username = m_localSudoUsername;
        profile.password = m_localSudoPassword;
        return true;
    }

    // Aquí se PREGUNTA, y preguntar es lo único que ataba esta cadena a que hubiera una
    // ventana. Ahora se pide al proveedor de la sesión: la interfaz pone uno que abre un
    // diálogo; un CLI, uno que lee del descriptor o pregunta por terminal.
    QString usuarioPedido;
    QString clavePedida;
    if (!m_transport.askCredentials(
            trk(QStringLiteral("t_local_sudo_dlg1"),
                QStringLiteral("Credenciales sudo locales"),
                QStringLiteral("Local sudo credentials")),
            usuarioPedido, clavePedida)) {
        return false;
    }
    if (usuarioPedido.trimmed().isEmpty() || clavePedida.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_local_sudo_req1"),
                                 QStringLiteral("Usuario y password sudo son obligatorios."),
                                 QStringLiteral("Sudo user and password are required.")));
        return false;
    }
    m_localSudoUsername = usuarioPedido.trimmed();
    m_localSudoPassword = clavePedida;
    profile.username = m_localSudoUsername;
    profile.password = m_localSudoPassword;
    appLog(QStringLiteral("INFO"), QStringLiteral("Credenciales sudo locales guardadas en memoria"));

    ConnectionProfile localCfg;
    bool foundLocal = false;
    for (const ConnectionProfile& p : m_conns.profiles) {
        if (!isLocalConnection(p)) {
            continue;
        }
        localCfg = p;
        foundLocal = true;
        break;
    }
    if (!foundLocal) {
        localCfg.id = QStringLiteral("local");
        localCfg.name = QStringLiteral("Local");
        localCfg.connType = QStringLiteral("LOCAL");
        if (!profile.osType.isEmpty()) {
            localCfg.osType = profile.osType;
        } else {
#ifdef Q_OS_WIN
            localCfg.osType = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
            localCfg.osType = QStringLiteral("macOS");
#elif defined(Q_OS_FREEBSD)
            localCfg.osType = QStringLiteral("FreeBSD");
#else
            localCfg.osType = QStringLiteral("Linux");
#endif
        }
        localCfg.host = QStringLiteral("localhost");
        localCfg.port = 0;
        localCfg.useSudo = true;
    }
    localCfg.machineUid = localUid;
    localCfg.username = m_localSudoUsername;
    localCfg.password = m_localSudoPassword;
    if (!localCfg.useSudo) {
        localCfg.useSudo = true;
    }
    QString storeErr;
    if (!m_conns.store.upsertConnection(localCfg, storeErr)) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("No se pudieron persistir credenciales sudo locales en config.json: %1").arg(oneLine(storeErr)));
    } else {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Credenciales sudo locales persistidas en config.json para machine_uid=%1")
                   .arg(localUid.isEmpty() ? QStringLiteral("-") : localUid));
        bool updated = false;
        for (ConnectionProfile& p : m_conns.profiles) {
            if (!isLocalConnection(p)) {
                continue;
            }
            p.machineUid = localCfg.machineUid;
            p.username = localCfg.username;
            p.password = localCfg.password;
            p.useSudo = localCfg.useSudo;
            updated = true;
            break;
        }
        if (!updated) {
            m_conns.profiles.push_front(localCfg);
        }
    }
    return true;
}

bool MainWindow::hasEquivalentLocalSshConnection() const {
    QString localUid = m_localMachineUuid.trimmed();
    if (localUid.isEmpty()) {
        localUid = currentLocalMachineUidFallback();
    }

    for (const ConnectionProfile& candidate : m_conns.profiles) {
        if (isLocalConnection(candidate)) {
            continue;
        }
        if (candidate.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString candUid = candidate.machineUid.trimmed();
        if (!localUid.isEmpty() && !candUid.isEmpty() && candUid.compare(localUid, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (isLocalHostTokenLocal(candidate.host)) {
            return true;
        }
    }
    return false;
}

void MainWindow::ensureStartupLocalSudoConnection() {
    if (m_startupLocalSudoChecked) {
        return;
    }
    m_startupLocalSudoChecked = true;

    for (int i = 0; i < m_conns.profiles.size(); ++i) {
        ConnectionProfile& localProfile = m_conns.profiles[i];
        if (!isLocalConnection(localProfile) || isWindowsConnection(localProfile) || !localProfile.useSudo) {
            continue;
        }
        if (hasEquivalentLocalSshConnection()) {
            return;
        }
        if (ensureLocalSudoCredentials(localProfile)) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Credenciales sudo locales comprobadas al arrancar"));
        }
        return;
    }
}

bool MainWindow::shouldRefreshSizePropsForCommand(const QString& actionLabel, const QString& command) const {
    const QString label = actionLabel.trimmed().toLower();
    const QString cmd = command.trimmed().toLower();
    if (label.isEmpty() && cmd.isEmpty()) {
        return false;
    }

    if (containsAnyToken(label,
                         {QStringLiteral("copiar"),
                          QStringLiteral("copy"),
                          QStringLiteral("clonar"),
                          QStringLiteral("clone"),
                          QStringLiteral("rollback"),
                          QStringLiteral("borrar"),
                          QStringLiteral("destroy"),
                          QStringLiteral("desglosar"),
                          QStringLiteral("ensamblar"),
                          QStringLiteral("desde dir"),
                          QStringLiteral("hacia dir"),
                          QStringLiteral("from dir"),
                          QStringLiteral("to dir"),
                          QStringLiteral("sync"),
                          QStringLiteral("sincron"),
                          QStringLiteral("aplicar propiedades")})) {
        return true;
    }

    if (containsAnyToken(cmd,
                         {QStringLiteral("zfs recv"),
                          QStringLiteral("zfs receive"),
                          QStringLiteral("zfs clone"),
                          QStringLiteral("zfs rollback"),
                          QStringLiteral("zfs destroy"),
                          QStringLiteral("zfs create"),
                          QStringLiteral("rsync "),
                          QStringLiteral("tar --"),
                          QStringLiteral("zfs set quota="),
                          QStringLiteral("zfs set refquota="),
                          QStringLiteral("zfs set reservation="),
                          QStringLiteral("zfs set refreservation="),
                          QStringLiteral("zfs set volsize=")})) {
        return true;
    }
    return false;
}

bool MainWindow::refreshDatasetAndPoolSizeProperties(int connIdx,
                                                     const QString& poolName,
                                                     const QString& datasetName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedDataset = datasetName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty() || trimmedDataset.isEmpty()) {
        return false;
    }

    bool refreshedSomething = false;
    const QStringList dsProps = datasetSizePropertyNames();
    if (ensureDatasetPropertySubsetLoaded(connIdx, trimmedPool, trimmedDataset, dsProps)) {
        const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool);
        const QString key = QStringLiteral("%1|%2").arg(token, trimmedDataset);
        QMap<QString, QString> mergedValues = m_connContentPropValuesByObject.value(key);
        const QMap<QString, QString> sizeValues =
            datasetPropertyValuesForNames(connIdx, trimmedPool, trimmedDataset, dsProps);
        for (auto it = sizeValues.cbegin(); it != sizeValues.cend(); ++it) {
            mergedValues.insert(it.key(), it.value());
        }
        if (!mergedValues.isEmpty()) {
            updateConnContentPropertyValues(token, trimmedDataset, mergedValues);
        }
        if (m_connContentTree && m_topDetailConnIdx == connIdx) {
            syncConnContentPropertyColumnsFor(m_connContentTree, token);
            if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
                syncConnContentPropertyColumnsFor(m_bottomConnContentTree, token);
            }
        }
        refreshedSomething = true;
    }

    const ConnectionProfile profile = m_conns.profiles.at(connIdx);
    QString out;
    QString err;
    int rc = -1;
    const QStringList poolProps = poolSizePropertyNames();
    const QString propsCsv = poolProps.join(QLatin1Char(','));
    const bool poolWin = isWindowsConnection(connIdx);
    const bool daemonReadApiOk =
        !poolWin
        && connIdx >= 0
        && connIdx < m_conns.states.size()
        && m_conns.states[connIdx].daemonInstalled
        && m_conns.states[connIdx].daemonActive
        && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    // Por argv cuando hay daemon: la orden no pasa por ninguna cadena de shell.
    const QStringList poolCmdDaemonArgv = mwhelpers::argvQt(zfsmgr::commands::requests::poolProperties(trimmedPool.toStdString()));
    bool poolPropsOk = daemonReadApiOk
          && runAgentCommand(profile, poolCmdDaemonArgv, 15000, out, err, rc)
          && rc == 0;
    if (poolPropsOk) {
        const QString cacheKey = poolDetailsCacheKey(connIdx, trimmedPool);
        PoolDetailsCacheEntry entry = m_conns.poolDetailsCache.value(cacheKey);
        QMap<QString, QStringList> rowsByProp;
        for (const QStringList& row : std::as_const(entry.propsRows)) {
            if (row.size() >= 2) {
                rowsByProp.insert(row.value(0).trimmed().toLower(), row);
            }
        }

        if (poolWin) {
            const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QStringList parts = line.split('\t');
                if (parts.size() < 3) {
                    continue;
                }
                const QString prop = parts.value(0).trimmed();
                if (prop.isEmpty()) {
                    continue;
                }
                rowsByProp.insert(prop.toLower(),
                                  QStringList{prop, parts.value(1).trimmed(), parts.value(2).trimmed()});
            }
        } else {
            const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
            const QJsonObject poolObj = doc.object().value(QStringLiteral("pools")).toObject().value(trimmedPool).toObject();
            const QJsonObject properties = poolObj.value(QStringLiteral("properties")).toObject();
            for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
                const QJsonObject propObj = it.value().toObject();
                const QString value = propObj.value(QStringLiteral("value")).toString().trimmed();
                const QString source = propObj.value(QStringLiteral("source")).toObject().value(QStringLiteral("type")).toString().trimmed();
                rowsByProp.insert(it.key().trimmed().toLower(), QStringList{it.key(), value, source});
            }
        }

        QVector<QStringList> mergedRows;
        mergedRows.reserve(rowsByProp.size());
        for (auto it = rowsByProp.cbegin(); it != rowsByProp.cend(); ++it) {
            mergedRows.push_back(it.value());
        }
        entry.loaded = true;
        entry.propsRows = mergedRows;
        m_conns.poolDetailsCache.insert(cacheKey, entry);

        if (PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool)) {
            poolInfo->runtime.detailsState = LoadState::Loaded;
            poolInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
            poolInfo->runtime.zpoolPropertyRows = mergedRows;
            poolInfo->runtime.zpoolProperties.clear();
            for (const QStringList& row : std::as_const(mergedRows)) {
                if (row.size() >= 2) {
                    poolInfo->runtime.zpoolProperties.insert(row.value(0).trimmed(), row.value(1));
                }
            }
        }
        rebuildConnInfoFor(connIdx);
        const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool);
        if (m_connContentTree && m_topDetailConnIdx == connIdx) {
            syncConnContentPoolColumnsFor(m_connContentTree, token);
            if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
                syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
            }
        }
        refreshedSomething = true;
    } else {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("No se pudieron refrescar propiedades de tamaño del pool %1::%2: %3")
                   .arg(m_conns.profiles.at(connIdx).name, trimmedPool, oneLine(err.isEmpty() ? out : err)));
    }

    return refreshedSomething;
}

void MainWindow::invalidateDatasetCacheEntry(int connIdx,
                                             const QString& poolName,
                                             const QString& objectName,
                                             bool invalidatePermissions) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return;
    }

    removeDatasetPropertyEntry(connIdx, trimmedPool, trimmedObject);
    if (invalidatePermissions) {
        removeDatasetPermissionsEntry(connIdx, trimmedPool, trimmedObject);
    }

    const QString uiKey = QStringLiteral("%1::%2|%3")
                              .arg(connToken(connIdx),
                                   trimmedPool,
                                   trimmedObject);
    m_connContentPropValuesByObject.remove(uiKey);

    if (DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        dsInfo->runtime.errorText.clear();
    }
}

void MainWindow::invalidateDatasetSubtreeCacheEntries(int connIdx,
                                                      const QString& poolName,
                                                      const QString& datasetName,
                                                      bool invalidatePermissions) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedDataset = datasetName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedDataset.isEmpty()) {
        return;
    }

    QStringList objectsToInvalidate{trimmedDataset};
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool)) {
        const QString childPrefix = trimmedDataset + QStringLiteral("/");
        const QString snapshotPrefix = trimmedDataset + QStringLiteral("@");
        for (auto itDs = poolInfo->objectsByFullName.cbegin();
             itDs != poolInfo->objectsByFullName.cend();
             ++itDs) {
            const QString objectName = itDs.key().trimmed();
            if (objectName == trimmedDataset
                || objectName.startsWith(childPrefix)
                || objectName.startsWith(snapshotPrefix)) {
                objectsToInvalidate.push_back(objectName);
            }
        }
    }

    objectsToInvalidate.removeDuplicates();
    for (const QString& objectName : std::as_const(objectsToInvalidate)) {
        invalidateDatasetCacheEntry(connIdx, trimmedPool, objectName, invalidatePermissions);
    }
}

void MainWindow::invalidatePoolDatasetListingCache(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty()) {
        return;
    }
    m_conns.poolDatasetCache.remove(datasetCacheKey(connIdx, trimmedPool));
    removeDatasetPropertyEntriesForPool(connIdx, trimmedPool);
    removeDatasetPermissionsEntriesForPool(connIdx, trimmedPool);
    const QString uiPrefix =
        QStringLiteral("%1::%2|").arg(QString::number(connIdx), trimmedPool);
    auto vit = m_connContentPropValuesByObject.begin();
    while (vit != m_connContentPropValuesByObject.end()) {
        if (vit.key().startsWith(uiPrefix)) {
            vit = m_connContentPropValuesByObject.erase(vit);
        } else {
            ++vit;
        }
    }
    rebuildConnInfoFor(connIdx);
}

void MainWindow::invalidateDatasetCacheForPool(int connIdx, const QString& poolName) {
    m_conns.poolDatasetCache.remove(datasetCacheKey(connIdx, poolName));
    m_conns.poolDetailsCache.remove(poolDetailsCacheKey(connIdx, poolName));
    invalidateDatasetPermissionsCacheForPool(connIdx, poolName);
    removeDatasetPropertyEntriesForPool(connIdx, poolName);
    const QString uiPrefix =
        QStringLiteral("%1::%2|").arg(QString::number(connIdx), poolName.trimmed());
    auto vit = m_connContentPropValuesByObject.begin();
    while (vit != m_connContentPropValuesByObject.end()) {
        if (vit.key().startsWith(uiPrefix)) {
            vit = m_connContentPropValuesByObject.erase(vit);
        } else {
            ++vit;
        }
    }
    rebuildConnInfoFor(connIdx);
}

void MainWindow::scheduleReloadFlush() {
    if (m_reloadFlushScheduled) {
        return;
    }
    m_reloadFlushScheduled = true;
    QTimer::singleShot(40, this, [this]() {
        flushPendingReloads();
    });
}

void MainWindow::flushPendingReloads() {
    m_reloadFlushScheduled = false;
    if (m_pendingConnContentPoolReloadKeys.isEmpty() && m_pendingConnectionRefreshIndices.isEmpty()) {
        return;
    }

    const QSet<QString> poolKeys = m_pendingConnContentPoolReloadKeys;
    const QSet<int> refreshIdxs = m_pendingConnectionRefreshIndices;
    m_pendingConnContentPoolReloadKeys.clear();
    m_pendingConnectionRefreshIndices.clear();

    for (const QString& key : poolKeys) {
        int connIdx = -1;
        QString poolName;
        if (!splitConnToken(key, connIdx, poolName)) {
            continue;
        }
        reloadConnContentPoolNow(connIdx, poolName);
    }

    for (int connIdx : refreshIdxs) {
        if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
            continue;
        }
        refreshConnectionByIndex(connIdx);
    }
}

void MainWindow::reloadConnContentPoolNow(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return;
    }

    constexpr int kConnIdxRoleLocal = Qt::UserRole + 10;
    constexpr int kPoolNameRoleLocal = Qt::UserRole + 11;
    constexpr int kIsPoolRootRoleLocal = Qt::UserRole + 12;
    const QString targetToken = QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool);

    auto tokenMatchesTarget = [&](QTreeWidget* tree) -> bool {
        if (!tree) {
            return false;
        }
        const QString token = connContentTokenForTree(tree).trimmed();
        if (token.isEmpty()) {
            return false;
        }
        int tokenConnIdx = -1;
        QString tokenPool;
        if (!splitConnToken(token, tokenConnIdx, tokenPool)) {
            return false;
        }
        return tokenConnIdx == connIdx
               && tokenPool.compare(trimmedPool, Qt::CaseInsensitive) == 0;
    };

    auto treeHasTargetPool = [&](QTreeWidget* tree) -> bool {
        if (!tree) {
            return false;
        }
        std::function<bool(QTreeWidgetItem*)> recHasPool = [&](QTreeWidgetItem* node) -> bool {
            if (!node) {
                return false;
            }
            if (node->data(0, kIsPoolRootRoleLocal).toBool()
                && node->data(0, kConnIdxRoleLocal).toInt() == connIdx
                && node->data(0, kPoolNameRoleLocal).toString().trimmed().compare(
                       trimmedPool, Qt::CaseInsensitive) == 0) {
                return true;
            }
            for (int i = 0; i < node->childCount(); ++i) {
                if (recHasPool(node->child(i))) {
                    return true;
                }
            }
            return false;
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (recHasPool(tree->topLevelItem(i))) {
                return true;
            }
        }
        return false;
    };

    auto removeTargetPoolRoots = [&](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        std::function<void(QTreeWidgetItem*)> recPurge = [&](QTreeWidgetItem* parent) {
            if (!parent) {
                return;
            }
            for (int i = parent->childCount() - 1; i >= 0; --i) {
                QTreeWidgetItem* child = parent->child(i);
                if (!child) {
                    continue;
                }
                if (child->data(0, kIsPoolRootRoleLocal).toBool()
                    && child->data(0, kConnIdxRoleLocal).toInt() == connIdx
                    && child->data(0, kPoolNameRoleLocal).toString().trimmed().compare(
                           trimmedPool, Qt::CaseInsensitive) == 0) {
                    delete parent->takeChild(i);
                    continue;
                }
                recPurge(child);
            }
        };
        for (int i = tree->topLevelItemCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* top = tree->topLevelItem(i);
            if (!top) {
                continue;
            }
            if (top->data(0, kIsPoolRootRoleLocal).toBool()
                && top->data(0, kConnIdxRoleLocal).toInt() == connIdx
                && top->data(0, kPoolNameRoleLocal).toString().trimmed().compare(
                       trimmedPool, Qt::CaseInsensitive) == 0) {
                delete tree->takeTopLevelItem(i);
                continue;
            }
            recPurge(top);
        }
    };

    auto refreshTargetPoolInTree = [&](QTreeWidget* tree) -> bool {
        if (!tree || !(tokenMatchesTarget(tree) || treeHasTargetPool(tree))) {
            return false;
        }
        const QString stateToken = connContentTokenForTree(tree).trimmed();
        if (!stateToken.isEmpty()) {
            saveConnContentTreeStateFor(tree, stateToken);
        }
        removeTargetPoolRoots(tree);
        const bool groupedByConnectionRoots =
            tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool();
        const DatasetTreeContext ctx = groupedByConnectionRoots
                                           ? DatasetTreeContext::ConnectionContentMulti
                                           : DatasetTreeContext::ConnectionContent;
        const DatasetTreeRenderOptions opts = datasetTreeRenderOptionsForTree(tree, ctx);
        appendDatasetTreeForPool(tree, connIdx, trimmedPool, ctx, opts, true);
        if (groupedByConnectionRoots) {
            tree->expandToDepth(0);
        }
        syncConnContentPoolColumnsFor(tree, targetToken);
        if (!stateToken.isEmpty()) {
            restoreConnContentTreeStateFor(tree, stateToken);
        }
        return true;
    };

    beginUiBusy();
    bool refreshed = false;
    refreshed = refreshTargetPoolInTree(m_connContentTree) || refreshed;
    if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
        refreshed = refreshTargetPoolInTree(m_bottomConnContentTree) || refreshed;
    }
    if (!refreshed) {
        refreshConnectionByIndex(connIdx);
    }
    endUiBusy();
}

void MainWindow::reloadConnContentPool(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    m_pendingConnContentPoolReloadKeys.insert(QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool));
    scheduleReloadFlush();
}

void MainWindow::reloadDatasetSide(const QString& side) {
    if (side == QStringLiteral("origin") || side == QStringLiteral("dest")) {
        const DatasetSelectionContext ctx = currentDatasetSelection(side);
        if (ctx.valid && ctx.connIdx >= 0 && ctx.connIdx < m_conns.profiles.size()) {
            m_pendingConnectionRefreshIndices.insert(ctx.connIdx);
            scheduleReloadFlush();
        }
        return;
    } else if (side == QStringLiteral("conncontent")) {
        auto reloadTree = [this](QTreeWidget* tree) -> bool {
            const QString token = connContentTokenForTree(tree);
            if (token.isEmpty()) {
                return false;
            }
            int connIdx = -1;
            QString poolName;
            if (!splitConnToken(token, connIdx, poolName)) {
                return false;
            }
            m_pendingConnContentPoolReloadKeys.insert(QStringLiteral("%1::%2").arg(connToken(connIdx), poolName));
            scheduleReloadFlush();
            return true;
        };
        const bool reloadedTop = reloadTree(m_connContentTree);
        if (reloadedTop) {
            return;
        }
    }
}

bool MainWindow::mountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    Q_UNUSED(side);
    return cliRunOn(ctx, QStringLiteral("mount"));
}

bool MainWindow::umountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    Q_UNUSED(side);
    return cliRunOn(ctx, QStringLiteral("unmount"));
}
