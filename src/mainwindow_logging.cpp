#include "mainwindow.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
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
    const QString current = m_logLevelCombo ? m_logLevelCombo->currentText().toLower() : QStringLiteral("normal");
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
        m_logView->appendPlainText(line);
        trimLogWidget(m_logView);
    }
    if (m_lastDetailText) {
        m_lastDetailText->setPlainText(line);
    }
    appendLogToFile(line);
}

int MainWindow::maxLogLines() const {
    bool ok = false;
    const int v = m_logMaxLinesCombo ? m_logMaxLinesCombo->currentText().toInt(&ok) : 500;
    if (!ok || v <= 0) {
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
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSize(8);
        view->setFont(mono);
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
    QPlainTextEdit* view = m_connectionLogViews.value(connId, nullptr);
    if (!view) {
        return;
    }
    view->appendPlainText(QStringLiteral("[%1] %2").arg(tsNowForLog(), maskSecrets(line)));
    trimLogWidget(view);
}
