#include "mainwindow.h"

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
//   QT_LOGGING_RULES="zfsmgr.rpc.debug=true" ./zfsmgr-gui
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
        // Al registro va la ETIQUETA estable del fallo, no una frase: este log se lee con
        // grep y puede venir de una máquina configurada en otro idioma.
        const std::string porQue = std::string(BT::labelOf(diag.failure.failure))
                                   + (diag.failure.detail.empty() ? std::string()
                                                                   : ": " + diag.failure.detail);
        qCDebug(lcAgentRpc, "[agent-rpc] cmd=%s FALLÓ en %lld ms: %s", qPrintable(cmd),
                diag.elapsedMs, porQue.c_str());
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
    // El último recurso ejecuta **el agente** por SSH, no `zfs`. La diferencia importa y por
    // eso este camino se queda: SSH aquí es TRANSPORTE, no intérprete. Lo que se ha retirado
    // del resto de la aplicación son los respaldos que ejecutaban órdenes de ZFS en crudo
    // cuando no había agente; esto es otra cosa —hay agente y no se le pudo hablar por el
    // túnel—, y hace falta además para los verbos que leen la entrada estándar, que el canal
    // RPC no transporta.
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

void MainWindow::closeAllRemoteDaemonRpcTunnels() {
    BT::closeAllTunnels(m_transport);
}

QString MainWindow::transportNoticeText(const BT::WarningNote& a) const {
    using A = BT::Warning;
    const QString ruta = QString::fromStdString(a.path);
    const QString detalle = QString::fromStdString(a.detail);
    switch (a.warning) {
        case A::None_:
            return QString();
        case A::LocalTlsUnreadable:
            return trk(QStringLiteral("t_notice_tls_unreadable"),
                       QStringLiteral("No se pudo leer el material TLS del daemon local en %1. "
                                      "Reinstale el daemon desde el menú de la conexión."),
                       QStringLiteral("Could not read the local daemon's TLS material at %1. "
                                      "Reinstall the daemon from the connection menu."))
                .arg(ruta);
        case A::LocalTlsNeedsSudo:
            return trk(QStringLiteral("t_notice_tls_nosudo"),
                       QStringLiteral("No se pudo leer el material TLS del daemon local: faltan "
                                      "credenciales de sudo."),
                       QStringLiteral("Could not read the local daemon's TLS material: sudo "
                                      "credentials are missing."));
        case A::LocalTlsCannotBeRead:
            return trk(QStringLiteral("t_notice_tls_readfail"),
                       QStringLiteral("No se pudo leer el material TLS del daemon local: %1"),
                       QStringLiteral("Could not read the local daemon's TLS material: %1"))
                .arg(detalle);
        case A::LocalTlsIncomplete:
            return trk(QStringLiteral("t_notice_tls_partial"),
                       QStringLiteral("El material TLS del daemon local llegó incompleto."),
                       QStringLiteral("The local daemon's TLS material arrived incomplete."));
        case A::SshHostUnverified:
            return trk(QStringLiteral("t_notice_hostkey"),
                       QStringLiteral("Falló la verificación del host SSH."),
                       QStringLiteral("SSH host verification failed."));
        case A::NoSshpass:
            return trk(QStringLiteral("t_notice_nosshpass"),
                       QStringLiteral("Hay contraseña guardada, pero no está sshpass: se usará "
                                      "SSH no interactivo."),
                       QStringLiteral("A password is stored, but sshpass is missing: "
                                      "non-interactive SSH will be used."));
        case A::MultiplexingFailed:
            return trk(QStringLiteral("t_notice_muxfail"),
                       QStringLiteral("El SSH multiplexado falló; se reintenta sin ControlMaster."),
                       QStringLiteral("Multiplexed SSH failed; retrying without ControlMaster."));
        case A::MultiplexingDisabled:
            return trk(QStringLiteral("t_notice_muxoff"),
                       QStringLiteral("El SSH multiplexado queda desactivado para esta conexión "
                                      "en esta sesión."),
                       QStringLiteral("Multiplexed SSH is disabled for this connection in this "
                                      "session."));
        case A::TunnelNotAcceptingSshDied:
            return trk(QStringLiteral("t_notice_tunnel_died"),
                       QStringLiteral("El túnel SSH no aceptó conexiones: el ssh terminó (%1 ms)."),
                       QStringLiteral("The SSH tunnel accepted no connections: ssh exited (%1 ms)."))
                .arg(detalle);
        case A::TunnelNotAcceptingTimedOut:
            return trk(QStringLiteral("t_notice_tunnel_timeout"),
                       QStringLiteral("El túnel SSH no aceptó conexiones: se agotó la espera "
                                      "(%1 ms)."),
                       QStringLiteral("The SSH tunnel accepted no connections: the wait timed out "
                                      "(%1 ms)."))
                .arg(detalle);
    }
    return QString();
}

QString MainWindow::transportFailureText(const BT::FailureReason& m) const {
    using F = BT::Failure;
    const QString detalle = QString::fromStdString(m.detail).trimmed();
    // El detalle —el error de OpenSSL, lo que dijo la otra máquina— NO se traduce: viene
    // del sistema, ya en su idioma, y reescribirlo perdería justo lo que sirve para
    // diagnosticar.
    const auto con = [&detalle](const QString& texto) {
        return detalle.isEmpty() ? texto : QStringLiteral("%1: %2").arg(texto, detalle);
    };
    switch (m.failure) {
        case F::None_:
            return QString();
        case F::TunnelBusy:
            return trk(QStringLiteral("t_rpcfail_busy"),
                       QStringLiteral("el túnel se está montando para esta conexión"),
                       QStringLiteral("the tunnel is being set up for this connection"));
        case F::OffTheTunnelThread:
            return trk(QStringLiteral("t_rpcfail_thread"),
                       QStringLiteral("RPC pedido fuera del hilo de los túneles"),
                       QStringLiteral("RPC requested outside the tunnel thread"));
        case F::EmptyArguments:
            return trk(QStringLiteral("t_rpcfail_noargs"),
                       QStringLiteral("no se dijo qué ejecutar"),
                       QStringLiteral("nothing to run was given"));
        case F::ConnectionNotSsh:
            return trk(QStringLiteral("t_rpcfail_nossh"),
                       QStringLiteral("la conexión no es SSH"),
                       QStringLiteral("the connection is not SSH"));
        case F::Cooling:
            return trk(QStringLiteral("t_rpcfail_backoff"),
                       QStringLiteral("en espera tras un fallo reciente (%1 s)"),
                       QStringLiteral("waiting after a recent failure (%1 s)"))
                .arg(detalle);
        case F::MaterialCannotBeRead:
            return con(trk(QStringLiteral("t_rpcfail_tlsread"),
                           QStringLiteral("no se pudo leer el material TLS del daemon"),
                           QStringLiteral("could not read the daemon's TLS material")));
        case F::HostKeyChanged: {
            // Qué máquina, dónde está la clave vieja y cómo se resuelve. Ver el mismo caso en
            // src/cli/session.cpp.
            const QString donde = m.where.empty() ? QStringLiteral("~/.ssh/known_hosts")
                                                  : QString::fromStdString(m.where);
            return trk(QStringLiteral("t_rpcfail_hostkey"),
                       QStringLiteral("la clave SSH de %1 ha cambiado y ya no coincide con la "
                                      "guardada en %2. Si es lo esperado —se reinstaló la "
                                      "máquina, o esa dirección es ahora de otra—, bórrela con "
                                      "«ssh-keygen -R %1» y vuelva a intentarlo. Si no lo "
                                      "esperaba, no siga: alguien podría estar haciéndose pasar "
                                      "por ella"),
                       QStringLiteral("the SSH host key of %1 has changed and no longer matches "
                                      "the one stored in %2. If that is expected —the machine "
                                      "was reinstalled, or that address now belongs to another "
                                      "one—, remove it with «ssh-keygen -R %1» and try again. "
                                      "If you did not expect it, stop: someone could be "
                                      "impersonating it"))
                .arg(detalle, donde);
        }
        case F::MaterialIncomplete:
            return trk(QStringLiteral("t_rpcfail_tlspartial"),
                       QStringLiteral("el material TLS del daemon llegó incompleto"),
                       QStringLiteral("the daemon's TLS material arrived incomplete"));
        case F::ClientKeyUnavailable:
            return trk(QStringLiteral("t_rpcfail_nokey"),
                       QStringLiteral("no hay clave TLS de cliente, ni guardada ni en la otra máquina"),
                       QStringLiteral("no client TLS key, neither stored nor on the other machine"));
        case F::InvalidCertificates:
            return trk(QStringLiteral("t_rpcfail_badcerts"),
                       QStringLiteral("los certificados TLS del daemon no son válidos"),
                       QStringLiteral("the daemon's TLS certificates are not valid"));
        case F::InvalidClientKey:
            return trk(QStringLiteral("t_rpcfail_badkey"),
                       QStringLiteral("la clave TLS de cliente no es válida"),
                       QStringLiteral("the client TLS key is not valid"));
        case F::TunnelCannotBeBuilt:
            return trk(QStringLiteral("t_rpcfail_notunnel"),
                       QStringLiteral("no se pudo montar el túnel SSH hasta el daemon"),
                       QStringLiteral("could not set up the SSH tunnel to the daemon"));
        case F::ConnectionRefused:
            return con(trk(QStringLiteral("t_rpcfail_refused"),
                           QStringLiteral("el daemon no aceptó la conexión"),
                           QStringLiteral("the daemon did not accept the connection")));
        case F::CertificateMismatch:
            return trk(QStringLiteral("t_rpcfail_pinning"),
                       QStringLiteral("el certificado que presenta el daemon no es el fijado"),
                       QStringLiteral("the certificate the daemon presents is not the pinned one"));
        case F::SendFailed:
            return trk(QStringLiteral("t_rpcfail_send"),
                       QStringLiteral("no se pudo enviar la petición"),
                       QStringLiteral("the request could not be sent"));
        case F::TunnelCutWhileWaiting:
            return trk(QStringLiteral("t_rpcfail_cut"),
                       QStringLiteral("el túnel se cortó mientras se esperaba respuesta"),
                       QStringLiteral("the tunnel dropped while waiting for an answer"));
        case F::HandshakeFailed:
            return con(trk(QStringLiteral("t_rpcfail_handshake"),
                           QStringLiteral("falló el saludo TLS con el daemon"),
                           QStringLiteral("the TLS handshake with the daemon failed")));
        case F::InvalidAnswer:
            return con(trk(QStringLiteral("t_rpcfail_badresp"),
                           QStringLiteral("el daemon no devolvió una respuesta válida"),
                           QStringLiteral("the daemon did not return a valid answer")));
        case F::Unspecified:
            return trk(QStringLiteral("t_rpcfail_unknown"),
                       QStringLiteral("falló sin decir por qué"),
                       QStringLiteral("failed without saying why"));
    }
    return QString();
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
    const BT::FailureReason motivo =
        reasonIt == m_transport.retryReasonByConnKey.end() ? BT::FailureReason{} : reasonIt->second;
    // Antes esto se decidía buscando «TLS», «cert», «clave» y «handshake» DENTRO de la
    // frase del motivo. Además de romperse al traducirla, colaba por error cualquier
    // mensaje que mencionara una clave: el detalle de un fallo de red que dijera «clave»
    // se anunciaba al usuario como problema de TLS.
    if (!BT::looksLikeTls(motivo.failure)) {
        return QString();
    }
    const QString reason = transportFailureText(motivo).trimmed();
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
    BT::FailureReason motivo;
    const bool ok = transport::tryRunRemoteAgentRpcViaTunnel(
        m_transport, p, agentArgs, timeoutMs, out, err, rc, &motivo, commandMayHaveRunOut);
    if (failureReason) {
        *failureReason = transportFailureText(motivo);
    }
    return ok;
}

// Una orden al agente de ESTA conexión, sea del tipo que sea.
//
// `tryRunRemoteAgentRpcViaTunnel` rechaza de plano todo lo que no sea SSH —«Local» es otro
// tipo de conexión— y devuelve false sin intentar nada ni dejar rastro. Llamarla directamente
// con un perfil que puede ser Local es lo que dejaba el panel de Transferencias SIN progreso:
// el sondeo se saltaba el trabajo en silencio, y por el mismo motivo no funcionaban ni
// «Cancelar seleccionado» ni la recuperación de trabajos huérfanos cuando el origen era la
// máquina de uno. El reparto del trabajo ya se hacía bien al LANZARLO; faltaba en los tres
// sitios que lo consultan después.
bool MainWindow::runAgentOnConnection(const ConnectionProfile& p, const QStringList& agentArgs,
                                      int timeoutMs, QString& out, QString& err, int& rc) {
    return isLocalConnection(p) ? runAgentCommand(p, agentArgs, timeoutMs, out, err, rc)
                                : tryRunRemoteAgentRpcViaTunnel(p, agentArgs, timeoutMs, out, err, rc);
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
        runAgentCommand(p, mwhelpers::argvQt(zfsmgr::commands::requests::datasetProperty(prop.toStdString(), dataset.toStdString())), 15000, out, err, rc)
        && rc == 0;
    if (!ok) {
        return false;
    }
    valueOut = out.trimmed();
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
                && runAgentCommand(p, mwhelpers::argvQt(zfsmgr::commands::requests::poolGuid(trimmedPool.toStdString())),
                                   12000, gOut, gErr, gRc)
                && gRc == 0;
            if (guidOk) {
                const QString guid = gOut.section('\n', 0, 0).trimmed();
                if (!guid.isEmpty() && guid != QStringLiteral("-")) {
                    m_conns.states[connIdx].poolGuidByName.insert(trimmedPool, guid);
                    m_transport.log(TransportSession::Level::Debug,
                           QStringLiteral("Loaded missing pool GUID %1::%2 -> %3")
                               .arg(p.name, trimmedPool, guid));
                } else {
                    m_transport.log(TransportSession::Level::Warn,
                           QStringLiteral("Pool GUID missing after query %1::%2")
                               .arg(p.name, trimmedPool));
                }
            } else {
                m_transport.log(TransportSession::Level::Warn,
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
    QString out;
    QString err;
    int rc = -1;
    struct SnapshotMetaRow {
        QString creation;
        QString snapName;
        QString guid;
    };
    QMap<QString, QVector<SnapshotMetaRow>> snapshotMetaByDataset;
    bool loadedFromDaemon = false;
    m_transport.log(TransportSession::Level::Info, QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));

    // **Windows también pasa por aquí.** Antes esto estaba envuelto en `if (!isWin)`, así que
    // en Windows ni se intentaba el daemon: se iba directo al respaldo por SSH. Su agente
    // nativo sirve `--dump-zfs-list-all` igual que los demás —comprobado contra una máquina
    // real—, y el formato es el mismo TSV.

    if (!daemonReadApiOk) {
        requireDaemonForRead(connIdx, QStringLiteral("listar los datasets de un pool"));
    }
    if (daemonReadApiOk
        && runAgentCommand(p, mwhelpers::argvQt(zfsmgr::commands::requests::datasetList(poolName.toStdString())), 35000, out, err, rc)
        && rc == 0) {
        // Aquí se comprobaba si la respuesta era JSON, y si no, se TIRABA.
        //
        // `--dump-zfs-list-all` es `zfs list -H -p`: TSV de diez columnas, y nunca ha
        // sido JSON. Así que la comprobación no se cumplía jamás, esta llamada al daemon
        // se descartaba entera y el listado se volvía a pedir por SSH con sudo unas
        // líneas más abajo. Dos viajes por pool y por refresco, y el árbol de datasets
        // era el único camino de lectura que no pasaba por el daemon.
        //
        // El reparto de columnas lo hace `commands::listings`: una línea con menos de diez
        // columnas se salta en vez de
        // rellenar con vacíos, porque un punto de montaje con un tabulador dentro
        // correría los campos y enseñaría un montaje donde va el nombre.
        const std::vector<zfsmgr::base::listings::Entry> entradas =
            zfsmgr::base::listings::entries(out.toStdString());
        for (const zfsmgr::base::listings::Entry& e : entradas) {
            const QString name = QString::fromStdString(e.name).trimmed();
            if (name.isEmpty()) {
                continue;
            }
            DatasetRecord rec{name,
                              QString::fromStdString(e.guid),
                              QString::fromStdString(e.used),
                              QString::fromStdString(e.compression),
                              QString::fromStdString(e.encryption),
                              QString::fromStdString(e.creation),
                              QString::fromStdString(e.referenced),
                              QString::fromStdString(e.mounted),
                              QString::fromStdString(e.mountpoint),
                              QString::fromStdString(e.canmount)};
            if (!rec.guid.trimmed().isEmpty() && rec.guid.trimmed() != QStringLiteral("-")) {
                cache.objectGuidByName.insert(name, rec.guid.trimmed());
            }
            if (e.isSnapshot()) {
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
        // Una respuesta vacía NO se da por buena: todo pool tiene al menos su dataset
        // raíz, así que cero entradas significa que algo se torció y conviene el
        // respaldo, no un árbol vacío.
        loadedFromDaemon = !entradas.empty();
    }
    // **Sin daemon no se lista.** Aquí había un respaldo que ejecutaba `zfs list` por SSH con
    // sudo. Se ha quitado a propósito: ningún cliente opera sobre una máquina sin agente.
    //
    // No es una pérdida de función, es dejar de fingir una: mientras existió, el respaldo
    // ATENDÍA EL 100 % de los listings —la rama del daemon comprobaba si la respuesta era
    // JSON y ese verbo devuelve TSV—, así que la aplicación llevaba meses leyendo el árbol
    // por shell sin que nadie lo supiera.
    if (!loadedFromDaemon) {
        m_transport.log(TransportSession::Level::Warn,
                        QStringLiteral("Failed datasets %1::%2 -> sin agente utilizable")
                            .arg(p.name, poolName));
        // Anotar el fallo para no reintentarlo en cada reconstrucción del árbol.
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
        // Por verbo tipado. Aquí se armaba `zfs get -H -o name,value,source -r driveletter`
        // y se ejecutaba con sudo por SSH: era el último camino de shell que le quedaba al
        // refresco. El `source` sigue siendo imprescindible —distingue la letra PUESTA en un
        // dataset de la HEREDADA del pool— y ahora eso está escrito donde vive el verbo.
        if (runAgentCommand(p,
                            mwhelpers::argvQt(
                                zfsmgr::commands::requests::driveLetters(poolName.toStdString())),
                            20000, dOut, dErr, dRc)
            && dRc == 0) {
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
                    m_transport.log(TransportSession::Level::Warn,
                           QStringLiteral("%1::%2 driveletter duplicado %3 en datasets: %4")
                               .arg(p.name, poolName, it.key(), it.value().join(QStringLiteral(", "))));
                }
            }
        } else if (!dErr.trimmed().isEmpty()) {
            m_transport.log(TransportSession::Level::Info,
                   QStringLiteral("%1: no se pudieron cargar driveletters -> %2").arg(p.name, oneLine(dErr)));
        }
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
