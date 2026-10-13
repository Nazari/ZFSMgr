#include "mainwindow.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include <memory>

// A connection's log and its daemon's log, each in a window of its own opened from the
// Connections menu. They used to be two tabs under each pane.
//
// A window's view does NOT own its text: it shows the document of the hidden view that
// syncConnectionLogTabs() creates per connection and appendConnectionLog() writes to. That is
// why releaseLogWindowsOf() must run before an owner is destroyed.

namespace {

// A read-only log view that follows the last line while the user is at the bottom, and stops
// following as soon as they scroll up to read.
//
// It moves the CURSOR, not the scroll bar: the document is shared with the hidden owner, whose
// layout governs the bar (measured: maximum 400 in the owner, 0 in the view).
QPlainTextEdit* vistaQueSigueLaCola(QWidget* padre, const QFont& fuente) {
    auto* view = new QPlainTextEdit(padre);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(fuente);
    auto pegado = std::make_shared<bool>(true);
    if (QScrollBar* sb = view->verticalScrollBar()) {
        QObject::connect(sb, &QScrollBar::valueChanged, view, [sb, pegado](int v) {
            *pegado = (v >= sb->maximum() - 2);  // a half-shown last line is still «bottom»
        });
    }
    QObject::connect(view, &QPlainTextEdit::textChanged, view, [view, pegado]() {
        if (!*pegado) {
            return;
        }
        view->moveCursor(QTextCursor::End);
        view->ensureCursorVisible();
        // Back to the start of the line: that is where the time and the machine are.
        if (QScrollBar* h = view->horizontalScrollBar()) {
            h->setValue(h->minimum());
        }
    });
    return view;
}

}  // namespace

void MainWindow::openConnectionLogWindow(int connIdx, bool daemon) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }
    const ConnectionProfile& p = m_conns.profiles.at(connIdx);
    QPlainTextEdit* owner = daemon ? m_connectionGsaLogViews.value(p.id, nullptr)
                                   : m_connectionLogViews.value(p.id, nullptr);
    if (!owner) {
        return;
    }
    // One window per connection and kind: asking again brings it to the front.
    for (const LogWindow& w : std::as_const(m_logWindows)) {
        if (w.dialog && w.connId == p.id && w.daemon == daemon) {
            w.dialog->raise();
            w.dialog->activateWindow();
            return;
        }
    }
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(daemon ? QStringLiteral("daemonLogWindow")
                              : QStringLiteral("connectionLogWindow"));
    dlg->setWindowTitle(
        (daemon ? trk(QStringLiteral("t_daemon_log_win_001"), QStringLiteral("Log del daemon · %1"),
                      QStringLiteral("Daemon log · %1"))
                : trk(QStringLiteral("t_conn_log_win_001"), QStringLiteral("Log · %1"),
                      QStringLiteral("Log · %1")))
            .arg(p.name));
    auto* layout = new QVBoxLayout(dlg);
    if (daemon) {
        auto* fila = new QHBoxLayout();
        auto* latido = new QPushButton(QStringLiteral("Heartbeat"), dlg);
        const QString connId = p.id;
        connect(latido, &QPushButton::clicked, this,
                [this, connId]() { runDaemonHeartbeat(connId); });
        fila->addWidget(latido, 0);
        fila->addStretch(1);
        layout->addLayout(fila);
    }
    QPlainTextEdit* view = vistaQueSigueLaCola(dlg, m_logView ? m_logView->font() : font());
    view->setDocument(owner->document());
    view->moveCursor(QTextCursor::End);
    view->ensureCursorVisible();
    layout->addWidget(view, 1);
    dlg->resize(900, 480);
    m_logWindows.push_back(LogWindow{dlg, view, p.id, daemon});
    dlg->show();
    if (daemon) {
        refreshConnectionDaemonLogAsync(connIdx, /*fullReset=*/false, /*userAsked=*/true);
    }
}

void MainWindow::releaseLogWindowsOf(const QString& connId) {
    for (int i = m_logWindows.size() - 1; i >= 0; --i) {
        const LogWindow& w = m_logWindows.at(i);
        if (!w.dialog) {
            m_logWindows.removeAt(i);
            continue;
        }
        if (w.connId != connId) {
            continue;
        }
        // `nullptr` makes the view create an empty document of its own; a bare
        // QTextDocument lacks the layout QPlainTextEdit needs and Qt ignores it.
        if (w.view) {
            w.view->setDocument(nullptr);
        }
        w.dialog->close();
        m_logWindows.removeAt(i);
    }
}
