#include "connectionstore.h"

#include "base/connectionjson.h"

namespace CJ = zfsmgr::base::connjson;
namespace BP = zfsmgr::base;
namespace BJ = zfsmgr::base::json;
#include "i18nmanager.h"
#include "secretcipher.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QLoggingCategory>

// Medición del arranque. Se dejó por defecto en stderr al perseguir la lentitud de
// la ventana, y así salía una línea en la terminal en cada ejecución. Misma
// categoría-interruptor que el resto: QT_LOGGING_RULES="zfsmgr.startup.debug=true"
Q_LOGGING_CATEGORY(lcStartup, "zfsmgr.startup", QtWarningMsg)

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

QString currentLocalMachineUid() {
    // Cache: ioreg/registry can take 400-600ms; no need to repeat within the same process.
    static QString s_cached;
    if (!s_cached.isEmpty()) {
        return s_cached;
    }
    QElapsedTimer t;
    t.start();
#if defined(Q_OS_WIN)
    {
        QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography"),
                      QSettings::NativeFormat);
        const QString guid = reg.value(QStringLiteral("MachineGuid")).toString().trimmed().toLower();
        qCDebug(lcStartup, "[startup] currentLocalMachineUid (Windows registry): %lld ms", t.elapsed());
        if (!guid.isEmpty()) {
            s_cached = guid;
            return s_cached;
        }
    }
#elif defined(Q_OS_MACOS)
    QProcess proc;
    proc.start(QStringLiteral("sh"),
               QStringList{QStringLiteral("-lc"),
                           QStringLiteral("ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F\\\" '/IOPlatformUUID/{print $(NF-1); exit}'")});
    if (proc.waitForFinished(3000)) {
        const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toLower();
        qCDebug(lcStartup, "[startup] currentLocalMachineUid (ioreg): %lld ms", t.elapsed());
        if (!out.isEmpty()) {
            s_cached = out;
            return s_cached;
        }
    }
#endif
    qCDebug(lcStartup, "[startup] currentLocalMachineUid (QSysInfo fallback): %lld ms", t.elapsed());
    s_cached = QString::fromLatin1(QSysInfo::machineUniqueId().toHex()).trimmed().toLower();
    return s_cached;
}

// --- Adaptadores hacia la capa base.
//
// La traducción entre ConnectionProfile y JSON vive en `src/base/connectionjson.cpp`,
// que no depende de Qt. Aquí solo se convierte en la frontera y se le entrega el
// identificador de máquina local, que la base no puede averiguar por su cuenta:
// consultarlo cuesta 400-600 ms porque lanza `ioreg` o lee el registro.
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string bs(const QString& s) { return s.toStdString(); }

BP::ConnectionProfile aBase(const ConnectionProfile& p) {
    BP::ConnectionProfile o;
    o.id = bs(p.id);
    o.name = bs(p.name);
    o.machineUid = bs(p.machineUid);
    o.connType = bs(p.connType);
    o.osType = bs(p.osType);
    o.host = bs(p.host);
    o.port = p.port;
    o.sshAddressFamily = bs(p.sshAddressFamily);
    o.username = bs(p.username);
    o.password = bs(p.password);
    o.keyPath = bs(p.keyPath);
    o.useSudo = p.useSudo;
    o.daemonTlsServerCertPem = bs(p.daemonTlsServerCertPem);
    o.daemonTlsClientCertPem = bs(p.daemonTlsClientCertPem);
    o.daemonTlsClientKeyPem = bs(p.daemonTlsClientKeyPem);
    o.daemonTlsPort = p.daemonTlsPort;
    return o;
}

ConnectionProfile deBase(const BP::ConnectionProfile& o) {
    ConnectionProfile p;
    p.id = qs(o.id);
    p.name = qs(o.name);
    p.machineUid = qs(o.machineUid);
    p.connType = qs(o.connType);
    p.osType = qs(o.osType);
    p.host = qs(o.host);
    p.port = o.port;
    p.sshAddressFamily = qs(o.sshAddressFamily);
    p.username = qs(o.username);
    p.password = qs(o.password);
    p.keyPath = qs(o.keyPath);
    p.useSudo = o.useSudo;
    p.daemonTlsServerCertPem = qs(o.daemonTlsServerCertPem);
    p.daemonTlsClientCertPem = qs(o.daemonTlsClientCertPem);
    p.daemonTlsClientKeyPem = qs(o.daemonTlsClientKeyPem);
    p.daemonTlsPort = o.daemonTlsPort;
    return p;
}

// Puente Qt <-> capa base para el JSON. Se pasa por el texto compacto: es la única forma
// de que las dos representaciones coincidan sin duplicar el árbol de tipos, y el coste
// es irrelevante frente a los 40 ms de PBKDF2 que hay en cada campo cifrado.
QJsonObject aQtJson(const BJ::Value& v) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(BJ::toCompact(v))).object();
}

BJ::Value deQtJson(const QJsonObject& o) {
    BJ::Value v;
    std::string err;
    const QByteArray txt = QJsonDocument(o).toJson(QJsonDocument::Compact);
    BJ::parse(std::string(txt.constData(), static_cast<std::size_t>(txt.size())), v, &err);
    return v;
}

int ensurePort(const QString& connType, int port) {
    return CJ::ensurePort(bs(connType), port);
}

bool migratePsrpProfileToSsh(ConnectionProfile& p) {
    BP::ConnectionProfile o = aBase(p);
    const bool cambio = CJ::migratePsrpProfileToSsh(o);
    if (cambio) {
        p = deBase(o);
    }
    return cambio;
}

bool shouldForceLocalSudo(const ConnectionProfile& p) {
    return CJ::shouldForceLocalSudo(aBase(p));
}

bool profileHasDaemonTls(const ConnectionProfile& p) {
    return CJ::profileHasDaemonTls(aBase(p));
}

bool isLocalProfile(const ConnectionProfile& p) {
    return CJ::isLocalProfile(aBase(p));
}

QJsonObject connectionToJson(const ConnectionProfile& p) {
    return aQtJson(CJ::connectionToJson(aBase(p), bs(currentLocalMachineUid())));
}

QJsonObject connectionTrustToJson(const ConnectionProfile& p) {
    return aQtJson(CJ::connectionTrustToJson(aBase(p), bs(currentLocalMachineUid())));
}

ConnectionProfile connectionFromJson(const QJsonObject& obj) {
    return deBase(CJ::connectionFromJson(deQtJson(obj), bs(currentLocalMachineUid())));
}

int indexOfConnectionById(const QJsonArray& connections, const QString& id) {
    BJ::Array arr;
    arr.reserve(static_cast<std::size_t>(connections.size()));
    for (const QJsonValue& v : connections) {
        arr.push_back(deQtJson(v.toObject()));
    }
    return static_cast<int>(CJ::indexOfConnectionById(arr, bs(id)));
}

bool upsertConnectionJson(QJsonArray& connections, const ConnectionProfile& p) {
    if (p.id.trimmed().isEmpty()) {
        return false;
    }
    const int idx = indexOfConnectionById(connections, p.id);
    const QJsonObject obj = connectionToJson(p);
    if (idx >= 0) {
        connections[idx] = obj;
    } else {
        connections.push_back(obj);
    }
    return true;
}

}  // namespace

QJsonValue jsonValueFromVariant(const QVariant& v) {
    if (!v.isValid()) {
        return QJsonValue();
    }
    if (v.metaType().id() == QMetaType::QByteArray) {
        return QString::fromLatin1(v.toByteArray().toBase64());
    }
    if (v.metaType().id() == QMetaType::QStringList) {
        QJsonArray arr;
        const QStringList sl = v.toStringList();
        for (const QString& s : sl) {
            arr.push_back(s);
        }
        return arr;
    }
    return QJsonValue::fromVariant(v);
}

QJsonObject readJsonRootNoMigration(const QString& path) {
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        return QJsonObject();
    }
    return doc.object();
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

QString ConnectionStore::configDir() const {
    QString base = QDir::homePath() + "/.config/" + m_appName;
    QDir dir(base);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return dir.absolutePath();
}

QString ConnectionStore::configPath() const {
    return configDir() + QStringLiteral("/config.json");
}

QString ConnectionStore::trustStorePath() const {
    return configDir() + QStringLiteral("/trust-store.json");
}


QJsonObject ConnectionStore::loadConfigJson(QString* error) const {
    if (error) {
        error->clear();
    }
    QFile file(configPath());
    if (!file.exists()) {
        return QJsonObject();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_read_open_err"),
                         QStringLiteral("No se pudo abrir config.json"),
                         QStringLiteral("Could not open config.json"),
                         QStringLiteral("无法打开 config.json"));
        }
        return QJsonObject();
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_parse_err"),
                         QStringLiteral("config.json no es válido"),
                         QStringLiteral("config.json is invalid"),
                         QStringLiteral("config.json 无效"));
        }
        return QJsonObject();
    }
    return doc.object();
}

bool ConnectionStore::saveConfigJson(const QJsonObject& root, QString* error) const {
    if (error) {
        error->clear();
    }
    QDir dir(configDir());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_dir_err"),
                         QStringLiteral("No se pudo crear el directorio de configuración"),
                         QStringLiteral("Could not create configuration directory"),
                         QStringLiteral("无法创建配置目录"));
        }
        return false;
    }
    QFile file(configPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_write_open_err"),
                         QStringLiteral("No se pudo escribir config.json"),
                         QStringLiteral("Could not write config.json"),
                         QStringLiteral("无法写入 config.json"));
        }
        return false;
    }
    // Solo el dueño. Se fija ANTES de escribir: al revés quedaría un instante con el
    // fichero ya lleno de secretos cifrados y los permisos que dejara el umask.
    //
    // Nadie los fijaba hasta ahora. Que trust-store.json saliera 0600 y config.json 0664
    // era casualidad del umask del momento en que se crearon, no una decisión.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

QJsonObject ConnectionStore::loadTrustStoreJson(QString* error) const {
    if (error) {
        error->clear();
    }
    QFile file(trustStorePath());
    if (!file.exists()) {
        return QJsonObject();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = trk(QStringLiteral("t_trust_json_read_open_err"),
                         QStringLiteral("No se pudo abrir trust-store.json"),
                         QStringLiteral("Could not open trust-store.json"),
                         QStringLiteral("无法打开 trust-store.json"));
        }
        return QJsonObject();
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = trk(QStringLiteral("t_trust_json_parse_err"),
                         QStringLiteral("trust-store.json no es válido"),
                         QStringLiteral("trust-store.json is invalid"),
                         QStringLiteral("trust-store.json 无效"));
        }
        return QJsonObject();
    }
    return doc.object();
}

bool ConnectionStore::saveTrustStoreJson(const QJsonObject& root, QString* error) const {
    if (error) {
        error->clear();
    }
    QDir dir(configDir());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = trk(QStringLiteral("t_cfg_json_dir_err"),
                         QStringLiteral("No se pudo crear el directorio de configuración"),
                         QStringLiteral("Could not create configuration directory"),
                         QStringLiteral("无法创建配置目录"));
        }
        return false;
    }
    QFile file(trustStorePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = trk(QStringLiteral("t_trust_json_write_open_err"),
                         QStringLiteral("No se pudo escribir trust-store.json"),
                         QStringLiteral("Could not write trust-store.json"),
                         QStringLiteral("无法写入 trust-store.json"));
        }
        return false;
    }
    // Solo el dueño. Se fija ANTES de escribir: al revés quedaría un instante con el
    // fichero ya lleno de secretos cifrados y los permisos que dejara el umask.
    //
    // Nadie los fijaba hasta ahora. Que trust-store.json saliera 0600 y config.json 0664
    // era casualidad del umask del momento en que se crearon, no una decisión.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool ConnectionStore::upsertTrustStoreConnection(const ConnectionProfile& profile, QString& error) const {
    error.clear();
    if (profile.id.trimmed().isEmpty() || isLocalProfile(profile) || !profileHasDaemonTls(profile)) {
        return true;
    }
    ConnectionProfile toSave = profile;

    auto encryptIfNeeded = [&](QString& value, const QString& fieldLabel) -> bool {
        if (value.isEmpty() || SecretCipher::isEncrypted(value)) {
            return true;
        }
        if (m_masterPassword.isEmpty()) {
            error = trk(QStringLiteral("t_cstore_tls_mp_required_001"),
                        QStringLiteral("Password maestro requerido para cifrar %1"),
                        QStringLiteral("Master password required to encrypt %1"),
                        QStringLiteral("加密 %1 需要主密码"))
                        .arg(fieldLabel);
            return false;
        }
        QString encErr;
        QString encrypted;
        if (!SecretCipher::encryptEncv1(value, m_masterPassword, encrypted, encErr)) {
            error = trk(QStringLiteral("t_cstore_tls_enc_fail_001"),
                        QStringLiteral("No se pudo cifrar %1: %2"),
                        QStringLiteral("Could not encrypt %1: %2"),
                        QStringLiteral("无法加密 %1：%2"))
                        .arg(fieldLabel, encErr);
            return false;
        }
        value = encrypted;
        return true;
    };
    if (!encryptIfNeeded(toSave.daemonTlsServerCertPem, QStringLiteral("daemon_tls_server_cert_pem"))
        || !encryptIfNeeded(toSave.daemonTlsClientCertPem, QStringLiteral("daemon_tls_client_cert_pem"))
        || !encryptIfNeeded(toSave.daemonTlsClientKeyPem, QStringLiteral("daemon_tls_client_key_pem"))) {
        return false;
    }

    QString loadErr;
    QJsonObject root = loadTrustStoreJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("created_by"), QStringLiteral("ZFSMgr"));
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    const QJsonObject obj = connectionTrustToJson(toSave);
    const int idx = indexOfConnectionById(connections, toSave.id);
    if (idx >= 0) {
        connections[idx] = obj;
    } else {
        connections.push_back(obj);
    }
    root.insert(QStringLiteral("connections"), connections);
    return saveTrustStoreJson(root, &error);
}

bool ConnectionStore::deleteTrustStoreConnectionById(const QString& id, QString& error) const {
    error.clear();
    QString loadErr;
    QJsonObject root = loadTrustStoreJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    bool touched = false;
    for (int i = connections.size() - 1; i >= 0; --i) {
        const ConnectionProfile p = connectionFromJson(connections.at(i).toObject());
        if (p.id.trimmed().compare(id.trimmed(), Qt::CaseInsensitive) == 0) {
            connections.removeAt(i);
            touched = true;
        }
    }
    if (!touched) {
        return true;
    }
    root.insert(QStringLiteral("connections"), connections);
    return saveTrustStoreJson(root, &error);
}

void ConnectionStore::mergeTrustStoreIntoConnections(QVector<ConnectionProfile>& profiles, QStringList& warnings) const {
    QString loadErr;
    const QJsonObject root = loadTrustStoreJson(&loadErr);
    if (!loadErr.isEmpty()) {
        warnings.push_back(loadErr);
        return;
    }
    const QJsonArray trustConnections = root.value(QStringLiteral("connections")).toArray();
    for (const QJsonValue& v : trustConnections) {
        ConnectionProfile trust = connectionFromJson(v.toObject());
        if (trust.id.trimmed().isEmpty() || isLocalProfile(trust)) {
            continue;
        }
        auto decryptField = [&](QString& value, const QString& suffix) {
            if (!SecretCipher::isEncrypted(value)) {
                return;
            }
            QString dec;
            QString err;
            if (!m_masterPassword.isEmpty() && SecretCipher::decryptEncv1(value, m_masterPassword, dec, err)) {
                value = dec;
            } else {
                warnings.push_back(QStringLiteral("%1.%2: %3")
                                       .arg(trust.name.isEmpty() ? trust.id : trust.name,
                                            suffix,
                                            err.isEmpty()
                                                ? trk(QStringLiteral("t_cstore_auto_tls_dec_001"),
                                                      QStringLiteral("no se pudo descifrar"),
                                                      QStringLiteral("could not decrypt"),
                                                      QStringLiteral("无法解密"))
                                                : err));
            }
        };
        decryptField(trust.username, QStringLiteral("username"));
        decryptField(trust.password, QStringLiteral("password"));
        decryptField(trust.daemonTlsServerCertPem, QStringLiteral("daemon_tls_server_cert_pem"));
        decryptField(trust.daemonTlsClientCertPem, QStringLiteral("daemon_tls_client_cert_pem"));
        decryptField(trust.daemonTlsClientKeyPem, QStringLiteral("daemon_tls_client_key_pem"));

        int idx = -1;
        for (int i = 0; i < profiles.size(); ++i) {
            if (profiles[i].id.trimmed().compare(trust.id.trimmed(), Qt::CaseInsensitive) == 0) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) {
            if (!trust.daemonTlsServerCertPem.trimmed().isEmpty()) {
                profiles[idx].daemonTlsServerCertPem = trust.daemonTlsServerCertPem;
            }
            if (!trust.daemonTlsClientCertPem.trimmed().isEmpty()) {
                profiles[idx].daemonTlsClientCertPem = trust.daemonTlsClientCertPem;
            }
            if (!trust.daemonTlsClientKeyPem.trimmed().isEmpty()) {
                profiles[idx].daemonTlsClientKeyPem = trust.daemonTlsClientKeyPem;
            }
            if (trust.daemonTlsPort > 0 && trust.daemonTlsPort <= 65535) {
                profiles[idx].daemonTlsPort = trust.daemonTlsPort;
            }
            continue;
        }
        profiles.push_back(trust);
    }
}

bool ConnectionStore::migrateLegacyTlsToTrustStore(const QJsonArray& connections, QString& error) const {
    error.clear();
    for (const QJsonValue& v : connections) {
        const ConnectionProfile p = connectionFromJson(v.toObject());
        if (isLocalProfile(p) || !profileHasDaemonTls(p)) {
            continue;
        }
        if (!upsertTrustStoreConnection(p, error)) {
            return false;
        }
    }
    return true;
}

bool ConnectionStore::validateMasterPassword(QString& error) const {
    error.clear();
    const QJsonObject root = loadConfigJson(&error);
    if (!error.isEmpty()) {
        return false;
    }
    const QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    bool hasEncrypted = false;
    auto validateConnections = [&](const QJsonArray& items) -> bool {
        for (const QJsonValue& v : items) {
            const ConnectionProfile p = connectionFromJson(v.toObject());
            const QString connName = p.name.trimmed().isEmpty() ? p.id : p.name;

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

            if (!checkOne(p.username, QStringLiteral("usuario"))) {
                return false;
            }
            if (!checkOne(p.password, QStringLiteral("password"))) {
                return false;
            }
            if (!checkOne(p.daemonTlsServerCertPem, QStringLiteral("daemon_tls_server_cert_pem"))) {
                return false;
            }
            if (!checkOne(p.daemonTlsClientCertPem, QStringLiteral("daemon_tls_client_cert_pem"))) {
                return false;
            }
            if (!checkOne(p.daemonTlsClientKeyPem, QStringLiteral("daemon_tls_client_key_pem"))) {
                return false;
            }
        }
        return true;
    };
    if (!validateConnections(connections)) {
        return false;
    }
    QString trustErr;
    const QJsonObject trustRoot = loadTrustStoreJson(&trustErr);
    if (!trustErr.isEmpty()) {
        error = trustErr;
        return false;
    }
    if (!validateConnections(trustRoot.value(QStringLiteral("connections")).toArray())) {
        return false;
    }

    if (hasEncrypted && m_masterPassword.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto003"), QStringLiteral("Password maestro requerido"),
                    QStringLiteral("Master password required"),
                    QStringLiteral("需要主密码"));
        return false;
    }
    return true;
}

void ConnectionStore::ensureAppDefaults() const {
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    QJsonObject zdefs = root.value(QStringLiteral("ZPoolCreationDefaults")).toObject();
    bool touched = false;
    auto ensure = [&](const QString& key, const QJsonValue& value) {
        if (!zdefs.contains(key)) {
            zdefs.insert(key, value);
            touched = true;
        }
    };
    ensure(QStringLiteral("force"), true);
    ensure(QStringLiteral("altroot"), QString());
    ensure(QStringLiteral("ashift"), QStringLiteral("12"));
    ensure(QStringLiteral("autotrim"), QStringLiteral("on"));
    ensure(QStringLiteral("compatibility"), QStringLiteral("openzfs-2.4-linux"));
    ensure(QStringLiteral("fs_properties"),
           QStringLiteral("acltype=posixacl,"
                          "xattr=sa,"
                          "dnodesize=auto,"
                          "compression=lz4,"
                          "normalization=formD,"
                          "relatime=on,"
                          "canmount=noauto,"
                          "mountpoint=none"));
    if (touched) {
        root.insert(QStringLiteral("ZPoolCreationDefaults"), zdefs);
        QString saveErr;
        saveConfigJson(root, &saveErr);
    }
}








bool ConnectionStore::migratePsrpProfileToSshForTest(ConnectionProfile& p) {
    return migratePsrpProfileToSsh(p);
}

LoadResult ConnectionStore::loadConnections() const {
    LoadResult result;
    QString loadErr;
    const QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        result.warnings.push_back(loadErr);
    }
    const QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    QString trustMigrationError;
    if (!migrateLegacyTlsToTrustStore(connections, trustMigrationError)
        && !trustMigrationError.trimmed().isEmpty()) {
        result.warnings.push_back(trustMigrationError);
    }
    for (const QJsonValue& v : connections) {
        ConnectionProfile p = connectionFromJson(v.toObject());
        // Antes de ensurePort: la conversión decide el puerto y no debe pisarla nadie.
        if (migratePsrpProfileToSsh(p)) {
            result.warnings.push_back(
                QStringLiteral("%1: %2")
                    .arg(p.name.isEmpty() ? p.id : p.name,
                         trk(QStringLiteral("t_cstore_psrp001"),
                             QStringLiteral("usaba PSRP, que ya no está soportado. Se ha convertido a "
                                            "SSH en el puerto 22; revise usuario, clave y acceso SSH."),
                             QStringLiteral("used PSRP, which is no longer supported. It has been "
                                            "converted to SSH on port 22; check the user, key and SSH access."),
                             QStringLiteral("使用了不再受支持的 PSRP。已转换为端口 22 的 SSH；"
                                            "请检查用户、密钥和 SSH 访问。"))));
        }
        p.port = ensurePort(p.connType, p.port);

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

        const auto decryptField = [&](QString& value, const QString& suffix, const QString& fallbackTrKey) {
            if (!SecretCipher::isEncrypted(value)) {
                return;
            }
            QString dec;
            QString err;
            if (!m_masterPassword.isEmpty() && SecretCipher::decryptEncv1(value, m_masterPassword, dec, err)) {
                value = dec;
            } else {
                result.warnings.push_back(
                    QStringLiteral("%1.%2: %3").arg(p.name.isEmpty() ? p.id : p.name,
                                                    suffix,
                                                    err.isEmpty()
                                                        ? trk(fallbackTrKey,
                                                              QStringLiteral("no se pudo descifrar"),
                                                              QStringLiteral("could not decrypt"),
                                                              QStringLiteral("无法解密"))
                                                        : err));
            }
        };
        decryptField(p.daemonTlsServerCertPem,
                     QStringLiteral("daemon_tls_server_cert_pem"),
                     QStringLiteral("t_cstore_auto_tls_dec_001"));
        decryptField(p.daemonTlsClientCertPem,
                     QStringLiteral("daemon_tls_client_cert_pem"),
                     QStringLiteral("t_cstore_auto_tls_dec_002"));
        decryptField(p.daemonTlsClientKeyPem,
                     QStringLiteral("daemon_tls_client_key_pem"),
                     QStringLiteral("t_cstore_auto_tls_dec_003"));

        if (!p.name.isEmpty()) {
            result.profiles.push_back(p);
        }
    }
    mergeTrustStoreIntoConnections(result.profiles, result.warnings);

    // Determine the platform-correct values for the local profile.
    const QString localPlatformOsType =
#ifdef Q_OS_WIN
        QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
        QStringLiteral("macOS");
#elif defined(Q_OS_FREEBSD)
        QStringLiteral("FreeBSD");
#else
        QStringLiteral("Linux");
#endif
    const QString localFreshMachineUid = currentLocalMachineUid();

    bool hasLocal = false;
    for (ConnectionProfile& p : result.profiles) {
        if (p.id.compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0
            || p.connType.compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0) {
            hasLocal = true;
            // Always overwrite osType and machineUid with current platform values
            // so a profile saved from a different build or with a wrong default is corrected.
            p.osType = localPlatformOsType;
            if (!localFreshMachineUid.isEmpty()) {
                p.machineUid = localFreshMachineUid;
            }
            p.useSudo = shouldForceLocalSudo(p);
            break;
        }
    }
    if (!hasLocal) {
        ConnectionProfile local;
        local.id = QStringLiteral("local");
        local.name = QStringLiteral("Local");
        local.machineUid = localFreshMachineUid;
        local.connType = QStringLiteral("LOCAL");
        local.port = 0;
        local.host = QStringLiteral("localhost");
        local.sshAddressFamily = QStringLiteral("auto");
        const QString userEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USER"));
        const QString userEnvWin = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USERNAME"));
        local.username = !userEnv.trimmed().isEmpty() ? userEnv.trimmed() : userEnvWin.trimmed();
        local.osType = localPlatformOsType;
        local.useSudo = shouldForceLocalSudo(local);
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

    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();

    const QString targetName = profile.name.trimmed();
    for (int i = 0; i < connections.size(); ++i) {
        const ConnectionProfile existing = connectionFromJson(connections.at(i).toObject());
        const QString existingId = existing.id.trimmed();
        const QString existingName = existing.name.trimmed();
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

    ConnectionProfile toSave = profile;
    bool existingEndpointStable = true;
    bool hadExistingConnection = false;
    {
        const int existingIdx = indexOfConnectionById(connections, id);
        if (existingIdx >= 0) {
            hadExistingConnection = true;
            const ConnectionProfile existing = connectionFromJson(connections.at(existingIdx).toObject());
            QString existingUsername = existing.username;
            if (SecretCipher::isEncrypted(existingUsername) && !m_masterPassword.isEmpty()) {
                QString dec;
                QString decErr;
                if (SecretCipher::decryptEncv1(existingUsername, m_masterPassword, dec, decErr)) {
                    existingUsername = dec;
                }
            }
            const bool endpointStable =
                existing.host.trimmed().compare(profile.host.trimmed(), Qt::CaseInsensitive) == 0
                && ensurePort(existing.connType, existing.port) == ensurePort(profile.connType, profile.port)
                && existingUsername.trimmed().compare(profile.username.trimmed(), Qt::CaseInsensitive) == 0
                && existing.keyPath.trimmed() == profile.keyPath.trimmed();
            if (endpointStable) {
                if (toSave.daemonTlsServerCertPem.trimmed().isEmpty()) {
                    toSave.daemonTlsServerCertPem = existing.daemonTlsServerCertPem;
                }
                if (toSave.daemonTlsClientCertPem.trimmed().isEmpty()) {
                    toSave.daemonTlsClientCertPem = existing.daemonTlsClientCertPem;
                }
                if (toSave.daemonTlsClientKeyPem.trimmed().isEmpty()) {
                    toSave.daemonTlsClientKeyPem = existing.daemonTlsClientKeyPem;
                }
            }
            if (endpointStable && (toSave.daemonTlsPort <= 0 || toSave.daemonTlsPort > 65535)) {
                toSave.daemonTlsPort = (existing.daemonTlsPort > 0 && existing.daemonTlsPort <= 65535)
                                           ? existing.daemonTlsPort
                                           : 47653;
            }
            existingEndpointStable = endpointStable;
        }
    }
    toSave.id = id;
    toSave.port = ensurePort(profile.connType, profile.port);
    if (toSave.daemonTlsPort <= 0 || toSave.daemonTlsPort > 65535) {
        toSave.daemonTlsPort = 47653;
    }
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

    auto encryptIfNeeded = [&](QString& value, const QString& fieldLabel) -> bool {
        if (value.isEmpty() || SecretCipher::isEncrypted(value)) {
            return true;
        }
        if (m_masterPassword.isEmpty()) {
            error = trk(QStringLiteral("t_cstore_tls_mp_required_001"),
                        QStringLiteral("Password maestro requerido para cifrar %1"),
                        QStringLiteral("Master password required to encrypt %1"),
                        QStringLiteral("加密 %1 需要主密码"))
                        .arg(fieldLabel);
            return false;
        }
        QString encErr;
        QString encrypted;
        if (!SecretCipher::encryptEncv1(value, m_masterPassword, encrypted, encErr)) {
            error = trk(QStringLiteral("t_cstore_tls_enc_fail_001"),
                        QStringLiteral("No se pudo cifrar %1: %2"),
                        QStringLiteral("Could not encrypt %1: %2"),
                        QStringLiteral("无法加密 %1：%2"))
                        .arg(fieldLabel, encErr);
            return false;
        }
        value = encrypted;
        return true;
    };
    if (!encryptIfNeeded(toSave.daemonTlsServerCertPem, QStringLiteral("daemon_tls_server_cert_pem"))) {
        return false;
    }
    if (!encryptIfNeeded(toSave.daemonTlsClientCertPem, QStringLiteral("daemon_tls_client_cert_pem"))) {
        return false;
    }
    if (!encryptIfNeeded(toSave.daemonTlsClientKeyPem, QStringLiteral("daemon_tls_client_key_pem"))) {
        return false;
    }

    if (!upsertConnectionJson(connections, toSave)) {
        error = trk(QStringLiteral("t_cstore_json_upsert_err"),
                    QStringLiteral("No se pudo guardar la conexión"),
                    QStringLiteral("Could not save connection"),
                    QStringLiteral("无法保存连接"));
        return false;
    }
    if (hadExistingConnection && !existingEndpointStable && !deleteTrustStoreConnectionById(id, error)) {
        return false;
    }
    if (!upsertTrustStoreConnection(toSave, error)) {
        return false;
    }
    root.insert(QStringLiteral("connections"), connections);
    return saveConfigJson(root, &error);
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
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (int i = connections.size() - 1; i >= 0; --i) {
        const ConnectionProfile p = connectionFromJson(connections.at(i).toObject());
        if (p.id.trimmed().compare(clean, Qt::CaseInsensitive) == 0) {
            connections.removeAt(i);
        }
    }
    root.insert(QStringLiteral("connections"), connections);
    if (!deleteTrustStoreConnectionById(clean, error)) {
        return false;
    }
    return saveConfigJson(root, &error);
}

bool ConnectionStore::encryptStoredPasswords(QString& error) {
    error.clear();
    if (m_masterPassword.isEmpty()) {
        error = trk(QStringLiteral("t_cstore_auto014"), QStringLiteral("Password maestro requerido"),
                    QStringLiteral("Master password required"),
                    QStringLiteral("需要主密码"));
        return false;
    }
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (int i = 0; i < connections.size(); ++i) {
        ConnectionProfile p = connectionFromJson(connections.at(i).toObject());
        const QString current = p.password;
        if (!current.isEmpty() && !SecretCipher::isEncrypted(current)) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(current, m_masterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            p.password = encrypted;
            connections[i] = connectionToJson(p);
        }
        auto encryptField = [&](QString& value) -> bool {
            if (value.isEmpty() || SecretCipher::isEncrypted(value)) {
                return true;
            }
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(value, m_masterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            value = encrypted;
            return true;
        };
        bool touched = false;
        QString server = p.daemonTlsServerCertPem;
        QString clientCert = p.daemonTlsClientCertPem;
        QString clientKey = p.daemonTlsClientKeyPem;
        if (!server.isEmpty() && !SecretCipher::isEncrypted(server)) {
            if (!encryptField(server)) {
                return false;
            }
            touched = true;
        }
        if (!clientCert.isEmpty() && !SecretCipher::isEncrypted(clientCert)) {
            if (!encryptField(clientCert)) {
                return false;
            }
            touched = true;
        }
        if (!clientKey.isEmpty() && !SecretCipher::isEncrypted(clientKey)) {
            if (!encryptField(clientKey)) {
                return false;
            }
            touched = true;
        }
        if (touched) {
            p.daemonTlsServerCertPem = server;
            p.daemonTlsClientCertPem = clientCert;
            p.daemonTlsClientKeyPem = clientKey;
            connections[i] = connectionToJson(p);
        }
    }
    root.insert(QStringLiteral("connections"), connections);
    if (!saveConfigJson(root, &error)) {
        return false;
    }

    QString trustErr;
    QJsonObject trustRoot = loadTrustStoreJson(&trustErr);
    if (!trustErr.isEmpty()) {
        error = trustErr;
        return false;
    }
    QJsonArray trustConnections = trustRoot.value(QStringLiteral("connections")).toArray();
    bool trustTouched = false;
    for (int i = 0; i < trustConnections.size(); ++i) {
        ConnectionProfile p = connectionFromJson(trustConnections.at(i).toObject());
        auto encryptField = [&](QString& value) -> bool {
            if (value.isEmpty() || SecretCipher::isEncrypted(value)) {
                return true;
            }
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(value, m_masterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            value = encrypted;
            return true;
        };
        if (!encryptField(p.daemonTlsServerCertPem)
            || !encryptField(p.daemonTlsClientCertPem)
            || !encryptField(p.daemonTlsClientKeyPem)) {
            return false;
        }
        trustConnections[i] = connectionTrustToJson(p);
        trustTouched = true;
    }
    if (trustTouched) {
        trustRoot.insert(QStringLiteral("schema"), 1);
        trustRoot.insert(QStringLiteral("created_by"), QStringLiteral("ZFSMgr"));
        trustRoot.insert(QStringLiteral("connections"), trustConnections);
        if (!saveTrustStoreJson(trustRoot, &error)) {
            return false;
        }
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
    QString loadErr;
    QJsonObject root = loadConfigJson(&loadErr);
    if (!loadErr.isEmpty()) {
        error = loadErr;
        return false;
    }
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (int i = 0; i < connections.size(); ++i) {
        ConnectionProfile p = connectionFromJson(connections.at(i).toObject());
        const QString current = p.password;
        QString plain = current;
        if (!current.isEmpty() && SecretCipher::isEncrypted(current)) {
            QString decErr;
            if (!SecretCipher::decryptEncv1(current, oldMasterPassword, plain, decErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, decErr);
                return false;
            }
        }
        if (!plain.isEmpty()) {
            QString encErr;
            QString encrypted;
            if (!SecretCipher::encryptEncv1(plain, newMasterPassword, encrypted, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            p.password = encrypted;
            connections[i] = connectionToJson(p);
        }

        auto rotateField = [&](QString& value) -> bool {
            if (value.isEmpty()) {
                return true;
            }
            QString plainField = value;
            if (SecretCipher::isEncrypted(value)) {
                QString decErr;
                if (!SecretCipher::decryptEncv1(value, oldMasterPassword, plainField, decErr)) {
                    error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, decErr);
                    return false;
                }
            }
            QString encErr;
            QString encryptedField;
            if (!SecretCipher::encryptEncv1(plainField, newMasterPassword, encryptedField, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            value = encryptedField;
            return true;
        };
        bool tlsTouched = false;
        QString server = p.daemonTlsServerCertPem;
        QString clientCert = p.daemonTlsClientCertPem;
        QString clientKey = p.daemonTlsClientKeyPem;
        if (!server.isEmpty()) {
            if (!rotateField(server)) {
                return false;
            }
            tlsTouched = true;
        }
        if (!clientCert.isEmpty()) {
            if (!rotateField(clientCert)) {
                return false;
            }
            tlsTouched = true;
        }
        if (!clientKey.isEmpty()) {
            if (!rotateField(clientKey)) {
                return false;
            }
            tlsTouched = true;
        }
        if (tlsTouched) {
            p.daemonTlsServerCertPem = server;
            p.daemonTlsClientCertPem = clientCert;
            p.daemonTlsClientKeyPem = clientKey;
            connections[i] = connectionToJson(p);
        }
    }
    root.insert(QStringLiteral("connections"), connections);
    if (!saveConfigJson(root, &error)) {
        return false;
    }
    QString trustErr;
    QJsonObject trustRoot = loadTrustStoreJson(&trustErr);
    if (!trustErr.isEmpty()) {
        error = trustErr;
        return false;
    }
    QJsonArray trustConnections = trustRoot.value(QStringLiteral("connections")).toArray();
    bool trustTouched = false;
    for (int i = 0; i < trustConnections.size(); ++i) {
        ConnectionProfile p = connectionFromJson(trustConnections.at(i).toObject());
        auto rotateField = [&](QString& value) -> bool {
            if (value.isEmpty()) {
                return true;
            }
            QString plainField = value;
            if (SecretCipher::isEncrypted(value)) {
                QString decErr;
                if (!SecretCipher::decryptEncv1(value, oldMasterPassword, plainField, decErr)) {
                    error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, decErr);
                    return false;
                }
            }
            QString encErr;
            QString encryptedField;
            if (!SecretCipher::encryptEncv1(plainField, newMasterPassword, encryptedField, encErr)) {
                error = QStringLiteral("%1: %2").arg(p.name.isEmpty() ? p.id : p.name, encErr);
                return false;
            }
            value = encryptedField;
            return true;
        };
        if (!rotateField(p.daemonTlsServerCertPem)
            || !rotateField(p.daemonTlsClientCertPem)
            || !rotateField(p.daemonTlsClientKeyPem)) {
            return false;
        }
        trustConnections[i] = connectionTrustToJson(p);
        trustTouched = true;
    }
    if (trustTouched) {
        trustRoot.insert(QStringLiteral("schema"), 1);
        trustRoot.insert(QStringLiteral("created_by"), QStringLiteral("ZFSMgr"));
        trustRoot.insert(QStringLiteral("connections"), trustConnections);
        if (!saveTrustStoreJson(trustRoot, &error)) {
            return false;
        }
    }
    m_masterPassword = newMasterPassword;
    return true;
}
