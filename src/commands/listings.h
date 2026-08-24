#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "json.h"

// What the agent answers, turned into structures.
//
// The three formats that have to be read to show a ZFS tree:
//
//   `--dump-zpool-list`     JSON from `zpool list -j`
//   `--dump-zfs-list-all`   ten-column TSV
//   `--dump-zfs-get-all`    JSON from `zfs get -j all`
//
// They used to live inside the shell, tangled up with the text table that paints them. They
// are RULES —which field is which, and what to do when one is missing—, and another client
// needs the same data wearing a different face. Nothing about presentation is decided here:
// no column order, no units, no translations.
namespace zfsmgr::base::listings {

struct Pool {
    std::string name;
    std::string state;       // ONLINE, DEGRADED…
    std::string health;
    std::string size;
    std::string free;
    std::string used;        // «27%», exactly as zpool gives it
    std::string guid;
};

// From `zpool list -j`.
//
// **Empty output is NOT an error**: with no pools, Linux prints an object with an empty
// «pools» and the macOS OpenZFS —2.4.1— prints nothing at all and exits 0. Treating that as
// unreadable JSON said «unreadable answer» on a machine where the only thing going on is
// that there are no pools yet.
bool pools(const std::string& output, std::vector<Pool>& out, std::string& error);

struct Entry {
    std::string name;
    std::string guid;
    std::string used;
    std::string compression;
    std::string encryption;
    std::string creation;
    std::string referenced;
    std::string mounted;      // «yes», «no», «-»
    std::string mountpoint;
    std::string canmount;

    bool isSnapshot() const { return name.find('@') != std::string::npos; }
};

// From `--dump-zfs-list-all`: TSV with ten columns in this order —name, guid, used,
// compressratio, encryption, creation, referenced, mounted, mountpoint, canmount—.
//
// A line with fewer than ten columns is SKIPPED rather than padded with blanks: a mountpoint
// with a tab inside it would break the split, and losing the row beats showing the fields
// shifted by one.
std::vector<Entry> entries(const std::string& tsvOutput);

struct Property {
    std::string name;
    std::string value;
    // The source, SPELLED THE WAY `zfs get -H -o source` spells it: «local», «default»,
    // «inherited from fc16», «received», «-».
    //
    // The JSON does not give it that way: it brings `{"type":"DEFAULT","data":"-"}`, and
    // keeping `data` —which is what used to happen— leaves everything that comes by default
    // as «-». That «-» means something else: it is the mark of a COMPUTED property, like
    // `used` or `creation`. With both spelled identically there was no way to tell «this can
    // be changed and nobody has» from «this is not changeable», and one client stopped
    // offering to edit `atime`, `quota` and `recordsize` — everything left at its default.
    std::string source;
};

// From `zfs get -j all`. Sorted by name, which is how they get read.
bool properties(const std::string& output, std::vector<Property>& out, std::string& error);

// One entry of a directory's contents, from `--dump-dir-list`.
//
// The daemon walks the directory itself and answers JSON, and only when the path falls
// inside a ZFS mountpoint. Each client used to list it over the shell —and in TWO different
// formats: `ls -lA` on Unix and `Get-ChildItem` with tabs on Windows—, so the same command
// showed different columns depending on the machine.
struct DirectoryEntry {
    std::string name;
    std::uint64_t size{0};
    bool directory{false};
};

// The entries, sorted by name. Unreadable JSON IS an error; an empty list is not: an empty
// directory is a legitimate answer.
bool directoryContents(const std::string& output, std::vector<DirectoryEntry>& out,
                           std::string& error);

// One block device, from `--dump-block-devices`.
//
// `alias` tells apart the entries that are an ALTERNATE NAME —the `by-id` ones— from the
// ones that are the device: the former only carry a path and what they point at. Not an
// ornament: a pool created with `/dev/sdb` breaks if tomorrow the kernel calls that disk
// `sdc`, and with the alias it does not.
struct Device {
    std::string path;
    std::string resolved;   // what an alias points at; empty when it is not one
    std::string type;       // «disk» or «part»
    std::string fs;
    std::string mountpoint;
    std::string parent;
    std::uint64_t size{0};
    bool inUse{false};
    bool alias{false};
};

bool devices(const std::string& jsonOutput, std::vector<Device>& out,
                  std::string& error);

// The MOUNTED datasets, from `--dump-zfs-mount`. The key is the name and the value its real
// mountpoint —the actual one, not the `mountpoint` property—.
bool mounted(const std::string& jsonOutput, std::vector<std::pair<std::string, std::string>>& out,
              std::string& error);

// Is any DESCENDANT of this dataset mounted?
//
// Answered from the mount list `--dump-zfs-mount` already brings, without asking anything
// else. This used to be a script —one for Unix and another for Windows— run over SSH just to
// answer yes or no.
//
// The dataset itself does NOT count: the question is whether unmounting it will drag others
// along.
bool hasMountedDescendants(const std::string& jsonOutput, const std::string& dataset);

// From `zpool get -j all`. The SAME format under a different section: `zfs` hangs its
// objects off «datasets» and `zpool` off «pools». Two functions and not one parameter,
// because the caller knows which one they asked for, and a boolean at the call site does not
// read.
bool poolProperties(const std::string& output, std::vector<Property>& out,
                       std::string& error);

}  // namespace zfsmgr::base::listings
