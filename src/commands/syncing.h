#pragma once

#include <string>
#include <utility>
#include <vector>

// Syncing two datasets.
//
// **This has nothing to do with `zfs send`.** Syncing works at the FILE level, over the
// mountpoints of the two endpoints: it compares, copies what changed and —when asked to—
// deletes at the target whatever is no longer at the source. That is why it needs both to
// be mounted, and why it can destroy work at the target, which sending or levelling never
// do.
//
// The design document grouped it with the others as if it shared their path. It does not:
// `zfs send` ships blocks of a shared history, and this compares directory trees. That is
// why it is «the one that shares the least».
//
// What lives here is the RULE —when it can be done and why not— and the typed payload of
// the `--mutate-rsync-local` verb, which used to sit inside the main window
// (`mainwindow.cpp:1963`) and which any other client needs just the same.
namespace zfsmgr::base::syncing {

enum class Failure {
    None_,
    SameObject,
    SourceIsNotDataset,
    TargetIsNotDataset,
    WindowsEndpoint,
    DifferentMachine,
    SourceNotMounted,
    TargetNotMounted,
    UnusablePath,
    NoDaemon,
};

struct Endpoint {
    std::string connection;
    std::string object;         // the dataset; one with an «@» in it is not valid
    bool mounted{false};
    std::string mountpoint;
    bool isWindows{false};
    bool hasDaemon{false};
};

struct Plan {
    std::string sourcePath;
    std::string targetPath;
    Failure failure{Failure::None_};
    bool ok() const { return failure == Failure::None_; }
};

// What can be decided WITHOUT asking anyone: same machine, both are datasets, no Windows
// endpoint, daemon up.
//
// It is kept apart from `makePlan` because the source's mountpoint costs a call to the
// agent, and whoever paints the actions menu paints it for every dataset that gets looked
// at. With a single function, offering the action cost one call per repaint; this way the
// repaint is free and the call happens once, on click.
Failure check(const Endpoint& source, const Endpoint& target);

// The whole check, mountpoints included. Returns both paths.
//
// The mountpoints are THE fact: without them there is nothing to compare. A dataset with
// `canmount=off`, or mounted where there is no absolute path, is not synced through here
// even though it exists.
Plan makePlan(const Endpoint& source, const Endpoint& target);

std::string labelOf(Failure f);

// Is this path usable for syncing?
//
// On Unix, an absolute path. **On Windows, one with a drive letter** —«Z:/sa/»—, which is
// what can actually be opened there: the `mountpoint` property of a dataset on Windows says
// «/winpool/sa», and that path DOES NOT EXIST as far as the system is concerned. Verified
// live: `Test-Path` calls it false, and the good one comes from `zfs mount`.
//
// It is kept apart because it is the same check the Qt interface makes
// (`isUsableMountPath`), and having it twice means having it wrong in one of the two.
bool isUsablePath(const std::string& path, bool isWindows = false);

// The payload of `--mutate-rsync-local`: base64 of a JSON
// `[delete, dryRun, rsh, targetHost, source1, target1, ...]`.
//
// Empty when any pair is unusable. The paths have to be absolute: the daemon rejects the
// ones that do not start with a slash, so letting them through here only changes where it
// fails.
std::string rsyncPayload(const std::vector<std::pair<std::string, std::string>>& pairs,
                       bool remove, bool dryRun,
                       const std::string& rsh, const std::string& targetHost);

}  // namespace zfsmgr::base::syncing
