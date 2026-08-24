#pragma once

#include <string>
#include <vector>

// Snapshots and what is done with them: create, destroy, roll back, clone and hold.
//
// As everywhere else in `commands/`, what lives here is the argv and the rules; who talks to
// the agent is the client's business.
namespace zfsmgr::commands::snapshots {

// The scope of a destroy or a rollback. An enum and not a string, deliberately: the daemon
// verb receives it as «R», «r» or empty, and those three letters are indistinguishable at a
// glance in a call. With names, whoever reads the line knows what it takes down with it.
enum class Scope {
    Only,         // the named object only
    Descendants,  // -r: its children
    Dependents,   // -R: its children AND whatever depends on them (clones)
};

// The letter the daemon verb expects.
std::string scopeFlag(Scope s);

// Does it take down more than the named object? Used to decide how loudly to warn.
bool reachesBeyondTarget(Scope s);

// `--mutate-zfs-snapshot <dataset@name> <0|1>`
//
// The last argument is whether it pulls in the descendants. It goes as «0» or «1» and the
// full name is composed here: taking it pre-assembled invited one client to send the dataset
// and another to send the bare name.
//
// Empty when the name is unusable —see `isValidName`—, because ZFS accepts very little there
// and the error it returns does not say which of the two halves is wrong.
std::vector<std::string> argvCreateSnapshot(const std::string& dataset,
                                              const std::string& name, bool recursive);

// `--mutate-zfs-destroy <object> <0|1> <scope>`
//
// Works for datasets AND for snapshots: the verb looks for the «@» in the name.
std::vector<std::string> argvDestroy(const std::string& object, bool force,
                                      Scope scope = Scope::Only);

// `--mutate-zfs-rollback <snapshot> <0|1> <scope>`
//
// **Rollback DISCARDS everything written after that snapshot.** There is no useful «Solo»
// scope here when later snapshots exist: ZFS refuses until it is told it may take those too,
// so a caller that does not pass `Descendants` will see a ZFS error instead of a question.
// The argv is returned all the same: deciding whether to ask is the client's call.
std::vector<std::string> argvRollback(const std::string& snapshot, bool force,
                                      Scope scope = Scope::Only);

// `--mutate-zfs-clone <source-snapshot> <new-dataset>`
//
// Empty when the source is not a snapshot: cloning a dataset does not exist in ZFS, and
// sending it returns a message about something else entirely.
std::vector<std::string> argvClone(const std::string& sourceSnapshot,
                                    const std::string& newDataset);

// The same one, shaped as a `zfs` argv for the generic path: `zfs clone [flags] <source>
// <new>`.
//
// It exists for the same reason as its twin among the holds: the daemon's typed verb takes
// exactly two arguments and accepts no flags, and there are screens that offer `-p`, `-u`
// and properties. The checks are the same in both shapes.
std::vector<std::string> argvZfsClone(const std::string& sourceSnapshot,
                                       const std::string& newDataset,
                                       const std::vector<std::string>& flags = {});

// `--mutate-zfs-hold <tag> <snapshot>` and its opposite.
//
// **The tag goes FIRST**, which is the reverse of how one says it out loud («hold this
// snapshot with this tag»). Swapping them raises no error: `zfs hold` accepts any pair of
// strings and fails later saying it cannot find the snapshot «mycopy».
//
// **These typed verbs do NOT take `-r`**: the daemon reads exactly two parameters and hands
// them to `zfs`. To hold a whole tree there is the pair below, which goes through the generic
// verb. This is not an oversight in this module: the typed verb is deliberately narrower, and
// that is reflected here rather than papered over with a parameter that would be ignored.
std::vector<std::string> argvHold(const std::string& tag, const std::string& snapshot);
std::vector<std::string> argvRelease(const std::string& tag, const std::string& snapshot);

// The same ones, shaped as a `zfs` argv for `--mutate-zfs-generic`, the only path that takes
// the recursive form: `zfs hold [-r] <tag> <snapshot>`.
std::vector<std::string> argvZfsHold(const std::string& etiqueta,
                                        const std::string& snapshot, bool recursive);
std::vector<std::string> argvZfsRelease(const std::string& etiqueta,
                                       const std::string& snapshot, bool recursive);

// Is this hold tag usable?
//
// It travels all the way into a `zfs` argv, and a space or an at sign inside would turn the
// command into a different one. Nor a slash: that would make it look like a dataset name.
bool isValidTag(const std::string& tag);

// Is this a snapshot? The at sign is what decides, and that check used to be hand-written in
// every client.
bool isSnapshot(const std::string& object);

// The full name of a new snapshot on a dataset: `<dataset>@<name>`.
//
// If the caller already brings the at sign, whatever they wrote is honoured.
std::string snapshotName(const std::string& dataset, const std::string& name);

}  // namespace zfsmgr::commands::snapshots
