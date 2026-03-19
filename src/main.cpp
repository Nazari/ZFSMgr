#include "connectionstore.h"
#include "i18nmanager.h"
#include "masterpassworddialog.h"
#include "mainwindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QFile>
#include <QFontDatabase>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QProcess>
#include <QSettings>
#include <QDir>
#include <QStyleFactory>
#include <QStyleOption>
#include <QToolTip>
#include <QProcessEnvironment>
#include <QSysInfo>

namespace {
QString trk(const QString& lang,
            const QString& key,
            const QString& es = QString(),
            const QString& en = QString(),
            const QString& zh = QString()) {
    return I18nManager::instance().translateKey(lang, key, es, en, zh);
}

QString currentLocalMachineUid() {
#if defined(Q_OS_MACOS)
    QProcess proc;
    proc.start(QStringLiteral("sh"),
               QStringList{QStringLiteral("-lc"),
                           QStringLiteral("ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F\\\" '/IOPlatformUUID/{print $(NF-1); exit}'")});
    if (proc.waitForFinished(3000)) {
        const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) {
            return out;
        }
    }
#endif
    return QString::fromLatin1(QSysInfo::machineUniqueId().toHex()).trimmed();
}

QPalette createMacFusionDarkPalette() {
    QPalette palette;
    const QColor window(32, 34, 37);
    const QColor base(24, 26, 29);
    const QColor alternateBase(36, 39, 43);
    const QColor text(232, 234, 237);
    const QColor dimText(156, 163, 175);
    const QColor button(45, 48, 53);
    const QColor highlight(64, 110, 181);
    const QColor highlightedText(255, 255, 255);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternateBase);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, highlight.lighter(120));
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, highlightedText);
    palette.setColor(QPalette::Mid, QColor(79, 85, 94));
    palette.setColor(QPalette::Midlight, QColor(97, 103, 112));
    palette.setColor(QPalette::Shadow, QColor(12, 13, 15));
    palette.setColor(QPalette::PlaceholderText, dimText);

    palette.setColor(QPalette::Disabled, QPalette::WindowText, dimText);
    palette.setColor(QPalette::Disabled, QPalette::Text, dimText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, dimText);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(70, 74, 80));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(205, 209, 214));
    palette.setColor(QPalette::Disabled, QPalette::Base, QColor(28, 30, 33));
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor(39, 41, 45));

    return palette;
}

class MacFusionProxyStyle final : public QProxyStyle {
public:
    explicit MacFusionProxyStyle(QStyle* baseStyle)
        : QProxyStyle(baseStyle) {}

    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widget = nullptr) const override {
        if ((element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck)
            && option && painter) {
            drawTickIndicator(option, painter);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

private:
    static void drawTickIndicator(const QStyleOption* option, QPainter* painter) {
        const QRect rect = option->rect.adjusted(1, 1, -1, -1);
        if (!rect.isValid()) {
            return;
        }

        const bool enabled = option->state & State_Enabled;
        const bool checked = option->state & State_On;
        const bool mixed = option->state & State_NoChange;

        const QColor border = enabled ? QColor(47, 95, 140) : QColor(139, 154, 168);
        const QColor fill = checked || mixed ? QColor(53, 112, 168) : QColor(255, 255, 255);
        const QColor tick = QColor(255, 255, 255);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRect(rect);

        if (checked || mixed) {
            QPen tickPen(tick, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter->setPen(tickPen);
            if (mixed) {
                const int y = rect.center().y();
                painter->drawLine(rect.left() + 3, y, rect.right() - 3, y);
            } else {
                QPainterPath path;
                path.moveTo(rect.left() + rect.width() * 0.22, rect.top() + rect.height() * 0.55);
                path.lineTo(rect.left() + rect.width() * 0.44, rect.top() + rect.height() * 0.76);
                path.lineTo(rect.left() + rect.width() * 0.78, rect.top() + rect.height() * 0.28);
                painter->drawPath(path);
            }
        }
        painter->restore();
    }
};
}

int main(int argc, char* argv[]) {
#ifdef Q_OS_MAC
    // Keep macOS visuals consistent by routing dialogs and widgets through Qt's Fusion style.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
#endif
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
    QApplication::setOrganizationName(QStringLiteral("ZFSMgr"));
    QApplication::setApplicationName(QStringLiteral("ZFSMgr"));
    {
        QFont tooltipFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        tooltipFont.setPointSize(app.font().pointSize());
        QToolTip::setFont(tooltipFont);
    }
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(new MacFusionProxyStyle(fusion));
    }
    app.setPalette(createMacFusionDarkPalette());
#endif

    QStringList missingI18n;
    if (!I18nManager::instance().areJsonCatalogsAvailable(&missingI18n)) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("ZFSMgr"),
            QStringLiteral("No se encontraron todos los ficheros JSON de idioma (%1).\n"
                           "Se utilizará español como fallback.")
                .arg(missingI18n.join(QStringLiteral(", "))));
    }

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
    if (!missingI18n.isEmpty()) {
        language = QStringLiteral("es");
    }
    store.setLanguage(language);
    bool firstRunCreateIni = false;
    bool requireLocalSudoAtStartup = false;
    {
        const bool hasConfig = QFileInfo::exists(store.iniPath());
        const QDir cfgDir(store.configDir());
        firstRunCreateIni = !hasConfig;
        requireLocalSudoAtStartup = !QFileInfo::exists(cfgDir.filePath(QStringLiteral("connLocal.ini")));
    }
    while (true) {
        MasterPasswordDialog dlg;
        dlg.setSelectedLanguage(language);
        dlg.setFirstRunCreationMode(firstRunCreateIni);
        dlg.setRequestLocalSudoCredentials(requireLocalSudoAtStartup);
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
            const QDir cfgDir(store.configDir());
            const QStringList connFiles = cfgDir.entryList({QStringLiteral("conn*.ini")}, QDir::Files);
            for (const QString& f : connFiles) {
                const QString p = cfgDir.filePath(f);
                if (!QFile::remove(p) && removeErr.isEmpty()) {
                    removeErr = trk(language,
                                    QStringLiteral("t_reset_ini_err_conn"),
                                    QStringLiteral("No se pudo borrar %1.").arg(f),
                                    QStringLiteral("Could not delete %1.").arg(f),
                                    QStringLiteral("无法删除 %1。").arg(f));
                }
            }
            if (!removeErr.isEmpty()) {
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), removeErr);
                continue;
            }
            firstRunCreateIni = true;
            requireLocalSudoAtStartup = true;
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
        store.ensureAppDefaults();
        auto ensureLocalConnAtStartup = [&](const QString& localUser, const QString& localPassword) -> bool {
            if (!requireLocalSudoAtStartup) {
                return true;
            }
            if (localUser.isEmpty() || localPassword.isEmpty()) {
                QMessageBox::warning(
                    nullptr,
                    QStringLiteral("ZFSMgr"),
                    trk(language,
                        QStringLiteral("t_local_sudo_req1"),
                        QStringLiteral("Usuario y password sudo son obligatorios."),
                        QStringLiteral("Sudo user and password are required."),
                        QStringLiteral("必须提供 sudo 用户和密码。")));
                return false;
            }
            ConnectionProfile local;
            local.id = QStringLiteral("local");
            local.name = QStringLiteral("Local");
            local.machineUid = currentLocalMachineUid();
            local.connType = QStringLiteral("LOCAL");
            local.port = 0;
            local.host = QStringLiteral("localhost");
            local.sshAddressFamily = QStringLiteral("auto");
            local.username = localUser;
            local.password = localPassword;
#ifdef Q_OS_WIN
            local.osType = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
            local.osType = QStringLiteral("macOS");
#else
            local.osType = QStringLiteral("Linux");
#endif
            local.useSudo = !local.osType.contains(QStringLiteral("Windows"), Qt::CaseInsensitive);
            QString localErr;
            if (!store.upsertConnection(local, localErr)) {
                QMessageBox::warning(
                    nullptr,
                    QStringLiteral("ZFSMgr"),
                    trk(language,
                        QStringLiteral("t_local_conn_create_err001"),
                        QStringLiteral("No se pudo crear connLocal.ini.\n%1"),
                        QStringLiteral("Could not create connLocal.ini.\n%1"),
                        QStringLiteral("无法创建 connLocal.ini。\n%1")).arg(localErr));
                return false;
            }
            requireLocalSudoAtStartup = false;
            return true;
        };
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
            if (!ensureLocalConnAtStartup(dlg.localUsername(), dlg.localPassword())) {
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
                if (!ensureLocalConnAtStartup(dlg.localUsername(), dlg.localPassword())) {
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
