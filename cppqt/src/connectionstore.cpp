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
