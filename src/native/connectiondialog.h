#pragma once

#include "connectionstore.h"

#include <QDialog>

// Declaración adelantada a propósito: mainwindow_helpers.h incluye este fichero,
// así que incluirlo aquí cerraría el ciclo. La definición se necesita solo en el .cpp.
namespace mwhelpers { enum class SudoCheck; }

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

class ConnectionDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(const QString& language = QStringLiteral("es"), QWidget* parent = nullptr);

    void setProfile(const ConnectionProfile& profile);
    ConnectionProfile profile() const;

private:
    void updateConnectionModeUi();
    void updateDetectedOsLabel();
    void ensureDefaultPortForMode();
    void testConnection();
    void acceptDialog();
    bool runSshProbe(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs, QString& out, QString& err) const;
    bool testSshConnection(const ConnectionProfile& p, QString& detail) const;
    // Comprueba la contraseña de sudo en la máquina remota, con la MISMA orden que se
    // usará después. Sin esto se guardaba sin más: SSH entra por clave, así que una
    // contraseña de sudo equivocada no rompe nada visible al aceptar y solo aparece
    // mucho después, en forma de "el agente no está instalado" en una máquina donde
    // sí lo está.
    mwhelpers::SudoCheck checkRemoteSudoPassword(const ConnectionProfile& p, QString& detailOut) const;
    bool detectSshPlatform(const ConnectionProfile& p,
                           QString& osTypeOut,
                           QString& flavorOut,
                           QString& detailOut) const;
    void browsePrivateKey();
    QString trk(const QString& key,
                const QString& es = QString(),
                const QString& en = QString(),
                const QString& zh = QString()) const;

    QLineEdit* m_nameEdit{nullptr};
    QComboBox* m_connTypeCombo{nullptr};
    QLabel* m_osInfoLabel{nullptr};
    QComboBox* m_sshFamilyCombo{nullptr};
    QLineEdit* m_hostEdit{nullptr};
    QLineEdit* m_portEdit{nullptr};
    QLineEdit* m_userEdit{nullptr};
    QLineEdit* m_passwordEdit{nullptr};
    QLineEdit* m_keyEdit{nullptr};
    QPushButton* m_keyBrowseBtn{nullptr};
    QString m_id;
    QString m_lastAutoPort;
    QString m_language{QStringLiteral("es")};
    QString m_detectedOsType;
    QString m_detectedOsFlavor;
};
