#include "masterpassworddialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
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
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &QDialog::accept);
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

void MasterPasswordDialog::retranslateUi() {
    const QString lang = selectedLanguage();
    setWindowTitle(QStringLiteral("ZFSMgr"));
    m_passwordEdit->setPlaceholderText(tr3(lang, QStringLiteral("Password maestro"), QStringLiteral("Master password"), QStringLiteral("主密码")));
    m_okButton->setText(tr3(lang, QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确定")));
    if (m_cancelButton) {
        m_cancelButton->setText(tr3(lang, QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")));
    }
    if (m_authorLabel) {
        m_authorLabel->setText(tr3(lang,
                                   QStringLiteral("Autor: Eladio Linares  |  Licencia: GNU"),
                                   QStringLiteral("Author: Eladio Linares  |  License: GNU"),
                                   QStringLiteral("作者：Eladio Linares  |  许可证：GNU")));
    }
}
