#pragma once

#include <string>
#include <vector>

// The four actions that move CONTENT between datasets and directories.
//
//   Breakdown   a subdirectory becomes a child dataset that takes its place
//   Assemble    the opposite: a child dataset goes back to being a directory
//   ToDir       the dataset's content is poured into a plain directory
//   FromDir     a directory —perhaps on another machine— is poured INTO the dataset
//
// **What this module does and why it exists.** This is where each one's argv is composed and
// where their rules live. Each client used to assemble it on its own: the shell in
// `cli/shell.cpp`, the server in `web/main.cpp` and the interface in
// `native/mainwindow_advanced*.cpp`. The same command three times, and with it its rules
// three times —or not at all—.
//
// This is not a theoretical worry. The `assemble` rule below was discovered by RUNNING it,
// after the operation reported success having done nothing, and it ended up written in a
// comment in the shell, again in the server's, and solved a third way in the interface. A
// single place to put it was what was missing.
namespace zfsmgr::commands::advanced {

// --- Breakdown ---------------------------------------------------------------
//
// Each pair says: WHICH subdirectory, and WHICH child dataset takes its place.
struct Breakdown {
    std::string subdirectory;  // relative to the dataset's mountpoint
    std::string newDataset;    // relative to the parent dataset
};

// `--mutate-advanced-breakdown <dataset> <subdir> <nuevo> [<subdir> <nuevo>...]`
//
// Empty when there is no usable pair: the verb with nothing but the dataset behind it does
// nothing, and sending it would be asking the daemon to decide something already known here.
std::vector<std::string> argvBreakdown(const std::string& dataset,
                                       const std::vector<Breakdown>& pairs);

// --- Assemble ----------------------------------------------------------------

// A child's FULL name, from whatever the caller wrote.
//
// **This is the rule that takes finding.** The agent checks each child with
// `zfs list <child>`, so a relative name —«photos» instead of «tank/data/photos»— does not
// exist as far as it is concerned. And it did not fail: the operation settled with «already
// absorbed» and **rc=0**, that is, it said yes and had done nothing. It was seen by running,
// not by reading.
//
// A name that already carries a slash is honoured as-is: it may be a grandchild
// («tank/data/photos/2024»), and completing it again would break it.
std::string childWithFullName(const std::string& dataset, const std::string& child);

// `--mutate-advanced-assemble <dataset> <full-child> [<full-child>...]`
//
// The children go through `childWithFullName`. Empty when none is left.
std::vector<std::string> argvAssemble(const std::string& dataset,
                                       const std::vector<std::string>& children);

// --- ToDir -------------------------------------------------------------------
//
// `--mutate-advanced-todir <dataset> <directorio> <0|1>`
//
// The last argument is whether the source dataset is DESTROYED when it finishes. It goes as
// «0» or «1» and not as a named flag because that is how the verb reads it; that it is a
// boolean in this interface and not a string is precisely what stops someone sending «true»
// and destroying, or «no» and destroying too.
std::vector<std::string> argvToDir(const std::string& dataset, const std::string& directory,
                                      bool destroySource);

// Is this path usable as a «ToDir» target?
//
// It has to be absolute. A relative one would be interpreted by the daemon from ITS working
// directory, which is not the one belonging to whoever wrote it: the dump would end up
// somewhere nobody chose.
bool isValidDestinationPath(const std::string& directory);

// --- FromDir -----------------------------------------------------------------
//
// **It is the only one of the four that cannot be an RPC**, and not by oversight: the
// agent's verb reads a tar off standard input, and the RPC channel has no standard input. So
// a pipeline is built with both ends over SSH and the machine of whoever is driving in the
// middle, since that is the one holding both sets of credentials. The daemon says so itself
// in the comment on `runMutateAdvancedFromDir`.
//
// What does belong here are its RULES, which until now lived inside an interface function
// and belonged to nobody else.

// Is this relative subdirectory usable as a target inside the dataset?
//
// Only the daemon checked it, and **after the tar was already running**: by then half the
// content may have left the source machine. Here it is checked before the pipe is opened.
//
// Empty IS valid: it means the root of the dataset.
bool isValidRelativeSubdir(const std::string& rel);

// `--mutate-advanced-fromdir <dataset> [<rel>]`
//
// `rel` is only added when it is not empty: the verb treats it as optional, and sending it
// an empty string behind is asking it to decide what that means.
std::vector<std::string> argvFromDir(const std::string& dataset, const std::string& rel);

// Where a content comes from: the directory and the machine it is on.
struct FromDirSource {
    std::string path;      // exactly as the caller gave it
    std::string machine;   // the name of the connection it comes from
    bool windows{false};   // whether its separators are «\\»
};

// Where each source lands INSIDE the dataset: one relative subdirectory per source, in the
// same order. Empty means the root.
//
// The rule:
//   - a single source  -> its CONTENT goes to the root of the dataset;
//   - several          -> each to a subdirectory named after its directory;
//   - if two collide   -> its machine's name is prefixed.
//
// **And the result is GUARANTEED UNIQUE**, which is what did not hold. Prefixing the machine
// only breaks the tie when the machines differ: two directories called «docs» on the SAME
// connection both produced «fc16-docs», and the second tar extracted on top of the first. In
// an operation whose job is copying, that is losing data without saying so.
//
// The resulting name is also stripped of whatever cannot be a directory name: a connection
// name with a slash inside would have created an extra level, and a «..» would have taken
// the dump outside the dataset —the daemon would have stopped it, but with the tar already
// running—.
std::vector<std::string> destinationSubdirs(const std::vector<FromDirSource>& sources);

// `--mutate-advanced-fromdir-prepare <dataset> [<rel>]`
//
// The FIRST HALF of FromDir: mount, resolve the mountpoint, create the subdirectory and say
// which absolute path it ended up at. Without the tar.
//
// **This is what makes FromDir possible with no shell pipeline.** `--tree-recv-listen`, the
// receiver of the daemon-to-daemon tree, requires the directory to exist already; that is
// why one client only knew how to dump into the root of the dataset. With this in front, the
// tree also works for a subdirectory, and then the data goes machine to machine instead of
// passing through the driver's own computer.
std::vector<std::string> argvFromDirPrepare(const std::string& dataset, const std::string& rel);

// The path that verb answers with: one «DST=<absolute path>» line. Empty when it is absent.
std::string preparedPath(const std::string& output);

// Can this pair do FromDir over the daemon-to-daemon tree, with no pipeline?
//
// BOTH ends need a daemon: the target to prepare and listen, the source to send. The tar
// path only asks for a daemon at the target —SSH is enough at the source—, so this does NOT
// replace it: it takes precedence when it can and leaves the other as the fallback.
//
// The other reason for keeping the fallback is not visible from here: the tree opens an
// ephemeral port at the target and the source connects to it. Wherever there is a firewall
// between the two machines, SSH gets through and this does not.
bool canUseTreeTransfer(bool sourceHasDaemon, bool targetHasDaemon);

// ── File subtrees ────────────────────────────────────────────────────────────

// The paths a `#content/...` names, expanding the brace notation.
//
//     ""                 -> {""}            the whole tree
//     "sub"              -> {"sub"}
//     "{a,b,dir}"        -> {"a", "b", "dir"}
//     "docs/{2024,2025}" -> {"docs/2024", "docs/2025"}
//
// The braces are expanded HERE and not in the URL parser on purpose: turning one URL into
// several would change the contract of `parseZfsmUrl` for all of its users, and only whoever
// works with file trees needs this.
//
// A brace group that is empty, unclosed or nested returns an empty list: it is a typing
// mistake, and guessing what was meant is worse than saying so.
std::vector<std::string> contentPaths(const std::string& path);

// Does this relative path stay INSIDE the tree it hangs from?
//
// Empty does: it is the root. Absolute does not, and neither does one with `..` —not at the
// start nor in the middle—: `sub/../../etc` leaves the mountpoint, and what runs it is a
// process running as root. The daemon does not check this for rsync: it only requires the
// path to be absolute.
bool isValidContentPath(const std::string& path);

}  // namespace zfsmgr::commands::advanced
