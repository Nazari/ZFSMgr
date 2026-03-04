#include "connectionstore.h"
#include "i18nmanager.h"
#include "masterpassworddialog.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>

namespace {
QString tr3(const QString& lang, const QString& es, const QString& en, const QString& zh) {
    return I18nManager::instance().translate(lang, es, en, zh);
}
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
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
    store.setLanguage(language);
    while (true) {
        MasterPasswordDialog dlg;
        dlg.setSelectedLanguage(language);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        language = dlg.selectedLanguage();
        store.setLanguage(language);
        {
            QSettings ini(store.iniPath(), QSettings::IniFormat);
            ini.beginGroup(QStringLiteral("ui"));
            ini.setValue(QStringLiteral("language"), language);
            ini.endGroup();
            ini.sync();
        }
        if (dlg.changePasswordRequested()) {
            QString err;
            store.setMasterPassword(dlg.changeOldPassword());
            if (!store.validateMasterPassword(err)) {
                const QString msg = tr3(language,
                                        QStringLiteral("Password maestro actual incorrecto.\n%1").arg(err),
                                        QStringLiteral("Current master password is invalid.\n%1").arg(err),
                                        QStringLiteral("当前主密码错误。\n%1").arg(err));
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            if (!store.rotateMasterPassword(dlg.changeOldPassword(), dlg.changeNewPassword(), err)) {
                const QString msg = tr3(language,
                                        QStringLiteral("No se pudo cambiar el password maestro.\n%1").arg(err),
                                        QStringLiteral("Could not change master password.\n%1").arg(err),
                                        QStringLiteral("无法修改主密码。\n%1").arg(err));
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            masterPassword = dlg.changeNewPassword();
            store.setMasterPassword(masterPassword);
            if (!store.encryptStoredPasswords(err)) {
                const QString msg = tr3(language,
                                        QStringLiteral("No se pudieron migrar passwords guardados.\n%1").arg(err),
                                        QStringLiteral("Could not migrate stored passwords.\n%1").arg(err),
                                        QStringLiteral("无法迁移已存储密码。\n%1").arg(err));
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            break;
        } else {
            masterPassword = dlg.password();
            store.setMasterPassword(masterPassword);
            QString err;
            if (store.validateMasterPassword(err)) {
                if (!store.encryptStoredPasswords(err)) {
                    const QString msg = tr3(language,
                                            QStringLiteral("No se pudieron migrar passwords guardados.\n%1").arg(err),
                                            QStringLiteral("Could not migrate stored passwords.\n%1").arg(err),
                                            QStringLiteral("无法迁移已存储密码。\n%1").arg(err));
                    QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                    continue;
                }
                break;
            }
            const QString msg = tr3(language,
                                    QStringLiteral("Password maestro incorrecto.\n%1").arg(err),
                                    QStringLiteral("Invalid master password.\n%1").arg(err),
                                    QStringLiteral("主密码错误。\n%1").arg(err));
            QMessageBox::warning(
                nullptr,
                QStringLiteral("ZFSMgr"),
                msg);
        }
    }

    MainWindow w(masterPassword, language);
    w.show();
    return app.exec();
}
