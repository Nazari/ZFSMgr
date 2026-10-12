#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// SCHEDULED snapshots (GSA): what they are and what is valid.
//
// A dataset's schedule does not live in a file belonging to this program: it is made of USER
// PROPERTIES on the dataset itself, prefixed `org.fc16.gsa:`. What runs them is a thread of
// the agent of that machine (`runGsaSchedulerThread` in `daemon_main.cpp`), every five
// minutes; the shell only reads and writes properties, and reads the scheduler's state.
//
// The decisions the scheduler takes —what to send, what to prune on each side, what the
// state of every schedule means— are pure functions here, with tests, and not buried in the
// daemon: see `planLevel`, `planPrune` and `summarize`.
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
    LegacyTargetFormat,      // detail: the URL it should have been written as
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

// The target as the user writes it: ONLY `zfsm://<connection>/<pool>/<dataset>`. The old
// «Connection::Pool/Dataset» is deprecated on input (`LegacyTargetFormat`, with the URL it
// should have been in `detail`); it is still what is STORED, and the daemon still reads it.
// A connection alone, a snapshot, a section or a pattern are `MalformedTarget`.
bool targetFromUrl(const std::string& text, std::string& stored, Reason& why);

// ── Levelling: what to send and what to change on the target ─────────────────
//
// A snapshot of a dataset: its short name (after the «@») and its guid. The guid is what
// identifies it on both sides: `zfs send`/`recv` keep it and `zfs rename` does not change it,
// so the common base is found even after a snapshot was renamed on one side only.
struct SnapRef {
    std::string name;
    std::string guid;
};

struct LevelPlan {
    enum class Kind {
        Full,          // the target does not exist: everything is sent
        Incremental,   // `send -I @base` up to the newest of the source
        UpToDate,      // the target already has the newest of the source
        Conflict,      // sending would destroy something on the target: nothing is sent
    };
    Kind kind{Kind::Full};
    std::string base;   // source name of the newest common snapshot
    // Common snapshots whose name differs: (name on the target, name it must take).
    std::vector<std::pair<std::string, std::string>> renameOnTarget;
    // GSA snapshots of the target, older than the base, that the source no longer has.
    std::vector<std::string> pruneOnTarget;
    // Why it is a conflict: the target snapshots that `recv -F` would destroy, or empty when
    // the target exists and shares nothing with the source.
    std::vector<std::string> conflicting;
};

// Both lists oldest first, as `zfs list -s creation` gives them. `targetExists` tells «no
// snapshots» from «no dataset». Only `GSA-` snapshots are ever pruned on the target, and
// never the base.
LevelPlan planLevel(const std::vector<SnapRef>& source, const std::vector<SnapRef>& target,
                    bool targetExists);

// ── Pruning one class ────────────────────────────────────────────────────────

struct PrunePlan {
    std::vector<std::string> destroy;   // short names, oldest first
    bool keptBase{false};   // the protected snapshot was beyond the retention and was kept
};

// The `GSA-<klass>-<stamp>` among `names` (short names) beyond the newest `keep`, ordered by
// the stamp in the NAME, not by `creation`. `protectedName` —the base of the levelling— is
// never destroyed: with the target down for longer than the retention, destroying it would
// break the incremental chain for good.
PrunePlan planPrune(const std::vector<std::string>& names, const std::string& klass, int keep,
                    const std::string& protectedName);

// The newest `GSA-<klass>-…` among `names`, by the stamp in the name. Empty if there is none.
std::string newestOfClass(const std::vector<std::string>& names, const std::string& klass);

// ── Whether to level now ─────────────────────────────────────────────────────
//
// The levelling runs as a JOB of the daemon, apart from the pass: the pass takes the
// snapshots and prunes, and this decides whether a levelling job has to be launched.
struct LevelTrigger {
    bool levelOn{false};
    bool jobActive{false};          // a levelling job for this dataset is queued or running
    bool newSnapshot{false};        // this pass took or renamed a snapshot
    bool prunedSource{false};       // this pass destroyed snapshots at the source
    bool forced{false};             // `schedule run`
    bool hasPrevious{false};        // there has been a levelling before
    bool previousOk{false};
    bool previousCancelled{false};  // the previous one was cancelled by the user
};

enum class LevelAction {
    Nothing,
    Wait,    // one is already queued or running: never two for the same dataset
    Start,
};

// - Never a second job for the same dataset.
// - A new snapshot, a pruned source (the target is pruned at the same time), the first time
//   or `schedule run`: start.
// - A failed one is retried in the next pass, without waiting for the next period.
// - A CANCELLED one is not: the user stopped it, and restarting it five minutes later would
//   make cancelling pointless. It waits for a new snapshot or `schedule run`.
LevelAction decideLevel(const LevelTrigger& t);

// ── State of the schedules, as the daemon reports it (`--dump-gsa-status`) ───

struct DatasetState {
    std::string dataset;
    std::string destination;   // stored form; empty if it does not level
    bool level{false};
    bool pending{false};       // scheduled, but no pass has looked at it yet
    long long lastEval{0};
    std::string lastSnapshot;
    long long lastSnapshotTime{0};
    long long lastLevelTime{0};
    bool levelOk{true};
    bool hasLevel{false};      // there has been at least one levelling
    // The first thing that went wrong, in this order: levelling, snapshot, pruning,
    // pruning of the target. Empty when everything is green.
    std::string failedStep;    // «level», «snapshot», «prune», «target-prune»
    std::string error;
    long long errorTime{0};
    bool levelCancelled{false};   // the last levelling was cancelled by the user
    // The levelling job in progress, if there is one. Being in progress is not an error.
    std::string levelJob;         // its id
    std::string levelJobState;    // «queued» or «running»
    unsigned long long levelJobBytes{0};
};

struct ConnectionState {
    std::string connection;
    bool checked{false};
    std::string whyNotChecked;
    bool stale{false};         // there are schedules and the scheduler has not passed lately
    long long lastPass{0};
    long long now{0};          // the daemon's clock when it answered
    std::vector<DatasetState> datasets;
};

// The daemon's JSON → structure. `stale` is decided here, with the daemon's own clock, so a
// skewed client clock cannot make it lie: more than `staleAfterSecs` since the last pass with
// schedules enabled.
bool parseState(const std::string& json, ConnectionState& out, std::string& error,
                long long staleAfterSecs = 900);

enum class Verdict { NoSchedules, AllGreen, WithErrors };

// What the CLI says when it starts. `bad` gets the datasets in red; `unchecked`, the
// connections that could not be asked. A stale scheduler counts as an error of every
// dataset of that connection.
Verdict summarize(const std::vector<ConnectionState>& all,
                  std::vector<std::pair<std::string, DatasetState>>& bad,
                  std::vector<const ConnectionState*>& unchecked);

// How many levellings are queued or running among `all`: the start-up line mentions them.
int levellingsInProgress(const std::vector<ConnectionState>& all);

}  // namespace zfsmgr::base::gsa
