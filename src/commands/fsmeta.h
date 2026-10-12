#pragma once

#include <map>
#include <string>

// The metadata of a file that is NOT owner, mode or time: extended attributes and ACLs.
//
// Shared by the copy within one machine (`copytree`) and the copy between machines
// (`remotetree`), so both preserve exactly the same thing. Each platform keeps it differently:
//
//   - Linux: everything is an extended attribute, POSIX ACLs included
//     (`system.posix_acl_access` / `system.posix_acl_default`). No libacl needed.
//   - FreeBSD: `extattr` in two namespaces, written here as «user.» and «system.» so the map
//     looks the same; ZFS uses NFSv4 ACLs, which are NOT attributes.
//   - macOS: a flat attribute namespace, and «extended» ACLs, which are not attributes either.
//   - Windows: none of this here; the copy within one machine copies the DACL on its own.
//
// Between machines of DIFFERENT families not everything fits —a Linux POSIX ACL means nothing
// on FreeBSD—. `fitsIn` says what the other end can hold, so only that travels and the rest is
// counted and reported instead of being lost in silence.
namespace zfsmgr::fsmeta {

enum class Family { Linux, FreeBSD, Darwin, Windows, Other };

Family localFamily();
const char* nameOf(Family f);
Family familyFromName(const std::string& name);

using Xattrs = std::map<std::string, std::string>;

// A non-attribute ACL. `kind` is "nfs4" (FreeBSD) or "darwin" (macOS); empty means none —
// FreeBSD's TRIVIAL ACL, which is only the mode written another way, counts as none.
struct Acl {
    std::string kind;
    std::string text;
    bool operator==(const Acl& o) const { return kind == o.kind && text == o.text; }
    bool operator!=(const Acl& o) const { return !(*this == o); }
};

struct Metadata {
    Xattrs xattrs;
    Acl acl;
    bool empty() const { return xattrs.empty() && acl.kind.empty(); }
    bool operator==(const Metadata& o) const { return xattrs == o.xattrs && acl == o.acl; }
    bool operator!=(const Metadata& o) const { return !(*this == o); }
};

// Attributes that belong to the machine and not to the file: the SELinux label (the target's
// policy sets it) and the provenance macOS stamps on everything. Neither copied nor deleted.
bool isLocalOnly(const std::string& name);

// What a path carries, without following a symbolic link. Local-only names are left out.
Metadata read(const std::string& path);

// Leaves on `path` exactly the attributes in `want` (those it carries and are not in `want`
// are removed: an ACL withdrawn at the source must not keep granting access at the copy).
// Returns how many could NOT be set or removed.
int syncXattrs(const std::string& path, const Xattrs& want);

// Puts the ACL on `path`; an empty one removes a non-trivial ACL it carries. On FreeBSD and
// macOS this goes AFTER `chmod`: there it is changing the mode that rewrites the ACL.
// False if it could not.
bool setAcl(const std::string& path, const Acl& acl);

// Whether an attribute name / an ACL can be kept by a machine of family `f`.
bool xattrFits(const std::string& name, Family f);
bool aclFits(const Acl& acl, Family f);
// The part of `m` that a machine of family `f` can hold, coming from a machine of the local
// family (between two of the same family, everything). `lost` (optional) says whether
// something was left out.
Metadata fitFor(const Metadata& m, Family f, bool* lost = nullptr);

// A short digest of the metadata, to compare two ends without sending the values. Empty for
// empty metadata.
std::string digestOf(const Metadata& m);

// The wire form, and back. Lengths are explicit: names and values are arbitrary bytes.
std::string serialize(const Metadata& m);
bool parse(const std::string& data, Metadata& out, std::string& error);

}  // namespace zfsmgr::fsmeta
