#pragma once

// The small pieces every page is built from, so the same state looks the same everywhere:
// green good, amber needs a look, red broken, grey waiting or paused.

#include <QJsonObject>
#include <QString>

#include <functional>

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

// A read-only window with what the CLI wrote (a pool's history, a dataset's permissions…),
// opened with open(): it never blocks the window behind it.
void showText(QWidget* parent, const QString& title, const QString& text);

// How many snapshots of each class are kept: one spin box per class, starting at `keep`
// ({"hourly":n, "daily":n…}). On OK `done` gets the flags for `schedule`, «--hourly 0 --daily 7…».
void askKeep(QWidget* parent, const QString& title, const QString& note, const QJsonObject& keep,
             std::function<void(const QString& flags)> done);

}  // namespace zfsmgr::gui
