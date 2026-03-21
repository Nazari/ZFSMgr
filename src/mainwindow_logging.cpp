#include "mainwindow.h"

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
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QThread>
#include <QSet>
#include <QSysInfo>
#include <QVBoxLayout>

namespace {
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
            continue;
        }
        auto* tab = new QWidget(m_logsTabs);
        auto* lay = new QVBoxLayout(tab);
        auto* view = new QPlainTextEdit(tab);
        view->setReadOnly(true);
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        lay->addWidget(view, 1);
        m_logsTabs->addTab(tab, p.name);
        m_connectionLogViews.insert(p.id, view);
    }

    for (auto it = m_connectionLogViews.begin(); it != m_connectionLogViews.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        QWidget* tab = it.value() ? it.value()->parentWidget() : nullptr;
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->removeTab(idx);
        }
        if (tab) {
            tab->deleteLater();
        }
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
        QWidget* tab = m_connectionLogViews.value(id)
                           ? m_connectionLogViews.value(id)->parentWidget()
                           : nullptr;
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->setTabText(idx, m_profiles[i].name);
        }
    }
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
}
