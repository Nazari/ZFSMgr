#include "unlockdialog.h"

#include "buildstamp.h"
#include "text.h"
#include "uikit.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace zfsmgr::gui {

UnlockDialog::UnlockDialog(bool keyPresent, QWidget* parent) : QDialog(parent) {
    setWindowTitle(appTitle());
    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(10);

    // The key, first and large: it is the quick way.
    m_key = new QLabel(this);
    QFont big = m_key->font();
    big.setPointSizeF(big.pointSizeF() * 1.3);
    big.setBold(true);
    m_key->setFont(big);
    m_key->setWordWrap(true);
    lay->addWidget(m_key);
    m_keyNote = new QLabel(this);
    m_keyNote->setWordWrap(true);
    m_keyNote->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(QStringLiteral("muted"))));
    lay->addWidget(m_keyNote);

    lay->addSpacing(6);
    lay->addWidget(
        new QLabel(L("Or type the master password:", "O escribe la contraseña maestra:"), this));
    auto* row = new QHBoxLayout();
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    row->addWidget(m_password, 1);
    m_enter = new QPushButton(L("Enter", "Entrar"), this);
    m_enter->setDefault(true);
    row->addWidget(m_enter);
    lay->addLayout(row);
    m_error = new QLabel(this);
    m_error->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(QStringLiteral("error"))));
    m_error->setWordWrap(true);
    lay->addWidget(m_error);

    connect(m_enter, &QPushButton::clicked, this, &UnlockDialog::send);
    connect(m_password, &QLineEdit::returnPressed, this, &UnlockDialog::send);
    setKeyPresent(keyPresent);
    resize(440, sizeHint().height());
    m_password->setFocus();
}

void UnlockDialog::setKeyPresent(bool present) {
    if (present) {
        m_key->setText(L("Touch your key to get in", "Toca tu llave para entrar"));
        m_keyNote->setText(L("It blinks while it waits.", "Parpadea mientras espera."));
    } else {
        m_key->setText(L("Plug in your key and touch it", "Conecta tu llave y tócala"));
        m_keyNote->setText(L("None is connected now; it is noticed as soon as it is.",
                             "Ahora no hay ninguna conectada; se nota en cuanto la conectes."));
    }
}

void UnlockDialog::setTouchWaiting() {
    setKeyPresent(true);
}

void UnlockDialog::setPasswordWrong() {
    m_password->setEnabled(true);
    m_enter->setEnabled(true);
    m_password->clear();
    m_password->setFocus();
    m_error->setText(L("That is not the master password.", "Esa no es la contraseña maestra."));
}

void UnlockDialog::setKeyFailed(const QString& code) {
    if (code == QLatin1String("not_registered")) {
        m_keyNote->setText(
            L("This key is not registered here: use the master password, or "
              "another key.",
              "Esta llave no está registrada aquí: usa la contraseña maestra u "
              "otra llave."));
    } else if (code == QLatin1String("pin_required")) {
        m_keyNote->setText(
            L("This key asks for its PIN, which is not supported at start yet: "
              "use the master password.",
              "Esta llave pide su PIN, que al arrancar todavía no se admite: usa "
              "la contraseña maestra."));
    } else {
        m_keyNote->setText(L("The key failed; touch it again or use the master password.",
                             "La llave falló; tócala otra vez o usa la contraseña maestra."));
    }
}

void UnlockDialog::send() {
    if (m_password->text().isEmpty()) {
        return;
    }
    m_error->clear();
    m_password->setEnabled(false);
    m_enter->setEnabled(false);
    QByteArray pw = m_password->text().toUtf8();
    Q_EMIT passwordEntered(pw);
    pw.fill('\0');
}

}  // namespace zfsmgr::gui
