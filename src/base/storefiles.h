#pragma once

#include <string>

#include "connectionjson.h"
#include "json.h"
#include "storewarnings.h"

// Reading and writing the two files of the connection store, without Qt.
//
// The directory arrives as an argument rather than being computed here: today it is
// `~/.config/<app>`, and reimplementing each platform's rules to save one parameter would
// risk the application no longer finding people's configuration, in exchange for nothing.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::store {

std::string configPath(const std::string& configDir);
std::string trustStorePath(const std::string& configDir);

// The file NOT existing is not a warning: it is the first run. It returns an empty object
// and a `warning` with no reason.
json::Value readConfig(const std::string& configDir, Warning& warning);
json::Value readTrustStore(const std::string& configDir, Warning& warning);

// They write with **owner-only** permissions, set BEFORE the contents are poured in: the
// other way round would leave an instant with the file already full of encrypted secrets and
// whatever permissions the umask happened to leave.
bool writeConfig(const std::string& configDir, const json::Value& root, Warning& warning);
bool writeTrustStore(const std::string& configDir, const json::Value& root, Warning& warning);

// Changes the MASTER PASSWORD: decrypts with the old one and re-encrypts with the new one
// EVERYTHING that hangs off it, in both files.
//
// This is not «changing a value»: hanging off the master are each connection's password and
// the daemon's TLS material —server certificate, client certificate and key—, and the latter
// also lives in the trust store. Doing it halfway leaves fields encrypted with the old key,
// and the next session opens neither the connections nor the TLS.
//
// That is why a COPY of both files is written first —with the suffix returned in
// `backupSuffix`— and only then are they touched. If something fails midway, the warning
// says which field it was and the copies are still there.
//
// It lives in the base layer, and not in the interface, because the shell needs it just the
// same: it was the last thing that could only be done with a window in front of you.
bool rotateMasterKey(const std::string& configDir, const std::string& oldMaster,
                      const std::string& newMaster, std::string& backupSuffix, Warning& warning);

// Saves a profile into `config.json`: replaces the one carrying its id, or adds it.
//
// It does three things that cannot be separated from writing, and that lived only in the
// interface:
//
//  - **Encrypts** whatever is in the clear. Without a master password NOTHING is written:
//    leaving an access password readable on disk to save a step is a bad trade.
//  - **Keeps the TLS material** of the profile already stored when the ENDPOINT has not
//    changed —same host, port, user and key—, so as not to force renegotiating it.
//  - **And drops it when the endpoint DID change.** This is the important one: a certificate
//    pinned for one machine is no good for another, and dragging it along when the host
//    changes leaves the client trusting a certificate that is not its own. The shell did
//    neither: it wrote the profile as-is, so an `edit` that changed the host kept the old
//    host's TLS.
//
// Validating the fields and deciding where the id comes from is NOT here: each half has its
// own, and they are different policies.
bool saveProfile(const std::string& configDir, const ConnectionProfile& p,
                  const std::string& master, Warning& warning);

// Stores in the trust store the TLS material negotiated with a machine.
//
// Both write it: the interface when creating or editing a connection, and the shell every
// time the transport negotiates with a daemon. Without a master password NOTHING is stored:
// leaving the client's private key readable on disk to save one SSH read is a bad trade, and
// both halves already applied that rule on their own.
//
// LOCAL connections do not go to the store, and neither do the ones carrying no material:
// there is nothing to pin.
bool saveTlsToStore(const std::string& configDir, const ConnectionProfile& p,
                        const std::string& master, Warning& warning);

// Encrypts whatever was left IN THE CLEAR in both files. What is already encrypted is left
// alone —there is no telling which key it is under, and re-encrypting it would require
// opening it first—.
//
// It is the migration of a configuration written without a master password, or of a field
// that slipped through in the clear. It carries no backup like the rotation does: there is
// nothing to lose here, because the only change is from readable to unreadable.
bool encryptWhatIsMissing(const std::string& configDir, const std::string& master,
                          Warning& warning);

// Removes a connection from BOTH files.
//
// From both, and here is why: ever since a trust-store entry with no matching connection
// gets turned into a connection, leaving its entry behind is not untidiness — it means the
// connection COMES BACK on the next start. Verified: deleting «oldlau» in the shell removed
// it from config.json and it returned to the list on reopening.
//
// It returns false only when there was none carrying that id, or when it could not be
// written.
bool deleteProfile(const std::string& configDir, const std::string& id, Warning& warning);

// Is there ANYTHING encrypted in the two files? When there is not, asking for the master
// password is friction for no reason, and that is the kind of friction that ends with the
// password written into a shell alias.
bool hasSomethingEncrypted(const std::string& configDir);

// Does this master password open EVERYTHING that is encrypted? It returns the first field
// that did not open.
//
// It is checked on entry and not when a secret is first needed: a wrong master does not fail
// on its own —the fields stay shut and the failure surfaces later disguised as something
// else, a «could not read the TLS material» or a sudo that gets asked for again—, and one
// spends a while staring at the remote machine before realising that what was mistyped was
// the master password.
//
// EVERYTHING is walked and not just the first field. With the Fernet format, opening one
// would be enough to know the key is the right one; but this also detects a HALF-ROTATED
// configuration, with some fields under the new key and others under the old, which is
// exactly what an interrupted rotation can leave behind.
bool masterOpensEverything(const std::string& configDir, const std::string& master,
                           Warning& warning);

}  // namespace zfsmgr::base::store
