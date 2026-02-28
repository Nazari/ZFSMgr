#include "connectionstore.h"
#include "secretcipher.h"

#include <QDir>
#include <QSettings>

ConnectionStore::ConnectionStore(const QString& appName)
    : m_appName(appName) {}

void ConnectionStore::setMasterPassword(const QString& password) {
    m_masterPassword = password;
}

bool ConnectionStore::validateMasterPassword(QString& error) const {
    error.clear();
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    bool hasEncrypted = false;
    for (const QString& group : groups) {
        if (!group.startsWith("connection:")) {
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
                error = QStringLiteral("%1: %2 incorrecto").arg(connName.isEmpty() ? group : connName, fieldName);
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
        error = QStringLiteral("Password maestro requerido");
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
    return configDir() + "/connections.ini";
}

LoadResult ConnectionStore::loadConnections() const {
    LoadResult result;

    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    for (const QString& group : groups) {
        if (!group.startsWith("connection:")) {
            continue;
        }
        ini.beginGroup(group);
        ConnectionProfile p;
        p.id = ini.value("id", group.mid(QString("connection:").size())).toString();
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
                    QStringLiteral("%1.username: %2").arg(p.name.isEmpty() ? p.id : p.name, err.isEmpty() ? QStringLiteral("no se pudo descifrar") : err));
            }
        }

        if (SecretCipher::isEncrypted(p.password)) {
            QString dec;
            QString err;
            if (!m_masterPassword.isEmpty() && SecretCipher::decryptEncv1(p.password, m_masterPassword, dec, err)) {
                p.password = dec;
            } else {
                result.warnings.push_back(
                    QStringLiteral("%1.password: %2").arg(p.name.isEmpty() ? p.id : p.name, err.isEmpty() ? QStringLiteral("no se pudo descifrar") : err));
            }
        }

        if (!p.name.isEmpty()) {
            result.profiles.push_back(p);
        }
    }

    return result;
}

bool ConnectionStore::upsertConnection(const ConnectionProfile& profile, QString& error) {
    error.clear();
    if (profile.name.trimmed().isEmpty()) {
        error = QStringLiteral("Nombre requerido");
        return false;
    }
    if (profile.host.trimmed().isEmpty()) {
        error = QStringLiteral("Host requerido");
        return false;
    }
    if (profile.username.trimmed().isEmpty()) {
        error = QStringLiteral("Usuario requerido");
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
    const QString group = QStringLiteral("connection:%1").arg(id);
    QString storedPassword = profile.password;
    if (!storedPassword.isEmpty() && !SecretCipher::isEncrypted(storedPassword)) {
        if (m_masterPassword.isEmpty()) {
            error = QStringLiteral("Password maestro requerido para cifrar password");
            return false;
        }
        QString encErr;
        QString encrypted;
        if (!SecretCipher::encryptEncv1(storedPassword, m_masterPassword, encrypted, encErr)) {
            error = QStringLiteral("No se pudo cifrar password: %1").arg(encErr);
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
        error = QStringLiteral("Error guardando INI");
        return false;
    }
    return true;
}

bool ConnectionStore::deleteConnectionById(const QString& id, QString& error) {
    error.clear();
    const QString clean = id.trimmed();
    if (clean.isEmpty()) {
        error = QStringLiteral("ID vacío");
        return false;
    }
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QString group = QStringLiteral("connection:%1").arg(clean);
    ini.beginGroup(group);
    ini.remove("");
    ini.endGroup();
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        error = QStringLiteral("Error borrando en INI");
        return false;
    }
    return true;
}

bool ConnectionStore::encryptStoredPasswords(QString& error) {
    error.clear();
    if (m_masterPassword.isEmpty()) {
        error = QStringLiteral("Password maestro requerido");
        return false;
    }

    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    for (const QString& group : groups) {
        if (!group.startsWith(QStringLiteral("connection:"))) {
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
        error = QStringLiteral("Error guardando INI");
        return false;
    }
    return true;
}

bool ConnectionStore::rotateMasterPassword(const QString& oldMasterPassword, const QString& newMasterPassword, QString& error) {
    error.clear();
    if (newMasterPassword.isEmpty()) {
        error = QStringLiteral("Nuevo password maestro vacío");
        return false;
    }
    QSettings ini(iniPath(), QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    for (const QString& group : groups) {
        if (!group.startsWith(QStringLiteral("connection:"))) {
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
        error = QStringLiteral("Error guardando INI");
        return false;
    }
    m_masterPassword = newMasterPassword;
    return true;
}
