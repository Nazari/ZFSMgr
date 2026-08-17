#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QVector>

#include "base/connectionprofile.h"
#include "base/storewarnings.h"


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

// Traducción a y desde el espejo sin Qt de este mismo struct.
//
// EN UN SOLO SITIO a propósito. Había dos copias idénticas —una en connectionstore.cpp y
// otra en mainwindow_helpers.cpp— y estaba a punto de aparecer una tercera. Es un espejo
// de 16 campos: si una copia se queda atrás al añadir un campo, ese campo llega VACÍO al
// otro lado sin que nada falle, que es exactamente el fallo del que avisa la cabecera de
// base/connectionprofile.h.
inline zfsmgr::base::ConnectionProfile toBaseProfile(const ConnectionProfile& p) {
    zfsmgr::base::ConnectionProfile o;
    o.id = p.id.toStdString();
    o.name = p.name.toStdString();
    o.machineUid = p.machineUid.toStdString();
    o.connType = p.connType.toStdString();
    o.osType = p.osType.toStdString();
    o.host = p.host.toStdString();
    o.port = p.port;
    o.sshAddressFamily = p.sshAddressFamily.toStdString();
    o.username = p.username.toStdString();
    o.password = p.password.toStdString();
    o.keyPath = p.keyPath.toStdString();
    o.useSudo = p.useSudo;
    o.daemonTlsServerCertPem = p.daemonTlsServerCertPem.toStdString();
    o.daemonTlsClientCertPem = p.daemonTlsClientCertPem.toStdString();
    o.daemonTlsClientKeyPem = p.daemonTlsClientKeyPem.toStdString();
    o.daemonTlsPort = p.daemonTlsPort;
    return o;
}

inline ConnectionProfile fromBaseProfile(const zfsmgr::base::ConnectionProfile& o) {
    ConnectionProfile p;
    p.id = QString::fromStdString(o.id);
    p.name = QString::fromStdString(o.name);
    p.machineUid = QString::fromStdString(o.machineUid);
    p.connType = QString::fromStdString(o.connType);
    p.osType = QString::fromStdString(o.osType);
    p.host = QString::fromStdString(o.host);
    p.port = o.port;
    p.sshAddressFamily = QString::fromStdString(o.sshAddressFamily);
    p.username = QString::fromStdString(o.username);
    p.password = QString::fromStdString(o.password);
    p.keyPath = QString::fromStdString(o.keyPath);
    p.useSudo = o.useSudo;
    p.daemonTlsServerCertPem = QString::fromStdString(o.daemonTlsServerCertPem);
    p.daemonTlsClientCertPem = QString::fromStdString(o.daemonTlsClientCertPem);
    p.daemonTlsClientKeyPem = QString::fromStdString(o.daemonTlsClientKeyPem);
    p.daemonTlsPort = o.daemonTlsPort;
    return p;
}

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
    QJsonObject loadConfigJson(QString* error = nullptr) const;
    bool saveConfigJson(const QJsonObject& root, QString* error = nullptr) const;
    void ensureAppDefaults() const;
    LoadResult loadConnections() const;
    bool upsertConnection(const ConnectionProfile& profile, QString& error);
    bool deleteConnectionById(const QString& id, QString& error);
    bool encryptStoredPasswords(QString& error);
    bool rotateMasterPassword(const QString& oldMasterPassword, const QString& newMasterPassword, QString& error);

    // Expuesta para poder verificar la conversión de perfiles PSRP sin fabricar un
    // almacén en disco. El puerto es la parte que se olvida y la que convierte una
    // conexión rota en una conexión rota sin explicación.
    static bool migratePsrpProfileToSshForTest(ConnectionProfile& p);

private:
    QString aviso(zfsmgr::base::store::Motivo m,
                  const QString& conexion = QString(),
                  const QString& campo = QString(),
                  const QString& detalle = QString()) const;
    QString traduce(const zfsmgr::base::store::Aviso& a) const;
    QString trk(const QString& key,
                const QString& es = QString(),
                const QString& en = QString(),
                const QString& zh = QString()) const;
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
