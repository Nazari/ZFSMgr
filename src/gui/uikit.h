#pragma once

// The small pieces every page is built from, so the same state looks the same everywhere:
// green good, amber needs a look, red broken, grey waiting or paused.

#include <QString>

class QFrame;
class QLabel;
class QWidget;

namespace zfsmgr::gui {

QString colourOf(const QString& state);
// «● Fine», «▲ Needs a look», «○ Pending», «✕ Failing», «‖ Paused», «… Asking».
QString stateWord(const QString& state);
QLabel* label(const QString& text, QWidget* parent, bool wrap = true);
QLabel* heading(const QString& text, QWidget* parent);
QLabel* muted(const QString& text, QWidget* parent);
QLabel* stateLabel(const QString& state, QWidget* parent);
QFrame* card(QWidget* parent);

}  // namespace zfsmgr::gui
