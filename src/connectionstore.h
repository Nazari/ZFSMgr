#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QVector>

class QSettings;

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
    QString daemonTlsServerCertPem;
    QString daemonTlsClientCertPem;
    QString daemonTlsClientKeyPem;
    int daemonTlsPort{47653};
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
    QString configPath() const;
    QString trustStorePath() const;
    QString iniPath() const; // legacy alias
    QJsonObject loadConfigJson(QString* error = nullptr) const;
    bool saveConfigJson(const QJsonObject& root, QString* error = nullptr) const;
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
    static QString connectionGroupNameForId(const QString& id);
    static QStringList connectionGroups(QSettings& ini);
    static ConnectionProfile loadProfileFromGroup(QSettings& ini, const QString& groupName);
    static void saveProfileToGroup(QSettings& ini, const QString& groupName, const ConnectionProfile& profile);
    static ConnectionProfile loadProfileFromIni(const QString& path);
    QJsonObject loadTrustStoreJson(QString* error = nullptr) const;
    bool saveTrustStoreJson(const QJsonObject& root, QString* error = nullptr) const;
    bool upsertTrustStoreConnection(const ConnectionProfile& profile, QString& error) const;
    bool deleteTrustStoreConnectionById(const QString& id, QString& error) const;
    void mergeTrustStoreIntoConnections(QVector<ConnectionProfile>& profiles, QStringList& warnings) const;
    bool migrateLegacyTlsToTrustStore(const QJsonArray& connections, QString& error) const;
    QString m_appName;
    QString m_masterPassword;
    QString m_language{QStringLiteral("es")};
};
