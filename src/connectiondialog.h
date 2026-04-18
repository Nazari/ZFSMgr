#pragma once

#include "connectionstore.h"

#include <QDialog>

class QCheckBox;
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
    bool detectSshPlatform(const ConnectionProfile& p,
                           QString& osTypeOut,
                           QString& flavorOut,
                           QString& detailOut) const;
    bool testPsrpConnection(const ConnectionProfile& p, QString& detail) const;
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
    QWidget* m_privilegesRow{nullptr};
    QCheckBox* m_sudoCheck{nullptr};

    QString m_id;
    QString m_lastAutoPort;
    QString m_language{QStringLiteral("es")};
    QString m_detectedOsType;
    QString m_detectedOsFlavor;
};
