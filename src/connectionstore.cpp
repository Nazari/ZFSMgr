#include "connectionstore.h"
#include "i18nmanager.h"
#include "secretcipher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>

namespace {
bool isConnectionGroupName(const QString& group) {
    return group.startsWith(QStringLiteral("connection:")) || group.startsWith(QStringLiteral("connection%3A"));
}

QString defaultIdFromGroup(const QString& group) {
    if (group.startsWith(QStringLiteral("connection:"))) {
        return group.mid(QStringLiteral("connection:").size());
    }
    if (group.startsWith(QStringLiteral("connection%3A"))) {
        return group.mid(QStringLiteral("connection%3A").size());
    }
    return group;
}

int ensurePort(const QString& connType, int port) {
    if (port > 0) {
        return port;
    }
    return connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0 ? 5986 : 22;
}

bool shouldForceLocalSudo(const ConnectionProfile& p) {
    const bool isLocal = p.id.trimmed().compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0
        || p.connType.trimmed().compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0;
    if (!isLocal) {
        return false;
    }
    return !p.osType.trimmed().toLower().contains(QStringLiteral("windows"));
}

QString currentLocalMachineUid() {
#if defined(Q_OS_MACOS)
    QProcess proc;
    proc.start(QStringLiteral("sh"),
               QStringList{QStringLiteral("-lc"),
                           QStringLiteral("ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F\\\" '/IOPlatformUUID/{print $(NF-1); exit}'")});
    if (proc.waitForFinished(3000)) {
        const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) {
            return out;
        }
    }
#endif
    return QString::fromLatin1(QSysInfo::machineUniqueId().toHex()).trimmed();
}

QString decodeHexAsciiIfUuid(const QString& raw) {
    const QByteArray bytes = QByteArray::fromHex(raw.trimmed().toLatin1());
    if (bytes.isEmpty()) {
        return QString();
    }
    const QString decoded = QString::fromLatin1(bytes).trimmed();
    static const QRegularExpression uuidRx(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return uuidRx.match(decoded).hasMatch() ? decoded : QString();
}

QString normalizeMachineUidForStorage(const ConnectionProfile& p, QString raw) {
    raw = raw.trimmed();
    if (!shouldForceLocalSudo(p)) {
        return raw;
    }
    if (raw.isEmpty()) {
        return currentLocalMachineUid();
    }
    const QString decoded = decodeHexAsciiIfUuid(raw);
    if (!decoded.isEmpty()) {
        return decoded;
    }
    return raw;
}

QJsonValue jsonValueFromVariant(const QVariant& v) {
    if (!v.isValid()) {
        return QJsonValue();
    }
    if (v.metaType().id() == QMetaType::QByteArray) {
        return QString::fromLatin1(v.toByteArray().toBase64());
    }
    if (v.metaType().id() == QMetaType::QStringList) {
        QJsonArray arr;
        const QStringList sl = v.toStringList();
        for (const QString& s : sl) {
            arr.push_back(s);
        }
        return arr;
    }
    return QJsonValue::fromVariant(v);
}

QJsonObject connectionToJson(const ConnectionProfile& inProfile) {
    ConnectionProfile p = inProfile;
    p.machineUid = normalizeMachineUidForStorage(p, p.machineUid);
    if (shouldForceLocalSudo(p)) {
        p.useSudo = true;
    }
    const QString sshFamily = p.sshAddressFamily.trimmed().toLower();
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), p.id.trimmed());
    obj.insert(QStringLiteral("name"), p.name.trimmed());
    obj.insert(QStringLiteral("machine_uid"), p.machineUid);
    obj.insert(QStringLiteral("conn_type"),
               p.connType.trimmed().isEmpty() ? QStringLiteral("SSH")
                                              : p.connType.trimmed());
    obj.insert(QStringLiteral("os_type"),
               p.osType.trimmed().isEmpty() ? QStringLiteral("Linux")
                                            : p.osType.trimmed());
    obj.insert(QStringLiteral("host"), p.host.trimmed());
    obj.insert(QStringLiteral("port"), ensurePort(p.connType, p.port));
    obj.insert(QStringLiteral("ssh_address_family"),
               (sshFamily == QStringLiteral("ipv4") || sshFamily == QStringLiteral("ipv6"))
                   ? sshFamily
                   : QStringLiteral("auto"));
    obj.insert(QStringLiteral("username"), p.username);
    obj.insert(QStringLiteral("password"), p.password);
    obj.insert(QStringLiteral("key_path"), p.keyPath.trimmed());
    obj.insert(QStringLiteral("use_sudo"), p.useSudo);
    return obj;
}

ConnectionProfile connectionFromJson(const QJsonObject& obj) {
    ConnectionProfile p;
    p.id = obj.value(QStringLiteral("id")).toString().trimmed();
    p.name = obj.value(QStringLiteral("name")).toString();
    const QString rawMachineUid = obj.value(QStringLiteral("machine_uid")).toString();
    p.machineUid = rawMachineUid;
    p.connType = obj.value(QStringLiteral("conn_type")).toString();
    p.osType = obj.value(QStringLiteral("os_type")).toString();
    p.host = obj.value(QStringLiteral("host")).toString();
    p.port = obj.value(QStringLiteral("port")).toInt(22);
    p.sshAddressFamily =
        obj.value(QStringLiteral("ssh_address_family")).toString(QStringLiteral("auto")).trimmed().toLower();
    p.username = obj.value(QStringLiteral("username")).toString();
    p.password = obj.value(QStringLiteral("password")).toString();
    p.keyPath = obj.value(QStringLiteral("key_path")).toString();
    p.useSudo = obj.value(QStringLiteral("use_sudo")).toBool(false);
    p.machineUid = normalizeMachineUidForStorage(p, p.machineUid);
    if (shouldForceLocalSudo(p)) {
        p.useSudo = true;
    }
    return p;
}

int indexOfConnectionById(const QJsonArray& connections, const QString& id) {
    const QString target = id.trimmed();
    if (target.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < connections.size(); ++i) {
        const QJsonObject obj = connections.at(i).toObject();
        if (obj.value(QStringLiteral("id")).toString().trimmed().compare(target, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

bool upsertConnectionJson(QJsonArray& connections, const ConnectionProfile& p) {
    if (p.id.trimmed().isEmpty()) {
        return false;
    }
    const int idx = indexOfConnectionById(connections, p.id);
    const QJsonObject obj = connectionToJson(p);
    if (idx >= 0) {
        connections[idx] = obj;
    } else {
        connections.push_back(obj);
    }
    return true;
}

QJsonObject readJsonRootNoMigration(const QString& path) {
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        return QJsonObject();
    }
    return doc.object();
}
} // namespace

ConnectionStore::ConnectionStore(const QString& appName)
    : m_appName(appName) {}

void ConnectionStore::setMasterPassword(const QString& password) {
    m_masterPassword = password;
}

void ConnectionStore::setLanguage(const QString& language) {
    const QString l = language.trimmed().toLower();
    if (l == QStringLiteral("en") || l == QStringLiteral("zh")) {
        m_language = l;
    } else {
        m_language = QStringLiteral("es");
    }
}

QString ConnectionStore::trk(const QString& key,
                             const QString& es,
                             const QString& en,
                             const QString& zh) const {
    return I18nManager::instance().translateKey(m_language, key, es, en, zh);
}

QString ConnectionStore::configDir() const {
    QString base = QDir::homePath() + "/.config/" + m_appName;
    QDir dir(base);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return dir.absolutePath();
}

QString ConnectionStore::configPath() const {
    return configDir() + QStringLiteral("/config.json");
}

QString ConnectionStore::iniPath() const {
    return configPath();
}

QJsonObject ConnectionStore::loadConfigJson(QString* error) const {
    if (error) {
        error->clear();
    }
    QFile file(configPath());
    if (!file.exists()) {
        QString migrateErr;
        migrateLegacyConnectionsToPerFile(migrateErr);
        file.setFileName(configPath());
    }
    if (!file.exists()) {
        return QJsonObject();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_read_open_err"),
                         QStringLiteral("No se pudo abrir config.json"),
                         QStringLiteral("Could not open config.json"),
                         QStringLiteral("无法打开 config.json"));
        }
        return QJsonObject();
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_parse_err"),
                         QStringLiteral("config.json no es válido"),
                         QStringLiteral("config.json is invalid"),
                         QStringLiteral("config.json 无效"));
        }
        return QJsonObject();
    }
    return doc.object();
}

bool ConnectionStore::saveConfigJson(const QJsonObject& root, QString* error) const {
    if (error) {
        error->clear();
    }
    QDir dir(configDir());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_dir_err"),
                         QStringLiteral("No se pudo crear el directorio de configuración"),
                         QStringLiteral("Could not create configuration directory"),
                         QStringLiteral("无法创建配置目录"));
        }
        return false;
    }
    QFile file(configPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_write_open_err"),
                         QStringLiteral("No se pudo escribir config.json"),
                         QStringLiteral("Could not write config.json"),
                         QStringLiteral("无法写入 config.json"));
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool ConnectionStore::validateMasterPassword(QString& error) const {
    error.clear();
    QString migrationError;
    if (!migrateLegacyConnectionsToPerFile(migrationError)) {
        return false;
    }
    const QJsonObject root = loadConfigJson(&error);
    if (!error.isEmpty()) {
        return false;
    }
    const QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    bool hasEncrypted = false;
    for (const QJsonValue& v : connections) {
        const ConnectionProfile p = connectionFromJson(v.toObject());
        const QString connName = p.name.trimmed().isEmpty() ? p.id : p.name;

        auto checkOne = [&](const QString& value, const QString& fieldName) -> bool {
            if (!SecretCipher::isEncrypted(value)) {
                return true;
            }
            hasEncrypted = true;
            QString dec;
            QString err;
            if (m_masterPassword.isEmpty() || !SecretCipher::decryptEncv1(value, m_masterPassword, dec, err)) {
                const QString msg = trk(QStringLiteral("t_cstore_auto002"), QStringLiteral("%1: %2 incorrecto"),
                                        QStringLiteral("%1: invalid %2"),
                                        QStringLiteral("%1：%2 无效"));
                error = msg.arg(connName, fieldName);
                return false;
            }
            return true;
        };

        if (!checkOne(p.username, QStringLiteral("usuario"))) {
            return false;
        }
        if (!checkOne(p.password, QStringLiteral("password"))) {
            return false;
        }
    }

    if (hasEncrypted && m_masterPassword.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto003"), QStringLiteral("Password maestro requerido"),
                    QStringLiteral("Master password required"),
                    QStringLiteral("需要主密码"));
        return false;
    }
    return true;
}

void ConnectionStore::ensureAppDefaults() const {
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    QJsonObject zdefs = root.value(QStringLiteral("ZPoolCreationDefaults")).toObject();
    bool touched = false;
    auto ensure = [&](const QString& key, const QJsonValue& value) {
        if (!zdefs.contains(key)) {
            zdefs.insert(key, value);
            touched = true;
        }
    };
    ensure(QStringLiteral("force"), true);
    ensure(QStringLiteral("altroot"), QString());
    ensure(QStringLiteral("ashift"), QStringLiteral("12"));
    ensure(QStringLiteral("autotrim"), QStringLiteral("on"));
    ensure(QStringLiteral("compatibility"), QStringLiteral("openzfs-2.4-linux"));
    ensure(QStringLiteral("fs_properties"),
           QStringLiteral("acltype=posixacl,"
                          "xattr=sa,"
                          "dnodesize=auto,"
                          "compression=lz4,"
                          "normalization=formD,"
                          "relatime=on,"
                          "canmount=noauto,"
                          "mountpoint=none"));
    if (touched) {
        root.insert(QStringLiteral("ZPoolCreationDefaults"), zdefs);
        QString saveErr;
        saveConfigJson(root, &saveErr);
    }
}

QString ConnectionStore::connectionGroupNameForId(const QString& id) {
    QString clean = id.trimmed();
    if (clean.isEmpty()) {
        clean = QStringLiteral("connection");
    }
    return QStringLiteral("connection:%1").arg(clean);
}

QStringList ConnectionStore::connectionIniPaths() const {
    QDir dir(configDir());
    const QStringList files = dir.entryList({QStringLiteral("conn*.ini")}, QDir::Files, QDir::Name);
    QStringList paths;
    paths.reserve(files.size());
    for (const QString& f : files) {
        paths.push_back(dir.filePath(f));
    }
    return paths;
}

QStringList ConnectionStore::connectionGroups(QSettings& ini) {
    QStringList groupsOut;
    const QStringList groups = ini.childGroups();
    for (const QString& group : groups) {
        if (isConnectionGroupName(group)) {
            groupsOut.push_back(group);
        }
    }
    return groupsOut;
}

ConnectionProfile ConnectionStore::loadProfileFromIni(const QString& path) {
    QSettings ini(path, QSettings::IniFormat);
    ConnectionProfile p;
    p.id = ini.value(QStringLiteral("id")).toString().trimmed();
    p.name = ini.value(QStringLiteral("name")).toString();
    const QString rawMachineUid = ini.value(QStringLiteral("machine_uid")).toString();
    p.machineUid = rawMachineUid;
    p.connType = ini.value(QStringLiteral("conn_type")).toString();
    p.osType = ini.value(QStringLiteral("os_type")).toString();
    p.host = ini.value(QStringLiteral("host")).toString();
    p.port = ini.value(QStringLiteral("port"), 22).toInt();
    p.sshAddressFamily = ini.value(QStringLiteral("ssh_address_family"), QStringLiteral("auto")).toString().trimmed().toLower();
    p.username = ini.value(QStringLiteral("username")).toString();
    p.password = ini.value(QStringLiteral("password")).toString();
    p.keyPath = ini.value(QStringLiteral("key_path")).toString();
    p.useSudo = ini.value(QStringLiteral("use_sudo"), false).toBool();
    if (p.id.isEmpty()) {
        p.id = QFileInfo(path).completeBaseName();
    }
    p.machineUid = normalizeMachineUidForStorage(p, p.machineUid);
    if (shouldForceLocalSudo(p)) {
        p.useSudo = true;
    }
    return p;
}

ConnectionProfile ConnectionStore::loadProfileFromGroup(QSettings& ini, const QString& groupName) {
    ConnectionProfile p;
    ini.beginGroup(groupName);
    p.id = ini.value(QStringLiteral("id")).toString().trimmed();
    p.name = ini.value(QStringLiteral("name")).toString();
    const QString rawMachineUid = ini.value(QStringLiteral("machine_uid")).toString();
    p.machineUid = rawMachineUid;
    p.connType = ini.value(QStringLiteral("conn_type")).toString();
    p.osType = ini.value(QStringLiteral("os_type")).toString();
    p.host = ini.value(QStringLiteral("host")).toString();
    p.port = ini.value(QStringLiteral("port"), 22).toInt();
    p.sshAddressFamily =
        ini.value(QStringLiteral("ssh_address_family"), QStringLiteral("auto")).toString().trimmed().toLower();
    p.username = ini.value(QStringLiteral("username")).toString();
    p.password = ini.value(QStringLiteral("password")).toString();
    p.keyPath = ini.value(QStringLiteral("key_path")).toString();
    p.useSudo = ini.value(QStringLiteral("use_sudo"), false).toBool();
    ini.endGroup();
    if (p.id.isEmpty()) {
        p.id = defaultIdFromGroup(groupName);
    }
    p.machineUid = normalizeMachineUidForStorage(p, p.machineUid);
    if (shouldForceLocalSudo(p)) {
        p.useSudo = true;
    }
    return p;
}

void ConnectionStore::saveProfileToGroup(QSettings& ini, const QString& groupName, const ConnectionProfile& profile) {
    ConnectionProfile normalized = profile;
    normalized.machineUid = normalizeMachineUidForStorage(normalized, normalized.machineUid);
    if (shouldForceLocalSudo(normalized)) {
        normalized.useSudo = true;
    }
    const QString sshFamily = normalized.sshAddressFamily.trimmed().toLower();
    ini.beginGroup(groupName);
    ini.remove(QStringLiteral(""));
    ini.setValue(QStringLiteral("id"), normalized.id.trimmed());
    ini.setValue(QStringLiteral("name"), normalized.name.trimmed());
    ini.setValue(QStringLiteral("machine_uid"), normalized.machineUid);
    ini.setValue(QStringLiteral("conn_type"),
                 normalized.connType.trimmed().isEmpty() ? QStringLiteral("SSH")
                                                         : normalized.connType.trimmed());
    ini.setValue(QStringLiteral("os_type"),
                 normalized.osType.trimmed().isEmpty() ? QStringLiteral("Linux")
                                                       : normalized.osType.trimmed());
    ini.setValue(QStringLiteral("host"), normalized.host.trimmed());
    ini.setValue(QStringLiteral("port"), ensurePort(normalized.connType, normalized.port));
    ini.setValue(QStringLiteral("ssh_address_family"),
                 (sshFamily == QStringLiteral("ipv4") || sshFamily == QStringLiteral("ipv6"))
                     ? sshFamily
                     : QStringLiteral("auto"));
    ini.setValue(QStringLiteral("username"), normalized.username);
    ini.setValue(QStringLiteral("password"), normalized.password);
    ini.setValue(QStringLiteral("key_path"), normalized.keyPath.trimmed());
    ini.setValue(QStringLiteral("use_sudo"), normalized.useSudo);
    ini.endGroup();
}

bool ConnectionStore::migrateLegacyConnectionsToPerFile(QString& error) const {
    error.clear();
    const QString jsonPath = configPath();
    const QString cfgDir = configDir();
    const QString legacyConfigPath = cfgDir + QStringLiteral("/config.ini");
    const QString legacyConnectionsPath = cfgDir + QStringLiteral("/connections.ini");
    const QStringList connFiles = connectionIniPaths();
    const bool hasLegacy = QFile::exists(legacyConfigPath)
                           || QFile::exists(legacyConnectionsPath)
                           || !connFiles.isEmpty();
    if (!hasLegacy) {
        return true;
    }

    QJsonObject root = readJsonRootNoMigration(jsonPath);
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    bool changed = false;

    auto mergeFromIni = [&](const QString& path) {
        if (!QFile::exists(path)) {
            return;
        }
        QSettings ini(path, QSettings::IniFormat);
        for (const QString& group : connectionGroups(ini)) {
            ConnectionProfile p = loadProfileFromGroup(ini, group);
            if (p.id.trimmed().isEmpty()) {
                p.id = defaultIdFromGroup(group);
            }
            if (upsertConnectionJson(connections, p)) {
                changed = true;
            }
        }
        auto copyGroup = [&](const QString& groupName) {
            ini.beginGroup(groupName);
            const QStringList keys = ini.childKeys();
            if (keys.isEmpty()) {
                ini.endGroup();
                return;
            }
            QJsonObject groupObj = root.value(groupName).toObject();
            for (const QString& key : keys) {
                groupObj.insert(key, jsonValueFromVariant(ini.value(key)));
            }
            ini.endGroup();
            root.insert(groupName, groupObj);
            changed = true;
        };
        copyGroup(QStringLiteral("app"));
        copyGroup(QStringLiteral("ui"));
        copyGroup(QStringLiteral("ZPoolCreationDefaults"));
    };

    mergeFromIni(legacyConfigPath);
    mergeFromIni(legacyConnectionsPath);

    for (const QString& path : connFiles) {
        ConnectionProfile p = loadProfileFromIni(path);
        if (p.id.trimmed().isEmpty()) {
            p.id = QFileInfo(path).completeBaseName();
        }
        if (upsertConnectionJson(connections, p)) {
            changed = true;
        }
    }

    if (changed || !QFile::exists(jsonPath)) {
        root.insert(QStringLiteral("connections"), connections);
        QString saveErr;
        if (!saveConfigJson(root, &saveErr)) {
            error = saveErr;
            return false;
        }
    }

    QFile::remove(legacyConfigPath);
    QFile::remove(legacyConnectionsPath);
    for (const QString& path : connFiles) {
        QFile::remove(path);
    }
    return true;
}

LoadResult ConnectionStore::loadConnections() const {
    LoadResult result;
    QString migrationError;
    if (!migrateLegacyConnectionsToPerFile(migrationError)) {
        if (!migrationError.trimmed().isEmpty()) {
            result.warnings.push_back(migrationError);
        }
    }
    QString loadErr;
    const QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        result.warnings.push_back(loadErr);
    }
    const QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (const QJsonValue& v : connections) {
        ConnectionProfile p = connectionFromJson(v.toObject());
        p.port = ensurePort(p.connType, p.port);

        if (SecretCipher::isEncrypted(p.username)) {
            QString dec;
            QString err;
            if (!m_masterPassword.isEmpty() && SecretCipher::decryptEncv1(p.username, m_masterPassword, dec, err)) {
                p.username = dec;
            } else {
                result.warnings.push_back(
                    QStringLiteral("%1.username: %2").arg(p.name.isEmpty() ? p.id : p.name,
                                                          err.isEmpty() ? trk(QStringLiteral("t_cstore_auto004"), QStringLiteral("no se pudo descifrar"),
                                                                               QStringLiteral("could not decrypt"),
                                                                               QStringLiteral("无法解密"))
                                                                        : err));
            }
        }

        if (SecretCipher::isEncrypted(p.password)) {
            QString dec;
            QString err;
            if (!m_masterPassword.isEmpty() && SecretCipher::decryptEncv1(p.password, m_masterPassword, dec, err)) {
                p.password = dec;
            } else {
                result.warnings.push_back(
                    QStringLiteral("%1.password: %2").arg(p.name.isEmpty() ? p.id : p.name,
                                                          err.isEmpty() ? trk(QStringLiteral("t_cstore_auto005"), QStringLiteral("no se pudo descifrar"),
                                                                               QStringLiteral("could not decrypt"),
                                                                               QStringLiteral("无法解密"))
                                                                        : err));
            }
        }

        if (!p.name.isEmpty()) {
            result.profiles.push_back(p);
        }
    }

    bool hasLocal = false;
    for (const ConnectionProfile& p : result.profiles) {
        if (p.id.compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0
            || p.connType.compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0) {
            hasLocal = true;
            break;
        }
    }
    if (!hasLocal) {
        ConnectionProfile local;
        local.id = QStringLiteral("local");
        local.name = QStringLiteral("Local");
        local.machineUid = currentLocalMachineUid();
        local.connType = QStringLiteral("LOCAL");
        local.port = 0;
        local.host = QStringLiteral("localhost");
        local.sshAddressFamily = QStringLiteral("auto");
        const QString userEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USER"));
        const QString userEnvWin = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USERNAME"));
        local.username = !userEnv.trimmed().isEmpty() ? userEnv.trimmed() : userEnvWin.trimmed();
#ifdef Q_OS_WIN
        local.osType = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
        local.osType = QStringLiteral("macOS");
#elif defined(Q_OS_FREEBSD)
        local.osType = QStringLiteral("FreeBSD");
#else
        local.osType = QStringLiteral("Linux");
#endif
        local.useSudo = shouldForceLocalSudo(local);
        if (local.username.isEmpty()) {
            local.username = QStringLiteral("local");
        }
        result.profiles.push_front(local);
    }

    return result;
}

bool ConnectionStore::upsertConnection(const ConnectionProfile& profile, QString& error) {
    error.clear();
    QString migrationError;
    if (!migrateLegacyConnectionsToPerFile(migrationError)) {
        error = migrationError;
        return false;
    }
    if (profile.name.trimmed().isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto006"), QStringLiteral("Nombre requerido"),
                    QStringLiteral("Name required"),
                    QStringLiteral("名称必填"));
        return false;
    }
    if (profile.host.trimmed().isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto007"), QStringLiteral("Host requerido"),
                    QStringLiteral("Host required"),
                    QStringLiteral("主机必填"));
        return false;
    }
    if (profile.username.trimmed().isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto008"), QStringLiteral("Usuario requerido"),
                    QStringLiteral("User required"),
                    QStringLiteral("用户必填"));
        return false;
    }

    QString id = profile.id.trimmed();
    if (id.isEmpty()) {
        id = profile.name.trimmed().toLower();
        id.replace(' ', '_');
        id.replace(':', '_');
        id.replace('/', '_');
    }

    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();

    const QString targetName = profile.name.trimmed();
    for (int i = 0; i < connections.size(); ++i) {
        const ConnectionProfile existing = connectionFromJson(connections.at(i).toObject());
        const QString existingId = existing.id.trimmed();
        const QString existingName = existing.name.trimmed();
        if (existingId.compare(id, Qt::CaseInsensitive) == 0) {
            continue;
        }
        if (!existingName.isEmpty() && existingName.compare(targetName, Qt::CaseInsensitive) == 0) {
            error = trk(QStringLiteral("t_conn_name_unique_01"),
                        QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                        QStringLiteral("Connection name already exists. It must be unique."),
                        QStringLiteral("连接名称已存在，必须唯一。"));
            return false;
        }
    }

    ConnectionProfile toSave = profile;
    toSave.id = id;
    toSave.port = ensurePort(profile.connType, profile.port);
    QString storedPassword = profile.password;
    if (!storedPassword.isEmpty() && !SecretCipher::isEncrypted(storedPassword)) {
        if (m_masterPassword.isEmpty()) {
            error = trk(QStringLiteral("t_cstore_auto009"), QStringLiteral("Password maestro requerido para cifrar password"),
                        QStringLiteral("Master password required to encrypt password"),
                        QStringLiteral("加密密码需要主密码"));
            return false;
        }
        QString encErr;
        QString encrypted;
        if (!SecretCipher::encryptEncv1(storedPassword, m_masterPassword, encrypted, encErr)) {
            error = trk(QStringLiteral("t_cstore_auto010"), QStringLiteral("No se pudo cifrar password: %1"),
                        QStringLiteral("Could not encrypt password: %1"),
                        QStringLiteral("无法加密密码：%1")).arg(encErr);
            return false;
        }
        storedPassword = encrypted;
    }
    toSave.password = storedPassword;
    if (!upsertConnectionJson(connections, toSave)) {
        error = trk(QStringLiteral("t_cstore_json_upsert_err"),
                    QStringLiteral("No se pudo guardar la conexión"),
                    QStringLiteral("Could not save connection"),
                    QStringLiteral("无法保存连接"));
        return false;
    }
    root.insert(QStringLiteral("connections"), connections);
    return saveConfigJson(root, &error);
}

bool ConnectionStore::deleteConnectionById(const QString& id, QString& error) {
    error.clear();
    QString migrationError;
    if (!migrateLegacyConnectionsToPerFile(migrationError)) {
        error = migrationError;
        return false;
    }
    const QString clean = id.trimmed();
    if (clean.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto012"), QStringLiteral("ID vacío"),
                    QStringLiteral("Empty ID"),
                    QStringLiteral("ID 为空"));
        return false;
    }
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (int i = connections.size() - 1; i >= 0; --i) {
        const ConnectionProfile p = connectionFromJson(connections.at(i).toObject());
        if (p.id.trimmed().compare(clean, Qt::CaseInsensitive) == 0) {
            connections.removeAt(i);
        }
    }
    root.insert(QStringLiteral("connections"), connections);
    return saveConfigJson(root, &error);
}

bool ConnectionStore::encryptStoredPasswords(QString& error) {
    error.clear();
    QString migrationError;
    if (!migrateLegacyConnectionsToPerFile(migrationError)) {
        error = migrationError;
        return false;
    }
    if (m_masterPassword.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto014"), QStringLiteral("Password maestro requerido"),
                    QStringLiteral("Master password required"),
                    QStringLiteral("需要主密码"));
        return false;
    }
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (int i = 0; i < connections.size(); ++i) {
        ConnectionProfile p = connectionFromJson(connections.at(i).toObject());
        const QString current = p.password;
        if (!current.isEmpty() && !SecretCipher::isEncrypted(current)) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(current, m_masterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            p.password = encrypted;
            connections[i] = connectionToJson(p);
        }
    }
    root.insert(QStringLiteral("connections"), connections);
    return saveConfigJson(root, &error);
}

bool ConnectionStore::rotateMasterPassword(const QString& oldMasterPassword, const QString& newMasterPassword, QString& error) {
    error.clear();
    QString migrationError;
    if (!migrateLegacyConnectionsToPerFile(migrationError)) {
        error = migrationError;
        return false;
    }
    if (newMasterPassword.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto016"), QStringLiteral("Nuevo password maestro vacío"),
                    QStringLiteral("New master password is empty"),
                    QStringLiteral("新主密码为空"));
        return false;
    }
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (int i = 0; i < connections.size(); ++i) {
        ConnectionProfile p = connectionFromJson(connections.at(i).toObject());
        const QString current = p.password;
        QString plain = current;
        if (!current.isEmpty() && SecretCipher::isEncrypted(current)) {
            QString decErr;
            if (!SecretCipher::decryptEncv1(current, oldMasterPassword, plain, decErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, decErr);
                return false;
            }
        }
        if (!plain.isEmpty()) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(plain, newMasterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            p.password = encrypted;
            connections[i] = connectionToJson(p);
        }
    }
    root.insert(QStringLiteral("connections"), connections);
    if (!saveConfigJson(root, &error)) {
        return false;
    }
    m_masterPassword = newMasterPassword;
    return true;
}
