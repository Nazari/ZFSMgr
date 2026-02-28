#include "connectionstore.h"
#include "masterpassworddialog.h"
#include "mainwindow.h"

#include <QApplication>
#include <QMessageBox>
#include <QSettings>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ZFSMgr"));
    QApplication::setApplicationName(QStringLiteral("ZFSMgr"));

    QString masterPassword;
    QString language = QStringLiteral("es");
    ConnectionStore store(QStringLiteral("ZFSMgr"));
    {
        QSettings ini(store.iniPath(), QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("ui"));
        const QString persistedLang = ini.value(QStringLiteral("language"), QStringLiteral("es")).toString().trimmed().toLower();
        ini.endGroup();
        if (persistedLang == QStringLiteral("es") || persistedLang == QStringLiteral("en") || persistedLang == QStringLiteral("zh")) {
            language = persistedLang;
        }
    }
    while (true) {
        MasterPasswordDialog dlg;
        dlg.setSelectedLanguage(language);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        masterPassword = dlg.password();
        language = dlg.selectedLanguage();
        {
            QSettings ini(store.iniPath(), QSettings::IniFormat);
            ini.beginGroup(QStringLiteral("ui"));
            ini.setValue(QStringLiteral("language"), language);
            ini.endGroup();
            ini.sync();
        }
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
