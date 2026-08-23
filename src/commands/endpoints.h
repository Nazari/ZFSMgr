#pragma once

#include <string>

// The actions that need TWO endpoints: a source and a target.
//
// In the Qt interface they are asked for by marking a source and then clicking on another
// node, like copy and paste; the «With the source …» submenu offers all six and **greys out
// the ones that do not apply, with the reason**. That «what applies and why not» is a RULE
// —what ZFS lets you do between two objects— and not an interface decision, so it lives
// here: the interface used to keep it inside the context menu, and any second client would
// have ended up with a second copy that drifts out of step.
//
// See help/en/menus_contextuales.md, «The six source-and-target actions».
namespace zfsmgr::base::endpoints {

enum class Action {
    Diff,
    Clone,
    Send,
    Move,
    Sync,
    Level,
};

// Why it can NOT be done, typed. A boolean forced whoever paints the menu to guess the
// reason, and the reason is exactly what has to be shown: «does not apply» without saying
// why leaves the user trying combinations.
enum class NotApplicable {
    None_,                  // it does apply
    NoSource,
    SameObject,
    SourceIsNotSnapshot,
    TargetIsNotDataset,
    DifferentDataset,       // `zfs diff` compares two points of the SAME dataset
    DifferentMachine,
    DifferentPool,          // `zfs rename` does not cross pools
    SourceIsNotDataset,
    TargetInsideSource,     // putting a dataset inside itself
    NotInTheWebYet,
};

const char* keyOf(Action a);
std::string labelOf(Action a);
std::string labelOf(NotApplicable n);

// One endpoint: on which machine, and which object.
struct Endpoint {
    std::string connection;
    std::string object;

    bool empty() const { return connection.empty() || object.empty(); }
    bool isSnapshot() const { return object.find('@') != std::string::npos; }
    // The dataset it belongs to: itself when it is not a snapshot, and whatever comes
    // before the «@» when it is.
    std::string dataset() const {
        const std::size_t i = object.find('@');
        return i == std::string::npos ? object : object.substr(0, i);
    }
    // The pool: whatever comes before the first slash. Needed because `zfs rename` does NOT
    // cross pools —that is what send is for— and the reason has to be sayable.
    std::string pool() const {
        const std::string d = dataset();
        const std::size_t i = d.find('/');
        return i == std::string::npos ? d : d.substr(0, i);
    }
    // The last component of the name, which is the one it keeps when moved.
    std::string leaf() const {
        const std::string d = dataset();
        const std::size_t i = d.rfind('/');
        return i == std::string::npos ? d : d.substr(i + 1);
    }
};

// Where a dataset moved under another one ends up: «target/leaf of the source».
//
// It lives here and not in whoever paints the menu because it is the SAME arithmetic the Qt
// interface does when it queues the rename, and having it twice means having it wrong in
// one of the two places.
std::string moveDestination(const Endpoint& source, const Endpoint& target);

// Can `a` be done from `source` to `target`? `NotApplicable::None_` means yes.
NotApplicable check(Action a, const Endpoint& source, const Endpoint& target);

}  // namespace zfsmgr::base::endpoints
