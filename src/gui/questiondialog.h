#pragma once

// The question the CLI asks before something a person should think about first, shown as it
// comes: the title, whether it can be undone, what is lost, what is kept, the notes. When the
// CLI asks for a word to be typed, the button only works once it is typed; the CLI checks it
// again on its side, so this is help for the person, not the safeguard.

#include "cliservice.h"

#include <QDialog>

class QLineEdit;
class QPushButton;

namespace zfsmgr::gui {

class QuestionDialog : public QDialog {
    Q_OBJECT
public:
    explicit QuestionDialog(const CliQuestion& q, QWidget* parent = nullptr);
    QString typed() const;

private:
    QLineEdit* m_word{nullptr};
    QPushButton* m_yes{nullptr};
};

}  // namespace zfsmgr::gui
