#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// SCHEDULED snapshots (GSA): what they are and what is valid.
//
// A dataset's schedule does not live in a file belonging to this program: it is made of USER
// PROPERTIES on the dataset itself, prefixed `org.fc16.gsa:`. What runs them is a different
// agent —`/usr/local/libexec/zfsmgr-gsa.sh`, with its timer— and neither the interface nor
// the shell takes part in that: both do nothing but read and write properties.
//
// **Why this is in the base layer and not in the interface.** The rules lived inside
// `MainWindow::validatePendingGsaDrafts`, in Qt, and nowhere else. The moment the shell
// wanted to schedule anything —and it already can: `set org.fc16.gsa:diario=7` works today—
// they would have to be rewritten, and there would be two copies drifting apart. This
// repository has already paid that bill three times: the ZFS property table, the transport
// reasons and the `zfs send` flags.
//
// There are no messages here: there are TYPED REASONS. Each interface words them as it must
// —the graphical one in its three languages, the shell in its own— and neither can invent a
// rule the other does not have.
//
// See docs/propuesta_gsa_cli.md.
namespace zfsmgr::base::gsa {

// The property prefix. The names themselves are in Spanish because that is how they are on
// machines already out there, and renaming them would break their schedules.
extern const char* const kPropertyPrefix;

struct Schedule {
    bool enabled{false};
    bool recursive{false};
    bool level{false};
    int hourly{0};
    int daily{0};
    int weekly{0};
    int monthly{0};
    int yearly{0};
    std::string target;   // «Connection::Pool/Dataset»

    bool hasNoRetentions() const {
        return hourly <= 0 && daily <= 0 && weekly <= 0 && monthly <= 0 && yearly <= 0;
    }
};

enum class Failure {
    None_,
    RetentionNotAnInteger,   // detail: the offending property
    EnabledWithNoRetention,
    LevelWithNoTarget,
    MalformedTarget,         // no «::» in it
    TargetHasNoConnection,   // detail: the name of the missing connection
    ClashesWithRecursive,    // detail: the dataset that already has one
};

struct Reason {
    Failure failure{Failure::None_};
    std::string dataset;   // who it happens to
    std::string detail;    // the property, the missing connection, or the other dataset
};

// Properties exactly as `zfs get` returns them → structure. Case-insensitive on the
// property name, as the interface was.
//
// It returns false only when a retention is not an integer >= 0; the remaining values cannot
// fail here (an unrecognised boolean is «off», which is the conservative reading).
bool fromProperties(const std::map<std::string, std::string>& props, Schedule& out,
                      Reason& why);

// Structure → the properties that have to be written, prefix included.
std::map<std::string, std::string> toProperties(const Schedule& p);

// One schedule, on its own. `connectionExists` is resolved by the caller: the list of
// connections belongs to the client, not to this layer.
bool isValid(const std::string& dataset, const Schedule& p,
            const std::function<bool(const std::string&)>& connectionExists, Reason& why);

// The set as a whole: two ENABLED schedules in the same pool cannot overlap when one of them
// is recursive. Checked apart because it is not a property of either one.
struct Entry {
    std::string dataset;
    Schedule schedule;
};
bool isValidSet(const std::vector<Entry>& fromTheSamePool, Reason& why);

// Is `dataset` the same as `ancestor`, or does it hang from it?
bool isSameOrDescendant(const std::string& dataset, const std::string& ancestor);

// The fallback Spanish wording of the reason, for whoever has no catalogue of their own.
std::string labelOf(Failure f);

// Which CLASS a snapshot belongs to, by its name: «hourly», «daily», «weekly», «monthly»,
// «yearly» —or whatever it says, since the classes are not a closed set—. Empty when the
// scheduler did not make it.
//
// The name is written by `gsaCreateSnapshot` as «GSA-<class>-<date>-<time>», so the class is
// whatever sits between the first and the second hyphen.
std::string snapshotClass(const std::string& name);

// ── The target, in the two shapes it has ─────────────────────────────────────
//
// **Stored in ZFS it goes as «Connection::Pool/Dataset»**, and that cannot change: it is
// written into the properties of datasets that already exist, and the daemon's scheduler
// splits it on «::» (`daemon_main.cpp`). Changing the format would break the schedules
// already out there and the interface that wrote them.
//
// But that notation predates `zfsm://`, and on screen it sits badly next to the addresses
// the rest of the program uses. So it is STORED as always and SHOWN as a URL. These two
// functions are the conversion, and they live here —next to what reads and validates the
// target— so that there does not end up being one copy per client.

// «Connection::Pool/Dataset» → «zfsm://Connection/Pool/Dataset». Returns the text as-is when
// it does not have the expected shape: showing something odd beats hiding it.
std::string destinationAsUrl(const std::string& target);

// The way back. It accepts BOTH shapes on input —a URL or the long-standing format— because
// whoever types it by hand may write either, and both are understood.
std::string destinationFromUrl(const std::string& text);

// A dataset's snapshots, ORDERED for display: first the manual ones, in the order they
// arrived, and then the scheduled ones grouped by class, with the known classes in order of
// period —from hourly to yearly— and the unknown ones last.
//
// Returns (class, snapshots) pairs; the empty class is the manual group, which always goes
// first and only when there is one. It lives here and not inside the tree because it is a
// RULE, not painting: the shell will want the same one when it lists snapshots.
std::vector<std::pair<std::string, std::vector<std::string>>> groupSnapshots(
    const std::vector<std::string>& names);

}  // namespace zfsmgr::base::gsa
