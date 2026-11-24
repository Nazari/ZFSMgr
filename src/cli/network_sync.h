#pragma once

// The client's side of the network's configuration (docs/diseno_red_unica.md, phase 3).
//
// A computer that has joined a network keeps it in <config dir>/network/: the signed document
// as the daemons serve it (network.json), what is its own (local.json: SSH key paths, preferred
// addresses) and a MATERIALISATION of the document as config.json + trust-store.json. The rest
// of the client reads and writes those two files exactly as before; what is written is then
// PUBLISHED: merged on top of the network's latest revision and put to a daemon, which passes
// it on to the others.

#include <string>

namespace zfsmgr::cli {

struct Sesion;
struct Conexiones;

// <dir>/network.
std::string dirDeRed(const std::string& dirConfig);
// This configuration directory has joined a network: the client must work in dirDeRed().
bool tieneRed(const std::string& dirConfig);
// `dir` IS a network's directory (it holds network.json).
bool esDirDeRed(const std::string& dir);

// For each machine with several addresses, the first that answers from here becomes this
// computer's preferred one (local.json), and the files are rematerialised. True if any changed.
bool eligeDirecciones(const std::string& dir);

// At start, with the secrets open: asks the daemons for a newer revision and adopts it if its
// HMAC verifies. True when the files changed (the connections must be loaded again). `aviso`:
// a daemon served something that does not verify, or similar, for the person.
bool sincronizaRed(Sesion& ses, const Conexiones& conns, std::string& aviso);

// After something was written: publishes it. True when a new revision went to the network (the
// files were rematerialised from it). With no daemon reachable the change stays here and
// `aviso` says so; it goes out at the next publication.
bool publicaRed(Sesion& ses, const Conexiones& conns, std::string& aviso);

// `zfsmgr-cli network seed`: this computer's configuration becomes the network's, put to every
// daemon it reaches —none may have one yet—, and this computer joins it.
int sembrarRed(const std::string& dirConfig, const std::string& dataKey);

// `zfsmgr-cli network join`: fetches the network from a daemon this configuration reaches, opens
// it with the network's master password (`password`) and joins: only local.json is kept of this
// computer's own configuration.
int unirseARed(const std::string& dirConfig, const std::string& dataKey, const std::string& password);

}  // namespace zfsmgr::cli
