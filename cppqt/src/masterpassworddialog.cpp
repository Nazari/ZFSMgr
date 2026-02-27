#include "masterpassworddialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

MasterPasswordDialog::MasterPasswordDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("ZFSMgr"));
    setModal(true);
    resize(520, 220);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItems({QStringLiteral("es"), QStringLiteral("en"), QStringLiteral("zh")});
    form->addRow(QStringLiteral("Idioma"), m_languageCombo);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QStringLiteral("Password maestro"));
    form->addRow(QStringLiteral("Password"), m_passwordEdit);
    root->addLayout(form);

    auto* author = new QLabel(QStringLiteral("Autor: Eladio Linares  |  Licencia: GNU"), this);
    root->addWidget(author);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    m_okButton->setText(QStringLiteral("Aceptar"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancelar"));
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &QDialog::accept);
    QTimer::singleShot(0, this, [this]() {
        m_passwordEdit->setFocus(Qt::OtherFocusReason);
        m_passwordEdit->selectAll();
    });
}

QString MasterPasswordDialog::password() const {
    return m_passwordEdit->text();
}
