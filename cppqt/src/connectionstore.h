#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct ConnectionProfile {
    QString id;
    QString name;
    QString connType;
    QString osType;
    QString transport;
    QString host;
    int port{0};
    QString username;
    QString password;
    QString keyPath;
    bool useSudo{false};
};

struct LoadResult {
    QVector<ConnectionProfile> profiles;
    QStringList warnings;
};

class ConnectionStore {
public:
    explicit ConnectionStore(const QString& appName);

    void setMasterPassword(const QString& password);
    bool validateMasterPassword(QString& error) const;
    QString configDir() const;
    QString iniPath() const;
    LoadResult loadConnections() const;
    bool upsertConnection(const ConnectionProfile& profile, QString& error);
    bool deleteConnectionById(const QString& id, QString& error);
    bool encryptStoredPasswords(QString& error);
    bool rotateMasterPassword(const QString& oldMasterPassword, const QString& newMasterPassword, QString& error);

private:
    QString m_appName;
    QString m_masterPassword;
};
