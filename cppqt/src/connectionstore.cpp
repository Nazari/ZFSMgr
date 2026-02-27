#include "connectionstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

ConnectionStore::ConnectionStore(const QString& appName)
    : m_appName(appName) {}

QString ConnectionStore::configDir() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + "/.config/" + m_appName;
    }
    QDir dir(base);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return dir.absolutePath();
}

QString ConnectionStore::iniPath() const {
    const QString primary = configDir() + "/connections.ini";
    if (QFileInfo::exists(primary)) {
        return primary;
    }

    // Fallback para transición desde la versión Python del mismo repo.
    const QString fallback = QCoreApplication::applicationDirPath() + "/../vpython/connections.ini";
    if (QFileInfo::exists(fallback)) {
        return QFileInfo(fallback).absoluteFilePath();
    }

    return primary;
}

QVector<ConnectionProfile> ConnectionStore::loadConnections() const {
    QVector<ConnectionProfile> out;

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
        p.host = ini.value("host").toString();
        p.port = ini.value("port", 0).toInt();
        ini.endGroup();

        if (!p.name.isEmpty()) {
            out.push_back(p);
        }
    }

    return out;
}
