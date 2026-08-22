#pragma once

#include <string>
#include <vector>

// `zfsm://` — naming any element of the tree.
//
//     zfsm://<connection>/<pool>/<dataset>[@snapshot][#<section>[/<detail>]]
//
// **The rule, in one sentence:** before `#` comes the ZFS object; after `#`, the path
// INSIDE that object, using the same names shown in the tree.
//
// Almost nothing here is invented, and that is deliberate: a ZFS name is already a
// `/`-separated path, a snapshot already uses `@`, and a fragment already means "a part
// of this" in any URL. The only decisions are that **the connection is the authority**
// —the "where", which is what an authority is for— and that the first path segment is
// the pool.
//
//     zfsm://unibody                                    the connection
//     zfsm://unibody#daemon                             its Daemon tab
//     zfsm://unibody/sback                              the pool, which IS ALSO a dataset
//     zfsm://unibody/sback@before                       its snapshot
//     zfsm://unibody/sback/user                         a dataset
//     zfsm://unibody/sback/user@yesterday               a snapshot
//     zfsm://unibody/sback/user#properties/compression  a property
//     zfsm://unibody/sback/user#content/docs/a.pdf      a file inside
//
// **For now it only NAMES.** Resolving —going to fetch what it names, opening it in the
// tree, accepting it from the system command line— comes later and is built on top
// without changing any of this: that is why parsing returns a structure and not a
// half-split string.
//
// **This API is in English, unlike the rest of this layer.** `zfsm://` is a PUBLIC
// interface: the CLI uses it, integrators will use it, and it will end up documented
// outside. The rest of `base/` is internal and stays in Spanish, the project's language.
//
// See docs/diseno_tecnico_url_zfsm.md.
namespace zfsmgr::base {

// What the URL names.
//
// **There is no separate "pool" kind, and that is not an oversight:** in ZFS a pool IS a
// dataset —`zfs list sback` returns it, and `zfs snapshot sback@before` works—. Having it
// as a distinct kind was a lie in the model, and it also made it impossible to name a
// pool's snapshot. To tell whether a dataset is its pool's root, use `isPoolRoot()`.
enum class ZfsmKind {
    Invalid,
    Connection,  // zfsm://unibody
    Dataset,    // zfsm://unibody/sback  y  zfsm://unibody/sback/user
    Snapshot,   // zfsm://unibody/sback@antes  y  zfsm://unibody/sback/user@ayer
};

// Section names the application knows today.
//
// **In English, even though the tree may be shown in Spanish or Chinese.** A URL is an
// identifier, not text to read: if the literal depended on the language of whoever wrote
// it, the same thing would have three names and none would be usable for storing or
// comparing.
//
// Other names are accepted: an unknown section is valid and whoever resolves it decides
// what to do. Rejecting them would mean touching this file every time the tree gains a
// tab.
namespace zfsmSection {
constexpr const char* kContent = "content";
constexpr const char* kProperties = "properties";
constexpr const char* kPermissions = "permissions";
constexpr const char* kInfo = "info";
constexpr const char* kDaemon = "daemon";
}  // namespace zfsmSection

struct ZfsmUrl {
    ZfsmKind kind{ZfsmKind::Invalid};

    // The connection, exactly as written. It is its id or its name; who resolves that is
    // not this layer's business.
    std::string connection;

    // The pool: the first path segment. Empty if the URL only names the connection.
    std::string pool;

    // The FULL ZFS name, pool included: "sback/user/docs". This is what gets passed to
    // `zfs`, so it is stored already assembled rather than in pieces.
    std::string dataset;

    // Without the `@`. Empty if this is not a snapshot.
    std::string snapshot;

    // Lowercased, without the `#`. Empty if there is no fragment.
    std::string section;

    // What follows the section, already decoded and split:
    // `#content/docs/a.pdf` -> {"docs", "a.pdf"}; `#properties/compression` ->
    // {"compression"}.
    std::vector<std::string> detail;

    bool isValid() const { return kind != ZfsmKind::Invalid; }

    // Is the named dataset its pool's root? This replaces the former "pool" kind without
    // pretending it is anything other than a dataset.
    bool isPoolRoot() const { return !dataset.empty() && dataset == pool; }

    // `dataset@snapshot`, the way ZFS writes it. Without a snapshot, just the dataset.
    std::string zfsName() const;
};

// Parses. Returns false and explains in `error` what is missing or extra.
//
// Strict about anything that could hide a mistake —wrong scheme, empty connection, two
// `@`— and lenient about anything that could not —an unknown section, a trailing slash—.
bool parseZfsmUrl(const std::string& texto, ZfsmUrl& out, std::string& error);

// The way back: rebuilds the text, encoding whatever needs it. `parse(format(x)) == x`
// for every valid URL, which is what keeps the two halves from drifting apart.
std::string formatZfsmUrl(const ZfsmUrl& u);

// Percent-encoding of one segment, per RFC 3986. Exposed because anyone building a URL by
// hand needs it: ZFS allows spaces in names.
std::string percentEncodeSegment(const std::string& s);
bool percentDecode(const std::string& s, std::string& out);

}  // namespace zfsmgr::base
