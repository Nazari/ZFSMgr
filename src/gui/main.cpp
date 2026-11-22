// zfsmgr-gui2: the new graphical interface. A client of `zfsmgr-cli --serve` and nothing
// else: it links only Qt, launches no process but that one, and every rule lives in the CLI.
//
// Start-up: a service is started with an EMPTY master password first —with nothing
// encrypted, no question is needed—; only if it answers «wrong master password» is the
// person asked, and asked again while it keeps being wrong.

#include "cliservice.h"
#include "mainwindow.h"
#include "text.h"

#include <QApplication>
#include <QFile>
#include <QInputDialog>
#include <QTimer>
#include <QLineEdit>
#include <QMessageBox>

#include <memory>

using namespace zfsmgr::gui;

namespace {

class Starter : public QObject {
public:
    explicit Starter(QString cli) : m_cli(std::move(cli)) {}

    void attempt(const QByteArray& master) {
        auto* s = new CliService(m_cli, uiLang());
        m_master = master;
        connect(s, &CliService::ready, this, [this, s](const QString&) {
            // From here the window owns the service and listens to it; the starter does not.
            s->disconnect(this);
            auto* w = new MainWindow(m_cli, m_master, s);
            w->setAttribute(Qt::WA_DeleteOnClose);
            w->show();
            m_master.fill('\0');
            // From now on, closing the window ends the program.
            QApplication::setQuitOnLastWindowClosed(true);
            // For tests without a screen: ZFSMGR_GUI_CAPTURE=<file.png> saves what the window
            // shows once the machines have had time to answer, and ends.
            const QString capture = qEnvironmentVariable("ZFSMGR_GUI_CAPTURE");
            if (!capture.isEmpty()) {
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
                                       QApplication::exit(0);
                                   });
            }
        });
        connect(s, &CliService::failed, this, [this, s](const QString& why, bool wrongPassword) {
            s->deleteLater();
            if (!wrongPassword) {
                QMessageBox::critical(nullptr, QStringLiteral("ZFSMgr"),
                                      L("The command line client could not start:\n%1",
                                        "No se pudo arrancar el cliente de línea de órdenes:\n%1")
                                          .arg(why));
                QApplication::exit(1);
                return;
            }
            askPassword(m_asked);
        });
        s->start(master);
    }

private:
    void askPassword(bool again) {
        m_asked = true;
        auto* d = new QInputDialog();
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle(QStringLiteral("ZFSMgr"));
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
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ZFSMgr"));
    // Without windows open while the password is being asked, closing the last dialog must
    // not end the program before the main window exists.
    QApplication::setQuitOnLastWindowClosed(false);

    const QString cli = findCli();
    if (cli.isEmpty()) {
        QMessageBox::critical(nullptr, QStringLiteral("ZFSMgr"),
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
