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

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, []() { writeHeartbeat(); });
    timer.start(60000);
    writeHeartbeat();
    return app.exec();
}
