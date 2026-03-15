#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHostInfo>
#include <QLibrary>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cstring>

namespace {
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

QString sanitizePsrpText(const QString& raw) {
    QString s = raw;
    if (s.isEmpty()) {
        return s;
    }
    s.replace(QStringLiteral("#< CLIXML"), QStringLiteral(""));
    // PowerShell remoting escaping: _x000A_, _x001B_, ...
    s.replace(QRegularExpression(QStringLiteral("_x[0-9A-Fa-f]{4}_")), QStringLiteral(""));
    // Keep payload text, drop XML tags if present.
    s.replace(QRegularExpression(QStringLiteral("<[^>]+>")), QStringLiteral(""));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    return s.trimmed();
}

bool shouldRetrySshWithoutMultiplexing(const QString& stderrText) {
    const QString lowered = stderrText.toLower();
    return lowered.contains(QStringLiteral("getsockname failed"))
        || lowered.contains(QStringLiteral("not a socket"))
        || lowered.contains(QStringLiteral("bad stdio forwarding specification"));
}

using mwhelpers::isMountedValueTrue;
using mwhelpers::looksLikePowerShellScript;
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

namespace {
struct LocalLibzfsOps {
    using InitFn = void* (*)();
    using FiniFn = void (*)(void*);
    using ZfsOpenFn = void* (*)(void*, const char*, int);
    using ZfsCloseFn = void (*)(void*);
    using ZfsMountFn = int (*)(void*, const char*, int);
    using ZfsUnmountFn = int (*)(void*, const char*, int);
    using ZfsRenameFn = int (*)(void*, const char*, bool, bool, bool);
    using ZfsPropSetFn = int (*)(void*, const char*, const char*);
    using ZfsPropInheritFn = int (*)(void*, const char*, bool);
    using ErrDescFn = const char* (*)(void*);

    QLibrary lib;
    QString candidate;
    InitFn initFn{nullptr};
    FiniFn finiFn{nullptr};
    ZfsOpenFn zfsOpenFn{nullptr};
    ZfsCloseFn zfsCloseFn{nullptr};
    ZfsMountFn zfsMountFn{nullptr};
    ZfsUnmountFn zfsUnmountFn{nullptr};
    ZfsRenameFn zfsRenameFn{nullptr};
    ZfsPropSetFn zfsPropSetFn{nullptr};
    ZfsPropInheritFn zfsPropInheritFn{nullptr};
    ErrDescFn errDescFn{nullptr};
};

bool loadLocalLibzfsOps(LocalLibzfsOps& ops, QString* detail) {
#if defined(Q_OS_WIN)
    Q_UNUSED(ops);
    if (detail) {
        *detail = QStringLiteral("libzfs runtime operations not available on Windows build");
    }
    return false;
#else
    QStringList candidates;
#if defined(Q_OS_MACOS)
    candidates << QStringLiteral("/usr/local/zfs/lib/libzfs.dylib")
               << QStringLiteral("libzfs.dylib");
#else
    candidates << QStringLiteral("libzfs.so.7")
               << QStringLiteral("libzfs.so.6")
               << QStringLiteral("libzfs.so.5")
               << QStringLiteral("libzfs.so.4")
               << QStringLiteral("libzfs.so");
#endif
    QString localDetail = QStringLiteral("no loadable libzfs library found");
    for (const QString& cand : candidates) {
        ops.lib.setFileName(cand);
        if (!ops.lib.load()) {
            continue;
        }
        ops.candidate = cand;
        ops.initFn = reinterpret_cast<LocalLibzfsOps::InitFn>(ops.lib.resolve("libzfs_init"));
        ops.finiFn = reinterpret_cast<LocalLibzfsOps::FiniFn>(ops.lib.resolve("libzfs_fini"));
        ops.zfsOpenFn = reinterpret_cast<LocalLibzfsOps::ZfsOpenFn>(ops.lib.resolve("zfs_open"));
        ops.zfsCloseFn = reinterpret_cast<LocalLibzfsOps::ZfsCloseFn>(ops.lib.resolve("zfs_close"));
        ops.zfsMountFn = reinterpret_cast<LocalLibzfsOps::ZfsMountFn>(ops.lib.resolve("zfs_mount"));
        ops.zfsUnmountFn = reinterpret_cast<LocalLibzfsOps::ZfsUnmountFn>(ops.lib.resolve("zfs_unmount"));
        ops.zfsRenameFn = reinterpret_cast<LocalLibzfsOps::ZfsRenameFn>(ops.lib.resolve("zfs_rename"));
        ops.zfsPropSetFn = reinterpret_cast<LocalLibzfsOps::ZfsPropSetFn>(ops.lib.resolve("zfs_prop_set"));
        ops.zfsPropInheritFn = reinterpret_cast<LocalLibzfsOps::ZfsPropInheritFn>(ops.lib.resolve("zfs_prop_inherit"));
        ops.errDescFn = reinterpret_cast<LocalLibzfsOps::ErrDescFn>(ops.lib.resolve("libzfs_error_description"));
        if (!ops.initFn || !ops.finiFn || !ops.zfsOpenFn || !ops.zfsCloseFn) {
            localDetail = QStringLiteral("%1 loaded but required symbols are missing").arg(cand);
            ops.lib.unload();
            continue;
        }
        if (detail) {
            *detail = QStringLiteral("%1 loaded").arg(cand);
        }
        return true;
    }
    if (detail) {
        *detail = localDetail;
    }
    return false;
#endif
}

QString localLibzfsError(LocalLibzfsOps& ops, void* handle) {
    if (!ops.errDescFn || !handle) {
        return QString();
    }
    const char* msg = ops.errDescFn(handle);
    if (!msg || !*msg) {
        return QString();
    }
    return QString::fromUtf8(msg);
}
} // namespace

bool MainWindow::runSsh(const ConnectionProfile& p,
                        const QString& remoteCmd,
                        int timeoutMs,
                        QString& out,
                        QString& err,
                        int& rc,
                        const std::function<void(const QString&)>& onStdoutLine,
                        const std::function<void(const QString&)>& onStderrLine,
                        const std::function<void(int)>& onIdleTimeoutRemaining,
                        MainWindow::WindowsCommandMode windowsMode) {
    out.clear();
    err.clear();
    rc = -1;

    if (isLocalConnection(p)) {
        const QString localCmd = remoteCmd.trimmed();
        const QString cmdLine = QStringLiteral("[local] $ %1").arg(localCmd);
        appLog(QStringLiteral("INFO"), cmdLine);
        appendConnectionLog(p.id, cmdLine);

        QProcess proc;
        QString program;
        QStringList args;
#ifdef Q_OS_WIN
        program = QStringLiteral("cmd.exe");
        args << "/C" << wrapRemoteCommand(p, localCmd, windowsMode);
#else
        program = QStringLiteral("sh");
        args << "-lc" << localCmd;
#endif
        QElapsedTimer timer;
        timer.start();
        proc.start(program, args);
        if (!proc.waitForStarted(4000)) {
            err = QStringLiteral("No se pudo iniciar %1").arg(program);
            appendConnectionLog(p.id, err);
            return false;
        }
        QString outLineBuf;
        QString errLineBuf;
        auto flushLines = [&](QString& buf, const QString& chunk, const std::function<void(const QString&)>& cb) {
            if (!chunk.isEmpty()) {
                buf += chunk;
            }
            int nl = -1;
            while ((nl = buf.indexOf('\n')) >= 0) {
                QString line = buf.left(nl);
                buf.remove(0, nl + 1);
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
        if (!out.trimmed().isEmpty()) {
            appendConnectionLog(p.id, oneLine(out));
        }
        if (!err.trimmed().isEmpty()) {
            appendConnectionLog(p.id, oneLine(err));
        }
        return true;
    }

    if (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
        QString program = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
        if (program.isEmpty()) {
            program = QStandardPaths::findExecutable(QStringLiteral("powershell"));
        }
        if (program.isEmpty()) {
            err = QStringLiteral("No se encontró pwsh/powershell para PSRP");
            appendConnectionLog(p.id, err);
            return false;
        }

        const QString hostEsc = QString(p.host).replace('\'', QStringLiteral("''"));
        const QString userEsc = QString(p.username).replace('\'', QStringLiteral("''"));
        const QString wrappedRemoteCmd = wrapRemoteCommand(p, remoteCmd, windowsMode);
        const QString remoteB64 = QString::fromLatin1(wrappedRemoteCmd.toUtf8().toBase64());
        const QString passB64 = QString::fromLatin1(p.password.toUtf8().toBase64());
        const int port = (p.port > 0) ? p.port : 5986;
        const QString script = QStringLiteral(
            "$remote=[System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%1')); "
            "$pwd=[System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%2')); "
            "$sec=ConvertTo-SecureString $pwd -AsPlainText -Force; "
            "$cred=New-Object System.Management.Automation.PSCredential('%3',$sec); "
            "$so=$null; "
            "try { $so=New-PSSessionOption -SkipCACheck -SkipCNCheck -SkipRevocationCheck } "
            "catch { $so=New-PSSessionOption -SkipCACheck -SkipCNCheck }; "
            "$res=$null; "
            "try { "
            "  $res=Invoke-Command -ComputerName '%4' -Port %5 -UseSSL -Authentication Negotiate -Credential $cred -SessionOption $so "
            "    -ScriptBlock { param($cmd) & ([ScriptBlock]::Create($cmd)); if($LASTEXITCODE -ne $null){ exit $LASTEXITCODE } } "
            "    -ArgumentList $remote -ErrorAction Stop 2>&1 "
            "} catch { "
            "  $res=Invoke-Command -ComputerName '%4' -Port %5 -UseSSL -Authentication Basic -Credential $cred -SessionOption $so "
            "    -ScriptBlock { param($cmd) & ([ScriptBlock]::Create($cmd)); if($LASTEXITCODE -ne $null){ exit $LASTEXITCODE } } "
            "    -ArgumentList $remote -ErrorAction Stop 2>&1 "
            "}; "
            "$rc=$LASTEXITCODE; "
            "$res | ForEach-Object { $_.ToString() }; "
            "if($rc -eq $null){ $rc=0 }; "
            "exit [int]$rc;")
                                   .arg(remoteB64,
                                        passB64,
                                        userEsc,
                                        hostEsc,
                                        QString::number(port));

        const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
        const QString encoded = QString::fromLatin1(utf16.toBase64());
        QStringList args;
        args << "-NoProfile" << "-NonInteractive" << "-EncodedCommand" << encoded;

        const QString cmdLine = QStringLiteral("%1@%2:%3 [PSRP] $ %4")
                                    .arg(p.username, p.host)
                                    .arg(port)
                                    .arg(remoteCmd);
        appLog(QStringLiteral("INFO"), cmdLine);
        appendConnectionLog(p.id, cmdLine);

        QProcess proc;
        QElapsedTimer timer;
        timer.start();
        proc.start(program, args);
        if (!proc.waitForStarted(4000)) {
            err = QStringLiteral("No se pudo iniciar %1").arg(program);
            appendConnectionLog(p.id, err);
            return false;
        }

        QString outLineBuf;
        QString errLineBuf;
        auto flushLines = [&](QString& buf, const QString& chunk, const std::function<void(const QString&)>& cb) {
            if (!chunk.isEmpty()) {
                buf += chunk;
            }
            int nl = -1;
            while ((nl = buf.indexOf('\n')) >= 0) {
                QString line = buf.left(nl);
                buf.remove(0, nl + 1);
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
        out = sanitizePsrpText(out);
        err = sanitizePsrpText(err);
        const QString mergedPsrp = (out + QStringLiteral("\n") + err);
        if (mergedPsrp.contains(QStringLiteral("no supported wsman client library"), Qt::CaseInsensitive)
            || mergedPsrp.contains(QStringLiteral("requires WSMan"), Qt::CaseInsensitive)) {
            rc = -1;
            err = QStringLiteral("PSRP no disponible en este host: falta WSMan client library (instale PSWSMan/Install-WSMan en PowerShell local).");
            appendConnectionLog(p.id, oneLine(err));
            return false;
        }
        if (!out.trimmed().isEmpty()) {
            appendConnectionLog(p.id, oneLine(out));
        }
        if (!err.trimmed().isEmpty()) {
            appendConnectionLog(p.id, oneLine(err));
        }
        return true;
    }

    const bool hasPassword = !p.password.trimmed().isEmpty();
    QString program = QStringLiteral("ssh");
    QStringList sshpassPrefixArgs;
    bool usingSshpass = false;
    if (hasPassword) {
        const QString sshpassExe = QStandardPaths::findExecutable(QStringLiteral("sshpass"));
        if (!sshpassExe.isEmpty()) {
            program = sshpassExe;
            sshpassPrefixArgs << "-p" << p.password << "ssh";
            usingSshpass = true;
        }
    }

    const QString wrappedCmd = wrapRemoteCommand(p, remoteCmd, windowsMode);
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
        args << "-o" << "StrictHostKeyChecking=no";
        args << "-o" << "UserKnownHostsFile=/dev/null";
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
        QString outLineBuf;
        QString errLineBuf;
        auto flushLines = [&](QString& buf, const QString& chunk, const std::function<void(const QString&)>& cb) {
            if (!chunk.isEmpty()) {
                buf += chunk;
            }
            int nl = -1;
            while ((nl = buf.indexOf('\n')) >= 0) {
                QString line = buf.left(nl);
                buf.remove(0, nl + 1);
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
        if (!m_loggedSshResolutionKeys.contains(sshResolutionKey)) {
            m_loggedSshResolutionKeys.insert(sshResolutionKey);
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

    const bool allowMultiplexing = !m_sshDisableMultiplexKeys.contains(sshConnKey);
    bool startedOk = runSshAttempt(allowMultiplexing, out, err, rc);
    if (allowMultiplexing && startedOk && rc != 0 && shouldRetrySshWithoutMultiplexing(err)) {
        m_sshDisableMultiplexKeys.insert(sshConnKey);
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
        args << "-o" << "StrictHostKeyChecking=no";
        args << "-o" << "UserKnownHostsFile=/dev/null";
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

bool MainWindow::detectLocalLibzfs(QString* detail) const {
    if (m_localLibzfsChecked) {
        if (detail) {
            *detail = m_localLibzfsDetail;
        }
        return m_localLibzfsAvailable;
    }
    m_localLibzfsChecked = true;
    m_localLibzfsAvailable = false;
    m_localLibzfsDetail = QStringLiteral("not checked");

#if defined(Q_OS_WIN)
    m_localLibzfsDetail = QStringLiteral("libzfs runtime check not available on Windows build");
    if (detail) {
        *detail = m_localLibzfsDetail;
    }
    return false;
#else
    QStringList candidates;
#if defined(Q_OS_MACOS)
    candidates << QStringLiteral("/usr/local/zfs/lib/libzfs.dylib")
               << QStringLiteral("libzfs.dylib");
#else
    candidates << QStringLiteral("libzfs.so.7")
               << QStringLiteral("libzfs.so.6")
               << QStringLiteral("libzfs.so.5")
               << QStringLiteral("libzfs.so.4")
               << QStringLiteral("libzfs.so");
#endif
    for (const QString& cand : candidates) {
        QLibrary lib(cand);
        if (!lib.load()) {
            continue;
        }
        using InitFn = void* (*)();
        using FiniFn = void (*)(void*);
        const InitFn initFn = reinterpret_cast<InitFn>(lib.resolve("libzfs_init"));
        const FiniFn finiFn = reinterpret_cast<FiniFn>(lib.resolve("libzfs_fini"));
        if (!initFn || !finiFn) {
            lib.unload();
            continue;
        }
        void* h = initFn();
        if (!h) {
            m_localLibzfsDetail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(cand);
            lib.unload();
            continue;
        }
        finiFn(h);
        m_localLibzfsAvailable = true;
        m_localLibzfsDetail = QStringLiteral("%1 loaded and initialized").arg(cand);
        lib.unload();
        break;
    }
    if (!m_localLibzfsAvailable && m_localLibzfsDetail == QStringLiteral("not checked")) {
        m_localLibzfsDetail = QStringLiteral("no loadable libzfs library found");
    }
    if (detail) {
        *detail = m_localLibzfsDetail;
    }
    return m_localLibzfsAvailable;
#endif
}

bool MainWindow::localLibzfsMountDataset(const QString& dataset, QString* detail) const {
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime mount not available on Windows build");
    }
    Q_UNUSED(dataset);
    return false;
#else
    LocalLibzfsOps ops;
    if (!loadLocalLibzfsOps(ops, detail)) {
        return false;
    }
    if (!ops.zfsMountFn) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but zfs_mount symbol is missing").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    void* h = ops.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    const QByteArray ds = dataset.toUtf8();
    constexpr int kZfsTypeAny = -1;
    void* zhp = ops.zfsOpenFn(h, ds.constData(), kZfsTypeAny);
    if (!zhp) {
        const QString err = localLibzfsError(ops, h);
        if (detail) {
            *detail = err.isEmpty()
                          ? QStringLiteral("zfs_open(%1) failed").arg(dataset)
                          : QStringLiteral("zfs_open(%1) failed: %2").arg(dataset, err);
        }
        ops.finiFn(h);
        ops.lib.unload();
        return false;
    }
    const int rc = ops.zfsMountFn(zhp, nullptr, 0);
    const QString err = (rc == 0) ? QString() : localLibzfsError(ops, h);
    ops.zfsCloseFn(zhp);
    ops.finiFn(h);
    ops.lib.unload();
    if (detail) {
        *detail = (rc == 0)
                      ? QStringLiteral("zfs_mount(%1) ok").arg(dataset)
                      : (err.isEmpty()
                             ? QStringLiteral("zfs_mount(%1) failed (rc=%2)").arg(dataset).arg(rc)
                             : QStringLiteral("zfs_mount(%1) failed: %2").arg(dataset, err));
    }
    return rc == 0;
#endif
}

bool MainWindow::localLibzfsUnmountDataset(const QString& dataset, QString* detail) const {
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime unmount not available on Windows build");
    }
    Q_UNUSED(dataset);
    return false;
#else
    LocalLibzfsOps ops;
    if (!loadLocalLibzfsOps(ops, detail)) {
        return false;
    }
    if (!ops.zfsUnmountFn) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but zfs_unmount symbol is missing").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    void* h = ops.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    const QByteArray ds = dataset.toUtf8();
    constexpr int kZfsTypeAny = -1;
    void* zhp = ops.zfsOpenFn(h, ds.constData(), kZfsTypeAny);
    if (!zhp) {
        const QString err = localLibzfsError(ops, h);
        if (detail) {
            *detail = err.isEmpty()
                          ? QStringLiteral("zfs_open(%1) failed").arg(dataset)
                          : QStringLiteral("zfs_open(%1) failed: %2").arg(dataset, err);
        }
        ops.finiFn(h);
        ops.lib.unload();
        return false;
    }
    const int rc = ops.zfsUnmountFn(zhp, nullptr, 0);
    const QString err = (rc == 0) ? QString() : localLibzfsError(ops, h);
    ops.zfsCloseFn(zhp);
    ops.finiFn(h);
    ops.lib.unload();
    if (detail) {
        *detail = (rc == 0)
                      ? QStringLiteral("zfs_unmount(%1) ok").arg(dataset)
                      : (err.isEmpty()
                             ? QStringLiteral("zfs_unmount(%1) failed (rc=%2)").arg(dataset).arg(rc)
                             : QStringLiteral("zfs_unmount(%1) failed: %2").arg(dataset, err));
    }
    return rc == 0;
#endif
}

bool MainWindow::localLibzfsRenameDataset(const QString& oldName, const QString& newName, QString* detail) const {
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime rename not available on Windows build");
    }
    Q_UNUSED(oldName);
    Q_UNUSED(newName);
    return false;
#else
    LocalLibzfsOps ops;
    if (!loadLocalLibzfsOps(ops, detail)) {
        return false;
    }
    if (!ops.zfsRenameFn) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but zfs_rename symbol is missing").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    void* h = ops.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    const QByteArray oldDs = oldName.toUtf8();
    const QByteArray newDs = newName.toUtf8();
    constexpr int kZfsTypeAny = -1;
    void* zhp = ops.zfsOpenFn(h, oldDs.constData(), kZfsTypeAny);
    if (!zhp) {
        const QString err = localLibzfsError(ops, h);
        if (detail) {
            *detail = err.isEmpty()
                          ? QStringLiteral("zfs_open(%1) failed").arg(oldName)
                          : QStringLiteral("zfs_open(%1) failed: %2").arg(oldName, err);
        }
        ops.finiFn(h);
        ops.lib.unload();
        return false;
    }
    const int rc = ops.zfsRenameFn(zhp, newDs.constData(), false, false, false);
    const QString err = (rc == 0) ? QString() : localLibzfsError(ops, h);
    ops.zfsCloseFn(zhp);
    ops.finiFn(h);
    ops.lib.unload();
    if (detail) {
        *detail = (rc == 0)
                      ? QStringLiteral("zfs_rename(%1 -> %2) ok").arg(oldName, newName)
                      : (err.isEmpty()
                             ? QStringLiteral("zfs_rename(%1 -> %2) failed (rc=%3)").arg(oldName, newName).arg(rc)
                             : QStringLiteral("zfs_rename(%1 -> %2) failed: %3").arg(oldName, newName, err));
    }
    return rc == 0;
#endif
}

bool MainWindow::localLibzfsSetProperty(const QString& dataset, const QString& prop, const QString& value, QString* detail) const {
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime set property not available on Windows build");
    }
    Q_UNUSED(dataset);
    Q_UNUSED(prop);
    Q_UNUSED(value);
    return false;
#else
    LocalLibzfsOps ops;
    if (!loadLocalLibzfsOps(ops, detail)) {
        return false;
    }
    if (!ops.zfsPropSetFn) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but zfs_prop_set symbol is missing").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    void* h = ops.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    const QByteArray ds = dataset.toUtf8();
    const QByteArray propBa = prop.toUtf8();
    const QByteArray valueBa = value.toUtf8();
    constexpr int kZfsTypeAny = -1;
    void* zhp = ops.zfsOpenFn(h, ds.constData(), kZfsTypeAny);
    if (!zhp) {
        const QString err = localLibzfsError(ops, h);
        if (detail) {
            *detail = err.isEmpty()
                          ? QStringLiteral("zfs_open(%1) failed").arg(dataset)
                          : QStringLiteral("zfs_open(%1) failed: %2").arg(dataset, err);
        }
        ops.finiFn(h);
        ops.lib.unload();
        return false;
    }
    const int rc = ops.zfsPropSetFn(zhp, propBa.constData(), valueBa.constData());
    const QString err = (rc == 0) ? QString() : localLibzfsError(ops, h);
    ops.zfsCloseFn(zhp);
    ops.finiFn(h);
    ops.lib.unload();
    if (detail) {
        *detail = (rc == 0)
                      ? QStringLiteral("zfs_prop_set(%1=%2 on %3) ok").arg(prop, value, dataset)
                      : (err.isEmpty()
                             ? QStringLiteral("zfs_prop_set(%1=%2 on %3) failed (rc=%4)").arg(prop, value, dataset).arg(rc)
                             : QStringLiteral("zfs_prop_set(%1=%2 on %3) failed: %4").arg(prop, value, dataset, err));
    }
    return rc == 0;
#endif
}

bool MainWindow::localLibzfsInheritProperty(const QString& dataset, const QString& prop, QString* detail) const {
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime inherit property not available on Windows build");
    }
    Q_UNUSED(dataset);
    Q_UNUSED(prop);
    return false;
#else
    LocalLibzfsOps ops;
    if (!loadLocalLibzfsOps(ops, detail)) {
        return false;
    }
    if (!ops.zfsPropInheritFn) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but zfs_prop_inherit symbol is missing").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    void* h = ops.initFn();
    if (!h) {
        if (detail) {
            *detail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(ops.candidate);
        }
        ops.lib.unload();
        return false;
    }
    const QByteArray ds = dataset.toUtf8();
    const QByteArray propBa = prop.toUtf8();
    constexpr int kZfsTypeAny = -1;
    void* zhp = ops.zfsOpenFn(h, ds.constData(), kZfsTypeAny);
    if (!zhp) {
        const QString err = localLibzfsError(ops, h);
        if (detail) {
            *detail = err.isEmpty()
                          ? QStringLiteral("zfs_open(%1) failed").arg(dataset)
                          : QStringLiteral("zfs_open(%1) failed: %2").arg(dataset, err);
        }
        ops.finiFn(h);
        ops.lib.unload();
        return false;
    }
    const int rc = ops.zfsPropInheritFn(zhp, propBa.constData(), false);
    const QString err = (rc == 0) ? QString() : localLibzfsError(ops, h);
    ops.zfsCloseFn(zhp);
    ops.finiFn(h);
    ops.lib.unload();
    if (detail) {
        *detail = (rc == 0)
                      ? QStringLiteral("zfs_prop_inherit(%1 on %2) ok").arg(prop, dataset)
                      : (err.isEmpty()
                             ? QStringLiteral("zfs_prop_inherit(%1 on %2) failed (rc=%3)").arg(prop, dataset).arg(rc)
                             : QStringLiteral("zfs_prop_inherit(%1 on %2) failed: %3").arg(prop, dataset, err));
    }
    return rc == 0;
#endif
}

bool MainWindow::listLocalImportedPoolsLibzfs(QStringList& poolsOut, QString* detail) const {
    poolsOut.clear();
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime pool listing not available on Windows build");
    }
    return false;
#else
    QStringList candidates;
#if defined(Q_OS_MACOS)
    candidates << QStringLiteral("/usr/local/zfs/lib/libzfs.dylib")
               << QStringLiteral("libzfs.dylib");
#else
    candidates << QStringLiteral("libzfs.so.7")
               << QStringLiteral("libzfs.so.6")
               << QStringLiteral("libzfs.so.5")
               << QStringLiteral("libzfs.so.4")
               << QStringLiteral("libzfs.so");
#endif

    QString localDetail = QStringLiteral("no loadable libzfs library found");
    for (const QString& cand : candidates) {
        QLibrary lib(cand);
        if (!lib.load()) {
            continue;
        }
        using InitFn = void* (*)();
        using FiniFn = void (*)(void*);
        using IterCb = int (*)(void*, void*);
        using ZpoolIterFn = int (*)(void*, IterCb, void*);
        using ZpoolGetNameFn = const char* (*)(void*);
        using ZpoolCloseFn = void (*)(void*);

        const InitFn initFn = reinterpret_cast<InitFn>(lib.resolve("libzfs_init"));
        const FiniFn finiFn = reinterpret_cast<FiniFn>(lib.resolve("libzfs_fini"));
        const ZpoolIterFn zpoolIterFn = reinterpret_cast<ZpoolIterFn>(lib.resolve("zpool_iter"));
        const ZpoolGetNameFn zpoolGetNameFn =
            reinterpret_cast<ZpoolGetNameFn>(lib.resolve("zpool_get_name"));
        const ZpoolCloseFn zpoolCloseFn = reinterpret_cast<ZpoolCloseFn>(lib.resolve("zpool_close"));
        if (!initFn || !finiFn || !zpoolIterFn || !zpoolGetNameFn || !zpoolCloseFn) {
            localDetail = QStringLiteral("%1 loaded but required symbols are missing").arg(cand);
            lib.unload();
            continue;
        }

        void* h = initFn();
        if (!h) {
            localDetail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(cand);
            lib.unload();
            continue;
        }

        struct Ctx {
            QStringList* pools;
            ZpoolGetNameFn getName;
            ZpoolCloseFn closePool;
        } ctx{&poolsOut, zpoolGetNameFn, zpoolCloseFn};

        auto cb = [](void* poolHandle, void* opaque) -> int {
            Ctx* c = static_cast<Ctx*>(opaque);
            if (!c || !poolHandle || !c->getName || !c->closePool) {
                return 0;
            }
            const char* n = c->getName(poolHandle);
            if (n && *n) {
                c->pools->push_back(QString::fromUtf8(n));
            }
            c->closePool(poolHandle);
            return 0;
        };

        (void)zpoolIterFn(h, cb, &ctx);
        finiFn(h);
        lib.unload();
        poolsOut.removeAll(QString());
        poolsOut.removeDuplicates();
        std::sort(poolsOut.begin(), poolsOut.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
        localDetail = QStringLiteral("%1 loaded and zpool_iter succeeded").arg(cand);
        if (detail) {
            *detail = localDetail;
        }
        return true;
    }

    if (detail) {
        *detail = localDetail;
    }
    return false;
#endif
}

bool MainWindow::listLocalDatasetsLibzfs(const QString& poolName, PoolDatasetCache& cacheOut, QString* detail) const {
    cacheOut.datasets.clear();
    cacheOut.snapshotsByDataset.clear();
    cacheOut.recordByName.clear();
    cacheOut.driveletterByDataset.clear();
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime dataset listing not available on Windows build");
    }
    return false;
#else
    QStringList candidates;
#if defined(Q_OS_MACOS)
    candidates << QStringLiteral("/usr/local/zfs/lib/libzfs.dylib")
               << QStringLiteral("libzfs.dylib");
#else
    candidates << QStringLiteral("libzfs.so.7")
               << QStringLiteral("libzfs.so.6")
               << QStringLiteral("libzfs.so.5")
               << QStringLiteral("libzfs.so.4")
               << QStringLiteral("libzfs.so");
#endif

    QString localDetail = QStringLiteral("no loadable libzfs library found");
    for (const QString& cand : candidates) {
        QLibrary lib(cand);
        if (!lib.load()) {
            continue;
        }

        using InitFn = void* (*)();
        using FiniFn = void (*)(void*);
        using IterCb = int (*)(void*, void*);
        using ZfsIterRootFn = int (*)(void*, IterCb, void*);
        using ZfsIterFilesystemsFn = int (*)(void*, IterCb, void*);
        using ZfsIterSnapshotsFn = int (*)(void*, IterCb, void*, bool);
        using ZfsGetNameFn = const char* (*)(void*);
        using ZfsIsMountedFn = int (*)(void*, char**);
        using ZfsCloseFn = void (*)(void*);

        const InitFn initFn = reinterpret_cast<InitFn>(lib.resolve("libzfs_init"));
        const FiniFn finiFn = reinterpret_cast<FiniFn>(lib.resolve("libzfs_fini"));
        const ZfsIterRootFn iterRootFn = reinterpret_cast<ZfsIterRootFn>(lib.resolve("zfs_iter_root"));
        const ZfsIterFilesystemsFn iterFsFn =
            reinterpret_cast<ZfsIterFilesystemsFn>(lib.resolve("zfs_iter_filesystems"));
        const ZfsIterSnapshotsFn iterSnapFn =
            reinterpret_cast<ZfsIterSnapshotsFn>(lib.resolve("zfs_iter_snapshots"));
        const ZfsGetNameFn getNameFn = reinterpret_cast<ZfsGetNameFn>(lib.resolve("zfs_get_name"));
        const ZfsIsMountedFn isMountedFn =
            reinterpret_cast<ZfsIsMountedFn>(lib.resolve("zfs_is_mounted"));
        const ZfsCloseFn closeFn = reinterpret_cast<ZfsCloseFn>(lib.resolve("zfs_close"));

        if (!initFn || !finiFn || !iterRootFn || !iterFsFn || !iterSnapFn || !getNameFn || !closeFn) {
            localDetail = QStringLiteral("%1 loaded but required dataset symbols are missing").arg(cand);
            lib.unload();
            continue;
        }

        void* h = initFn();
        if (!h) {
            localDetail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(cand);
            lib.unload();
            continue;
        }

        struct WalkCtx {
            QString pool;
            PoolDatasetCache* cache;
            QSet<QString> seenDatasets;
            QSet<QString> seenSnapshots;
            ZfsIterFilesystemsFn iterFs;
            ZfsIterSnapshotsFn iterSnaps;
            ZfsGetNameFn getName;
            ZfsIsMountedFn isMounted;
            ZfsCloseFn closeHandle;
        };
        struct Walker {
            static int cb(void* zh, void* opaque) {
                WalkCtx* c = static_cast<WalkCtx*>(opaque);
                if (!c || !zh || !c->getName || !c->closeHandle) {
                    return 0;
                }
                const char* raw = c->getName(zh);
                const QString name = raw ? QString::fromUtf8(raw).trimmed() : QString();
                if (name.isEmpty()) {
                    c->closeHandle(zh);
                    return 0;
                }

                const bool inPool = (name == c->pool || name.startsWith(c->pool + QStringLiteral("/"))
                                     || name.startsWith(c->pool + QStringLiteral("@")));
                if (!inPool) {
                    c->closeHandle(zh);
                    return 0;
                }

                if (name.contains('@')) {
                    const QString ds = name.section('@', 0, 0);
                    const QString snap = name.section('@', 1);
                    const QString key = ds + QStringLiteral("@") + snap;
                    if (!ds.isEmpty() && !snap.isEmpty() && !c->seenSnapshots.contains(key)) {
                        c->seenSnapshots.insert(key);
                        c->cache->snapshotsByDataset[ds].push_back(snap);
                    }
                    c->closeHandle(zh);
                    return 0;
                }

                if (!c->seenDatasets.contains(name)) {
                    c->seenDatasets.insert(name);
                    DatasetRecord rec;
                    rec.name = name;
                    rec.used = QStringLiteral("-");
                    rec.compressRatio = QStringLiteral("-");
                    rec.encryption = QStringLiteral("-");
                    rec.creation = QStringLiteral("-");
                    rec.referenced = QStringLiteral("-");
                    rec.canmount = QStringLiteral("-");
                    rec.mountpoint = QStringLiteral("-");
                    rec.mounted = QStringLiteral("no");
                    if (c->isMounted) {
                        char* where = nullptr;
                        const int mounted = c->isMounted(zh, &where);
                        rec.mounted = (mounted != 0) ? QStringLiteral("yes") : QStringLiteral("no");
                        if (mounted != 0 && where && *where) {
                            rec.mountpoint = QString::fromUtf8(where);
                        }
                    }
                    c->cache->datasets.push_back(rec);
                    c->cache->recordByName[name] = rec;
                }

                if (c->iterFs) {
                    c->iterFs(zh, cb, opaque);
                }
                if (c->iterSnaps) {
                    c->iterSnaps(zh, cb, opaque, false);
                }
                c->closeHandle(zh);
                return 0;
            }
        };
        WalkCtx ctx{poolName, &cacheOut, {}, {}, iterFsFn, iterSnapFn, getNameFn, isMountedFn, closeFn};

        (void)iterRootFn(h, Walker::cb, &ctx);
        finiFn(h);
        lib.unload();

        for (auto it = cacheOut.snapshotsByDataset.begin(); it != cacheOut.snapshotsByDataset.end(); ++it) {
            QStringList& snaps = it.value();
            std::sort(snaps.begin(), snaps.end(), [](const QString& a, const QString& b) {
                return a.compare(b, Qt::CaseInsensitive) > 0;
            });
            snaps.removeDuplicates();
        }

        std::sort(cacheOut.datasets.begin(), cacheOut.datasets.end(), [](const DatasetRecord& a, const DatasetRecord& b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        localDetail = QStringLiteral("%1 loaded and zfs iteration succeeded").arg(cand);
        if (detail) {
            *detail = localDetail;
        }
        return !cacheOut.datasets.isEmpty();
    }

    if (detail) {
        *detail = localDetail;
    }
    return false;
#endif
}

bool MainWindow::getLocalDatasetPropsLibzfs(const QString& objectName,
                                            const QStringList& propNames,
                                            QMap<QString, QString>& valuesOut,
                                            QString* detail) const {
    valuesOut.clear();
#if defined(Q_OS_WIN)
    if (detail) {
        *detail = QStringLiteral("libzfs runtime property listing not available on Windows build");
    }
    return false;
#else
    if (objectName.trimmed().isEmpty() || propNames.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("empty object or property list");
        }
        return false;
    }
    QStringList candidates;
#if defined(Q_OS_MACOS)
    candidates << QStringLiteral("/usr/local/zfs/lib/libzfs.dylib")
               << QStringLiteral("libzfs.dylib");
#else
    candidates << QStringLiteral("libzfs.so.7")
               << QStringLiteral("libzfs.so.6")
               << QStringLiteral("libzfs.so.5")
               << QStringLiteral("libzfs.so.4")
               << QStringLiteral("libzfs.so");
#endif
    QString localDetail = QStringLiteral("no loadable libzfs library found");
    for (const QString& cand : candidates) {
        QLibrary lib(cand);
        if (!lib.load()) {
            continue;
        }
        using InitFn = void* (*)();
        using FiniFn = void (*)(void*);
        using ZfsOpenFn = void* (*)(void*, const char*, int);
        using ZfsCloseFn = void (*)(void*);
        using ZfsNameToPropFn = int (*)(const char*);
        using ZfsPropGetFn = int (*)(void*, int, char*, size_t, int*, char*, size_t, int);

        const InitFn initFn = reinterpret_cast<InitFn>(lib.resolve("libzfs_init"));
        const FiniFn finiFn = reinterpret_cast<FiniFn>(lib.resolve("libzfs_fini"));
        const ZfsOpenFn openFn = reinterpret_cast<ZfsOpenFn>(lib.resolve("zfs_open"));
        const ZfsCloseFn closeFn = reinterpret_cast<ZfsCloseFn>(lib.resolve("zfs_close"));
        const ZfsNameToPropFn nameToPropFn =
            reinterpret_cast<ZfsNameToPropFn>(lib.resolve("zfs_name_to_prop"));
        const ZfsPropGetFn propGetFn = reinterpret_cast<ZfsPropGetFn>(lib.resolve("zfs_prop_get"));
        if (!initFn || !finiFn || !openFn || !closeFn || !nameToPropFn || !propGetFn) {
            localDetail = QStringLiteral("%1 loaded but required prop symbols are missing").arg(cand);
            lib.unload();
            continue;
        }
        void* h = initFn();
        if (!h) {
            localDetail = QStringLiteral("%1 loaded but libzfs_init returned null").arg(cand);
            lib.unload();
            continue;
        }
        void* zh = openFn(h, objectName.toUtf8().constData(), 0xFFFF);
        if (!zh) {
            finiFn(h);
            lib.unload();
            localDetail = QStringLiteral("%1 loaded but zfs_open failed for %2").arg(cand, objectName);
            continue;
        }
        int found = 0;
        for (const QString& pNameRaw : propNames) {
            const QString pName = pNameRaw.trimmed();
            if (pName.isEmpty()) {
                continue;
            }
            const int pId = nameToPropFn(pName.toUtf8().constData());
            if (pId < 0) {
                continue;
            }
            char buf[4096];
            char statbuf[4096];
            std::memset(buf, 0, sizeof(buf));
            std::memset(statbuf, 0, sizeof(statbuf));
            int src = 0;
            const int rc = propGetFn(zh, pId, buf, sizeof(buf), &src, statbuf, sizeof(statbuf), 1);
            if (rc == 0) {
                valuesOut[pName] = QString::fromUtf8(buf).trimmed();
                ++found;
            }
        }
        closeFn(zh);
        finiFn(h);
        lib.unload();
        localDetail = QStringLiteral("%1 loaded and zfs_prop_get succeeded (%2 props)").arg(cand).arg(found);
        if (detail) {
            *detail = localDetail;
        }
        return found > 0;
    }
    if (detail) {
        *detail = localDetail;
    }
    return false;
#endif
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

QString MainWindow::wrapRemoteCommand(const ConnectionProfile& p,
                                      const QString& remoteCmd,
                                      MainWindow::WindowsCommandMode windowsMode) const {
    if (!isWindowsConnection(p)) {
        return remoteCmd;
    }
    const QString trimmed = remoteCmd.trimmed();
    const QString low = trimmed.toLower();
    const bool zfsStreamCmd = low.contains(QStringLiteral("zfs send"))
        || low.contains(QStringLiteral("zfs recv"));
    MainWindow::WindowsCommandMode effectiveMode = windowsMode;
    if (effectiveMode == MainWindow::WindowsCommandMode::Auto) {
        // zfs send/recv transport binary streams; forcing PowerShell execution can break stdin/stdout.
        // For these commands we must route through a Unix shell (MSYS/MinGW) when available.
        effectiveMode = zfsStreamCmd || !looksLikePowerShellScript(trimmed)
            ? MainWindow::WindowsCommandMode::UnixShell
            : MainWindow::WindowsCommandMode::PowerShellNative;
    }
    const QString psEscaped = QString(trimmed).replace('\'', QStringLiteral("''"));
    QString script = QStringLiteral(
        "$ProgressPreference='SilentlyContinue'; "
        "$InformationPreference='SilentlyContinue'; "
        "$WarningPreference='Continue'; "
        "$zfsPaths=@("
        "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin',"
        "'C:\\\\Program Files\\\\OpenZFS On Windows',"
        "'C:\\\\msys64\\\\usr\\\\bin',"
        "'C:\\\\msys64\\\\mingw64\\\\bin',"
        "'C:\\\\msys64\\\\mingw32\\\\bin',"
        "'C:\\\\MinGW\\\\bin',"
        "'C:\\\\mingw64\\\\bin'"
        "); "
        "foreach($p in $zfsPaths){ "
        "  if(Test-Path -LiteralPath $p){ "
        "    if(-not (($env:Path -split ';') -contains $p)){ $env:Path = $p + ';' + $env:Path } "
        "  } "
        "}; ");
    if (effectiveMode == MainWindow::WindowsCommandMode::PowerShellNative) {
        script += trimmed;
    } else {
        script += QStringLiteral(
            "$cmd='%1'; "
            "$unixShells=@("
            "'C:\\\\msys64\\\\usr\\\\bin\\\\bash.exe',"
            "'C:\\\\msys64\\\\usr\\\\bin\\\\sh.exe',"
            "'C:\\\\msys64\\\\mingw64\\\\bin\\\\bash.exe',"
            "'C:\\\\msys64\\\\mingw32\\\\bin\\\\bash.exe',"
            "'C:\\\\MinGW\\\\msys\\\\1.0\\\\bin\\\\sh.exe'"
            "); "
            "$shell=$null; foreach($s in $unixShells){ if(Test-Path -LiteralPath $s){ $shell=$s; break } }; "
            "if($shell){ & $shell -lc $cmd; exit $LASTEXITCODE } "
            "Invoke-Expression $cmd; exit $LASTEXITCODE;")
                      .arg(psEscaped);
    }
    const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
    const QString b64 = QString::fromLatin1(utf16.toBase64());
    // Windows command-line length can be hit with very large EncodedCommand payloads.
    // For long PowerShell-native scripts, fallback to -Command to avoid UTF-16+Base64 expansion.
    if (effectiveMode == MainWindow::WindowsCommandMode::PowerShellNative && b64.size() > 7000) {
        QString inlineScript = script;
        inlineScript.replace(QStringLiteral("\""), QStringLiteral("`\""));
        return QStringLiteral("powershell -NoProfile -NonInteractive -Command \"& { %1 }\"")
            .arg(inlineScript);
    }
    return QStringLiteral("powershell -NoProfile -NonInteractive -EncodedCommand %1").arg(b64);
}

QString MainWindow::sshExecFromLocal(const ConnectionProfile& p,
                                     const QString& remoteCmd,
                                     MainWindow::WindowsCommandMode windowsMode) const {
    if (isLocalConnection(p)) {
        return remoteCmd;
    }
    const QString sshBase = sshBaseCommand(p);
    const QString target = shSingleQuote(sshUserHost(p));
    const QString wrapped = wrapRemoteCommand(p, remoteCmd, windowsMode);
    return sshBase + QStringLiteral(" ") + target + QStringLiteral(" ") + shSingleQuote(wrapped);
}

bool MainWindow::getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut) {
    valueOut.clear();
    if (connIdx < 0 || connIdx >= m_profiles.size() || dataset.isEmpty() || prop.isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd =
        QStringLiteral("zfs get -H -o value %1 %2").arg(shSingleQuote(prop), shSingleQuote(dataset));
    cmd = withSudo(p, cmd);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 15000, out, err, rc) || rc != 0) {
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
    if (!isWindowsConnection(connIdx)) {
        return mountpointHint.trimmed();
    }
    if (!isMountedValueTrue(mountedValue)) {
        return mountpointHint.trimmed();
    }
    if (poolName.isEmpty() || datasetName.isEmpty()) {
        return mountpointHint.trimmed();
    }
    if (!(datasetName == poolName || datasetName.startsWith(poolName + QStringLiteral("/")))) {
        return mountpointHint.trimmed();
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
        return base;
    }
    QString rel = datasetName.mid(anchor.size());
    if (rel.startsWith('/')) {
        rel.remove(0, 1);
    }
    rel.replace('/', '\\');
    return rel.isEmpty() ? base : (base + rel);
}

QString MainWindow::datasetCacheKey(int connIdx, const QString& poolName) const {
    return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
}

bool MainWindow::ensureDatasetsLoaded(int connIdx, const QString& poolName, bool allowRemoteLoadIfMissing) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    PoolDatasetCache& cache = m_poolDatasetCache[key];
    if (cache.loaded) {
        return true;
    }
    if (!allowRemoteLoadIfMissing) {
        return false;
    }
    auto loadSnapshotsFromCli = [&](const ConnectionProfile& p, PoolDatasetCache& targetCache) {
        QString snapCmd = QStringLiteral("zfs list -H -p -t snapshot -o name,creation -r %1")
                              .arg(shSingleQuote(poolName));
        snapCmd = withSudo(p, snapCmd);
        QString snapOut;
        QString snapErr;
        int snapRc = -1;
        if (!runSsh(p, snapCmd, 25000, snapOut, snapErr, snapRc) || snapRc != 0) {
            return;
        }
        QMap<QString, QVector<QPair<QString, QString>>> snapshotMetaByDataset;
        const QStringList snapLines = snapOut.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : snapLines) {
            const QStringList f = line.split('\t');
            if (f.size() < 2) {
                continue;
            }
            const QString name = f[0].trimmed();
            const QString creation = f[1].trimmed();
            if (!name.contains('@')) {
                continue;
            }
            const QString ds = name.section('@', 0, 0);
            const QString snap = name.section('@', 1);
            if (ds.isEmpty() || snap.isEmpty()) {
                continue;
            }
            snapshotMetaByDataset[ds].push_back(qMakePair(creation, snap));
        }
        for (auto it = snapshotMetaByDataset.begin(); it != snapshotMetaByDataset.end(); ++it) {
            auto rows = it.value();
            std::sort(rows.begin(), rows.end(), [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
                bool aOk = false;
                bool bOk = false;
                const qlonglong av = a.first.toLongLong(&aOk);
                const qlonglong bv = b.first.toLongLong(&bOk);
                if (aOk && bOk && av != bv) {
                    return av > bv;
                }
                if (a.first != b.first) {
                    return a.first > b.first;
                }
                return a.second > b.second;
            });
            QStringList sortedSnaps;
            sortedSnaps.reserve(rows.size());
            for (const auto& row : rows) {
                sortedSnaps.push_back(row.second);
            }
            targetCache.snapshotsByDataset.insert(it.key(), sortedSnaps);
        }
    };

    if (isLocalConnection(connIdx) && detectLocalLibzfs()) {
        QString detail;
        if (listLocalDatasetsLibzfs(poolName, cache, &detail)) {
            if (cache.snapshotsByDataset.isEmpty()) {
                loadSnapshotsFromCli(m_profiles[connIdx], cache);
            }
            cache.loaded = true;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Datasets loaded via libzfs %1::%2 (%3)")
                       .arg(m_profiles[connIdx].name, poolName, detail));
            return true;
        }
        appLog(QStringLiteral("WARN"),
               QStringLiteral("libzfs dataset listing fallback to CLI %1::%2 (%3)")
                   .arg(m_profiles[connIdx].name, poolName, detail));
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd = QStringLiteral(
        "zfs list -H -p -t filesystem,volume,snapshot "
        "-o name,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r %1")
                      .arg(poolName);
    cmd = withSudo(p, cmd);

    QString out;
    QString err;
    int rc = -1;
    appLog(QStringLiteral("INFO"), QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));
    if (!runSsh(p, cmd, 35000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("WARN"), QStringLiteral("Failed datasets %1::%2 -> %3")
                                        .arg(p.name, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        return false;
    }

    QMap<QString, QVector<QPair<QString, QString>>> snapshotMetaByDataset;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList f = line.split('\t');
        if (f.size() < 9) {
            continue;
        }
        const QString name = f[0].trimmed();
        if (name.isEmpty()) {
            continue;
        }
        DatasetRecord rec{name, f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]};
        if (name.contains('@')) {
            const QString ds = name.section('@', 0, 0);
            const QString snap = name.section('@', 1);
            snapshotMetaByDataset[ds].push_back(qMakePair(rec.creation, snap));
        } else {
            cache.datasets.push_back(rec);
            cache.recordByName[name] = rec;
        }
    }
    for (auto it = snapshotMetaByDataset.begin(); it != snapshotMetaByDataset.end(); ++it) {
        auto rows = it.value();
        std::sort(rows.begin(), rows.end(), [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
            bool aOk = false;
            bool bOk = false;
            const qlonglong av = a.first.toLongLong(&aOk);
            const qlonglong bv = b.first.toLongLong(&bOk);
            if (aOk && bOk && av != bv) {
                return av > bv; // más nuevo primero
            }
            if (a.first != b.first) {
                return a.first > b.first; // fallback textual desc
            }
            return a.second > b.second; // fallback por nombre desc
        });
        QStringList sortedSnaps;
        sortedSnaps.reserve(rows.size());
        for (const auto& row : rows) {
            sortedSnaps.push_back(row.second);
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
    proc.start(QStringLiteral("sh"), QStringList{QStringLiteral("-lc"), command});
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
                            appLog(QStringLiteral("INFO"), QStringLiteral("[progress] %1").arg(ln));
                            continue;
                        }
                    }
                    if (progressTimer.elapsed() < 700) {
                        continue;
                    }
                    progressTimer.restart();
                    lastProgressSnippet = ln;
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
            if (heartbeatTimer.elapsed() >= 2000) {
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
