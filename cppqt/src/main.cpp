#include "connectionstore.h"
#include "masterpassworddialog.h"
#include "mainwindow.h"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ZFSMgr"));
    QApplication::setApplicationName(QStringLiteral("ZFSMgr"));

    QString masterPassword;
    ConnectionStore store(QStringLiteral("ZFSMgr"));
    while (true) {
        MasterPasswordDialog dlg;
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        masterPassword = dlg.password();
        store.setMasterPassword(masterPassword);
        QString err;
        if (store.validateMasterPassword(err)) {
            break;
        }
        QMessageBox::warning(
            nullptr,
            QStringLiteral("ZFSMgr"),
            QStringLiteral("Password maestro incorrecto.\n%1").arg(err));
    }

    MainWindow w(masterPassword);
    w.show();
    return app.exec();
}
