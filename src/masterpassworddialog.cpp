#include "masterpassworddialog.h"
#include "i18nmanager.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QIcon>

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.9.7"
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
    resize(420, 340);
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
    m_passwordConfirmEdit = new QLineEdit(this);
    m_passwordConfirmEdit->setEchoMode(QLineEdit::Password);
    form->addRow(trk(m_lang, QStringLiteral("t_repeat_pwd_001"),
                     QStringLiteral("Repetir password"),
                     QStringLiteral("Repeat password"),
                     QStringLiteral("重复密码")),
                 m_passwordConfirmEdit);
    root->addLayout(form);

    m_creationInfoLabel = new QLabel(this);
    m_creationInfoLabel->setWordWrap(true);
    m_creationInfoLabel->setStyleSheet(QStringLiteral("QLabel { color: #0b3f6f; font-weight: 600; }"));
    root->addWidget(m_creationInfoLabel);

    m_authorLabel = new QLabel(this);
    root->addWidget(m_authorLabel);

    m_okButton = new QPushButton(this);
    m_cancelButton = new QPushButton(this);
    m_changePwdButton = new QPushButton(this);
    m_resetIniButton = new QPushButton(this);

    auto* actionsCol = new QVBoxLayout();
    actionsCol->setSpacing(6);
    actionsCol->addWidget(m_changePwdButton);
    actionsCol->addWidget(m_resetIniButton);
    root->addLayout(actionsCol);

    auto* decisionRow = new QHBoxLayout();
    decisionRow->setSpacing(8);
    decisionRow->addStretch(1);
    decisionRow->addWidget(m_cancelButton);
    decisionRow->addWidget(m_okButton);
    root->addLayout(decisionRow);

    connect(m_okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &QDialog::accept);
    connect(m_passwordConfirmEdit, &QLineEdit::returnPressed, this, &QDialog::accept);
    connect(m_changePwdButton, &QPushButton::clicked, this, [this]() {
        openChangePasswordDialog();
    });
    connect(m_resetIniButton, &QPushButton::clicked, this, [this]() {
        const auto ans = QMessageBox::question(
            this,
            QStringLiteral("ZFSMgr"),
            trk(m_lang,
                QStringLiteral("t_reset_ini_q_001"),
                QStringLiteral("Esto borrará config.ini y todas las conexiones guardadas.\n¿Desea continuar?"),
                QStringLiteral("This will delete config.ini and all saved connections.\nDo you want to continue?"),
                QStringLiteral("这将删除 config.ini 及所有已保存连接。\n是否继续？")),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ans != QMessageBox::Yes) {
            return;
        }
        m_resetIniRequested = true;
        accept();
    });
    connect(m_languageCombo, &QComboBox::currentTextChanged, this, [this](const QString& lang) {
        m_lang = lang.trimmed().toLower();
        retranslateUi();
    });

    retranslateUi();
    if (layout()) {
        layout()->activate();
    }
    const QSize lockedSize = sizeHint().expandedTo(QSize(420, 340));
    setFixedSize(lockedSize);
    QTimer::singleShot(0, this, [this]() {
        m_passwordEdit->setFocus(Qt::OtherFocusReason);
        m_passwordEdit->selectAll();
    });
}

QString MasterPasswordDialog::password() const {
    return m_passwordEdit->text();
}

QString MasterPasswordDialog::confirmPassword() const {
    return m_passwordConfirmEdit ? m_passwordConfirmEdit->text() : QString();
}

QString MasterPasswordDialog::selectedLanguage() const {
    return m_languageCombo ? m_languageCombo->currentText().trimmed().toLower() : QStringLiteral("es");
}

bool MasterPasswordDialog::resetIniRequested() const {
    return m_resetIniRequested;
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

void MasterPasswordDialog::setFirstRunCreationMode(bool enabled) {
    m_firstRunCreationMode = enabled;
    retranslateUi();
    QTimer::singleShot(0, this, [this]() {
        if (m_passwordEdit) {
            m_passwordEdit->setFocus(Qt::OtherFocusReason);
            m_passwordEdit->selectAll();
        }
    });
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
    if (m_passwordConfirmEdit) {
        m_passwordConfirmEdit->setPlaceholderText(trk(lang, QStringLiteral("t_repeat_pwd_001"),
                                                      QStringLiteral("Repetir password"),
                                                      QStringLiteral("Repeat password"),
                                                      QStringLiteral("重复密码")));
        m_passwordConfirmEdit->setVisible(m_firstRunCreationMode);
        m_passwordConfirmEdit->setEnabled(m_firstRunCreationMode);
        if (!m_firstRunCreationMode) {
            m_passwordConfirmEdit->clear();
        }
    }
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
                                       QStringLiteral("Cambiar Password"),
                                       QStringLiteral("Change Password"),
                                       QStringLiteral("修改密码")));
        m_changePwdButton->setVisible(!m_firstRunCreationMode);
        m_changePwdButton->setEnabled(!m_firstRunCreationMode);
    }
    if (m_resetIniButton) {
        m_resetIniButton->setText(trk(lang,
                                      QStringLiteral("t_reset_ini_btn001"),
                                      QStringLiteral("Borrar Config"),
                                      QStringLiteral("Delete Config"),
                                      QStringLiteral("删除配置")));
        m_resetIniButton->setVisible(!m_firstRunCreationMode);
        m_resetIniButton->setEnabled(!m_firstRunCreationMode);
    }
    if (m_creationInfoLabel) {
        if (m_firstRunCreationMode) {
            m_creationInfoLabel->setText(trk(lang,
                                             QStringLiteral("t_create_ini_001"),
                                             QStringLiteral("No existe config.ini. Se va a crear ahora.\nIntroduzca y confirme el password maestro."),
                                             QStringLiteral("config.ini does not exist. It will be created now.\nEnter and confirm the master password."),
                                             QStringLiteral("config.ini 不存在，将立即创建。\n请输入并确认主密码。")));
            m_creationInfoLabel->show();
        } else {
            m_creationInfoLabel->hide();
        }
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
    if (layout()) {
        layout()->activate();
    }
    const QSize lockedSize = sizeHint().expandedTo(QSize(420, 340));
    setFixedSize(lockedSize);
}
