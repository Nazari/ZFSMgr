#include "connectionstore.h"

#include "base/connectionjson.h"
#include "base/storefiles.h"
#include "base/storewarnings.h"

namespace CJ = zfsmgr::base::connjson;
namespace BP = zfsmgr::base;
namespace BJ = zfsmgr::base::json;
namespace BS = zfsmgr::base::store;
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

std::string bs(const QString& s) { return s.toStdString(); }

BP::ConnectionProfile aBase(const ConnectionProfile& p) { return toBaseProfile(p); }
ConnectionProfile deBase(const BP::ConnectionProfile& o) { return fromBaseProfile(o); }

}  // namespace


namespace {

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


// --- Traducción de los motivos tipificados.
//
// La capa base devuelve un `Aviso` con motivo y datos; el texto se decide AQUÍ, que es
// donde vive el idioma. Es el mismo reparto que en connectioncapabilities, y es lo que
// permitió sacar el almacén de Qt sin llevarse consigo el sistema de traducción.
// Atajo para los sitios que solo necesitan el texto de un motivo.
QString ConnectionStore::aviso(BS::Motivo m, const QString& conexion, const QString& campo,
                               const QString& detalle) const {
    return traduce(BS::Aviso{m, conexion.toStdString(), campo.toStdString(), detalle.toStdString()});
}

QString ConnectionStore::traduce(const BS::Aviso& a) const {
    using M = BS::Motivo;
    const QString conexion = QString::fromStdString(a.conexion);
    const QString campo = QString::fromStdString(a.campo);
    const QString detalle = QString::fromStdString(a.detalle);
    switch (a.motivo) {
        case M::Ninguno:
            return QString();
        case M::ConfigNoSeAbre:
            return trk(QStringLiteral("t_cfg_json_read_open_err"),
                       QStringLiteral("No se pudo abrir config.json"),
                       QStringLiteral("Could not open config.json"),
                       QStringLiteral("无法打开 config.json"));
        case M::ConfigNoValido:
            return trk(QStringLiteral("t_cfg_json_parse_err"),
                       QStringLiteral("config.json no es válido"),
                       QStringLiteral("config.json is invalid"),
                       QStringLiteral("config.json 无效"));
        case M::ConfigDirNoSeCrea:
            return trk(QStringLiteral("t_cfg_json_dir_err"),
                       QStringLiteral("No se pudo crear el directorio de configuración"),
                       QStringLiteral("Could not create configuration directory"),
                       QStringLiteral("无法创建配置目录"));
        case M::ConfigNoSeEscribe:
            return trk(QStringLiteral("t_cfg_json_write_open_err"),
                       QStringLiteral("No se pudo escribir config.json"),
                       QStringLiteral("Could not write config.json"),
                       QStringLiteral("无法写入 config.json"));
        case M::TrustNoSeAbre:
            return trk(QStringLiteral("t_trust_json_read_open_err"),
                       QStringLiteral("No se pudo abrir trust-store.json"),
                       QStringLiteral("Could not open trust-store.json"),
                       QStringLiteral("无法打开 trust-store.json"));
        case M::TrustNoValido:
            return trk(QStringLiteral("t_trust_json_parse_err"),
                       QStringLiteral("trust-store.json no es válido"),
                       QStringLiteral("trust-store.json is invalid"),
                       QStringLiteral("trust-store.json 无效"));
        case M::TrustNoSeEscribe:
            return trk(QStringLiteral("t_trust_json_write_open_err"),
                       QStringLiteral("No se pudo escribir trust-store.json"),
                       QStringLiteral("Could not write trust-store.json"),
                       QStringLiteral("无法写入 trust-store.json"));
        case M::ClaveMaestraRequerida:
            return trk(QStringLiteral("t_cstore_auto003"),
                       QStringLiteral("Password maestro requerido"),
                       QStringLiteral("Master password required"),
                       QStringLiteral("需要主密码"));
        case M::ClaveMaestraRequeridaParaCifrar:
            return trk(QStringLiteral("t_cstore_tls_mp_required_001"),
                       QStringLiteral("Password maestro requerido para cifrar %1"),
                       QStringLiteral("Master password required to encrypt %1"),
                       QStringLiteral("需要主密码才能加密 %1")).arg(campo);
        case M::NuevaClaveMaestraVacia:
            return trk(QStringLiteral("t_cstore_auto016"),
                       QStringLiteral("Nuevo password maestro vacío"),
                       QStringLiteral("New master password is empty"),
                       QStringLiteral("新主密码为空"));
        case M::NoSeCifra:
            return trk(QStringLiteral("t_cstore_tls_enc_fail_001"),
                       QStringLiteral("No se pudo cifrar %1: %2"),
                       QStringLiteral("Could not encrypt %1: %2"),
                       QStringLiteral("无法加密 %1：%2")).arg(campo, detalle);
        case M::NoSeDescifra:
            return QStringLiteral("%1.%2: %3").arg(
                conexion, campo,
                detalle.trimmed().isEmpty()
                    ? trk(QStringLiteral("t_cstore_auto_tls_dec_001"),
                          QStringLiteral("no se pudo descifrar"),
                          QStringLiteral("could not decrypt"),
                          QStringLiteral("无法解密"))
                    : detalle);
        case M::CampoIncorrecto:
            return trk(QStringLiteral("t_cstore_auto002"),
                       QStringLiteral("%1: %2 incorrecto"),
                       QStringLiteral("%1: invalid %2"),
                       QStringLiteral("%1：%2 无效")).arg(conexion, campo);
        case M::IdVacio:
            return trk(QStringLiteral("t_cstore_auto012"),
                       QStringLiteral("ID vacío"),
                       QStringLiteral("Empty ID"),
                       QStringLiteral("ID 为空"));
        case M::NombreRequerido:
            return trk(QStringLiteral("t_cstore_auto006"),
                       QStringLiteral("Nombre requerido"),
                       QStringLiteral("Name required"),
                       QStringLiteral("需要名称"));
        case M::HostRequerido:
            return trk(QStringLiteral("t_cstore_auto007"),
                       QStringLiteral("Host requerido"),
                       QStringLiteral("Host required"),
                       QStringLiteral("需要主机"));
        case M::UsuarioRequerido:
            return trk(QStringLiteral("t_cstore_auto008"),
                       QStringLiteral("Usuario requerido"),
                       QStringLiteral("User required"),
                       QStringLiteral("需要用户"));
        case M::NombreDuplicado:
            return trk(QStringLiteral("t_conn_name_unique_01"),
                       QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                       QStringLiteral("Connection name already exists. It must be unique."),
                       QStringLiteral("连接名称已存在，必须唯一。"));
        case M::NoSeGuardaConexion:
            return trk(QStringLiteral("t_cstore_json_upsert_err"),
                       QStringLiteral("No se pudo guardar la conexión"),
                       QStringLiteral("Could not save the connection"),
                       QStringLiteral("无法保存该连接"));
        case M::PerfilPsrpConvertido:
            return QStringLiteral("%1: %2").arg(
                conexion,
                trk(QStringLiteral("t_cstore_psrp001"),
                    QStringLiteral("usaba PSRP, que ya no está soportado. Se ha convertido a "
                                   "SSH en el puerto 22; revise usuario, clave y acceso SSH."),
                    QStringLiteral("used PSRP, which is no longer supported. It has been "
                                   "converted to SSH on port 22; check user, key and SSH access."),
                    QStringLiteral("此前使用 PSRP，该方式已不再支持。已转换为端口 22 上的 "
                                   "SSH；请检查用户、密钥与 SSH 访问。")));
    }
    return QString();
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
    BS::Aviso a;
    const BJ::Value v = BS::leerConfig(bs(configDir()), a);
    if (error) {
        *error = traduce(a);
    }
    return aQtJson(v);
}

bool ConnectionStore::saveConfigJson(const QJsonObject& root, QString* error) const {
    BS::Aviso a;
    const bool ok = BS::escribirConfig(bs(configDir()), deQtJson(root), a);
    if (error) {
        *error = traduce(a);
    }
    return ok;
}

QJsonObject ConnectionStore::loadTrustStoreJson(QString* error) const {
    BS::Aviso a;
    const BJ::Value v = BS::leerTrustStore(bs(configDir()), a);
    if (error) {
        *error = traduce(a);
    }
    return aQtJson(v);
}

bool ConnectionStore::saveTrustStoreJson(const QJsonObject& root, QString* error) const {
    BS::Aviso a;
    const bool ok = BS::escribirTrustStore(bs(configDir()), deQtJson(root), a);
    if (error) {
        *error = traduce(a);
    }
    return ok;
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
            error = aviso(BS::Motivo::ClaveMaestraRequeridaParaCifrar, QString(), fieldLabel);
            return false;
        }
        QString encErr;
        QString encrypted;
        if (!SecretCipher::encryptEncv1(value, m_masterPassword, encrypted, encErr)) {
            error = aviso(BS::Motivo::NoSeCifra, QString(), fieldLabel, encErr);
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
                warnings.push_back(aviso(BS::Motivo::NoSeDescifra,
                                         trust.name.isEmpty() ? trust.id : trust.name,
                                         suffix, err));
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
                    error = aviso(BS::Motivo::CampoIncorrecto, connName, fieldName);
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
        error = aviso(BS::Motivo::ClaveMaestraRequerida);
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
                aviso(BS::Motivo::PerfilPsrpConvertido, p.name.isEmpty() ? p.id : p.name));
        }
        p.port = ensurePort(p.connType, p.port);

        if (SecretCipher::isEncrypted(p.username)) {
            QString dec;
            QString err;
            if (!m_masterPassword.isEmpty() && SecretCipher::decryptEncv1(p.username, m_masterPassword, dec, err)) {
                p.username = dec;
            } else {
                result.warnings.push_back(aviso(BS::Motivo::NoSeDescifra, p.name.isEmpty() ? p.id : p.name, QStringLiteral("username"), err));
            }
        }

        if (SecretCipher::isEncrypted(p.password)) {
            QString dec;
            QString err;
            if (!m_masterPassword.isEmpty() && SecretCipher::decryptEncv1(p.password, m_masterPassword, dec, err)) {
                p.password = dec;
            } else {
                result.warnings.push_back(aviso(BS::Motivo::NoSeDescifra, p.name.isEmpty() ? p.id : p.name, QStringLiteral("password"), err));
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
        error = aviso(BS::Motivo::NombreRequerido);
        return false;
    }
    if (profile.host.trimmed().isEmpty()) {
        error = aviso(BS::Motivo::HostRequerido);
        return false;
    }
    if (profile.username.trimmed().isEmpty()) {
        error = aviso(BS::Motivo::UsuarioRequerido);
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
            error = aviso(BS::Motivo::NombreDuplicado);
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
            error = aviso(BS::Motivo::ClaveMaestraRequeridaParaCifrar, QString(), QStringLiteral("password"));
            return false;
        }
        QString encErr;
        QString encrypted;
        if (!SecretCipher::encryptEncv1(storedPassword, m_masterPassword, encrypted, encErr)) {
            error = aviso(BS::Motivo::NoSeCifra, QString(), QStringLiteral("password"), encErr);
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
            error = aviso(BS::Motivo::ClaveMaestraRequeridaParaCifrar, QString(), fieldLabel);
            return false;
        }
        QString encErr;
        QString encrypted;
        if (!SecretCipher::encryptEncv1(value, m_masterPassword, encrypted, encErr)) {
            error = aviso(BS::Motivo::NoSeCifra, QString(), fieldLabel, encErr);
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
        error = aviso(BS::Motivo::NoSeGuardaConexion);
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
        error = aviso(BS::Motivo::IdVacio);
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
        error = aviso(BS::Motivo::ClaveMaestraRequerida);
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
    // La rotación la hace la CAPA BASE. Aquí solo se traduce el aviso.
    //
    // Estaban las dos: 138 líneas en este fichero, con Qt, y el intérprete sin poder
    // cambiar la clave maestra porque no enlaza Qt. Dos implementaciones de esto habrían
    // sido dos formas distintas de dejar la configuración a medio cifrar. Y allí, además,
    // se puede comprobar sin arrancar una ventana.
    error.clear();
    std::string copiaSufijo;
    zfsmgr::base::store::Aviso aviso;
    if (zfsmgr::base::store::rotaClaveMaestra(configDir().toStdString(),
                                              oldMasterPassword.toStdString(),
                                              newMasterPassword.toStdString(),
                                              copiaSufijo, aviso)) {
        m_masterPassword = newMasterPassword;
        return true;
    }
    error = traduce(aviso);
    return false;
}
