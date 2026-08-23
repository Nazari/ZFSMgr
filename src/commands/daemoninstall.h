#pragma once

#include <functional>
#include <string>

#include "connectionprofile.h"
#include "transportsession.h"

// Installing or updating the daemon on a machine.
//
// **This is the only operation that can NOT go through the daemon**, and that is not an
// oversight: it is the chicken-and-egg problem. It goes in over SSH and `scp` because if
// there already were a daemon answering, there would be nothing to install. That is why it
// still sends a shell script when everything else stopped doing so — see
// docs/plan_shell_rpc.md.
//
// This used to live inside `src/cli/shell.cpp`, tangled up with the terminal confirmation
// and the `fprintf`s. None of it was shell-specific: it is 200 lines of deciding which
// script fits the system on the other end. Nothing here asks or prints — the caller decides
// how to confirm and how to report, which is what lets more than one client use it without
// duplicating the script.
namespace zfsmgr::base::daemoninstall {

// Why it could not be done, TYPED. A `bool` forced the caller to guess between «there is no
// binary for that platform» —which is fixed by building one— and «the other machine refused
// it», which is not.
enum class Failure {
    None_,
    UnreadableBinary,     // the path does not exist, or the file is empty
    UploadFailed,         // the scp, or the local copy, failed
    InstallFailed,        // the script ran and returned something other than 0
};

std::string labelOf(Failure f);

struct Result {
    Failure fallo{Failure::None_};
    int rc{0};
    std::string detail;              // what the other end said, verbatim
    std::string version;             // the one written into agent.conf
    bool versionBehind{false};       // the bundled agent lags behind this client
    bool isMac{false};               // the caller decides whether to warn about «Full Disk Access»

    bool ok() const { return fallo == Failure::None_; }
};

// «linux», «macos», «freebsd» or «windows», spelled the way whoever looks for the bundled
// binary expects. It comes from the profile's `osType`, not from asking the machine.
std::string platformOf(const ConnectionProfile& p);

// The architecture of the OTHER side, by asking it. On Windows it is not asked: it is
// x86_64 and there is no agent for anything else. Empty when the machine does not answer.
std::string remoteArchitecture(TransportSession& ses, const ConnectionProfile& p, bool verbose);

// The script that gets sent, per platform. **Public on purpose**: it is the piece that
// decides where the binary goes, which service manager is used and what gets checked
// afterwards, so a test can examine it without a machine of each system at hand.
std::string installScript(const std::string& platform, const std::string& version,
                               const std::string& apiVersion);

// Installs or updates, and starts it. It asks nothing: confirming is the caller's job,
// because this replaces a binary and restarts a service on the other end.
//
// `trace` receives every line the installation emits, so it can be shown as it happens.
Result install(TransportSession& ses, const ConnectionProfile& profile,
                  const std::string& binaryPath,
                  const std::function<void(const std::string&)>& trace = {},
                  bool verbose = false);

}  // namespace zfsmgr::base::daemoninstall
