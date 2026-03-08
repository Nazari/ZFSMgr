#include "connectionstore.h"
#include "i18nmanager.h"
#include "secretcipher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    const QStringList connFiles = connectionIniPaths();
    bool hasEncrypted = false;
    for (const QString& path : connFiles) {
        const ConnectionProfile p = loadProfileFromIni(path);
        const QString connName = p.name.trimmed().isEmpty() ? QFileInfo(path).baseName() : p.name;
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
    ensure(QStringLiteral("ZPoolCreationDefaults"), QStringLiteral("altroot"), QStringLiteral("/mnt/fc16"));
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

QString ConnectionStore::sanitizedConnFileId(const QString& id) {
    QString clean = id.trimmed().toLower();
    if (clean.isEmpty()) {
        clean = QStringLiteral("connection");
    }
    clean.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]+")), QStringLiteral("_"));
    clean.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    clean.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    if (clean.isEmpty()) {
        clean = QStringLiteral("connection");
    }
    return clean.left(64);
}

QString ConnectionStore::connectionIniPathForId(const QString& id) const {
    const QString safe = sanitizedConnFileId(id);
    return QDir(configDir()).filePath(QStringLiteral("conn_%1.ini").arg(safe));
}

QString ConnectionStore::connectionIniPathForProfile(const ConnectionProfile& profile, const QString& currentPath) const {
    const QString dirPath = configDir();
    const QDir dir(dirPath);
    const QString baseName = sanitizedConnFileId(profile.name.trimmed().isEmpty() ? profile.id : profile.name);
    QString candidate = dir.filePath(QStringLiteral("conn_%1.ini").arg(baseName));
    const QString currentNorm = QFileInfo(currentPath).absoluteFilePath();
    const QString candidateNorm = QFileInfo(candidate).absoluteFilePath();
    if (candidateNorm == currentNorm) {
        return candidate;
    }
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }

    const QString idSuffix = sanitizedConnFileId(profile.id);
    candidate = dir.filePath(QStringLiteral("conn_%1_%2.ini").arg(baseName, idSuffix));
    const QString candidateWithIdNorm = QFileInfo(candidate).absoluteFilePath();
    if (candidateWithIdNorm == currentNorm || !QFileInfo::exists(candidate)) {
        return candidate;
    }

    for (int i = 2; i < 1000; ++i) {
        const QString probe =
            dir.filePath(QStringLiteral("conn_%1_%2_%3.ini").arg(baseName, idSuffix).arg(i));
        const QString probeNorm = QFileInfo(probe).absoluteFilePath();
        if (probeNorm == currentNorm || !QFileInfo::exists(probe)) {
            return probe;
        }
    }
    return connectionIniPathForId(profile.id);
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

ConnectionProfile ConnectionStore::loadProfileFromIni(const QString& path) {
    QSettings ini(path, QSettings::IniFormat);
    ConnectionProfile p;
    p.id = ini.value(QStringLiteral("id")).toString().trimmed();
    p.name = ini.value(QStringLiteral("name")).toString();
    p.machineUid = ini.value(QStringLiteral("machine_uid")).toString();
    p.connType = ini.value(QStringLiteral("conn_type")).toString();
    p.osType = ini.value(QStringLiteral("os_type")).toString();
    p.host = ini.value(QStringLiteral("host")).toString();
    p.port = ini.value(QStringLiteral("port"), 22).toInt();
    p.username = ini.value(QStringLiteral("username")).toString();
    p.password = ini.value(QStringLiteral("password")).toString();
    p.keyPath = ini.value(QStringLiteral("key_path")).toString();
    p.useSudo = ini.value(QStringLiteral("use_sudo"), false).toBool();
    if (p.id.isEmpty()) {
        p.id = QFileInfo(path).completeBaseName();
    }
    return p;
}

bool ConnectionStore::saveProfileToIni(const QString& path, const ConnectionProfile& profile, QString& error) {
    error.clear();
    QSettings ini(path, QSettings::IniFormat);
    ini.clear();
    ini.setValue(QStringLiteral("id"), profile.id.trimmed());
    ini.setValue(QStringLiteral("name"), profile.name.trimmed());
    ini.setValue(QStringLiteral("machine_uid"), profile.machineUid.trimmed());
    ini.setValue(QStringLiteral("conn_type"),
                 profile.connType.trimmed().isEmpty() ? QStringLiteral("SSH") : profile.connType.trimmed());
    ini.setValue(QStringLiteral("os_type"),
                 profile.osType.trimmed().isEmpty() ? QStringLiteral("Linux") : profile.osType.trimmed());
    ini.setValue(QStringLiteral("host"), profile.host.trimmed());
    ini.setValue(QStringLiteral("port"), ensurePort(profile.connType, profile.port));
    ini.setValue(QStringLiteral("username"), profile.username);
    ini.setValue(QStringLiteral("password"), profile.password);
    ini.setValue(QStringLiteral("key_path"), profile.keyPath.trimmed());
    ini.setValue(QStringLiteral("use_sudo"), profile.useSudo);
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        error = QStringLiteral("Error saving INI");
        return false;
    }
    return true;
}

bool ConnectionStore::migrateLegacyConnectionsToPerFile(QString& error) const {
    error.clear();
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    bool hadLegacy = false;
    for (const QString& group : groups) {
        if (!isConnectionGroupName(group)) {
            continue;
        }
        hadLegacy = true;
        ini.beginGroup(group);
        ConnectionProfile p;
        p.id = ini.value(QStringLiteral("id"), defaultIdFromGroup(group)).toString().trimmed();
        p.name = ini.value(QStringLiteral("name")).toString();
        p.machineUid = ini.value(QStringLiteral("machine_uid")).toString();
        p.connType = ini.value(QStringLiteral("conn_type")).toString();
        p.osType = ini.value(QStringLiteral("os_type")).toString();
        p.host = ini.value(QStringLiteral("host")).toString();
        p.port = ini.value(QStringLiteral("port"), 22).toInt();
        p.username = ini.value(QStringLiteral("username")).toString();
        p.password = ini.value(QStringLiteral("password")).toString();
        p.keyPath = ini.value(QStringLiteral("key_path")).toString();
        p.useSudo = ini.value(QStringLiteral("use_sudo"), false).toBool();
        ini.endGroup();
        if (p.id.isEmpty()) {
            p.id = defaultIdFromGroup(group);
        }
        const QString filePath = connectionIniPathForProfile(p);
        QString saveErr;
        if (!saveProfileToIni(filePath, p, saveErr)) {
            error = trk(QStringLiteral("t_cstore_auto011"), QStringLiteral("Error guardando INI"),
                        QStringLiteral("Error saving INI"), QStringLiteral("保存 INI 出错"));
            return false;
        }
        ini.beginGroup(group);
        ini.remove(QStringLiteral(""));
        ini.endGroup();
    }
    if (hadLegacy) {
        ini.sync();
        if (ini.status() != QSettings::NoError) {
            error = trk(QStringLiteral("t_cstore_auto011"), QStringLiteral("Error guardando INI"),
                        QStringLiteral("Error saving INI"), QStringLiteral("保存 INI 出错"));
            return false;
        }
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
    const QStringList connFiles = connectionIniPaths();
    for (const QString& path : connFiles) {
        ConnectionProfile p = loadProfileFromIni(path);
        p.port = ensurePort(p.connType, p.port);
        if (p.id.trimmed().isEmpty()) {
            p.id = QFileInfo(path).completeBaseName();
        }

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
        local.machineUid = QString::fromLatin1(QSysInfo::machineUniqueId().toHex());
        local.connType = QStringLiteral("LOCAL");
        local.port = 0;
        local.host = QStringLiteral("localhost");
        const QString userEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USER"));
        const QString userEnvWin = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USERNAME"));
        local.username = !userEnv.trimmed().isEmpty() ? userEnv.trimmed() : userEnvWin.trimmed();
#ifdef Q_OS_WIN
        local.osType = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
        local.osType = QStringLiteral("macOS");
#else
        local.osType = QStringLiteral("Linux");
#endif
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

    const QStringList connFiles = connectionIniPaths();
    const QString targetName = profile.name.trimmed();
    QString existingPathForId;
    for (const QString& path : connFiles) {
        const ConnectionProfile existing = loadProfileFromIni(path);
        const QString existingId = existing.id.trimmed();
        const QString existingName = existing.name.trimmed();
        if (existingId.compare(id, Qt::CaseInsensitive) == 0) {
            existingPathForId = path;
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
    const QString path = connectionIniPathForProfile(toSave, existingPathForId);
    if (!saveProfileToIni(path, toSave, error)) {
        error = trk(QStringLiteral("t_cstore_auto011"), QStringLiteral("Error guardando INI"),
                    QStringLiteral("Error saving INI"),
                    QStringLiteral("保存 INI 出错"));
        return false;
    }
    if (!existingPathForId.trimmed().isEmpty()) {
        const QString existingNorm = QFileInfo(existingPathForId).absoluteFilePath();
        const QString newNorm = QFileInfo(path).absoluteFilePath();
        if (existingNorm != newNorm) {
            QFile::remove(existingPathForId);
        }
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
    const QStringList connFiles = connectionIniPaths();
    bool removedAny = false;
    for (const QString& path : connFiles) {
        const ConnectionProfile p = loadProfileFromIni(path);
        if (p.id.trimmed().compare(clean, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!QFile::remove(path)) {
            error = trk(QStringLiteral("t_cstore_auto013"), QStringLiteral("Error borrando en INI"),
                        QStringLiteral("Error deleting from INI"),
                        QStringLiteral("删除 INI 项时出错"));
            return false;
        }
        removedAny = true;
    }
    Q_UNUSED(removedAny);
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

    for (const QString& path : connectionIniPaths()) {
        ConnectionProfile p = loadProfileFromIni(path);
        const QString current = p.password;
        if (!current.isEmpty() && !SecretCipher::isEncrypted(current)) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(current, m_masterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(path, encErr);
                return false;
            }
            p.password = encrypted;
            if (!saveProfileToIni(path, p, error)) {
                error = trk(QStringLiteral("t_cstore_auto015"), QStringLiteral("Error guardando INI"),
                            QStringLiteral("Error saving INI"),
                            QStringLiteral("保存 INI 出错"));
                return false;
            }
        }
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
    for (const QString& path : connectionIniPaths()) {
        ConnectionProfile p = loadProfileFromIni(path);
        const QString current = p.password;
        QString plain = current;
        if (!current.isEmpty() && SecretCipher::isEncrypted(current)) {
            QString decErr;
            if (!SecretCipher::decryptEncv1(current, oldMasterPassword, plain, decErr)) {
                error = QStringLiteral("%1: %2").arg(path, decErr);
                return false;
            }
        }
        if (!plain.isEmpty()) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(plain, newMasterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(path, encErr);
                return false;
            }
            p.password = encrypted;
            if (!saveProfileToIni(path, p, error)) {
                error = trk(QStringLiteral("t_cstore_auto017"), QStringLiteral("Error guardando INI"),
                            QStringLiteral("Error saving INI"),
                            QStringLiteral("保存 INI 出错"));
                return false;
            }
        }
    }
    m_masterPassword = newMasterPassword;
    return true;
}
