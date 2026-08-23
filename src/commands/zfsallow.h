#pragma once

#include <string>
#include <vector>

// A dataset's DELEGATED permissions: reading what `zfs allow` says and composing the
// commands that change it.
//
// The output format of `zfs allow` is one of those that look easy until you look: titled
// sections, tab indentation, and the SCOPE —local, descendants, both, on-create— lives in
// the section title and not in the line. Missing that detail means granting to the
// descendants what was meant to be granted here only.
//
// It lives in the base layer because it is a formatting rule, not an interface one, and
// because more than one client needs it.
namespace zfsmgr::base::zfsallow {

// Where it applies. This is what tells the output sections apart, and what decides which
// flag the command carries.
enum class Scope {
    Local,                 // -l : this dataset only
    Descendants,           // -d : the ones below only
    LocalAndDescendants,   //      no flag: both
    OnCreate,              // -c : what whoever creates a descendant receives
    Set,              // -s : un @conjunto de permisos con nombre
};

// To whom.
enum class Who {
    User,     // -u
    Group,       // -g
    Everyone,    // -e
    Set,         // the @name itself, in the sets section
};

struct Entry {
    Scope scope{Scope::LocalAndDescendants};
    Who who{Who::User};
    std::string name;                       // empty when `who` is Everyone
    std::vector<std::string> permissions;   // «create», «mount», «@basic»…
};

const char* keyOf(Scope a);

// The exact TITLE `zfs allow` writes for that section, and the exact word it uses for the
// grantee. These are not for reading: they are for machine output —the shell's tsv and
// json— which already used those texts and which a script may be comparing against.
// Replacing those strings with prettier ones would break the script silently.
const char* zfsSectionTitle(Scope a);
const char* zfsToken(Who q);
const char* keyOf(Who q);
std::string labelOf(Scope a);
std::string labelOf(Who q);
Scope scopeFrom(const std::string& key);
Who whoFrom(const std::string& key);

// What `zfs allow <dataset>` says, turned into entries. Empty output —a dataset with nothing
// delegated— returns an empty list and is NOT an error.
std::vector<Entry> parse(const std::string& output);

// The argv for `zfs allow` and for `zfs unallow` for one entry, with the dataset last.
//
// An argv and not a string, deliberately: what runs it is the daemon with `execvp`, and a
// string would have to be split again —and a user name with a space in it would break the
// split.
std::vector<std::string> argvAllow(const Entry& e, const std::string& dataset);
std::vector<std::string> argvUnallow(const Entry& e, const std::string& dataset);

}  // namespace zfsmgr::base::zfsallow
