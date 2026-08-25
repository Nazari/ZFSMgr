#include "connectionstore.h"
#include "i18nmanager.h"
#include "masterpassworddialog.h"
#include "mainwindow.h"

#include "cli/secretinput.h"
#include "mainwindow_helpers.h"

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
#include <QJsonObject>
#include <QDir>
#include <QStyleFactory>
#include <QStyleOption>
#include <QThread>
#include <QThreadPool>
#include <QToolTip>
#include <QProcessEnvironment>
#include <QSysInfo>

namespace {
QString trk(const QString& lang,
            const QString& key,
            const QString& es = QString(),
            const QString& en = QString()) {
    return I18nManager::instance().translateKey(lang, key, es, en);
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
        // El borde se dibuja, que para eso se calcula su color.
        //
        // Estaba `setPen(Qt::NoPen)`, así que `border` —con sus dos tonos, activo e
        // inactivo— se computaba y se tiraba: una casilla DESMARCADA quedaba en un
        // cuadrado blanco sin contorno, invisible sobre un fondo claro. Es código de
        // macOS y aquí no hay pantalla para verlo; el arreglo es lo que el propio código
        // decía querer.
        painter->setPen(QPen(border, 1.0));
        painter->setBrush(fill);
        painter->drawRect(rect.adjusted(0, 0, -1, -1));

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

namespace {

QtMessageHandler g_previousMessageHandler = nullptr;

// Trampa para avisos de Qt, apagada salvo que se pida.
//
// Existe porque un aviso como «QAbstractItemModel::endRemoveRows: Invalid index»
// dice QUÉ está mal pero no QUIÉN lo hizo, y el modelo de un QTreeWidget lo tocan
// docenas de sitios. Con ZFSMGR_TRAP_WARNING=<texto> el proceso aborta en cuanto
// aparece ese texto, y el volcado trae la pila del culpable. Mismo método con el que
// se cazó la referencia colgante del cierre.
//
// Sin la variable, no cambia absolutamente nada: se delega en el manejador anterior.
void zfsmgrMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    if (g_previousMessageHandler) {
        g_previousMessageHandler(type, context, msg);
    } else {
        fprintf(stderr, "%s\n", qPrintable(msg));
    }
    static const QByteArray trap = qgetenv("ZFSMGR_TRAP_WARNING");
    if (trap.isEmpty()) {
        return;
    }
    if (!msg.contains(QString::fromLocal8Bit(trap))) {
        return;
    }
    fprintf(stderr,
            "[zfsmgr] ZFSMGR_TRAP_WARNING coincide (\"%s\"): abortando para dejar volcado\n",
            trap.constData());
    fflush(stderr);
    abort();
}

}  // namespace

int main(int argc, char* argv[]) {
    Q_INIT_RESOURCE(resources);
    g_previousMessageHandler = qInstallMessageHandler(zfsmgrMessageHandler);
#ifdef Q_OS_MAC
    // Keep macOS visuals consistent by routing dialogs and widgets through Qt's Fusion style.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
#endif
    QApplication app(argc, argv);
    {
        const int desiredWorkers = qMin(16, qMax(4, QThread::idealThreadCount()));
        QThreadPool* pool = QThreadPool::globalInstance();
        pool->setMaxThreadCount(qMax(pool->maxThreadCount(), desiredWorkers));
    }
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
    QApplication::setOrganizationName(QStringLiteral("ZFSMgr"));
    QApplication::setApplicationName(QStringLiteral("ZFSMgr"));
    {
        QFont uiFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        const int basePointSize = (app.font().pointSize() > 0) ? app.font().pointSize() : uiFont.pointSize();
        uiFont.setPointSize(qMax(6, basePointSize - 1));
        app.setFont(uiFont);
        QToolTip::setFont(uiFont);
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

    // --password-fd <n>: la contraseña maestra entra por un descriptor, igual que en el
    // intérprete. NUNCA por argumento ni por variable de entorno: las dos cosas salen en
    // `ps` para cualquier usuario de la máquina.
    //
    // Sirve para arrancar sin ventana de por medio —una prueba, un arranque de sesión— y
    // para gestores de secretos:
    //
    //     zfsmgr-gui --password-fd 3  3< <(pass show zfsmgr)
    //
    // Si el descriptor no se puede leer o la contraseña no vale, NO se sale: se cae al
    // diálogo de siempre. Un arranque desatendido que falla y deja la aplicación cerrada
    // sin decir nada es peor que preguntar.
    QString masterPassword;
    int masterPasswordFd = -1;
    {
        const QStringList args = QCoreApplication::arguments();
        for (int i = 1; i < args.size(); ++i) {
            if (args.at(i) == QStringLiteral("--password-fd") && i + 1 < args.size()) {
                bool ok = false;
                const int fd = args.at(i + 1).toInt(&ok);
                if (ok && fd >= 0) {
                    masterPasswordFd = fd;
                }
                ++i;
            }
        }
    }
    QString language = QStringLiteral("es");
    ConnectionStore store(QStringLiteral("ZFSMgr"));
    {
        const QJsonObject root = store.loadConfigJson();
        const QJsonObject appObj = root.value(QStringLiteral("app")).toObject();
        const QJsonObject uiObj = root.value(QStringLiteral("ui")).toObject();
        auto validLang = [](const QString& v) -> bool {
            return v == QStringLiteral("es") || v == QStringLiteral("en");
        };
        const QString appLang = appObj.value(QStringLiteral("language")).toString().trimmed().toLower();
        const QString uiLang = uiObj.value(QStringLiteral("language")).toString().trimmed().toLower();
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
        const bool hasConfig = QFileInfo::exists(store.configPath());
        firstRunCreateIni = !hasConfig;
        LoadResult lr = store.loadConnections();
        bool hasLocalConnWithCreds = false;
        for (const ConnectionProfile& cp : lr.profiles) {
            const bool isLocal = cp.id.trimmed().compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0
                || cp.connType.trimmed().compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0;
            if (!isLocal) {
                continue;
            }
            if (!cp.username.trimmed().isEmpty() && !cp.password.trimmed().isEmpty()) {
                hasLocalConnWithCreds = true;
                break;
            }
        }
        requireLocalSudoAtStartup = !hasLocalConnWithCreds;
    }
    // Con descriptor y sin nada que crear, se prueba antes de enseñar el diálogo.
    bool unattendedAccepted = false;
    if (masterPasswordFd >= 0 && !firstRunCreateIni) {
        std::string secret;
        std::string readErr;
        if (zfsmgr::cli::leerSecretoDeDescriptor(masterPasswordFd, secret, readErr)) {
            masterPassword = QString::fromStdString(secret);
            store.setMasterPassword(masterPassword);
            QString err;
            if (store.validateMasterPassword(err) && store.encryptStoredPasswords(err)) {
                unattendedAccepted = true;
            } else {
                masterPassword.clear();
                store.setMasterPassword(QString());
            }
        }
        std::fill(secret.begin(), secret.end(), '\0');
    }
    while (!unattendedAccepted) {
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
            if (QFileInfo::exists(store.configPath()) && !QFile::remove(store.configPath())) {
                removeErr = trk(language,
                                QStringLiteral("t_reset_ini_err001"),
                                QStringLiteral("No se pudo borrar config.json."),
                                QStringLiteral("config.json could not be deleted."));
            }
            const QDir cfgDir(store.configDir());
            const QStringList connFiles = cfgDir.entryList({QStringLiteral("conn*.ini")}, QDir::Files);
            for (const QString& f : connFiles) {
                const QString p = cfgDir.filePath(f);
                if (!QFile::remove(p) && removeErr.isEmpty()) {
                    removeErr = trk(language,
                                    QStringLiteral("t_reset_ini_err_conn"),
                                    QStringLiteral("No se pudo borrar %1."),
                                    QStringLiteral("%1 could not be deleted.")).arg(f);
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
            QString jsonErr;
            QJsonObject root = store.loadConfigJson(&jsonErr);
            QJsonObject appObj = root.value(QStringLiteral("app")).toObject();
            appObj.insert(QStringLiteral("language"), language);
            root.insert(QStringLiteral("app"), appObj);
            QJsonObject uiObj = root.value(QStringLiteral("ui")).toObject();
            uiObj.insert(QStringLiteral("language"), language);
            root.insert(QStringLiteral("ui"), uiObj);
            store.saveConfigJson(root, &jsonErr);
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
                        QStringLiteral("A sudo user and password are required.")));
                return false;
            }
            // Comprobar la contraseña ANTES de guardarla. Guardar una equivocada dejaba
            // la conexión Local inservible sin arreglo posible desde la aplicación:
            // aquí solo se preguntaba con el campo vacío, y Local no se puede editar.
            QString sudoDetail;
            const mwhelpers::SudoCheck sudoCheck =
                mwhelpers::checkLocalSudoPassword(localPassword, &sudoDetail);
            if (sudoCheck == mwhelpers::SudoCheck::WrongPassword) {
                QMessageBox::warning(
                    nullptr,
                    QStringLiteral("ZFSMgr"),
                    trk(language,
                        QStringLiteral("t_local_sudo_bad1"),
                        QStringLiteral("La contraseña de sudo local no es válida.\n%1\n\nVuelva a introducirla."),
                        QStringLiteral("The local sudo password is not valid.\n%1\n\nEnter it again.")).arg(sudoDetail));
                return false;
            }
            // No se pudo comprobar: se avisa y se sigue. Bloquear aquí dejaría al
            // usuario sin poder arrancar por un fallo que no es suyo, que es
            // exactamente el encierro que esta comprobación viene a evitar.
            if (sudoCheck == mwhelpers::SudoCheck::CouldNotCheck) {
                QMessageBox::warning(
                    nullptr,
                    QStringLiteral("ZFSMgr"),
                    trk(language,
                        QStringLiteral("t_local_sudo_unchecked1"),
                        QStringLiteral("No se pudo comprobar la contraseña de sudo local:\n%1\n\n"
                                       "Se guarda de todos modos. Si las operaciones locales fallan, "
                                       "use «Cambiar credenciales sudo local…» en el menú de la conexión Local."),
                        QStringLiteral("The local sudo password could not be checked:\n%1\n\n"
                                       "It is saved anyway. If local operations fail, use "
                                       "\"Change local sudo credentials…\" in the Connections menu.")).arg(sudoDetail));
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
#elif defined(Q_OS_FREEBSD)
            local.osType = QStringLiteral("FreeBSD");
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
                        QStringLiteral("No se pudo crear la conexión Local en config.json.\n%1"),
                        QStringLiteral("The Local connection could not be created in config.json.\n%1")).arg(localErr));
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
                                        QStringLiteral("Password maestro actual incorrecto.\n%1")).arg(err);
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            if (!store.rotateMasterPassword(dlg.changeOldPassword(), dlg.changeNewPassword(), err)) {
                const QString msg = trk(language, QStringLiteral("t_no_se_pudo_204a1d"),
                                        QStringLiteral("No se pudo cambiar el password maestro.\n%1")).arg(err);
                QMessageBox::warning(nullptr, QStringLiteral("ZFSMgr"), msg);
                continue;
            }
            masterPassword = dlg.changeNewPassword();
            store.setMasterPassword(masterPassword);
            if (!store.encryptStoredPasswords(err)) {
                const QString msg = trk(language, QStringLiteral("t_no_se_pudi_17885e"),
                                        QStringLiteral("No se pudieron migrar passwords guardados.\n%1")).arg(err);
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
                            QStringLiteral("The new password cannot be empty.")));
                    continue;
                }
                if (masterPassword != confirm) {
                    QMessageBox::warning(
                        nullptr,
                        QStringLiteral("ZFSMgr"),
                        trk(language,
                            QStringLiteral("t_pwd_confirm01"),
                            QStringLiteral("La confirmación no coincide."),
                            QStringLiteral("The confirmation does not match.")));
                    continue;
                }
            }
            store.setMasterPassword(masterPassword);
            QString err;
            if (store.validateMasterPassword(err)) {
                if (!store.encryptStoredPasswords(err)) {
                    const QString msg = trk(language, QStringLiteral("t_no_se_pudi_17885e"),
                                            QStringLiteral("No se pudieron migrar passwords guardados.\n%1")).arg(err);
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
                                    QStringLiteral("Password maestro incorrecto.\n%1")).arg(err);
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
