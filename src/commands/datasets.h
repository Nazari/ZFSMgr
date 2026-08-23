#pragma once

#include <string>
#include <vector>

// Datasets: create, rename, mount, promote and set properties.
//
// All of this travels over `--mutate-zfs-generic`, so what is composed here is the argv for
// `zfs` WITHOUT the program name: `{"rename", old, new}`.
namespace zfsmgr::commands::datasets {

// Is this a usable dataset name?
//
// ZFS accepts letters, digits and `_-.:/ `; an at sign is NOT allowed —that would make a
// snapshot— and neither is a space. This check used to be hand-written, and differently, in
// every client.
bool isValidName(const std::string& name);

// The full name to rename to, derived from whatever the caller typed.
//
// **The rule**: a name WITHOUT a slash means «change its leaf, leave it where it is», so the
// current parent is prepended. A name with a slash is taken as-is, which is how a dataset is
// moved elsewhere.
//
// Without this, typing «photos» to rename `tank/media/movies` sends `zfs rename
// tank/media/movies photos`, and ZFS answers «cannot create 'photos': missing dataset name»
// —a message that does not say what to do about it—. The shell already applied the rule; the
// web server did not, so the same product behaved differently depending on the way in.
std::string renamedName(const std::string& current, const std::string& wanted);

// `zfs rename <current> <full-new-name>`
std::vector<std::string> argvRename(const std::string& current, const std::string& wanted);

// `zfs create [-p] [-o p=v...] <dataset>`
//
// `parents` adds `-p`: create the missing intermediate datasets. Without it, `zfs create
// a/b/c` fails when `a/b` does not exist.
std::vector<std::string> argvCreate(const std::string& dataset,
                                   const std::vector<std::string>& properties = {},
                                   bool parents = false);

// The name of a child: `<parent>/<leaf>`. If the leaf already carries a slash, it is honoured.
std::string childName(const std::string& parent, const std::string& leaf);

// `zfs promote <dataset>`
//
// Only does something when the dataset is a CLONE: promoting it flips the relationship with
// its origin. On one that is not, ZFS answers «not a cloned filesystem» —correct, but the
// client can say it sooner and better—.
std::vector<std::string> argvPromote(const std::string& dataset);

// `zfs mount [-f] <dataset>` and `zfs unmount [-f] <dataset>`
std::vector<std::string> argvMount(const std::string& dataset, bool force = false);
std::vector<std::string> argvUnmount(const std::string& dataset, bool force = false);

// `zfs set <prop>=<value> <dataset>`
//
// Empty when the property has no name. The VALUE may well be empty: some properties are
// deliberately blanked out.
std::vector<std::string> argvSetProperty(const std::string& dataset, const std::string& property,
                                            const std::string& value);

// `zfs inherit <prop> <dataset>`: hand a property back to whatever it inherits from its parent.
std::vector<std::string> argvInheritProperty(const std::string& dataset,
                                              const std::string& property);

}  // namespace zfsmgr::commands::datasets
