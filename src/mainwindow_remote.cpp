#include "mainwindow.h"

#include "transport.h"
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
#include <QHostAddress>
#include <QHostInfo>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>

#include <mutex>
#include <QTcpServer>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMutexLocker>

#include <algorithm>
#include <cstring>

namespace {
// Rutas del agente EN ESTA MÁQUINA. Estaban fijas a las POSIX, así que en Windows la
// conexión Local nunca encontraba su material TLS, no podía hablar por RPC con su
// propio daemon y todo el refresco caía al camino de shell: cada orden arranca un
// PowerShell nuevo y el refresco medía 102 s, de los que 99 eran sondas.
// Tienen que coincidir con kDefaultTlsDir/kDefaultAgentConfigPath de daemon_main.cpp.
#ifdef Q_OS_WIN
constexpr const char* kDefaultAgentConfigPath = "C:\\ProgramData\\ZFSMgr\\agent\\agent.conf";
constexpr const char* kDefaultAgentTlsCertPath = "C:\\ProgramData\\ZFSMgr\\agent\\tls\\server.crt";
constexpr const char* kDefaultAgentTlsClientCertPath = "C:\\ProgramData\\ZFSMgr\\agent\\tls\\client.crt";
constexpr const char* kDefaultAgentTlsClientKeyPath = "C:\\ProgramData\\ZFSMgr\\agent\\tls\\client.key";
#else
constexpr const char* kDefaultAgentConfigPath = "/etc/zfsmgr/agent.conf";
constexpr const char* kDefaultAgentTlsCertPath = "/etc/zfsmgr/tls/server.crt";
constexpr const char* kDefaultAgentTlsClientCertPath = "/etc/zfsmgr/tls/client.crt";
constexpr const char* kDefaultAgentTlsClientKeyPath = "/etc/zfsmgr/tls/client.key";
#endif

struct LocalAgentConfig {
    QString bindAddress{QStringLiteral("127.0.0.1")};
    quint16 port{47653};
    QString tlsCertPath{QString::fromLatin1(kDefaultAgentTlsCertPath)};
    QString tlsClientCertPath{QString::fromLatin1(kDefaultAgentTlsClientCertPath)};
    QString tlsClientKeyPath{QString::fromLatin1(kDefaultAgentTlsClientKeyPath)};
};

struct RemoteDaemonTlsCacheEntry {
    QByteArray serverCertPem;
    QByteArray clientCertPem;
    QByteArray clientKeyPem;
    quint16 port{47653};
    QDateTime fetchedAtUtc;
};

QMutex s_remoteDaemonTlsCacheMutex;
QHash<QString, RemoteDaemonTlsCacheEntry> s_remoteDaemonTlsCache;
QMutex s_remoteDaemonTlsPersistMutex;

QString remoteDaemonTlsCacheKey(const ConnectionProfile& p) {
    return QStringLiteral("%1|%2|%3|%4")
        .arg(p.username.trimmed().toLower(),
             p.host.trimmed().toLower(),
             QString::number((p.port > 0) ? p.port : 22),
             p.keyPath.trimmed());
}

QString stripConfigQuotes(QString v) {
    v = v.trimmed();
    if (v.size() >= 2) {
        const QChar first = v.front();
        const QChar last = v.back();
        if ((first == QLatin1Char('\'') && last == QLatin1Char('\''))
            || (first == QLatin1Char('"') && last == QLatin1Char('"'))) {
            return v.mid(1, v.size() - 2);
        }
    }
    return v;
}

int parseConfigInt(QString v, int fallback) {
    bool ok = false;
    const int parsed = stripConfigQuotes(v).trimmed().toInt(&ok);
    return ok ? parsed : fallback;
}

LocalAgentConfig loadLocalAgentConfig() {
    LocalAgentConfig cfg;
    QFile f(QString::fromLatin1(kDefaultAgentConfigPath));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return cfg;
    }
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        const QString key = line.left(eq).trimmed().toUpper();
        const QString value = line.mid(eq + 1).trimmed();
        if (key == QStringLiteral("AGENT_BIND") || key == QStringLiteral("BIND")) {
            cfg.bindAddress = stripConfigQuotes(value);
        } else if (key == QStringLiteral("AGENT_PORT") || key == QStringLiteral("PORT")) {
            const int parsedPort = parseConfigInt(value, cfg.port);
            if (parsedPort > 0 && parsedPort <= 65535) {
                cfg.port = static_cast<quint16>(parsedPort);
            }
        } else if (key == QStringLiteral("TLS_CERT")) {
            cfg.tlsCertPath = stripConfigQuotes(value);
        } else if (key == QStringLiteral("TLS_CLIENT_CERT")) {
            cfg.tlsClientCertPath = stripConfigQuotes(value);
        } else if (key == QStringLiteral("TLS_CLIENT_KEY")) {
            cfg.tlsClientKeyPath = stripConfigQuotes(value);
        }
    }
    return cfg;
}


// Whether an agent invocation changes remote state. Re-sending a `--dump-*` costs
// nothing, but re-sending a mutation that may already be running is how a single
// destructive operation ends up executed twice.
bool isMutatingAgentCommand(const QStringList& agentArgs) {
    if (agentArgs.isEmpty()) {
        return false;
    }
    const QString cmd = agentArgs.first().trimmed();
    return cmd.startsWith(QStringLiteral("--mutate-"))
           || cmd.startsWith(QStringLiteral("--zfs-pipe-"))
           || cmd.startsWith(QStringLiteral("--zfs-send-"))
           || cmd.startsWith(QStringLiteral("--zfs-recv-"))
           || cmd == QStringLiteral("--repair-alt-mountpoints")
           // --job-submit también: reenviarlo lanza la MISMA transferencia por segunda
           // vez, con dos jobs corriendo a la vez sobre los mismos datos. Estaba
           // --job-cancel pero no éste, que es el que causa daño al duplicarse.
           || cmd == QStringLiteral("--job-submit")
           || cmd == QStringLiteral("--job-cancel");
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
    // El resultado de esta función se usa para DESVIAR la orden al RPC. Hay verbos que
    // el daemon no sirve por ahí a propósito —transportan flujos por la entrada o la
    // salida estándar, y --mutate-shell-generic además ejecuta shell arbitrario como
    // root—, así que desviarlos es garantizar un "unknown command".
    //
    // runAgentCommand ya lo comprobaba, pero este camino heredado no, y sus TRES
    // interceptaciones tampoco. Resultado: borrar un dataset fallaba con
    // «unknown command: --mutate-shell-generic». Se filtra aquí, que es el único sitio
    // por el que pasan las tres.
    // Se prueban las dos rutas del agente, no solo la de Unix. Buscar únicamente la
    // Unix es lo que dejaba a Windows fuera del RPC: agentCommand() emite la ruta de
    // Windows, no casaba con el marcador, y el comando acababa ejecutándose por SSH en
    // crudo, sin el mTLS del túnel, sin que nada lo advirtiera.
    int pos = -1;
    int markerLen = 0;
    for (const QString& marker : {daemonpayload::unixBinPath(), daemonpayload::windowsBinPath()}) {
        const int found = remoteCmd.lastIndexOf(marker);
        if (found > pos) {
            pos = found;
            markerLen = marker.size();
        }
    }
    if (pos < 0) {
        return false;
    }
    QString tail = remoteCmd.mid(pos + markerLen).trimmed();
    // La forma de Windows es: & "C:\...\zfsmgr-agent.exe" --health
    // Tras el marcador queda la comilla de cierre, que no es parte de los argumentos.
    if (tail.startsWith(QLatin1Char('"'))) {
        tail = tail.mid(1).trimmed();
    }
    if (tail.isEmpty()) {
        return false;
    }
    // Cortamos en el primer separador de shell NO entrecomillado.
    //
    // El comentario decía "no entrecomillado" pero la regex no lo comprobaba: cortaba
    // en el primer ';', '&' o '|' apareciera donde apareciera, incluso dentro de un
    // argumento correctamente protegido. Un directorio llamado "Copias & Backups"
    // truncaba la orden, y con --mutate-advanced-todir ese directorio lo elige el
    // usuario: se perdían el destino y el indicador de borrar el origen.
    {
        bool inSQ = false;
        bool inDQ = false;
        int sep = -1;
        for (int i = 0; i < tail.size(); ++i) {
            const QChar c = tail.at(i);
            if (inSQ) {
                if (c == QLatin1Char('\'')) { inSQ = false; }
                continue;
            }
            if (inDQ) {
                if (c == QLatin1Char('\\') && i + 1 < tail.size()) { ++i; continue; }
                if (c == QLatin1Char('"')) { inDQ = false; }
                continue;
            }
            if (c == QLatin1Char('\'')) { inSQ = true; continue; }
            if (c == QLatin1Char('"')) { inDQ = true; continue; }
            if (c == QLatin1Char(';') || c == QLatin1Char('&') || c == QLatin1Char('|')
                || c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
                sep = i;
                break;
            }
        }
        if (sep >= 0) {
            tail = tail.left(sep).trimmed();
        }
    }
    // Los args vienen del comando construido con shSingleQuote() luego envuelto en otro
    // shSingleQuote() para el argumento de `sh -c '...'`.  El patrón '"'"' representa un
    // carácter ' escapado en ese doble-envoltorio.  Reemplazarlo antes de parsear evita
    // que el parser interprete los " como delimitadores adicionales de cita.
    tail.replace(QStringLiteral("'\"'\"'"), QStringLiteral("'"));
    if (tail.isEmpty()) {
        return false;
    }
    const QStringList parsed = mwhelpers::posixShellSplitArgs(tail);
    if (parsed.isEmpty()) {
        return false;
    }
    const QString cmd = parsed.first().trimmed();
    if (!(cmd == QStringLiteral("--health")
          || cmd == QStringLiteral("--heartbeat")
          || cmd.startsWith(QStringLiteral("--dump-"))
          || cmd.startsWith(QStringLiteral("--mutate-")))) {
        return false;
    }
    if (mwhelpers::isCliOnlyAgentCommand(cmd)) {
        return false;  // se queda en el camino clásico; el RPC no lo sirve
    }
    argsOut = parsed;
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
bool peerCertificateIsPinned(const QSslSocket& sock, const QList<QSslCertificate>& expected) {
    const QSslCertificate peer = sock.peerCertificate();
    if (peer.isNull()) {
        return false;
    }
    for (const QSslCertificate& c : expected) {
        if (!c.isNull() && c == peer) {
            return true;
        }
    }
    return false;
}

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
    out.clear();
    err.clear();
    rc = -1;
    if (agentArgs.isEmpty()) {
        return false;
    }
    const QString cmd = agentArgs.first();
    const QStringList params = agentArgs.mid(1);
    const QList<QSslCertificate> caCerts = QSslCertificate::fromData(serverCertPem, QSsl::Pem);
    const QList<QSslCertificate> clientCerts = QSslCertificate::fromData(clientCertPem, QSsl::Pem);
    QSslKey clientKey(clientKeyPem, QSsl::Rsa, QSsl::Pem);
    if (clientKey.isNull()) {
        clientKey = QSslKey(clientKeyPem, QSsl::Ec, QSsl::Pem);
    }
    if (caCerts.isEmpty() || clientCerts.isEmpty() || clientKey.isNull()) {
        return false;
    }
    LocalAgentConfig cfg = loadLocalAgentConfig();
    if (daemonPort > 0) {
        cfg.port = daemonPort;
    }

    const QHostAddress bindAddr(cfg.bindAddress);
    const bool bindIsAny = (bindAddr == QHostAddress::Any
                            || bindAddr == QHostAddress::AnyIPv6
                            || bindAddr == QHostAddress::AnyIPv4);
    const QString host = (bindAddr.isNull() || bindIsAny) ? QStringLiteral("127.0.0.1")
                                                           : bindAddr.toString();
    const QStringList peerNames = {QStringLiteral("zfsmgr-agent-server"), QStringLiteral("zfsmgr-agent")};
    // Localhost TLS: either connects in <10ms or ECONNREFUSED immediately.
    // A 2500ms cap was wasting seconds per call when daemon is not running.
    const int connectTimeout = qBound(200, timeoutMs > 0 ? timeoutMs / 20 : 400, 700);
    // Igual que en el RPC remoto: 0 es "sin límite". Ver la nota de allí.
    const bool noIoDeadline = (timeoutMs <= 0);
    const qint64 ioTimeout = noIoDeadline ? 0 : qMax(800, timeoutMs);
    QElapsedTimer rpcTimer;
    rpcTimer.start();
    qCDebug(lcAgentRpc, "[agent-rpc] cmd=%s port=%d connectTimeout=%d", qPrintable(cmd), cfg.port, connectTimeout);
    for (const QString& peerName : peerNames) {
        const qint64 t0 = rpcTimer.elapsed();
        QSslSocket sock;
        sock.setProtocol(QSsl::TlsV1_2OrLater);
        QSslConfiguration conf = sock.sslConfiguration();
        conf.setCaCertificates(caCerts);
        conf.setLocalCertificate(clientCerts.first());
        conf.setPrivateKey(clientKey);
        conf.setProtocol(QSsl::TlsV1_2OrLater);
        // VerifyNone + fijación: la validación la hace peerCertificateIsPinned, no
        // la política PKI del backend, que difiere entre OpenSSL y Apple.
        conf.setPeerVerifyMode(QSslSocket::VerifyNone);
        sock.setSslConfiguration(conf);

        sock.connectToHostEncrypted(host, cfg.port, peerName);
        if (!sock.waitForEncrypted(connectTimeout)) {
            qCDebug(lcAgentRpc, "[agent-rpc] peerName=%s FAILED in %lld ms (err: %s)",
                   qPrintable(peerName), rpcTimer.elapsed() - t0,
                   qPrintable(sock.errorString()));
            continue;
        }
        if (!peerCertificateIsPinned(sock, caCerts)) {
            qCDebug(lcAgentRpc, "[agent-rpc] peerName=%s certificado del daemon NO coincide con el fijado",
                   qPrintable(peerName));
            sock.abort();
            continue;
        }

        QJsonObject req;
        req.insert(QStringLiteral("cmd"), cmd);
        QJsonArray args;
        for (const QString& p : params) {
            args.push_back(p);
        }
        req.insert(QStringLiteral("args"), args);
        const QByteArray payload = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
        if (sock.write(payload) < 0 || !sock.waitForBytesWritten(connectTimeout)) {
            continue;
        }

        QByteArray line;
        QElapsedTimer timer;
        timer.start();
        while (noIoDeadline || timer.elapsed() < ioTimeout) {
            if (!sock.waitForReadyRead(300)) {
                // Escape imprescindible ahora que 0 significa "sin límite": aquí no hay
                // proceso de túnel cuya muerte delate al daemon, así que el equivalente
                // es el estado del socket. Si el daemon cae, la sesión se corta y sin
                // esto el bucle quedaría girando para siempre.
                if (sock.state() != QAbstractSocket::ConnectedState) {
                    break;
                }
                continue;
            }
            line.append(sock.readAll());
            const int nl = line.indexOf('\n');
            if (nl < 0) {
                continue;
            }
            const QByteArray one = line.left(nl).trimmed();
            const QJsonObject resp = QJsonDocument::fromJson(one).object();
            rc = resp.value(QStringLiteral("rc")).toInt(1);
            out = resp.value(QStringLiteral("stdout")).toString();
            err = resp.value(QStringLiteral("stderr")).toString();
            qCDebug(lcAgentRpc, "[agent-rpc] cmd=%s OK via peerName=%s total=%lld ms",
                   qPrintable(cmd), qPrintable(peerName), rpcTimer.elapsed());
            return true;
        }
    }
    qCDebug(lcAgentRpc, "[agent-rpc] cmd=%s FAILED total=%lld ms", qPrintable(cmd), rpcTimer.elapsed());
    return false;
}

} // end anonymous namespace — runSshRawNoLog must be externally linkable for background watcher threads.

bool runSshRawNoLog(const ConnectionProfile& p,
                    const QString& remoteCmd,
                    int timeoutMs,
                    QString& out,
                    QString& err,
                    int& rc) {
    out.clear();
    err.clear();
    rc = -1;

    QString program = QStringLiteral("ssh");
    QStringList sshpassPrefixArgs;
    const bool hasPassword = !p.password.trimmed().isEmpty();
    if (hasPassword) {
        const QString sshpassExe = mwhelpers::findLocalExecutable(QStringLiteral("sshpass"));
        if (!sshpassExe.isEmpty()) {
            program = sshpassExe;
            sshpassPrefixArgs << "-p" << p.password << "ssh";
        }
    }

    QStringList args = sshpassPrefixArgs;
    const QString familyOpt = mwhelpers::sshAddressFamilyOption(p);
    if (!familyOpt.isEmpty()) {
        args << familyOpt;
    }
    args << "-o" << "BatchMode=yes";
    args << "-o" << "ConnectTimeout=10";
    args << "-o" << "LogLevel=ERROR";
    args << "-o" << "StrictHostKeyChecking=accept-new";
    if (hasPassword && !sshpassPrefixArgs.isEmpty()) {
        args << "-o" << "BatchMode=no";
        args << "-o" << "PreferredAuthentications=password,keyboard-interactive,publickey";
        args << "-o" << "NumberOfPasswordPrompts=1";
    }
    if (p.port > 0) {
        args << "-p" << QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        args << "-i" << p.keyPath;
    }
    // Ver asciiSafeShellCommand: en macOS los argumentos de un proceso se
    // descomponen, y la orden remota es un argumento. Inocuo si ya es ASCII.
    args << mwhelpers::sshUserHost(p) << mwhelpers::asciiSafeShellCommand(remoteCmd);

    QProcess proc;
    struct ProcessGuard {
        QProcess* proc;
        ~ProcessGuard() {
            if (proc && proc->state() != QProcess::NotRunning) {
                proc->kill();
                proc->waitForFinished(2000);
            }
        }
    } processGuard{&proc};  // ídem: que no quede un ssh suelto por ninguna salida
    proc.start(program, args);
    if (!proc.waitForStarted(4000)) {
        err = QStringLiteral("cannot start ssh");
        return false;
    }
    if (!proc.waitForFinished(timeoutMs > 0 ? timeoutMs : 15000)) {
        proc.kill();
        proc.waitForFinished(1000);
        err = QStringLiteral("timeout");
        rc = -1;
        return false;
    }
    out = QString::fromUtf8(proc.readAllStandardOutput());
    err = QString::fromUtf8(proc.readAllStandardError());
    rc = proc.exitCode();
    return true;
}

namespace {

bool parseRemoteDaemonTlsBundle(const QString& text,
                                QByteArray& serverCertPem,
                                QByteArray& clientCertPem,
                                QByteArray& clientKeyPem,
                                quint16& portOut,
                                bool* clientKeyIncludedOut = nullptr) {
    serverCertPem.clear();
    clientCertPem.clear();
    clientKeyPem.clear();
    portOut = 47653;
    if (clientKeyIncludedOut) {
        *clientKeyIncludedOut = false;
    }

    QString currentPath;
    QByteArray currentContent;
    const QStringList lines = text.split('\n', Qt::KeepEmptyParts);
    for (const QString& rawLine : lines) {
        const QString line = rawLine;
        if (line.startsWith(QStringLiteral("__ZFSMGR_TLS_BEGIN__:"))) {
            currentPath = line.mid(QStringLiteral("__ZFSMGR_TLS_BEGIN__:").size()).trimmed();
            currentContent.clear();
            continue;
        }
        if (line.startsWith(QStringLiteral("__ZFSMGR_TLS_END__:"))) {
            const QString endPath = line.mid(QStringLiteral("__ZFSMGR_TLS_END__:").size()).trimmed();
            if (!currentPath.isEmpty() && endPath == currentPath) {
                const QByteArray content = currentContent.trimmed() + QByteArray("\n");
                if (currentPath.endsWith(QStringLiteral("/server.crt"))) {
                    serverCertPem = content;
                } else if (currentPath.endsWith(QStringLiteral("/client.crt"))) {
                    clientCertPem = content;
                } else if (currentPath.endsWith(QStringLiteral("/client.key"))) {
                    clientKeyPem = content;
                    if (clientKeyIncludedOut) {
                        *clientKeyIncludedOut = true;
                    }
                }
            }
            currentPath.clear();
            currentContent.clear();
            continue;
        }
        if (line.startsWith(QStringLiteral("__ZFSMGR_AGENT_PORT__:"))) {
            bool ok = false;
            const int parsed = line.mid(QStringLiteral("__ZFSMGR_AGENT_PORT__:").size()).trimmed().toInt(&ok);
            if (ok && parsed > 0 && parsed <= 65535) {
                portOut = static_cast<quint16>(parsed);
            }
            continue;
        }
        if (!currentPath.isEmpty()) {
            currentContent += rawLine.toUtf8();
            currentContent += '\n';
        }
    }
    return !serverCertPem.isEmpty() && !clientCertPem.isEmpty();
}

bool fetchRemoteDaemonTlsMaterial(const ConnectionProfile& p,
                                  QByteArray& serverCertPem,
                                  QByteArray& clientCertPem,
                                  QByteArray& clientKeyPem,
                                  quint16& daemonPort,
                                  bool forceRefresh = false,
                                  bool* fetchedFromRemoteOut = nullptr,
                                  bool* clientKeyFetchedFromRemoteOut = nullptr,
                                  QString* failureReason = nullptr) {
    if (failureReason) {
        failureReason->clear();
    }
    if (fetchedFromRemoteOut) {
        *fetchedFromRemoteOut = false;
    }
    if (clientKeyFetchedFromRemoteOut) {
        *clientKeyFetchedFromRemoteOut = false;
    }
    const QString key = remoteDaemonTlsCacheKey(p);
    if (!forceRefresh) {
        QMutexLocker lock(&s_remoteDaemonTlsCacheMutex);
        const auto it = s_remoteDaemonTlsCache.constFind(key);
        if (it != s_remoteDaemonTlsCache.constEnd()
            && it->fetchedAtUtc.isValid()
            && it->fetchedAtUtc.msecsTo(QDateTime::currentDateTimeUtc()) <= 5 * 60 * 1000) {
            serverCertPem = it->serverCertPem;
            clientCertPem = it->clientCertPem;
            clientKeyPem = it->clientKeyPem;
            daemonPort = it->port;
            return true;
        }
    }
    if (!forceRefresh
        && p.daemonTlsServerCertPem.contains(QStringLiteral("BEGIN CERTIFICATE"))
        && p.daemonTlsClientCertPem.contains(QStringLiteral("BEGIN CERTIFICATE"))
        && p.daemonTlsClientKeyPem.contains(QStringLiteral("BEGIN"))) {
        serverCertPem = p.daemonTlsServerCertPem.toUtf8();
        clientCertPem = p.daemonTlsClientCertPem.toUtf8();
        clientKeyPem = p.daemonTlsClientKeyPem.toUtf8();
        daemonPort = (p.daemonTlsPort > 0 && p.daemonTlsPort <= 65535)
                         ? static_cast<quint16>(p.daemonTlsPort)
                         : static_cast<quint16>(47653);
        RemoteDaemonTlsCacheEntry entry;
        entry.serverCertPem = serverCertPem;
        entry.clientCertPem = clientCertPem;
        entry.clientKeyPem = clientKeyPem;
        entry.port = daemonPort;
        entry.fetchedAtUtc = QDateTime::currentDateTimeUtc();
        QMutexLocker lock(&s_remoteDaemonTlsCacheMutex);
        s_remoteDaemonTlsCache.insert(key, entry);
        return true;
    }

    const QString bundleScript =
        QStringLiteral("set -eu; "
                       "for f in /etc/zfsmgr/tls/server.crt /etc/zfsmgr/tls/client.crt /etc/zfsmgr/tls/client.key; do "
                       "  if [ -r \"$f\" ]; then "
                       "    printf '__ZFSMGR_TLS_BEGIN__:%s\\n' \"$f\"; "
                       "    cat \"$f\"; "
                       "    printf '__ZFSMGR_TLS_END__:%s\\n' \"$f\"; "
                       "  fi; "
                       "done; "
                       "if [ -r /etc/zfsmgr/agent.conf ]; then "
                       "  port=$(awk -F= '/^[[:space:]]*AGENT_PORT[[:space:]]*=/{print $2}' /etc/zfsmgr/agent.conf | tail -n1 | tr -d \"' \\t\\r\"); "
                       "  if [ -n \"$port\" ]; then printf '__ZFSMGR_AGENT_PORT__:%s\\n' \"$port\"; fi; "
                       "fi");
    // Windows guarda el material TLS bajo C:\ProgramData, no bajo /etc, y su shell por
    // omisión es cmd: el bucle de arriba no vale. Se emite el equivalente en PowerShell
    // codificado en base64, que es un único token y no atraviesa ninguna capa de
    // comillas. Las rutas van con barra normal a propósito: Windows las acepta, y así
    // los marcadores terminan en "/server.crt" y el parseador de arriba sirve sin tocar.
    QString cmdPlain;
    if (mwhelpers::isWindowsOsType(p.osType)) {
        const QString winTlsDir = QStringLiteral("C:/ProgramData/ZFSMgr/agent/tls");
        const QString winScript =
            QStringLiteral(
                "$ErrorActionPreference='SilentlyContinue'; "
                "foreach($f in @('%1/server.crt','%1/client.crt','%1/client.key')){ "
                "  if(Test-Path -LiteralPath $f){ "
                "    Write-Output ('__ZFSMGR_TLS_BEGIN__:' + $f); "
                "    Get-Content -LiteralPath $f -Raw; "
                "    Write-Output ('__ZFSMGR_TLS_END__:' + $f); "
                "  } "
                "}; "
                "$conf='C:/ProgramData/ZFSMgr/agent/agent.conf'; "
                "if(Test-Path -LiteralPath $conf){ "
                "  $m=Select-String -LiteralPath $conf -Pattern '^\\s*AGENT_PORT\\s*=\\s*(\\d+)' "
                "     | Select-Object -Last 1; "
                "  if($m){ Write-Output ('__ZFSMGR_AGENT_PORT__:' + $m.Matches[0].Groups[1].Value) } "
                "}")
                .arg(winTlsDir);
        const QByteArray utf16(reinterpret_cast<const char*>(winScript.utf16()), winScript.size() * 2);
        cmdPlain = QStringLiteral("powershell -NoProfile -NonInteractive -EncodedCommand %1")
                       .arg(QString::fromLatin1(utf16.toBase64()));
    } else {
        cmdPlain = QStringLiteral("sh -lc %1").arg(mwhelpers::shSingleQuote(bundleScript));
    }
    // withSudoCommand ya devuelve el comando intacto en Windows, así que este reintento
    // es inocuo allí: simplemente repite el mismo PowerShell.
    const QString cmdSudo = mwhelpers::withSudoCommand(p, cmdPlain);

    auto readBundle = [&](const QString& cmd,
                          int timeoutMs,
                          QString& outText,
                          QString& errText) -> bool {
        int runRc = -1;
        outText.clear();
        errText.clear();
        return runSshRawNoLog(p, cmd, timeoutMs, outText, errText, runRc) && runRc == 0;
    };

    QString out;
    QString err;
    bool ok = readBundle(cmdPlain, 12000, out, err);
    if (!ok) {
        ok = readBundle(cmdSudo, 15000, out, err);
    } else if (!out.contains(QStringLiteral("__ZFSMGR_TLS_BEGIN__:"))) {
        // En instalaciones con TLS 600 root:root, la lectura sin sudo puede
        // devolver rc=0 pero sin material. Reintentar con sudo.
        QString sudoOut;
        QString sudoErr;
        if (readBundle(cmdSudo, 15000, sudoOut, sudoErr)
            && sudoOut.contains(QStringLiteral("__ZFSMGR_TLS_BEGIN__:"))) {
            out = sudoOut;
            err = sudoErr;
        }
    }
    if (!ok) {
        if (failureReason) {
            const QString detail = mwhelpers::oneLine(err).trimmed();
            *failureReason = detail.isEmpty()
                                 ? QStringLiteral("no se pudo leer material TLS del daemon remoto")
                                 : QStringLiteral("lectura TLS remota fallida: %1").arg(detail);
        }
        return false;
    }

    quint16 parsedPort = 47653;
    bool remoteIncludedClientKey = false;
    if (!parseRemoteDaemonTlsBundle(out, serverCertPem, clientCertPem, clientKeyPem, parsedPort, &remoteIncludedClientKey)) {
        if (failureReason) {
            *failureReason =
                QStringLiteral("bundle TLS inválido o incompleto en respuesta remota");
        }
        return false;
    }
    if (clientKeyPem.isEmpty() && !p.daemonTlsClientKeyPem.trimmed().isEmpty()) {
        clientKeyPem = p.daemonTlsClientKeyPem.toUtf8();
    }
    if (clientKeyPem.isEmpty()) {
        if (failureReason) {
            *failureReason = QStringLiteral("clave TLS cliente no disponible (local ni remota)");
        }
        return false;
    }
    daemonPort = parsedPort;
    if (fetchedFromRemoteOut) {
        *fetchedFromRemoteOut = true;
    }
    if (clientKeyFetchedFromRemoteOut) {
        *clientKeyFetchedFromRemoteOut = remoteIncludedClientKey;
    }

    RemoteDaemonTlsCacheEntry entry;
    entry.serverCertPem = serverCertPem;
    entry.clientCertPem = clientCertPem;
    entry.clientKeyPem = clientKeyPem;
    entry.port = daemonPort;
    entry.fetchedAtUtc = QDateTime::currentDateTimeUtc();
    {
        QMutexLocker lock(&s_remoteDaemonTlsCacheMutex);
        s_remoteDaemonTlsCache.insert(key, entry);
    }
    return true;
}

bool tryReviveRemoteDaemonService(const ConnectionProfile& p) {
    const QString reviveScript = QStringLiteral(
        "set +e; "
        "if command -v systemctl >/dev/null 2>&1; then "
        "  systemctl daemon-reload >/dev/null 2>&1 || true; "
        "  systemctl enable zfsmgr-agent.service >/dev/null 2>&1 || true; "
        "  systemctl restart zfsmgr-agent.service >/dev/null 2>&1 || "
        "    systemctl start zfsmgr-agent.service >/dev/null 2>&1 || true; "
        "fi; "
        "if command -v launchctl >/dev/null 2>&1; then "
        "  launchctl bootstrap system /Library/LaunchDaemons/org.zfsmgr.agent.plist >/dev/null 2>&1 || true; "
        "  launchctl enable system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
        "  launchctl kickstart system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
        "fi; "
        "if command -v service >/dev/null 2>&1; then "
        "  service zfsmgr_agent onestart >/dev/null 2>&1 || "
        "    service zfsmgr_agent start >/dev/null 2>&1 || "
        "    service zfsmgr-agent start >/dev/null 2>&1 || true; "
        "fi; "
        "if [ -x /etc/rc.d/zfsmgr_agent ]; then /etc/rc.d/zfsmgr_agent onestart >/dev/null 2>&1 || true; fi; "
        "if [ -x /usr/local/etc/rc.d/zfsmgr_agent ]; then /usr/local/etc/rc.d/zfsmgr_agent onestart >/dev/null 2>&1 || true; fi; "
        "if [ -x /etc/init.d/zfsmgr-agent ]; then /etc/init.d/zfsmgr-agent restart >/dev/null 2>&1 || /etc/init.d/zfsmgr-agent start >/dev/null 2>&1 || true; fi; "
        "exit 0");
    QString out;
    QString err;
    int rc = -1;
    const QString cmd = mwhelpers::withSudoCommand(
        p, QStringLiteral("sh -lc %1").arg(mwhelpers::shSingleQuote(reviveScript)));
    return runSshRawNoLog(p, cmd, 15000, out, err, rc);
}

QString sanitizeWindowsCliXml(const QString& raw) {
    QString s = raw;
    if (s.isEmpty()) {
        return s;
    }
    s.replace(QStringLiteral("#< CLIXML"), QStringLiteral(""));
    const int xmlPos = s.indexOf(QStringLiteral("<Objs Version="), 0, Qt::CaseInsensitive);
    if (xmlPos >= 0) {
        s = s.left(xmlPos);
    }
    return s.trimmed();
}

bool shouldRetrySshWithoutMultiplexing(const QString& stderrText) {
    const QString lowered = stderrText.toLower();
    return lowered.contains(QStringLiteral("getsockname failed"))
        || lowered.contains(QStringLiteral("not a socket"))
        || lowered.contains(QStringLiteral("bad stdio forwarding specification"));
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
QString describeHostAddress(const QHostAddress& address) {
    const QString protocol =
        (address.protocol() == QAbstractSocket::IPv6Protocol) ? QStringLiteral("IPv6")
                                                              : QStringLiteral("IPv4");
    return QStringLiteral("%1:%2").arg(protocol, address.toString());
}
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
    // Este camino crea y mantiene los QProcess de los túneles, que cuelgan del dueño de
    // la sesión. Ejecutarlo desde un hilo de QtConcurrent crearía hijos con padre en otro
    // hilo, que es un aviso de afinidad o directamente una caída.
    if (failureReason) {
        failureReason->clear();
    }
    if (commandMayHaveRunOut) {
        *commandMayHaveRunOut = false;
    }
    if (!ses.enHiloDeTuneles()) {
        if (failureReason) {
            *failureReason = QStringLiteral("rpc invocado fuera del hilo de los túneles");
        }
        return false;
    }

    out.clear();
    err.clear();
    rc = -1;
    if (agentArgs.isEmpty()) {
        if (failureReason) {
            *failureReason = QStringLiteral("argumentos de agente vacíos");
        }
        return false;
    }
    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        if (failureReason) {
            *failureReason = QStringLiteral("tipo de conexión no SSH");
        }
        return false;
    }
    const QString rpcConnKey = remoteDaemonTlsCacheKey(p);
    // Reentrancia: si el túnel de esta conexión se está montando en un marco anterior de
    // la pila, NO se monta otro ni se intenta nada.
    //
    // Se sale con el motivo reservado, que NO cuenta como fallo: la primera versión de
    // esto devolvía un false corriente y el llamante lo tomaba por un túnel caído, metía
    // la conexión en backoff de 30 s y con eso tumbaba el refresco de verdad. Estar
    // ocupado no es estar roto.
    {
        QMutexLocker lock(&ses.mutex);
        if (ses.tunnelsBeingCreated.contains(rpcConnKey)) {
            if (failureReason) {
                *failureReason = mwhelpers::rpcTunnelBusyReason();
            }
            return false;
        }
    }
    const auto closeTunnelForKey = [&](const QString& key) {
        QPointer<QProcess> proc;
        {
            QMutexLocker lock(&ses.mutex);
            const auto it = ses.tunnelsByConnKey.find(key);
            if (it == ses.tunnelsByConnKey.end()) {
                return;
            }
            proc = it->process;
            ses.tunnelsByConnKey.erase(it);
        }
        if (proc && proc->state() != QProcess::NotRunning) {
            proc->terminate();
            if (!proc->waitForFinished(700)) {
                proc->kill();
                proc->waitForFinished(700);
            }
        }
        if (proc) {
            proc->deleteLater();
        }
    };
    const auto pruneIdleTunnels = [&]() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QStringList staleKeys;
        {
            QMutexLocker lock(&ses.mutex);
            for (auto it = ses.tunnelsByConnKey.cbegin();
                 it != ses.tunnelsByConnKey.cend(); ++it) {
                const bool running = it.value().process && it.value().process->state() != QProcess::NotRunning;
                const bool tooIdle = it.value().lastUsedUtc.isValid() && it.value().lastUsedUtc.secsTo(now) > 60;
                if (!running || tooIdle) {
                    staleKeys.push_back(it.key());
                }
            }
        }
        for (const QString& key : staleKeys) {
            closeTunnelForKey(key);
        }
    };
    pruneIdleTunnels();

    const auto ensureTunnel = [&](const QString& key,
                                  quint16 remotePort,
                                  quint16& localPortOut,
                                  QPointer<QProcess>& processOut) -> bool {
        localPortOut = 0;
        processOut = nullptr;
        // Marca de "montando este túnel ahora mismo", contra la REENTRANCIA que los
        // multiplicaba. Quien la consulta es rpcTunnelCreationInProgress, arriba del
        // todo: aquí solo se pone y se quita.
        //
        // Montar un túnel espera a que el puerto acepte conexiones, y esa espera bombea
        // el ciclo de eventos para no congelar la ventana cinco segundos. En ese hueco
        // salta el temporizador del latido, pide RPC a la MISMA conexión, no encuentra
        // túnel en el mapa —el de fuera todavía no se ha registrado— y monta un segundo.
        // Al terminar, el `insert` del de fuera pisaba la entrada del de dentro y lo
        // dejaba huérfano: vivo, colgando de la ventana y fuera del mapa, así que ni se
        // reutilizaba ni se cerraba nunca. Medido: 3 túneles por máquina donde debe
        // haber 1, seis conexiones SSH abiertas y subiendo.
        struct CreationLock {
            TransportSession* ses;
            QString key;
            ~CreationLock() {
                QMutexLocker lock(&ses->mutex);
                ses->tunnelsBeingCreated.remove(key);
            }
        };
        {
            QMutexLocker lock(&ses.mutex);
            ses.tunnelsBeingCreated.insert(key);
        }
        CreationLock creationLock{&ses, key};
        const QDateTime now = QDateTime::currentDateTimeUtc();
        bool needsRecreate = false;
        {
            QMutexLocker lock(&ses.mutex);
            const auto it = ses.tunnelsByConnKey.find(key);
            if (it != ses.tunnelsByConnKey.end()) {
                const bool running = it->process && it->process->state() != QProcess::NotRunning;
                const bool remoteMatches = (it->remotePort == remotePort);
                const bool tooIdle = it->lastUsedUtc.isValid() && it->lastUsedUtc.secsTo(now) > 45;
                if (running && remoteMatches && !tooIdle) {
                    it->lastUsedUtc = now;
                    localPortOut = it->localPort;
                    processOut = it->process;
                    return (localPortOut > 0 && processOut);
                }
                needsRecreate = true;
            }
        }
        if (needsRecreate) {
            closeTunnelForKey(key);
        }

        QTcpServer portProbe;
        if (!portProbe.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        const quint16 localPort = portProbe.serverPort();
        portProbe.close();
        if (localPort == 0) {
            return false;
        }

        QString tunnelProgram = QStringLiteral("ssh");
        QStringList tunnelArgs;
        const bool hasPassword = !p.password.trimmed().isEmpty();
        if (hasPassword) {
            const QString sshpassExe = findLocalExecutable(QStringLiteral("sshpass"));
            if (!sshpassExe.isEmpty()) {
                tunnelProgram = sshpassExe;
                tunnelArgs << "-p" << p.password << "ssh";
            }
        }
        const QString familyOpt = sshAddressFamilyOption(p);
        if (!familyOpt.isEmpty()) {
            tunnelArgs << familyOpt;
        }
        tunnelArgs << "-o" << "BatchMode=yes";
        tunnelArgs << "-o" << "ConnectTimeout=10";
        tunnelArgs << "-o" << "LogLevel=ERROR";
        tunnelArgs << "-o" << "StrictHostKeyChecking=accept-new";
        tunnelArgs << "-o" << "ExitOnForwardFailure=yes";
        if (hasPassword && tunnelProgram != QStringLiteral("ssh")) {
            tunnelArgs << "-o" << "BatchMode=no";
            tunnelArgs << "-o" << "PreferredAuthentications=password,keyboard-interactive,publickey";
            tunnelArgs << "-o" << "NumberOfPasswordPrompts=1";
        }
        if (p.port > 0) {
            tunnelArgs << "-p" << QString::number(p.port);
        }
        if (!p.keyPath.isEmpty()) {
            tunnelArgs << "-i" << p.keyPath;
        }
        tunnelArgs << "-L" << QStringLiteral("%1:127.0.0.1:%2").arg(localPort).arg(remotePort);
        tunnelArgs << "-N" << sshUserHost(p);

        // El padre es el dueño de la sesión, que es quien vive en este hilo. Sin dueño
        // (una herramienta de un solo hilo) el proceso no tiene padre y lo gobierna el
        // propio mapa de túneles.
        QProcess* proc = new QProcess(ses.owner);
        proc->start(tunnelProgram, tunnelArgs);
        if (!proc->waitForStarted(5000)) {
            proc->deleteLater();
            return false;
        }
        // Wait until the forwarded port actually accepts connections. A fixed short
        // sleep used to be enough only on warm/fast links: establishing the tunnel
        // needs a full SSH handshake (plus mDNS resolution for *.local hosts), which
        // measured ~830 ms against unib.local. Connecting too early yields
        // ECONNREFUSED, which the caller then reports as a TLS handshake failure and
        // puts the connection into TLS backoff — a misleading dead end.
        {
            QElapsedTimer readyTimer;
            readyTimer.start();
            bool tunnelReady = false;
            bool sshDied = false;
            while (readyTimer.elapsed() < 5000) {
                if (proc->state() == QProcess::NotRunning) {
                    sshDied = true;
                    break;
                }
                QTcpSocket probe;
                probe.connectToHost(QHostAddress::LocalHost, localPort);
                if (probe.waitForConnected(200)) {
                    probe.abort();
                    tunnelReady = true;
                    break;
                }
                // Bombear mientras se espera: esto corre en el hilo de INTERFAZ, y
                // waitForConnected + msleep lo dejaban congelado hasta 5 s por intento.
                // Con una máquina apagada son dos intentos seguidos nada más arrancar,
                // así que la ventana ya existía pero no llegaba a pintarse: es lo que se
                // veía como "tarda mucho en aparecer".
                //
                // ExcludeUserInputEvents a propósito: deja pasar los repintados pero NO
                // las acciones del usuario, así que no puede colarse por aquí nada que
                // recargue las conexiones y deje colgando las referencias que sostiene
                // quien nos llamó. Es más estricto que el processEvents de runSsh.
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 40);
                QThread::msleep(10);
            }
            if (!tunnelReady) {
                // El tiempo REAL y el motivo, no un "5 s" fijo. Este bucle sale antes si
                // el ssh muere —máquina apagada, sin ruta—, así que el mensaje anterior
                // afirmaba cinco segundos cuando podían haber sido doscientos
                // milisegundos. Un registro que miente sobre cuánto tardó algo hace
                // perder horas buscando la lentitud donde no está.
                const QString why = sshDied
                                        ? QStringLiteral("el ssh del túnel terminó")
                                        : QStringLiteral("agotados los 5 s de espera");
                ses.log(TransportSession::Nivel::Warn,
                       QStringLiteral("daemon-rpc: el túnel SSH a %1 no aceptó conexiones (%2, %3 ms)")
                           .arg(p.name, why, QString::number(readyTimer.elapsed())));
                if (proc->state() != QProcess::NotRunning) {
                    proc->terminate();
                    if (!proc->waitForFinished(1500)) {
                        proc->kill();
                        proc->waitForFinished(1500);
                    }
                }
                proc->deleteLater();
                return false;
            }
        }

        // El contexto de la conexión es el PROPIO proceso, no la ventana: esta función
        // ya no es un método suyo. Sigue habiendo contexto —que es lo que importa para
        // que la lambda no corra tras destruirse el emisor—.
        QObject::connect(
            proc,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            proc,
            [&ses, key, proc](int, QProcess::ExitStatus) {
                QMutexLocker lock(&ses.mutex);
                auto it = ses.tunnelsByConnKey.find(key);
                if (it != ses.tunnelsByConnKey.end() && it->process == proc) {
                    ses.tunnelsByConnKey.erase(it);
                }
                proc->deleteLater();
            });

        {
            QMutexLocker lock(&ses.mutex);
            RemoteRpcTunnelState st;
            st.process = proc;
            st.localPort = localPort;
            st.remotePort = remotePort;
            st.startedAtUtc = now;
            st.lastUsedUtc = now;
            ses.tunnelsByConnKey.insert(key, st);
        }

        localPortOut = localPort;
        processOut = proc;
        return true;
    };

    QString lastAttemptReason;
    const auto attempt = [&](bool forceRefreshTls) -> bool {
        QByteArray serverCertPem;
        QByteArray clientCertPem;
        QByteArray clientKeyPem;
        quint16 daemonPort = 47653;
        bool fetchedFromRemote = false;
        bool clientKeyFetchedFromRemote = false;
        if (!fetchRemoteDaemonTlsMaterial(
                p,
                serverCertPem,
                clientCertPem,
                clientKeyPem,
                daemonPort,
                forceRefreshTls,
                &fetchedFromRemote,
                &clientKeyFetchedFromRemote,
                &lastAttemptReason)) {
            return false;
        }
        if (fetchedFromRemote) {
            QString persistErr;
            if (!ses.persistTls(p, serverCertPem, clientCertPem, clientKeyPem, daemonPort, &persistErr)) {
                ses.log(TransportSession::Nivel::Warn,
                       QStringLiteral("daemon-rpc TLS persist fallback %1 -> %2")
                           .arg(p.name, persistErr.isEmpty() ? QStringLiteral("upsert failed") : persistErr));
            }
        }

        const QList<QSslCertificate> caCerts = QSslCertificate::fromData(serverCertPem, QSsl::Pem);
        const QList<QSslCertificate> clientCerts = QSslCertificate::fromData(clientCertPem, QSsl::Pem);
        if (caCerts.isEmpty() || clientCerts.isEmpty()) {
            lastAttemptReason = QStringLiteral("certificados TLS del daemon inválidos");
            return false;
        }
        QSslKey clientKey(clientKeyPem, QSsl::Rsa, QSsl::Pem);
        if (clientKey.isNull()) {
            clientKey = QSslKey(clientKeyPem, QSsl::Ec, QSsl::Pem);
        }
        if (clientKey.isNull()) {
            lastAttemptReason = QStringLiteral("clave TLS cliente inválida");
            return false;
        }
        quint16 localPort = 0;
        QPointer<QProcess> tunnelProc;
        if (!ensureTunnel(rpcConnKey, daemonPort, localPort, tunnelProc) || localPort == 0 || !tunnelProc) {
            lastAttemptReason = QStringLiteral("no se pudo establecer túnel SSH local->daemon");
            return false;
        }

        // Qué backend TLS está activo, una sola vez por ejecución. Sin este dato, un
        // fallo de handshake no dice si lo está evaluando OpenSSL o el
        // SecureTransport de Apple, que aplican políticas distintas: la app de macOS
        // no podía hablar con ningún daemon porque cae a SecureTransport (el bundle
        // no lleva libssl) y este rechaza los certificados que OpenSSL acepta.
        static std::once_flag tlsBackendLogOnce;
        std::call_once(tlsBackendLogOnce, [&ses]() {
            // Preferir OpenSSL cuando esté disponible, para que todas las
            // plataformas se comporten igual. En macOS, SecureTransport además
            // exige la clave privada del cliente en el llavero y hace que el
            // sistema pida la contraseña al usuario en cada arranque.
            const QString before = QSslSocket::activeBackend();
            if (before != QStringLiteral("openssl")
                && QSslSocket::availableBackends().contains(QStringLiteral("openssl"))) {
                if (QSslSocket::setActiveBackend(QStringLiteral("openssl"))) {
                    ses.log(TransportSession::Nivel::Info,
                           QStringLiteral("TLS: backend cambiado de %1 a openssl").arg(before));
                }
            }
            ses.log(TransportSession::Nivel::Info,
                   QStringLiteral("TLS: backend activo=%1 disponibles=[%2] libssl=%3")
                       .arg(QSslSocket::activeBackend(),
                            QSslSocket::availableBackends().join(QStringLiteral(", ")),
                            QSslSocket::sslLibraryVersionString()));
        });

        const int connectTimeout = qBound(600, timeoutMs > 0 ? timeoutMs / 5 : 1200, 3500);
        // timeoutMs <= 0 significa SIN LÍMITE, igual que en el camino SSH que esta vía
        // sustituye (`if (timeoutMs > 0 && timer.elapsed() > timeoutMs)`). Antes se
        // convertía en 30 s, y un `qBound(..., 70000)` capaba además cualquier plazo
        // explícito: pedir 600000 daba 70 s.
        //
        // Desglosar, Ensamblar y Hacia Dir piden 0 a propósito porque copian datos
        // reales. Al expirar se cerraba el túnel y se reintentaba, y cerrar el túnel NO
        // aborta nada en el remoto —comprobado: el daemon completa la operación tras la
        // desconexión—, así que la misma orden destructiva podía solaparse consigo misma.
        //
        // "Sin límite" no es "colgado para siempre": el bucle de lectura sale en cuanto
        // el proceso del túnel muere, que es la misma red de seguridad que tiene SSH.
        const bool noIoDeadline = (timeoutMs <= 0);
        const qint64 ioTimeout = noIoDeadline ? 0 : qMax(1000, timeoutMs);
        const QString cmd = agentArgs.first().trimmed();
        const QStringList params = agentArgs.mid(1);
        const QStringList peerNames = {QStringLiteral("zfsmgr-agent-server"), QStringLiteral("zfsmgr-agent")};

        for (const QString& peerName : peerNames) {
            QSslSocket sock;
            sock.setProtocol(QSsl::TlsV1_2OrLater);
            QSslConfiguration conf = sock.sslConfiguration();
            conf.setCaCertificates(caCerts);
            conf.setLocalCertificate(clientCerts.first());
            conf.setPrivateKey(clientKey);
            conf.setProtocol(QSsl::TlsV1_2OrLater);
            // VerifyNone + fijación: ver peerCertificateIsPinned.
            conf.setPeerVerifyMode(QSslSocket::VerifyNone);
            sock.setSslConfiguration(conf);

            sock.connectToHostEncrypted(QStringLiteral("127.0.0.1"), localPort, peerName);
            if (!sock.waitForEncrypted(connectTimeout)) {
                QStringList sslErrStrs;
                for (const QSslError& e : sock.sslHandshakeErrors()) {
                    sslErrStrs << e.errorString();
                }
                if (sslErrStrs.isEmpty()) {
                    // No TLS-level error was produced: the socket never got far enough
                    // (connection refused, timeout, tunnel not ready yet). Reporting
                    // that as a TLS handshake failure points diagnosis at the
                    // certificates and marks the connection "TLS desincronizado",
                    // which triggers a re-provisioning that cannot fix a transport
                    // problem.
                    lastAttemptReason = QStringLiteral("conexión daemon-rpc fallida (peer=%1): %2")
                                            .arg(peerName, sock.errorString());
                } else {
                    lastAttemptReason = QStringLiteral("fallo handshake TLS daemon-rpc (peer=%1): %2")
                                            .arg(peerName, sslErrStrs.join(QStringLiteral("; ")));
                }
                continue;
            }
            if (!peerCertificateIsPinned(sock, caCerts)) {
                lastAttemptReason =
                    QStringLiteral("el certificado que presenta el daemon no coincide con el fijado");
                sock.abort();
                continue;
            }

            QJsonObject req;
            req.insert(QStringLiteral("cmd"), cmd);
            QJsonArray args;
            for (const QString& pArg : params) {
                args.push_back(pArg);
            }
            req.insert(QStringLiteral("args"), args);
            const QByteArray payload = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
            // Mark before writing, not after: a partial write can still reach the
            // daemon and start the command, so anything from here on is ambiguous.
            if (commandMayHaveRunOut) {
                *commandMayHaveRunOut = true;
            }
            if (sock.write(payload) < 0 || !sock.waitForBytesWritten(connectTimeout)) {
                lastAttemptReason = QStringLiteral("fallo al enviar solicitud RPC");
                continue;
            }

            QByteArray line;
            QElapsedTimer timer;
            timer.start();
            while (noIoDeadline || timer.elapsed() < ioTimeout) {
                if (!sock.waitForReadyRead(300)) {
                    if (!tunnelProc || tunnelProc->state() == QProcess::NotRunning) {
                        lastAttemptReason = QStringLiteral("túnel SSH daemon-rpc finalizado durante espera");
                        break;
                    }
                    continue;
                }
                line.append(sock.readAll());
                const int nl = line.indexOf('\n');
                if (nl < 0) {
                    continue;
                }
                const QByteArray one = line.left(nl).trimmed();
                const QJsonObject resp = QJsonDocument::fromJson(one).object();
                rc = resp.value(QStringLiteral("rc")).toInt(1);
                out = resp.value(QStringLiteral("stdout")).toString();
                err = resp.value(QStringLiteral("stderr")).toString();
                {
                    QMutexLocker lock(&ses.mutex);
                    auto it = ses.tunnelsByConnKey.find(rpcConnKey);
                    if (it != ses.tunnelsByConnKey.end()) {
                        it->lastUsedUtc = QDateTime::currentDateTimeUtc();
                    }
                }
                return true;
            }
            if (lastAttemptReason.isEmpty()) {
                lastAttemptReason = QStringLiteral("timeout esperando respuesta RPC");
            }
            // Ya se escribió la solicitud: probar el segundo nombre de par abriría otra
            // sesión y ENVIARÍA LA MISMA ORDEN otra vez. Este bucle existe solo para
            // resolver con qué nombre se presenta el certificado del daemon, no para
            // reintentar trabajo. Era un envío duplicado dentro de un mismo intento, y
            // el guardián de commandMayHaveRun no lo cubría porque no llega a verlo.
            break;
        }
        closeTunnelForKey(rpcConnKey);
        if (lastAttemptReason.isEmpty()) {
            lastAttemptReason = QStringLiteral("daemon-rpc sin respuesta válida");
        }
        return false;
    };

    if (attempt(false)) {
        return true;
    }
    if (commandMayHaveRunOut && *commandMayHaveRunOut) {
        // The request already reached the daemon. Retrying would submit the same
        // command a second time while the first one may still be running remotely,
        // which for a mutation means duplicated destructive work.
        if (failureReason && !lastAttemptReason.isEmpty()) {
            *failureReason = lastAttemptReason;
        }
        return false;
    }
    const QString firstFailure = lastAttemptReason.trimmed().toLower();
    if (firstFailure.contains(QStringLiteral("handshake tls daemon-rpc"))
        || firstFailure.contains(QStringLiteral("conexión daemon-rpc fallida"))
        || firstFailure.contains(QStringLiteral("daemon-rpc sin respuesta válida"))
        || firstFailure.contains(QStringLiteral("túnel ssh daemon-rpc finalizado"))) {
        if (tryReviveRemoteDaemonService(p)) {
            ses.log(TransportSession::Nivel::Info,
                   QStringLiteral("daemon-rpc revive requested on %1 after failure: %2")
                       .arg(p.name, lastAttemptReason));
        }
    }
    if (attempt(true)) {
        return true;
    }
    if (failureReason && !lastAttemptReason.isEmpty()) {
        *failureReason = lastAttemptReason;
    }
    return false;
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
    QByteArray serverCertPem;
    QByteArray clientCertPem;
    QByteArray clientKeyPem;
    quint16 daemonPort = 47653;
    bool fetchedFromRemote = false;
    bool clientKeyFetchedFromRemote = false;
    QString fetchReason;
    if (!fetchRemoteDaemonTlsMaterial(
            p,
            serverCertPem,
            clientCertPem,
            clientKeyPem,
            daemonPort,
            true,
            &fetchedFromRemote,
            &clientKeyFetchedFromRemote,
            &fetchReason)) {
        if (errorOut) {
            *errorOut = fetchReason.isEmpty() ? QStringLiteral("no se pudo obtener bundle TLS") : fetchReason;
        }
        return false;
    }
    QString persistErr;
    if (!persistDaemonTlsMaterialForConnection(
            p, serverCertPem, clientCertPem, clientKeyPem, daemonPort, &persistErr)) {
        if (errorOut) {
            *errorOut = persistErr.isEmpty() ? QStringLiteral("no se pudo persistir TLS en config") : persistErr;
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
    QMutexLocker lock(&s_localDaemonTlsCacheMutex);
    s_localDaemonTlsCache = LocalDaemonTlsCacheEntry{};
}

bool transport::ensureLocalDaemonTlsMaterial(TransportSession& ses,
                                             QByteArray& serverCertPem,
                                              QByteArray& clientCertPem,
                                              QByteArray& clientKeyPem,
                                              quint16& daemonPort) {
    {
        QMutexLocker lock(&s_localDaemonTlsCacheMutex);
        if (s_localDaemonTlsCache.fetchedAtUtc.isValid()
            && s_localDaemonTlsCache.fetchedAtUtc.msecsTo(QDateTime::currentDateTimeUtc())
                   <= 5 * 60 * 1000
            && !s_localDaemonTlsCache.serverCertPem.isEmpty()) {
            serverCertPem = s_localDaemonTlsCache.serverCertPem;
            clientCertPem = s_localDaemonTlsCache.clientCertPem;
            clientKeyPem = s_localDaemonTlsCache.clientKeyPem;
            daemonPort = s_localDaemonTlsCache.port;
            return true;
        }
    }

    // Primero sin elevar: si la interfaz corre como root, o alguien aflojó los
    // permisos, no hay por qué pedir nada.
    const LocalAgentConfig cfg = loadLocalAgentConfig();
    const auto readFile = [](const QString& path) -> QByteArray {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    QByteArray srv = readFile(cfg.tlsCertPath);
    QByteArray cli = readFile(cfg.tlsClientCertPath);
    QByteArray key = readFile(cfg.tlsClientKeyPath);
    quint16 port = static_cast<quint16>(cfg.port > 0 ? cfg.port : 47653);

    if (srv.isEmpty() || cli.isEmpty() || key.isEmpty()) {
#ifdef Q_OS_WIN
        // En Windows el material vive bajo C:\ProgramData y lo puede leer el usuario,
        // así que la lectura directa de arriba basta. Si ha fallado, no queda camino
        // alternativo: no hay sudo ni intérprete POSIX que ejecute el guion de abajo,
        // y lanzarlo daría un error que no dice nada. Se explica lo que pasa.
        ses.log(TransportSession::Nivel::Warn,
               QStringLiteral("Local: no se pudo leer el material TLS del daemon en %1. "
                              "Reinstale el daemon desde el menú de la conexión.")
                   .arg(cfg.tlsCertPath));
        return false;
#else
        // Mismo guion y mismos marcadores que el camino remoto, para poder reutilizar
        // su parseador en vez de escribir un segundo formato que se desincronice.
        const QString bundleScript =
            QStringLiteral("set -eu; "
                           "for f in %1 %2 %3; do "
                           "  if [ -r \"$f\" ]; then "
                           "    printf '__ZFSMGR_TLS_BEGIN__:%s\\n' \"$f\"; "
                           "    cat \"$f\"; "
                           "    printf '__ZFSMGR_TLS_END__:%s\\n' \"$f\"; "
                           "  fi; "
                           "done; "
                           "if [ -r %4 ]; then "
                           "  port=$(awk -F= '/^[[:space:]]*AGENT_PORT[[:space:]]*=/{print $2}' %4 "
                           "| tail -n1 | tr -d \"' \\t\\r\"); "
                           "  if [ -n \"$port\" ]; then printf '__ZFSMGR_AGENT_PORT__:%s\\n' \"$port\"; fi; "
                           "fi")
                .arg(mwhelpers::shSingleQuote(cfg.tlsCertPath),
                     mwhelpers::shSingleQuote(cfg.tlsClientCertPath),
                     mwhelpers::shSingleQuote(cfg.tlsClientKeyPath),
                     mwhelpers::shSingleQuote(QString::fromLatin1(kDefaultAgentConfigPath)));

        ConnectionProfile sudoProfile;
        sudoProfile.id = QStringLiteral("local");
        sudoProfile.connType = QStringLiteral("LOCAL");
        sudoProfile.useSudo = true;
        if (!ses.resolveLocalSudo(sudoProfile)) {
            ses.log(TransportSession::Nivel::Warn,
                   QStringLiteral("Local: no se pudo leer el material TLS del daemon (faltan "
                                  "credenciales sudo locales)."));
            return false;
        }
        const QString cmd =
            mwhelpers::withSudoCommand(sudoProfile,
                                       QStringLiteral("sh -lc %1").arg(mwhelpers::shSingleQuote(bundleScript)));
        QString out;
        QString err;
        int rc = -1;
        if (!runSsh(ses, sudoProfile, cmd, 15000, out, err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false)
            || rc != 0) {
            ses.log(TransportSession::Nivel::Warn,
                   QStringLiteral("Local: no se pudo leer el material TLS del daemon -> %1")
                       .arg(mwhelpers::oneLine(err.isEmpty() ? out : err)));
            return false;
        }
        if (!parseRemoteDaemonTlsBundle(out, srv, cli, key, port)) {
            ses.log(TransportSession::Nivel::Warn,
                   QStringLiteral("Local: el material TLS del daemon llegó incompleto."));
            return false;
        }
#endif
    }

    if (srv.isEmpty() || cli.isEmpty() || key.isEmpty()) {
        return false;
    }
    {
        QMutexLocker lock(&s_localDaemonTlsCacheMutex);
        s_localDaemonTlsCache.serverCertPem = srv;
        s_localDaemonTlsCache.clientCertPem = cli;
        s_localDaemonTlsCache.clientKeyPem = key;
        s_localDaemonTlsCache.port = port;
        s_localDaemonTlsCache.fetchedAtUtc = QDateTime::currentDateTimeUtc();
    }
    serverCertPem = srv;
    clientCertPem = cli;
    clientKeyPem = key;
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
    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0 || agentArgs.isEmpty()) {
        return false;
    }
    const QString rpcConnKey = remoteDaemonTlsCacheKey(p);
    bool allowRpcAttempt = true;
    QString suppressedReason;
    {
        QMutexLocker lock(&ses.mutex);
        const auto it = ses.retryAfterByConnKey.constFind(rpcConnKey);
        if (it != ses.retryAfterByConnKey.constEnd()
            && it.value().isValid()
            && QDateTime::currentDateTimeUtc() < it.value()) {
            allowRpcAttempt = false;
            suppressedReason = QStringLiteral("backoff activo %1s")
                                   .arg(QDateTime::currentDateTimeUtc().secsTo(it.value()));
        }
    }
    bool rpcAttemptOk = false;
    QString rpcFailureReason;
    bool rpcCommandMayHaveRun = false;
    if (allowRpcAttempt) {
        if (ses.enHiloDeTuneles()) {
            rpcAttemptOk = tryRunRemoteAgentRpcViaTunnel(
                ses, p, agentArgs, timeoutMs, out, err, rc, &rpcFailureReason,
                &rpcCommandMayHaveRun);
        } else {
            // Al hilo del dueño, y bloqueando: el resultado se necesita aquí. Es lo que
            // serializa el montaje de túneles cuando el dueño es la ventana.
            QMetaObject::invokeMethod(
                ses.owner,
                [&ses, &p, &agentArgs, timeoutMs, &out, &err, &rc, &rpcAttemptOk,
                 &rpcFailureReason, &rpcCommandMayHaveRun]() {
                    rpcAttemptOk = tryRunRemoteAgentRpcViaTunnel(
                        ses, p, agentArgs, timeoutMs, out, err, rc, &rpcFailureReason,
                        &rpcCommandMayHaveRun);
                },
                Qt::BlockingQueuedConnection);
        }
    }
    if (rpcAttemptOk) {
        {
            QMutexLocker lock(&ses.mutex);
            ses.retryAfterByConnKey.remove(rpcConnKey);
            ses.retryReasonByConnKey.remove(rpcConnKey);
        }
        const QString cmdLine = QStringLiteral("%1 $ [daemon-rpc] %2")
                                    .arg(sshUserHostPort(p), mwhelpers::maskedAgentArgvForLog(agentArgs));
        ses.logConn(TransportSession::Nivel::Info, p.id, cmdLine);
        const auto emitLines = [&](const QString& text, const std::function<void(const QString&)>& cb) {
            const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
            for (const QString& rawLine : lines) {
                const QString line = rawLine.trimmed();
                if (line.isEmpty()) {
                    continue;
                }
                if (cb) {
                    cb(line);
                }
                if (echoOutputToLog) {
                    ses.logConn(TransportSession::Nivel::Normal, p.id, line);
                }
            }
        };
        emitLines(out, onStdoutLine);
        emitLines(err, onStderrLine);
        if (!out.trimmed().isEmpty()) {
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(out));
            }
        }
        if (!err.trimmed().isEmpty()) {
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(err));
            }
        }
        return true;
    } else if (allowRpcAttempt && rpcCommandMayHaveRun && isMutatingAgentCommand(agentArgs)) {
        // The daemon received a mutating command and we never got its answer.
        // Closing the tunnel does not abort remote work, so falling back to
        // SSH here would run the same destructive command a second time,
        // possibly overlapping with the first. Fail loudly instead.
        const QString reason = rpcFailureReason.trimmed().isEmpty()
                                   ? QStringLiteral("motivo no especificado")
                                   : rpcFailureReason.trimmed();
        const QString abortLine =
            QStringLiteral("%1 $ [daemon-rpc:sin-fallback] %2 -> %3"
                           " (la orden ya llegó al daemon; no se reintenta para no duplicarla)")
                .arg(sshUserHostPort(p), mwhelpers::maskedAgentArgvForLog(agentArgs), reason);
        ses.logConn(TransportSession::Nivel::Error, p.id, abortLine);
        out.clear();
        err = QStringLiteral(
                  "La orden se envió al daemon pero no se recibió respuesta (%1).\n"
                  "Puede seguir ejecutándose en el equipo remoto, así que ZFSMgr no la "
                  "reintenta automáticamente.\nCompruebe el estado antes de repetirla.")
                  .arg(reason);
        rc = 124;
        return true;
    } else if (allowRpcAttempt
               && rpcFailureReason.trimmed() == mwhelpers::rpcTunnelBusyReason()) {
        // Ocupado no es roto. El túnel se está montando en un marco anterior de la pila,
        // así que esta llamada se salta el RPC y sale por el camino de siempre SIN
        // castigar a la conexión: con el backoff de 30 s, un sondeo que llegara en ese
        // hueco dejaba sin daemon al refresco que venía detrás.
        const QString skippedLine =
            QStringLiteral("%1 $ [daemon-rpc:skip] %2 -> %3")
                .arg(sshUserHostPort(p), mwhelpers::maskedAgentArgvForLog(agentArgs),
                     mwhelpers::rpcTunnelBusyReason());
        ses.logConn(TransportSession::Nivel::Info, p.id, skippedLine);
    } else if (allowRpcAttempt) {
        const QString reason = rpcFailureReason.trimmed().isEmpty()
                                   ? QStringLiteral("motivo no especificado")
                                   : rpcFailureReason.trimmed();
        const QString fallbackLine =
            QStringLiteral("%1 $ [daemon-rpc:fallback] %2 -> %3")
                .arg(sshUserHostPort(p), mwhelpers::maskedAgentArgvForLog(agentArgs), reason);
        ses.logConn(TransportSession::Nivel::Info, p.id, fallbackLine);
        QMutexLocker lock(&ses.mutex);
        constexpr int kDaemonRpcRetryBackoffSec = 30;
        ses.retryAfterByConnKey.insert(
            rpcConnKey, QDateTime::currentDateTimeUtc().addSecs(kDaemonRpcRetryBackoffSec));
        ses.retryReasonByConnKey.insert(rpcConnKey, reason);
    } else if (!suppressedReason.isEmpty()) {
        const QString skippedLine =
            QStringLiteral("%1 $ [daemon-rpc:skip] %2 -> %3")
                .arg(sshUserHostPort(p), mwhelpers::maskedAgentArgvForLog(agentArgs), suppressedReason);
        ses.logConn(TransportSession::Nivel::Info, p.id, skippedLine);
    }
    return false;
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
    return m_transport.callsForTest;
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
        m_transport.callsForTest.push_back(TransportSession::AgentCallForTest{agentArgs, QString(), stdinPayload});
        return m_transport.transportForTest(agentArgs, out, err, rc);
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
    out.clear();
    err.clear();
    rc = -1;

    // Con el transporte de mentira puesto no se abre ninguna conexión. Si la orden es
    // una invocación del agente construida como cadena, se atiende igual —para que los
    // sitios aún sin migrar funcionen en los tests—; si no lo es, se registra y fracasa,
    // que es lo que permite afirmar "esto NO debía irse por shell".
    if (ses.transportForTest) {
        QStringList parsedArgs;
        if (allowAgentRpc && stdinPayload.isEmpty()
            && extractLocalAgentArgs(remoteCmd.trimmed(), parsedArgs)) {
            ses.callsForTest.push_back(
                TransportSession::AgentCallForTest{parsedArgs, remoteCmd.trimmed(), stdinPayload});
            return ses.transportForTest(parsedArgs, out, err, rc);
        }
        ses.callsForTest.push_back(
            TransportSession::AgentCallForTest{QStringList(), remoteCmd.trimmed(), stdinPayload});
        err = QStringLiteral("transporte de prueba: no se ejecuta shell");
        rc = 127;
        return false;
    }

    if (isLocalConnection(p)) {
        const QString localCmd = remoteCmd.trimmed();
        const QString cmdLine = QStringLiteral("[local] $ %1").arg(localCmd);
        ses.logConn(TransportSession::Nivel::Info, p.id, cmdLine);

        // stdin no vacío descarta el RPC: el canal no transporta stdin (el daemon lo
        // dice en runExecCaptureWithStdin) y la intercepción no lo miraba, así que la
        // passphrase de un dataset cifrado se perdía en silencio al desglosarlo.
        QStringList localAgentArgs;
        if (allowAgentRpc && stdinPayload.isEmpty() && extractLocalAgentArgs(localCmd, localAgentArgs)) {
            // Al hilo de interfaz, igual que la rama SSH. El RPC abre un socket TLS y
            // hacerlo desde un hilo de trabajo revienta: no saltaba porque la conexión
            // Local estaba excluida de todo lo que se ejecuta en segundo plano.
            bool localRpcOk = false;
            const auto runLocalRpc = [&]() {
                QByteArray srvPem;
                QByteArray cliPem;
                QByteArray keyPem;
                quint16 localPort = 47653;
                localRpcOk = ensureLocalDaemonTlsMaterial(ses, srvPem, cliPem, keyPem, localPort)
                             && tryRunLocalAgentRpc(localAgentArgs, srvPem, cliPem, keyPem,
                                                    localPort, timeoutMs, out, err, rc);
            };
            if (ses.enHiloDeTuneles()) {
                runLocalRpc();
            } else {
                QMetaObject::invokeMethod(ses.owner, runLocalRpc, Qt::BlockingQueuedConnection);
            }
            if (localRpcOk) {
                const auto emitLines = [&](const QString& text, const std::function<void(const QString&)>& cb) {
                    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
                    for (const QString& rawLine : lines) {
                        const QString line = rawLine.trimmed();
                        if (line.isEmpty()) {
                            continue;
                        }
                        if (cb) {
                            cb(line);
                        }
                        if (echoOutputToLog) {
                            ses.logConn(TransportSession::Nivel::Normal, p.id, line);
                        }
                    }
                };
                emitLines(out, onStdoutLine);
                emitLines(err, onStderrLine);
                if (!out.trimmed().isEmpty()) {
                    if (echoOutputToLog) {
                        ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(out));
                    }
                }
                if (!err.trimmed().isEmpty()) {
                    if (echoOutputToLog) {
                        ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(err));
                    }
                }
                return true;
            }
        }

        QProcess proc;
        QString program;
        QStringList args;
#ifdef Q_OS_WIN
        program = QStringLiteral("cmd.exe");
        args << "/C" << wrapRemoteCommand(p, localCmd);
#else
        program = QStringLiteral("sh");
        args << "-c" << mwhelpers::asciiSafeShellCommand(localCmd);
#endif
        QElapsedTimer timer;
        timer.start();
        proc.start(program, args);
        if (!proc.waitForStarted(4000)) {
            err = QStringLiteral("No se pudo iniciar %1").arg(program);
            ses.logConn(TransportSession::Nivel::Normal, p.id, err);
            return false;
        }
        if (!stdinPayload.isEmpty()) {
            proc.write(stdinPayload);
            proc.closeWriteChannel();
        }
        QString outLineBuf;
        QString errLineBuf;
        auto flushLines = [&](QString& buf, const QString& chunk, const std::function<void(const QString&)>& cb) {
            if (!chunk.isEmpty()) {
                buf += chunk;
            }
            while (true) {
                const int nl = buf.indexOf('\n');
                const int cr = buf.indexOf('\r');
                int sep = -1;
                if (nl >= 0 && cr >= 0) {
                    sep = qMin(nl, cr);
                } else if (nl >= 0) {
                    sep = nl;
                } else if (cr >= 0) {
                    sep = cr;
                }
                if (sep < 0) {
                    break;
                }
                QString line = buf.left(sep);
                buf.remove(0, sep + 1);
                line = line.trimmed();
                if (line.isEmpty()) {
                    continue;
                }
                if (cb) {
                    cb(line);
                }
                if (echoOutputToLog) {
                    ses.logConn(TransportSession::Nivel::Normal, p.id, line);
                }
            }
        };

        bool timedOut = false;
        int lastIdleRemainingSec = -1;
        while (proc.state() != QProcess::NotRunning) {
            proc.waitForReadyRead(120);
            const QString outChunk = QString::fromUtf8(proc.readAllStandardOutput());
            const QString errChunk = QString::fromUtf8(proc.readAllStandardError());
            if (!outChunk.isEmpty()) {
                timer.restart();
                lastIdleRemainingSec = -1;
                out += outChunk;
                flushLines(outLineBuf, outChunk, onStdoutLine);
            }
            if (!errChunk.isEmpty()) {
                timer.restart();
                lastIdleRemainingSec = -1;
                err += errChunk;
                flushLines(errLineBuf, errChunk, onStderrLine);
            }
            if (timeoutMs > 0 && onIdleTimeoutRemaining) {
                const int remainingSec = qMax(0, (timeoutMs - int(timer.elapsed()) + 999) / 1000);
                if (remainingSec != lastIdleRemainingSec) {
                    lastIdleRemainingSec = remainingSec;
                    onIdleTimeoutRemaining(remainingSec);
                }
            }
            if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
                timedOut = true;
                proc.kill();
                proc.waitForFinished(1000);
                break;
            }
            // Bombear el bucle solo en el hilo del dueño: es lo que evita que la
            // ventana se congele durante la espera. Sin dueño —una herramienta de un
            // solo hilo— no hay interfaz que refrescar y no se bombea nada.
            if (ses.owner && QThread::currentThread() == ses.owner->thread()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            }
        }
        const QString outTail = QString::fromUtf8(proc.readAllStandardOutput());
        const QString errTail = QString::fromUtf8(proc.readAllStandardError());
        if (!outTail.isEmpty()) {
            out += outTail;
            flushLines(outLineBuf, outTail, onStdoutLine);
        }
        if (!errTail.isEmpty()) {
            err += errTail;
            flushLines(errLineBuf, errTail, onStderrLine);
        }
        if (!outLineBuf.trimmed().isEmpty()) {
            const QString line = outLineBuf.trimmed();
            if (onStdoutLine) {
                onStdoutLine(line);
            }
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, line);
            }
        }
        if (!errLineBuf.trimmed().isEmpty()) {
            const QString line = errLineBuf.trimmed();
            if (onStderrLine) {
                onStderrLine(line);
            }
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, line);
            }
        }
        if (timedOut) {
            rc = -1;
            err = QStringLiteral("Timeout");
            ses.logConn(TransportSession::Nivel::Normal, p.id, err);
            return false;
        }
        rc = proc.exitCode();
        // ssh sale con 255 y un "Host key verification failed" escueto cuando la
        // clave del host no coincide. Sin traducirlo, eso llega al usuario como un
        // fallo de red cualquiera, y es precisamente el caso que no debe ignorar.
        if (rc != 0) {
            const QString hostKeyHint = mwhelpers::sshHostKeyProblemHint(err);
            if (!hostKeyHint.isEmpty()) {
                err = hostKeyHint + QStringLiteral("\n\n") + err;
                ses.log(TransportSession::Nivel::Warn,
                       QStringLiteral("%1: verificación de host SSH fallida").arg(p.name));
            }
        }
        if (!out.trimmed().isEmpty()) {
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(out));
            }
        }
        if (!err.trimmed().isEmpty()) {
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(err));
            }
        }
        return true;
    }

    // Windows entra por RPC como cualquier otro sistema: el daemon nativo sirve TLS por
    // el mismo túnel "ssh -L", verificado contra un Windows 11 real ejecutando ZFS.
    // Camino histórico: los argumentos se recuperan parseando la cadena. runAgentCommand
    // los pasa ya hechos y no pasa por aquí. Este parseo desaparece cuando migren todos
    // los sitios; hasta entonces convive con el nuevo.
    if (allowAgentRpc && stdinPayload.isEmpty()) {
        QStringList agentArgs;
        if (extractLocalAgentArgs(remoteCmd.trimmed(), agentArgs)
            && tryAgentRpcOverSsh(ses, p, agentArgs, timeoutMs, out, err, rc, onStdoutLine, onStderrLine,
                                  echoOutputToLog)) {
            return true;
        }
    }

    const bool hasPassword = !p.password.trimmed().isEmpty();
    QString program = QStringLiteral("ssh");
    QStringList sshpassPrefixArgs;
    bool usingSshpass = false;
    if (hasPassword) {
        const QString sshpassExe = findLocalExecutable(QStringLiteral("sshpass"));
        if (!sshpassExe.isEmpty()) {
            program = sshpassExe;
            sshpassPrefixArgs << "-p" << p.password << "ssh";
            usingSshpass = true;
        }
    }

    const QString wrappedCmd = wrapRemoteCommand(p, remoteCmd);
    const QString sshConnKey = QStringLiteral("%1|%2|%3|%4")
                                   .arg(p.username,
                                        p.host,
                                        QString::number((p.port > 0) ? p.port : 22),
                                        p.keyPath);
    const QString sshResolutionKey = QStringLiteral("%1|%2")
                                         .arg(p.host.trimmed().toLower(),
                                              p.sshAddressFamily.trimmed().toLower());

    const QString cmdLine = QStringLiteral("%1 $ %2")
                                .arg(sshUserHostPort(p), wrappedCmd);
    ses.logConn(TransportSession::Nivel::Info, p.id, cmdLine);
    if (hasPassword && !usingSshpass) {
        ses.logConn(TransportSession::Nivel::Normal, p.id, QStringLiteral("Password guardado, pero sshpass no está disponible; se usará SSH no interactivo."));
    }

    auto runSshAttempt = [&](bool enableMultiplexing, QString& attemptOut, QString& attemptErr, int& attemptRc) -> bool {
        attemptOut.clear();
        attemptErr.clear();
        attemptRc = -1;

        QStringList args = sshpassPrefixArgs;
        const QString familyOpt = sshAddressFamilyOption(p);
        if (!familyOpt.isEmpty()) {
            args << familyOpt;
        }
        args << "-o" << "BatchMode=yes";
        args << "-o" << "ConnectTimeout=10";
        args << "-o" << "LogLevel=ERROR";
        args << "-o" << "StrictHostKeyChecking=accept-new";
        if (enableMultiplexing) {
            args << "-o" << "ControlMaster=auto";
            args << "-o" << "ControlPersist=yes";
            args << "-o" << QStringLiteral("ControlPath=%1").arg(sshControlPath());
        }
        if (hasPassword && usingSshpass) {
            args << "-o" << "BatchMode=no";
            args << "-o" << "PreferredAuthentications=password,keyboard-interactive,publickey";
            args << "-o" << "NumberOfPasswordPrompts=1";
        }
        if (p.port > 0) {
            args << "-p" << QString::number(p.port);
        }
        if (!p.keyPath.isEmpty()) {
            args << "-i" << p.keyPath;
        }
        args << sshUserHost(p);
        args << mwhelpers::asciiSafeShellCommand(wrappedCmd);

        QProcess proc;
        // Garantiza que el hijo no sobreviva al QProcess, salga por donde salga esta
        // función. Qt avisa —"Destroyed while process is still running"— y el `ssh` o el
        // `sshpass` se quedan sueltos, reteniendo su socket de multiplexado. Había ramas
        // que ya mataban y otras que no; en vez de perseguirlas una a una, se cubre el
        // destructor, que es por donde pasan todas.
        struct ProcessGuard {
            QProcess* proc;
            ~ProcessGuard() {
                if (proc && proc->state() != QProcess::NotRunning) {
                    proc->kill();
                    proc->waitForFinished(2000);
                }
            }
        } processGuard{&proc};
        QElapsedTimer timer;
        timer.start();
        proc.start(program, args);
        if (!proc.waitForStarted(4000)) {
            attemptErr = QStringLiteral("No se pudo iniciar %1").arg(program);
            ses.logConn(TransportSession::Nivel::Normal, p.id, attemptErr);
            return false;
        }
        if (!stdinPayload.isEmpty()) {
            proc.write(stdinPayload);
            proc.closeWriteChannel();
        }
        QString outLineBuf;
        QString errLineBuf;
        auto flushLines = [&](QString& buf, const QString& chunk, const std::function<void(const QString&)>& cb) {
            if (!chunk.isEmpty()) {
                buf += chunk;
            }
            while (true) {
                const int nl = buf.indexOf('\n');
                const int cr = buf.indexOf('\r');
                int sep = -1;
                if (nl >= 0 && cr >= 0) {
                    sep = qMin(nl, cr);
                } else if (nl >= 0) {
                    sep = nl;
                } else if (cr >= 0) {
                    sep = cr;
                }
                if (sep < 0) {
                    break;
                }
                QString line = buf.left(sep);
                buf.remove(0, sep + 1);
                line = line.trimmed();
                if (line.isEmpty()) {
                    continue;
                }
                if (cb) {
                    cb(line);
                }
                if (echoOutputToLog) {
                    ses.logConn(TransportSession::Nivel::Normal, p.id, line);
                }
            }
        };

        bool timedOut = false;
        int lastIdleRemainingSec = -1;
        while (proc.state() != QProcess::NotRunning) {
            proc.waitForReadyRead(120);
            const QString outChunk = QString::fromUtf8(proc.readAllStandardOutput());
            const QString errChunk = QString::fromUtf8(proc.readAllStandardError());
            if (!outChunk.isEmpty()) {
                timer.restart();
                lastIdleRemainingSec = -1;
                attemptOut += outChunk;
                flushLines(outLineBuf, outChunk, onStdoutLine);
            }
            if (!errChunk.isEmpty()) {
                timer.restart();
                lastIdleRemainingSec = -1;
                attemptErr += errChunk;
                flushLines(errLineBuf, errChunk, onStderrLine);
            }
            if (timeoutMs > 0 && onIdleTimeoutRemaining) {
                const int remainingSec = qMax(0, (timeoutMs - int(timer.elapsed()) + 999) / 1000);
                if (remainingSec != lastIdleRemainingSec) {
                    lastIdleRemainingSec = remainingSec;
                    onIdleTimeoutRemaining(remainingSec);
                }
            }
            if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
                timedOut = true;
                proc.kill();
                proc.waitForFinished(1000);
                break;
            }
            // Bombear el bucle solo en el hilo del dueño: es lo que evita que la
            // ventana se congele durante la espera. Sin dueño —una herramienta de un
            // solo hilo— no hay interfaz que refrescar y no se bombea nada.
            if (ses.owner && QThread::currentThread() == ses.owner->thread()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            }
        }

        const QString outTail = QString::fromUtf8(proc.readAllStandardOutput());
        const QString errTail = QString::fromUtf8(proc.readAllStandardError());
        if (!outTail.isEmpty()) {
            attemptOut += outTail;
            flushLines(outLineBuf, outTail, onStdoutLine);
        }
        if (!errTail.isEmpty()) {
            attemptErr += errTail;
            flushLines(errLineBuf, errTail, onStderrLine);
        }
        if (!outLineBuf.trimmed().isEmpty()) {
            const QString line = outLineBuf.trimmed();
            if (onStdoutLine) {
                onStdoutLine(line);
            }
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, line);
            }
        }
        if (!errLineBuf.trimmed().isEmpty()) {
            const QString line = errLineBuf.trimmed();
            if (onStderrLine) {
                onStderrLine(line);
            }
            if (echoOutputToLog) {
                ses.logConn(TransportSession::Nivel::Normal, p.id, line);
            }
        }

        if (timedOut) {
            attemptRc = -1;
            attemptErr = QStringLiteral("Timeout");
            ses.logConn(TransportSession::Nivel::Normal, p.id, attemptErr);
            return false;
        }

        attemptRc = proc.exitCode();
        return true;
    };

    const QString hostLower = p.host.trimmed().toLower();
    const QString familyLower = p.sshAddressFamily.trimmed().toLower();
    if ((!hostLower.isEmpty() && hostLower.endsWith(QStringLiteral(".local")))
        || (familyLower == QStringLiteral("ipv4"))
        || (familyLower == QStringLiteral("ipv6"))) {
        bool shouldLogResolution = false;
        {
            QMutexLocker lock(&ses.mutex);
            if (!ses.loggedResolutionKeys.contains(sshResolutionKey)) {
                ses.loggedResolutionKeys.insert(sshResolutionKey);
                shouldLogResolution = true;
            }
        }
        if (shouldLogResolution) {
            const QHostInfo resolved = QHostInfo::fromName(p.host);
            if (resolved.error() != QHostInfo::NoError) {
                const QString msg = QStringLiteral("Resolucion SSH %1 (%2): %3")
                                        .arg(p.host,
                                             familyLower.isEmpty() ? QStringLiteral("auto") : familyLower,
                                             resolved.errorString());
                ses.log(TransportSession::Nivel::Warn, QStringLiteral("%1: %2").arg(p.name, msg));
                ses.logConn(TransportSession::Nivel::Normal, p.id, msg);
            } else {
                QStringList addresses;
                for (const QHostAddress& address : resolved.addresses()) {
                    addresses << describeHostAddress(address);
                }
                const QString msg = QStringLiteral("Resolucion SSH %1 (%2): %3")
                                        .arg(p.host,
                                             familyLower.isEmpty() ? QStringLiteral("auto") : familyLower,
                                             addresses.isEmpty() ? QStringLiteral("sin direcciones") : addresses.join(QStringLiteral(", ")));
                ses.log(TransportSession::Nivel::Info, QStringLiteral("%1: %2").arg(p.name, msg));
                ses.logConn(TransportSession::Nivel::Normal, p.id, msg);
            }
        }
    }

    // El multiplexado no funciona cuando la aplicación corre en Windows: su OpenSSH
    // responde `getsockname failed: Not a socket` (comprobado contra fc16 desde una VM
    // Windows 11), y `ControlPersist` deja un proceso maestro de fondo que no suelta
    // las tuberías heredadas, así que waitForFinished no vuelve y la ventana se queda
    // bloqueada. El reintento sin multiplexar no salvaba nada: para llegar a él hay
    // que esperar primero a que el intento colgado agote su plazo, y son ~16 órdenes
    // por refresco.
#ifdef Q_OS_WIN
    const bool allowMultiplexing = false;
#else
    bool allowMultiplexing = true;
    {
        QMutexLocker lock(&ses.mutex);
        allowMultiplexing = !ses.disableMultiplexKeys.contains(sshConnKey);
    }
#endif
    bool startedOk = runSshAttempt(allowMultiplexing, out, err, rc);
    if (allowMultiplexing && startedOk && rc != 0 && shouldRetrySshWithoutMultiplexing(err)) {
        {
            QMutexLocker lock(&ses.mutex);
            ses.disableMultiplexKeys.insert(sshConnKey);
        }
        const QString retryMsg = QStringLiteral("SSH multiplexado falló; reintentando sin ControlMaster/ControlPath.");
        ses.log(TransportSession::Nivel::Warn, QStringLiteral("%1: %2").arg(p.name, retryMsg));
        ses.logConn(TransportSession::Nivel::Normal, p.id, retryMsg);
        startedOk = runSshAttempt(false, out, err, rc);
    } else if (!allowMultiplexing) {
        ses.logConn(TransportSession::Nivel::Normal, p.id, QStringLiteral("SSH multiplexado deshabilitado para esta conexión en la sesión actual."));
    }

    if (!startedOk) {
        return false;
    }
    if (isWindowsConnection(p)) {
        out = sanitizeWindowsCliXml(out);
        err = sanitizeWindowsCliXml(err);
    }
    if (!out.trimmed().isEmpty()) {
        if (echoOutputToLog) {
            ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(out));
        }
    }
    if (!err.trimmed().isEmpty()) {
        if (echoOutputToLog) {
            ses.logConn(TransportSession::Nivel::Normal, p.id, oneLine(err));
        }
    }
    return true;
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
    const QString key = remoteDaemonTlsCacheKey(p);
    // Close the SSH tunnel so a fresh one is opened after daemon restart.
    QPointer<QProcess> proc;
    {
        QMutexLocker lock(&m_transport.mutex);
        const auto it = m_transport.tunnelsByConnKey.find(key);
        if (it != m_transport.tunnelsByConnKey.end()) {
            proc = it->process;
            m_transport.tunnelsByConnKey.erase(it);
        }
        // Clear backoff so the first RPC after restart is not suppressed.
        m_transport.retryAfterByConnKey.remove(key);
        m_transport.retryReasonByConnKey.remove(key);
    }
    if (proc && proc->state() != QProcess::NotRunning) {
        proc->terminate();
        if (!proc->waitForFinished(700)) {
            proc->kill();
            proc->waitForFinished(700);
        }
    }
    if (proc) {
        proc->deleteLater();
    }
    // Evict in-memory TLS cache so next fetch reads the new certs.
    {
        QMutexLocker lock(&s_remoteDaemonTlsCacheMutex);
        s_remoteDaemonTlsCache.remove(key);
    }
}

void MainWindow::clearDaemonRpcBackoffForConnection(const ConnectionProfile& p) {
    const QString key = remoteDaemonTlsCacheKey(p);
    QMutexLocker lock(&m_transport.mutex);
    m_transport.retryAfterByConnKey.remove(key);
    m_transport.retryReasonByConnKey.remove(key);
}

void MainWindow::closeAllRemoteDaemonRpcTunnels() {
    QVector<QPointer<QProcess>> procs;
    {
        QMutexLocker lock(&m_transport.mutex);
        for (auto it = m_transport.tunnelsByConnKey.cbegin();
             it != m_transport.tunnelsByConnKey.cend(); ++it) {
            if (it.value().process) {
                procs.push_back(it.value().process);
            }
        }
        m_transport.tunnelsByConnKey.clear();
    }
    for (const QPointer<QProcess>& proc : procs) {
        if (!proc) {
            continue;
        }
        if (proc->state() != QProcess::NotRunning) {
            proc->terminate();
            if (!proc->waitForFinished(700)) {
                proc->kill();
                proc->waitForFinished(700);
            }
        }
        proc->deleteLater();
    }
}

QString MainWindow::daemonRpcBackoffTextForConnection(const ConnectionProfile& p) const {
    const QString key = remoteDaemonTlsCacheKey(p);
    QMutexLocker lock(&m_transport.mutex);
    const auto retryIt = m_transport.retryAfterByConnKey.constFind(key);
    if (retryIt == m_transport.retryAfterByConnKey.constEnd() || !retryIt.value().isValid()) {
        return QString();
    }
    const qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(retryIt.value());
    if (seconds <= 0) {
        return QString();
    }
    const QString reason = m_transport.retryReasonByConnKey.value(key).trimmed();
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
    return p.connType.compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0;
}

bool MainWindow::isLocalConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return false;
    }
    return isLocalConnection(m_conns.profiles[connIdx]);
}

bool transport::isWindowsConnection(const ConnectionProfile& p) {
    return mwhelpers::isWindowsOsType(p.osType);
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
    if (!isWindowsConnection(p)) {
        return remoteCmd;
    }
    QString trimmed = remoteCmd.trimmed();
    // Los comandos clásicos llegan envueltos por withUnixSearchPathCommand, que antepone
    // un "PATH=...; export PATH; " de sintaxis Unix. Eso existía para el bash de MSYS2;
    // en PowerShell es un error de sintaxis. Se retira aquí, en un único punto, en vez
    // de en la treintena de sitios que lo aplican: el prólogo de abajo ya pone las rutas
    // de OpenZFS en $env:Path, que es lo único que ese prefijo aportaba.
    //
    // Comprobado contra un Windows 11 real: "zfs version" y "zpool list -H -p -o ..."
    // se ejecutan así sin ninguna capa Unix de por medio.
    static const QRegularExpression unixPathPrefix(
        QStringLiteral("^PATH=\"[^\"]*\";\\s*export PATH;\\s*"));
    trimmed.remove(unixPathPrefix);

    QString script = QStringLiteral(
        "$ProgressPreference='SilentlyContinue'; "
        "$InformationPreference='SilentlyContinue'; "
        "$WarningPreference='Continue'; "
        "$zfsPaths=@("
        "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin',"
        "'C:\\\\Program Files\\\\OpenZFS On Windows'"
        "); "
        "foreach($p in $zfsPaths){ "
        "  if(Test-Path -LiteralPath $p){ "
        "    if(-not (($env:Path -split ';') -contains $p)){ $env:Path = $p + ';' + $env:Path } "
        "  } "
        "}; ");
    script += trimmed;

    const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
    const QString b64 = QString::fromLatin1(utf16.toBase64());
    // La línea de comandos de cmd.exe se agota con payloads muy grandes en ejecución
    // local. Por SSH no se usa -Command: el shell remoto expandiría las variables de
    // PowerShell (p. ej. "$p") y rompería los foreach.
    if (isLocalConnection(p) && b64.size() > 7000) {
        QString inlineScript = script;
        inlineScript.replace(QStringLiteral("\""), QStringLiteral("`\""));
        return QStringLiteral("powershell -NoProfile -NonInteractive -Command \"& { %1 }\"")
            .arg(inlineScript);
    }
    return QStringLiteral("powershell -NoProfile -NonInteractive -EncodedCommand %1").arg(b64);
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
            QJsonDocument doc = QJsonDocument::fromJson(jsonPayload.toUtf8(), &parseErr);
            if (parseErr.error != QJsonParseError::NoError) {
                const int lastBrace = jsonPayload.lastIndexOf(QLatin1Char('}'));
                if (lastBrace > 0) {
                    jsonPayload = jsonPayload.left(lastBrace + 1);
                    parseErr = QJsonParseError{};
                    doc = QJsonDocument::fromJson(jsonPayload.toUtf8(), &parseErr);
                }
            }
            if (parseErr.error != QJsonParseError::NoError) {
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
            QStringLiteral("zfs get -H -o name,value -r driveletter %1").arg(shSingleQuote(poolName)));
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
                cache.driveletterByDataset[ds] = drive;
                if (!drive.isEmpty()) {
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
