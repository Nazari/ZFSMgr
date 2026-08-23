#pragma once

#include <map>
#include <string>
#include <vector>

// The catalogue of ZFS properties this program knows about.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md: there is no Qt here, so the interface and the
// shell use it alike.
namespace zfsmgr::base::zfsprops {

// The properties whose value comes from a CLOSED list, and that list. Empty for the ones
// that have none —`quota`, `mountpoint`—, which is not the same as «no such property».
const std::map<std::string, std::vector<std::string>>& propertiesWithValues();

// The possible values of a property, or empty when it has no closed list.
const std::vector<std::string>& valuesOf(const std::string& property);

// The OS family of the machine the dataset lives on. It matters because there are
// properties that exist on one only: `jailed` is FreeBSD's, `zoned` is Linux's.
enum class Platform {
    Linux,
    MacOs,
    FreeBsd,
    Windows,
    Other,
};

// From what is known about the machine —the type declared in the profile and the `uname`
// line— to a family. It looks at both together: the profile may come in unfilled.
Platform platformOf(const std::string& osType, const std::string& osLine);

// User properties carry a «:» in the name. They can always be written: ZFS does not
// interpret them, and this program keeps its schedule in there (`org.fc16.gsa:*`).
bool isUserProperty(const std::string& prop);

// Does that property exist on that platform? Offering `jailed` on Linux is offering an error.
bool isSupportedOn(const std::string& prop, Platform p);

// Can that property's value be changed by TYPING OVER it?
//
// It takes more than the name: `source` tells a real property from a computed one —the «-»
// marks those—, `readonly` is what ZFS itself says, and the type separates a filesystem
// from a volume; nothing is changed on a snapshot.
//
// **This was duplicated LETTER FOR LETTER in `mainwindow_dataset_props.cpp` and in
// `mainwindow_dataset_tree.cpp`**, both with Qt inside. It is not an interface rule: it is
// what ZFS allows, and any other client needs exactly the same one to know which cell to
// paint with an edit box and which not.
bool isInlineEditable(const std::string& prop, const std::string& datasetType,
                     const std::string& source, const std::string& readonly, Platform p);

// One `zfs send` flag, exactly as the user writes it.
struct SendFlag {
    const char* form;       // "-w"
    bool value{false};      // does it carry a value behind it? («-X <dataset>»)
    const char* key{""};    // the translation key for `what`
    const char* what{""};   // what it does, in one line, for the help
};

// The `zfs send` flags that are allowed to reach the command.
//
// The list lives HERE and not in the shell because the one that has to enforce it is the
// daemon: it is the daemon that builds the `zfs send` argv and runs it with privileges. The
// shell is just another client, and validating only in the client validates nothing.
//
// Deliberately out: `-i` and `-I`, which the program sets from `--base`, and `-t`, the
// resume token. If the user could write those, they could also name ANOTHER dataset in the
// value and pull something out over the socket that nobody asked for.
const std::vector<SendFlag>& sendFlagCatalog();

// Checks a whole string of flags —«-w -L»— against that list. When something is not there,
// it returns false and leaves the offending token in `bad`.
bool areValidSendFlags(const std::string& text, std::string& bad);

}  // namespace zfsmgr::base::zfsprops
