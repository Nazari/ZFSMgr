#include "mainwindow.h"

#include "transport.h"

#include "base/json.h"
#include "base/transportcmd.h"
#include "base/transportrpc.h"
#include "base/transporttunnel.h"
#include "base/process.h"
#include "base/tlsclient.h"
#include "mainwindow_helpers.h"
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

QString remoteDaemonTlsCacheKey(const ConnectionProfile& p) {
    return QString::fromStdString(BT::remoteDaemonTlsCacheKey(toBaseProfile(p)));
}

LocalAgentConfig loadLocalAgentConfig() {
    const auto c = BT::loadLocalAgentConfig();
    LocalAgentConfig cfg;
    cfg.bindAddress = QString::fromStdString(c.bindAddress);
    cfg.port = c.port;
    cfg.tlsCertPath = QString::fromStdString(c.tlsCertPath);
    cfg.tlsClientCertPath = QString::fromStdString(c.tlsClientCertPath);
    cfg.tlsClientKeyPath = QString::fromStdString(c.tlsClientKeyPath);
    return cfg;
}


// Whether an agent invocation changes remote state. Re-sending a `--dump-*` costs
// nothing, but re-sending a mutation that may already be running is how a single
// destructive operation ends up executed twice.
bool isMutatingAgentCommand(const QStringList& agentArgs) {
    std::vector<std::string> a;
    a.reserve(static_cast<std::size_t>(agentArgs.size()));
    for (const QString& x : agentArgs) {
        a.push_back(x.toStdString());
    }
    return BT::isMutatingAgentCommand(a);
}

// Camino HEREDADO: recupera los argumentos parseando una cadena de shell.
//
// Existe solo para los sitios que todavía construyen la orden como cadena. Los que ya
// pasan por runAgentCommand no lo tocan, y cuando migren los últimos esta función y las
// dos interceptaciones de runSsh se borran de un tajo.
//
// No añadir sitios nuevos por aquí: el corte por separador, la lista blanca por prefijo
// y el deshacer del doble entrecomillado son suposiciones sobre cómo se construyó la
// cadena, y cada una ha fallado al menos una vez.
bool extractLocalAgentArgs(const QString& remoteCmd, QStringList& argsOut) {
    argsOut.clear();
    std::vector<std::string> a;
    if (!BT::extractLocalAgentArgs(remoteCmd.toStdString(), a)) {
        return false;
    }
    for (const std::string& x : a) {
        argsOut << QString::fromStdString(x);
    }
    return true;
}

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
// Categoría propia, APAGADA por omisión. Estas trazas salían por la salida de error
// estándar en cada llamada RPC local, así que arrancar la aplicación desde un terminal
// lo llenaba de ruido —y en un binario publicado no pinta nada—. Siguen disponibles
// para diagnosticar con:
//   QT_LOGGING_RULES="zfsmgr.rpc.debug=true" ./zfsmgr_qt
Q_LOGGING_CATEGORY(lcAgentRpc, "zfsmgr.rpc", QtWarningMsg)

bool tryRunLocalAgentRpc(const QStringList& agentArgs,
                         const QByteArray& serverCertPem,
                         const QByteArray& clientCertPem,
                         const QByteArray& clientKeyPem,
                         quint16 daemonPort,
                         int timeoutMs,
                         QString& out,
                         QString& err,
                         int& rc) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(agentArgs.size()));
    for (const QString& a : agentArgs) {
        args.push_back(a.toStdString());
    }
    std::string o;
    std::string e;
    BT::LocalRpcDiag diag;
    const bool ok = BT::runLocalAgentRpc(args, serverCertPem.toStdString(),
                                         clientCertPem.toStdString(), clientKeyPem.toStdString(),
                                         daemonPort, timeoutMs, o, e, rc, &diag);
    out = QString::fromStdString(o);
    err = QString::fromStdString(e);
    // El registro se queda en este lado: la capa base no sabe dónde está, así que devuelve
    // lo que costó y por qué falló, y aquí se escribe como siempre.
    const QString cmd = agentArgs.isEmpty() ? QString() : agentArgs.first();
    if (ok) {
        qCDebug(lcAgentRpc, "[agent-rpc] cmd=%s OK en %lld ms", qPrintable(cmd), diag.elapsedMs);
    } else {
        qCDebug(lcAgentRpc, "[agent-rpc] cmd=%s FALLÓ en %lld ms: %s", qPrintable(cmd),
                diag.elapsedMs, diag.failure.c_str());
    }
    return ok;
}

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

bool parseRemoteDaemonTlsBundle(const QString& text,
                                QByteArray& serverCertPem,
                                QByteArray& clientCertPem,
                                QByteArray& clientKeyPem,
                                quint16& portOut,
                                bool* clientKeyIncludedOut = nullptr) {
    BT::RemoteTlsBundle b;
    const bool ok = BT::parseRemoteDaemonTlsBundle(text.toStdString(), b);
    serverCertPem = QByteArray::fromStdString(b.serverCertPem);
    clientCertPem = QByteArray::fromStdString(b.clientCertPem);
    clientKeyPem = QByteArray::fromStdString(b.clientKeyPem);
    portOut = b.port;
    if (clientKeyIncludedOut) {
        *clientKeyIncludedOut = b.clientKeyIncluded;
    }
    return ok;
}

QString sanitizeWindowsCliXml(const QString& raw) {
    return QString::fromStdString(BT::sanitizeWindowsCliXml(raw.toStdString()));
}

bool shouldRetrySshWithoutMultiplexing(const QString& stderrText) {
    return BT::shouldRetrySshWithoutMultiplexing(stderrText.toStdString());
}

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

bool MainWindow::runAgentMutationAsJob(const ConnectionProfile& p,
                                       const QStringList& agentArgs,
                                       QString& out,
                                       QString& err,
                                       int& rc,
                                       const std::function<void(const QString&)>& progressCb,
                                       bool* jobSubmittedOut) {
    out.clear();
    err.clear();
    rc = -1;
    if (jobSubmittedOut) {
        *jobSubmittedOut = false;
    }

    if (agentArgs.isEmpty()) {
        return false;
    }
    // --job-submit delante, y el resto como carga del trabajo. Antes esto se hacía
    // buscando la ruta del binario dentro de una cadena y partiéndola en dos.
    const QStringList submitArgs = QStringList{QStringLiteral("--job-submit")} + agentArgs;

    QString subOut;
    QString subErr;
    int subRc = -1;
    if (!runAgentCommand(p, submitArgs, 20000, subOut, subErr, subRc) || subRc != 0) {
        err = subErr.trimmed().isEmpty() ? QStringLiteral("no se pudo enviar el trabajo al daemon")
                                         : subErr;
        return false;
    }
    QString jobId;
    for (const QString& line : subOut.split('\n', Qt::SkipEmptyParts)) {
        const QString t = line.trimmed();
        if (t.startsWith(QStringLiteral("JOB_ID="))) {
            jobId = t.mid(7).trimmed();
            break;
        }
    }
    if (jobId.isEmpty()) {
        err = QStringLiteral("el daemon no devolvió un identificador de trabajo");
        return false;
    }
    if (jobSubmittedOut) {
        *jobSubmittedOut = true;
    }
    m_transport.log(TransportSession::Nivel::Info,
           QStringLiteral("%1: trabajo %2 en curso en el daemon").arg(p.name, jobId));

    const QStringList statusArgs = {QStringLiteral("--job-status"), jobId};
    QString lastProgress;
    // No overall deadline on purpose: the daemon owns the operation and reports when
    // it is done. Each individual poll is short, so a dead daemon still surfaces.
    while (true) {
        QThread::msleep(1000);
        // CON eventos de entrada: sin ellos la ventana se repinta pero ignora los clics,
        // y entonces no hay forma de pedir la cancelación —el menú contextual que la
        // ofrece no llega ni a abrirse—. Es decir, la espera de un trabajo largo era
        // justo el momento en que no se podía detener el trabajo largo.
        //
        // Reentrar es aceptable aquí: m_actionsLocked está puesto y las acciones lo
        // comprueban al entrar, así que lo que el usuario puede hacer mientras tanto es
        // mirar, desplazarse y cancelar.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        // Cancelación. Este bucle no la miraba, así que Desglosar y Ensamblar —las dos
        // que se envían como trabajo— no se podían detener de ninguna manera desde la
        // interfaz: no hay proceso local que matar, el trabajo vive en el daemon.
        if (m_cancelActionRequested) {
            m_cancelActionRequested = false;
            QString cOut;
            QString cErr;
            int cRc = -1;
            const QStringList cancelArgs = {QStringLiteral("--job-cancel"), jobId};
            const bool asked = runAgentCommand(p, cancelArgs, 20000, cOut, cErr, cRc);
            m_transport.log(asked && cRc == 0 ? TransportSession::Nivel::Normal
                                              : TransportSession::Nivel::Error,
                   asked && cRc == 0
                       ? QStringLiteral("%1: cancelación pedida para el trabajo %2")
                             .arg(p.name, jobId)
                       : QStringLiteral("%1: no se pudo cancelar el trabajo %2 (%3). Puede "
                                        "seguir en curso en el daemon.")
                             .arg(p.name, jobId, mwhelpers::oneLine(cErr)));
            err = QStringLiteral("cancelado por el usuario");
            rc = 125;
            return false;
        }
        QString stOut;
        QString stErr;
        int stRc = -1;
        // Por el túnel ya abierto, en vez de un SSH completo con sudo cada segundo:
        // --job-status no pasaba la lista blanca del parseo y caía siempre a shell.
        if (!runAgentCommand(p, statusArgs, 20000, stOut, stErr, stRc) || stRc != 0) {
            err = QStringLiteral("se perdió el contacto con el daemon mientras el trabajo %1 seguía en curso")
                      .arg(jobId);
            return false;
        }
        QString state;
        QString jobErr;
        QString jobOut;
        int jobRc = 0;
        for (const QString& line : stOut.split('\n', Qt::SkipEmptyParts)) {
            const QString t = line.trimmed();
            if (t.startsWith(QStringLiteral("STATE="))) {
                state = t.mid(6).trimmed();
            } else if (t.startsWith(QStringLiteral("ERROR="))) {
                jobErr = t.mid(6).trimmed();
            } else if (t.startsWith(QStringLiteral("OUT="))) {
                jobOut = t.mid(4).trimmed();
            } else if (t.startsWith(QStringLiteral("RC="))) {
                jobRc = t.mid(3).trimmed().toInt();
            } else if (t.startsWith(QStringLiteral("PROGRESS_LINE="))) {
                const QString pl = t.mid(14).trimmed();
                if (!pl.isEmpty() && pl != lastProgress) {
                    lastProgress = pl;
                    if (progressCb) {
                        progressCb(pl);
                    }
                }
            }
        }
        if (state == QStringLiteral("done") || state == QStringLiteral("failed")
            || state == QStringLiteral("cancelled")) {
            out = jobOut;
            err = jobErr;
            rc = (state == QStringLiteral("done")) ? jobRc : (jobRc != 0 ? jobRc : 1);
            if (progressCb && !jobOut.isEmpty()) {
                progressCb(jobOut);
            }
            return true;
        }
        if (state.isEmpty()) {
            err = QStringLiteral("estado de trabajo ilegible para %1").arg(jobId);
            return false;
        }
    }
}

bool transport::tryRunRemoteAgentRpcViaTunnel(TransportSession& ses,
                                              const ConnectionProfile& p,
                                              const QStringList& agentArgs,
                                              int timeoutMs,
                                              QString& out,
                                              QString& err,
                                              int& rc,
                                              QString* failureReason,
                                              bool* commandMayHaveRunOut) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(agentArgs.size()));
    for (const QString& a : agentArgs) {
        args.push_back(a.toStdString());
    }
    std::string o;
    std::string e;
    std::string motivo;
    const bool ok = BT::tryRunRemoteAgentRpcViaTunnel(ses, toBaseProfile(p), args, timeoutMs, o, e,
                                                      rc, &motivo, commandMayHaveRunOut);
    out = QString::fromStdString(o);
    err = QString::fromStdString(e);
    if (failureReason) {
        *failureReason = QString::fromStdString(motivo);
    }
    return ok;
}

bool MainWindow::persistDaemonTlsMaterialForConnection(const ConnectionProfile& p,
                                                       const QByteArray& serverCertPem,
                                                       const QByteArray& clientCertPem,
                                                       const QByteArray& clientKeyPem,
                                                       quint16 daemonPort,
                                                       QString* errorOut) {
    if (errorOut) {
        errorOut->clear();
    }
    const QString connId = p.id.trimmed();
    if (connId.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("id de conexión vacío");
        }
        return false;
    }
    ConnectionProfile updated;
    bool found = false;
    for (const ConnectionProfile& cp : std::as_const(m_conns.profiles)) {
        if (cp.id.trimmed().compare(connId, Qt::CaseInsensitive) == 0) {
            updated = cp;
            found = true;
            break;
        }
    }
    if (!found) {
        updated = p;
    }
    updated.daemonTlsServerCertPem = QString::fromUtf8(serverCertPem);
    updated.daemonTlsClientCertPem = QString::fromUtf8(clientCertPem);
    updated.daemonTlsClientKeyPem = QString::fromUtf8(clientKeyPem);
    updated.daemonTlsPort = (daemonPort > 0) ? static_cast<int>(daemonPort) : 47653;

    // Actualizar el estado en memoria antes de intentar el persist a disco:
    // así la sesión actual usa siempre el material TLS recién obtenido del
    // daemon remoto, aunque el upsertConnection falle por cualquier motivo.
    for (int i = 0; i < m_conns.profiles.size(); ++i) {
        if (m_conns.profiles[i].id.trimmed().compare(connId, Qt::CaseInsensitive) == 0) {
            m_conns.profiles[i].daemonTlsServerCertPem = updated.daemonTlsServerCertPem;
            m_conns.profiles[i].daemonTlsClientCertPem = updated.daemonTlsClientCertPem;
            m_conns.profiles[i].daemonTlsClientKeyPem = updated.daemonTlsClientKeyPem;
            m_conns.profiles[i].daemonTlsPort = updated.daemonTlsPort;
            break;
        }
    }

    QString storeErr;
    {
        QMutexLocker lock(&s_remoteDaemonTlsPersistMutex);
        if (!m_conns.store.upsertConnection(updated, storeErr)) {
            if (errorOut) {
                *errorOut = storeErr;
            }
            return false;
        }
    }
    return true;
}

bool MainWindow::cacheDaemonTlsMaterialForConnection(const ConnectionProfile& p, QString* errorOut) {
    if (errorOut) {
        errorOut->clear();
    }
    BT::RemoteTlsMaterial mat;
    std::string fetchReason;
    if (!BT::fetchRemoteDaemonTlsMaterial(toBaseProfile(p), true, mat, &fetchReason)) {
        if (errorOut) {
            *errorOut = fetchReason.empty() ? QStringLiteral("no se pudo obtener bundle TLS")
                                            : QString::fromStdString(fetchReason);
        }
        return false;
    }
    QString persistErr;
    if (!persistDaemonTlsMaterialForConnection(
            p, QByteArray::fromStdString(mat.serverCertPem),
            QByteArray::fromStdString(mat.clientCertPem),
            QByteArray::fromStdString(mat.clientKeyPem), mat.daemonPort, &persistErr)) {
        if (errorOut) {
            *errorOut = persistErr.isEmpty() ? QStringLiteral("no se pudo persistir TLS en config")
                                             : persistErr;
        }
        return false;
    }
    return true;
}

bool MainWindow::cleanupRemoteDaemonClientPrivateKey(const ConnectionProfile& p, QString* errorOut) {
    if (errorOut) {
        errorOut->clear();
    }
    QString out;
    QString err;
    int rc = -1;
    const QString cmd = withSudo(
        p,
        QStringLiteral(
            "sh -lc %1")
            .arg(shSingleQuote(
                QStringLiteral("set -eu; "
                               "rm -f /etc/zfsmgr/tls/client.key; "
                               "if [ -r /etc/zfsmgr/agent.conf ]; then "
                               "  tmp='/tmp/zfsmgr-agent-conf.$$'; "
                               "  awk '!/^[[:space:]]*TLS_CLIENT_KEY[[:space:]]*=/' /etc/zfsmgr/agent.conf > \"$tmp\"; "
                               "  install -m 600 \"$tmp\" /etc/zfsmgr/agent.conf; "
                               "  rm -f \"$tmp\"; "
                               "fi"))));
    const bool ok = runSshRawNoLog(p, cmd, 12000, out, err, rc) && rc == 0;
    if (!ok && errorOut) {
        *errorOut = mwhelpers::oneLine(err).trimmed();
    }
    return ok;
}

// Intenta servir una invocación del agente por el túnel RPC y decide qué hacer si no
// puede. Devuelve true si la ha atendido (con éxito, o abortando a propósito para no
// duplicar una mutación); false significa "ejecútala por SSH".
//
// Vive aquí, y no dentro de runSsh, porque hay dos entradas: la histórica, que recupera
// los argumentos parseando una cadena de shell, y runAgentCommand, que ya los tiene. La
// política de reintento, backoff y la regla de no reintentar mutaciones tiene que ser
// exactamente la misma para las dos.
QStringList MainWindow::extractAgentArgsForTest(const QString& remoteCmd) {
    QStringList args;
    extractLocalAgentArgs(remoteCmd, args);
    return args;
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

void MainWindow::clearLocalDaemonTlsCache() {
    BT::clearLocalDaemonTlsCache();
}

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

bool MainWindow::isMutatingAgentCommandForTest(const QStringList& agentArgs) {
    return isMutatingAgentCommand(agentArgs);
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

void MainWindow::setAgentTransportForTest(AgentTransportForTest fn) {
    m_transport.transportForTest = std::move(fn);
    m_transport.callsForTest.clear();
}

QVector<MainWindow::AgentCallForTest> MainWindow::agentCallsForTest() const {
    QVector<AgentCallForTest> v;
    v.reserve(static_cast<int>(m_transport.callsForTest.size()));
    for (const auto& c : m_transport.callsForTest) {
        v.push_back(c);
    }
    return v;
}

void MainWindow::clearAgentCallsForTest() {
    m_transport.callsForTest.clear();
}

bool MainWindow::runAgentCommand(const ConnectionProfile& p,
                                 const QStringList& agentArgs,
                                 int timeoutMs,
                                 QString& out,
                                 QString& err,
                                 int& rc,
                                 const QByteArray& stdinPayload) {
    out.clear();
    err.clear();
    rc = -1;
    if (agentArgs.isEmpty()) {
        return false;
    }
    if (m_transport.transportForTest) {
        std::vector<std::string> argv;
        argv.reserve(static_cast<std::size_t>(agentArgs.size()));
        for (const QString& a : agentArgs) {
            argv.push_back(a.toStdString());
        }
        m_transport.callsForTest.push_back(
            TransportSession::AgentCallForTest{argv, std::string(),
                                              stdinPayload.toStdString()});
        std::string o;
        std::string e;
        const bool ok = m_transport.transportForTest(argv, o, e, rc);
        out = QString::fromStdString(o);
        err = QString::fromStdString(e);
        return ok;
    }
    const QString verb = agentArgs.first().trimmed();
    // stdin no vacío descarta el RPC: el canal no lo transporta. Antes esto no se
    // comprobaba y la passphrase de un dataset cifrado se perdía sin aviso.
    const bool rpcEligible =
        stdinPayload.isEmpty() && !mwhelpers::isCliOnlyAgentCommand(verb);
    if (rpcEligible) {
        if (isLocalConnection(p)) {
            QByteArray srvPem;
            QByteArray cliPem;
            QByteArray keyPem;
            quint16 localPort = 47653;
            if (ensureLocalDaemonTlsMaterial(srvPem, cliPem, keyPem, localPort)
                && tryRunLocalAgentRpc(agentArgs, srvPem, cliPem, keyPem, localPort,
                                       timeoutMs, out, err, rc)) {
                return true;
            }
        } else if (tryAgentRpcOverSsh(p, agentArgs, timeoutMs, out, err, rc)) {
            return true;
        }
    }
    const QString shellCmd = stdinPayload.isEmpty()
                                 ? mwhelpers::agentShellCommand(p, agentArgs)
                                 : mwhelpers::agentShellCommandStreamInput(p, agentArgs);
    return runSsh(p, shellCmd, timeoutMs, out, err, rc, {}, {}, {}, stdinPayload,
                  /*allowAgentRpc=*/false);
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

void MainWindow::clearDaemonRpcStateForConnection(const ConnectionProfile& p) {
    // Se cierra el túnel para que tras reiniciar el daemon se abra uno nuevo.
    BT::closeTunnelForConnection(m_transport, toBaseProfile(p));
    const std::string key = BT::remoteDaemonTlsCacheKey(toBaseProfile(p));
    {
        // Y se quita la espera, para que el primer RPC tras el reinicio no salga suprimido.
        std::lock_guard<std::mutex> lock(m_transport.mutex);
        m_transport.retryAfterByConnKey.erase(key);
        m_transport.retryReasonByConnKey.erase(key);
    }
    // La caché en memoria del material TLS, SOLO la de esta conexión: vaciar la de todas
    // obligaría a las demás máquinas a una ida y vuelta por SSH sin motivo.
    BT::clearRemoteDaemonTlsCacheForConnection(toBaseProfile(p));
}

void MainWindow::clearDaemonRpcBackoffForConnection(const ConnectionProfile& p) {
    const std::string key = BT::remoteDaemonTlsCacheKey(toBaseProfile(p));
    std::lock_guard<std::mutex> lock(m_transport.mutex);
    m_transport.retryAfterByConnKey.erase(key);
    m_transport.retryReasonByConnKey.erase(key);
}

void MainWindow::closeAllRemoteDaemonRpcTunnels() {
    BT::closeAllTunnels(m_transport);
}

QString MainWindow::daemonRpcBackoffTextForConnection(const ConnectionProfile& p) const {
    const std::string key = BT::remoteDaemonTlsCacheKey(toBaseProfile(p));
    std::lock_guard<std::mutex> lock(m_transport.mutex);
    const auto retryIt = m_transport.retryAfterByConnKey.find(key);
    if (retryIt == m_transport.retryAfterByConnKey.end()) {
        return QString();
    }
    const qint64 seconds = std::chrono::duration_cast<std::chrono::seconds>(
                               retryIt->second - std::chrono::steady_clock::now())
                               .count();
    if (seconds <= 0) {
        return QString();
    }
    const auto reasonIt = m_transport.retryReasonByConnKey.find(key);
    const QString reason =
        reasonIt == m_transport.retryReasonByConnKey.end()
            ? QString()
            : QString::fromStdString(reasonIt->second).trimmed();
    const bool tlsRelated =
        reason.contains(QStringLiteral("TLS"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("cert"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("certificate"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("clave"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("handshake"), Qt::CaseInsensitive);
    if (!tlsRelated) {
        return QString();
    }
    if (reason.isEmpty()) {
        return QStringLiteral("daemon-rpc TLS en backoff (%1s)").arg(seconds);
    }
    return QStringLiteral("daemon-rpc TLS en backoff (%1s): %2").arg(seconds).arg(reason);
}

QString MainWindow::daemonRpcBackoffTextForConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return QString();
    }
    return daemonRpcBackoffTextForConnection(m_conns.profiles[connIdx]);
}

QString MainWindow::withSudo(const ConnectionProfile& p, const QString& cmd) const {
    return mwhelpers::withSudoCommand(p, cmd);
}

QString MainWindow::withSudoStreamInput(const ConnectionProfile& p, const QString& cmd) const {
    return mwhelpers::withSudoStreamInputCommand(p, cmd);
}

bool MainWindow::runSsh(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs,
                        QString& out, QString& err, int& rc,
                        const std::function<void(const QString&)>& onStdoutLine,
                        const std::function<void(const QString&)>& onStderrLine,
                        const std::function<void(int)>& onIdleTimeoutRemaining,
                        const QByteArray& stdinPayload, bool allowAgentRpc, bool echoOutputToLog) {
    return transport::runSsh(m_transport, p, remoteCmd, timeoutMs, out, err, rc, onStdoutLine,
                             onStderrLine, onIdleTimeoutRemaining, stdinPayload, allowAgentRpc,
                             echoOutputToLog);
}

bool MainWindow::tryAgentRpcOverSsh(const ConnectionProfile& p, const QStringList& agentArgs,
                                    int timeoutMs, QString& out, QString& err, int& rc,
                                    const std::function<void(const QString&)>& onStdoutLine,
                                    const std::function<void(const QString&)>& onStderrLine,
                                    bool echoOutputToLog) {
    return transport::tryAgentRpcOverSsh(m_transport, p, agentArgs, timeoutMs, out, err, rc,
                                         onStdoutLine, onStderrLine, echoOutputToLog);
}

bool MainWindow::tryRunRemoteAgentRpcViaTunnel(const ConnectionProfile& p,
                                               const QStringList& agentArgs, int timeoutMs,
                                               QString& out, QString& err, int& rc,
                                               QString* failureReason,
                                               bool* commandMayHaveRunOut) {
    return transport::tryRunRemoteAgentRpcViaTunnel(m_transport, p, agentArgs, timeoutMs, out, err,
                                                    rc, failureReason, commandMayHaveRunOut);
}

bool MainWindow::ensureLocalDaemonTlsMaterial(QByteArray& serverCertPem, QByteArray& clientCertPem,
                                              QByteArray& clientKeyPem, quint16& daemonPort) {
    return transport::ensureLocalDaemonTlsMaterial(m_transport, serverCertPem, clientCertPem,
                                                   clientKeyPem, daemonPort);
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

QString MainWindow::wrapRemoteCommand(const ConnectionProfile& p, const QString& remoteCmd) const {
    return transport::wrapRemoteCommand(p, remoteCmd);
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

QString MainWindow::sshExecFromLocal(const ConnectionProfile& p,
                                     const QString& remoteCmd) const {
    if (isLocalConnection(p)) {
        return remoteCmd;
    }
    const QString sshBase = sshBaseCommand(p);
    const QString target = shSingleQuote(sshUserHost(p));
    const QString wrapped = wrapRemoteCommand(p, remoteCmd);
    return sshBase + QStringLiteral(" ") + target + QStringLiteral(" ") + shSingleQuote(wrapped);
}

bool MainWindow::getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut) {
    valueOut.clear();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || dataset.isEmpty() || prop.isEmpty()) {
        return false;
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    const bool daemonReadApiOk =
        connIdx >= 0
        && connIdx < m_conns.states.size()
        && m_conns.states[connIdx].daemonInstalled
        && m_conns.states[connIdx].daemonActive
        && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    if (!daemonReadApiOk
        && !requireDaemonForRead(connIdx, QStringLiteral("leer una propiedad de dataset"))) {
        return false;
    }
    QString out;
    QString err;
    int rc = -1;
    const bool ok =
        runAgentCommand(p, {QStringLiteral("--dump-zfs-get-prop"), prop, dataset}, 15000, out, err, rc)
        && rc == 0;
    if (!ok) {
        return false;
    }
    valueOut = out.trimmed();
    return true;
}

bool MainWindow::ensureObjectGuidLoaded(int connIdx,
                                        const QString& poolName,
                                        const QString& objectName,
                                        QString* guidOut) {
    if (guidOut) {
        guidOut->clear();
    }
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return false;
    }
    if (!ensureDatasetsLoaded(connIdx, trimmedPool, true)) {
        return false;
    }
    const QString key = datasetCacheKey(connIdx, trimmedPool);
    QString guid;
    {
        // Scoped so the iterator cannot survive the runSsh() below: runSsh pumps
        // the event loop, and a queued refresh can erase entries from
        // m_conns.poolDatasetCache, leaving the iterator dangling.
        auto cacheIt = m_conns.poolDatasetCache.find(key);
        if (cacheIt == m_conns.poolDatasetCache.end()) {
            return false;
        }
        guid = cacheIt->objectGuidByName.value(trimmedObject).trimmed();
    }
    if (guid.isEmpty() || guid == QStringLiteral("-")) {
        // Copy, not a reference: m_conns.profiles is reassigned wholesale by
        // loadConnections(), which a queued event can trigger during runSsh().
        const ConnectionProfile p = m_conns.profiles[connIdx];
        QString out;
        QString err;
        int rc = -1;
        const bool daemonReadApiOk =
            connIdx >= 0
            && connIdx < m_conns.states.size()
            && m_conns.states[connIdx].daemonInstalled
            && m_conns.states[connIdx].daemonActive
            && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
        if (!daemonReadApiOk
            && !requireDaemonForRead(connIdx, QStringLiteral("leer el GUID de un objeto"))) {
            return false;
        }
        const bool ok = runAgentCommand(
                            p, {QStringLiteral("--dump-zfs-get-prop"), QStringLiteral("guid"), trimmedObject},
                            15000, out, err, rc)
                        && rc == 0;
        if (!ok) {
            m_transport.log(TransportSession::Nivel::Warn,
                   QStringLiteral("No se pudo cargar GUID de objeto %1::%2/%3 -> %4")
                       .arg(p.name, trimmedPool, trimmedObject, oneLine(err.isEmpty() ? out : err)));
            return false;
        }
        guid = out.section('\n', 0, 0).trimmed();
        if (guid.isEmpty() || guid == QStringLiteral("-")) {
            return false;
        }
        // Re-look up after the yield: the entry may have been erased or replaced.
        auto refreshedIt = m_conns.poolDatasetCache.find(key);
        if (refreshedIt != m_conns.poolDatasetCache.end()) {
            refreshedIt->objectGuidByName.insert(trimmedObject, guid);
        }
        if (DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
            dsInfo->runtime.properties.insert(QStringLiteral("guid"), guid);
        }
    }
    if (guidOut) {
        *guidOut = guid;
    }
    return !guid.isEmpty() && guid != QStringLiteral("-");
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
    // se le tomaba por la raíz de la unidad y se le asignaba `Z:\`. Resultado visto en
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
    // be dispatched from the event loop while runSsh() pumps it.
    const ConnectionProfile p = m_conns.profiles[connIdx];
    const bool daemonReadApiOk =
        connIdx >= 0
        && connIdx < m_conns.states.size()
        && m_conns.states[connIdx].daemonInstalled
        && m_conns.states[connIdx].daemonActive
        && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    if (connIdx >= 0 && connIdx < m_conns.states.size()) {
        const QString trimmedPool = poolName.trimmed();
        if (!trimmedPool.isEmpty()
            && m_conns.states[connIdx].poolGuidByName.value(trimmedPool).trimmed().isEmpty()) {
            QString gOut;
            QString gErr;
            int gRc = -1;
            const bool guidOk =
                daemonReadApiOk
                && runAgentCommand(p, {QStringLiteral("--dump-zpool-guid"), trimmedPool},
                                   12000, gOut, gErr, gRc)
                && gRc == 0;
            if (guidOk) {
                const QString guid = gOut.section('\n', 0, 0).trimmed();
                if (!guid.isEmpty() && guid != QStringLiteral("-")) {
                    m_conns.states[connIdx].poolGuidByName.insert(trimmedPool, guid);
                    m_transport.log(TransportSession::Nivel::Debug,
                           QStringLiteral("Loaded missing pool GUID %1::%2 -> %3")
                               .arg(p.name, trimmedPool, guid));
                } else {
                    m_transport.log(TransportSession::Nivel::Warn,
                           QStringLiteral("Pool GUID missing after query %1::%2")
                               .arg(p.name, trimmedPool));
                }
            } else {
                m_transport.log(TransportSession::Nivel::Warn,
                       QStringLiteral("Could not query pool GUID %1::%2 -> %3")
                           .arg(p.name,
                                trimmedPool,
                                oneLine(gErr.isEmpty() ? QStringLiteral("exit %1").arg(gRc) : gErr)));
            }
        }
    }
    // Built locally and published into m_conns.poolDatasetCache only once it is complete.
    // Holding a reference into the map across the runSsh() calls below is what made
    // this function corrupt the heap and crash on refresh.
    PoolDatasetCache cache;
    const bool isWin = isWindowsConnection(p);
    QString out;
    QString err;
    int rc = -1;
    struct SnapshotMetaRow {
        QString creation;
        QString snapName;
        QString guid;
    };
    QMap<QString, QVector<SnapshotMetaRow>> snapshotMetaByDataset;
    bool loadedFromJson = false;
    m_transport.log(TransportSession::Nivel::Info, QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));

    if (!isWin) {
        if (!daemonReadApiOk) {
            requireDaemonForRead(connIdx, QStringLiteral("listar los datasets de un pool"));
        }
        if (daemonReadApiOk
            && runAgentCommand(p, {QStringLiteral("--dump-zfs-list-all"), poolName}, 35000, out, err, rc)
            && rc == 0) {
            QString jsonPayload = mwhelpers::stripToJson(out);
            QJsonParseError parseErr{};
            QJsonDocument doc;
            // Solo se intenta si la salida PARECE JSON.
            //
            // `--dump-zfs-list-all` responde `zfs list -H -p`, que es texto separado por
            // tabuladores y nunca ha sido JSON. Intentarlo igualmente hacía que CADA
            // refresco de CADA pool dejara un aviso «Invalid JSON ... (illegal value)»
            // que no señalaba ningún problema: el análisis por texto de más abajo
            // funciona. Eran avisos falsos escondiendo los de verdad.
            const bool pareceJson = jsonPayload.trimmed().startsWith(QLatin1Char('{'));
            if (pareceJson) {
                doc = QJsonDocument::fromJson(jsonPayload.toUtf8(), &parseErr);
                if (parseErr.error != QJsonParseError::NoError) {
                    const int lastBrace = jsonPayload.lastIndexOf(QLatin1Char('}'));
                    if (lastBrace > 0) {
                        jsonPayload = jsonPayload.left(lastBrace + 1);
                        parseErr = QJsonParseError{};
                        doc = QJsonDocument::fromJson(jsonPayload.toUtf8(), &parseErr);
                    }
                }
            }
            if (pareceJson && parseErr.error != QJsonParseError::NoError) {
                m_transport.log(TransportSession::Nivel::Warn,
                       QStringLiteral("Invalid JSON from zfsmgr-zfs-list-all %1::%2 (%3)")
                           .arg(p.name,
                                poolName,
                                parseErr.errorString()));
            }
            const QJsonObject datasets = doc.object().value(QStringLiteral("datasets")).toObject();
            if (!datasets.isEmpty()) {
                loadedFromJson = true;
                for (auto it = datasets.constBegin(); it != datasets.constEnd(); ++it) {
                    const QString name = it.key().trimmed();
                    if (name.isEmpty()) {
                        continue;
                    }
                    const QJsonObject props = it.value().toObject()
                                              .value(QStringLiteral("properties")).toObject();
                    auto propValue = [&props](const QString& prop) -> QString {
                        return props.value(prop).toObject().value(QStringLiteral("value")).toString();
                    };
                    DatasetRecord rec{
                        name,
                        propValue(QStringLiteral("guid")),
                        propValue(QStringLiteral("used")),
                        propValue(QStringLiteral("compressratio")),
                        propValue(QStringLiteral("encryption")),
                        propValue(QStringLiteral("creation")),
                        propValue(QStringLiteral("referenced")),
                        propValue(QStringLiteral("mounted")),
                        propValue(QStringLiteral("mountpoint")),
                        propValue(QStringLiteral("canmount")),
                    };
                    if (!rec.guid.trimmed().isEmpty()) {
                        cache.objectGuidByName.insert(name, rec.guid.trimmed());
                    }
                    const QString type = propValue(QStringLiteral("type")).trimmed().toLower();
                    if (type == QStringLiteral("snapshot") || name.contains(QLatin1Char('@'))) {
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
            }
        }
    }

    if (!loadedFromJson) {
        QString cmd = QStringLiteral(
            "zfs list -H -p -t filesystem,volume,snapshot "
            "-o name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r %1")
                          .arg(poolName);
        if (!isWin) {
            cmd = mwhelpers::withUnixSearchPathCommand(cmd);
        }
        cmd = withSudo(p, cmd);
        out.clear();
        err.clear();
        rc = -1;
        if (!runSsh(p, cmd, 35000, out, err, rc) || rc != 0) {
            m_transport.log(TransportSession::Nivel::Warn, QStringLiteral("Failed datasets %1::%2 -> %3")
                                            .arg(p.name, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
            // Anotar el fallo para no reintentarlo en cada reconstrucción del árbol.
            m_conns.poolDatasetCache[key].loadFailed = true;
            return false;
        }
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList f = line.split('\t');
            if (f.size() < 10) {
                continue;
            }
            const QString name = f[0].trimmed();
            if (name.isEmpty()) {
                continue;
            }
            DatasetRecord rec{name, f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]};
            if (!rec.guid.trimmed().isEmpty() && rec.guid.trimmed() != QStringLiteral("-")) {
                cache.objectGuidByName.insert(name, rec.guid.trimmed());
            }
            if (name.contains('@')) {
                const QString ds = name.section('@', 0, 0);
                const QString snap = name.section('@', 1);
                snapshotMetaByDataset[ds].push_back(SnapshotMetaRow{rec.creation, snap, rec.guid});
            } else {
                cache.datasets.push_back(rec);
                cache.recordByName[name] = rec;
            }
        }
    }

    for (auto it = snapshotMetaByDataset.begin(); it != snapshotMetaByDataset.end(); ++it) {
        auto rows = it.value();
        std::sort(rows.begin(), rows.end(), [](const SnapshotMetaRow& a, const SnapshotMetaRow& b) {
            bool aOk = false;
            bool bOk = false;
            const qlonglong av = a.creation.toLongLong(&aOk);
            const qlonglong bv = b.creation.toLongLong(&bOk);
            if (aOk && bOk && av != bv) {
                return av > bv; // más nuevo primero
            }
            if (a.creation != b.creation) {
                return a.creation > b.creation; // fallback textual desc
            }
            return a.snapName > b.snapName; // fallback por nombre desc
        });
        QStringList sortedSnaps;
        sortedSnaps.reserve(rows.size());
        for (const auto& row : rows) {
            sortedSnaps.push_back(row.snapName);
            const QString fullSnapshotName = QStringLiteral("%1@%2").arg(it.key(), row.snapName);
            if (!row.guid.trimmed().isEmpty() && row.guid.trimmed() != QStringLiteral("-")) {
                cache.objectGuidByName.insert(fullSnapshotName, row.guid.trimmed());
            }
        }
        cache.snapshotsByDataset.insert(it.key(), sortedSnaps);
    }
    cache.driveletterByDataset.clear();
    if (isWindowsConnection(connIdx)) {
        QString dOut;
        QString dErr;
        int dRc = -1;
        const QString dCmd = withSudo(
            p,
            // Con `source`: hace falta para distinguir la letra PUESTA en un dataset de la
            // HEREDADA del pool. Sin eso, cualquier pool con más de un dataset parecía
            // tener letras duplicadas.
            QStringLiteral("zfs get -H -o name,value,source -r driveletter %1").arg(shSingleQuote(poolName)));
        if (runSsh(p, dCmd, 20000, dOut, dErr, dRc) && dRc == 0) {
            QMap<QString, QStringList> byDrive;
            const QStringList dLines = dOut.split('\n', Qt::SkipEmptyParts);
            for (const QString& ln : dLines) {
                const QStringList f = ln.split('\t');
                if (f.size() < 2) {
                    continue;
                }
                const QString ds = f[0].trimmed();
                const QString drive = normalizeDriveLetterValue(f[1]);
                // La letra REAL de cada dataset se guarda siempre, heredada o no: es lo
                // que dice dónde está montado.
                cache.driveletterByDataset[ds] = drive;
                // Pero para el aviso de duplicados solo cuentan las PUESTAS en el propio
                // dataset. En Windows los descendientes heredan la del pool y se montan
                // planos bajo esa unidad, así que dos datasets con la misma letra
                // heredada es el funcionamiento normal, no un conflicto. Avisar de eso
                // llenaba el registro en cada refresco y tapaba los avisos de verdad.
                const QString origen = (f.size() >= 3) ? f[2].trimmed().toLower() : QString();
                const bool puestaAqui = origen.isEmpty() || origen == QStringLiteral("local");
                if (!drive.isEmpty() && puestaAqui) {
                    byDrive[drive].push_back(ds);
                }
            }
            for (auto it = byDrive.constBegin(); it != byDrive.constEnd(); ++it) {
                if (it.value().size() > 1) {
                    m_transport.log(TransportSession::Nivel::Warn,
                           QStringLiteral("%1::%2 driveletter duplicado %3 en datasets: %4")
                               .arg(p.name, poolName, it.key(), it.value().join(QStringLiteral(", "))));
                }
            }
        } else if (!dErr.trimmed().isEmpty()) {
            m_transport.log(TransportSession::Nivel::Info,
                   QStringLiteral("%1: no se pudieron cargar driveletters -> %2").arg(p.name, oneLine(dErr)));
        }
    }
    cache.loaded = true;
    // Publish before rebuildConnInfoFor() so the entry is visible to it, exactly as
    // when this wrote through a reference into the map.
    m_conns.poolDatasetCache.insert(key, cache);
    rebuildConnInfoFor(connIdx);
    m_transport.log(TransportSession::Nivel::Debug, QStringLiteral("Datasets loaded %1::%2 (%3)")
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

bool MainWindow::runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs, bool forceConfirmDialog, bool streamProgress) {
    if (!confirmActionExecution(displayLabel, {QStringLiteral("[local]\n%1").arg(command)}, forceConfirmDialog)) {
        return false;
    }
    setActionsLocked(true);
    m_transport.log(TransportSession::Nivel::Normal, QStringLiteral("%1").arg(displayLabel));
    updateStatus(QStringLiteral("%1").arg(displayLabel));
    m_transport.log(TransportSession::Nivel::Info, QStringLiteral("$ %1").arg(command));
    QProcess proc;
    m_cancelActionRequested = false;
    m_activeLocalProcess = &proc;
#ifdef Q_OS_WIN
    ConnectionProfile localWinProfile;
    localWinProfile.connType = QStringLiteral("LOCAL");
    localWinProfile.osType = QStringLiteral("Windows");
    proc.start(QStringLiteral("cmd.exe"), QStringList{QStringLiteral("/C"), wrapRemoteCommand(localWinProfile, command)});
#else
    // SIN `set -o pipefail`, y a conciencia.
    //
    // Lo puse para que un emisor que falla no diera resultado correcto, y rompió las
    // transferencias: cuando el `tar` receptor acaba de extraer el archivo sale, y el
    // `cat` que todavía envía recibe SIGPIPE (141). Eso es normal e inofensivo en una
    // tubería, pero con pipefail pasa a ser fatal, y como los tramos van encadenados con
    // `&&`, el primero que "falla" detiene los demás: un Desde Dir de tres orígenes
    // moría tras el primero.
    //
    // El agujero original —un emisor fallido que se contaba como éxito— sigue abierto y
    // hay que taparlo mirando el resultado de cada extremo, no con una opción del shell
    // que no sabe distinguir un SIGPIPE benigno de un fallo real.
    proc.start(QStringLiteral("sh"),
               QStringList{QStringLiteral("-c"), mwhelpers::asciiSafeShellCommand(command)});
#endif
    if (!proc.waitForStarted(4000)) {
        m_transport.log(TransportSession::Nivel::Normal,
               trk(QStringLiteral("t_no_se_pudo_874fae"),
                   QStringLiteral("No se pudo iniciar comando local"),
                   QStringLiteral("Could not start local command"),
                   QStringLiteral("无法启动本地命令")));
        updateStatus(QStringLiteral("%1 (ERROR: start)").arg(displayLabel));
        m_activeLocalProcess = nullptr;
        m_activeLocalPid = -1;
        setActionsLocked(false);
        return false;
    }
    m_activeLocalPid = static_cast<qint64>(proc.processId());
    QString outBuf;
    QString errBuf;
    QString outRemainder;
    QString errRemainder;
    QElapsedTimer progressTimer;
    progressTimer.start();
    QElapsedTimer stallTimer;
    stallTimer.start();
    QString lastStallSnippet;
    // Cuándo llegó la ÚLTIMA línea de avance, cambiara o no. Distingue dos situaciones
    // que no se parecen en nada: `pv` vivo repitiendo el mismo recuento —el receptor ha
    // muerto y la tubería no se entera— y silencio total, que puede ser un tramo
    // terminando y el siguiente arrancando, o un disco escribiendo despacio.
    QElapsedTimer lastProgressLine;
    lastProgressLine.start();
    bool sawAnyProgressLine = false;
    QElapsedTimer heartbeatTimer;
    heartbeatTimer.start();
    int lastProgressPercent = -1;
    QString lastProgressSnippet;
    bool sawProgressOutput = false;
    auto flushLines = [&](QString& remainder, const QString& chunk, const QString& level, bool progressAware) {
        if (chunk.isEmpty()) {
            return;
        }
        QString chunkData = remainder + chunk;
        chunkData.replace('\r', '\n');
        const QStringList parts = chunkData.split('\n');
        if (!chunkData.endsWith('\n')) {
            remainder = parts.isEmpty() ? chunkData : parts.last();
        } else {
            remainder.clear();
        }
        const int limit = chunkData.endsWith('\n') ? parts.size() : qMax(0, parts.size() - 1);
        for (int i = 0; i < limit; ++i) {
            const QString ln = parts[i].trimmed();
            if (ln.isEmpty()) {
                continue;
            }
            if (progressAware) {
                const QString low = ln.toLower();
                const bool looksLikeProgress = ln.contains('%')
                    || low.contains(QStringLiteral("ib/s"))
                    || low.contains(QStringLiteral("b/s"))
                    || low.contains(QStringLiteral("to-chk"))
                    || low.contains(QStringLiteral("xfr#"));
                if (looksLikeProgress) {
                    const QRegularExpression pctRx(QStringLiteral("(\\d{1,3})%"));
                    const QRegularExpressionMatch pctM = pctRx.match(ln);
                    if (pctM.hasMatch()) {
                        bool okPct = false;
                        const int pct = pctM.captured(1).toInt(&okPct);
                        if (okPct && pct >= 0 && pct <= 100) {
                            if (lastProgressPercent >= 0 && pct <= lastProgressPercent) {
                                continue;
                            }
                            if (lastProgressPercent >= 0 && (pct - lastProgressPercent) < 1) {
                                continue;
                            }
                            lastProgressPercent = pct;
                            sawProgressOutput = true;
                            m_transport.log(TransportSession::Nivel::Info, QStringLiteral("[progress] %1").arg(ln));
                            continue;
                        }
                    }
                    if (progressTimer.elapsed() < 700) {
                        continue;
                    }
                    progressTimer.restart();
                    lastProgressSnippet = ln;
                    sawProgressOutput = true;
                    m_transport.log(TransportSession::Nivel::Info, QStringLiteral("[progress] %1").arg(ln));
                    continue;
                }
            }
            // El nivel viene calculado más arriba a partir de la propia línea, así que
            // se traduce aquí en vez de en el destino.
            m_transport.log(level == QStringLiteral("ERROR")   ? TransportSession::Nivel::Error
                            : level == QStringLiteral("WARN")  ? TransportSession::Nivel::Warn
                            : level == QStringLiteral("INFO")  ? TransportSession::Nivel::Info
                            : level == QStringLiteral("DEBUG") ? TransportSession::Nivel::Debug
                                                               : TransportSession::Nivel::Normal,
                            oneLine(ln));
        }
        if (progressAware) {
            const QString partial = remainder.trimmed();
            if (!partial.isEmpty()) {
                const QString low = partial.toLower();
                const bool looksLikeProgress = partial.contains('%')
                    || low.contains(QStringLiteral("ib/s"))
                    || low.contains(QStringLiteral("b/s"))
                    || low.contains(QStringLiteral("to-chk"))
                    || low.contains(QStringLiteral("xfr#"));
                if (looksLikeProgress && progressTimer.elapsed() >= 900 && partial != lastProgressSnippet) {
                    progressTimer.restart();
                    lastProgressSnippet = partial;
                    sawProgressOutput = true;
                    m_transport.log(TransportSession::Nivel::Info, QStringLiteral("[progress] %1").arg(partial));
                }
                // Estancamiento: `pv` sigue imprimiendo aunque no pase un solo byte, así
                // que la línea cambia siempre —cambia el reloj— y el temporizador de
                // avance nunca salta. Quitando el reloj queda lo que de verdad importa;
                // si ESO no cambia, la tubería está viva pero no transporta nada. Es lo
                // que pasa cuando el extremo receptor falla: se queda así para siempre.
                if (looksLikeProgress) {
                    lastProgressLine.restart();
                    sawAnyProgressLine = true;
                    static const QRegularExpression clockRx(QStringLiteral("\\d+:\\d\\d:\\d\\d"));
                    QString withoutClock = partial;
                    withoutClock.remove(clockRx);
                    if (withoutClock != lastStallSnippet) {
                        lastStallSnippet = withoutClock;
                        stallTimer.restart();
                    }
                }
            }
        }
    };

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (m_cancelActionRequested) {
            m_transport.log(TransportSession::Nivel::Normal, trk(QStringLiteral("t_canceling_act001"),
                                                 QStringLiteral("Cancelando acción en curso..."),
                                                 QStringLiteral("Canceling running action..."),
                                                 QStringLiteral("正在取消执行中的操作...")));
            terminateProcessTree(m_activeLocalPid);
            proc.terminate();
            if (!proc.waitForFinished(800)) {
                proc.kill();
                proc.waitForFinished(800);
            }
            m_transport.log(TransportSession::Nivel::Normal, trk(QStringLiteral("t_acc_cancel_usr2"),
                                                 QStringLiteral("Acción cancelada por el usuario."),
                                                 QStringLiteral("Action canceled by user."),
                                                 QStringLiteral("操作已被用户取消。")));
            updateStatus(QStringLiteral("%1 (CANCELADO)").arg(displayLabel));
            m_activeLocalProcess = nullptr;
            m_activeLocalPid = -1;
            m_cancelActionRequested = false;
            setActionsLocked(false);
            return false;
        }
        // Sin datos durante dos minutos con la tubería viva: no va a arrancar sola. El
        // caso real era Desde Dir contra un dataset sin punto de montaje utilizable —el
        // receptor fallaba al instante y esto seguía "en curso" indefinidamente—, pero
        // vale para cualquier extremo que muera sin cerrar la tubería.
        // Dos formas de estancarse, y hay que cazar las dos:
        //
        //   - `pv` VIVO repitiendo el mismo recuento: el receptor murió y la tubería no
        //     se entera. Dos minutos bastan.
        //   - SILENCIO total: `pv` bloqueado escribiendo porque el destino dejó de
        //     responder. Pasó de verdad —un SSD USB que se cayó a mitad de copia, con
        //     resets de xhci en el registro del núcleo— y `pv` no volvió a imprimir.
        //     Aquí se espera más, porque el silencio también aparece entre tramo y
        //     tramo de una copia con varios orígenes, y abortar ahí sería un error.
        const bool stalledWithLiveProgress =
            sawAnyProgressLine && lastProgressLine.elapsed() < kProgressAliveMs
            && stallTimer.elapsed() > kStallAbortMs;
        const bool stalledInSilence =
            sawAnyProgressLine && lastProgressLine.elapsed() > kSilenceAbortMs;
        if (streamProgress && sawProgressOutput && (stalledWithLiveProgress || stalledInSilence)) {
            m_transport.log(TransportSession::Nivel::Error,
                   trk(QStringLiteral("t_pipe_stalled_001"),
                       QStringLiteral("%1: la transferencia lleva %2 s sin mover datos. Se "
                                      "aborta: lo habitual es que el extremo receptor haya "
                                      "fallado sin cerrar la tubería. Revise el registro."),
                       QStringLiteral("%1: the transfer has moved no data for %2 s. Aborting: "
                                      "the usual cause is the receiving end failing without "
                                      "closing the pipe. Check the log."),
                       QStringLiteral("%1：传输已有 %2 秒没有数据流动，现已中止。常见原因是接收端"
                                      "失败但未关闭管道。请查看日志。"))
                       .arg(displayLabel)
                       .arg((stalledInSilence ? lastProgressLine.elapsed()
                                              : stallTimer.elapsed()) / 1000));
            terminateProcessTree(m_activeLocalPid);
            proc.terminate();
            if (!proc.waitForFinished(800)) {
                proc.kill();
                proc.waitForFinished(800);
            }
            updateStatus(QStringLiteral("%1 (SIN AVANCE)").arg(displayLabel));
            m_activeLocalProcess = nullptr;
            m_activeLocalPid = -1;
            setActionsLocked(false);
            return false;
        }
        if (timeoutMs > 0 && (QDateTime::currentMSecsSinceEpoch() - startMs) > timeoutMs) {
            terminateProcessTree(m_activeLocalPid);
            proc.kill();
            proc.waitForFinished(1000);
            m_transport.log(TransportSession::Nivel::Normal, QStringLiteral("Timeout en comando local"));
            updateStatus(QStringLiteral("%1 (TIMEOUT)").arg(displayLabel));
            m_activeLocalProcess = nullptr;
            m_activeLocalPid = -1;
            setActionsLocked(false);
            return false;
        }
        proc.waitForReadyRead(200);
        const QString outChunk = QString::fromUtf8(proc.readAllStandardOutput());
        const QString errChunk = QString::fromUtf8(proc.readAllStandardError());
        outBuf += outChunk;
        errBuf += errChunk;
        if (streamProgress) {
            flushLines(outRemainder, outChunk, QStringLiteral("INFO"), true);
            flushLines(errRemainder, errChunk, QStringLiteral("INFO"), true);
            if (!lastProgressSnippet.isEmpty()) {
                updateStatus(displayLabel + QStringLiteral(" — ") + lastProgressSnippet);
            }
            if (!sawProgressOutput && heartbeatTimer.elapsed() >= 2000) {
                heartbeatTimer.restart();
                m_transport.log(TransportSession::Nivel::Info, QStringLiteral("[progress] running..."));
            }
        }
    }
    if (streamProgress) {
        flushLines(outRemainder, QStringLiteral("\n"), QStringLiteral("INFO"), true);
        flushLines(errRemainder, QStringLiteral("\n"), QStringLiteral("INFO"), true);
    }

    const int rc = proc.exitCode();
    const QString out = outBuf.trimmed();
    const QString err = errBuf.trimmed();
    if (!out.isEmpty() && !streamProgress) {
        m_transport.log(TransportSession::Nivel::Info, oneLine(out));
    }
    if (!err.isEmpty() && !streamProgress) {
        m_transport.log(TransportSession::Nivel::Info, oneLine(err));
    }
    if (rc != 0) {
        m_transport.log(TransportSession::Nivel::Normal, QStringLiteral("Comando finalizó con error %1").arg(rc));
        updateStatus(QStringLiteral("%1 (ERROR %2)").arg(displayLabel).arg(rc));
        const QString errorDetail = err.isEmpty() ? out : err;
        if (!errorDetail.isEmpty()) {
            const QStringList lines = errorDetail.split('\n', Qt::SkipEmptyParts);
            const int maxLines = 15;
            QStringList shown = lines.mid(qMax(0, lines.size() - maxLines));
            QString msg = QStringLiteral("<b>%1</b> — error %2:<br><br><pre>%3</pre>")
                              .arg(displayLabel.toHtmlEscaped(),
                                   QString::number(rc),
                                   shown.join('\n').toHtmlEscaped());
            QMessageBox errBox(QMessageBox::Critical, QStringLiteral("ZFSMgr — Error"), msg, QMessageBox::Ok, this);
            errBox.exec();
        }
        m_activeLocalProcess = nullptr;
        m_activeLocalPid = -1;
        setActionsLocked(false);
        return false;
    }
    m_transport.log(TransportSession::Nivel::Normal, QStringLiteral("Comando finalizado correctamente"));
    updateStatus(QStringLiteral("%1 finalizado").arg(displayLabel));
    m_activeLocalProcess = nullptr;
    m_activeLocalPid = -1;
    setActionsLocked(false);
    return true;
}

QString MainWindow::buildSshPreviewCommand(const ConnectionProfile& p, const QString& remoteCmd) const {
    if (isLocalConnection(p)) {
        return QStringLiteral("[local] %1").arg(remoteCmd);
    }
    // En Windows la orden NO se ejecuta tal como se ve aquí: runSsh la envuelve después
    // en PowerShell. Mostrar la forma sin envolver hacía que la confirmación enseñara un
    // comando que nunca llega a ejecutarse, y es justo el sitio donde el usuario decide
    // si sigue adelante con una operación destructiva.
    //
    // Se muestra la forma -Command, que es equivalente y legible; lo que viaja de verdad
    // es la misma orden en base64 (-EncodedCommand), ilegible en un diálogo.
    if (isWindowsConnection(p)) {
        const QString shown =
            QStringLiteral("powershell -NoProfile -NonInteractive -Command \"& { %1 }\"")
                .arg(remoteCmd.trimmed());
        return mwhelpers::buildSshPreviewCommandText(p, shown);
    }
    return mwhelpers::buildSshPreviewCommandText(p, remoteCmd);
}
