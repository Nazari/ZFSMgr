#pragma once

#include <string>

#include "connectionprofile.h"
#include "json.h"
#include "storewarnings.h"

// Translation between `ConnectionProfile` and the JSON stored on disk, without Qt.
//
// It is the glue of `ConnectionStore`, and what decides what ends up written into
// `config.json` and `trust-store.json`. The format does not change: there are files already
// written that way. See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::connjson {

// Default SSH port when there is no valid one stored.
int ensurePort(const std::string& connType, int port);

// «local» either by id or by connection type.
bool isLocalProfile(const ConnectionProfile& p);

// The local connection on Unix ALWAYS goes with sudo: without it not even half of what the
// application needs can be read. It does not apply on Windows, where there is no sudo.
bool shouldForceLocalSudo(const ConnectionProfile& p);

bool profileHasDaemonTls(const ConnectionProfile& p);

// Opens with the master password the five fields that travel encrypted: user, password and
// the daemon's TLS trio. Returns false when any of them stayed shut.
//
// **A field that could not be opened KEEPS its ciphertext**, which is why it warns: whoever
// receives it must not use it as though it were the plaintext value. Each failure goes into
// `warnings` with its reason, its connection and its field, with no text: whoever has a
// catalogue does the wording.
//
// This was written twice —the interface collected typed warnings and the shell swallowed
// them silently— and that is why the same configuration was described differently depending
// on which way you looked at it.
bool openSecrets(ConnectionProfile& p, const std::string& master, store::Warnings& warnings);

// The «Local» profile of THIS machine: corrected when present, synthesised when absent.
//
// The operating system, the machine id and whether it elevates are always corrected, because
// a profile saved from another build —or copied from another computer— brings the wrong
// machine's, and then the program believes it is talking to something else.
//
// `machineUid` is supplied by the caller: finding it out means reading the registry on
// Windows or running `ioreg` on macOS, and that does not come down here. Empty = whatever
// was there is kept.
void ensureLocalProfile(std::vector<ConnectionProfile>& profiles, const std::string& machineUid);

// Merges into each profile whatever TLS material lives in the trust store, indexing by id.
//
// **The STORE wins, not the profile**, and that is a decision, not a detail: the store is
// where the material negotiated with each daemon is persisted, and whatever is left in
// `config.json` predates it —there is a migration that pulls it out of there—. The two
// halves of the program did this the opposite way from each other: the interface let the
// store win and the shell let the profile win, so with old material still in `config.json`
// one used the fresh one and the other the stale one.
//
// A store entry with NO matching connection is added as a connection. It is TLS material
// negotiated with a machine that is still there: discarding it would force renegotiating it
// over SSH, and on a host where /etc/zfsmgr is root-only that means asking for sudo. Local
// ones are excluded, since those are synthesised separately.
void mergeTrustStore(std::vector<ConnectionProfile>& profiles, const json::Value& trust,
                     const std::string& master, store::Warnings& warnings);

// PSRP was withdrawn as a transport: it cannot carry the daemon, because the RPC travels
// through an `ssh -L` tunnel and without SSH there is no tunnel. A profile saved with PSRP
// can neither stay as it is —it would fail opaquely— nor simply vanish, so it is converted
// to SSH.
//
// The port is the part that gets forgotten: 5986 is WinRM, and leaving it turns a broken
// connection into a broken connection WITH NO explanation, which is worse than where it
// started.
bool migratePsrpProfileToSsh(ConnectionProfile& p);

// When `raw` is the ASCII hex of a UUID, returns the UUID; otherwise, empty.
//
// It imitates `QByteArray::fromHex`, which **skips non-hex characters** instead of failing,
// and which on an odd number of digits behaves as though there were a leading '0'. Not a
// whim: there are stored ids that depend on it.
std::string decodeHexAsciiIfUuid(const std::string& raw);

// `localUid` is THIS machine's id. It is passed as an argument rather than looked up here
// because finding it out costs between 400 and 600 ms —it launches `ioreg` on macOS or reads
// the registry on Windows— and that is exactly what cannot live in the base layer. It is
// only used as a fallback for the local profile when nothing is stored.
std::string normalizeMachineUidForStorage(const ConnectionProfile& p,
                                          std::string raw,
                                          const std::string& localUid);

// `config.json`: the connection data and the password. WITHOUT the TLS material, which
// lives apart in the trust store.
json::Value connectionToJson(const ConnectionProfile& p, const std::string& localUid);

// `trust-store.json`: the same WITHOUT the password and WITH the TLS material.
json::Value connectionTrustToJson(const ConnectionProfile& p, const std::string& localUid);

// Reads from either of the two: absent fields keep their default value.
ConnectionProfile connectionFromJson(const json::Value& obj, const std::string& localUid);

// Position of a connection by id, case-insensitively. -1 when it is not there.
long long indexOfConnectionById(const json::Array& connections, const std::string& id);

// Inserts or replaces by id. Returns false when the profile has none.
bool upsertConnectionJson(json::Array& connections,
                          const ConnectionProfile& p,
                          const std::string& localUid);

}  // namespace zfsmgr::base::connjson
