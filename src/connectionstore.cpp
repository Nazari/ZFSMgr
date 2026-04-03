#include "connectionstore.h"
#include "i18nmanager.h"
#include "secretcipher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QProcess>
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

bool ConnectionStore::validateMasterPassword(QString& error) const {
    error.clear();
    if (!migrateLegacyConnectionsToPerFile(error)) {
        return false;
    }
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = connectionGroups(ini);
    bool hasEncrypted = false;
    for (const QString& group : groups) {
        const ConnectionProfile p = loadProfileFromGroup(ini, group);
        const QString connName = p.name.trimmed().isEmpty() ? p.id : p.name;
        const QString username = p.username;
        const QString password = p.password;

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

        if (!checkOne(username, QStringLiteral("usuario"))) {
            return false;
        }
        if (!checkOne(password, QStringLiteral("password"))) {
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

QString ConnectionStore::configDir() const {
    QString base = QDir::homePath() + "/.config/" + m_appName;
    QDir dir(base);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return dir.absolutePath();
}

QString ConnectionStore::iniPath() const {
    const QString dir = configDir();
    const QString newPath = dir + "/config.ini";
    const QString oldPath = dir + "/connections.ini";
    if (!QFile::exists(newPath) && QFile::exists(oldPath)) {
        if (!QFile::rename(oldPath, newPath)) {
            QFile::copy(oldPath, newPath);
        }
    }
    return newPath;
}

void ConnectionStore::ensureAppDefaults() const {
    QSettings ini(iniPath(), QSettings::IniFormat);
    bool touched = false;
    auto ensure = [&](const QString& group, const QString& key, const QVariant& value) {
        ini.beginGroup(group);
        if (!ini.contains(key)) {
            ini.setValue(key, value);
            touched = true;
        }
        ini.endGroup();
    };

    ensure(QStringLiteral("ZPoolCreationDefaults"), QStringLiteral("force"), true);
    ensure(QStringLiteral("ZPoolCreationDefaults"), QStringLiteral("altroot"), QString());
    ensure(QStringLiteral("ZPoolCreationDefaults"), QStringLiteral("ashift"), QStringLiteral("12"));
    ensure(QStringLiteral("ZPoolCreationDefaults"), QStringLiteral("autotrim"), QStringLiteral("on"));
    ensure(QStringLiteral("ZPoolCreationDefaults"), QStringLiteral("compatibility"), QStringLiteral("openzfs-2.4-linux"));
    ensure(QStringLiteral("ZPoolCreationDefaults"), QStringLiteral("fs_properties"),
           QStringLiteral("acltype=posixacl,"
                          "xattr=sa,"
                          "dnodesize=auto,"
                          "compression=lz4,"
                          "normalization=formD,"
                          "relatime=on,"
                          "canmount=noauto,"
                          "mountpoint=none"));

    if (touched) {
        ini.sync();
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
    if (p.machineUid != rawMachineUid) {
        ini.setValue(QStringLiteral("machine_uid"), p.machineUid);
        ini.sync();
    }
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
    QSettings ini(iniPath(), QSettings::IniFormat);
    bool changed = false;
    const QStringList groups = connectionGroups(ini);
    for (const QString& group : groups) {
        const ConnectionProfile p = loadProfileFromGroup(ini, group);
        const QString normalizedGroup = connectionGroupNameForId(p.id.isEmpty() ? defaultIdFromGroup(group) : p.id);
        if (group.compare(normalizedGroup, Qt::CaseInsensitive) == 0) {
            continue;
        }
        saveProfileToGroup(ini, normalizedGroup, p);
        ini.beginGroup(group);
        ini.remove(QStringLiteral(""));
        ini.endGroup();
        changed = true;
    }

    const QStringList connFiles = connectionIniPaths();
    for (const QString& path : connFiles) {
        ConnectionProfile p = loadProfileFromIni(path);
        if (p.id.trimmed().isEmpty()) {
            p.id = QFileInfo(path).completeBaseName();
        }
        const QString groupName = connectionGroupNameForId(p.id);
        saveProfileToGroup(ini, groupName, p);
        changed = true;
    }

    if (changed) {
        ini.sync();
        if (ini.status() != QSettings::NoError) {
            error = trk(QStringLiteral("t_cstore_auto011"), QStringLiteral("Error guardando INI"),
                        QStringLiteral("Error saving INI"), QStringLiteral("保存 INI 出错"));
            return false;
        }
    }
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
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = connectionGroups(ini);
    for (const QString& group : groups) {
        ConnectionProfile p = loadProfileFromGroup(ini, group);
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

    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = connectionGroups(ini);
    const QString targetName = profile.name.trimmed();
    QString existingGroupForId;
    for (const QString& group : groups) {
        const ConnectionProfile existing = loadProfileFromGroup(ini, group);
        const QString existingId = existing.id.trimmed();
        const QString existingName = existing.name.trimmed();
        if (existingId.compare(id, Qt::CaseInsensitive) == 0) {
            existingGroupForId = group;
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
    const QString targetGroup = connectionGroupNameForId(toSave.id);
    saveProfileToGroup(ini, targetGroup, toSave);
    if (!existingGroupForId.isEmpty() && existingGroupForId.compare(targetGroup, Qt::CaseInsensitive) != 0) {
        ini.beginGroup(existingGroupForId);
        ini.remove(QStringLiteral(""));
        ini.endGroup();
    }
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        error = trk(QStringLiteral("t_cstore_auto011"), QStringLiteral("Error guardando INI"),
                    QStringLiteral("Error saving INI"),
                    QStringLiteral("保存 INI 出错"));
        return false;
    }
    return true;
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
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = connectionGroups(ini);
    for (const QString& group : groups) {
        const ConnectionProfile p = loadProfileFromGroup(ini, group);
        if (p.id.trimmed().compare(clean, Qt::CaseInsensitive) != 0) {
            continue;
        }
        ini.beginGroup(group);
        ini.remove(QStringLiteral(""));
        ini.endGroup();
    }
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        error = trk(QStringLiteral("t_cstore_auto013"), QStringLiteral("Error borrando en INI"),
                    QStringLiteral("Error deleting from INI"),
                    QStringLiteral("删除 INI 项时出错"));
        return false;
    }
    return true;
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

    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = connectionGroups(ini);
    for (const QString& group : groups) {
        ConnectionProfile p = loadProfileFromGroup(ini, group);
        const QString current = p.password;
        if (!current.isEmpty() && !SecretCipher::isEncrypted(current)) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(current, m_masterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            p.password = encrypted;
            saveProfileToGroup(ini, group, p);
        }
    }
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        error = trk(QStringLiteral("t_cstore_auto015"), QStringLiteral("Error guardando INI"),
                    QStringLiteral("Error saving INI"),
                    QStringLiteral("保存 INI 出错"));
        return false;
    }
    return true;
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
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = connectionGroups(ini);
    for (const QString& group : groups) {
        ConnectionProfile p = loadProfileFromGroup(ini, group);
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
            saveProfileToGroup(ini, group, p);
        }
    }
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        error = trk(QStringLiteral("t_cstore_auto017"), QStringLiteral("Error guardando INI"),
                    QStringLiteral("Error saving INI"),
                    QStringLiteral("保存 INI 出错"));
        return false;
    }
    m_masterPassword = newMasterPassword;
    return true;
}
