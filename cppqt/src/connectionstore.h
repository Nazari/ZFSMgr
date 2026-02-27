#pragma once

#include <QString>
#include <QVector>

struct ConnectionProfile {
    QString id;
    QString name;
    QString connType;
    QString osType;
    QString host;
    int port{0};
};

class ConnectionStore {
public:
    explicit ConnectionStore(const QString& appName);

    QString configDir() const;
    QString iniPath() const;
    QVector<ConnectionProfile> loadConnections() const;

private:
    QString m_appName;
};
