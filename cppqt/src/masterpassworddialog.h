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

private:
    void retranslateUi();
    QLineEdit* m_passwordEdit{nullptr};
    QComboBox* m_languageCombo{nullptr};
    QPushButton* m_okButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
    QLabel* m_authorLabel{nullptr};
    QString m_lang{QStringLiteral("es")};
};
