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
    QString language = QStringLiteral("es");
    ConnectionStore store(QStringLiteral("ZFSMgr"));
    while (true) {
        MasterPasswordDialog dlg;
        dlg.setSelectedLanguage(language);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        masterPassword = dlg.password();
        language = dlg.selectedLanguage();
        store.setMasterPassword(masterPassword);
        QString err;
        if (store.validateMasterPassword(err)) {
            break;
        }
        const QString msg = (language == QStringLiteral("en"))
                                ? QStringLiteral("Invalid master password.\n%1").arg(err)
                                : (language == QStringLiteral("zh"))
                                      ? QStringLiteral("主密码错误。\n%1").arg(err)
                                      : QStringLiteral("Password maestro incorrecto.\n%1").arg(err);
        QMessageBox::warning(
            nullptr,
            QStringLiteral("ZFSMgr"),
            msg);
    }

    MainWindow w(masterPassword, language);
    w.show();
    return app.exec();
}
