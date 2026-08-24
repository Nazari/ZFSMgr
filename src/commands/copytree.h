#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Copying file trees without rsync.
//
// It exists because rsync is not on Windows, and without it Breakdown, Assemble, ToDir and
// Sync all fall over. The alternative —installing a Cygwin rsync— would bring back the Unix
// layer that was removed along with MSYS2, and its ACLs over NTFS are an approximate
// emulation exactly where it matters most. See
// docs/diseno_tecnico_copia_nativa_sin_rsync.md.
//
// It reproduces what was being asked of `rsync -aHWS [-x] --exclude=/x/`: recursive,
// preserving permissions, timestamps, symlinks, HARD LINKS and sparse files, without
// crossing into another filesystem when asked not to, and with excludes anchored at the
// root of the copy.
namespace zfsmgr::copytree {

struct Options {
    // Do not descend into another filesystem. On Unix the device is compared; on Windows
    // reparse points are not descended into, which is how OpenZFS mounts each dataset.
    bool oneFileSystem = false;
    // TOP-LEVEL names that are not copied. Anchored on purpose: without that, excluding
    // "Tools" would swallow any directory by that name anywhere in the subtree.
    std::vector<std::string> excludes;

    // Skip whatever is already identical at the target, comparing size and timestamp.
    //
    // On by default, which is what rsync did: `rsync -a` does not rewrite what has not
    // changed. Without this, the copy rebuilt the whole tree on every pass —irrelevant
    // against an empty target, but it makes any sync absurd, and it makes Assemble's
    // retries expensive, since those repeat the copy up to five times.
    bool skipUnchanged = true;

    // Delete from the target whatever is not at the source (rsync's `--delete`).
    //
    // Only meaningful when syncing. What is excluded is NOT deleted, same as in rsync:
    // deliberately leaving something out of the copy and then having the delete take it
    // anyway would be the worst of both worlds.
    bool deleteExtraneous = false;

    // Touch nothing: walk and count what would be done. This is what feeds the
    // interface's preview before applying.
    bool dryRun = false;
};

struct Result {
    bool ok = false;
    std::string error;
    std::uint64_t filesCopied = 0;
    std::uint64_t dirsCreated = 0;
    std::uint64_t symlinksCopied = 0;
    // Hard links recreated as such instead of copied again. This is the main difference
    // from robocopy, which would silently duplicate them.
    std::uint64_t hardLinksRecreated = 0;
    std::uint64_t bytesWritten = 0;
    // Already identical at the target.
    std::uint64_t filesSkipped = 0;
    // Were left over at the target and got deleted (or would be, in a dry run).
    std::uint64_t entriesDeleted = 0;
};

// Copies the CONTENTS of srcDir into dstDir, like `rsync src/ dst/`.
Result copyTree(const std::string& srcDir, const std::string& dstDir, const Options& opt);

// Counts what is still MISSING at the target, with the same excludes as the copy.
//
// Equivalent to `rsync -rni --ignore-existing src/ dst/` counting the «>f» lines: it is the
// verification done BEFORE deleting the source in Breakdown, Assemble and ToDir, and the
// only net that keeps data from being lost when a copy came up short.
//
// Returns -1 when it could not be checked, which the caller must treat as «not verified»
// and NOT as «zero pending».
long long countPending(const std::string& srcDir, const std::string& dstDir,
                       const Options& opt);

}  // namespace zfsmgr::copytree
