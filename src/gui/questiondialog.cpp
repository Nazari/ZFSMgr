#include "questiondialog.h"

#include "text.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace zfsmgr::gui {

namespace {

QLabel* paragraph(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QString bullets(const QStringList& items) {
    QString html = QStringLiteral("<ul style='margin:0'>");
    for (const QString& i : items) {
        html += QStringLiteral("<li>") + i.toHtmlEscaped() + QStringLiteral("</li>");
    }
    return html + QStringLiteral("</ul>");
}

}  // namespace

QuestionDialog::QuestionDialog(const CliQuestion& q, QWidget* parent) : QDialog(parent) {
    setWindowTitle(q.title);
    setModal(true);
    setMinimumWidth(480);
    auto* lay = new QVBoxLayout(this);

    auto* title = paragraph(q.title, this);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    f.setBold(true);
    title->setFont(f);
    lay->addWidget(title);
    if (q.irreversible) {
        auto* warn = paragraph(L("This cannot be undone.", "Esto no se puede deshacer."), this);
        warn->setStyleSheet(QStringLiteral("color:#B42318; font-weight:600"));
        lay->addWidget(warn);
    }
    if (!q.lose.isEmpty()) {
        lay->addWidget(paragraph(q.irreversible ? L("Lost for good:", "Se pierde para siempre:")
                                                : L("Lost:", "Se pierde:"),
                                 this));
        auto* l = paragraph(bullets(q.lose), this);
        l->setTextFormat(Qt::RichText);
        lay->addWidget(l);
    }
    if (!q.keep.isEmpty()) {
        lay->addWidget(paragraph(L("Kept:", "Se conserva:"), this));
        auto* l = paragraph(bullets(q.keep), this);
        l->setTextFormat(Qt::RichText);
        lay->addWidget(l);
    }
    for (const QString& n : q.notes) {
        auto* l = paragraph(n, this);
        l->setStyleSheet(QStringLiteral("color:palette(mid)"));
        lay->addWidget(l);
    }

    auto* buttons = new QDialogButtonBox(this);
    buttons->addButton(QDialogButtonBox::Cancel);
    m_yes = buttons->addButton(
        q.irreversible ? L("Do it for good", "Hacerlo para siempre") : L("Continue", "Seguir"),
        QDialogButtonBox::AcceptRole);
    if (q.irreversible) {
        m_yes->setStyleSheet(QStringLiteral("background:#B42318; color:white"));
    }
    if (!q.typeToConfirm.isEmpty()) {
        lay->addWidget(paragraph(L("To confirm, type %1:", "Para confirmar, escribe %1:")
                                     .arg(QStringLiteral("<b>") + q.typeToConfirm.toHtmlEscaped()
                                          + QStringLiteral("</b>")),
                                 this));
        m_word = new QLineEdit(this);
        lay->addWidget(m_word);
        m_yes->setEnabled(false);
        const QString word = q.typeToConfirm;
        connect(m_word, &QLineEdit::textChanged, this,
                [this, word](const QString& t) { m_yes->setEnabled(t.trimmed() == word); });
        m_word->setFocus();
    } else {
        // The safe choice is the default: Enter does not destroy anything.
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

QString QuestionDialog::typed() const {
    return m_word ? m_word->text().trimmed() : QString();
}

}  // namespace zfsmgr::gui
