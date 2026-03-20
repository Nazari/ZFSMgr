#include "masterpassworddialog.h"
#include "i18nmanager.h"

#include <QComboBox>
#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QIcon>
#include <QProcessEnvironment>

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.9.9rc3"
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
    resize(336, 340);
    setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));

    auto* root = new QVBoxLayout(this);
    m_iconLabel = new QLabel(this);
    m_iconLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_iconLabel->setPixmap(QPixmap(QStringLiteral(":/icons/ZFSMgr-512.png")).scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    root->addWidget(m_iconLabel);
    m_formLayout = new QFormLayout();

    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItem(QString(), QStringLiteral("es"));
    m_languageCombo->addItem(QString(), QStringLiteral("en"));
    m_languageCombo->addItem(QString(), QStringLiteral("zh"));
    m_languageLabel = new QLabel(this);
    m_formLayout->addRow(m_languageLabel, m_languageCombo);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordLabel = new QLabel(this);
    m_formLayout->addRow(m_passwordLabel, m_passwordEdit);
    m_passwordConfirmEdit = new QLineEdit(this);
    m_passwordConfirmEdit->setEchoMode(QLineEdit::Password);
    m_passwordConfirmLabel = new QLabel(this);
    m_formLayout->addRow(m_passwordConfirmLabel, m_passwordConfirmEdit);
    m_localUserEdit = new QLineEdit(this);
    const QString envUser = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USER")).trimmed();
    const QString envUserWin = QProcessEnvironment::systemEnvironment().value(QStringLiteral("USERNAME")).trimmed();
    m_localUserEdit->setText(!envUser.isEmpty() ? envUser : envUserWin);
    m_localUserLabel = new QLabel(this);
    m_formLayout->addRow(m_localUserLabel, m_localUserEdit);
    m_localPasswordEdit = new QLineEdit(this);
    m_localPasswordEdit->setEchoMode(QLineEdit::Password);
    m_localPasswordLabel = new QLabel(this);
    m_formLayout->addRow(m_localPasswordLabel, m_localPasswordEdit);
    root->addLayout(m_formLayout);

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

    auto* actionsRow = new QHBoxLayout();
    actionsRow->setSpacing(8);
    actionsRow->addStretch(1);
    actionsRow->addWidget(m_changePwdButton);
    actionsRow->addWidget(m_resetIniButton);
    actionsRow->addStretch(1);
    root->addLayout(actionsRow);

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
    connect(m_languageCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        m_lang = selectedLanguage();
        retranslateUi();
    });

    retranslateUi();
    if (layout()) {
        layout()->activate();
    }
    const QSize lockedSize = sizeHint().expandedTo(QSize(336, 340));
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
    if (!m_languageCombo) {
        return QStringLiteral("es");
    }
    const QString code = m_languageCombo->currentData().toString().trimmed().toLower();
    return code.isEmpty() ? QStringLiteral("es") : code;
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

void MasterPasswordDialog::setRequestLocalSudoCredentials(bool enabled) {
    m_requestLocalSudoCredentials = enabled;
    retranslateUi();
}

QString MasterPasswordDialog::localUsername() const {
    return m_localUserEdit ? m_localUserEdit->text().trimmed() : QString();
}

QString MasterPasswordDialog::localPassword() const {
    return m_localPasswordEdit ? m_localPasswordEdit->text() : QString();
}

void MasterPasswordDialog::setSelectedLanguage(const QString& langCode) {
    if (!m_languageCombo) {
        return;
    }
    const int idx = languageIndexForCode(langCode);
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
    const QString lang = selectedLanguage();
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(460, 220);
    dlg.setWindowTitle(trk(lang,
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
    form->addRow(trk(lang, QStringLiteral("t_cur_pwd_lbl001"), QStringLiteral("Password actual"), QStringLiteral("Current password"), QStringLiteral("当前密码")), oldPwd);
    form->addRow(trk(lang, QStringLiteral("t_new_pwd_lbl001"), QStringLiteral("Password nuevo"), QStringLiteral("New password"), QStringLiteral("新密码")), newPwd);
    form->addRow(trk(lang, QStringLiteral("t_rep_pwd_lbl001"), QStringLiteral("Repetir password"), QStringLiteral("Repeat password"), QStringLiteral("重复密码")), newPwd2);
    root->addLayout(form);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    root->addWidget(box);
    if (QPushButton* okButton = box->button(QDialogButtonBox::Ok)) {
        okButton->setText(trk(lang,
                              QStringLiteral("t_aceptar_8f9f73"),
                              QStringLiteral("Aceptar"),
                              QStringLiteral("Accept"),
                              QStringLiteral("确定")));
    }
    if (QPushButton* cancelButton = box->button(QDialogButtonBox::Cancel)) {
        cancelButton->setText(trk(lang,
                                  QStringLiteral("t_cancelar_c111e0"),
                                  QStringLiteral("Cancelar"),
                                  QStringLiteral("Cancel"),
                                  QStringLiteral("取消")));
    }
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
                             trk(lang, QStringLiteral("t_new_pwd_empty1"), QStringLiteral("El nuevo password no puede estar vacío."),
                                 QStringLiteral("New password cannot be empty."),
                                 QStringLiteral("新密码不能为空。")));
        return;
    }
    if (newv != newv2) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(lang, QStringLiteral("t_pwd_confirm01"), QStringLiteral("La confirmación no coincide."),
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
    refreshLanguageComboTexts();
    if (m_languageLabel) {
        m_languageLabel->setText(trk(lang, QStringLiteral("t_idioma_009433"),
                                     QStringLiteral("Idioma"),
                                     QStringLiteral("Language"),
                                     QStringLiteral("语言")));
    }
    if (m_passwordLabel) {
        m_passwordLabel->setText(trk(lang, QStringLiteral("t_password_8be3c9"),
                                     QStringLiteral("Password"),
                                     QStringLiteral("Password"),
                                     QStringLiteral("密码")));
    }
    if (m_passwordConfirmLabel) {
        const QString repeatText = trk(lang, QStringLiteral("t_repeat_pwd_001"),
                                       QStringLiteral("Repetir password"),
                                       QStringLiteral("Repeat password"),
                                       QStringLiteral("重复密码"));
        m_passwordConfirmLabel->setText(repeatText);
        m_passwordConfirmLabel->setVisible(m_firstRunCreationMode);
        m_passwordConfirmLabel->setEnabled(m_firstRunCreationMode);
        if (m_formLayout) {
            if (QWidget* rowLabel = m_formLayout->labelForField(m_passwordConfirmEdit)) {
                if (auto* formLabel = qobject_cast<QLabel*>(rowLabel)) {
                    formLabel->setText(repeatText);
                    formLabel->setVisible(m_firstRunCreationMode);
                    formLabel->setEnabled(m_firstRunCreationMode);
                }
            }
        }
    }
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
    if (m_localUserLabel) {
        const QString userText = trk(lang,
                                     QStringLiteral("t_usuario_d31f58"),
                                     QStringLiteral("Usuario"),
                                     QStringLiteral("User"),
                                     QStringLiteral("用户"));
        m_localUserLabel->setText(userText);
        m_localUserLabel->setVisible(m_requestLocalSudoCredentials);
        m_localUserLabel->setEnabled(m_requestLocalSudoCredentials);
        if (m_formLayout) {
            if (QWidget* rowLabel = m_formLayout->labelForField(m_localUserEdit)) {
                rowLabel->setVisible(m_requestLocalSudoCredentials);
                rowLabel->setEnabled(m_requestLocalSudoCredentials);
            }
        }
    }
    if (m_localUserEdit) {
        m_localUserEdit->setPlaceholderText(trk(lang,
                                                QStringLiteral("t_local_user_ph_001"),
                                                QStringLiteral("Usuario local con sudo"),
                                                QStringLiteral("Local sudo user"),
                                                QStringLiteral("本地 sudo 用户")));
        m_localUserEdit->setVisible(m_requestLocalSudoCredentials);
        m_localUserEdit->setEnabled(m_requestLocalSudoCredentials);
    }
    if (m_localPasswordLabel) {
        const QString passwordText = trk(lang,
                                         QStringLiteral("t_password_8be3c9"),
                                         QStringLiteral("Password"),
                                         QStringLiteral("Password"),
                                         QStringLiteral("密码"));
        m_localPasswordLabel->setText(passwordText);
        m_localPasswordLabel->setVisible(m_requestLocalSudoCredentials);
        m_localPasswordLabel->setEnabled(m_requestLocalSudoCredentials);
        if (m_formLayout) {
            if (QWidget* rowLabel = m_formLayout->labelForField(m_localPasswordEdit)) {
                rowLabel->setVisible(m_requestLocalSudoCredentials);
                rowLabel->setEnabled(m_requestLocalSudoCredentials);
            }
        }
    }
    if (m_localPasswordEdit) {
        m_localPasswordEdit->setPlaceholderText(trk(lang,
                                                    QStringLiteral("t_local_pwd_ph_001"),
                                                    QStringLiteral("Password local sudo"),
                                                    QStringLiteral("Local sudo password"),
                                                    QStringLiteral("本地 sudo 密码")));
        m_localPasswordEdit->setVisible(m_requestLocalSudoCredentials);
        m_localPasswordEdit->setEnabled(m_requestLocalSudoCredentials);
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
            QString info = trk(lang,
                               QStringLiteral("t_create_ini_001"),
                               QStringLiteral("No existe config.ini. Se va a crear ahora.\nIntroduzca y confirme el password maestro."),
                               QStringLiteral("config.ini does not exist. It will be created now.\nEnter and confirm the master password."),
                               QStringLiteral("config.ini 不存在，将立即创建。\n请输入并确认主密码。"));
            if (m_requestLocalSudoCredentials) {
                info += QStringLiteral("\n\n") + trk(lang,
                                                     QStringLiteral("t_local_sudo_boot_001"),
                                                     QStringLiteral("También debe indicar usuario y password local con sudo."),
                                                     QStringLiteral("You must also provide the local sudo user and password."),
                                                     QStringLiteral("还必须提供本地 sudo 用户和密码。"));
            }
            m_creationInfoLabel->setText(info);
            m_creationInfoLabel->show();
        } else if (m_requestLocalSudoCredentials) {
            m_creationInfoLabel->setText(trk(lang,
                                             QStringLiteral("t_local_sudo_boot_002"),
                                             QStringLiteral("No existe connLocal.ini.\nIntroduzca usuario y password local con sudo."),
                                             QStringLiteral("connLocal.ini does not exist.\nEnter the local sudo user and password."),
                                             QStringLiteral("connLocal.ini 不存在。\n请输入本地 sudo 用户和密码。")));
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
    const QSize lockedSize = sizeHint().expandedTo(QSize(336, 340));
    setFixedSize(lockedSize);
}

int MasterPasswordDialog::languageIndexForCode(const QString& langCode) const {
    if (!m_languageCombo) {
        return -1;
    }
    const QString code = langCode.trimmed().toLower();
    for (int i = 0; i < m_languageCombo->count(); ++i) {
        if (m_languageCombo->itemData(i).toString().trimmed().toLower() == code) {
            return i;
        }
    }
    return -1;
}

void MasterPasswordDialog::refreshLanguageComboTexts() {
    if (!m_languageCombo) {
        return;
    }
    const QString currentCode = selectedLanguage();
    const QSignalBlocker blocker(m_languageCombo);
    const int esIdx = languageIndexForCode(QStringLiteral("es"));
    const int enIdx = languageIndexForCode(QStringLiteral("en"));
    const int zhIdx = languageIndexForCode(QStringLiteral("zh"));
    QString esText = QStringLiteral("Español");
    QString enText = QStringLiteral("Inglés");
    QString zhText = QStringLiteral("Chino");
    if (currentCode == QStringLiteral("en")) {
        esText = QStringLiteral("Spanish");
        enText = QStringLiteral("English");
        zhText = QStringLiteral("Chinese");
    } else if (currentCode == QStringLiteral("zh")) {
        esText = QStringLiteral("西班牙语");
        enText = QStringLiteral("英语");
        zhText = QStringLiteral("中文");
    }
    if (esIdx >= 0) {
        m_languageCombo->setItemText(esIdx, esText);
    }
    if (enIdx >= 0) {
        m_languageCombo->setItemText(enIdx, enText);
    }
    if (zhIdx >= 0) {
        m_languageCombo->setItemText(zhIdx, zhText);
    }
    const int currentIdx = languageIndexForCode(currentCode);
    if (currentIdx >= 0) {
        m_languageCombo->setCurrentIndex(currentIdx);
    }
    m_languageCombo->update();
    if (QAbstractItemView* popupView = m_languageCombo->view()) {
        popupView->viewport()->update();
    }
}
