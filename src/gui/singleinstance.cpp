#include "singleinstance.h"

#include "../base/sessionlock.h"
#include "buildstamp.h"
#include "text.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <memory>

namespace zfsmgr::gui {
namespace {

namespace S = zfsmgr::base::session;

// «Show yourself»: the second launch writes a token here and the first writes it back beside
// it once its windows are in front. Per user —the runtime directory is—, so a second user
// can never raise the first user's windows.
QString raisePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    return dir + QStringLiteral("/zfsmgr-gui-raise");
}

QString ackPath() {
    return raisePath() + QStringLiteral(".ack");
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll().trimmed();
}

void writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(data);
    }
}

// Asks the first interface to come to the front, and waits a little for it to say it did.
bool askToRaise() {
    const QByteArray token = QByteArray::number(QApplication::applicationPid()) + '-'
                             + QByteArray::number(QRandomGenerator::global()->generate());
    writeFile(raisePath(), token);
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 3000) {
        if (readFile(ackPath()) == token) {
            return true;
        }
        QThread::msleep(100);
    }
    return false;
}

QString describe(const S::Holder& h) {
    if (h.pid <= 0) {
        return L("another session, which has not said yet which one it is",
                 "otra sesión, que todavía no ha dicho cuál es");
    }
    QString since = QStringLiteral("?");
    if (h.since > 0) {
        const QDateTime when = QDateTime::fromSecsSinceEpoch(h.since);
        since = when.date() == QDate::currentDate()
                    ? when.toString(QStringLiteral("HH:mm"))
                    : when.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    }
    const QString user = QString::fromStdString(h.user);
    if (h.kind == "gui") {
        return L("the interface of %1 (PID %2, since %3)",
                 "la interfaz gráfica de %1 (PID %2, desde %3)")
            .arg(user)
            .arg(h.pid)
            .arg(since);
    }
    return L("the zfsmgr-cli shell of %1 (PID %2, since %3)",
             "el intérprete de zfsmgr-cli de %1 (PID %2, desde %3)")
        .arg(user)
        .arg(h.pid)
        .arg(since);
}

}  // namespace

bool takeSession(S::Lock& lock) {
    bool askedToRaise = false;
    for (;;) {
        S::Holder h;
        std::string detail;
        const S::Acquire r = lock.acquire("gui", h, detail);
        if (r == S::Acquire::Held) {
            return true;
        }
        if (r == S::Acquire::Failed) {
            // Not a reason to refuse: only that two could be open at once, as before.
            std::fprintf(stderr, "zfsmgr-gui: cannot check whether ZFSMgr is already open: %s\n",
                         detail.c_str());
            return true;
        }
        const bool mine = h.kind == "gui" && h.user == S::currentUser();
        if (mine && !askedToRaise) {
            askedToRaise = true;
            if (askToRaise()) {
                return false;
            }
        }
        QMessageBox box(QMessageBox::Warning, appTitle(),
                        L("ZFSMgr is already open on this computer: %1.",
                          "ZFSMgr ya está abierto en este ordenador: %1.")
                            .arg(describe(h)));
        QString info =
            L("There can be only one session at a time on each computer, this "
              "interface or the zfsmgr-cli shell.",
              "Solo puede haber una sesión a la vez en cada ordenador, sea esta "
              "interfaz o el intérprete de zfsmgr-cli.");
        if (mine) {
            info += QStringLiteral("\n\n")
                    + L("It was asked to come to the front and did not answer: it may be hung.",
                        "Se le pidió que se mostrara y no contestó: puede estar colgada.");
        }
        box.setInformativeText(info);
        QPushButton* close = nullptr;
        if (h.pid > 0) {
            close = box.addButton(
                L("Close that session and open this one", "Cerrar esa sesión y abrir esta"),
                QMessageBox::AcceptRole);
        }
        QPushButton* quit = box.addButton(L("Quit", "Salir"), QMessageBox::RejectRole);
        box.setDefaultButton(quit);
        // For tests without a screen: ZFSMGR_GUI_SESSION_CAPTURE=<file.png> saves this dialog
        // and answers «Quit».
        const QString capture = qEnvironmentVariable("ZFSMGR_GUI_SESSION_CAPTURE");
        if (!capture.isEmpty()) {
            QTimer::singleShot(500, &box, [&box, capture] {
                box.grab().save(capture);
                box.reject();
            });
        }
        box.exec();
        if (!close || box.clickedButton() != close) {
            return false;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const S::Stop stopped = S::stop(h, detail);
        QApplication::restoreOverrideCursor();
        switch (stopped) {
            case S::Stop::Stopped:
                continue;  // and take it
            case S::Stop::NotAllowed:
                QMessageBox::warning(
                    nullptr, appTitle(),
                    L("It belongs to %1, and only that person or an administrator can close it.",
                      "Es de %1, y solo esa persona o un administrador puede cerrarla.")
                        .arg(QString::fromStdString(h.user)));
                return false;
            case S::Stop::StillRunning:
                QMessageBox::warning(
                    nullptr, appTitle(),
                    L("Process %1 does not end.", "El proceso %1 no termina.").arg(h.pid));
                return false;
            case S::Stop::Failed:
                QMessageBox::warning(nullptr, appTitle(),
                                     L("It could not be closed: %1", "No se pudo cerrar: %1")
                                         .arg(QString::fromStdString(detail)));
                return false;
        }
    }
}

void listenForRaise(QObject* parent) {
    // Old requests are not for this one.
    QFile::remove(raisePath());
    QFile::remove(ackPath());
    auto* timer = new QTimer(parent);
    auto last = std::make_shared<QByteArray>();
    QObject::connect(timer, &QTimer::timeout, parent, [last] {
        const QByteArray token = readFile(raisePath());
        if (token.isEmpty() || token == *last) {
            return;
        }
        *last = token;
        QWidget* modal = QApplication::activeModalWidget();
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (!w->isVisible() || w == modal) {
                continue;
            }
            w->setWindowState((w->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            w->raise();
            w->activateWindow();
        }
        if (modal) {
            modal->raise();
            modal->activateWindow();
        }
        writeFile(ackPath(), token);
    });
    timer->start(400);
}

}  // namespace zfsmgr::gui
