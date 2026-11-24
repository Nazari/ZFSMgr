#pragma once

// ONE configuration for the whole network (docs/diseno_red_unica.md), phase 1: the document,
// its integrity, and building it from a client's configuration. No network here.
//
// The document is kept by every daemon and read by every client. Its secrets are encrypted
// with the network's DATA KEY (storefiles.h, «THE DATA KEY»), which no daemon has: a daemon
// keeps what it cannot read. Nor can it forge it: the document carries an HMAC under a key
// derived from the data key, and a client rejects one that does not verify —root on one machine
// could otherwise point another machine's address at itself and collect the SSH password the
// client sends there—. And it rejects one OLDER than the copy it has: a stale document is valid
// but superseded.
//
// The master block (the data key's slots: the master password and every FIDO2 key) belongs to
// the network too: one master password, and a key registered once opens on every computer.

#include "json.h"

#include <map>
#include <string>
#include <vector>

namespace zfsmgr::base::network {

struct Machine {
    std::string uid;   // the machine's identity, as its daemon reads it of itself
    std::string name;  // a label, unique in the network; what URLs say (zfsm://<name>)
    std::string os;
    // Where it is reached, in order: each client uses the first that answers. From fc16 FreeB
    // is 192.168.122.119; from anywhere else, «freeb».
    std::vector<std::string> addresses;
    std::string sshUser;
    int sshPort{22};
    std::string sshFamily{"auto"};
    bool sudo{false};
    // Encrypted with the data key (encv1$…), or empty. Never in the clear.
    std::string password;
    int tlsPort{47653};
    std::string tlsServerCert;  // encrypted
    std::string tlsClientCert;  // encrypted
    std::string tlsClientKey;   // encrypted
};

struct Document {
    int schema{1};
    long long revision{0};
    std::string writtenBy;  // uid of the machine whose client wrote this revision
    long long writtenAt{0};
    json::Value master;     // {version, slots, fido_salt, check}: opaque here
    std::vector<Machine> machines;
    std::string mac;        // base64 HMAC-SHA256 over the canonical form without «mac»
};

// What stays on each computer, beside the network's document.
struct LocalSettings {
    std::map<std::string, std::string> keyPaths;          // uid → SSH key path on THIS computer
    std::map<std::string, std::string> preferredAddress;  // uid → the address to try first
};

json::Value toJson(const Document& d, bool withMac = true);
// Checks the shape: schema 1, a uid and a name on every machine, both unique, revision ≥ 0,
// no secret in the clear. `error` says what was wrong.
bool fromJson(const json::Value& v, Document& d, std::string& error);

// The same bytes for the same content whatever the order of the keys: objects sorted by key at
// every level, compact. What the HMAC is computed over.
std::string canonical(const json::Value& v);

// base64 of HMAC-SHA256(key, canonical(document without «mac»)), where key is
// HMAC-SHA256(dataKey, "zfsmgr-network-mac-v1"): its own key, never the data key itself.
std::string macOf(const Document& d, const std::string& dataKey);
void sign(Document& d, const std::string& dataKey);
// Constant-time comparison.
bool verify(const Document& d, const std::string& dataKey);

enum class Verdict {
    Newer,     // valid and newer than the copy: adopt it
    Same,      // the copy itself
    Older,     // valid but superseded: keep the copy
    Diverged,  // valid, same revision as the copy, different content: two writers met
    BadMac,    // not written by someone holding the data key: rejected, and said where from
};
// What to do with a document received, against the copy this client has (revision 0 and an
// empty mac when it has none).
Verdict judge(const Document& incoming, const std::string& dataKey, long long cachedRevision,
              const std::string& cachedMac);

// For the DAEMONS, which cannot verify the HMAC: whether a document offered by a peer replaces
// the one kept. A higher revision wins; on the same revision with another content —two writers
// met—, the greater mac, so that every daemon ends up with the same one whatever the order the
// offers arrive in. The clients, which can verify, judge() what the daemons serve.
bool offeredWins(long long keptRevision, const std::string& keptMac, long long offeredRevision,
                 const std::string& offeredMac);

// The network built from a client's configuration —config.json and trust-store.json, as they
// are on disk, without opening anything: the secrets are already under the data key and go as
// they are—. The LOCAL connection becomes the machine called `thisHost`, reached at
// `thisHost`. SSH key paths go to `local`: they belong to that computer. Revision 1, written by
// `thisUid` now. False, with the reason, if a connection has no uid, two share one, or a secret
// is in the clear.
bool fromClientConfig(const json::Value& config, const json::Value& trustStore,
                      const std::string& thisHost, const std::string& thisUid, long long now,
                      Document& d, LocalSettings& local, std::string& error);

// The other way: what a client loads, in the shapes config.json and trust-store.json have
// —their «connections»— so the code that turns them into connections and opens their secrets
// stays as it is. Every machine is a connection named after it; the one whose uid is
// `thisUid` is THIS computer's (type LOCAL, no SSH to itself) and is called by its network
// name too: there is no «local» any more. The address is this computer's preferred one, or the
// first. The master block goes in `config` as config.json keeps it (master + master_check).
void toClientConfig(const Document& d, const LocalSettings& local, const std::string& thisUid,
                    json::Value& config, json::Value& trustStore);

// --- What a client has on disk, back into the network (the publication, phase 3b).

// The network as this client's materialised files say it now, on top of `base` —the document
// they were materialised from—: machines by uid; a machine's addresses are kept, and an address
// edited here goes first unless it is only this computer's preferred one; SSH key paths go to
// `local`. A connection without a uid (no daemon yet) cannot be in the network: its id goes to
// `withoutUid`. Same revision as `base`.
Document fromMaterialized(const Document& base, const json::Value& config,
                          const json::Value& trustStore, LocalSettings& local,
                          std::vector<std::string>& withoutUid);

// The same machines and master block, whatever the revision, the writer and the mac.
bool sameContent(const Document& a, const Document& b);

// What this client changed (`mine` against `base`, the copy it started from), applied on top of
// what the network has now (`theirs`): per machine —changed, added or removed here: mine;
// otherwise: theirs— and the master block alike. The revision, writer and mac are the caller's.
Document threeWay(const Document& base, const Document& mine, const Document& theirs);

json::Value localToJson(const LocalSettings& l);
LocalSettings localFromJson(const json::Value& v);

}  // namespace zfsmgr::base::network
