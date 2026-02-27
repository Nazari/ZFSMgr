#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPushButton;

class MasterPasswordDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MasterPasswordDialog(QWidget* parent = nullptr);

    QString password() const;

private:
    QLineEdit* m_passwordEdit{nullptr};
    QComboBox* m_languageCombo{nullptr};
    QPushButton* m_okButton{nullptr};
};

