#pragma once

// Adding a machine: how to reach it, and then what is there. It runs `create` without --os
// —the CLI asks the machine which system it is—, and the password, if any, is asked by the
// CLI as a `secret` event: it never goes on a command line. Then `probe` says whether it
// works, and if its daemon is missing the dialog offers to install it.

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace zfsmgr::gui {

class StateStore;

class AddMachineDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddMachineDialog(StateStore* store, QWidget* parent = nullptr);

Q_SIGNALS:
    void added();

private:
    void create();
    void probe();
    QString problem() const;

    StateStore* m_store;
    QLineEdit* m_id{nullptr};
    QLineEdit* m_host{nullptr};
    QLineEdit* m_user{nullptr};
    QSpinBox* m_port{nullptr};
    QLineEdit* m_key{nullptr};
    QCheckBox* m_sudo{nullptr};
    QLabel* m_result{nullptr};
    QPushButton* m_go{nullptr};
    QPushButton* m_install{nullptr};
    QString m_created;  // the id, once created
};

// Quotes a value for a command line when it has spaces: «"/home/x/my key"».
QString quoted(const QString& value);

}  // namespace zfsmgr::gui
