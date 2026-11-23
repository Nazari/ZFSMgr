// zfsmgr-gui: the graphical interface. A client of `zfsmgr-cli --serve` and nothing
// else: it links only Qt, launches no process but that one, and every rule lives in the CLI.
//
// Start-up: a service is started with an EMPTY master password first —with nothing
// encrypted, no question is needed—; only if it answers «wrong master password» is the
// person asked, and asked again while it keeps being wrong.

#include "buildstamp.h"
#include "cliservice.h"
#include "mainwindow.h"
#include "text.h"
#include "unlockdialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFile>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QIcon>
#include <QInputDialog>
#include <QTimer>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

using namespace zfsmgr::gui;

namespace {

// Knows when a click or a key is being handled, and on which control: a request made inside
// one is the person's, shows the busy cursor and keeps that control disabled until its
// answer (BusyHooks). notify() wraps the whole dispatch, so the slot a click ends in —and the
// request it makes— runs inside it.
class GuiApp : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject* receiver, QEvent* e) override {
        switch (e->type()) {
            case QEvent::MouseButtonRelease:
            case QEvent::MouseButtonDblClick:
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
            case QEvent::Shortcut: {
                const QPointer<QWidget> before = m_control;
                ++m_input;
                m_control = controlOf(receiver);
                const bool r = QApplication::notify(receiver, e);
                --m_input;
                m_control = before;
                return r;
            }
            default:
                return QApplication::notify(receiver, e);
        }
    }
    bool inUserInput() const { return m_input > 0; }
    QWidget* control() const { return m_control; }

private:
    // The button pressed; for an entry of a button's menu, that button.
    static QWidget* controlOf(QObject* o) {
        if (auto* b = qobject_cast<QAbstractButton*>(o)) {
            return b;
        }
        if (auto* m = qobject_cast<QMenu*>(o)) {
            QWidget* w = m;
            while (qobject_cast<QMenu*>(w) && w->parentWidget()) {
                w = w->parentWidget();
            }
            return qobject_cast<QAbstractButton*>(w);
        }
        return nullptr;
    }

    int m_input{0};
    QPointer<QWidget> m_control;
};

void installBusyCursor(GuiApp& app) {
    BusyHooks::userInput = [&app] { return app.inUserInput(); };
    BusyHooks::adjust = [](int delta) {
        static int count = 0;
        const bool before = count > 0;
        count = std::max(0, count + delta);
        const bool now = count > 0;
        if (now && !before) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
        } else if (!now && before) {
            QApplication::restoreOverrideCursor();
        }
    };
    // Per control, how many of its requests are still out; it is enabled again with the last,
    // and only if it was this that disabled it. A control rebuilt meanwhile —a page reloads
    // after an action— is a new one, and the old pointer just goes null.
    struct Held {
        QPointer<QWidget> w;
        int count{0};
        bool disabledHere{false};
    };
    static std::map<QWidget*, Held> held;
    static std::map<int, QWidget*> tokens;
    static int next = 0;
    BusyHooks::begin = [&app]() -> int {
        const int token = ++next;
        QWidget* w = app.control();
        tokens[token] = w;
        if (w) {
            Held& h = held[w];
            if (!h.w) {
                h = Held{};  // a dead control's entry, its address reused by a new one
            }
            if (h.count++ == 0) {
                h.w = w;
                h.disabledHere = w->isEnabled();
                w->setEnabled(false);
            }
        }
        return token;
    };
    BusyHooks::end = [](int token) {
        const auto t = tokens.find(token);
        if (t == tokens.end()) {
            return;
        }
        QWidget* key = t->second;
        tokens.erase(t);
        const auto it = key ? held.find(key) : held.end();
        if (it == held.end() || --it->second.count > 0) {
            return;
        }
        if (it->second.w && it->second.disabledHere) {
            it->second.w->setEnabled(true);
        }
        held.erase(it);
    };
}

class Starter : public QObject {
public:
    explicit Starter(QString cli) : m_cli(std::move(cli)) {}

    void attempt(const QByteArray& master) {
        auto* s = new CliService(m_cli, uiLang());
        m_master = master;
        connect(s, &CliService::ready, this, [this, s](const QString&) {
            s->disconnect(this);
            // No master password yet: the welcome, which creates it, and a service started
            // again with it —the CLI records the first master it is given—.
            if (s->noMaster() && m_master.isEmpty()) {
                s->deleteLater();
                welcome();
                return;
            }
            // Opened while locked —a touch or the password—: the dialog goes away by itself.
            if (m_unlock) {
                m_unlocked = true;
                m_unlock->close();
            }
            // From here the window owns the service and listens to it; the starter does not.
            // The other services get the DATA KEY this one opened, not the password: with a
            // key registered, each would otherwise ask for its own touch.
            const bool conClave = !s->dataKey().isEmpty();
            auto* w = new MainWindow(m_cli, conClave ? s->dataKey() : m_master, conClave, s);
            w->setAttribute(Qt::WA_DeleteOnClose);
            w->show();
            if (m_firstRun) {
                w->startFirstRun();
            }
            m_master.fill('\0');
            // From now on, closing the window ends the program.
            QApplication::setQuitOnLastWindowClosed(true);
            // For tests without a screen: ZFSMGR_GUI_CAPTURE=<file.png> saves what the window
            // shows once the machines have had time to answer, and ends.
            const QString capture = qEnvironmentVariable("ZFSMGR_GUI_CAPTURE");
            if (!capture.isEmpty()) {
                if (qEnvironmentVariableIntValue("ZFSMGR_GUI_ADVANCED") > 0) {
                    w->setAdvanced(true, /*remember=*/false);
                }
                w->showPage(qEnvironmentVariableIntValue("ZFSMGR_GUI_PAGE"));
                if (const QString r = qEnvironmentVariable("ZFSMGR_GUI_REVEAL"); !r.isEmpty()) {
                    // «machine» or «machine/pool/dataset»: the Machines page, opened there.
                    w->showMachine(r.section(QLatin1Char('/'), 0, 0),
                                   r.section(QLatin1Char('/'), 1));
                }
                if (qEnvironmentVariable("ZFSMGR_GUI_OPEN") == QLatin1String("newcopy")) {
                    // After the first answers, as a person would: the assistant reads them.
                    QTimer::singleShot(4000, w, [w] { w->openNewCopy(); });
                }
                QTimer::singleShot(qEnvironmentVariableIntValue("ZFSMGR_GUI_CAPTURE_MS") > 0
                                       ? qEnvironmentVariableIntValue("ZFSMGR_GUI_CAPTURE_MS")
                                       : 8000,
                                   w, [w, capture] {
                                       // The dialog on top, if one is open.
                                       QWidget* top = QApplication::activeModalWidget();
                                       (top ? top : w)->grab().save(capture);
                                       // Closed as a person closes it, not with exit(): that
                                       // way every capture also goes through the window's
                                       // destruction, where the exit crash was.
                                       if (top) {
                                           top->close();
                                       }
                                       w->close();
                                   });
            }
        });
        connect(s, &CliService::failed, this, [this, s](const QString& why, bool wrongPassword) {
            s->deleteLater();
            if (!wrongPassword) {
                QMessageBox::critical(nullptr, appTitle(),
                                      L("The command line client could not start:\n%1",
                                        "No se pudo arrancar el cliente de línea de órdenes:\n%1")
                                          .arg(why));
                QApplication::exit(1);
                return;
            }
            askPassword(m_asked);
        });
        // Keys are registered: the service waits for a touch, and the password can be typed
        // meanwhile into the same service (UnlockDialog).
        connect(s, &CliService::locked, this,
                [this, s](bool keyPresent) { unlockWindow(s, keyPresent); });
        connect(s, &CliService::touchWaiting, this, [this] {
            if (m_unlock) {
                m_unlock->setTouchWaiting();
            }
        });
        connect(s, &CliService::keyPresence, this, [this](bool present) {
            if (m_unlock) {
                m_unlock->setKeyPresent(present);
            }
        });
        connect(s, &CliService::unlockFailed, this,
                [this](const QString& reason, const QString& code) {
                    if (!m_unlock) {
                        return;
                    }
                    if (reason == QLatin1String("password")) {
                        m_unlock->setPasswordWrong();
                    } else {
                        m_unlock->setKeyFailed(code);
                    }
                });
        s->start(master);
    }

private:
    // The first run on this computer (or a configuration that never had a master password):
    // the master is created here, typed twice. Without it no password can be stored —not
    // even this computer's sudo one— so there is no going on without it.
    void welcome() {
        auto* d = new QDialog();
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle(appTitle());
        auto* lay = new QVBoxLayout(d);
        auto* head = new QLabel(L("<b>Welcome to ZFSMgr</b>", "<b>Bienvenido a ZFSMgr</b>"), d);
        lay->addWidget(head);
        auto* text = new QLabel(
            L("This is the first time on this computer. Choose a master password: it encrypts "
              "the passwords of your machines, which ZFSMgr keeps here, and it will be asked "
              "for every time it starts. It is not stored anywhere: if it is forgotten, those "
              "passwords have to be entered again.",
              "Es la primera vez en este ordenador. Elige una contraseña maestra: cifra las "
              "contraseñas de tus máquinas, que ZFSMgr guarda aquí, y se pedirá cada vez que "
              "arranque. No se guarda en ningún sitio: si se olvida, hay que volver a dar esas "
              "contraseñas."),
            d);
        text->setWordWrap(true);
        lay->addWidget(text);
        auto* form = new QFormLayout();
        auto* one = new QLineEdit(d);
        auto* two = new QLineEdit(d);
        one->setEchoMode(QLineEdit::Password);
        two->setEchoMode(QLineEdit::Password);
        form->addRow(L("Master password", "Contraseña maestra"), one);
        form->addRow(L("Again", "Repítela"), two);
        lay->addLayout(form);
        auto* why = new QLabel(d);
        lay->addWidget(why);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
        bb->button(QDialogButtonBox::Ok)->setText(L("Create and go on", "Crearla y seguir"));
        lay->addWidget(bb);
        const auto check = [one, two, why, bb] {
            QString problem;
            if (one->text().isEmpty()) {
                problem = L("Type it twice.", "Escríbela dos veces.");
            } else if (two->text().isEmpty()) {
                problem = L("Type it again below.", "Repítela debajo.");
            } else if (one->text() != two->text()) {
                problem = L("The two are not the same.", "Las dos no coinciden.");
            }
            why->setText(problem);
            bb->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
        };
        connect(one, &QLineEdit::textChanged, d, check);
        connect(two, &QLineEdit::textChanged, d, check);
        check();
        connect(bb, &QDialogButtonBox::accepted, d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
        connect(d, &QDialog::finished, this, [this, one](int result) {
            if (result != QDialog::Accepted) {
                QApplication::exit(0);
                return;
            }
            m_firstRun = true;
            attempt(one->text().toUtf8());
        });
        d->resize(460, d->sizeHint().height());
        d->open();
        // For tests without a screen: ZFSMGR_GUI_WELCOME_CAPTURE=<file.png> saves the welcome
        // and goes on with a test master password, so the rest of the first run can be seen.
        if (const QString shot = qEnvironmentVariable("ZFSMGR_GUI_WELCOME_CAPTURE");
            !shot.isEmpty()) {
            QTimer::singleShot(1500, d, [d, one, two, bb, shot] {
                d->grab().save(shot);
                one->setText(QStringLiteral("prueba123"));
                two->setText(QStringLiteral("prueba123"));
                bb->button(QDialogButtonBox::Ok)->click();
            });
        }
    }

    // The locked service's window: the password goes to the SAME service, which is waiting
    // for a touch at the same time; whichever comes first opens, and the window goes away.
    void unlockWindow(CliService* s, bool keyPresent) {
        auto* d = new UnlockDialog(keyPresent);
        d->setAttribute(Qt::WA_DeleteOnClose);
        m_unlock = d;
        connect(d, &UnlockDialog::passwordEntered, this,
                [s](const QByteArray& pw) { s->unlockWithPassword(pw); });
        connect(d, &QDialog::finished, this, [this](int) {
            if (!m_unlocked) {
                QApplication::exit(0);
            }
        });
        d->open();
        // For tests without a screen: ZFSMGR_GUI_UNLOCK_CAPTURE=<file.png> saves this window and
        // ends.
        if (const QString shot = qEnvironmentVariable("ZFSMGR_GUI_UNLOCK_CAPTURE");
            !shot.isEmpty()) {
            QTimer::singleShot(1500, d, [d, shot] {
                d->grab().save(shot);
                d->close();
            });
        }
    }

    void askPassword(bool again) {
        m_asked = true;
        auto* d = new QInputDialog();
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle(appTitle());
        d->setLabelText(again ? L("That is not the master password. Try again:",
                                  "Esa no es la contraseña maestra. Prueba otra vez:")
                              : L("Master password (the one that encrypts the stored passwords):",
                                  "Contraseña maestra (la que cifra las contraseñas guardadas):"));
        d->setTextEchoMode(QLineEdit::Password);
        connect(d, &QDialog::finished, this, [this, d](int result) {
            if (result != QDialog::Accepted) {
                QApplication::exit(0);
                return;
            }
            attempt(d->textValue().toUtf8());
        });
        d->open();
    }

    QString m_cli;
    QByteArray m_master;
    bool m_asked{false};
    bool m_firstRun{false};
    QPointer<UnlockDialog> m_unlock;
    bool m_unlocked{false};
};

}  // namespace

int main(int argc, char** argv) {
    GuiApp app(argc, argv);
    installBusyCursor(app);
    QApplication::setApplicationName(QStringLiteral("ZFSMgr"));
    // `--version`: the only way to check WITHOUT A SCREEN that the interface starts. After
    // creating QApplication on purpose: by then Qt's libraries and the platform plugin are
    // loaded, which is what fails in a broken installation. The installers call it with
    // QT_QPA_PLATFORM=offscreen.
    if (QApplication::arguments().contains(QStringLiteral("--version"))) {
        std::fprintf(stdout, "ZFSMgr %s\n", buildStamp().toUtf8().constData());
        std::fflush(stdout);
        return 0;
    }
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
    // Preferences in ~/.config/ZFSMgr/zfsmgr-gui.conf, beside the CLI's configuration.
    QApplication::setOrganizationName(QStringLiteral("ZFSMgr"));
    QApplication::setApplicationName(QStringLiteral("zfsmgr-gui"));
    // Without windows open while the password is being asked, closing the last dialog must
    // not end the program before the main window exists.
    QApplication::setQuitOnLastWindowClosed(false);
    // The language chosen in Administration, if one was: before any text is written, since
    // uiLang() decides once. Without it, the system's.
    if (const QString lang = QSettings().value(QStringLiteral("gui/lang")).toString();
        lang == QLatin1String("es") || lang == QLatin1String("en")) {
        QLocale::setDefault(
            QLocale(lang == QLatin1String("es") ? QLocale::Spanish : QLocale::English));
    }

    const QString cli = findCli();
    if (cli.isEmpty()) {
        QMessageBox::critical(nullptr, appTitle(),
                              L("zfsmgr-cli was not found next to this program nor on PATH.",
                                "No se encontró zfsmgr-cli junto a este programa ni en el PATH."));
        return 1;
    }
    // `--password-fd <n>`: the master password from that descriptor, as in the CLI. Never
    // from argv or the environment.
    QByteArray master;
    const QStringList args = QApplication::arguments();
    const qsizetype at = args.indexOf(QStringLiteral("--password-fd"));
    if (at >= 0 && at + 1 < args.size()) {
        QFile fd;
        if (fd.open(args.at(at + 1).toInt(), QIODevice::ReadOnly)) {
            master = fd.readLine();
            while (master.endsWith('\n') || master.endsWith('\r')) {
                master.chop(1);
            }
        }
    }
    Starter starter(cli);
    starter.attempt(master);
    master.fill('\0');
    return app.exec();
}
