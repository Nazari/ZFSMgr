#include "mainwindow.h"
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
#include <QMutexLocker>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* kDefaultAgentConfigPath = "/etc/zfsmgr/agent.conf";
constexpr const char* kDefaultAgentTlsCertPath = "/etc/zfsmgr/tls/server.crt";
constexpr const char* kDefaultAgentTlsClientCertPath = "/etc/zfsmgr/tls/client.crt";
constexpr const char* kDefaultAgentTlsClientKeyPath = "/etc/zfsmgr/tls/client.key";

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

// Parser POSIX mínimo: entiende '...' y "..." y el patrón '"'"' de shSingleQuote.
// QProcess::splitCommand solo maneja "..." (no '...') y produce resultados incorrectos
// cuando los args vienen de shSingleQuote embebido en otro shSingleQuote.
static QStringList posixShellSplitArgs(const QString& s) {
    QStringList result;
    QString current;
    bool inSQ = false, inDQ = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inSQ) {
            if (c == QLatin1Char('\'')) { inSQ = false; }
            else { current += c; }
        } else if (inDQ) {
            if (c == QLatin1Char('"')) { inDQ = false; }
            else if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                const QChar next = s.at(++i);
                if (next == QLatin1Char('"') || next == QLatin1Char('\\')
                    || next == QLatin1Char('$') || next == QLatin1Char('`')) {
                    current += next;
                } else {
                    current += c;
                    current += next;
                }
            } else {
                current += c;
            }
        } else {
            if (c == QLatin1Char('\'')) { inSQ = true; }
            else if (c == QLatin1Char('"')) { inDQ = true; }
            else if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                current += s.at(++i);
            } else if (c.isSpace()) {
                if (!current.isEmpty()) { result += current; current.clear(); }
            } else {
                current += c;
            }
        }
    }
    if (!current.isEmpty()) { result += current; }
    return result;
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
           || cmd == QStringLiteral("--job-cancel");
}

bool extractLocalAgentArgs(const QString& remoteCmd, QStringList& argsOut) {
    argsOut.clear();
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
    // Cortamos en el primer separador de shell no entrecomillado
    const int sep = tail.indexOf(QRegularExpression(QStringLiteral("[;&|\\n\\r]")));
    if (sep >= 0) {
        tail = tail.left(sep).trimmed();
    }
    // Los args vienen del comando construido con shSingleQuote() luego envuelto en otro
    // shSingleQuote() para el argumento de `sh -c '...'`.  El patrón '"'"' representa un
    // carácter ' escapado en ese doble-envoltorio.  Reemplazarlo antes de parsear evita
    // que el parser interprete los " como delimitadores adicionales de cita.
    tail.replace(QStringLiteral("'\"'\"'"), QStringLiteral("'"));
    if (tail.isEmpty()) {
        return false;
    }
    const QStringList parsed = posixShellSplitArgs(tail);
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

bool tryRunLocalAgentRpc(const QStringList& agentArgs,
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
    const LocalAgentConfig cfg = loadLocalAgentConfig();

    QFile caFile(cfg.tlsCertPath);
    QFile clientCertFile(cfg.tlsClientCertPath);
    QFile clientKeyFile(cfg.tlsClientKeyPath);
    QList<QSslCertificate> caCerts;
    QList<QSslCertificate> clientCerts;
    QSslKey clientKey;
    if (caFile.open(QIODevice::ReadOnly)) {
        caCerts = QSslCertificate::fromData(caFile.readAll(), QSsl::Pem);
    }
    if (clientCertFile.open(QIODevice::ReadOnly)) {
        clientCerts = QSslCertificate::fromData(clientCertFile.readAll(), QSsl::Pem);
    }
    if (clientKeyFile.open(QIODevice::ReadOnly)) {
        const QByteArray keyPem = clientKeyFile.readAll();
        clientKey = QSslKey(keyPem, QSsl::Rsa, QSsl::Pem);
        if (clientKey.isNull()) {
            clientKey = QSslKey(keyPem, QSsl::Ec, QSsl::Pem);
        }
    }
    if (caCerts.isEmpty() || clientCerts.isEmpty() || clientKey.isNull()) {
        return false;
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
    const int ioTimeout = qBound(800, timeoutMs > 0 ? timeoutMs : 30000, 70000);
    QElapsedTimer rpcTimer;
    rpcTimer.start();
    qDebug("[agent-rpc] cmd=%s port=%d connectTimeout=%d", qPrintable(cmd), cfg.port, connectTimeout);
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
            qDebug("[agent-rpc] peerName=%s FAILED in %lld ms (err: %s)",
                   qPrintable(peerName), rpcTimer.elapsed() - t0,
                   qPrintable(sock.errorString()));
            continue;
        }
        if (!peerCertificateIsPinned(sock, caCerts)) {
            qDebug("[agent-rpc] peerName=%s certificado del daemon NO coincide con el fijado",
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
        while (timer.elapsed() < ioTimeout) {
            if (!sock.waitForReadyRead(300)) {
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
            qDebug("[agent-rpc] cmd=%s OK via peerName=%s total=%lld ms",
                   qPrintable(cmd), qPrintable(peerName), rpcTimer.elapsed());
            return true;
        }
    }
    qDebug("[agent-rpc] cmd=%s FAILED total=%lld ms", qPrintable(cmd), rpcTimer.elapsed());
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
    args << mwhelpers::sshUserHost(p) << remoteCmd;

    QProcess proc;
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
                                       const QString& agentCommandLine,
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

    const QString marker = QStringLiteral("/usr/local/libexec/zfsmgr-agent");
    const int pos = agentCommandLine.indexOf(marker);
    if (pos < 0) {
        return false;
    }
    // Insert --job-submit right after the binary path, leaving the original command
    // and its arguments as the job payload.
    const QString head = agentCommandLine.left(pos + marker.size());
    const QString tail = agentCommandLine.mid(pos + marker.size());
    const QString submitCmd = head + QStringLiteral(" --job-submit") + tail;

    QString subOut;
    QString subErr;
    int subRc = -1;
    if (!runSsh(p, withSudo(p, mwhelpers::withUnixSearchPathCommand(submitCmd)), 20000, subOut, subErr, subRc)
        || subRc != 0) {
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
    appLog(QStringLiteral("INFO"),
           QStringLiteral("%1: trabajo %2 en curso en el daemon").arg(p.name, jobId));

    const QString statusCmd =
        head + QStringLiteral(" --job-status ") + mwhelpers::shSingleQuote(jobId);
    QString lastProgress;
    // No overall deadline on purpose: the daemon owns the operation and reports when
    // it is done. Each individual poll is short, so a dead daemon still surfaces.
    while (true) {
        QThread::msleep(1000);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        QString stOut;
        QString stErr;
        int stRc = -1;
        if (!runSsh(p, withSudo(p, mwhelpers::withUnixSearchPathCommand(statusCmd)), 20000, stOut, stErr, stRc)
            || stRc != 0) {
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

bool MainWindow::tryRunRemoteAgentRpcViaTunnel(const ConnectionProfile& p,
                                               const QStringList& agentArgs,
                                               int timeoutMs,
                                               QString& out,
                                               QString& err,
                                               int& rc,
                                               QString* failureReason,
                                               bool* commandMayHaveRunOut) {
    // Este camino mantiene/crea QProcess asociados a estado de MainWindow.
    // Si se ejecuta desde un worker de QtConcurrent puede crear QObject hijos
    // con parent en otro hilo (warning/crash de afinidad).
    if (failureReason) {
        failureReason->clear();
    }
    if (commandMayHaveRunOut) {
        *commandMayHaveRunOut = false;
    }
    if (QThread::currentThread() != thread()) {
        if (failureReason) {
            *failureReason = QStringLiteral("rpc invocado fuera del hilo UI");
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
    const auto closeTunnelForKey = [&](const QString& key) {
        QPointer<QProcess> proc;
        {
            QMutexLocker lock(&m_sshRuntimeSetsMutex);
            const auto it = m_remoteDaemonRpcTunnelsByConnKey.find(key);
            if (it == m_remoteDaemonRpcTunnelsByConnKey.end()) {
                return;
            }
            proc = it->process;
            m_remoteDaemonRpcTunnelsByConnKey.erase(it);
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
            QMutexLocker lock(&m_sshRuntimeSetsMutex);
            for (auto it = m_remoteDaemonRpcTunnelsByConnKey.cbegin();
                 it != m_remoteDaemonRpcTunnelsByConnKey.cend(); ++it) {
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
        const QDateTime now = QDateTime::currentDateTimeUtc();
        bool needsRecreate = false;
        {
            QMutexLocker lock(&m_sshRuntimeSetsMutex);
            const auto it = m_remoteDaemonRpcTunnelsByConnKey.find(key);
            if (it != m_remoteDaemonRpcTunnelsByConnKey.end()) {
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

        QProcess* proc = new QProcess(this);
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
            while (readyTimer.elapsed() < 5000) {
                if (proc->state() == QProcess::NotRunning) {
                    break;
                }
                QTcpSocket probe;
                probe.connectToHost(QHostAddress::LocalHost, localPort);
                if (probe.waitForConnected(200)) {
                    probe.abort();
                    tunnelReady = true;
                    break;
                }
                QThread::msleep(50);
            }
            if (!tunnelReady) {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("daemon-rpc: el túnel SSH a %1 no aceptó conexiones en 5 s")
                           .arg(p.name));
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

        QObject::connect(
            proc,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, key, proc](int, QProcess::ExitStatus) {
                QMutexLocker lock(&m_sshRuntimeSetsMutex);
                auto it = m_remoteDaemonRpcTunnelsByConnKey.find(key);
                if (it != m_remoteDaemonRpcTunnelsByConnKey.end() && it->process == proc) {
                    m_remoteDaemonRpcTunnelsByConnKey.erase(it);
                }
                proc->deleteLater();
            });

        {
            QMutexLocker lock(&m_sshRuntimeSetsMutex);
            RemoteRpcTunnelState st;
            st.process = proc;
            st.localPort = localPort;
            st.remotePort = remotePort;
            st.startedAtUtc = now;
            st.lastUsedUtc = now;
            m_remoteDaemonRpcTunnelsByConnKey.insert(key, st);
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
            if (!persistDaemonTlsMaterialForConnection(
                    p, serverCertPem, clientCertPem, clientKeyPem, daemonPort, &persistErr)) {
                appLog(QStringLiteral("WARN"),
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
        std::call_once(tlsBackendLogOnce, [this]() {
            // Preferir OpenSSL cuando esté disponible, para que todas las
            // plataformas se comporten igual. En macOS, SecureTransport además
            // exige la clave privada del cliente en el llavero y hace que el
            // sistema pida la contraseña al usuario en cada arranque.
            const QString before = QSslSocket::activeBackend();
            if (before != QStringLiteral("openssl")
                && QSslSocket::availableBackends().contains(QStringLiteral("openssl"))) {
                if (QSslSocket::setActiveBackend(QStringLiteral("openssl"))) {
                    appLog(QStringLiteral("INFO"),
                           QStringLiteral("TLS: backend cambiado de %1 a openssl").arg(before));
                }
            }
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("TLS: backend activo=%1 disponibles=[%2] libssl=%3")
                       .arg(QSslSocket::activeBackend(),
                            QSslSocket::availableBackends().join(QStringLiteral(", ")),
                            QSslSocket::sslLibraryVersionString()));
        });

        const int connectTimeout = qBound(600, timeoutMs > 0 ? timeoutMs / 5 : 1200, 3500);
        const int ioTimeout = qBound(1000, timeoutMs > 0 ? timeoutMs : 30000, 70000);
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
            while (timer.elapsed() < ioTimeout) {
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
                    QMutexLocker lock(&m_sshRuntimeSetsMutex);
                    auto it = m_remoteDaemonRpcTunnelsByConnKey.find(rpcConnKey);
                    if (it != m_remoteDaemonRpcTunnelsByConnKey.end()) {
                        it->lastUsedUtc = QDateTime::currentDateTimeUtc();
                    }
                }
                return true;
            }
            if (lastAttemptReason.isEmpty()) {
                lastAttemptReason = QStringLiteral("timeout esperando respuesta RPC");
            }
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
            appLog(QStringLiteral("INFO"),
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
    for (const ConnectionProfile& cp : std::as_const(m_profiles)) {
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
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id.trimmed().compare(connId, Qt::CaseInsensitive) == 0) {
            m_profiles[i].daemonTlsServerCertPem = updated.daemonTlsServerCertPem;
            m_profiles[i].daemonTlsClientCertPem = updated.daemonTlsClientCertPem;
            m_profiles[i].daemonTlsClientKeyPem = updated.daemonTlsClientKeyPem;
            m_profiles[i].daemonTlsPort = updated.daemonTlsPort;
            break;
        }
    }

    QString storeErr;
    {
        QMutexLocker lock(&s_remoteDaemonTlsPersistMutex);
        if (!m_store.upsertConnection(updated, storeErr)) {
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

bool MainWindow::runSsh(const ConnectionProfile& p,
                        const QString& remoteCmd,
                        int timeoutMs,
                        QString& out,
                        QString& err,
                        int& rc,
                        const std::function<void(const QString&)>& onStdoutLine,
                        const std::function<void(const QString&)>& onStderrLine,
                        const std::function<void(int)>& onIdleTimeoutRemaining,
                        const QByteArray& stdinPayload) {
    out.clear();
    err.clear();
    rc = -1;

    if (isLocalConnection(p)) {
        const QString localCmd = remoteCmd.trimmed();
        const QString cmdLine = QStringLiteral("[local] $ %1").arg(localCmd);
        appLog(QStringLiteral("INFO"), cmdLine);
        appendConnectionLog(p.id, cmdLine);

        // stdin no vacío descarta el RPC: el canal no transporta stdin (el daemon lo
        // dice en runExecCaptureWithStdin) y la intercepción no lo miraba, así que la
        // passphrase de un dataset cifrado se perdía en silencio al desglosarlo.
        QStringList localAgentArgs;
        if (stdinPayload.isEmpty() && extractLocalAgentArgs(localCmd, localAgentArgs)) {
            if (tryRunLocalAgentRpc(localAgentArgs, timeoutMs, out, err, rc)) {
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
                        appendConnectionLog(p.id, line);
                    }
                };
                emitLines(out, onStdoutLine);
                emitLines(err, onStderrLine);
                if (!out.trimmed().isEmpty()) {
                    appendConnectionLog(p.id, oneLine(out));
                }
                if (!err.trimmed().isEmpty()) {
                    appendConnectionLog(p.id, oneLine(err));
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
        args << "-c" << localCmd;
#endif
        QElapsedTimer timer;
        timer.start();
        proc.start(program, args);
        if (!proc.waitForStarted(4000)) {
            err = QStringLiteral("No se pudo iniciar %1").arg(program);
            appendConnectionLog(p.id, err);
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
                appendConnectionLog(p.id, line);
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
            if (QThread::currentThread() == thread()) {
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
            appendConnectionLog(p.id, line);
        }
        if (!errLineBuf.trimmed().isEmpty()) {
            const QString line = errLineBuf.trimmed();
            if (onStderrLine) {
                onStderrLine(line);
            }
            appendConnectionLog(p.id, line);
        }
        if (timedOut) {
            rc = -1;
            err = QStringLiteral("Timeout");
            appendConnectionLog(p.id, err);
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
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("%1: verificación de host SSH fallida").arg(p.name));
            }
        }
        if (!out.trimmed().isEmpty()) {
            appendConnectionLog(p.id, oneLine(out));
        }
        if (!err.trimmed().isEmpty()) {
            appendConnectionLog(p.id, oneLine(err));
        }
        return true;
    }

    // Windows entra por RPC como cualquier otro sistema: el daemon nativo sirve TLS por
    // el mismo túnel "ssh -L", verificado contra un Windows 11 real ejecutando ZFS.
    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0) {
        // Ver la nota de la rama local: stdin no vacío hace el RPC inelegible.
        QStringList agentArgs;
        if (stdinPayload.isEmpty() && extractLocalAgentArgs(remoteCmd.trimmed(), agentArgs)) {
            const QString rpcConnKey = remoteDaemonTlsCacheKey(p);
            bool allowRpcAttempt = true;
            QString suppressedReason;
            {
                QMutexLocker lock(&m_sshRuntimeSetsMutex);
                const auto it = m_daemonRpcRetryAfterByConnKey.constFind(rpcConnKey);
                if (it != m_daemonRpcRetryAfterByConnKey.constEnd()
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
                if (QThread::currentThread() == thread()) {
                    rpcAttemptOk = tryRunRemoteAgentRpcViaTunnel(
                        p, agentArgs, timeoutMs, out, err, rc, &rpcFailureReason, &rpcCommandMayHaveRun);
                } else {
                    QMetaObject::invokeMethod(
                        this,
                        [this, &p, &agentArgs, timeoutMs, &out, &err, &rc, &rpcAttemptOk, &rpcFailureReason,
                         &rpcCommandMayHaveRun]() {
                            rpcAttemptOk = tryRunRemoteAgentRpcViaTunnel(
                                p, agentArgs, timeoutMs, out, err, rc, &rpcFailureReason,
                                &rpcCommandMayHaveRun);
                        },
                        Qt::BlockingQueuedConnection);
                }
            }
            if (rpcAttemptOk) {
                {
                    QMutexLocker lock(&m_sshRuntimeSetsMutex);
                    m_daemonRpcRetryAfterByConnKey.remove(rpcConnKey);
                    m_daemonRpcRetryReasonByConnKey.remove(rpcConnKey);
                }
                const QString cmdLine = QStringLiteral("%1 $ [daemon-rpc] %2")
                                            .arg(sshUserHostPort(p), agentArgs.join(QLatin1Char(' ')));
                appLog(QStringLiteral("INFO"), cmdLine);
                appendConnectionLog(p.id, cmdLine);
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
                        appendConnectionLog(p.id, line);
                    }
                };
                emitLines(out, onStdoutLine);
                emitLines(err, onStderrLine);
                if (!out.trimmed().isEmpty()) {
                    appendConnectionLog(p.id, oneLine(out));
                }
                if (!err.trimmed().isEmpty()) {
                    appendConnectionLog(p.id, oneLine(err));
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
                        .arg(sshUserHostPort(p), agentArgs.join(QLatin1Char(' ')), reason);
                appLog(QStringLiteral("ERROR"), abortLine);
                appendConnectionLog(p.id, abortLine);
                out.clear();
                err = QStringLiteral(
                          "La orden se envió al daemon pero no se recibió respuesta (%1).\n"
                          "Puede seguir ejecutándose en el equipo remoto, así que ZFSMgr no la "
                          "reintenta automáticamente.\nCompruebe el estado antes de repetirla.")
                          .arg(reason);
                rc = 124;
                return true;
            } else if (allowRpcAttempt) {
                const QString reason = rpcFailureReason.trimmed().isEmpty()
                                           ? QStringLiteral("motivo no especificado")
                                           : rpcFailureReason.trimmed();
                const QString fallbackLine =
                    QStringLiteral("%1 $ [daemon-rpc:fallback] %2 -> %3")
                        .arg(sshUserHostPort(p), agentArgs.join(QLatin1Char(' ')), reason);
                appLog(QStringLiteral("INFO"), fallbackLine);
                appendConnectionLog(p.id, fallbackLine);
                QMutexLocker lock(&m_sshRuntimeSetsMutex);
                constexpr int kDaemonRpcRetryBackoffSec = 30;
                m_daemonRpcRetryAfterByConnKey.insert(
                    rpcConnKey, QDateTime::currentDateTimeUtc().addSecs(kDaemonRpcRetryBackoffSec));
                m_daemonRpcRetryReasonByConnKey.insert(rpcConnKey, reason);
            } else if (!suppressedReason.isEmpty()) {
                const QString skippedLine =
                    QStringLiteral("%1 $ [daemon-rpc:skip] %2 -> %3")
                        .arg(sshUserHostPort(p), agentArgs.join(QLatin1Char(' ')), suppressedReason);
                appLog(QStringLiteral("INFO"), skippedLine);
                appendConnectionLog(p.id, skippedLine);
            }
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
    appLog(QStringLiteral("INFO"), cmdLine);
    appendConnectionLog(p.id, cmdLine);
    if (hasPassword && !usingSshpass) {
        appendConnectionLog(p.id, QStringLiteral("Password guardado, pero sshpass no está disponible; se usará SSH no interactivo."));
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
        args << wrappedCmd;

        QProcess proc;
        QElapsedTimer timer;
        timer.start();
        proc.start(program, args);
        if (!proc.waitForStarted(4000)) {
            attemptErr = QStringLiteral("No se pudo iniciar %1").arg(program);
            appendConnectionLog(p.id, attemptErr);
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
                appendConnectionLog(p.id, line);
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
            if (QThread::currentThread() == thread()) {
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
            appendConnectionLog(p.id, line);
        }
        if (!errLineBuf.trimmed().isEmpty()) {
            const QString line = errLineBuf.trimmed();
            if (onStderrLine) {
                onStderrLine(line);
            }
            appendConnectionLog(p.id, line);
        }

        if (timedOut) {
            attemptRc = -1;
            attemptErr = QStringLiteral("Timeout");
            appendConnectionLog(p.id, attemptErr);
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
            QMutexLocker lock(&m_sshRuntimeSetsMutex);
            if (!m_loggedSshResolutionKeys.contains(sshResolutionKey)) {
                m_loggedSshResolutionKeys.insert(sshResolutionKey);
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
                appLog(QStringLiteral("WARN"), QStringLiteral("%1: %2").arg(p.name, msg));
                appendConnectionLog(p.id, msg);
            } else {
                QStringList addresses;
                for (const QHostAddress& address : resolved.addresses()) {
                    addresses << describeHostAddress(address);
                }
                const QString msg = QStringLiteral("Resolucion SSH %1 (%2): %3")
                                        .arg(p.host,
                                             familyLower.isEmpty() ? QStringLiteral("auto") : familyLower,
                                             addresses.isEmpty() ? QStringLiteral("sin direcciones") : addresses.join(QStringLiteral(", ")));
                appLog(QStringLiteral("INFO"), QStringLiteral("%1: %2").arg(p.name, msg));
                appendConnectionLog(p.id, msg);
            }
        }
    }

    bool allowMultiplexing = true;
    {
        QMutexLocker lock(&m_sshRuntimeSetsMutex);
        allowMultiplexing = !m_sshDisableMultiplexKeys.contains(sshConnKey);
    }
    bool startedOk = runSshAttempt(allowMultiplexing, out, err, rc);
    if (allowMultiplexing && startedOk && rc != 0 && shouldRetrySshWithoutMultiplexing(err)) {
        {
            QMutexLocker lock(&m_sshRuntimeSetsMutex);
            m_sshDisableMultiplexKeys.insert(sshConnKey);
        }
        const QString retryMsg = QStringLiteral("SSH multiplexado falló; reintentando sin ControlMaster/ControlPath.");
        appLog(QStringLiteral("WARN"), QStringLiteral("%1: %2").arg(p.name, retryMsg));
        appendConnectionLog(p.id, retryMsg);
        startedOk = runSshAttempt(false, out, err, rc);
    } else if (!allowMultiplexing) {
        appendConnectionLog(p.id, QStringLiteral("SSH multiplexado deshabilitado para esta conexión en la sesión actual."));
    }

    if (!startedOk) {
        return false;
    }
    if (isWindowsConnection(p)) {
        out = sanitizeWindowsCliXml(out);
        err = sanitizeWindowsCliXml(err);
    }
    if (!out.trimmed().isEmpty()) {
        appendConnectionLog(p.id, oneLine(out));
    }
    if (!err.trimmed().isEmpty()) {
        appendConnectionLog(p.id, oneLine(err));
    }
    return true;
}

void MainWindow::closeAllSshControlMasters() {
    if (m_profiles.isEmpty()) {
        return;
    }
    QSet<QString> seen;
    for (const ConnectionProfile& p : m_profiles) {
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
        QMutexLocker lock(&m_sshRuntimeSetsMutex);
        const auto it = m_remoteDaemonRpcTunnelsByConnKey.find(key);
        if (it != m_remoteDaemonRpcTunnelsByConnKey.end()) {
            proc = it->process;
            m_remoteDaemonRpcTunnelsByConnKey.erase(it);
        }
        // Clear backoff so the first RPC after restart is not suppressed.
        m_daemonRpcRetryAfterByConnKey.remove(key);
        m_daemonRpcRetryReasonByConnKey.remove(key);
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
    QMutexLocker lock(&m_sshRuntimeSetsMutex);
    m_daemonRpcRetryAfterByConnKey.remove(key);
    m_daemonRpcRetryReasonByConnKey.remove(key);
}

void MainWindow::closeAllRemoteDaemonRpcTunnels() {
    QVector<QPointer<QProcess>> procs;
    {
        QMutexLocker lock(&m_sshRuntimeSetsMutex);
        for (auto it = m_remoteDaemonRpcTunnelsByConnKey.cbegin();
             it != m_remoteDaemonRpcTunnelsByConnKey.cend(); ++it) {
            if (it.value().process) {
                procs.push_back(it.value().process);
            }
        }
        m_remoteDaemonRpcTunnelsByConnKey.clear();
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
    QMutexLocker lock(&m_sshRuntimeSetsMutex);
    const auto retryIt = m_daemonRpcRetryAfterByConnKey.constFind(key);
    if (retryIt == m_daemonRpcRetryAfterByConnKey.constEnd() || !retryIt.value().isValid()) {
        return QString();
    }
    const qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(retryIt.value());
    if (seconds <= 0) {
        return QString();
    }
    const QString reason = m_daemonRpcRetryReasonByConnKey.value(key).trimmed();
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
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return QString();
    }
    return daemonRpcBackoffTextForConnection(m_profiles[connIdx]);
}

QString MainWindow::withSudo(const ConnectionProfile& p, const QString& cmd) const {
    return mwhelpers::withSudoCommand(p, cmd);
}

QString MainWindow::withSudoStreamInput(const ConnectionProfile& p, const QString& cmd) const {
    return mwhelpers::withSudoStreamInputCommand(p, cmd);
}

bool MainWindow::isLocalConnection(const ConnectionProfile& p) const {
    return p.connType.compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0;
}

bool MainWindow::isLocalConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    return isLocalConnection(m_profiles[connIdx]);
}

bool MainWindow::isWindowsConnection(const ConnectionProfile& p) const {
    return mwhelpers::isWindowsOsType(p.osType);
}

bool MainWindow::isWindowsConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    return isWindowsConnection(m_profiles[connIdx]);
}

bool MainWindow::supportsAlternateDatasetMount(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    const ConnectionProfile p = m_profiles[connIdx];
    if (isWindowsConnection(p)) {
        return false;
    }
    const QString os = p.osType.trimmed().toLower();
    return os.contains(QStringLiteral("linux"))
           || os.contains(QStringLiteral("freebsd"))
           || os.contains(QStringLiteral("macos"))
           || os.contains(QStringLiteral("darwin"))
           || os.contains(QStringLiteral("os x"));
}

QString MainWindow::wrapRemoteCommand(const ConnectionProfile& p,
                                      const QString& remoteCmd) const {
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
    if (connIdx < 0 || connIdx >= m_profiles.size() || dataset.isEmpty() || prop.isEmpty()) {
        return false;
    }
    const ConnectionProfile p = m_profiles[connIdx];
    const bool daemonReadApiOk =
        !isWindowsConnection(p)
        && connIdx >= 0
        && connIdx < m_states.size()
        && m_states[connIdx].daemonInstalled
        && m_states[connIdx].daemonActive
        && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    QString cmdClassic =
        QStringLiteral("zfs get -H -o value %1 %2").arg(shSingleQuote(prop), shSingleQuote(dataset));
    if (!isWindowsConnection(p)) {
        cmdClassic = mwhelpers::withUnixSearchPathCommand(cmdClassic);
    }
    const QString cmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(
               QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-get-prop %1 %2")
                   .arg(shSingleQuote(prop), shSingleQuote(dataset))));
    const QString cmd = daemonReadApiOk ? cmdDaemon : withSudo(p, cmdClassic);
    QString out;
    QString err;
    int rc = -1;
    bool ok = runSsh(p, cmd, 15000, out, err, rc) && rc == 0;
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
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
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
        // m_poolDatasetCache, leaving the iterator dangling.
        auto cacheIt = m_poolDatasetCache.find(key);
        if (cacheIt == m_poolDatasetCache.end()) {
            return false;
        }
        guid = cacheIt->objectGuidByName.value(trimmedObject).trimmed();
    }
    if (guid.isEmpty() || guid == QStringLiteral("-")) {
        // Copy, not a reference: m_profiles is reassigned wholesale by
        // loadConnections(), which a queued event can trigger during runSsh().
        const ConnectionProfile p = m_profiles[connIdx];
        QString out;
        QString err;
        int rc = -1;
        const bool daemonReadApiOk =
            !isWindowsConnection(p)
            && connIdx >= 0
            && connIdx < m_states.size()
            && m_states[connIdx].daemonInstalled
            && m_states[connIdx].daemonActive
            && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
        QString guidCmdClassic =
            QStringLiteral("zfs get -H -o value guid %1").arg(shSingleQuote(trimmedObject));
        if (!isWindowsConnection(connIdx)) {
            guidCmdClassic = mwhelpers::withUnixSearchPathCommand(guidCmdClassic);
        }
        const QString guidCmdDaemon = withSudo(
            p, mwhelpers::withUnixSearchPathCommand(
                   QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-get-prop guid %1")
                       .arg(shSingleQuote(trimmedObject))));
        const QString guidCmd = daemonReadApiOk ? guidCmdDaemon : withSudo(p, guidCmdClassic);
        bool ok = runSsh(p, guidCmd, 15000, out, err, rc) && rc == 0;
        if (!ok) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("No se pudo cargar GUID de objeto %1::%2/%3 -> %4")
                       .arg(p.name, trimmedPool, trimmedObject, oneLine(err.isEmpty() ? out : err)));
            return false;
        }
        guid = out.section('\n', 0, 0).trimmed();
        if (guid.isEmpty() || guid == QStringLiteral("-")) {
            return false;
        }
        // Re-look up after the yield: the entry may have been erased or replaced.
        auto refreshedIt = m_poolDatasetCache.find(key);
        if (refreshedIt != m_poolDatasetCache.end()) {
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
    return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
}

bool MainWindow::ensureDatasetsLoaded(int connIdx, const QString& poolName, bool allowRemoteLoadIfMissing) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    {
        // Scoped so this reference cannot outlive the runSsh() calls below. operator[]
        // is kept so a missing entry is still created, matching the previous behavior.
        const PoolDatasetCache& existing = m_poolDatasetCache[key];
        if (existing.loaded) {
            return true;
        }
    }
    if (!allowRemoteLoadIfMissing) {
        return false;
    }
    // Copy, not a reference: loadConnections() reassigns m_profiles wholesale and can
    // be dispatched from the event loop while runSsh() pumps it.
    const ConnectionProfile p = m_profiles[connIdx];
    const bool daemonReadApiOk =
        !isWindowsConnection(p)
        && connIdx >= 0
        && connIdx < m_states.size()
        && m_states[connIdx].daemonInstalled
        && m_states[connIdx].daemonActive
        && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    if (connIdx >= 0 && connIdx < m_states.size()) {
        const QString trimmedPool = poolName.trimmed();
        if (!trimmedPool.isEmpty()
            && m_states[connIdx].poolGuidByName.value(trimmedPool).trimmed().isEmpty()) {
            QString gOut;
            QString gErr;
            int gRc = -1;
            const QString guidCmdClassic = withSudo(
                p,
                mwhelpers::withUnixSearchPathCommand(
                    QStringLiteral("zpool get -H -o value guid %1")
                        .arg(shSingleQuote(trimmedPool))));
            const QString guidCmdDaemon = withSudo(
                p, mwhelpers::withUnixSearchPathCommand(
                       QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-guid %1")
                           .arg(shSingleQuote(trimmedPool))));
            const QString selectedGuidCmd = daemonReadApiOk ? guidCmdDaemon : guidCmdClassic;
            bool guidOk = runSsh(p, selectedGuidCmd, 12000, gOut, gErr, gRc) && gRc == 0;
            if (guidOk) {
                const QString guid = gOut.section('\n', 0, 0).trimmed();
                if (!guid.isEmpty() && guid != QStringLiteral("-")) {
                    m_states[connIdx].poolGuidByName.insert(trimmedPool, guid);
                    appLog(QStringLiteral("DEBUG"),
                           QStringLiteral("Loaded missing pool GUID %1::%2 -> %3")
                               .arg(p.name, trimmedPool, guid));
                } else {
                    appLog(QStringLiteral("WARN"),
                           QStringLiteral("Pool GUID missing after query %1::%2")
                               .arg(p.name, trimmedPool));
                }
            } else {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("Could not query pool GUID %1::%2 -> %3")
                           .arg(p.name,
                                trimmedPool,
                                oneLine(gErr.isEmpty() ? QStringLiteral("exit %1").arg(gRc) : gErr)));
            }
        }
    }
    // Built locally and published into m_poolDatasetCache only once it is complete.
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
    appLog(QStringLiteral("INFO"), QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));

    if (!isWin) {
        const QString jsonCmdClassic =
            mwhelpers::withUnixSearchPathCommand(
                QStringLiteral("zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount %1")
                    .arg(shSingleQuote(poolName)));
        const QString jsonCmdDaemon = withSudo(
            p, mwhelpers::withUnixSearchPathCommand(
                   QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-list-all %1")
                       .arg(shSingleQuote(poolName))));
        const QString jsonCmd = daemonReadApiOk ? jsonCmdDaemon : withSudo(p, jsonCmdClassic);
        if (runSsh(p, jsonCmd, 35000, out, err, rc) && rc == 0) {
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
                appLog(QStringLiteral("WARN"),
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
            appLog(QStringLiteral("WARN"), QStringLiteral("Failed datasets %1::%2 -> %3")
                                            .arg(p.name, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
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
                    appLog(QStringLiteral("WARN"),
                           QStringLiteral("%1::%2 driveletter duplicado %3 en datasets: %4")
                               .arg(p.name, poolName, it.key(), it.value().join(QStringLiteral(", "))));
                }
            }
        } else if (!dErr.trimmed().isEmpty()) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: no se pudieron cargar driveletters -> %2").arg(p.name, oneLine(dErr)));
        }
    }
    cache.loaded = true;
    // Publish before rebuildConnInfoFor() so the entry is visible to it, exactly as
    // when this wrote through a reference into the map.
    m_poolDatasetCache.insert(key, cache);
    rebuildConnInfoFor(connIdx);
    appLog(QStringLiteral("DEBUG"), QStringLiteral("Datasets loaded %1::%2 (%3)")
                                     .arg(p.name)
                                     .arg(poolName)
                                     .arg(cache.datasets.size()));
    return true;
}

bool MainWindow::runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs, bool forceConfirmDialog, bool streamProgress) {
    if (!confirmActionExecution(displayLabel, {QStringLiteral("[local]\n%1").arg(command)}, forceConfirmDialog)) {
        return false;
    }
    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1").arg(displayLabel));
    updateStatus(QStringLiteral("%1").arg(displayLabel));
    appLog(QStringLiteral("INFO"), QStringLiteral("$ %1").arg(command));
    QProcess proc;
    m_cancelActionRequested = false;
    m_activeLocalProcess = &proc;
#ifdef Q_OS_WIN
    ConnectionProfile localWinProfile;
    localWinProfile.connType = QStringLiteral("LOCAL");
    localWinProfile.osType = QStringLiteral("Windows");
    proc.start(QStringLiteral("cmd.exe"), QStringList{QStringLiteral("/C"), wrapRemoteCommand(localWinProfile, command)});
#else
    proc.start(QStringLiteral("sh"), QStringList{QStringLiteral("-c"), command});
#endif
    if (!proc.waitForStarted(4000)) {
        appLog(QStringLiteral("NORMAL"),
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
                            appLog(QStringLiteral("INFO"), QStringLiteral("[progress] %1").arg(ln));
                            continue;
                        }
                    }
                    if (progressTimer.elapsed() < 700) {
                        continue;
                    }
                    progressTimer.restart();
                    lastProgressSnippet = ln;
                    sawProgressOutput = true;
                    appLog(QStringLiteral("INFO"), QStringLiteral("[progress] %1").arg(ln));
                    continue;
                }
            }
            appLog(level, oneLine(ln));
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
                    appLog(QStringLiteral("INFO"), QStringLiteral("[progress] %1").arg(partial));
                }
            }
        }
    };

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (m_cancelActionRequested) {
            appLog(QStringLiteral("NORMAL"), trk(QStringLiteral("t_canceling_act001"),
                                                 QStringLiteral("Cancelando acción en curso..."),
                                                 QStringLiteral("Canceling running action..."),
                                                 QStringLiteral("正在取消执行中的操作...")));
            terminateProcessTree(m_activeLocalPid);
            proc.terminate();
            if (!proc.waitForFinished(800)) {
                proc.kill();
                proc.waitForFinished(800);
            }
            appLog(QStringLiteral("NORMAL"), trk(QStringLiteral("t_acc_cancel_usr2"),
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
        if (timeoutMs > 0 && (QDateTime::currentMSecsSinceEpoch() - startMs) > timeoutMs) {
            terminateProcessTree(m_activeLocalPid);
            proc.kill();
            proc.waitForFinished(1000);
            appLog(QStringLiteral("NORMAL"), QStringLiteral("Timeout en comando local"));
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
                appLog(QStringLiteral("INFO"), QStringLiteral("[progress] running..."));
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
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    if (!err.isEmpty() && !streamProgress) {
        appLog(QStringLiteral("INFO"), oneLine(err));
    }
    if (rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizó con error %1").arg(rc));
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
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizado correctamente"));
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
    return mwhelpers::buildSshPreviewCommandText(p, remoteCmd);
}
