#include "agentversion.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>

namespace {

constexpr const char* kDefaultConfigPath = "/etc/zfsmgr/agent.conf";
constexpr const char* kDefaultTlsCertPath = "/etc/zfsmgr/tls/server.crt";
constexpr const char* kDefaultTlsKeyPath = "/etc/zfsmgr/tls/server.key";
constexpr const char* kDefaultTlsClientCertPath = "/etc/zfsmgr/tls/client.crt";
constexpr const char* kDefaultTlsClientKeyPath = "/etc/zfsmgr/tls/client.key";
constexpr const char* kHeartbeatPath = "/tmp/zfsmgr-agent-heartbeat.log";

struct AgentConfig {
    QString bindAddress{QStringLiteral("127.0.0.1")};
    quint16 port{47653};
    QString tlsCertPath{QString::fromLatin1(kDefaultTlsCertPath)};
    QString tlsKeyPath{QString::fromLatin1(kDefaultTlsKeyPath)};
    QString tlsClientCertPath{QString::fromLatin1(kDefaultTlsClientCertPath)};
    QString tlsClientKeyPath{QString::fromLatin1(kDefaultTlsClientKeyPath)};
    int cacheTtlFastMs{2000};
    int cacheTtlSlowMs{8000};
    int reconcileIntervalMs{60000};
    bool zedEventsEnabled{true};
};

struct ExecResult {
    int rc{1};
    QString out;
    QString err;
};

struct CacheEntry {
    ExecResult result;
    QDateTime expiresAtUtc;
};

void writeHeartbeat() {
    QFile f(QString::fromLatin1(kHeartbeatPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
       << " agent alive\n";
}

QString stripQuotes(QString v) {
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

bool parseBoolValue(QString v, bool fallback) {
    v = stripQuotes(v).trimmed().toLower();
    if (v == QStringLiteral("1") || v == QStringLiteral("true") || v == QStringLiteral("yes")
        || v == QStringLiteral("on")) {
        return true;
    }
    if (v == QStringLiteral("0") || v == QStringLiteral("false") || v == QStringLiteral("no")
        || v == QStringLiteral("off")) {
        return false;
    }
    return fallback;
}

int parseIntValue(QString v, int fallback) {
    bool ok = false;
    const int parsed = stripQuotes(v).trimmed().toInt(&ok);
    return ok ? parsed : fallback;
}

AgentConfig loadAgentConfig(const QString& path) {
    AgentConfig cfg;
    QFile f(path.trimmed().isEmpty() ? QString::fromLatin1(kDefaultConfigPath) : path.trimmed());
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
        if (key == QStringLiteral("TLS_CERT")) {
            cfg.tlsCertPath = stripQuotes(value);
        } else if (key == QStringLiteral("TLS_KEY")) {
            cfg.tlsKeyPath = stripQuotes(value);
        } else if (key == QStringLiteral("TLS_CLIENT_CERT")) {
            cfg.tlsClientCertPath = stripQuotes(value);
        } else if (key == QStringLiteral("TLS_CLIENT_KEY")) {
            cfg.tlsClientKeyPath = stripQuotes(value);
        } else if (key == QStringLiteral("AGENT_BIND") || key == QStringLiteral("BIND")) {
            cfg.bindAddress = stripQuotes(value);
        } else if (key == QStringLiteral("AGENT_PORT") || key == QStringLiteral("PORT")) {
            const int p = parseIntValue(value, cfg.port);
            if (p > 0 && p <= 65535) {
                cfg.port = static_cast<quint16>(p);
            }
        } else if (key == QStringLiteral("CACHE_TTL_FAST_MS")) {
            cfg.cacheTtlFastMs = qMax(100, parseIntValue(value, cfg.cacheTtlFastMs));
        } else if (key == QStringLiteral("CACHE_TTL_SLOW_MS")) {
            cfg.cacheTtlSlowMs = qMax(100, parseIntValue(value, cfg.cacheTtlSlowMs));
        } else if (key == QStringLiteral("RECONCILE_INTERVAL_MS")) {
            cfg.reconcileIntervalMs = qMax(1000, parseIntValue(value, cfg.reconcileIntervalMs));
        } else if (key == QStringLiteral("ZED_EVENTS") || key == QStringLiteral("ZED_EVENTS_ENABLED")) {
            cfg.zedEventsEnabled = parseBoolValue(value, cfg.zedEventsEnabled);
        }
    }
    return cfg;
}

bool parseRemoteCommand(const QStringList& args, QString& cmd, QStringList& params) {
    struct CmdSpec {
        const char* name;
        int argc;
    };
    static const QVector<CmdSpec> specs = {
        {"--health", 0},
        {"--dump-zpool-list", 0},
        {"--dump-refresh-basics", 0},
        {"--dump-zfs-version", 0},
        {"--dump-zfs-mount", 0},
        {"--dump-zpool-guid-status-batch", 0},
        {"--dump-zpool-guid", 1},
        {"--dump-zpool-status", 1},
        {"--dump-zpool-status-p", 1},
        {"--dump-zpool-get-all", 1},
        {"--dump-zpool-import-probe", 0},
        {"--dump-zfs-list-all", 1},
        {"--dump-zfs-guid-map", 1},
        {"--dump-zfs-get-prop", 2},
        {"--dump-zfs-get-all", 1},
        {"--dump-zfs-get-json", 2},
        {"--dump-zfs-get-gsa-raw-all-pools", 0},
        {"--dump-zfs-get-gsa-raw-recursive", 1},
        {"--dump-gsa-connections-conf", 0},
    };

    for (int i = 0; i < args.size(); ++i) {
        const QString a = args.at(i);
        for (const CmdSpec& spec : specs) {
            const QString name = QString::fromLatin1(spec.name);
            if (a != name) {
                continue;
            }
            if (i + spec.argc >= args.size()) {
                return false;
            }
            cmd = name;
            params.clear();
            for (int k = 0; k < spec.argc; ++k) {
                params.push_back(args.at(i + 1 + k));
            }
            return true;
        }
    }
    return false;
}

bool isSlowCommand(const QString& cmd) {
    return cmd == QStringLiteral("--dump-zpool-status")
        || cmd == QStringLiteral("--dump-zpool-status-p")
        || cmd == QStringLiteral("--dump-zpool-guid-status-batch")
        || cmd == QStringLiteral("--dump-zpool-import-probe")
        || cmd == QStringLiteral("--dump-zfs-list-all")
        || cmd == QStringLiteral("--dump-zfs-get-gsa-raw-all-pools")
        || cmd == QStringLiteral("--dump-zfs-get-gsa-raw-recursive");
}

QString cacheKeyFor(const QString& cmd, const QStringList& params) {
    return cmd + QStringLiteral("\x1F") + params.join(QStringLiteral("\x1F"));
}

ExecResult runDirectViaChild(const QString& cmd, const QStringList& params) {
    ExecResult r;
    QProcess p;
    p.setProgram(QCoreApplication::applicationFilePath());
    QStringList childArgs;
    childArgs << QStringLiteral("--direct") << cmd;
    childArgs << params;
    p.setArguments(childArgs);
    p.start();
    const int timeoutMs = isSlowCommand(cmd) ? 65000 : 25000;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.rc = 124;
        r.err = QStringLiteral("agent server timeout executing %1").arg(cmd);
        return r;
    }
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    r.rc = (p.exitStatus() == QProcess::NormalExit) ? p.exitCode() : 125;
    return r;
}

ExecResult runProcessSync(const QString& program, const QStringList& args, int timeoutMs, const QString& timeoutMsg) {
    ExecResult r;
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.rc = 124;
        r.err = timeoutMsg;
        return r;
    }
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    r.rc = (p.exitStatus() == QProcess::NormalExit) ? p.exitCode() : 125;
    return r;
}

ExecResult runProcessSyncWithEnv(const QString& program,
                                 const QStringList& args,
                                 const QProcessEnvironment& env,
                                 int timeoutMs,
                                 const QString& timeoutMsg) {
    ExecResult r;
    QProcess p;
    p.setProcessEnvironment(env);
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.rc = 124;
        r.err = timeoutMsg;
        return r;
    }
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    r.rc = (p.exitStatus() == QProcess::NormalExit) ? p.exitCode() : 125;
    return r;
}

QString oneLineCompact(const QString& s) {
    QString out = s;
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    out = out.simplified();
    return out;
}

ExecResult runRefreshBasicsTyped() {
    ExecResult r;
    QString osLine;
    QString machineUuid;
    QString zfsRaw;

    {
        const ExecResult uname = runProcessSync(QStringLiteral("uname"),
                                                {QStringLiteral("-s")},
                                                8000,
                                                QStringLiteral("timeout uname -s"));
        const QString unameS = oneLineCompact(uname.out).trimmed();
        const ExecResult unameA = runProcessSync(QStringLiteral("uname"),
                                                 {QStringLiteral("-a")},
                                                 8000,
                                                 QStringLiteral("timeout uname -a"));
        osLine = oneLineCompact(unameA.out).trimmed();
        if (unameS == QStringLiteral("Linux")) {
            QFile osr(QStringLiteral("/etc/os-release"));
            if (osr.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QStringList lines = QString::fromUtf8(osr.readAll()).split(QLatin1Char('\n'));
                QString name;
                QString ver;
                for (const QString& raw : lines) {
                    const QString line = raw.trimmed();
                    if (line.startsWith(QStringLiteral("NAME="))) {
                        name = stripQuotes(line.mid(5));
                    } else if (line.startsWith(QStringLiteral("VERSION_ID="))) {
                        ver = stripQuotes(line.mid(QStringLiteral("VERSION_ID=").size()));
                    }
                }
                if (!name.isEmpty()) {
                    osLine = (name + QStringLiteral(" ") + ver).simplified();
                } else if (osLine.isEmpty()) {
                    osLine = QStringLiteral("Linux");
                }
            } else if (osLine.isEmpty()) {
                osLine = QStringLiteral("Linux");
            }
        } else if (unameS == QStringLiteral("Darwin")) {
            const ExecResult pn = runProcessSync(QStringLiteral("sw_vers"),
                                                 {QStringLiteral("-productName")},
                                                 8000,
                                                 QStringLiteral("timeout sw_vers -productName"));
            const ExecResult pv = runProcessSync(QStringLiteral("sw_vers"),
                                                 {QStringLiteral("-productVersion")},
                                                 8000,
                                                 QStringLiteral("timeout sw_vers -productVersion"));
            const QString mac = (oneLineCompact(pn.out) + QStringLiteral(" ") + oneLineCompact(pv.out)).simplified();
            if (!mac.isEmpty()) {
                osLine = mac;
            } else if (osLine.isEmpty()) {
                osLine = QStringLiteral("macOS");
            }
        } else if (unameS == QStringLiteral("FreeBSD")) {
            ExecResult fv = runProcessSync(QStringLiteral("freebsd-version"),
                                           {QStringLiteral("-k")},
                                           8000,
                                           QStringLiteral("timeout freebsd-version -k"));
            if (fv.rc != 0 || oneLineCompact(fv.out).isEmpty()) {
                fv = runProcessSync(QStringLiteral("freebsd-version"),
                                    {},
                                    8000,
                                    QStringLiteral("timeout freebsd-version"));
            }
            QString ver = oneLineCompact(fv.out).trimmed();
            if (ver.isEmpty()) {
                const ExecResult ur = runProcessSync(QStringLiteral("uname"),
                                                     {QStringLiteral("-r")},
                                                     8000,
                                                     QStringLiteral("timeout uname -r"));
                ver = oneLineCompact(ur.out).trimmed();
            }
            if (!ver.isEmpty()) {
                osLine = QStringLiteral("FreeBSD %1").arg(ver);
            } else if (osLine.isEmpty()) {
                osLine = QStringLiteral("FreeBSD");
            }
        }
    }

    {
        const QStringList candidates = {
            QStringLiteral("/etc/machine-id"),
            QStringLiteral("/var/lib/dbus/machine-id"),
        };
        for (const QString& path : candidates) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }
            machineUuid = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n')).value(0).trimmed();
            if (!machineUuid.isEmpty()) {
                break;
            }
        }
        if (machineUuid.isEmpty()) {
            const ExecResult io = runProcessSync(QStringLiteral("ioreg"),
                                                 {QStringLiteral("-rd1"), QStringLiteral("-c"), QStringLiteral("IOPlatformExpertDevice")},
                                                 8000,
                                                 QStringLiteral("timeout ioreg"));
            const QStringList lines = (io.out + QStringLiteral("\n") + io.err).split(QLatin1Char('\n'));
            for (const QString& line : lines) {
                if (!line.contains(QStringLiteral("IOPlatformUUID"))) {
                    continue;
                }
                const QRegularExpression rx(QStringLiteral("\"IOPlatformUUID\"\\s*=\\s*\"([^\"]+)\""));
                const QRegularExpressionMatch m = rx.match(line);
                if (m.hasMatch()) {
                    machineUuid = m.captured(1).trimmed();
                    break;
                }
            }
        }
    }

    {
        struct CmdTry { QString prog; QStringList args; };
        const QVector<CmdTry> tries = {
            {QStringLiteral("zfs"), {QStringLiteral("version")}},
            {QStringLiteral("zfs"), {QStringLiteral("--version")}},
            {QStringLiteral("zpool"), {QStringLiteral("--version")}},
        };
        for (const CmdTry& t : tries) {
            ExecResult e = runProcessSync(t.prog, t.args, 10000, QStringLiteral("timeout zfs/zpool version"));
            const QString merged = oneLineCompact(e.out + QStringLiteral("\n") + e.err);
            if (e.rc == 0 && !merged.isEmpty()) {
                zfsRaw = merged;
                break;
            }
        }
    }

    r.out = QStringLiteral("OS_LINE=%1\nMACHINE_UUID=%2\nZFS_VERSION_RAW=%3\n")
                .arg(oneLineCompact(osLine), oneLineCompact(machineUuid), oneLineCompact(zfsRaw));
    r.rc = 0;
    return r;
}

ExecResult runZfsVersionTyped() {
    struct CmdTry { QString prog; QStringList args; };
    const QVector<CmdTry> tries = {
        {QStringLiteral("zfs"), {QStringLiteral("version")}},
        {QStringLiteral("zfs"), {QStringLiteral("--version")}},
        {QStringLiteral("zpool"), {QStringLiteral("--version")}},
    };
    for (const CmdTry& t : tries) {
        ExecResult e = runProcessSync(t.prog, t.args, 10000, QStringLiteral("timeout zfs version"));
        const QString merged = (e.out + QStringLiteral("\n") + e.err).trimmed();
        if (e.rc == 0 && !merged.isEmpty()) {
            e.out = merged + QLatin1Char('\n');
            e.err.clear();
            return e;
        }
    }
    ExecResult fail;
    fail.rc = 1;
    return fail;
}

ExecResult runFastPathCommand(const QString& cmd, const QStringList& params, bool* handled) {
    if (handled) {
        *handled = true;
    }
    if (cmd == QStringLiteral("--dump-zpool-list")) {
        return runProcessSync(QStringLiteral("zpool"),
                              {QStringLiteral("list"), QStringLiteral("-j")},
                              20000,
                              QStringLiteral("agent timeout running zpool list -j"));
    }
    if (cmd == QStringLiteral("--dump-refresh-basics")) {
        return runRefreshBasicsTyped();
    }
    if (cmd == QStringLiteral("--dump-zfs-version")) {
        return runZfsVersionTyped();
    }
    if (cmd == QStringLiteral("--dump-zfs-mount")) {
        return runProcessSync(QStringLiteral("zfs"),
                              {QStringLiteral("mount"), QStringLiteral("-j")},
                              20000,
                              QStringLiteral("agent timeout running zfs mount -j"));
    }
    if (cmd == QStringLiteral("--dump-zpool-guid-status-batch")) {
        ExecResult pools = runProcessSync(QStringLiteral("zpool"),
                                          {QStringLiteral("list"), QStringLiteral("-H"), QStringLiteral("-o"), QStringLiteral("name")},
                                          15000,
                                          QStringLiteral("agent timeout running zpool list names"));
        if (pools.rc != 0) {
            return pools;
        }
        ExecResult agg;
        agg.rc = 0;
        const QStringList poolNames = pools.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& rawPool : poolNames) {
            const QString pool = rawPool.trimmed();
            if (pool.isEmpty()) {
                continue;
            }
            const ExecResult guid = runProcessSync(
                QStringLiteral("zpool"),
                {QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"),
                 QStringLiteral("value"), QStringLiteral("guid"), pool},
                12000,
                QStringLiteral("agent timeout running zpool guid in batch"));
            const ExecResult status = runProcessSync(
                QStringLiteral("zpool"),
                {QStringLiteral("status"), QStringLiteral("-v"), pool},
                25000,
                QStringLiteral("agent timeout running zpool status in batch"));

            agg.out += QStringLiteral("__ZFSMGR_POOL__:%1\n").arg(pool);
            agg.out += QStringLiteral("__ZFSMGR_GUID__:%1\n").arg(guid.out.section('\n', 0, 0).trimmed());
            agg.out += QStringLiteral("__ZFSMGR_STATUS_BEGIN__\n");
            agg.out += status.out;
            if (!status.err.trimmed().isEmpty()) {
                agg.out += status.err;
                if (!status.err.endsWith(QLatin1Char('\n'))) {
                    agg.out += QLatin1Char('\n');
                }
            }
            agg.out += QStringLiteral("__ZFSMGR_STATUS_END__\n");
        }
        return agg;
    }
    if (cmd == QStringLiteral("--dump-zpool-guid") && params.size() >= 1) {
        return runProcessSync(QStringLiteral("zpool"),
                              {QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"),
                               QStringLiteral("value"), QStringLiteral("guid"), params.at(0)},
                              15000,
                              QStringLiteral("agent timeout running zpool get guid"));
    }
    if (cmd == QStringLiteral("--dump-zpool-status") && params.size() >= 1) {
        return runProcessSync(QStringLiteral("zpool"),
                              {QStringLiteral("status"), QStringLiteral("-v"), params.at(0)},
                              20000,
                              QStringLiteral("agent timeout running zpool status -v"));
    }
    if (cmd == QStringLiteral("--dump-zpool-status-p") && params.size() >= 1) {
        return runProcessSync(QStringLiteral("zpool"),
                              {QStringLiteral("status"), QStringLiteral("-P"), params.at(0)},
                              20000,
                              QStringLiteral("agent timeout running zpool status -P"));
    }
    if (cmd == QStringLiteral("--dump-zpool-get-all") && params.size() >= 1) {
        return runProcessSync(QStringLiteral("zpool"),
                              {QStringLiteral("get"), QStringLiteral("-j"), QStringLiteral("all"), params.at(0)},
                              20000,
                              QStringLiteral("agent timeout running zpool get -j all"));
    }
    if (cmd == QStringLiteral("--dump-zpool-import-probe")) {
        ExecResult a = runProcessSync(QStringLiteral("zpool"),
                                      {QStringLiteral("import")},
                                      15000,
                                      QStringLiteral("agent timeout running zpool import"));
        ExecResult b = runProcessSync(QStringLiteral("zpool"),
                                      {QStringLiteral("import"), QStringLiteral("-s")},
                                      20000,
                                      QStringLiteral("agent timeout running zpool import -s"));
        ExecResult out;
        out.out = a.out + b.out;
        out.err = a.err + b.err;
        out.rc = b.rc;
        return out;
    }
    if (cmd == QStringLiteral("--dump-zfs-list-all") && params.size() >= 1) {
        const QString pool = params.at(0);
        const QStringList getArgs = {
            QStringLiteral("get"), QStringLiteral("-j"), QStringLiteral("-p"), QStringLiteral("-r"),
            QStringLiteral("-t"), QStringLiteral("filesystem,volume,snapshot"),
            QStringLiteral("type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount"),
            pool};
        QProcessEnvironment base = QProcessEnvironment::systemEnvironment();
        QProcessEnvironment envC = base;
        envC.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
        envC.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        ExecResult e = runProcessSyncWithEnv(
            QStringLiteral("zfs"), getArgs, envC, 45000, QStringLiteral("agent timeout running zfs get -j list-all"));
        if (e.rc == 0 && !e.out.trimmed().isEmpty()) {
            return e;
        }
        QProcessEnvironment envEn = base;
        envEn.insert(QStringLiteral("LC_ALL"), QStringLiteral("en_US.UTF-8"));
        envEn.insert(QStringLiteral("LANG"), QStringLiteral("en_US.UTF-8"));
        e = runProcessSyncWithEnv(
            QStringLiteral("zfs"), getArgs, envEn, 45000, QStringLiteral("agent timeout running zfs get -j list-all"));
        if (e.rc == 0 && !e.out.trimmed().isEmpty()) {
            return e;
        }
        e = runProcessSync(
            QStringLiteral("zfs"), getArgs, 45000, QStringLiteral("agent timeout running zfs get -j list-all"));
        if (e.rc == 0 && !e.out.trimmed().isEmpty()) {
            return e;
        }
        return runProcessSync(
            QStringLiteral("zfs"),
            {QStringLiteral("list"), QStringLiteral("-H"), QStringLiteral("-p"), QStringLiteral("-t"),
             QStringLiteral("filesystem,volume,snapshot"), QStringLiteral("-o"),
             QStringLiteral("name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount"),
             QStringLiteral("-r"), pool},
            45000,
            QStringLiteral("agent timeout running zfs list fallback"));
    }
    if (cmd == QStringLiteral("--dump-zfs-guid-map") && params.size() >= 1) {
        return runProcessSync(QStringLiteral("zfs"),
                              {QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"),
                               QStringLiteral("name,value"), QStringLiteral("guid"), QStringLiteral("-r"), params.at(0)},
                              25000,
                              QStringLiteral("agent timeout running zfs guid map"));
    }
    if (cmd == QStringLiteral("--dump-zfs-get-prop") && params.size() >= 2) {
        return runProcessSync(QStringLiteral("zfs"),
                              {QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"), QStringLiteral("value"),
                               params.at(0), params.at(1)},
                              15000,
                              QStringLiteral("agent timeout running zfs get prop"));
    }
    if (cmd == QStringLiteral("--dump-zfs-get-all") && params.size() >= 1) {
        const QString obj = params.at(0);
        const QStringList args = {QStringLiteral("get"), QStringLiteral("-j"), QStringLiteral("all"), obj};
        QProcessEnvironment base = QProcessEnvironment::systemEnvironment();
        QProcessEnvironment envC = base;
        envC.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
        envC.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        ExecResult e = runProcessSyncWithEnv(
            QStringLiteral("zfs"), args, envC, 20000, QStringLiteral("agent timeout running zfs get all -j"));
        if (e.rc == 0 && !e.out.trimmed().isEmpty()) {
            return e;
        }
        QProcessEnvironment envEn = base;
        envEn.insert(QStringLiteral("LC_ALL"), QStringLiteral("en_US.UTF-8"));
        envEn.insert(QStringLiteral("LANG"), QStringLiteral("en_US.UTF-8"));
        e = runProcessSyncWithEnv(
            QStringLiteral("zfs"), args, envEn, 20000, QStringLiteral("agent timeout running zfs get all -j"));
        if (e.rc == 0 && !e.out.trimmed().isEmpty()) {
            return e;
        }
        return runProcessSync(
            QStringLiteral("zfs"), args, 20000, QStringLiteral("agent timeout running zfs get all -j"));
    }
    if (cmd == QStringLiteral("--dump-zfs-get-json") && params.size() >= 2) {
        const QString props = params.at(0);
        const QString obj = params.at(1);
        const QStringList args = {QStringLiteral("get"), QStringLiteral("-j"), props, obj};
        QProcessEnvironment base = QProcessEnvironment::systemEnvironment();
        QProcessEnvironment envC = base;
        envC.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
        envC.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        ExecResult e = runProcessSyncWithEnv(
            QStringLiteral("zfs"), args, envC, 20000, QStringLiteral("agent timeout running zfs get -j subset"));
        if (e.rc == 0 && !e.out.trimmed().isEmpty()) {
            return e;
        }
        QProcessEnvironment envEn = base;
        envEn.insert(QStringLiteral("LC_ALL"), QStringLiteral("en_US.UTF-8"));
        envEn.insert(QStringLiteral("LANG"), QStringLiteral("en_US.UTF-8"));
        e = runProcessSyncWithEnv(
            QStringLiteral("zfs"), args, envEn, 20000, QStringLiteral("agent timeout running zfs get -j subset"));
        if (e.rc == 0 && !e.out.trimmed().isEmpty()) {
            return e;
        }
        return runProcessSync(
            QStringLiteral("zfs"), args, 20000, QStringLiteral("agent timeout running zfs get -j subset"));
    }
    if (cmd == QStringLiteral("--dump-zfs-get-gsa-raw-all-pools")) {
        ExecResult pools = runProcessSync(QStringLiteral("zpool"),
                                          {QStringLiteral("list"), QStringLiteral("-H"), QStringLiteral("-o"), QStringLiteral("name")},
                                          15000,
                                          QStringLiteral("agent timeout running zpool list names"));
        if (pools.rc != 0) {
            return pools;
        }
        ExecResult agg;
        agg.rc = 0;
        const QStringList poolNames = pools.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString props = QStringLiteral("org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino");
        for (const QString& rawPool : poolNames) {
            const QString pool = rawPool.trimmed();
            if (pool.isEmpty()) {
                continue;
            }
            ExecResult one = runProcessSync(
                QStringLiteral("zfs"),
                {QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"),
                 QStringLiteral("name,property,value"), QStringLiteral("-r"), props, pool},
                15000,
                QStringLiteral("agent timeout running gsa raw per pool"));
            agg.out += one.out;
            agg.err += one.err;
        }
        return agg;
    }
    if (cmd == QStringLiteral("--dump-zfs-get-gsa-raw-recursive") && params.size() >= 1) {
        return runProcessSync(QStringLiteral("zfs"),
                              {QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"),
                               QStringLiteral("name,property,value,source"), QStringLiteral("-r"),
                               QStringLiteral("org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino"),
                               params.at(0)},
                              30000,
                              QStringLiteral("agent timeout running gsa recursive scan"));
    }
    if (cmd == QStringLiteral("--dump-gsa-connections-conf")) {
        ExecResult r;
        QFile f(QStringLiteral("/etc/zfsmgr/gsa-connections.conf"));
        if (!f.exists()) {
            r.rc = 0;
            return r;
        }
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            r.rc = 1;
            r.err = QStringLiteral("cannot open /etc/zfsmgr/gsa-connections.conf\n");
            return r;
        }
        r.out = QString::fromUtf8(f.readAll());
        r.rc = 0;
        return r;
    }
    if (handled) {
        *handled = false;
    }
    return {};
}

class AgentServer final : public QObject {
public:
    explicit AgentServer(const AgentConfig& cfg, QObject* parent = nullptr)
        : QObject(parent)
        , m_cfg(cfg) {}

    bool start(QString* errOut) {
        if (!loadTls(errOut)) {
            return false;
        }
        m_server = new QTcpServer(this);
        QObject::connect(m_server, &QTcpServer::newConnection, this, [this]() { onNewConnection(); });
        const QHostAddress bindAddr(m_cfg.bindAddress);
        const QHostAddress addr = bindAddr.isNull() ? QHostAddress::LocalHost : bindAddr;
        if (!m_server->listen(addr, m_cfg.port)) {
            if (errOut) {
                *errOut = m_server->errorString();
            }
            return false;
        }
        if (m_cfg.zedEventsEnabled) {
            startZedWatcher();
        }
        startReconcileTimer();
        return true;
    }

private:
    bool loadTls(QString* errOut) {
        QFile certFile(m_cfg.tlsCertPath);
        QFile keyFile(m_cfg.tlsKeyPath);
        QFile clientCertFile(m_cfg.tlsClientCertPath);
        if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)
            || !clientCertFile.open(QIODevice::ReadOnly)) {
            if (errOut) {
                *errOut = QStringLiteral("TLS files not readable (%1, %2, %3)")
                              .arg(m_cfg.tlsCertPath, m_cfg.tlsKeyPath, m_cfg.tlsClientCertPath);
            }
            return false;
        }
        const QByteArray certPem = certFile.readAll();
        const QByteArray keyPem = keyFile.readAll();
        const QByteArray clientCertPem = clientCertFile.readAll();
        certFile.close();
        keyFile.close();
        clientCertFile.close();

        const QList<QSslCertificate> certs = QSslCertificate::fromData(certPem, QSsl::Pem);
        const QList<QSslCertificate> clientCerts = QSslCertificate::fromData(clientCertPem, QSsl::Pem);
        if (certs.isEmpty()) {
            if (errOut) {
                *errOut = QStringLiteral("Invalid TLS certificate");
            }
            return false;
        }
        if (clientCerts.isEmpty()) {
            if (errOut) {
                *errOut = QStringLiteral("Invalid TLS client certificate");
            }
            return false;
        }
        QSslKey key(keyPem, QSsl::Rsa, QSsl::Pem);
        if (key.isNull()) {
            key = QSslKey(keyPem, QSsl::Ec, QSsl::Pem);
        }
        if (key.isNull()) {
            if (errOut) {
                *errOut = QStringLiteral("Invalid TLS private key");
            }
            return false;
        }
        m_serverCert = certs.first();
        m_serverKey = key;
        m_clientCaCerts = clientCerts;
        return true;
    }

    void startZedWatcher() {
        m_zedProc = new QProcess(this);
        m_zedProc->setProgram(QStringLiteral("zpool"));
        m_zedProc->setArguments({QStringLiteral("events"), QStringLiteral("-f")});
        QObject::connect(m_zedProc, &QProcess::readyReadStandardOutput, this, [this]() {
            const QString out = QString::fromUtf8(m_zedProc->readAllStandardOutput());
            if (!out.trimmed().isEmpty()) {
                invalidateCache(QStringLiteral("zed_event"));
                m_lastZedEventUtc = QDateTime::currentDateTimeUtc();
            }
        });
        QObject::connect(m_zedProc, &QProcess::readyReadStandardError, this, [this]() {
            (void)m_zedProc->readAllStandardError();
        });
        QObject::connect(m_zedProc,
                         qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         this,
                         [this](int, QProcess::ExitStatus) {
                             if (!m_cfg.zedEventsEnabled) {
                                 return;
                             }
                             ++m_zedRestartCount;
                             QTimer::singleShot(3000, this, [this]() {
                                 if (!m_zedProc || m_zedProc->state() != QProcess::NotRunning) {
                                     return;
                                 }
                                 m_zedProc->start();
                             });
                         });
        m_zedProc->start();
    }

    void onNewConnection() {
        while (m_server->hasPendingConnections()) {
            QTcpSocket* tcp = m_server->nextPendingConnection();
            if (!tcp) {
                return;
            }
            QSslSocket* ssl = new QSslSocket(this);
            if (!ssl->setSocketDescriptor(tcp->socketDescriptor())) {
                tcp->deleteLater();
                ssl->deleteLater();
                continue;
            }
            tcp->deleteLater();
            ssl->setPeerVerifyMode(QSslSocket::VerifyPeer);
            QSslConfiguration conf = ssl->sslConfiguration();
            conf.setLocalCertificate(m_serverCert);
            conf.setPrivateKey(m_serverKey);
            conf.setCaCertificates(m_clientCaCerts);
            conf.setPeerVerifyMode(QSslSocket::VerifyPeer);
            conf.setProtocol(QSsl::TlsV1_2OrLater);
            ssl->setSslConfiguration(conf);
            ssl->setProperty("buffer", QByteArray());

            QObject::connect(ssl, &QSslSocket::readyRead, this, [this, ssl]() { onSocketReadyRead(ssl); });
            QObject::connect(ssl, &QSslSocket::disconnected, ssl, &QObject::deleteLater);
            QObject::connect(ssl,
                             qOverload<QAbstractSocket::SocketError>(&QAbstractSocket::errorOccurred),
                             ssl,
                             [ssl](QAbstractSocket::SocketError) {
                                 ssl->disconnectFromHost();
                             });
            ssl->startServerEncryption();
        }
    }

    void onSocketReadyRead(QSslSocket* ssl) {
        QByteArray buffer = ssl->property("buffer").toByteArray();
        buffer.append(ssl->readAll());
        int idx = -1;
        while ((idx = buffer.indexOf('\n')) >= 0) {
            const QByteArray line = buffer.left(idx).trimmed();
            buffer.remove(0, idx + 1);
            if (line.isEmpty()) {
                continue;
            }
            ExecResult result;
            const QJsonDocument reqDoc = QJsonDocument::fromJson(line);
            const QJsonObject req = reqDoc.object();
            const QString cmd = req.value(QStringLiteral("cmd")).toString().trimmed();
            QStringList params;
            for (const QJsonValue& v : req.value(QStringLiteral("args")).toArray()) {
                params.push_back(v.toString());
            }
            if (cmd.isEmpty()) {
                result.rc = 2;
                result.err = QStringLiteral("invalid request: empty cmd");
            } else if (cmd == QStringLiteral("--health")) {
                result.rc = 0;
                QStringList lines;
                lines << QStringLiteral("STATUS=OK")
                      << QStringLiteral("VERSION=%1").arg(agentversion::currentVersion())
                      << QStringLiteral("API=%1").arg(agentversion::expectedApiVersion())
                      << QStringLiteral("SERVER=1")
                      << QStringLiteral("CACHE_ENTRIES=%1").arg(m_cache.size())
                      << QStringLiteral("CACHE_INVALIDATIONS=%1").arg(m_cacheInvalidations)
                      << QStringLiteral("ZED_ACTIVE=%1").arg((m_zedProc && m_zedProc->state() != QProcess::NotRunning) ? 1 : 0)
                      << QStringLiteral("ZED_RESTARTS=%1").arg(m_zedRestartCount)
                      << QStringLiteral("ZED_LAST_EVENT_UTC=%1").arg(m_lastZedEventUtc.isValid()
                                                                        ? m_lastZedEventUtc.toString(Qt::ISODate)
                                                                        : QString())
                      << QStringLiteral("RECONCILE_INTERVAL_MS=%1").arg(m_cfg.reconcileIntervalMs)
                      << QStringLiteral("RECONCILE_LAST_UTC=%1").arg(m_lastReconcileUtc.isValid()
                                                                        ? m_lastReconcileUtc.toString(Qt::ISODate)
                                                                        : QString());
                result.out = lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
            } else {
                const QString key = cacheKeyFor(cmd, params);
                const QDateTime now = QDateTime::currentDateTimeUtc();
                const auto it = m_cache.constFind(key);
                if (it != m_cache.cend() && it->expiresAtUtc > now) {
                    result = it->result;
                } else {
                    bool handled = false;
                    result = runFastPathCommand(cmd, params, &handled);
                    if (!handled) {
                        result = runDirectViaChild(cmd, params);
                    }
                    CacheEntry entry;
                    entry.result = result;
                    const int ttl = isSlowCommand(cmd) ? m_cfg.cacheTtlSlowMs : m_cfg.cacheTtlFastMs;
                    entry.expiresAtUtc = now.addMSecs(ttl);
                    m_cache.insert(key, entry);
                }
            }

            QJsonObject resp;
            resp.insert(QStringLiteral("rc"), result.rc);
            resp.insert(QStringLiteral("stdout"), result.out);
            resp.insert(QStringLiteral("stderr"), result.err);
            const QByteArray payload = QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
            ssl->write(payload);
            ssl->flush();
        }
        ssl->setProperty("buffer", buffer);
    }

private:
    AgentConfig m_cfg;
    QTcpServer* m_server{nullptr};
    QSslCertificate m_serverCert;
    QSslKey m_serverKey;
    QList<QSslCertificate> m_clientCaCerts;
    QProcess* m_zedProc{nullptr};
    QTimer* m_reconcileTimer{nullptr};
    QHash<QString, CacheEntry> m_cache;
    QDateTime m_lastZedEventUtc;
    QDateTime m_lastReconcileUtc;
    quint64 m_cacheInvalidations{0};
    quint64 m_zedRestartCount{0};

    void invalidateCache(const QString&) {
        if (m_cache.isEmpty()) {
            return;
        }
        m_cache.clear();
        ++m_cacheInvalidations;
    }

    void startReconcileTimer() {
        m_reconcileTimer = new QTimer(this);
        m_reconcileTimer->setInterval(m_cfg.reconcileIntervalMs);
        QObject::connect(m_reconcileTimer, &QTimer::timeout, this, [this]() {
            m_lastReconcileUtc = QDateTime::currentDateTimeUtc();
            invalidateCache(QStringLiteral("reconcile"));
        });
        m_reconcileTimer->start();
    }
};

bool tryForwardToResidentDaemon(const AgentConfig& cfg,
                                const QString& cmd,
                                const QStringList& params,
                                ExecResult& resultOut) {
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

    const QHostAddress addr(cfg.bindAddress);
    const QString host = addr.isNull() ? cfg.bindAddress : addr.toString();
    const QStringList peerNames = {QStringLiteral("zfsmgr-agent-server"), QStringLiteral("zfsmgr-agent")};
    for (const QString& peerName : peerNames) {
        QSslSocket sock;
        sock.setProtocol(QSsl::TlsV1_2OrLater);
        QSslConfiguration conf = sock.sslConfiguration();
        conf.setCaCertificates(caCerts);
        conf.setLocalCertificate(clientCerts.first());
        conf.setPrivateKey(clientKey);
        conf.setProtocol(QSsl::TlsV1_2OrLater);
        conf.setPeerVerifyMode(QSslSocket::VerifyPeer);
        sock.setSslConfiguration(conf);

        sock.connectToHostEncrypted(host, cfg.port, peerName);
        if (!sock.waitForEncrypted(1200)) {
            continue;
        }

        QJsonObject req;
        req.insert(QStringLiteral("cmd"), cmd);
        QJsonArray a;
        for (const QString& p : params) {
            a.push_back(p);
        }
        req.insert(QStringLiteral("args"), a);
        const QByteArray payload = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
        if (sock.write(payload) < 0 || !sock.waitForBytesWritten(1000)) {
            continue;
        }

        QByteArray line;
        const QDateTime deadline = QDateTime::currentDateTimeUtc().addMSecs(isSlowCommand(cmd) ? 70000 : 30000);
        while (QDateTime::currentDateTimeUtc() < deadline) {
            if (!sock.waitForReadyRead(400)) {
                continue;
            }
            line.append(sock.readAll());
            const int nl = line.indexOf('\n');
            if (nl < 0) {
                continue;
            }
            const QByteArray one = line.left(nl).trimmed();
            const QJsonObject resp = QJsonDocument::fromJson(one).object();
            resultOut.rc = resp.value(QStringLiteral("rc")).toInt(1);
            resultOut.out = resp.value(QStringLiteral("stdout")).toString();
            resultOut.err = resp.value(QStringLiteral("stderr")).toString();
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const bool directMode = args.contains(QStringLiteral("--direct"));
    const AgentConfig cfg = loadAgentConfig(QString::fromLatin1(kDefaultConfigPath));

    if (args.contains(QStringLiteral("--version")) || args.contains(QStringLiteral("version"))) {
        QTextStream(stdout) << agentversion::currentVersion() << '\n';
        return 0;
    }
    if (args.contains(QStringLiteral("--api-version")) || args.contains(QStringLiteral("api"))) {
        QTextStream(stdout) << agentversion::expectedApiVersion() << '\n';
        return 0;
    }

    if (args.contains(QStringLiteral("--serve")) || args.contains(QStringLiteral("serve"))) {
        AgentServer server(cfg);
        QString err;
        if (!server.start(&err)) {
            QTextStream(stderr) << "cannot start zfsmgr-agent server: " << err << '\n';
            return 1;
        }
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, []() { writeHeartbeat(); });
        timer.start(60000);
        writeHeartbeat();
        return app.exec();
    }

    QString parsedCmd;
    QStringList parsedParams;
    const bool hasRemoteCmd = parseRemoteCommand(args, parsedCmd, parsedParams);
    if (hasRemoteCmd && !directMode) {
        ExecResult proxied;
        if (tryForwardToResidentDaemon(cfg, parsedCmd, parsedParams, proxied)) {
            if (!proxied.out.isEmpty()) {
                QTextStream(stdout) << proxied.out;
            }
            if (!proxied.err.isEmpty()) {
                QTextStream(stderr) << proxied.err;
            }
            return proxied.rc;
        }
        if (parsedCmd == QStringLiteral("--health")) {
            QTextStream(stdout) << "STATUS=DOWN\n";
            QTextStream(stdout) << "VERSION=" << agentversion::currentVersion() << '\n';
            QTextStream(stdout) << "API=" << agentversion::expectedApiVersion() << '\n';
            QTextStream(stderr) << "daemon server is not reachable\n";
            return 1;
        }
    }

    if (args.contains(QStringLiteral("--health"))) {
        QTextStream(stdout) << "STATUS=OK\n";
        QTextStream(stdout) << "VERSION=" << agentversion::currentVersion() << '\n';
        QTextStream(stdout) << "API=" << agentversion::expectedApiVersion() << '\n';
        return 0;
    }
    if (args.contains(QStringLiteral("--once"))) {
        writeHeartbeat();
        return 0;
    }
    if (hasRemoteCmd) {
        bool handled = false;
        const ExecResult local = runFastPathCommand(parsedCmd, parsedParams, &handled);
        if (handled) {
            if (!local.out.isEmpty()) {
                QTextStream(stdout) << local.out;
            }
            if (!local.err.isEmpty()) {
                QTextStream(stderr) << local.err;
            }
            return local.rc;
        }
    }
    if (args.contains(QStringLiteral("--dump-zpool-list"))) {
        QProcess p;
        p.setProgram(QStringLiteral("zpool"));
        p.setArguments({QStringLiteral("list"), QStringLiteral("-j")});
        p.start();
        if (!p.waitForFinished(20000)) {
            QTextStream(stderr) << "agent timeout running zpool list -j\n";
            return 124;
        }
        QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            QTextStream(stderr) << QString::fromUtf8(err);
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
    }
    if (args.contains(QStringLiteral("--dump-refresh-basics"))) {
        QProcess p;
        p.setProgram(QStringLiteral("sh"));
        p.setArguments({QStringLiteral("-lc"),
                        QStringLiteral(
                            "set +e; "
                            "PATH=\"$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin\"; "
                            "one_line(){ printf '%s' \"$1\" | tr '\\r\\n' ' ' | sed 's/[[:space:]]\\+/ /g; s/^ //; s/ $//'; }; "
                            "os_line=\"$(uname -a 2>/dev/null || true)\"; "
                            "uname_s=\"$(uname -s 2>/dev/null || true)\"; "
                            "if [ \"$uname_s\" = \"Linux\" ]; then "
                            "  refined=\"$(sh -lc '. /etc/os-release 2>/dev/null; printf \\\"%s %s\\\" \\\"$NAME\\\" \\\"$VERSION_ID\\\"' 2>/dev/null)\"; "
                            "  [ -n \"$refined\" ] && os_line=\"$refined\" || os_line='Linux'; "
                            "elif [ \"$uname_s\" = \"Darwin\" ]; then "
                            "  refined=\"$(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null)\"; "
                            "  refined=\"$(one_line \"$refined\")\"; [ -n \"$refined\" ] && os_line=\"$refined\" || os_line='macOS'; "
                            "elif [ \"$uname_s\" = \"FreeBSD\" ]; then "
                            "  refined=\"$(freebsd-version -k 2>/dev/null || freebsd-version 2>/dev/null || uname -r 2>/dev/null)\"; "
                            "  refined=\"$(one_line \"$refined\")\"; [ -n \"$refined\" ] && os_line=\"FreeBSD $refined\" || os_line='FreeBSD'; "
                            "fi; "
                            "machine_uuid=''; "
                            "for c in \"cat /etc/machine-id 2>/dev/null\" "
                            "         \"cat /var/lib/dbus/machine-id 2>/dev/null\" "
                            "         \"ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F\\\\\\\" '/IOPlatformUUID/{print $(NF-1); exit}'\"; do "
                            "  out=\"$(sh -lc \"$c\" 2>/dev/null | head -n1 | tr -d '\\r')\"; "
                            "  if [ -n \"$out\" ]; then machine_uuid=\"$out\"; break; fi; "
                            "done; "
                            "zfs_raw=''; "
                            "for c in \"zfs version\" \"zfs --version\" \"zpool --version\"; do "
                            "  out=\"$(sh -lc \"$c\" 2>&1)\"; rc=$?; "
                            "  if [ $rc -eq 0 ] && [ -n \"$out\" ]; then zfs_raw=\"$(one_line \"$out\")\"; break; fi; "
                            "done; "
                            "printf 'OS_LINE=%s\\n' \"$(one_line \"$os_line\")\"; "
                            "printf 'MACHINE_UUID=%s\\n' \"$(one_line \"$machine_uuid\")\"; "
                            "printf 'ZFS_VERSION_RAW=%s\\n' \"$(one_line \"$zfs_raw\")\"")});
        p.start();
        if (!p.waitForFinished(20000)) {
            QTextStream(stderr) << "agent timeout running refresh basics\n";
            return 124;
        }
        QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            QTextStream(stderr) << QString::fromUtf8(err);
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
    }
    if (args.contains(QStringLiteral("--dump-zfs-version"))) {
        QProcess p;
        p.setProgram(QStringLiteral("sh"));
        p.setArguments({QStringLiteral("-lc"),
                        QStringLiteral(
                            "set +e; "
                            "PATH=\"$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin\"; "
                            "for c in \"zfs version\" \"zfs --version\" \"zpool --version\"; do "
                            "  out=\"$(sh -lc \"$c\" 2>&1)\"; rc=$?; "
                            "  if [ $rc -eq 0 ] && [ -n \"$out\" ]; then printf '%s\\n' \"$out\"; exit 0; fi; "
                            "done; "
                            "exit 1")});
        p.start();
        if (!p.waitForFinished(15000)) {
            QTextStream(stderr) << "agent timeout running zfs version\n";
            return 124;
        }
        QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            QTextStream(stderr) << QString::fromUtf8(err);
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
    }
    if (args.contains(QStringLiteral("--dump-zfs-mount"))) {
        QProcess p;
        p.setProgram(QStringLiteral("zfs"));
        p.setArguments({QStringLiteral("mount"), QStringLiteral("-j")});
        p.start();
        if (!p.waitForFinished(20000)) {
            QTextStream(stderr) << "agent timeout running zfs mount -j\n";
            return 124;
        }
        QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            QTextStream(stderr) << QString::fromUtf8(err);
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
    }
    if (args.contains(QStringLiteral("--dump-zpool-guid-status-batch"))) {
        QProcess p;
        p.setProgram(QStringLiteral("sh"));
        p.setArguments({QStringLiteral("-lc"),
                        QStringLiteral(
                            "zpool list -H -o name 2>/dev/null | while IFS= read -r pool; do "
                            "[ -n \"$pool\" ] || continue; "
                            "guid=$(zpool get -H -o value guid \"$pool\" 2>/dev/null | head -n1 || true); "
                            "printf '__ZFSMGR_POOL__:%s\\n' \"$pool\"; "
                            "printf '__ZFSMGR_GUID__:%s\\n' \"$guid\"; "
                            "printf '__ZFSMGR_STATUS_BEGIN__\\n'; "
                            "zpool status -v \"$pool\" 2>&1 || true; "
                            "printf '__ZFSMGR_STATUS_END__\\n'; "
                            "done")});
        p.start();
        if (!p.waitForFinished(60000)) {
            QTextStream(stderr) << "agent timeout running zpool guid/status batch\n";
            return 124;
        }
        QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            QTextStream(stderr) << QString::fromUtf8(err);
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zpool-guid"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString pool = args.at(i + 1).trimmed();
            if (pool.isEmpty()) {
                QTextStream(stderr) << "missing pool name for --dump-zpool-guid\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("zpool"));
            p.setArguments({QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"), QStringLiteral("value"),
                            QStringLiteral("guid"), pool});
            p.start();
            if (!p.waitForFinished(15000)) {
                QTextStream(stderr) << "agent timeout running zpool get guid\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zpool-status"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString pool = args.at(i + 1).trimmed();
            if (pool.isEmpty()) {
                QTextStream(stderr) << "missing pool name for --dump-zpool-status\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("zpool"));
            p.setArguments({QStringLiteral("status"), QStringLiteral("-v"), pool});
            p.start();
            if (!p.waitForFinished(20000)) {
                QTextStream(stderr) << "agent timeout running zpool status -v\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zpool-status-p"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString pool = args.at(i + 1).trimmed();
            if (pool.isEmpty()) {
                QTextStream(stderr) << "missing pool name for --dump-zpool-status-p\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("sh"));
            p.setArguments({QStringLiteral("-lc"),
                            QStringLiteral("zpool status -P \"$1\""),
                            QStringLiteral("--"),
                            pool});
            p.start();
            if (!p.waitForFinished(20000)) {
                QTextStream(stderr) << "agent timeout running zpool status -P\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zpool-get-all"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString pool = args.at(i + 1).trimmed();
            if (pool.isEmpty()) {
                QTextStream(stderr) << "missing pool name for --dump-zpool-get-all\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("sh"));
            p.setArguments({QStringLiteral("-lc"),
                            QStringLiteral("zpool get -j all \"$1\""),
                            QStringLiteral("--"),
                            pool});
            p.start();
            if (!p.waitForFinished(20000)) {
                QTextStream(stderr) << "agent timeout running zpool get -j all\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    if (args.contains(QStringLiteral("--dump-zpool-import-probe"))) {
        QProcess p;
        p.setProgram(QStringLiteral("sh"));
        p.setArguments({QStringLiteral("-lc"), QStringLiteral("zpool import; zpool import -s")});
        p.start();
        if (!p.waitForFinished(25000)) {
            QTextStream(stderr) << "agent timeout running zpool import probe\n";
            return 124;
        }
        QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            QTextStream(stderr) << QString::fromUtf8(err);
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zfs-list-all"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString pool = args.at(i + 1).trimmed();
            if (pool.isEmpty()) {
                QTextStream(stderr) << "missing pool name for --dump-zfs-list-all\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("sh"));
            p.setArguments({QStringLiteral("-lc"),
                            QStringLiteral(
                                "pool=$1; "
                                "if LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot "
                                "type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount \"$pool\" >/dev/null 2>&1; then "
                                "  LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot "
                                "type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount \"$pool\"; "
                                "elif LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot "
                                "type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount \"$pool\" >/dev/null 2>&1; then "
                                "  LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot "
                                "type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount \"$pool\"; "
                                "elif zfs get -j -p -r -t filesystem,volume,snapshot "
                                "type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount \"$pool\" >/dev/null 2>&1; then "
                                "  zfs get -j -p -r -t filesystem,volume,snapshot "
                                "type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount \"$pool\"; "
                                "else "
                                "  zfs list -H -p -t filesystem,volume,snapshot "
                                "-o name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r \"$pool\"; "
                                "fi"),
                            QStringLiteral("--"),
                            pool});
            p.start();
            if (!p.waitForFinished(45000)) {
                QTextStream(stderr) << "agent timeout running zfs list all\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zfs-guid-map"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString pool = args.at(i + 1).trimmed();
            if (pool.isEmpty()) {
                QTextStream(stderr) << "missing pool name for --dump-zfs-guid-map\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("zfs"));
            p.setArguments({QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"),
                            QStringLiteral("name,value"), QStringLiteral("guid"), QStringLiteral("-r"), pool});
            p.start();
            if (!p.waitForFinished(25000)) {
                QTextStream(stderr) << "agent timeout running zfs guid map\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zfs-get-prop"));
        if (i >= 0 && i + 2 < args.size()) {
            const QString prop = args.at(i + 1).trimmed();
            const QString obj = args.at(i + 2).trimmed();
            if (prop.isEmpty() || obj.isEmpty()) {
                QTextStream(stderr) << "missing args for --dump-zfs-get-prop <prop> <obj>\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("zfs"));
            p.setArguments({QStringLiteral("get"), QStringLiteral("-H"), QStringLiteral("-o"), QStringLiteral("value"), prop, obj});
            p.start();
            if (!p.waitForFinished(15000)) {
                QTextStream(stderr) << "agent timeout running zfs get prop\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zfs-get-all"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString obj = args.at(i + 1).trimmed();
            if (obj.isEmpty()) {
                QTextStream(stderr) << "missing object for --dump-zfs-get-all\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("sh"));
            p.setArguments({QStringLiteral("-lc"),
                            QStringLiteral(
                                "obj=$1; "
                                "if LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j all \"$obj\" >/dev/null 2>&1; then "
                                "  LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j all \"$obj\"; "
                                "elif LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j all \"$obj\" >/dev/null 2>&1; then "
                                "  LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j all \"$obj\"; "
                                "else "
                                "  zfs get -j all \"$obj\"; "
                                "fi"),
                            QStringLiteral("--"),
                            obj});
            p.start();
            if (!p.waitForFinished(20000)) {
                QTextStream(stderr) << "agent timeout running zfs get all -j\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zfs-get-json"));
        if (i >= 0 && i + 2 < args.size()) {
            const QString props = args.at(i + 1).trimmed();
            const QString obj = args.at(i + 2).trimmed();
            if (props.isEmpty() || obj.isEmpty()) {
                QTextStream(stderr) << "missing args for --dump-zfs-get-json <props> <obj>\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("sh"));
            p.setArguments({QStringLiteral("-lc"),
                            QStringLiteral(
                                "props=$1; obj=$2; "
                                "if LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j \"$props\" \"$obj\" >/dev/null 2>&1; then "
                                "  LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j \"$props\" \"$obj\"; "
                                "elif LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j \"$props\" \"$obj\" >/dev/null 2>&1; then "
                                "  LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j \"$props\" \"$obj\"; "
                                "else "
                                "  zfs get -j \"$props\" \"$obj\"; "
                                "fi"),
                            QStringLiteral("--"),
                            props,
                            obj});
            p.start();
            if (!p.waitForFinished(20000)) {
                QTextStream(stderr) << "agent timeout running zfs get -j subset\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    if (args.contains(QStringLiteral("--dump-zfs-get-gsa-raw-all-pools"))) {
        QProcess p;
        p.setProgram(QStringLiteral("sh"));
        p.setArguments({QStringLiteral("-lc"),
                        QStringLiteral(
                            "props='org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino'; "
                            "zpool list -H -o name 2>/dev/null | while IFS= read -r pool; do "
                            "  [ -n \"$pool\" ] || continue; "
                            "  zfs get -H -o name,property,value -r \"$props\" \"$pool\" 2>/dev/null || true; "
                            "done")});
        p.start();
        if (!p.waitForFinished(30000)) {
            QTextStream(stderr) << "agent timeout running gsa raw scan\n";
            return 124;
        }
        QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            QTextStream(stderr) << QString::fromUtf8(err);
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
    }
    {
        const int i = args.indexOf(QStringLiteral("--dump-zfs-get-gsa-raw-recursive"));
        if (i >= 0 && i + 1 < args.size()) {
            const QString pool = args.at(i + 1).trimmed();
            if (pool.isEmpty()) {
                QTextStream(stderr) << "missing pool for --dump-zfs-get-gsa-raw-recursive\n";
                return 2;
            }
            QProcess p;
            p.setProgram(QStringLiteral("sh"));
            p.setArguments({QStringLiteral("-lc"),
                            QStringLiteral(
                                "zfs get -H -o name,property,value,source -r "
                                "org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,"
                                "org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,"
                                "org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino "
                                "\"$1\""),
                            QStringLiteral("--"),
                            pool});
            p.start();
            if (!p.waitForFinished(30000)) {
                QTextStream(stderr) << "agent timeout running gsa recursive scan\n";
                return 124;
            }
            QTextStream(stdout) << QString::fromUtf8(p.readAllStandardOutput());
            const QByteArray err = p.readAllStandardError();
            if (!err.isEmpty()) {
                QTextStream(stderr) << QString::fromUtf8(err);
            }
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 125;
        }
    }
    if (args.contains(QStringLiteral("--dump-gsa-connections-conf"))) {
        QFile f(QStringLiteral("/etc/zfsmgr/gsa-connections.conf"));
        if (!f.exists()) {
            return 0;
        }
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream(stderr) << "cannot open /etc/zfsmgr/gsa-connections.conf\n";
            return 1;
        }
        QTextStream(stdout) << QString::fromUtf8(f.readAll());
        return 0;
    }

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, []() { writeHeartbeat(); });
    timer.start(60000);
    writeHeartbeat();
    return app.exec();
}
