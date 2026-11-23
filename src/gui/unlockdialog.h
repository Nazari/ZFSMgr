#pragma once

// The start window when FIDO2 keys are registered: «touch your key» and the master password at
// once, whichever comes first. The CLI's service does the work (it is locked: see «Cerrado» in
// docs/protocolo_serve.md); this only shows what it says and hands it the password.

#include <QByteArray>
#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace zfsmgr::gui {

class UnlockDialog : public QDialog {
    Q_OBJECT
public:
    explicit UnlockDialog(bool keyPresent, QWidget* parent = nullptr);

    // What the service says.
    void setKeyPresent(bool present);
    void setTouchWaiting();
    void setPasswordWrong();
    void setKeyFailed(const QString& code);

Q_SIGNALS:
    void passwordEntered(const QByteArray& password);

private:
    void send();

    QLabel* m_key{nullptr};
    QLabel* m_keyNote{nullptr};
    QLineEdit* m_password{nullptr};
    QPushButton* m_enter{nullptr};
    QLabel* m_error{nullptr};
};

}  // namespace zfsmgr::gui
