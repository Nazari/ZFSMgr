#pragma once

// The interface's own texts, in the language of the system: Spanish or English, the same rule
// the CLI follows. They are few on purpose —labels and headings—: everything that explains a
// state, a warning or a question comes already written by the CLI in that language.

#include <QLocale>
#include <QString>

namespace zfsmgr::gui {

// «es» or «en», decided once at start-up from the system's locale.
inline const QString& uiLang() {
    // QLocale(), not QLocale::system(): the same unless a test sets a default.
    static const QString lang =
        QLocale().language() == QLocale::Spanish ? QStringLiteral("es") : QStringLiteral("en");
    return lang;
}

// L("Refresh", "Actualizar"): the text in the interface's language.
inline QString L(const char* en, const char* es) {
    return QString::fromUtf8(uiLang() == QLatin1String("es") ? es : en);
}

}  // namespace zfsmgr::gui
