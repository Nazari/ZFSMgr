#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHostInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>

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

bool MainWindow::runSsh(const ConnectionProfile& p,
                        const QString& remoteCmd,
                        int timeoutMs,
                        QString& out,
                        QString& err,
                        int& rc,
                        const std::function<void(const QString&)>& onStdoutLine,
                        const std::function<void(const QString&)>& onStderrLine,
                        const std::function<void(int)>& onIdleTimeoutRemaining,
                        MainWindow::WindowsCommandMode windowsMode,
                        const QByteArray& stdinPayload) {
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
        const QString sshpassExe = findLocalExecutable(QStringLiteral("sshpass"));
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
    const ConnectionProfile& p = m_profiles[connIdx];
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
    if (!isWindowsConnection(p)) {
        cmd = mwhelpers::withUnixSearchPathCommand(cmd);
    }
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
    const ConnectionProfile& p = m_profiles[connIdx];
    if (connIdx >= 0 && connIdx < m_states.size()) {
        const QString trimmedPool = poolName.trimmed();
        if (!trimmedPool.isEmpty()
            && m_states[connIdx].poolGuidByName.value(trimmedPool).trimmed().isEmpty()) {
            QString gOut;
            QString gErr;
            int gRc = -1;
            const QString guidCmd = withSudo(
                p,
                mwhelpers::withUnixSearchPathCommand(
                    QStringLiteral("zpool get -H -o value guid %1")
                        .arg(shSingleQuote(trimmedPool))));
            if (runSsh(p, guidCmd, 12000, gOut, gErr, gRc) && gRc == 0) {
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
    cache.datasets.clear();
    cache.snapshotsByDataset.clear();
    cache.objectGuidByName.clear();
    cache.recordByName.clear();
    cache.driveletterByDataset.clear();
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
        QString jsonCmd = mwhelpers::withUnixSearchPathCommand(
            QStringLiteral("zfs get -j -p -r -t filesystem,volume,snapshot "
                           "type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount %1")
                .arg(shSingleQuote(poolName)));
        jsonCmd = withSudo(p, jsonCmd);
        if (runSsh(p, jsonCmd, 35000, out, err, rc) && rc == 0) {
            const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
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
    proc.start(QStringLiteral("sh"), QStringList{QStringLiteral("-c"), command});
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
