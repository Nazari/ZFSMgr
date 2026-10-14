#include "mainwindow.h"

#include <QJsonArray>
#include <QJsonObject>

#include "agentmsg.h"

#include "transport.h"

#include "base/json.h"
#include "base/transportcmd.h"
#include "base/transportrpc.h"
#include "base/transporttunnel.h"
#include "base/processes.h"
#include "base/tlsclient.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "listings.h"
#include "agentversion.h"
#include "daemonpayload.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>

#include <mutex>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMutexLocker>

#include <algorithm>
#include <cstring>

namespace BT = zfsmgr::base::transport;

namespace {
// Las rutas del agente EN ESTA MÁQUINA viven en base/transportcmd.h, con los mismos
// #ifdef. Estaban fijas a las POSIX, y por eso en Windows la conexión Local nunca
// encontraba su material TLS, no podía hablar por RPC con su propio daemon y todo el
// refresco caía al camino de shell: el refresco medía 102 s, de los que 99 eran sondas.

// Espejo con QString del de la capa base, para no tocar los sitios que lo leen.
struct LocalAgentConfig {
    QString bindAddress;
    quint16 port{47653};
    QString tlsCertPath;
    QString tlsClientCertPath;
    QString tlsClientKeyPath;
};;

;

QMutex s_remoteDaemonTlsPersistMutex;

// Verificación del daemon por FIJACIÓN del certificado, no por confianza PKI.
//
// El certificado del daemon es autofirmado y lo hemos traído nosotros por SSH, así
// que sabemos exactamente cuál debe presentar: comparar el certificado entero es
// más estricto que aceptarlo como CA y comprobar el nombre de host, porque no
// delega en ninguna cadena ni depende de cómo se interprete el nombre.
//
// Y sobre todo, no depende de la política del backend TLS. El de OpenSSL y el
// SecureTransport de Apple discrepan: en macOS ningún certificado del daemon
// validaba nunca ("The root CA certificate is not trusted for this purpose"),
// incluso con subjectAltName, extendedKeyUsage, keyUsage y basicConstraints
// correctos — comprobado sobre el certificado real. La autenticación mutua se
// mantiene: el cliente sigue enviando su certificado y el daemon sigue
// exigiéndolo con SSL_VERIFY_PEER.
// El material TLS llega por parámetro, no se lee del disco.
//
// Vivía en /etc/zfsmgr/tls con permisos 600 de root, así que la interfaz —que corre
// como usuario normal— nunca podía abrirlo y el RPC local no llegaba a intentarse.
// No se notaba porque el camino clásico leía los pools sin privilegios; al retirarlo,
// la conexión Local se quedó sin datos.

} // end anonymous namespace — runSshRawNoLog must be externally linkable for background watcher threads.

bool runSshRawNoLog(const ConnectionProfile& p,
                    const QString& remoteCmd,
                    int timeoutMs,
                    QString& out,
                    QString& err,
                    int& rc) {
    std::string o;
    std::string e;
    const bool ok = BT::runSshRaw(toBaseProfile(p), remoteCmd.toStdString(), timeoutMs, o, e, rc);
    out = QString::fromStdString(o);
    err = QString::fromStdString(e);
    return ok;
}

namespace {

using mwhelpers::isMountedValueTrue;
using mwhelpers::findLocalExecutable;
using mwhelpers::normalizeDriveLetterValue;
using mwhelpers::oneLine;
using mwhelpers::parentDatasetName;
using mwhelpers::shSingleQuote;
using mwhelpers::sshAddressFamilyOption;
using mwhelpers::sshBaseCommand;
using mwhelpers::sshControlPath;
using mwhelpers::sshUserHost;
using mwhelpers::sshUserHostPort;
} // namespace

namespace {

} // namespace

bool transport::tryRunRemoteAgentRpcViaTunnel(TransportSession& ses,
                                              const ConnectionProfile& p,
                                              const QStringList& agentArgs,
                                              int timeoutMs,
                                              QString& out,
                                              QString& err,
                                              int& rc,
                                              BT::FailureReason* failureReason,
                                              bool* commandMayHaveRunOut) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(agentArgs.size()));
    for (const QString& a : agentArgs) {
        args.push_back(a.toStdString());
    }
    std::string o;
    std::string e;
    const bool ok = BT::tryRunRemoteAgentRpcViaTunnel(ses, toBaseProfile(p), args, timeoutMs, o, e,
                                                      rc, failureReason, commandMayHaveRunOut);
    out = QString::fromStdString(o);
    err = QString::fromStdString(e);
    return ok;
}

namespace {
// Caché del material TLS local. Sin ella habría que pedir la contraseña de sudo en
// cada orden, que es lo que hace inusable pedirla siquiera una vez.
struct LocalDaemonTlsCacheEntry {
    QByteArray serverCertPem;
    QByteArray clientCertPem;
    QByteArray clientKeyPem;
    quint16 port{47653};
    QDateTime fetchedAtUtc;
};
QMutex s_localDaemonTlsCacheMutex;
LocalDaemonTlsCacheEntry s_localDaemonTlsCache;
} // namespace

bool transport::ensureLocalDaemonTlsMaterial(TransportSession& ses,
                                             QByteArray& serverCertPem,
                                             QByteArray& clientCertPem,
                                             QByteArray& clientKeyPem,
                                             quint16& daemonPort) {
    std::string srv;
    std::string cli;
    std::string key;
    std::uint16_t port = 47653;
    if (!BT::ensureLocalDaemonTlsMaterial(ses, srv, cli, key, port)) {
        return false;
    }
    serverCertPem = QByteArray::fromStdString(srv);
    clientCertPem = QByteArray::fromStdString(cli);
    clientKeyPem = QByteArray::fromStdString(key);
    daemonPort = port;
    return true;
}

bool transport::tryAgentRpcOverSsh(TransportSession& ses,
                                   const ConnectionProfile& p,
                                   const QStringList& agentArgs,
                                   int timeoutMs,
                                   QString& out,
                                   QString& err,
                                   int& rc,
                                   const std::function<void(const QString&)>& onStdoutLine,
                                   const std::function<void(const QString&)>& onStderrLine,
                                   bool echoOutputToLog) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(agentArgs.size()));
    for (const QString& a : agentArgs) {
        args.push_back(a.toStdString());
    }
    std::string o;
    std::string e;
    const auto puente = [](const std::function<void(const QString&)>& cb) {
        return cb ? std::function<void(const std::string&)>(
                        [cb](const std::string& l) { cb(QString::fromStdString(l)); })
                  : std::function<void(const std::string&)>();
    };
    const bool ok = BT::tryAgentRpcOverSsh(ses, toBaseProfile(p), args, timeoutMs, o, e, rc,
                                           puente(onStdoutLine), puente(onStderrLine),
                                           echoOutputToLog);
    out = QString::fromStdString(o);
    err = QString::fromStdString(e);
    return ok;
}

// Invocación del agente a partir de sus argumentos, sin pasar por una cadena de shell.
//
// El protocolo de cable ya era argv: lo que sobraba era construir una cadena en el
// sitio de llamada para que runSsh la volviera a parsear. Ese ida y vuelta perdía
// argumentos con ';', '&' o '|' —un directorio elegido por el usuario, por ejemplo—,
// descartaba la entrada estándar en silencio y rechazaba los verbos de trabajos.
//
// El respaldo por shell sigue existiendo, pero se renderiza a partir de los mismos
// argumentos y solo cuando hace falta.
bool MainWindow::getDatasetPropertyForTest(int connIdx, const QString& dataset,
                                           const QString& prop, QString& valueOut) {
    return getDatasetProperty(connIdx, dataset, prop, valueOut);
}

bool transport::runSsh(TransportSession& ses,
                       const ConnectionProfile& p,
                       const QString& remoteCmd,
                       int timeoutMs,
                       QString& out,
                       QString& err,
                       int& rc,
                       const std::function<void(const QString&)>& onStdoutLine,
                       const std::function<void(const QString&)>& onStderrLine,
                       const std::function<void(int)>& onIdleTimeoutRemaining,
                       const QByteArray& stdinPayload,
                       bool allowAgentRpc,
                       bool echoOutputToLog) {
    const auto puente = [](const std::function<void(const QString&)>& cb) {
        return cb ? std::function<void(const std::string&)>(
                        [cb](const std::string& l) { cb(QString::fromStdString(l)); })
                  : std::function<void(const std::string&)>();
    };
    std::string o;
    std::string e;
    const bool ok = BT::runSsh(
        ses, toBaseProfile(p), remoteCmd.toStdString(), timeoutMs, o, e, rc, puente(onStdoutLine),
        puente(onStderrLine), onIdleTimeoutRemaining,
        std::string(stdinPayload.constData(), static_cast<std::size_t>(stdinPayload.size())),
        allowAgentRpc, echoOutputToLog);
    out = QString::fromStdString(o);
    err = QString::fromStdString(e);
    return ok;
}

void MainWindow::closeAllSshControlMasters() {
    if (m_conns.profiles.isEmpty()) {
        return;
    }
    QSet<QString> seen;
    for (const ConnectionProfile& p : m_conns.profiles) {
        if (isLocalConnection(p)) {
            continue;
        }
        if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString fingerprint = QStringLiteral("%1|%2|%3|%4")
                                        .arg(p.username,
                                             p.host,
                                             QString::number((p.port > 0) ? p.port : 22),
                                             p.keyPath);
        if (seen.contains(fingerprint)) {
            continue;
        }
        seen.insert(fingerprint);

        QStringList args;
        const QString familyOpt = sshAddressFamilyOption(p);
        if (!familyOpt.isEmpty()) {
            args << familyOpt;
        }
        args << "-o" << "BatchMode=yes";
        args << "-o" << "LogLevel=ERROR";
        args << "-o" << "StrictHostKeyChecking=accept-new";
        args << "-o" << QStringLiteral("ControlPath=%1").arg(sshControlPath());
        if (p.port > 0) {
            args << "-p" << QString::number(p.port);
        }
        if (!p.keyPath.isEmpty()) {
            args << "-i" << p.keyPath;
        }
        args << "-O" << "exit";
        args << sshUserHost(p);
        QProcess proc;
        proc.start(QStringLiteral("ssh"), args);
        proc.waitForFinished(1500);
    }
}

void MainWindow::closeAllRemoteDaemonRpcTunnels() {
    BT::closeAllTunnels(m_transport);
}

// --- Envoltorios: la lógica vive en `transport::`, que no depende de la ventana. Estos
// existen para no tocar los puntos de llamada en el mismo cambio, y desaparecen cuando
// migren.
bool MainWindow::isLocalConnection(const ConnectionProfile& p) const {
    return transport::isLocalConnection(p);
}

bool MainWindow::isWindowsConnection(const ConnectionProfile& p) const {
    return transport::isWindowsConnection(p);
}

bool transport::isLocalConnection(const ConnectionProfile& p) {
    return BT::isLocalConnection(toBaseProfile(p));
}

bool MainWindow::isLocalConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return false;
    }
    return isLocalConnection(m_conns.profiles[connIdx]);
}

bool transport::isWindowsConnection(const ConnectionProfile& p) {
    return BT::isWindowsConnection(toBaseProfile(p));
}

bool MainWindow::isWindowsConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return false;
    }
    return isWindowsConnection(m_conns.profiles[connIdx]);
}

bool MainWindow::supportsAlternateDatasetMount(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return false;
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    if (!featureAvailable(connIdx, zfsmgr::caps::Feature::AlternateMount)) {
        return false;
    }
    const QString os = p.osType.trimmed().toLower();
    return os.contains(QStringLiteral("linux"))
           || os.contains(QStringLiteral("freebsd"))
           || os.contains(QStringLiteral("macos"))
           || os.contains(QStringLiteral("darwin"))
           || os.contains(QStringLiteral("os x"));
}

QString transport::wrapRemoteCommand(const ConnectionProfile& p,
                                     const QString& remoteCmd) {
    return QString::fromStdString(BT::wrapRemoteCommand(toBaseProfile(p), remoteCmd.toStdString()));
}

bool MainWindow::getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut) {
    valueOut.clear();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || dataset.isEmpty() || prop.isEmpty()) {
        return false;
    }
    // The shell's `ls <url>#properties/<prop>`: one property answers its bare value.
    const QString ds = dataset.section(QLatin1Char('@'), 0, 0);
    const QString snap =
        dataset.contains(QLatin1Char('@')) ? dataset.section(QLatin1Char('@'), 1) : QString();
    const QString clave = cliConnName(connIdx);
    const CliService::Result r = m_cliReader->read(
        clave, QStringLiteral("ls %1#properties/%2").arg(zfsmUrl(connIdx, ds, snap), prop), 15000);
    if (r.rc != 0) {
        return false;
    }
    valueOut = r.out.trimmed();
    return true;
}

QString MainWindow::effectiveMountPath(int connIdx,
                                       const QString& poolName,
                                       const QString& datasetName,
                                       const QString& mountpointHint,
                                       const QString& mountedValue) {
    auto normalizePath = [](const QString& raw) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty()) {
            return trimmed;
        }
        // Keep remote shell paths stable across platforms/filesystems that are
        // sensitive to Unicode normalization (for example NFC vs NFD on Linux).
        return trimmed.normalized(QString::NormalizationForm_C);
    };
    if (!isWindowsConnection(connIdx)) {
        return normalizePath(mountpointHint);
    }
    if (!isMountedValueTrue(mountedValue)) {
        return normalizePath(mountpointHint);
    }
    if (poolName.isEmpty() || datasetName.isEmpty()) {
        return normalizePath(mountpointHint);
    }
    if (!(datasetName == poolName || datasetName.startsWith(poolName + QStringLiteral("/")))) {
        return normalizePath(mountpointHint);
    }

    // Primero, la lista REAL de montajes, que da el punto de cada dataset por separado.
    //
    // El cálculo de más abajo sube por los padres buscando `driveletter`, y esa propiedad
    // SE HEREDA: un hijo de un pool montado en Z: devuelve Z igual que su padre, así que
    // se le tomaba por la raíz de la unidad y se le asignaba `Z:\`. Result visto en
    // Windows: el árbol mostraba EL MISMO contenido en winpool y en winpool/subds1.
    //
    // La lista de montajes no se hereda ni se deduce: dice dónde está montado cada uno.
    if (connIdx >= 0 && connIdx < m_conns.states.size()) {
        for (const QPair<QString, QString>& row : m_conns.states[connIdx].mountedDatasets) {
            if (row.first.trimmed() != datasetName) {
                continue;
            }
            QString mp = row.second.trimmed();
            if (mp.isEmpty()) {
                break;
            }
            mp.replace(QLatin1Char('/'), QLatin1Char('\\'));
            return normalizePath(mp);
        }
    }

    QString anchor = datasetName;
    QString drive;
    while (!anchor.isEmpty()) {
        QString rawDrive;
        if (getDatasetProperty(connIdx, anchor, QStringLiteral("driveletter"), rawDrive)) {
            drive = normalizeDriveLetterValue(rawDrive);
        } else {
            drive.clear();
        }
        if (!drive.isEmpty()) {
            break;
        }
        if (anchor == poolName) {
            break;
        }
        const QString parent = parentDatasetName(anchor);
        if (parent.isEmpty()) {
            anchor.clear();
            break;
        }
        anchor = parent;
    }
    if (drive.isEmpty()) {
        return QString();
    }
    QString base = QStringLiteral("%1:\\").arg(drive);
    if (datasetName == anchor) {
        return normalizePath(base);
    }
    QString rel = datasetName.mid(anchor.size());
    if (rel.startsWith('/')) {
        rel.remove(0, 1);
    }
    rel.replace('/', '\\');
    return normalizePath(rel.isEmpty() ? base : (base + rel));
}

QString MainWindow::datasetCacheKey(int connIdx, const QString& poolName) const {
    return QStringLiteral("%1::%2").arg(connToken(connIdx), poolName);
}

// The guid of every imported pool of the connection, from the shell's pool listing.
void MainWindow::readPoolGuids(int connIdx) {
    QJsonObject pools;
    if (!cliReadJson(connIdx, QStringLiteral("ls %1").arg(zfsmUrl(connIdx)), pools, nullptr, 60000)
        || connIdx < 0 || connIdx >= m_conns.states.size()) {
        return;
    }
    for (const QJsonValue& v : pools.value(QStringLiteral("pools")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString guid = o.value(QStringLiteral("guid")).toString().trimmed();
        if (o.value(QStringLiteral("imported")).toBool() && !guid.isEmpty()) {
            m_conns.states[connIdx].poolGuidByName.insert(
                o.value(QStringLiteral("name")).toString(), guid);
        }
    }
}

bool MainWindow::ensureDatasetsLoaded(int connIdx, const QString& poolName, bool allowRemoteLoadIfMissing) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return false;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    {
        // Scoped so this reference cannot outlive the runSsh() calls below. operator[]
        // is kept so a missing entry is still created, matching the previous behavior.
        const PoolDatasetCache& existing = m_conns.poolDatasetCache[key];
        if (existing.loaded) {
            return true;
        }
        if (existing.loadFailed) {
            // Ya se intentó y falló desde el último refresco de la conexión. Repetirlo
            // no da un resultado distinto y sí vuelve a bloquear la interfaz durante
            // toda la ida y vuelta por SSH, con la reconstrucción del árbol detrás.
            return false;
        }
    }
    if (!allowRemoteLoadIfMissing) {
        return false;
    }
    // Copy, not a reference: loadConnections() reassigns m_conns.profiles wholesale and can
    // be dispatched from the event loop while a read pumps it.
    const ConnectionProfile p = m_conns.profiles[connIdx];
    if (connIdx < m_conns.states.size()
        && m_conns.states[connIdx].poolGuidByName.value(poolName.trimmed()).trimmed().isEmpty()) {
        readPoolGuids(connIdx);
    }
    // The datasets and snapshots of the pool, all at once: the shell's `ls -r`.
    // Built locally and published into m_conns.poolDatasetCache only once it is complete:
    // holding a reference into the map across a read is what corrupted the heap on refresh.
    PoolDatasetCache cache;
    struct SnapshotMetaRow {
        QString creation;
        QString snapName;
        QString guid;
    };
    QMap<QString, QVector<SnapshotMetaRow>> snapshotMetaByDataset;
    m_transport.log(TransportSession::Level::Info,
                    QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));
    QJsonObject listado;
    QString errorListado;
    const bool leido =
        cliReadJson(connIdx, QStringLiteral("ls -r %1").arg(zfsmUrl(connIdx, poolName.trimmed())),
                    listado, &errorListado, 120000);
    const QJsonArray entradas = listado.value(QStringLiteral("entries")).toArray();
    const auto texto = [](const QJsonObject& o, const char* k) {
        const QJsonValue v = o.value(QLatin1String(k));
        return v.isDouble() ? QString::number(static_cast<qint64>(v.toDouble())) : v.toString();
    };
    for (const QJsonValue& v : entradas) {
        const QJsonObject o = v.toObject();
        const QString name = texto(o, "name").trimmed();
        if (name.isEmpty()) {
            continue;
        }
        DatasetRecord rec{name,
                          texto(o, "guid"),
                          texto(o, "used"),
                          texto(o, "compressratio"),
                          texto(o, "encryption"),
                          texto(o, "creation"),
                          texto(o, "referenced"),
                          texto(o, "mounted"),
                          texto(o, "mountpoint"),
                          texto(o, "canmount")};
        if (!rec.guid.trimmed().isEmpty() && rec.guid.trimmed() != QStringLiteral("-")) {
            cache.objectGuidByName.insert(name, rec.guid.trimmed());
        }
        if (name.contains(QLatin1Char('@'))) {
            const QString ds = name.section('@', 0, 0);
            const QString snap = name.section('@', 1);
            if (!ds.isEmpty() && !snap.isEmpty()) {
                snapshotMetaByDataset[ds].push_back(SnapshotMetaRow{rec.creation, snap, rec.guid});
            }
        } else {
            cache.datasets.push_back(rec);
            cache.recordByName[name] = rec;
        }
    }
    // An empty answer is not good: every pool has at least its root dataset.
    if (!leido || entradas.isEmpty()) {
        m_transport.log(
            TransportSession::Level::Warn,
            QStringLiteral("Failed datasets %1::%2 -> %3").arg(p.name, poolName, errorListado));
        // Noted, so that every rebuild of the tree does not try again.
        m_conns.poolDatasetCache[key].loadFailed = true;
        return false;
    }

    for (auto it = snapshotMetaByDataset.begin(); it != snapshotMetaByDataset.end(); ++it) {
        auto rows = it.value();
        std::sort(rows.begin(), rows.end(), [](const SnapshotMetaRow& a, const SnapshotMetaRow& b) {
            bool aOk = false;
            bool bOk = false;
            const qlonglong av = a.creation.toLongLong(&aOk);
            const qlonglong bv = b.creation.toLongLong(&bOk);
            if (aOk && bOk && av != bv) {
                return av > bv;  // newest first
            }
            if (a.creation != b.creation) {
                return a.creation > b.creation;
            }
            return a.snapName > b.snapName;
        });
        QStringList sortedSnaps;
        sortedSnaps.reserve(rows.size());
        for (const auto& row : rows) {
            sortedSnaps.push_back(row.snapName);
        }
        cache.snapshotsByDataset.insert(it.key(), sortedSnaps);
    }
    cache.loaded = true;
    // Publish before rebuildConnInfoFor() so the entry is visible to it, exactly as
    // when this wrote through a reference into the map.
    m_conns.poolDatasetCache.insert(key, cache);
    rebuildConnInfoFor(connIdx);
    m_transport.log(TransportSession::Level::Debug, QStringLiteral("Datasets loaded %1::%2 (%3)")
                                     .arg(p.name)
                                     .arg(poolName)
                                     .arg(cache.datasets.size()));
    return true;
}

// Cuánto se tolera que una tubería siga viva sin transportar nada. Dos minutos: una
// copia real puede tardar en arrancar (rsync enumerando, tar abriendo el árbol), pero
// no dos minutos sin un solo byte.
constexpr qint64 kStallAbortMs = 120000;
// Se considera que `pv` sigue vivo si emitió algo hace menos de esto.
constexpr qint64 kProgressAliveMs = 15000;
// Y cuánto se tolera el silencio absoluto una vez que la copia ya había arrancado.
// Más largo que el otro umbral a propósito: entre un origen y el siguiente no se
// imprime nada, y ese hueco depende de lo que tarde `tar` en abrir el árbol remoto.
constexpr qint64 kSilenceAbortMs = 300000;
