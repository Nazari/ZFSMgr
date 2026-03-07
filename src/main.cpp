#include "connectionstore.h"
#include "i18nmanager.h"
#include "masterpassworddialog.h"
#include "mainwindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QFile>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>

namespace {
QString trk(const QString& lang,
            const QString& key,
            const QString& es = QString(),
            const QString& en = QString(),
            const QString& zh = QString()) {
    return I18nManager::instance().translateKey(lang, key, es, en, zh);
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
        auto validLang = [](const QString& v) -> bool {
            return v == QStringLiteral("es") || v == QStringLiteral("en") || v == QStringLiteral("zh");
        };
        ini.beginGroup(QStringLiteral("app"));
        const QString appLang = ini.value(QStringLiteral("language"), QString()).toString().trimmed().toLower();
        ini.endGroup();
        ini.beginGroup(QStringLiteral("ui"));
        const QString uiLang = ini.value(QStringLiteral("language"), QString()).toString().trimmed().toLower();
        ini.endGroup();
        if (validLang(appLang)) {
            language = appLang;
        } else if (validLang(uiLang)) {
            language = uiLang;
        }
    }
    store.setLanguage(language);
    bool firstRunCreateIni = !QFileInfo::exists(store.iniPath());
    while (true) {
        MasterPasswordDialog dlg;
        dlg.setSelectedLanguage(language);
        dlg.setFirstRunCreationMode(firstRunCreateIni);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        language = dlg.selectedLanguage();
        store.setLanguage(language);

        if (dlg.resetIniRequested()) {
            QString removeErr;
            if (QFileInfo::exists(store.iniPath()) && !QFile::remove(store.iniPath())) {
                removeErr = trk(language,
                                QStringLiteral("t_reset_ini_err001"),
                                QStringLiteral("No se pudo borrar config.ini."),
                                QStringLiteral("Could not delete config.ini."),
                                QStringLiteral("无法删除 config.ini。"));
            }
            if (!removeErr.isEmpty()) {
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), removeErr);
                continue;
            }
            firstRunCreateIni = true;
            masterPassword.clear();
            continue;
        }

        {
            QSettings ini(store.iniPath(), QSettings::IniFormat);
            ini.beginGroup(QStringLiteral("app"));
            ini.setValue(QStringLiteral("language"), language);
            ini.endGroup();
            ini.beginGroup(QStringLiteral("ui"));
            ini.setValue(QStringLiteral("language"), language);
            ini.endGroup();
            ini.sync();
        }
        if (dlg.changePasswordRequested()) {
            QString err;
            store.setMasterPassword(dlg.changeOldPassword());
            if (!store.validateMasterPassword(err)) {
                const QString msg = trk(language, QStringLiteral("t_password_m_397f2a"),
                                        QStringLiteral("Password maestro actual incorrecto.\n%1"),
                                        QStringLiteral("Current master password is invalid.\n%1"),
                                        QStringLiteral("当前主密码错误。\n%1")).arg(err);
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            if (!store.rotateMasterPassword(dlg.changeOldPassword(), dlg.changeNewPassword(), err)) {
                const QString msg = trk(language, QStringLiteral("t_no_se_pudo_204a1d"),
                                        QStringLiteral("No se pudo cambiar el password maestro.\n%1"),
                                        QStringLiteral("Could not change master password.\n%1"),
                                        QStringLiteral("无法修改主密码。\n%1")).arg(err);
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            masterPassword = dlg.changeNewPassword();
            store.setMasterPassword(masterPassword);
            if (!store.encryptStoredPasswords(err)) {
                const QString msg = trk(language, QStringLiteral("t_no_se_pudi_17885e"),
                                        QStringLiteral("No se pudieron migrar passwords guardados.\n%1"),
                                        QStringLiteral("Could not migrate stored passwords.\n%1"),
                                        QStringLiteral("无法迁移已存储密码。\n%1")).arg(err);
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            firstRunCreateIni = false;
            break;
        } else {
            masterPassword = dlg.password();
            if (firstRunCreateIni) {
                const QString confirm = dlg.confirmPassword();
                if (masterPassword.isEmpty()) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("ZFSMgr"),
                        trk(language,
                            QStringLiteral("t_new_pwd_empty1"),
                            QStringLiteral("El nuevo password no puede estar vacío."),
                            QStringLiteral("New password cannot be empty."),
                            QStringLiteral("新密码不能为空。")));
                    continue;
                }
                if (masterPassword != confirm) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("ZFSMgr"),
                        trk(language,
                            QStringLiteral("t_pwd_confirm01"),
                            QStringLiteral("La confirmación no coincide."),
                            QStringLiteral("Confirmation does not match."),
                            QStringLiteral("两次输入不一致。")));
                    continue;
                }
            }
            store.setMasterPassword(masterPassword);
            QString err;
            if (store.validateMasterPassword(err)) {
                if (!store.encryptStoredPasswords(err)) {
                    const QString msg = trk(language, QStringLiteral("t_no_se_pudi_17885e"),
                                            QStringLiteral("No se pudieron migrar passwords guardados.\n%1"),
                                            QStringLiteral("Could not migrate stored passwords.\n%1"),
                                            QStringLiteral("无法迁移已存储密码。\n%1")).arg(err);
                    QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                    continue;
                }
                firstRunCreateIni = false;
                break;
            }
            const QString msg = trk(language, QStringLiteral("t_password_m_07a72a"),
                                    QStringLiteral("Password maestro incorrecto.\n%1"),
                                    QStringLiteral("Invalid master password.\n%1"),
                                    QStringLiteral("主密码错误。\n%1")).arg(err);
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
