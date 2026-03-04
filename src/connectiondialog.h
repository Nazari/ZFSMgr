#pragma once

#include "connectionstore.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;

class ConnectionDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(const QString& language = QStringLiteral("es"), QWidget* parent = nullptr);

    void setProfile(const ConnectionProfile& profile);
    ConnectionProfile profile() const;

private:
    void updateConnectionModeUi();
    void ensureDefaultPortForMode();
    void testConnection();
    bool testSshConnection(const ConnectionProfile& p, QString& detail) const;
    QString tr3(const QString& es, const QString& en, const QString& zh) const;

    QLineEdit* m_nameEdit{nullptr};
    QComboBox* m_connTypeCombo{nullptr};
    QComboBox* m_osTypeCombo{nullptr};
    QComboBox* m_transportCombo{nullptr};
    QLineEdit* m_hostEdit{nullptr};
    QLineEdit* m_portEdit{nullptr};
    QLineEdit* m_userEdit{nullptr};
    QLineEdit* m_passwordEdit{nullptr};
    QLineEdit* m_keyEdit{nullptr};
    QCheckBox* m_sudoCheck{nullptr};

    QString m_id;
    QString m_lastAutoPort;
    QString m_language{QStringLiteral("es")};
};
