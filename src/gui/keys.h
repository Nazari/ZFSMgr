#pragma once

// Keys between machines, as the interface handles them: one kind only, the FULL keys —with
// them a machine both pulls copies from another and sends them to it—, and every machine
// holds those of every other. A machine is added to work with it, so adding one gives it the
// keys of all the others and gives it to all of them (AddMachineDialog); Administration can
// repeat that for everybody at once.
//
// The CLI keeps its `--role pull` for whoever wants it by hand; the interface never offers it.

#include <QStringList>

#include <functional>

class QObject;

namespace zfsmgr::gui {

class StateStore;

// Every connected machine whose daemon answers gets the full keys of all the others
// (`peers --push --role full --on <it>`), one after another on the general service and already
// confirmed: the person chose it by adding a machine or by pressing the button. `done` gets the
// machines that received them and, for the rest, «machine: why».
void giveAllKeys(StateStore* store, QObject* owner,
                 std::function<void(const QStringList& given, const QStringList& failed)> done);

}  // namespace zfsmgr::gui
