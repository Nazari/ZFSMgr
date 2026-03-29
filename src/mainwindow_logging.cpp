#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QThread>
#include <QSet>
#include <QSysInfo>
#include <QVBoxLayout>

#include <QtConcurrent/QtConcurrent>

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
#include <syslog.h>
#endif

#ifdef Q_OS_MACOS
#include <os/log.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <string>

namespace {
constexpr const char* kGsaLinuxRuntimeDirPath = "/var/lib/zfsmgr";
constexpr const char* kGsaFreeBsdRuntimeDirPath = "/var/db/zfsmgr";

QString tsNowForLog() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

QString normalizeHostTokenForLogs(QString host) {
    host = host.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']') && host.size() > 2) {
        host = host.mid(1, host.size() - 2);
    }
    while (host.endsWith('.')) {
        host.chop(1);
    }
    return host;
}

bool isLocalHostForLogs(const QString& host) {
    const QString h = normalizeHostTokenForLogs(host);
    if (h.isEmpty()) {
        return false;
    }
    if (h == QStringLiteral("localhost")
        || h == QStringLiteral("127.0.0.1")
        || h == QStringLiteral("::1")) {
        return true;
    }
    static const QSet<QString> aliases = []() {
        QSet<QString> s;
        s.insert(QStringLiteral("localhost"));
        s.insert(QStringLiteral("127.0.0.1"));
        s.insert(QStringLiteral("::1"));
        const QString local = normalizeHostTokenForLogs(QSysInfo::machineHostName());
        if (!local.isEmpty()) {
            s.insert(local);
            s.insert(local + QStringLiteral(".local"));
            const int dot = local.indexOf('.');
            if (dot > 0) {
                const QString shortName = local.left(dot);
                s.insert(shortName);
                s.insert(shortName + QStringLiteral(".local"));
            }
        }
        return s;
    }();
    return aliases.contains(h);
}

struct CompactLogParts {
    QString date;
    QString time;
    QString conn;
    QString level;
    QString msg;
};

CompactLogParts parseCompactLogParts(const QString& fullLine) {
    CompactLogParts out;
    static const QRegularExpression lineRx(
        QStringLiteral("^\\[(\\d{4}-\\d{2}-\\d{2})\\s+(\\d{2}:\\d{2}:\\d{2})\\]\\s+\\[([^\\]]+)\\]\\s*(.*)$"));
    static const QRegularExpression sshRx(QStringLiteral("^\\[SSH\\s+([^\\]]+)\\]\\s*(.*)$"));

    const QRegularExpressionMatch m = lineRx.match(fullLine.trimmed());
    if (!m.hasMatch()) {
        out.date = QStringLiteral("-");
        out.time = QStringLiteral("-");
        out.level = QStringLiteral("-");
        out.conn = QStringLiteral("-");
        out.msg = fullLine.trimmed();
        return out;
    }
    out.date = m.captured(1).trimmed();
    out.time = m.captured(2).trimmed();
    out.level = m.captured(3).trimmed();
    out.msg = m.captured(4).trimmed();
    out.conn = QStringLiteral("-");
    const QRegularExpressionMatch sm = sshRx.match(out.msg);
    if (sm.hasMatch()) {
        out.conn = sm.captured(1).trimmed();
        out.msg = sm.captured(2).trimmed();
    }
    if (out.msg.isEmpty()) {
        out.msg = QStringLiteral("-");
    }
    return out;
}

void scrollLogViewToLatest(QPlainTextEdit* view) {
    if (!view) {
        return;
    }
    auto apply = [view]() {
        if (QScrollBar* h = view->horizontalScrollBar()) {
            h->setValue(h->minimum());
        }
        if (QScrollBar* v = view->verticalScrollBar()) {
            v->setValue(v->maximum());
        }
    };
    apply();
    QMetaObject::invokeMethod(view, apply, Qt::QueuedConnection);
}

} // namespace

void MainWindow::initLogPersistence() {
    const QString dir = m_store.configDir();
    if (dir.isEmpty()) {
        return;
    }
    m_appLogPath = dir + "/application.log";
    rotateLogIfNeeded();
}

void MainWindow::rotateLogIfNeeded() {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    const qint64 maxBytes = qint64(qMax(1, m_logMaxSizeMb)) * 1024LL * 1024LL;
    constexpr int backups = 5;

    QFileInfo fi(m_appLogPath);
    if (!fi.exists() || fi.size() < maxBytes) {
        return;
    }

    for (int i = backups; i >= 1; --i) {
        const QString src = (i == 1) ? m_appLogPath : (m_appLogPath + "." + QString::number(i - 1));
        const QString dst = m_appLogPath + "." + QString::number(i);
        if (QFile::exists(dst)) {
            QFile::remove(dst);
        }
        if (QFile::exists(src)) {
            QFile::rename(src, dst);
        }
    }
}

void MainWindow::appendLogToFile(const QString& line) {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    rotateLogIfNeeded();
    QFile f(m_appLogPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << maskSecrets(line) << '\n';
    ts.flush();
}

void MainWindow::appendLogToNative(const QString& level, const QString& line) {
    const QString msg = maskSecrets(line).trimmed();
    if (msg.isEmpty()) {
        return;
    }

#ifdef Q_OS_MACOS
    static os_log_t nativeLog = os_log_create("com.zfsmgr.app", "application");
    os_log_type_t type = OS_LOG_TYPE_DEFAULT;
    const QString lvl = level.trimmed().toLower();
    if (lvl == QStringLiteral("error")) {
        type = OS_LOG_TYPE_ERROR;
    } else if (lvl == QStringLiteral("warn")) {
        type = OS_LOG_TYPE_FAULT;
    } else if (lvl == QStringLiteral("info")) {
        type = OS_LOG_TYPE_INFO;
    } else if (lvl == QStringLiteral("debug")) {
        type = OS_LOG_TYPE_DEBUG;
    }
    const QByteArray utf8 = msg.toUtf8();
    os_log_with_type(nativeLog, type, "%{public}s", utf8.constData());
#elif defined(Q_OS_WIN)
    WORD eventType = EVENTLOG_INFORMATION_TYPE;
    const QString lvl = level.trimmed().toLower();
    if (lvl == QStringLiteral("error")) {
        eventType = EVENTLOG_ERROR_TYPE;
    } else if (lvl == QStringLiteral("warn")) {
        eventType = EVENTLOG_WARNING_TYPE;
    }
    HANDLE handle = RegisterEventSourceW(nullptr, L"ZFSMgr");
    if (!handle) {
        handle = RegisterEventSourceW(nullptr, L"Application");
    }
    if (!handle) {
        return;
    }
    const std::wstring wide = msg.toStdWString();
    LPCWSTR strings[1] = { wide.c_str() };
    ReportEventW(handle, eventType, 0, 0x1000, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(handle);
#elif defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    static bool nativeOpen = false;
    if (!nativeOpen) {
        openlog("ZFSMgr", LOG_PID | LOG_NDELAY, LOG_USER);
        nativeOpen = true;
    }
    int priority = LOG_NOTICE;
    const QString lvl = level.trimmed().toLower();
    if (lvl == QStringLiteral("error")) {
        priority = LOG_ERR;
    } else if (lvl == QStringLiteral("warn")) {
        priority = LOG_WARNING;
    } else if (lvl == QStringLiteral("info")) {
        priority = LOG_INFO;
    } else if (lvl == QStringLiteral("debug")) {
        priority = LOG_DEBUG;
    }
    const QByteArray utf8 = msg.toUtf8();
    syslog(priority, "%s", utf8.constData());
#else
    Q_UNUSED(level);
    Q_UNUSED(msg);
#endif
}

void MainWindow::clearAppLog() {
    m_logView->clear();
    m_compactPrevValid = false;
    m_compactPrevDate.clear();
    m_compactPrevTime.clear();
    m_compactPrevConn.clear();
    m_compactPrevLevel.clear();
    for (auto it = m_connectionLogViews.begin(); it != m_connectionLogViews.end(); ++it) {
        if (it.value()) {
            it.value()->clear();
        }
    }
    for (auto it = m_connectionGsaLogViews.begin(); it != m_connectionGsaLogViews.end(); ++it) {
        if (it.value()) {
            it.value()->clear();
        }
    }
    if (m_lastDetailText) {
        m_lastDetailText->clear();
    }
    if (!m_appLogPath.isEmpty()) {
        QFile f(m_appLogPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.close();
        }
    }
}

void MainWindow::copyAppLogToClipboard() {
    QClipboard* cb = QApplication::clipboard();
    if (!cb) {
        return;
    }
    QString text = QStringLiteral("[Aplicación]\n") + m_logView->toPlainText();
    for (auto it = m_connectionLogViews.constBegin(); it != m_connectionLogViews.constEnd(); ++it) {
        const QString connId = it.key();
        const QPlainTextEdit* view = it.value();
        QString connName = connId;
        for (const auto& p : m_profiles) {
            if (p.id == connId) {
                connName = p.name;
                break;
            }
        }
        text += QStringLiteral("\n\n[%1]\n%2").arg(connName, view ? view->toPlainText() : QString());
    }
    cb->setText(text);
}

QString MainWindow::maskSecrets(const QString& text) const {
    if (text.isEmpty()) {
        return text;
    }
    QString out = text;
    out.replace(
        QRegularExpression(QStringLiteral("printf\\s+'%s\\\\n'\\s+.+?\\s+\\|\\s+sudo\\s+-S\\s+-p\\s+''")),
        QStringLiteral("printf '%s\\n' [secret] | sudo -S -p ''"));
    out.replace(
        QRegularExpression(QStringLiteral("(?i)(password\\s*[:=]\\s*)\\S+")),
        QStringLiteral("\\1[secret]"));
    for (const ConnectionProfile& p : m_profiles) {
        if (!p.password.isEmpty()) {
            out.replace(p.password, QStringLiteral("[secret]"));
        }
    }
    return out;
}

void MainWindow::logUiAction(const QString& action) {
    appLog(QStringLiteral("INFO"), QStringLiteral("UI action: %1").arg(action));
}

void MainWindow::appLog(const QString& level, const QString& msg) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, level, msg]() {
            appLog(level, msg);
        }, Qt::QueuedConnection);
        return;
    }
    const QString line = QStringLiteral("[%1] [%2] %3").arg(tsNowForLog(), level, maskSecrets(msg));
    const QString current = m_logLevelSetting.isEmpty() ? QStringLiteral("normal")
                                                         : m_logLevelSetting.toLower();
    auto rank = [](const QString& l) -> int {
        const QString x = l.toLower();
        if (x == QStringLiteral("debug")) {
            return 2;
        }
        if (x == QStringLiteral("info")) {
            return 1;
        }
        return 0;
    };
    const QString lvl = level.toLower();
    const bool always = (lvl == QStringLiteral("warn") || lvl == QStringLiteral("error"));
    if (always || rank(lvl) <= rank(current)) {
        appendAppLogLineToView(line);
    }
    if (m_lastDetailText) {
        m_lastDetailText->setPlainText(line);
    }
    appendLogToFile(line);
    appendLogToNative(level, line);
}

void MainWindow::debugTrace(const QString& msg) {
    appLog(QStringLiteral("DEBUG"), msg);
}

void MainWindow::appendAppLogLineToView(const QString& fullLine) {
    if (!m_logView) {
        return;
    }
    const CompactLogParts p = parseCompactLogParts(fullLine);
    QStringList changed;
    if (!m_compactPrevValid || p.date != m_compactPrevDate) {
        changed << p.date;
    }
    if (!m_compactPrevValid || p.time != m_compactPrevTime) {
        changed << p.time;
    }
    if (!m_compactPrevValid || p.conn != m_compactPrevConn) {
        changed << QStringLiteral("ssh=%1").arg(p.conn);
    }
    if (!m_compactPrevValid || p.level != m_compactPrevLevel) {
        changed << QStringLiteral("lvl=%1").arg(p.level);
    }
    const QString head = changed.isEmpty() ? QStringLiteral("...") : changed.join(' ');
    m_logView->appendPlainText(QStringLiteral("%1 | %2").arg(head, p.msg));
    trimLogWidget(m_logView);
    m_compactPrevValid = true;
    m_compactPrevDate = p.date;
    m_compactPrevTime = p.time;
    m_compactPrevConn = p.conn;
    m_compactPrevLevel = p.level;
}

void MainWindow::loadPersistedAppLogToView() {
    if (!m_logView || m_appLogPath.isEmpty()) {
        return;
    }
    QStringList allLines;
    const QFileInfo currentFi(m_appLogPath);
    const QString baseName = currentFi.fileName();
    const QString baseDir = currentFi.absolutePath();
    // Read rotated logs from oldest to newest, then current file.
    for (int i = 5; i >= 1; --i) {
        const QString fp = QStringLiteral("%1/%2.%3").arg(baseDir, baseName).arg(i);
        QFile f(fp);
        if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        QTextStream ts(&f);
        while (!ts.atEnd()) {
            const QString ln = ts.readLine();
            if (!ln.trimmed().isEmpty()) {
                allLines.append(ln);
            }
        }
    }
    QFile current(m_appLogPath);
    if (current.exists() && current.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&current);
        while (!ts.atEnd()) {
            const QString ln = ts.readLine();
            if (!ln.trimmed().isEmpty()) {
                allLines.append(ln);
            }
        }
    }
    if (allLines.isEmpty()) {
        return;
    }
    const int limit = qMax(1, maxLogLines());
    if (allLines.size() > limit) {
        allLines = allLines.mid(allLines.size() - limit);
    }
    m_logView->clear();
    m_compactPrevValid = false;
    m_compactPrevDate.clear();
    m_compactPrevTime.clear();
    m_compactPrevConn.clear();
    m_compactPrevLevel.clear();
    for (const QString& ln : allLines) {
        appendAppLogLineToView(maskSecrets(ln));
    }
}

int MainWindow::maxLogLines() const {
    const int v = m_logMaxLinesSetting;
    if (v != 100 && v != 200 && v != 500 && v != 1000) {
        return 500;
    }
    return v;
}

void MainWindow::trimLogWidget(QPlainTextEdit* widget) {
    if (!widget) {
        return;
    }
    QTextDocument* doc = widget->document();
    if (!doc) {
        return;
    }
    const int limit = maxLogLines();
    while (doc->blockCount() > limit) {
        QTextCursor c(doc);
        c.movePosition(QTextCursor::Start);
        c.select(QTextCursor::LineUnderCursor);
        c.removeSelectedText();
        c.deleteChar();
    }
}

void MainWindow::syncConnectionLogTabs() {
    if (!m_logsTabs) {
        return;
    }
    QSet<QString> wanted;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (isConnectionDisconnected(i)) {
            continue;
        }
        const auto& p = m_profiles[i];
        const bool localConn = isLocalConnection(p);
        const QString st = (i < m_states.size()) ? m_states[i].status.trimmed().toUpper() : QString();
        const bool redirectedLocal = (!localConn && st == QStringLiteral("OK") && isLocalHostForLogs(p.host));
        if (redirectedLocal) {
            continue;
        }
        wanted.insert(p.id);
        if (m_connectionLogViews.contains(p.id)) {
            refreshConnectionGsaLogAsync(i);
            continue;
        }
        auto* tab = new QWidget(m_logsTabs);
        auto* lay = new QVBoxLayout(tab);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        auto* innerTabs = new QTabWidget(tab);
        auto* terminalPage = new QWidget(innerTabs);
        auto* terminalLay = new QVBoxLayout(terminalPage);
        terminalLay->setContentsMargins(0, 0, 0, 0);
        terminalLay->setSpacing(0);
        auto* terminalView = new QPlainTextEdit(terminalPage);
        terminalView->setReadOnly(true);
        terminalView->setLineWrapMode(QPlainTextEdit::NoWrap);
        terminalView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        terminalView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        terminalLay->addWidget(terminalView, 1);
        innerTabs->addTab(terminalPage, QStringLiteral("Terminal"));

        auto* gsaPage = new QWidget(innerTabs);
        auto* gsaLay = new QVBoxLayout(gsaPage);
        gsaLay->setContentsMargins(0, 0, 0, 0);
        gsaLay->setSpacing(0);
        auto* gsaView = new QPlainTextEdit(gsaPage);
        gsaView->setReadOnly(true);
        gsaView->setLineWrapMode(QPlainTextEdit::NoWrap);
        gsaView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        gsaView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        gsaLay->addWidget(gsaView, 1);
        innerTabs->addTab(gsaPage, QStringLiteral("GSA"));

        lay->addWidget(innerTabs, 1);
        m_logsTabs->addTab(tab, p.name);
        m_connectionLogViews.insert(p.id, terminalView);
        m_connectionGsaLogViews.insert(p.id, gsaView);
        m_connectionLogTabs.insert(p.id, tab);
        refreshConnectionGsaLogAsync(i);
    }

    for (auto it = m_connectionLogViews.begin(); it != m_connectionLogViews.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        QWidget* tab = m_connectionLogTabs.value(it.key(), nullptr);
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->removeTab(idx);
        }
        if (tab) {
            tab->deleteLater();
        }
        m_connectionGsaLogViews.remove(it.key());
        m_connectionLogTabs.remove(it.key());
        it = m_connectionLogViews.erase(it);
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        if (isConnectionDisconnected(i)) {
            continue;
        }
        const QString id = m_profiles[i].id;
        if (!wanted.contains(id)) {
            continue;
        }
        QWidget* tab = m_connectionLogTabs.value(id, nullptr);
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->setTabText(idx, m_profiles[i].name);
        }
    }
}

void MainWindow::refreshConnectionGsaLogAsync(int idx) {
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const QString connId = m_profiles[idx].id;
    QPlainTextEdit* target = m_connectionGsaLogViews.value(connId, nullptr);
    if (!target) {
        return;
    }
    if (isConnectionDisconnected(idx)) {
        target->clear();
        scrollLogViewToLatest(target);
        return;
    }

    const ConnectionProfile profile = m_profiles[idx];
    const ConnectionRuntimeState state = (idx < m_states.size()) ? m_states[idx] : ConnectionRuntimeState{};
    if (!state.gsaInstalled) {
        target->setPlainText(QStringLiteral("GSA no instalado."));
        scrollLogViewToLatest(target);
        return;
    }

    const auto gsaConfigDir = [this](const ConnectionProfile& cp, const ConnectionRuntimeState& st) {
        if (isLocalConnection(cp)) {
            return QString::fromLatin1(kGsaLinuxRuntimeDirPath);
        }
        const QString osHint = (cp.osType + QStringLiteral(" ") + st.osLine).trimmed().toLower();
        const bool isMac = osHint.contains(QStringLiteral("darwin")) || osHint.contains(QStringLiteral("mac"));
        const bool isFreeBsd = osHint.contains(QStringLiteral("freebsd"));
        const bool isWindows = osHint.contains(QStringLiteral("windows"));
        QString user = cp.username.trimmed();
        if (isWindows) {
            const int slash = qMax(user.lastIndexOf(QLatin1Char('\\')), user.lastIndexOf(QLatin1Char('/')));
            if (slash >= 0) {
                user = user.mid(slash + 1);
            }
            if (user.isEmpty()) {
                user = QStringLiteral("Default");
            }
            return QStringLiteral("C:\\Users\\%1\\.config\\ZFSMgr").arg(user);
        }
        if (user.isEmpty()) {
            user = QStringLiteral("root");
        }
        if (isMac) {
            return (user == QStringLiteral("root"))
                       ? QStringLiteral("/var/root/.config/ZFSMgr")
                       : QStringLiteral("/Users/%1/.config/ZFSMgr").arg(user);
        }
        if (isFreeBsd) {
            return QString::fromLatin1(kGsaFreeBsdRuntimeDirPath);
        }
        return QString::fromLatin1(kGsaLinuxRuntimeDirPath);
    };

    const QString configDir = gsaConfigDir(profile, state);
    if (configDir.isEmpty()) {
        target->clear();
        return;
    }

    const bool isWindows = isWindowsConnection(profile);
    const QString remoteCmd = isWindows
        ? QStringLiteral(
              "$f='%1\\GSA.log'; "
              "if (Test-Path -LiteralPath $f) { Get-Content -LiteralPath $f -Raw }")
              .arg(configDir)
        : withSudo(profile,
                   QStringLiteral("f=%1; if [ -f \"$f\" ]; then cat \"$f\"; fi")
                       .arg(mwhelpers::shSingleQuote(QDir::cleanPath(configDir + QStringLiteral("/GSA.log")))));
    const WindowsCommandMode mode = isWindows ? WindowsCommandMode::PowerShellNative
                                              : WindowsCommandMode::Auto;

    (void)QtConcurrent::run([this, connId, profile, remoteCmd, mode, state]() {
        QString out;
        QString err;
        int rc = -1;
        const bool ok = runSsh(profile, remoteCmd, 15000, out, err, rc, {}, {}, {}, mode) && rc == 0;
        QString text;
        if (!state.gsaKnownConnections.isEmpty()) {
            text += QStringLiteral("Conexiones dadas de alta en GSA: %1\n\n")
                        .arg(state.gsaKnownConnections.join(QStringLiteral(", ")));
        }
        if (ok) {
            text += out;
        }
        QMetaObject::invokeMethod(this, [this, connId, text]() {
            if (QPlainTextEdit* view = m_connectionGsaLogViews.value(connId, nullptr)) {
                view->setPlainText(maskSecrets(text));
                scrollLogViewToLatest(view);
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::appendConnectionLog(const QString& connId, const QString& line) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, connId, line]() {
            appendConnectionLog(connId, line);
        }, Qt::QueuedConnection);
        return;
    }
    QString connName = connId;
    for (const auto& p : m_profiles) {
        if (p.id == connId) {
            connName = p.name.trimmed().isEmpty() ? p.id : p.name;
            break;
        }
    }
    // Mirror SSH/PSRP session output into Application log so the SSH tabs can be removed
    // without losing per-connection command/output traceability.
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("[SSH %1] %2").arg(connName, maskSecrets(line)));

    QPlainTextEdit* view = m_connectionLogViews.value(connId, nullptr);
    if (!view) {
        return;
    }
    view->appendPlainText(QStringLiteral("[%1] %2").arg(tsNowForLog(), maskSecrets(line)));
    trimLogWidget(view);
    scrollLogViewToLatest(view);
}
