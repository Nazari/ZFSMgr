#include "agentversion.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <QTimer>

namespace {

void writeHeartbeat() {
    QFile f(QStringLiteral("/tmp/zfsmgr-agent-heartbeat.log"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << " agent alive\n";
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--version")) || args.contains(QStringLiteral("version"))) {
        QTextStream(stdout) << agentversion::currentVersion() << '\n';
        return 0;
    }
    if (args.contains(QStringLiteral("--api-version")) || args.contains(QStringLiteral("api"))) {
        QTextStream(stdout) << agentversion::expectedApiVersion() << '\n';
        return 0;
    }
    if (args.contains(QStringLiteral("--once"))) {
        writeHeartbeat();
        return 0;
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

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, []() { writeHeartbeat(); });
    timer.start(60000);
    writeHeartbeat();
    return app.exec();
}
