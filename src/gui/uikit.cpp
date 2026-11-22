#include "uikit.h"

#include "text.h"

#include <QFrame>
#include <QLabel>
#include <QPalette>

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

}  // namespace zfsmgr::gui
