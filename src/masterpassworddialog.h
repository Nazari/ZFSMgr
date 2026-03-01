#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;

class MasterPasswordDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MasterPasswordDialog(QWidget* parent = nullptr);

    QString password() const;
    QString selectedLanguage() const;
    void setSelectedLanguage(const QString& langCode);
    bool changePasswordRequested() const;
    QString changeOldPassword() const;
    QString changeNewPassword() const;

private:
    void openChangePasswordDialog();
    void retranslateUi();
    QLineEdit* m_passwordEdit{nullptr};
    QComboBox* m_languageCombo{nullptr};
    QLabel* m_iconLabel{nullptr};
    QPushButton* m_okButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
    QPushButton* m_changePwdButton{nullptr};
    QLabel* m_authorLabel{nullptr};
    QString m_lang{QStringLiteral("es")};
    bool m_changePwdRequested{false};
    QString m_changeOldPwd;
    QString m_changeNewPwd;
};
