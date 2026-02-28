#include "masterpassworddialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString tr3(const QString& lang, const QString& es, const QString& en, const QString& zh) {
    if (lang == QStringLiteral("en")) return en;
    if (lang == QStringLiteral("zh")) return zh;
    return es;
}
}

MasterPasswordDialog::MasterPasswordDialog(QWidget* parent)
    : QDialog(parent) {
    setModal(true);
    resize(520, 220);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItems({QStringLiteral("es"), QStringLiteral("en"), QStringLiteral("zh")});
    form->addRow(QStringLiteral("Idioma"), m_languageCombo);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Password"), m_passwordEdit);
    root->addLayout(form);

    m_authorLabel = new QLabel(this);
    root->addWidget(m_authorLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    m_cancelButton = buttons->button(QDialogButtonBox::Cancel);
    m_changePwdButton = new QPushButton(this);
    buttons->addButton(m_changePwdButton, QDialogButtonBox::ActionRole);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &QDialog::accept);
    connect(m_changePwdButton, &QPushButton::clicked, this, [this]() {
        openChangePasswordDialog();
    });
    connect(m_languageCombo, &QComboBox::currentTextChanged, this, [this](const QString& lang) {
        m_lang = lang.trimmed().toLower();
        retranslateUi();
    });

    retranslateUi();
    QTimer::singleShot(0, this, [this]() {
        m_passwordEdit->setFocus(Qt::OtherFocusReason);
        m_passwordEdit->selectAll();
    });
}

QString MasterPasswordDialog::password() const {
    return m_passwordEdit->text();
}

QString MasterPasswordDialog::selectedLanguage() const {
    return m_languageCombo ? m_languageCombo->currentText().trimmed().toLower() : QStringLiteral("es");
}

bool MasterPasswordDialog::changePasswordRequested() const {
    return m_changePwdRequested;
}

QString MasterPasswordDialog::changeOldPassword() const {
    return m_changeOldPwd;
}

QString MasterPasswordDialog::changeNewPassword() const {
    return m_changeNewPwd;
}

void MasterPasswordDialog::setSelectedLanguage(const QString& langCode) {
    if (!m_languageCombo) {
        return;
    }
    const QString lc = langCode.trimmed().toLower();
    const int idx = m_languageCombo->findText(lc);
    if (idx >= 0) {
        m_languageCombo->setCurrentIndex(idx);
    }
    m_lang = selectedLanguage();
    retranslateUi();
}

void MasterPasswordDialog::openChangePasswordDialog() {
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(460, 220);
    dlg.setWindowTitle(tr3(m_lang,
                           QStringLiteral("Cambiar password maestro"),
                           QStringLiteral("Change master password"),
                           QStringLiteral("修改主密码")));
    auto* root = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QLineEdit* oldPwd = new QLineEdit(&dlg);
    QLineEdit* newPwd = new QLineEdit(&dlg);
    QLineEdit* newPwd2 = new QLineEdit(&dlg);
    oldPwd->setEchoMode(QLineEdit::Password);
    newPwd->setEchoMode(QLineEdit::Password);
    newPwd2->setEchoMode(QLineEdit::Password);
    form->addRow(tr3(m_lang, QStringLiteral("Password actual"), QStringLiteral("Current password"), QStringLiteral("当前密码")), oldPwd);
    form->addRow(tr3(m_lang, QStringLiteral("Password nuevo"), QStringLiteral("New password"), QStringLiteral("新密码")), newPwd);
    form->addRow(tr3(m_lang, QStringLiteral("Repetir password"), QStringLiteral("Repeat password"), QStringLiteral("重复密码")), newPwd2);
    root->addLayout(form);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    root->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QString oldv = oldPwd->text();
    const QString newv = newPwd->text();
    const QString newv2 = newPwd2->text();
    if (newv.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(m_lang, QStringLiteral("El nuevo password no puede estar vacío."),
                                 QStringLiteral("New password cannot be empty."),
                                 QStringLiteral("新密码不能为空。")));
        return;
    }
    if (newv != newv2) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(m_lang, QStringLiteral("La confirmación no coincide."),
                                 QStringLiteral("Confirmation does not match."),
                                 QStringLiteral("两次输入不一致。")));
        return;
    }
    m_changeOldPwd = oldv;
    m_changeNewPwd = newv;
    m_changePwdRequested = true;
    accept();
}

void MasterPasswordDialog::retranslateUi() {
    const QString lang = selectedLanguage();
    setWindowTitle(QStringLiteral("ZFSMgr"));
    m_passwordEdit->setPlaceholderText(tr3(lang, QStringLiteral("Password maestro"), QStringLiteral("Master password"), QStringLiteral("主密码")));
    m_okButton->setText(tr3(lang, QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确定")));
    if (m_cancelButton) {
        m_cancelButton->setText(tr3(lang, QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")));
    }
    if (m_changePwdButton) {
        m_changePwdButton->setText(tr3(lang,
                                       QStringLiteral("Cambiar password maestro..."),
                                       QStringLiteral("Change master password..."),
                                       QStringLiteral("修改主密码...")));
    }
    if (m_authorLabel) {
        m_authorLabel->setText(tr3(lang,
                                   QStringLiteral("Autor: Eladio Linares  |  Licencia: GNU"),
                                   QStringLiteral("Author: Eladio Linares  |  License: GNU"),
                                   QStringLiteral("作者：Eladio Linares  |  许可证：GNU")));
    }
}
