#pragma once

// Creating a pool (advanced mode): a name, the free disks of that machine (`devices`), how they
// are arranged and, optionally, the options of another pool. The dialog shows the exact
// command; the rules —how many disks each arrangement needs— are zpool's, and the CLI asks
// before writing anything, with the pool's name to type.

#include <QDialog>
#include <QJsonArray>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace zfsmgr::gui {

class StateStore;

class CreatePoolDialog : public QDialog {
    Q_OBJECT
public:
    CreatePoolDialog(StateStore* store, QString machine, QWidget* parent = nullptr);

Q_SIGNALS:
    void created();

private:
    QString command() const;
    void update();

    StateStore* m_store;
    QString m_machine;
    QLineEdit* m_name{nullptr};
    QListWidget* m_disks{nullptr};
    QComboBox* m_layout{nullptr};
    QComboBox* m_optsAs{nullptr};
    QLabel* m_command{nullptr};
    QLabel* m_result{nullptr};
};

}  // namespace zfsmgr::gui
