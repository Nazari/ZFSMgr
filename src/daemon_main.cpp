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

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, []() { writeHeartbeat(); });
    timer.start(60000);
    writeHeartbeat();
    return app.exec();
}
