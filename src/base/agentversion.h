#pragma once

#include <string>

// AGENT versions: the one this client expects, how two of them order, and which one a
// binary sitting on disk declares.
//
// It lives in the base layer because the shell needs them just as much as the interface
// does: today it marks with a « * » the machine whose version is not the expected one, and
// on install it writes a version into `agent.conf`. It used to have its own way of doing
// both, a poorer one —it compared strings with `!=` and wrote the version it had itself been
// built with, not the one of the binary it was copying—, and that is exactly the kind of
// divergence the base layer exists to prevent.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::agentversion {

// The one this client expects. From the compiler: it carries the schema-marker suffix.
std::string expected();

// The PROTOCOL version, which is not the agent's and changes far less often.
std::string expectedApi();

// Orders two «maj.min.patch[rcN][.suffix]» versions. Returns <0, 0 or >0.
//
// A release candidate —«0.93.0rc1»— comes BEFORE its final. Anything that does not fit the
// shape is compared as text, which is not correct but is predictable.
int compare(const std::string& a, const std::string& b);

// What to do with an installed agent, given the one this client expects.
//
// NOT `compare` on the full strings: the fourth number is a build FINGERPRINT, not a counter,
// so «0.99.3.589042275» vs «0.99.3.123456789» orders at random. The release part
// (maj.min.patch[rcN]) decides ahead/behind; with the same release, a different fingerprint
// is a different build and it is the client's that should be there.
enum class Verdict { UpToDate, Update, Ahead };
Verdict verdictFor(const std::string& installed, const std::string& expected);

// The version an agent binary declares, read out of the FILE.
//
// It has to be read that way because the bundled agent is usually for another platform and
// cannot be run here to be asked.
//
// **It looks for any well-formed version, not this build's.** It used to be anchored to the
// current version's prefix, and that is why it found NOTHING in exactly the case that
// matters: a bundled agent of a different version —which is when there is something to warn
// about—.
std::string versionInBinary(const std::string& path);

}  // namespace zfsmgr::base::agentversion
