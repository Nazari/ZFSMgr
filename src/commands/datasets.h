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
// —a message that does not say what to do about it—.
std::string renamedName(const std::string& current, const std::string& wanted);

// `zfs rename <current> <full-new-name>`
std::vector<std::string> argvRename(const std::string& current, const std::string& wanted);

// The name of a child: `<parent>/<leaf>`. If the leaf already carries a slash, it is honoured.
std::string childName(const std::string& parent, const std::string& leaf);

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
