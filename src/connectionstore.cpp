#include "connectionstore.h"
#include "i18nmanager.h"
#include "secretcipher.h"

#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
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
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    bool hasEncrypted = false;
    for (const QString& group : groups) {
        if (!isConnectionGroupName(group)) {
            continue;
        }
        ini.beginGroup(group);
        const QString connName = ini.value("name", group).toString();
        const QString username = ini.value("username").toString();
        const QString password = ini.value("password").toString();
        ini.endGroup();

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
                error = msg.arg(connName.isEmpty() ? group : connName, fieldName);
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

LoadResult ConnectionStore::loadConnections() const {
    LoadResult result;

    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    for (const QString& group : groups) {
        if (!isConnectionGroupName(group)) {
            continue;
        }
        ini.beginGroup(group);
        ConnectionProfile p;
        p.id = ini.value("id", defaultIdFromGroup(group)).toString();
        p.name = ini.value("name").toString();
        p.connType = ini.value("conn_type").toString();
        p.osType = ini.value("os_type").toString();
        p.transport = ini.value("transport").toString();
        p.host = ini.value("host").toString();
        p.port = ini.value("port", 22).toInt();
        p.username = ini.value("username").toString();
        p.password = ini.value("password").toString();
        p.keyPath = ini.value("key_path").toString();
        p.useSudo = ini.value("use_sudo", false).toBool();
        ini.endGroup();

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
            || p.connType.compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0
            || p.transport.compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0) {
            hasLocal = true;
            break;
        }
    }
    if (!hasLocal) {
        ConnectionProfile local;
        local.id = QStringLiteral("local");
        local.name = QStringLiteral("Local");
        local.connType = QStringLiteral("LOCAL");
        local.transport = QStringLiteral("LOCAL");
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

    QSettings iniRead(iniPath(), QSettings::IniFormat);
    const QStringList groups = iniRead.childGroups();
    const QString targetName = profile.name.trimmed();
    for (const QString& groupName : groups) {
        if (!isConnectionGroupName(groupName)) {
            continue;
        }
        iniRead.beginGroup(groupName);
        const QString existingId = iniRead.value("id", defaultIdFromGroup(groupName)).toString().trimmed();
        const QString existingName = iniRead.value("name").toString().trimmed();
        iniRead.endGroup();
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

    QSettings ini(iniPath(), QSettings::IniFormat);
    const QString group = QStringLiteral("connection:%1").arg(id);
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
    ini.beginGroup(group);
    ini.setValue("id", id);
    ini.setValue("name", profile.name.trimmed());
    ini.setValue("conn_type", profile.connType.trimmed().isEmpty() ? QStringLiteral("SSH") : profile.connType.trimmed());
    ini.setValue("os_type", profile.osType.trimmed().isEmpty() ? QStringLiteral("Linux") : profile.osType.trimmed());
    ini.setValue("transport", profile.transport.trimmed().isEmpty() ? QStringLiteral("SSH") : profile.transport.trimmed());
    ini.setValue("host", profile.host.trimmed());
    ini.setValue("port", profile.port > 0 ? profile.port : 22);
    ini.setValue("username", profile.username);
    ini.setValue("password", storedPassword);
    ini.setValue("key_path", profile.keyPath.trimmed());
    ini.setValue("use_sudo", profile.useSudo);
    ini.endGroup();
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
    const QString clean = id.trimmed();
    if (clean.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto012"), QStringLiteral("ID vacío"),
                    QStringLiteral("Empty ID"),
                    QStringLiteral("ID 为空"));
        return false;
    }
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QString group = QStringLiteral("connection:%1").arg(clean);
    ini.beginGroup(group);
    ini.remove("");
    ini.endGroup();
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
    if (m_masterPassword.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto014"), QStringLiteral("Password maestro requerido"),
                    QStringLiteral("Master password required"),
                    QStringLiteral("需要主密码"));
        return false;
    }

    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    for (const QString& group : groups) {
        if (!isConnectionGroupName(group)) {
            continue;
        }
        ini.beginGroup(group);
        const QString current = ini.value(QStringLiteral("password")).toString();
        if (!current.isEmpty() && !SecretCipher::isEncrypted(current)) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(current, m_masterPassword, encrypted, encErr)) {
                ini.endGroup();
                error = QStringLiteral("%1: %2").arg(group, encErr);
                return false;
            }
            ini.setValue(QStringLiteral("password"), encrypted);
        }
        ini.endGroup();
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
    if (newMasterPassword.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto016"), QStringLiteral("Nuevo password maestro vacío"),
                    QStringLiteral("New master password is empty"),
                    QStringLiteral("新主密码为空"));
        return false;
    }
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    for (const QString& group : groups) {
        if (!isConnectionGroupName(group)) {
            continue;
        }
        ini.beginGroup(group);
        const QString current = ini.value(QStringLiteral("password")).toString();
        QString plain = current;
        if (!current.isEmpty() && SecretCipher::isEncrypted(current)) {
            QString decErr;
            if (!SecretCipher::decryptEncv1(current, oldMasterPassword, plain, decErr)) {
                ini.endGroup();
                error = QStringLiteral("%1: %2").arg(group, decErr);
                return false;
            }
        }
        if (!plain.isEmpty()) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(plain, newMasterPassword, encrypted, encErr)) {
                ini.endGroup();
                error = QStringLiteral("%1: %2").arg(group, encErr);
                return false;
            }
            ini.setValue(QStringLiteral("password"), encrypted);
        }
        ini.endGroup();
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
