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
#include <QMutexLocker>

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

QString MainWindow::remoteScriptsVersionTag() const {
    return QStringLiteral(ZFSMGR_APP_VERSION) + QStringLiteral(".remote-scripts.13");
}

QString MainWindow::remoteScriptsBasePath(const ConnectionProfile& p) const {
    const QString user = p.username.trimmed();
    if (user.isEmpty()) {
        return QStringLiteral("$HOME/.config/ZFSMgr/bin");
    }
    return QStringLiteral("~%1/.config/ZFSMgr/bin").arg(user);
}

QString MainWindow::remoteScriptsVersionFilePath(const ConnectionProfile& p) const {
    return QStringLiteral("%1/.zfsmgr_remote_scripts_version").arg(remoteScriptsBasePath(p));
}

QMap<QString, QString> MainWindow::remoteScriptPayloads() const {
    QMap<QString, QString> scripts;
    scripts.insert(
        QStringLiteral("zfsmgr-rsync-opts"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DELETE=0
DRYRUN=0
for arg in "$@"; do
  case "$arg" in
    --delete) DELETE=1 ;;
    --dry-run) DRYRUN=1 ;;
  esac
done
OPTS="-aHWS"
[ "$DELETE" -eq 1 ] && OPTS="$OPTS --delete"
[ "$DRYRUN" -eq 1 ] && OPTS="$OPTS --dry-run"
PROGRESS="--info=progress2"
if ! rsync --help 2>/dev/null | grep -q -- '--info'; then
  PROGRESS="--progress"
fi
if rsync -A --version >/dev/null 2>&1; then
  OPTS="$OPTS -A"
fi
if rsync -X --version >/dev/null 2>&1; then
  OPTS="$OPTS -X"
elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then
  OPTS="$OPTS --extended-attributes"
fi
printf 'RSYNC_OPTS=%s\n' "$OPTS"
printf 'RSYNC_PROGRESS=%s\n' "$PROGRESS"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zpool-guid"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
pool="${1:-}"
[ -n "$pool" ] || exit 2
zpool get -H -o value guid "$pool"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zpool-list-json"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
zpool list -j
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zpool-import-probe"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
zpool import
zpool import -s
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-guid-map"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
pool="${1:-}"
[ -n "$pool" ] || exit 2
zfs get -H -o name,value guid -r "$pool"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-mount-list"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
if zfs mount -j >/dev/null 2>&1; then
  zfs mount -j
else
  zfs mount
fi
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zpool-status"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
pool="${1:-}"
[ -n "$pool" ] || exit 2
zpool status -v "$pool"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-list-all"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
pool="${1:-}"
[ -n "$pool" ] || exit 2
if LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount "$pool" >/dev/null 2>&1; then
  LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount "$pool"
elif LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount "$pool" >/dev/null 2>&1; then
  LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount "$pool"
elif zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount "$pool" >/dev/null 2>&1; then
  zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount "$pool"
else
  zfs list -H -p -t filesystem,volume,snapshot -o name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r "$pool"
fi
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-mountpoint"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
ds="${1:-}"
[ -n "$ds" ] || exit 2
zfs get -H -o value mountpoint "$ds"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-get-type"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
obj="${1:-}"
[ -n "$obj" ] || exit 2
zfs get -H -o value type "$obj"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-get-guid"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
obj="${1:-}"
[ -n "$obj" ] || exit 2
zfs get -H -o value guid "$obj"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-get-prop"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
prop="${1:-}"
obj="${2:-}"
[ -n "$prop" ] || exit 2
[ -n "$obj" ] || exit 2
zfs get -H -o value "$prop" "$obj"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-get-all-json"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
obj="${1:-}"
[ -n "$obj" ] || exit 2
if LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j all "$obj" >/dev/null 2>&1; then
  LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j all "$obj"
elif LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j all "$obj" >/dev/null 2>&1; then
  LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j all "$obj"
else
  zfs get -j all "$obj"
fi
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-get-json"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
props="${1:-}"
obj="${2:-}"
[ -n "$props" ] || exit 2
[ -n "$obj" ] || exit 2
if LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j "$props" "$obj" >/dev/null 2>&1; then
  LC_ALL=C.UTF-8 LANG=C.UTF-8 zfs get -j "$props" "$obj"
elif LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j "$props" "$obj" >/dev/null 2>&1; then
  LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 zfs get -j "$props" "$obj"
else
  zfs get -j "$props" "$obj"
fi
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zpool-get-all-json"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
pool="${1:-}"
[ -n "$pool" ] || exit 2
zpool get -j all "$pool"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-get-gsa-json-recursive"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
pool="${1:-}"
[ -n "$pool" ] || exit 2
zfs get -j -r org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino "$pool"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-get-gsa-raw-recursive"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
pool="${1:-}"
[ -n "$pool" ] || exit 2
zfs get -H -o name,property,value,source -r \
  org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino \
  "$pool"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-version"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set +e
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
for c in "zfs version" "zfs --version" "zpool --version"; do
  out="$(sh -c "$c" 2>&1)"
  rc=$?
  if [ $rc -eq 0 ] && [ -n "$out" ]; then
    printf '%s\n' "$out"
    exit 0
  fi
done
exit 1
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-gsa-status"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set +e
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
scheduler=''
installed=0
active=0
version=''
detail=''
guid_compare='0'
if [ "$(uname -s 2>/dev/null)" = 'Darwin' ]; then
  scheduler='launchd'
  script='/usr/local/libexec/zfsmgr-gsa.sh'
  plist='/Library/LaunchDaemons/org.zfsmgr.gsa.plist'
  [ -f "$script" ] && [ -f "$plist" ] && installed=1
  if [ "$installed" -eq 1 ]; then
    version="$(sed -n 's/^# ZFSMgr GSA Version: //p' "$script" | head -n1)"
    grep -q '^target_has_snapshot_guid()' "$script" >/dev/null 2>&1 && grep -q '^source_snapshot_name_by_guid()' "$script" >/dev/null 2>&1 && guid_compare='1'
    launchctl print system/org.zfsmgr.gsa >/dev/null 2>&1 && active=1
  fi
elif [ "$(uname -s 2>/dev/null)" = 'FreeBSD' ]; then
  scheduler='cron'
  script='/usr/local/libexec/zfsmgr-gsa.sh'
  conf='/etc/zfsmgr/gsa.conf'
  [ -f "$script" ] && [ -f "$conf" ] && installed=1
  if [ "$installed" -eq 1 ]; then
    version="$(sed -n 's/^# ZFSMgr GSA Version: //p' "$script" | head -n1)"
    grep -q '^target_has_snapshot_guid()' "$script" >/dev/null 2>&1 && grep -q '^source_snapshot_name_by_guid()' "$script" >/dev/null 2>&1 && guid_compare='1'
    if crontab -l 2>/dev/null | grep -F '/usr/local/libexec/zfsmgr-gsa.sh' >/dev/null 2>&1; then active=1; fi
  fi
elif command -v systemctl >/dev/null 2>&1; then
  scheduler='systemd'
  script='/usr/local/libexec/zfsmgr-gsa.sh'
  service='/etc/systemd/system/zfsmgr-gsa.service'
  timer='/etc/systemd/system/zfsmgr-gsa.timer'
  [ -f "$script" ] && [ -f "$service" ] && [ -f "$timer" ] && installed=1
  if [ "$installed" -eq 1 ]; then
    version="$(sed -n 's/^# ZFSMgr GSA Version: //p' "$script" | head -n1)"
    grep -q '^target_has_snapshot_guid()' "$script" >/dev/null 2>&1 && grep -q '^source_snapshot_name_by_guid()' "$script" >/dev/null 2>&1 && guid_compare='1'
    systemctl is-enabled zfsmgr-gsa.timer >/dev/null 2>&1 && systemctl is-active zfsmgr-gsa.timer >/dev/null 2>&1 && active=1
  fi
else
  detail='No native scheduler detected'
fi
printf 'SCHEDULER=%s\nINSTALLED=%s\nACTIVE=%s\nVERSION=%s\nDETAIL=%s\nGUID_COMPARE=%s\n' "$scheduler" "$installed" "$active" "$version" "$detail" "$guid_compare"
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-gsa-connections-cat"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
f='/etc/zfsmgr/gsa-connections.conf'
if [ -f "$f" ]; then
  cat "$f"
fi
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-gsa-log-cat"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
f="${1:-$HOME/.config/ZFSMgr/GSA.log}"
if [ -f "$f" ]; then
  cat "$f"
fi
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-allow-batch"),
        QString::fromLatin1(R"(#!/bin/sh
set +e
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
for ds in "$@"; do
  printf '__ZFSMGR_ALLOW_BEGIN__ %s\n' "$ds"
  zfs allow "$ds" 2>&1
  printf '__ZFSMGR_ALLOW_RC__ %s %s\n' "$ds" "$?"
  printf '__ZFSMGR_ALLOW_END__ %s\n' "$ds"
done
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-zfs-list-children"),
        QString::fromLatin1(R"(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
ds="${1:-}"
[ -n "$ds" ] || exit 2
zfs list -H -o name -r "$ds"
)"));
    scripts.insert(
        QStringLiteral("zfsmgr-advanced-breakdown-list"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
[ -n "$DATASET" ] || exit 2
saved_prop='org.fc16.zfsmgr:savedmountpoint'
ZFSMGR_PASS_READ=0
ZFSMGR_KEY_PASS=''
mount_alt_zfs(){ ds="$1"; mp="$2"; current_mp=$(zfs get -H -o value mountpoint "$ds" 2>/dev/null || true); zfs set "$saved_prop=$current_mp" "$ds"; zfs set mountpoint="$mp" "$ds"; zfs mount "$ds" >/dev/null 2>&1 || true; }
umount_alt_zfs(){ ds="$1"; mp="$2"; zfs unmount "$ds" >/dev/null 2>&1 || zfs unmount "$mp" >/dev/null 2>&1 || true; saved_mp=$(zfs get -H -o value "$saved_prop" "$ds" 2>/dev/null || true); if [ -n "$saved_mp" ] && [ "$saved_mp" != "-" ]; then zfs set mountpoint="$saved_mp" "$ds" >/dev/null 2>&1 || true; fi; zfs inherit "$saved_prop" "$ds" >/dev/null 2>&1 || true; }
resolve_mp(){ ds="$1"; zfs mount 2>/dev/null | awk -v d="$ds" '$1==d{print $2; exit}'; }
load_key_if_needed(){ ds="$1"; ks=$(zfs get -H -o value keystatus "$ds" 2>/dev/null || true); [ "$ks" = "available" ] && return 0; kl=$(zfs get -H -o value keylocation "$ds" 2>/dev/null || true); if [ "$kl" = "prompt" ]; then if [ "$ZFSMGR_PASS_READ" != "1" ]; then IFS= read -r ZFSMGR_KEY_PASS || return 1; ZFSMGR_PASS_READ=1; fi; printf '%s\n' "$ZFSMGR_KEY_PASS" | zfs load-key "$ds" >/dev/null; else zfs load-key "$ds" >/dev/null 2>&1 || true; fi; }
TMP_ROOT=''
cleanup(){
  if [ -n "$TMP_ROOT" ]; then umount_alt_zfs "$DATASET" "$TMP_ROOT" >/dev/null 2>&1 || true; fi
  if [ -n "$TMP_ROOT" ]; then rmdir "$TMP_ROOT" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT INT TERM
MP="$(resolve_mp "$DATASET")"
if [ -z "$MP" ]; then
  load_key_if_needed "$DATASET"
  TMP_ROOT="$(mktemp -d /tmp/zfsmgr-break-root-XXXXXX)"
  mount_alt_zfs "$DATASET" "$TMP_ROOT"
  MP="$(resolve_mp "$DATASET")"
fi
[ -n "$MP" ] || exit 0
[ -d "$MP" ] || exit 0
printf '__MP__=%s\n' "$MP"
find "$MP" -mindepth 1 -type d -print 2>/dev/null | while IFS= read -r d; do
  rel="$d"
  case "$rel" in
    "$MP"/*) rel=${rel#"$MP"/} ;;
    *) rel='' ;;
  esac
  [ -n "$rel" ] || continue
  case "$rel" in
    .zfs|.zfs/*) continue ;;
  esac
  printf '%s\n' "$rel"
done | sort -u
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-advanced-breakdown"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
[ -n "$DATASET" ] || exit 2
shift || true
saved_prop='org.fc16.zfsmgr:savedmountpoint'
ZFSMGR_PASS_READ=0
ZFSMGR_KEY_PASS=''
mount_alt_zfs(){ ds="$1"; mp="$2"; current_mp=$(zfs get -H -o value mountpoint "$ds" 2>/dev/null || true); zfs set "$saved_prop=$current_mp" "$ds"; zfs set mountpoint="$mp" "$ds"; zfs mount "$ds" >/dev/null 2>&1 || true; }
umount_alt_zfs(){ ds="$1"; mp="$2"; zfs unmount "$ds" >/dev/null 2>&1 || zfs unmount "$mp" >/dev/null 2>&1 || true; saved_mp=$(zfs get -H -o value "$saved_prop" "$ds" 2>/dev/null || true); if [ -n "$saved_mp" ] && [ "$saved_mp" != "-" ]; then zfs set mountpoint="$saved_mp" "$ds" >/dev/null 2>&1 || true; fi; zfs inherit "$saved_prop" "$ds" >/dev/null 2>&1 || true; }
resolve_mp(){ ds="$1"; zfs mount 2>/dev/null | awk -v d="$ds" '$1==d{print $2; exit}'; }
load_key_if_needed(){ ds="$1"; ks=$(zfs get -H -o value keystatus "$ds" 2>/dev/null || true); [ "$ks" = "available" ] && return 0; kl=$(zfs get -H -o value keylocation "$ds" 2>/dev/null || true); if [ "$kl" = "prompt" ]; then if [ "$ZFSMGR_PASS_READ" != "1" ]; then IFS= read -r ZFSMGR_KEY_PASS || return 1; ZFSMGR_PASS_READ=1; fi; printf '%s\n' "$ZFSMGR_KEY_PASS" | zfs load-key "$ds" >/dev/null; else zfs load-key "$ds" >/dev/null 2>&1 || true; fi; }
RSYNC_OPTS='-aHWS'
RSYNC_PROGRESS='--info=progress2'
if ! rsync --help 2>/dev/null | grep -q -- '--info'; then RSYNC_PROGRESS='--progress'; fi
if rsync -A --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -A"; fi
if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -X"; elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS="$RSYNC_OPTS --extended-attributes"; fi
TMP_ROOT=''
cleanup(){ if [ -n "$TMP_ROOT" ]; then umount_alt_zfs "$DATASET" "$TMP_ROOT" >/dev/null 2>&1 || true; rmdir "$TMP_ROOT" >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT INT TERM
MP="$(resolve_mp "$DATASET")"
if [ -z "$MP" ]; then
  load_key_if_needed "$DATASET"
  TMP_ROOT="$(mktemp -d /tmp/zfsmgr-break-root-XXXXXX)"
  mount_alt_zfs "$DATASET" "$TMP_ROOT"
  MP="$TMP_ROOT"
fi
[ -n "$MP" ] || { echo "mountpoint=none"; exit 2; }
for bn in "$@"; do
  d="$MP/$bn"
  [ -d "$d" ] || continue
  [ -L "$d" ] && { echo "[BREAKDOWN] skip link $bn"; continue; }
  echo "[BREAKDOWN] start $bn"
  child="$DATASET/$bn"
  zfs list -H -o name "$child" >/dev/null 2>&1 && { echo "child_exists=$child"; continue; }
  FINAL_MP="$MP/$bn"
  TMP_CHILD_MP="$(mktemp -d /tmp/zfsmgr-breakdown-child-XXXXXX)"
  zfs create -p -o mountpoint="$TMP_CHILD_MP" "$child"
  zfs mount "$child" >/dev/null 2>&1 || true
  try=0
  PENDING=1
  while :; do
    rsync $RSYNC_OPTS $RSYNC_PROGRESS "$d"/ "$TMP_CHILD_MP"/
    PENDING="$(rsync -rni --ignore-existing "$d"/ "$TMP_CHILD_MP"/ | awk 'length($0)>11{c=substr($0,1,11); if(substr(c,1,1)==">" && substr(c,2,1)=="f") n++} END{print n+0}')"
    [ "$PENDING" = "0" ] && break
    try=$((try+1))
    [ "$try" -lt 5 ] || break
  done
  [ "$PENDING" = "0" ] || { echo "verify_failed=$child pending=$PENDING"; exit 42; }
  rm -rf "$d"
  zfs set mountpoint="$FINAL_MP" "$child"
  zfs mount "$child" >/dev/null 2>&1 || true
  rmdir "$TMP_CHILD_MP" >/dev/null 2>&1 || true
  echo "[BREAKDOWN] ok $bn -> $child"
done
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-advanced-assemble"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
[ -n "$DATASET" ] || exit 2
shift || true
saved_prop='org.fc16.zfsmgr:savedmountpoint'
ZFSMGR_PASS_READ=0
ZFSMGR_KEY_PASS=''
mount_alt_zfs(){ ds="$1"; mp="$2"; current_mp=$(zfs get -H -o value mountpoint "$ds" 2>/dev/null || true); zfs set "$saved_prop=$current_mp" "$ds"; zfs set mountpoint="$mp" "$ds"; zfs mount "$ds" >/dev/null 2>&1 || true; }
umount_alt_zfs(){ ds="$1"; mp="$2"; zfs unmount "$ds" >/dev/null 2>&1 || zfs unmount "$mp" >/dev/null 2>&1 || true; saved_mp=$(zfs get -H -o value "$saved_prop" "$ds" 2>/dev/null || true); if [ -n "$saved_mp" ] && [ "$saved_mp" != "-" ]; then zfs set mountpoint="$saved_mp" "$ds" >/dev/null 2>&1 || true; fi; zfs inherit "$saved_prop" "$ds" >/dev/null 2>&1 || true; }
resolve_mp(){ ds="$1"; zfs mount 2>/dev/null | awk -v d="$ds" '$1==d{print $2; exit}'; }
load_key_if_needed(){ ds="$1"; ks=$(zfs get -H -o value keystatus "$ds" 2>/dev/null || true); [ "$ks" = "available" ] && return 0; kl=$(zfs get -H -o value keylocation "$ds" 2>/dev/null || true); if [ "$kl" = "prompt" ]; then if [ "$ZFSMGR_PASS_READ" != "1" ]; then IFS= read -r ZFSMGR_KEY_PASS || return 1; ZFSMGR_PASS_READ=1; fi; printf '%s\n' "$ZFSMGR_KEY_PASS" | zfs load-key "$ds" >/dev/null; else zfs load-key "$ds" >/dev/null 2>&1 || true; fi; }
RSYNC_OPTS='-aHWS'
RSYNC_PROGRESS='--info=progress2'
if ! rsync --help 2>/dev/null | grep -q -- '--info'; then RSYNC_PROGRESS='--progress'; fi
if rsync -A --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -A"; fi
if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -X"; elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS="$RSYNC_OPTS --extended-attributes"; fi
TMP_PARENT=''
cleanup(){ if [ -n "$TMP_PARENT" ]; then umount_alt_zfs "$DATASET" "$TMP_PARENT" >/dev/null 2>&1 || true; rmdir "$TMP_PARENT" >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT INT TERM
MP="$(resolve_mp "$DATASET")"
if [ -z "$MP" ]; then
  load_key_if_needed "$DATASET"
  TMP_PARENT="$(mktemp -d /tmp/zfsmgr-assemble-parent-XXXXXX)"
  mount_alt_zfs "$DATASET" "$TMP_PARENT"
  MP="$TMP_PARENT"
fi
[ -n "$MP" ] || { echo "mountpoint=none"; exit 2; }
for child in "$@"; do
  bn="${child##*/}"
  echo "[ASSEMBLE] start $child"
  CMP="$(resolve_mp "$child")"
  CHILD_TMP=''
  if [ -z "$CMP" ]; then
    load_key_if_needed "$child"
    CHILD_TMP="$(mktemp -d /tmp/zfsmgr-assemble-child-XXXXXX)"
    mount_alt_zfs "$child" "$CHILD_TMP"
    CMP="$CHILD_TMP"
  fi
  TMP="$(mktemp -d /tmp/zfsmgr-assemble-XXXXXX)"
  rsync $RSYNC_OPTS $RSYNC_PROGRESS "$CMP"/ "$TMP"/
  if [ -n "$CHILD_TMP" ]; then umount_alt_zfs "$child" "$CHILD_TMP" >/dev/null 2>&1 || true; rmdir "$CHILD_TMP" >/dev/null 2>&1 || true; fi
  zfs destroy -r "$child"
  mkdir -p "$MP/$bn"
  rsync $RSYNC_OPTS $RSYNC_PROGRESS "$TMP"/ "$MP/$bn"/
  rm -rf "$TMP"
  echo "[ASSEMBLE] ok $child -> $MP/$bn"
done
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-advanced-fromdir"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
SRC_DIR="${2:-}"
CREATE_CMD="${3:-}"
DELETE_SRC="${4:-0}"
[ -n "$DATASET" ] || exit 2
[ -n "$SRC_DIR" ] || exit 2
[ -n "$CREATE_CMD" ] || exit 2
RSYNC_OPTS='-aHWS'
RSYNC_PROGRESS='--info=progress2'
if ! rsync --help 2>/dev/null | grep -q -- '--info'; then RSYNC_PROGRESS='--progress'; fi
if rsync -A --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -A"; fi
if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -X"; elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS="$RSYNC_OPTS --extended-attributes"; fi
TMP_MP="$(mktemp -d /tmp/zfsmgr-fromdir-mp-XXXXXX)"
BACKUP_DIR=''
cleanup(){
  rc=$?
  if [ $rc -ne 0 ]; then
    if [ -n "$BACKUP_DIR" ] && [ ! -e "$SRC_DIR" ] && [ -d "$BACKUP_DIR" ]; then mv "$BACKUP_DIR" "$SRC_DIR" || true; fi
  fi
  rmdir "$TMP_MP" >/dev/null 2>&1 || true
  exit $rc
}
trap cleanup EXIT INT TERM
[ -d "$SRC_DIR" ] || { echo 'source directory does not exist'; exit 2; }
if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx "$SRC_DIR" >/dev/null 2>&1; then
  echo 'mountpoint already in use'; exit 3
fi
sh -c "$CREATE_CMD"
zfs set canmount=on "$DATASET" >/dev/null 2>&1 || true
zfs set mountpoint="$TMP_MP" "$DATASET"
zfs mount "$DATASET" >/dev/null 2>&1 || true
normp(){ p="$1"; [ -z "$p" ] && return; [ -d "$p" ] && (cd "$p" 2>/dev/null && pwd -P) || printf '%s' "$p"; }
ACTIVE_MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
N_ACTIVE="$(normp "$ACTIVE_MP")"; N_TMP="$(normp "$TMP_MP")"
[ "$N_ACTIVE" = "$N_TMP" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 4; }
echo "[FROMDIR] rsync start"
rsync $RSYNC_OPTS $RSYNC_PROGRESS "$SRC_DIR"/ "$TMP_MP"/
echo "[FROMDIR] rsync done"
if [ "$DELETE_SRC" = "1" ]; then
  BACKUP_DIR="$SRC_DIR.zfsmgr-bak-$$"
  i=0
  while [ -e "$BACKUP_DIR" ]; do i=$((i+1)); BACKUP_DIR="$SRC_DIR.zfsmgr-bak-$$-$i"; done
  mv "$SRC_DIR" "$BACKUP_DIR"
  mkdir -p "$SRC_DIR"
  zfs umount "$DATASET" >/dev/null 2>&1 || true
  zfs set mountpoint="$SRC_DIR" "$DATASET"
  zfs mount "$DATASET" >/dev/null 2>&1 || true
  FINAL_MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
  N_FINAL="$(normp "$FINAL_MP")"; N_SRC="$(normp "$SRC_DIR")"
  if [ "$N_FINAL" != "$N_SRC" ]; then
    zfs set mountpoint="$TMP_MP" "$DATASET" >/dev/null 2>&1 || true
    zfs mount "$DATASET" >/dev/null 2>&1 || true
    rm -rf "$SRC_DIR"
    mv "$BACKUP_DIR" "$SRC_DIR"
    echo 'failed to switch mountpoint to destination directory'
    exit 5
  fi
  rm -rf "$BACKUP_DIR"
  BACKUP_DIR=''
else
  zfs umount "$DATASET" >/dev/null 2>&1 || true
  zfs set mountpoint="$SRC_DIR" "$DATASET" >/dev/null 2>&1 || true
  zfs set canmount=off "$DATASET" >/dev/null 2>&1 || true
  echo "[FROMDIR] source preserved at $SRC_DIR; dataset left unmounted (canmount=off)"
fi
BACKUP_DIR=''
trap - EXIT INT TERM
rmdir "$TMP_MP" >/dev/null 2>&1 || true
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-advanced-todir"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
DST_DIR="${2:-}"
DELETE_SRC="${3:-0}"
[ -n "$DATASET" ] || exit 2
[ -n "$DST_DIR" ] || exit 2
RSYNC_OPTS='-aHWS'
RSYNC_PROGRESS='--info=progress2'
if ! rsync --help 2>/dev/null | grep -q -- '--info'; then RSYNC_PROGRESS='--progress'; fi
if rsync -A --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -A"; fi
if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS="$RSYNC_OPTS -X"; elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS="$RSYNC_OPTS --extended-attributes"; fi
TMP_MP="$(mktemp -d /tmp/zfsmgr-todir-mp-XXXXXX)"
TMP_OUT="$(mktemp -d /tmp/zfsmgr-todir-out-XXXXXX)"
BACKUP_DIR=''
RESTORE_NEEDED=0
OLD_MP="$(zfs get -H -o value mountpoint "$DATASET" 2>/dev/null || true)"
OLD_MOUNTED="$(zfs get -H -o value mounted "$DATASET" 2>/dev/null || true)"
cleanup(){
  rc=$?
  if [ $rc -ne 0 ]; then
    if [ "$RESTORE_NEEDED" = "1" ] && [ -n "$BACKUP_DIR" ] && [ -d "$BACKUP_DIR" ]; then
      rm -rf "$DST_DIR" >/dev/null 2>&1 || true
      mv "$BACKUP_DIR" "$DST_DIR" >/dev/null 2>&1 || true
    fi
    if zfs list -H -o name "$DATASET" >/dev/null 2>&1; then
      zfs set mountpoint="$OLD_MP" "$DATASET" >/dev/null 2>&1 || true
      if [ "$OLD_MOUNTED" = "yes" ] || [ "$OLD_MOUNTED" = "on" ]; then zfs mount "$DATASET" >/dev/null 2>&1 || true; fi
    fi
  fi
  rm -rf "$TMP_MP" >/dev/null 2>&1 || true
  rm -rf "$TMP_OUT" >/dev/null 2>&1 || true
  exit $rc
}
trap cleanup EXIT INT TERM
if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx "$DST_DIR" >/dev/null 2>&1; then
  echo 'destination directory is already a zfs mountpoint'; exit 2
fi
zfs set canmount=on "$DATASET" >/dev/null 2>&1 || true
zfs set mountpoint="$TMP_MP" "$DATASET"
zfs mount "$DATASET" >/dev/null 2>&1 || true
normp(){ p="$1"; [ -z "$p" ] && return; [ -d "$p" ] && (cd "$p" 2>/dev/null && pwd -P) || printf '%s' "$p"; }
ACTIVE_MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
N_ACTIVE="$(normp "$ACTIVE_MP")"; N_TMP="$(normp "$TMP_MP")"
[ "$N_ACTIVE" = "$N_TMP" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 3; }
echo "[TODIR] rsync start"
rsync $RSYNC_OPTS $RSYNC_PROGRESS "$TMP_MP"/ "$TMP_OUT"/
echo "[TODIR] rsync done"
if [ -e "$DST_DIR" ]; then
  BACKUP_DIR="$DST_DIR.zfsmgr-bak-$$"
  i=0; while [ -e "$BACKUP_DIR" ]; do i=$((i+1)); BACKUP_DIR="$DST_DIR.zfsmgr-bak-$$-$i"; done
  mv "$DST_DIR" "$BACKUP_DIR"
  RESTORE_NEEDED=1
else
  mkdir -p "$(dirname "$DST_DIR")"
fi
mv "$TMP_OUT" "$DST_DIR"
zfs umount "$DATASET" >/dev/null 2>&1 || true
if [ "$DELETE_SRC" = "1" ]; then
  zfs destroy -r "$DATASET"
else
  zfs set mountpoint="$OLD_MP" "$DATASET" >/dev/null 2>&1 || true
  zfs umount "$DATASET" >/dev/null 2>&1 || true
  zfs set canmount=off "$DATASET" >/dev/null 2>&1 || true
  echo "[TODIR] dataset preserved unmounted (canmount=off)"
fi
if [ -n "$BACKUP_DIR" ]; then rm -rf "$BACKUP_DIR"; fi
RESTORE_NEEDED=0
trap - EXIT INT TERM
rm -rf "$TMP_MP" >/dev/null 2>&1 || true
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-sync-temp-tar-source"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
CODEC="${2:-none}"
[ -n "$DATASET" ] || exit 2
saved_prop='org.fc16.zfsmgr:savedmountpoint'
mount_alt_zfs(){ ds="$1"; mp="$2"; current_mp=$(zfs get -H -o value mountpoint "$ds" 2>/dev/null || true); zfs set "$saved_prop=$current_mp" "$ds"; zfs set mountpoint="$mp" "$ds"; zfs mount "$ds" >/dev/null 2>&1 || true; }
umount_alt_zfs(){ ds="$1"; mp="$2"; zfs unmount "$ds" >/dev/null 2>&1 || zfs unmount "$mp" >/dev/null 2>&1 || true; saved_mp=$(zfs get -H -o value "$saved_prop" "$ds" 2>/dev/null || true); if [ -n "$saved_mp" ] && [ "$saved_mp" != "-" ]; then zfs set mountpoint="$saved_mp" "$ds" >/dev/null 2>&1 || true; fi; zfs inherit "$saved_prop" "$ds" >/dev/null 2>&1 || true; }
resolve_mp(){ ds="$1"; zfs mount 2>/dev/null | awk -v d="$ds" '$1==d{print $2; exit}'; }
TMP_MP=''
cleanup(){ if [ -n "$TMP_MP" ]; then umount_alt_zfs "$DATASET" "$TMP_MP" >/dev/null 2>&1 || true; rmdir "$TMP_MP" >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT INT TERM
MP="$(resolve_mp "$DATASET")"
if [ -z "$MP" ]; then TMP_MP="$(mktemp -d /tmp/zfsmgr-sync-src-XXXXXX)"; mount_alt_zfs "$DATASET" "$TMP_MP"; MP="$TMP_MP"; fi
[ -n "$MP" ] && [ -d "$MP" ] || exit 41
case "$CODEC" in
  zstd) tar --acls --xattrs -cpf - -C "$MP" . | zstd -1 -T0 -q -c ;;
  gzip) tar --acls --xattrs -cpf - -C "$MP" . | gzip -1 -c ;;
  *) tar --acls --xattrs -cpf - -C "$MP" . ;;
esac
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-sync-temp-tar-dest"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
CODEC="${2:-none}"
[ -n "$DATASET" ] || exit 2
saved_prop='org.fc16.zfsmgr:savedmountpoint'
mount_alt_zfs(){ ds="$1"; mp="$2"; current_mp=$(zfs get -H -o value mountpoint "$ds" 2>/dev/null || true); zfs set "$saved_prop=$current_mp" "$ds"; zfs set mountpoint="$mp" "$ds"; zfs mount "$ds" >/dev/null 2>&1 || true; }
umount_alt_zfs(){ ds="$1"; mp="$2"; zfs unmount "$ds" >/dev/null 2>&1 || zfs unmount "$mp" >/dev/null 2>&1 || true; saved_mp=$(zfs get -H -o value "$saved_prop" "$ds" 2>/dev/null || true); if [ -n "$saved_mp" ] && [ "$saved_mp" != "-" ]; then zfs set mountpoint="$saved_mp" "$ds" >/dev/null 2>&1 || true; fi; zfs inherit "$saved_prop" "$ds" >/dev/null 2>&1 || true; }
resolve_mp(){ ds="$1"; zfs mount 2>/dev/null | awk -v d="$ds" '$1==d{print $2; exit}'; }
TMP_MP=''
cleanup(){ if [ -n "$TMP_MP" ]; then umount_alt_zfs "$DATASET" "$TMP_MP" >/dev/null 2>&1 || true; rmdir "$TMP_MP" >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT INT TERM
MP="$(resolve_mp "$DATASET")"
if [ -z "$MP" ]; then TMP_MP="$(mktemp -d /tmp/zfsmgr-sync-dst-XXXXXX)"; mount_alt_zfs "$DATASET" "$TMP_MP"; MP="$TMP_MP"; fi
mkdir -p "$MP"
case "$CODEC" in
  zstd) zstd -d -q -c - | tar --acls --xattrs -xpf - -C "$MP" ;;
  gzip) gzip -d -c - | tar --acls --xattrs -xpf - -C "$MP" ;;
  *) tar --acls --xattrs -xpf - -C "$MP" ;;
esac
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-copy-fallback-source"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
SNAP="${2:-}"
[ -n "$DATASET" ] || exit 2
[ -n "$SNAP" ] || exit 2
WAS_MOUNTED="$(zfs get -H -o value mounted "$DATASET" 2>/dev/null || true)"
[ -n "$WAS_MOUNTED" ] || WAS_MOUNTED=no
MP="$(zfs mount 2>/dev/null | grep -E "^$DATASET[[:space:]]" | head -n1 | cut -d" " -f2-)"
if [ -z "$MP" ]; then if ! zfs mount "$DATASET" >/dev/null 2>&1; then :; fi; MP="$(zfs mount 2>/dev/null | grep -E "^$DATASET[[:space:]]" | head -n1 | cut -d" " -f2-)"; fi
[ -n "$MP" ] && [ -d "$MP" ] || { echo "[COPY][ERROR] cannot mount source dataset: $DATASET"; exit 41; }
SRC="$MP/.zfs/snapshot/$SNAP"
[ -d "$SRC" ] || { echo "[COPY][ERROR] snapshot path unavailable: $SRC"; exit 42; }
tar --acls --xattrs -cpf - -C "$SRC" .; RC=$?
if [ "$WAS_MOUNTED" != "yes" ]; then if ! zfs unmount "$DATASET" >/dev/null 2>&1; then :; fi; fi
exit $RC
)ZFSMGR"));
    scripts.insert(
        QStringLiteral("zfsmgr-copy-fallback-dest"),
        QString::fromLatin1(R"ZFSMGR(#!/bin/sh
set -eu
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin"
export PATH
DATASET="${1:-}"
[ -n "$DATASET" ] || exit 2
if ! zfs list -H -o name "$DATASET" >/dev/null 2>&1; then zfs create -u "$DATASET"; fi
WAS_MOUNTED="$(zfs get -H -o value mounted "$DATASET" 2>/dev/null || true)"
[ -n "$WAS_MOUNTED" ] || WAS_MOUNTED=no
MP="$(zfs mount 2>/dev/null | grep -E "^$DATASET[[:space:]]" | head -n1 | cut -d" " -f2-)"
if [ -z "$MP" ]; then if ! zfs mount "$DATASET" >/dev/null 2>&1; then :; fi; MP="$(zfs mount 2>/dev/null | grep -E "^$DATASET[[:space:]]" | head -n1 | cut -d" " -f2-)"; fi
[ -n "$MP" ] && [ -d "$MP" ] || { echo "[COPY][ERROR] cannot mount destination dataset: $DATASET"; exit 43; }
mkdir -p "$MP"; tar --acls --xattrs -xpf - -C "$MP"; RC=$?
if [ "$WAS_MOUNTED" != "yes" ]; then if ! zfs unmount "$DATASET" >/dev/null 2>&1; then :; fi; fi
exit $RC
)ZFSMGR"));
    return scripts;
}

QString MainWindow::remoteScriptCommand(const ConnectionProfile& p,
                                        const QString& scriptName,
                                        const QStringList& args) const {
    QString cmd = QStringLiteral("%1/%2")
                      .arg(remoteScriptsBasePath(p), scriptName.trimmed());
    for (const QString& arg : args) {
        cmd += QStringLiteral(" ") + shSingleQuote(arg);
    }
    return cmd;
}

QString MainWindow::buildRemoteScriptsInstallCommand(const QMap<QString, QString>& payloads,
                                                     const QString& versionTag) const {
    QString cmd;
    cmd += QStringLiteral("set -e; ");
    cmd += QStringLiteral("BASE=\"$HOME/.config/ZFSMgr/bin\"; ");
    cmd += QStringLiteral("mkdir -p \"$BASE\"; ");
    cmd += QStringLiteral("umask 077; ");
    int idx = 0;
    for (auto it = payloads.constBegin(); it != payloads.constEnd(); ++it, ++idx) {
        const QString delim = QStringLiteral("__ZFSMGR_SCRIPT_%1__").arg(idx);
        cmd += QStringLiteral("cat > \"$BASE/%1\" <<'%2'\n%3\n%2\n")
                   .arg(it.key(), delim, it.value());
        cmd += QStringLiteral("chmod 700 \"$BASE/%1\"; ").arg(it.key());
    }
    cmd += QStringLiteral("printf '%s\\n' %1 > \"$BASE/.zfsmgr_remote_scripts_version\"; ")
               .arg(shSingleQuote(versionTag));
    cmd += QStringLiteral("chmod 600 \"$BASE/.zfsmgr_remote_scripts_version\"; ");
    return cmd;
}

bool MainWindow::ensureRemoteScriptsUpToDate(const ConnectionProfile& p) {
    if (isWindowsConnection(p)) {
        return true;
    }
    const QString versionTag = remoteScriptsVersionTag();

    QString out;
    QString err;
    int rc = -1;
    const QString checkCmd = QStringLiteral("f=%1; if [ -f \"$f\" ]; then cat \"$f\"; fi")
                                 .arg(remoteScriptsVersionFilePath(p));
    if (!runSsh(p, checkCmd, 8000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("%1: no se pudo verificar scripts remotos (%2)")
                   .arg(p.name, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        return false;
    }
    if (out.section('\n', 0, 0).trimmed() == versionTag) {
        return true;
    }

    const QString baseExpr = remoteScriptsBasePath(p);
    const QString prepCmd = QStringLiteral(
                                "set -e; BASE=%1; case \"$BASE\" in ~*) eval \"BASE=$BASE\";; esac; "
                                "[ -n \"$BASE\" ] || exit 2; mkdir -p \"$BASE\"; umask 077;")
                                .arg(baseExpr);
    out.clear();
    err.clear();
    rc = -1;
    if (!runSsh(p, prepCmd, 12000, out, err, rc) || rc != 0) {
        const QString sudoPrep = withSudo(
            p,
            QStringLiteral("set -e; BASE=%1; case \"$BASE\" in ~*) eval \"BASE=$BASE\";; esac; "
                           "[ -n \"$BASE\" ] || exit 2; mkdir -p \"$BASE\"; "
                           "chown -R %2:%2 \"$BASE\" >/dev/null 2>&1 || true; "
                           "chmod 700 \"$BASE\"; umask 077;")
                .arg(baseExpr, shSingleQuote(p.username.trimmed())));
        out.clear();
        err.clear();
        rc = -1;
        if (!runSsh(p, sudoPrep, 12000, out, err, rc) || rc != 0) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("%1: no se pudo preparar directorio de scripts remotos (%2)")
                       .arg(p.name, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
            return false;
        }
    }

    const QMap<QString, QString> payloads = remoteScriptPayloads();
    int idx = 0;
    for (auto it = payloads.constBegin(); it != payloads.constEnd(); ++it, ++idx) {
        const QString delim = QStringLiteral("__ZFSMGR_SCRIPT_CHUNK_%1__").arg(idx);
        const QString perScriptCmd = QStringLiteral(
            "set -e; BASE=%1; case \"$BASE\" in ~*) eval \"BASE=$BASE\";; esac; "
            "[ -n \"$BASE\" ] || exit 2; "
            "cat > \"$BASE/%2\" <<'%3'\n%4\n%3\n"
            "chmod 700 \"$BASE/%2\";")
                                         .arg(baseExpr, it.key(), delim, it.value());
        out.clear();
        err.clear();
        rc = -1;
        if (!runSsh(p, perScriptCmd, 12000, out, err, rc) || rc != 0) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("%1: no se pudo instalar script remoto %2 (%3)")
                       .arg(p.name, it.key(), oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
            return false;
        }
    }

    const QString versionCmd = QStringLiteral(
                                   "set -e; BASE=%1; case \"$BASE\" in ~*) eval \"BASE=$BASE\";; esac; "
                                   "printf '%s\\n' %2 > \"$BASE/.zfsmgr_remote_scripts_version\"; "
                                   "chmod 600 \"$BASE/.zfsmgr_remote_scripts_version\";")
                                   .arg(baseExpr, shSingleQuote(versionTag));
    out.clear();
    err.clear();
    rc = -1;
    if (!runSsh(p, versionCmd, 12000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("%1: scripts remotos copiados pero no se pudo escribir versión (%2)")
                   .arg(p.name, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        return false;
    }
    appLog(QStringLiteral("INFO"),
           QStringLiteral("%1: scripts remotos instalados/actualizados (%2)")
               .arg(p.name, versionTag));
    return true;
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
    // Windows command-line length can be hit with very large EncodedCommand payloads
    // in local cmd.exe execution. Over SSH, forcing -Command lets the remote shell
    // expand PowerShell variables (e.g. "$p"), breaking scripts like foreach($p...).
    if (isLocalConnection(p)
        && effectiveMode == MainWindow::WindowsCommandMode::PowerShellNative
        && b64.size() > 7000) {
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
    const bool useRemoteScript = !isWindowsConnection(p);
    QString cmd = useRemoteScript
        ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-get-prop"), {prop, dataset})
        : QStringLiteral("zfs get -H -o value %1 %2").arg(shSingleQuote(prop), shSingleQuote(dataset));
    if (!isWindowsConnection(p) && !useRemoteScript) {
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
    auto cacheIt = m_poolDatasetCache.find(key);
    if (cacheIt == m_poolDatasetCache.end()) {
        return false;
    }
    QString guid = cacheIt->objectGuidByName.value(trimmedObject).trimmed();
    if (guid.isEmpty() || guid == QStringLiteral("-")) {
        const ConnectionProfile& p = m_profiles[connIdx];
        QString out;
        QString err;
        int rc = -1;
        const bool useRemoteScript = !isWindowsConnection(p);
        QString guidCmd = useRemoteScript
            ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-get-guid"), {trimmedObject})
            : QStringLiteral("zfs get -H -o value guid %1").arg(shSingleQuote(trimmedObject));
        if (!isWindowsConnection(connIdx) && !useRemoteScript) {
            guidCmd = mwhelpers::withUnixSearchPathCommand(guidCmd);
        }
        guidCmd = withSudo(p, guidCmd);
        if (!runSsh(p, guidCmd, 15000, out, err, rc) || rc != 0) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("No se pudo cargar GUID de objeto %1::%2/%3 -> %4")
                       .arg(p.name, trimmedPool, trimmedObject, oneLine(err.isEmpty() ? out : err)));
            return false;
        }
        guid = out.section('\n', 0, 0).trimmed();
        if (guid.isEmpty() || guid == QStringLiteral("-")) {
            return false;
        }
        cacheIt->objectGuidByName.insert(trimmedObject, guid);
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
            const bool useRemoteScript = !isWindowsConnection(p);
            const QString guidCmd = withSudo(
                p,
                useRemoteScript
                    ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zpool-guid"), {trimmedPool})
                    : mwhelpers::withUnixSearchPathCommand(
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
        const bool useRemoteScript = !isWindowsConnection(p);
        QString jsonCmd = useRemoteScript
            ? remoteScriptCommand(p, QStringLiteral("zfsmgr-zfs-list-all"), {poolName})
            : mwhelpers::withUnixSearchPathCommand(
                  QStringLiteral("zfs get -j -p -r -t filesystem,volume,snapshot type,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount %1")
                      .arg(shSingleQuote(poolName)));
        jsonCmd = withSudo(p, jsonCmd);
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
