#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QFormLayout;

class MasterPasswordDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MasterPasswordDialog(QWidget* parent = nullptr);

    QString password() const;
    QString confirmPassword() const;
    QString selectedLanguage() const;
    void setSelectedLanguage(const QString& langCode);
    void setFirstRunCreationMode(bool enabled);
    bool resetIniRequested() const;
    bool changePasswordRequested() const;
    QString changeOldPassword() const;
    QString changeNewPassword() const;
    void setRequestLocalSudoCredentials(bool enabled);
    QString localUsername() const;
    QString localPassword() const;

private:
    void openChangePasswordDialog();
    void retranslateUi();
    void refreshLanguageComboTexts();
    int languageIndexForCode(const QString& langCode) const;
    QLineEdit* m_passwordEdit{nullptr};
    QLineEdit* m_passwordConfirmEdit{nullptr};
    QComboBox* m_languageCombo{nullptr};
    QLabel* m_iconLabel{nullptr};
    QLabel* m_creationInfoLabel{nullptr};
    QLabel* m_languageLabel{nullptr};
    QLabel* m_passwordLabel{nullptr};
    QLabel* m_passwordConfirmLabel{nullptr};
    QLabel* m_localUserLabel{nullptr};
    QLabel* m_localPasswordLabel{nullptr};
    QPushButton* m_okButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
    QPushButton* m_changePwdButton{nullptr};
    QPushButton* m_resetIniButton{nullptr};
    QLabel* m_authorLabel{nullptr};
    QFormLayout* m_formLayout{nullptr};
    QLineEdit* m_localUserEdit{nullptr};
    QLineEdit* m_localPasswordEdit{nullptr};
    QString m_lang{QStringLiteral("es")};
    bool m_firstRunCreationMode{false};
    bool m_requestLocalSudoCredentials{false};
    bool m_resetIniRequested{false};
    bool m_changePwdRequested{false};
    QString m_changeOldPwd;
    QString m_changeNewPwd;
};
