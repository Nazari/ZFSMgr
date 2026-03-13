#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct ConnectionProfile {
    QString id;
    QString name;
    QString machineUid;
    QString connType;
    QString osType;
    QString host;
    int port{0};
    QString sshAddressFamily;
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
    void setLanguage(const QString& language);
    bool validateMasterPassword(QString& error) const;
    QString configDir() const;
    QString iniPath() const;
    void ensureAppDefaults() const;
    QStringList connectionIniPaths() const;
    LoadResult loadConnections() const;
    bool upsertConnection(const ConnectionProfile& profile, QString& error);
    bool deleteConnectionById(const QString& id, QString& error);
    bool encryptStoredPasswords(QString& error);
    bool rotateMasterPassword(const QString& oldMasterPassword, const QString& newMasterPassword, QString& error);

private:
    QString trk(const QString& key,
                const QString& es = QString(),
                const QString& en = QString(),
                const QString& zh = QString()) const;
    bool migrateLegacyConnectionsToPerFile(QString& error) const;
    QString connectionIniPathForId(const QString& id) const;
    QString connectionIniPathForProfile(const ConnectionProfile& profile, const QString& currentPath = QString()) const;
    static QString sanitizedConnFileId(const QString& id);
    static ConnectionProfile loadProfileFromIni(const QString& path);
    static bool saveProfileToIni(const QString& path, const ConnectionProfile& profile, QString& error);
    QString m_appName;
    QString m_masterPassword;
    QString m_language{QStringLiteral("es")};
};
