#include "uikit.h"

#include "text.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPalette>
#include <QScrollArea>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include <QVector>

namespace zfsmgr::gui {

// The colours of a state, the same everywhere: green good, amber needs a look, red broken,
// grey waiting.
QString colourOf(const QString& state) {
    if (state == QLatin1String("ok")) {
        return QStringLiteral("#1A7F37");
    }
    if (state == QLatin1String("warning") || state == QLatin1String("pending")) {
        return QStringLiteral("#9A6700");
    }
    if (state == QLatin1String("error")) {
        return QStringLiteral("#B42318");
    }
    return QStringLiteral("#6B7380");
}

QString stateWord(const QString& state) {
    if (state == QLatin1String("ok")) {
        return L("● Fine", "● Bien");
    }
    if (state == QLatin1String("warning")) {
        return L("▲ Needs a look", "▲ Revisar");
    }
    if (state == QLatin1String("pending")) {
        return L("○ Pending", "○ Pendiente");
    }
    if (state == QLatin1String("error")) {
        return L("✕ Failing", "✕ Falla");
    }
    if (state == QLatin1String("paused")) {
        return L("‖ Paused", "‖ En pausa");
    }
    return L("… Asking", "… Preguntando");
}

QLabel* label(const QString& text, QWidget* parent, bool wrap) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(wrap);
    l->setTextFormat(Qt::PlainText);
    return l;
}

QLabel* heading(const QString& text, QWidget* parent) {
    auto* l = label(text, parent, false);
    QFont f = l->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.15);
    l->setFont(f);
    return l;
}

QLabel* muted(const QString& text, QWidget* parent) {
    auto* l = label(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, p.color(QPalette::Disabled, QPalette::WindowText));
    l->setPalette(p);
    return l;
}

QLabel* stateLabel(const QString& state, QWidget* parent) {
    auto* l = label(stateWord(state), parent, false);
    l->setStyleSheet(QStringLiteral("color:%1; font-weight:600").arg(colourOf(state)));
    return l;
}

QFrame* card(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::StyledPanel);
    f->setObjectName(QStringLiteral("card"));
    return f;
}

void showText(QWidget* parent, const QString& title, const QString& text) {
    auto* d = new QDialog(parent);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(title);
    auto* lay = new QVBoxLayout(d);
    auto* t = new QPlainTextEdit(d);
    t->setReadOnly(true);
    t->setLineWrapMode(QPlainTextEdit::NoWrap);
    t->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    t->setPlainText(text.trimmed().isEmpty() ? L("(nothing)", "(nada)") : text);
    lay->addWidget(t);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, d);
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    lay->addWidget(bb);
    d->resize(760, 460);
    d->open();
}

void askKeep(QWidget* parent, const QString& title, const QString& note, const QJsonObject& keep,
             std::function<void(const QString& flags)> done) {
    auto* d = new QDialog(parent);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(title);
    auto* lay = new QVBoxLayout(d);
    if (!note.isEmpty()) {
        lay->addWidget(muted(note, d));
    }
    auto* form = new QFormLayout();
    QVector<QPair<QString, QSpinBox*>> spins;
    const QVector<QPair<QString, QString>> classes = {
        {QStringLiteral("hourly"), L("Hourly", "Por hora")},
        {QStringLiteral("daily"), L("Daily", "Diarias")},
        {QStringLiteral("weekly"), L("Weekly", "Semanales")},
        {QStringLiteral("monthly"), L("Monthly", "Mensuales")},
        {QStringLiteral("yearly"), L("Yearly", "Anuales")}};
    for (const auto& c : classes) {
        auto* s = new QSpinBox(d);
        s->setRange(0, 999);
        s->setValue(keep.value(c.first).toInt());
        form->addRow(c.second, s);
        spins.push_back({c.first, s});
    }
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    lay->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    QObject::connect(bb, &QDialogButtonBox::accepted, d, [d, spins, done] {
        QStringList flags;
        for (const auto& s : spins) {
            flags << QStringLiteral("--%1 %2").arg(s.first).arg(s.second->value());
        }
        done(flags.join(QLatin1Char(' ')));
        d->accept();
    });
    d->open();
}

void replaceScrollWidget(QScrollArea* area, QWidget* w) {
    if (QWidget* old = area->takeWidget()) {
        old->hide();
        old->deleteLater();
    }
    area->setWidget(w);
}

}  // namespace zfsmgr::gui
