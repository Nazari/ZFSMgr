#include "masterpassworddialog.h"
#include "i18nmanager.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QIcon>

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.1.0"
#endif

namespace {
QString trk(const QString& lang,
            const QString& key,
            const QString& es = QString(),
            const QString& en = QString(),
            const QString& zh = QString()) {
    return I18nManager::instance().translateKey(lang, key, es, en, zh);
}
}

MasterPasswordDialog::MasterPasswordDialog(QWidget* parent)
    : QDialog(parent) {
    setModal(true);
    resize(520, 220);
    setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));

    auto* root = new QVBoxLayout(this);
    m_iconLabel = new QLabel(this);
    m_iconLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_iconLabel->setPixmap(QPixmap(QStringLiteral(":/icons/ZFSMgr-512.png")).scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    root->addWidget(m_iconLabel);
    auto* form = new QFormLayout();

    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItems({QStringLiteral("es"), QStringLiteral("en"), QStringLiteral("zh")});
    form->addRow(trk(m_lang, QStringLiteral("t_idioma_009433"),
                     QStringLiteral("Idioma"),
                     QStringLiteral("Language"),
                     QStringLiteral("语言")),
                 m_languageCombo);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(trk(m_lang, QStringLiteral("t_password_8be3c9"),
                     QStringLiteral("Password"),
                     QStringLiteral("Password"),
                     QStringLiteral("密码")),
                 m_passwordEdit);
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
    dlg.setWindowTitle(trk(m_lang,
                           QStringLiteral("t_chg_master_001"),
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
    form->addRow(trk(m_lang, QStringLiteral("t_cur_pwd_lbl001"), QStringLiteral("Password actual"), QStringLiteral("Current password"), QStringLiteral("当前密码")), oldPwd);
    form->addRow(trk(m_lang, QStringLiteral("t_new_pwd_lbl001"), QStringLiteral("Password nuevo"), QStringLiteral("New password"), QStringLiteral("新密码")), newPwd);
    form->addRow(trk(m_lang, QStringLiteral("t_rep_pwd_lbl001"), QStringLiteral("Repetir password"), QStringLiteral("Repeat password"), QStringLiteral("重复密码")), newPwd2);
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
                             trk(m_lang, QStringLiteral("t_new_pwd_empty1"), QStringLiteral("El nuevo password no puede estar vacío."),
                                 QStringLiteral("New password cannot be empty."),
                                 QStringLiteral("新密码不能为空。")));
        return;
    }
    if (newv != newv2) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(m_lang, QStringLiteral("t_pwd_confirm01"), QStringLiteral("La confirmación no coincide."),
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
    const QString appVersion = QStringLiteral(ZFSMGR_APP_VERSION);
    setWindowTitle(QStringLiteral("ZFSMgr [%1]").arg(appVersion));
    m_passwordEdit->setPlaceholderText(trk(lang, QStringLiteral("t_password_m_07c917"),
                                           QStringLiteral("Password maestro"),
                                           QStringLiteral("Master password"),
                                           QStringLiteral("主密码")));
    m_okButton->setText(trk(lang, QStringLiteral("t_aceptar_8f9f73"),
                            QStringLiteral("Aceptar"),
                            QStringLiteral("Accept"),
                            QStringLiteral("确定")));
    if (m_cancelButton) {
        m_cancelButton->setText(trk(lang, QStringLiteral("t_cancelar_c111e0"),
                                    QStringLiteral("Cancelar"),
                                    QStringLiteral("Cancel"),
                                    QStringLiteral("取消")));
    }
    if (m_changePwdButton) {
        m_changePwdButton->setText(trk(lang,
                                       QStringLiteral("t_cambiar_pa_52b0b6"),
                                       QStringLiteral("Cambiar password maestro..."),
                                       QStringLiteral("Change master password..."),
                                       QStringLiteral("修改主密码...")));
    }
    if (m_authorLabel) {
        const QString footer = trk(lang,
                                   QStringLiteral("t_autor_elad_c26aa2"),
                                   QStringLiteral("Autor: Eladio Linares  |  Licencia: GNU"),
                                   QStringLiteral("Author: Eladio Linares  |  License: GNU"),
                                   QStringLiteral("作者：Eladio Linares  |  许可证：GNU"));
        m_authorLabel->setText(QStringLiteral("%1  |  %2")
                                   .arg(footer,
                                        trk(lang,
                                            QStringLiteral("t_version_lbl001"),
                                            QStringLiteral("Versión: %1"),
                                            QStringLiteral("Version: %1"),
                                            QStringLiteral("版本：%1"))
                                            .arg(appVersion)));
    }
}
