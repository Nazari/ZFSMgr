#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

// Pool maintenance operations: `zpool <op> [flags] <pool> [disks]`.
//
// **What is kept here is not the argv —that part is trivial— but three rules learned by
// watching them fail.**
//
// 1. `stop` and `pause` are NOT the same letter for all of them. On `scrub` they are `-s`
//    and `-p`; on `trim` and `initialize` they are `-c` and `-s`. Which means **`-s` means
//    «stop» on scrub and «suspend» on initialize**. A client that uses the same letter for
//    all three ends up with a button that says one thing and does another —it happened:
//    «Stop initialize» sent `-s`, which suspends—.
// 2. The order zpool wants is FLAGS, then the pool, then the disks. Both halves come from
//    seeing it fail: with the disks first, `trim <pool> <disk>` answered «invalid character
//    '/' in pool name»; with the flags after the pool, zpool ignores them SILENTLY —`trim
//    -r notarate` said «started» and the history recorded a bare `zpool trim <pool>`—.
//    Accepted-and-not-applied is the worse of the two ways to fail.
// 3. Which ones need confirming, and it is not just «the ones that destroy»: `clear` erases
//    no data but it does erase the pool's ERROR COUNTS, and it gets typed by someone meaning
//    to clear the terminal —which happened twice in a single testing session—. Losing that
//    without asking for it is losing exactly what one was looking at.
namespace zfsmgr::commands::pools {

enum class Operation {
    Scrub,
    Trim,
    Initialize,
    Clear,
    Sync,       // `zpool sync`, what the interface calls «Flush»
    Export,
    Import,
    Destroy,
    Upgrade,
    Reguid,
};

enum class Phase {
    Start,
    Stop,     // scrub: -s   trim/initialize: -c
    Pause,    // scrub: -p   trim/initialize: -s
};

// The subcommand exactly as `zpool` expects it.
const char* subcommand(Operation op);

// Does it take stop and pause? Only the three that are long-running processes.
bool acceptsPhase(Operation op);

// Must it be confirmed first? See rule 3 above.
bool needsConfirmation(Operation op);

// Can it not be undone? A subset of the ones that get confirmed, and it exists so whoever
// asks can use the right words instead of a generic «are you sure?».
bool isIrreversible(Operation op);

// `zpool <sub> [phase] [flags] <pool> [disks]`, in that order and for the reason in rule 2.
//
// Empty when the pool name is unusable, or when a phase is asked of an operation that does
// not take one: `zpool export -s` is not «stop the export», it is a syntax error, and it is
// worth more not to send it than to translate zpool's message afterwards.
std::vector<std::string> argv(Operation op, const std::string& pool, Phase phase = Phase::Start,
                              const std::vector<std::string>& flags = {},
                              const std::vector<std::string>& disks = {});

bool isValidPoolName(const std::string& name);

// --- `create <pool> … --opts-as zfsm://con/pool`: the options of another pool.
//
// What is copied is what was CHOSEN for that pool: the properties whose source is «local».
// Not everything with that source, though:
// - `feature@*` show as local on every pool; the set of features follows `compatibility`.
// - `altroot`, `cachefile` and `readonly` are how the pool was IMPORTED, not created.
// - On the root dataset, `mountpoint` would mount the new pool over the old one —the very
//   accident this program has paid for before—; the encryption ones need a key the creation
//   cannot ask for; and user properties (with «:») include the snapshot schedule, which
//   belongs to that pool's data and not to a new one.
// - A property set to the value it has by default anyway adds nothing, and is left out
//   without a word: `-o failmode=wait` creates the same pool as without it.
// - A root property the target platform does not have (`acltype` on FreeBSD…) is left out:
//   zpool would refuse the whole creation for it.
struct SourcedProperty {
    std::string name;
    std::string value;
    std::string source;  // as zfs/zpool give it: local, default, -…
};
struct CopiedOptions {
    std::vector<std::pair<std::string, std::string>> pool;    // for -o
    std::vector<std::pair<std::string, std::string>> rootFs;  // for -O
    std::vector<std::string> skipped;                         // «name=value», left out
};
CopiedOptions optionsToCopy(const std::vector<SourcedProperty>& poolProps,
                            const std::vector<SourcedProperty>& rootProps,
                            const std::function<bool(const std::string&)>& rootSupported);

// The line as typed, without `--opts-as <url>`, and with the copied options appended as
// `-o p=v` / `-O p=v`. One the user already wrote wins: it is not repeated. This is the line
// the shell offers to run instead, so what will be created is seen before it is.
std::string rewriteWithOptions(const std::string& line, const CopiedOptions& opts,
                               const std::vector<std::pair<std::string, std::string>>& given);

}  // namespace zfsmgr::commands::pools
