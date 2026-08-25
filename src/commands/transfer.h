#pragma once

#include <functional>
#include <string>
#include <vector>

#include "connectionprofile.h"
#include "transportsession.h"

// Moving DATA between two endpoints: which way the bytes go, and where a resume starts from.
//
// There is NO transfer here: there are the DECISIONS of a transfer. It lives in the base
// layer because every client has to make the same ones —which of the three routes, and why
// it cannot be done when it cannot— and a second copy of those rules drifts apart at the
// first fix.
//
// See docs/diseno_tecnico_transferencias.md. This is phase 0: the types and the choice,
// which can be tested without moving a single byte.
namespace zfsmgr::base::transfer {

// Which way the bytes go. The order of the enum IS the order of preference.
//
// **This list came out of READING the code, and it corrected the design**: the design had
// noted three routes with the tar fallback among them. It is not so. The tar belongs to Sync
// —which moves FILES with rsync and tar, not `zfs send`— and Send does not have it: when
// there is no pipeline to build, it stops and says so.
enum class Route {
    // Launched by `--job-submit` and held up by the daemon. It survives the client being
    // closed, and it is the ONLY one usable by a client that cannot wait.
    AsyncJob,
    // `--zfs-recv-listen` at the target and `--zfs-send-to-peer` at the source. No shell, and
    // the bytes do not pass through the client, but whoever launched it holds it up.
    DaemonToDaemon,
    // `ssh source 'zfs send' | ssh target 'zfs recv'`, in its variants. It needs no daemon at
    // either end: it is what is left when there is none.
    SshPipeline,
    None_,
};

// Why it cannot be done. TYPED because it is what has to be shown: «unavailable» without
// saying which of the six reasons it is leaves the user trying combinations.
enum class Failure {
    None_,
    SameObject,
    SourceIsNotSnapshot,
    TargetIsNotDataset,
    WindowsEndpoint,       // the Windows agent does not stream over a pipeline yet
    NoJobSupport,          // the async route is required and one end does not take it
    ZfsTooOld,             // below 2.3.3 nothing is transferred
};

const char* keyOf(Route c);
const char* keyOf(Failure f);
std::string labelOf(Route c);
std::string labelOf(Failure f);

// What has to be known about an endpoint in order to decide. Nothing is queried from here:
// the caller brings it, since the caller is the one holding the transport session.
struct Endpoint {
    std::string connection;
    std::string object;          // dataset, or dataset@snapshot at the source
    bool isWindows{false};
    bool hasDaemon{false};
    bool supportsJobs{false};    // `JOBS_SUPPORT=1` in its `--health`
    std::string zfsVersion;      // «2.3.3», «2.2.99-1», … empty when unknown

    bool isSnapshot() const { return object.find('@') != std::string::npos; }
    std::string dataset() const {
        const std::size_t i = object.find('@');
        return i == std::string::npos ? object : object.substr(0, i);
    }
};

// Can this version of OpenZFS transfer?
//
// Below **2.3.3**, no. That is a rule of this project, not of ZFS, and it used to be written
// inside the window. An empty or unparseable version does NOT block: not knowing the version
// is different from knowing it is old, and blocking on not knowing would rule out a machine
// that may well be able to.
bool versionSupportsTransfer(const std::string& version);

// The `zfs send` flags, in the order the program writes them.
struct SendOptions {
    bool w{false};   // raw: sends the encrypted dataset as-is, without decrypting it
    bool L{false};   // large blocks
    bool e{false};   // «embedded»: takes advantage of already-compressed blocks
    bool c{false};   // compressed
    bool R{false};   // the whole hierarchy, snapshots included
};

// «-wLR», or empty when there is none. Empty and not «-»: a lone «-» in the middle of a
// command line is an argument `zfs` does not understand.
std::string sendFlags(const SendOptions& o);

// Did a send that reported success actually copy nothing?
//
// A FULL send that moved zero bytes copied nothing, whatever the exit status said: a dataset
// always carries at least its own metadata, so zero means the receiver never took the stream.
// Saying «done» there is indistinguishable from a transfer that worked, which is exactly where
// trust in the transfers panel is lost.
//
// An INCREMENTAL is allowed to move nothing —there may be nothing new since the base
// snapshot—, and so is a RESUMED send that had already finished. Hence the two arguments:
// without them the rule would turn those legitimate cases into errors.
bool fullSendMovedNothing(unsigned long long bytes, const std::string& baseSnapshot,
                          const std::string& resumeToken);

// The routes worth trying, IN ORDER, and not just one.
//
// Because that is how it works: the first is attempted and, when it cannot be built, it falls
// through to the next. And that gets decided while running —`recv-listen` may fail at the
// target— not here. What is decided here is which ones are worth attempting.
struct Plan {
    std::vector<Route> routes;
    Failure failure{Failure::None_};

    bool ok() const { return !routes.empty(); }
};

// Which routes are worth trying between these two endpoints.
//
// `requiresAsync` is set by whoever CANNOT hold the transfer up for its duration: a client
// that serves one request at a time cannot have a request last four hours, so only the job
// route works for it. An interface that can wait does not require it.
Plan makePlan(const Endpoint& source, const Endpoint& target, bool requiresAsync);

// The resume token ZFS left at the target, if there is one.
//
// **It is looked for on the target AND on its descendants**, and that detail is no ornament:
// sends go with `-R`, that is, the whole hierarchy in a single stream, and when it is cut ZFS
// leaves the token on the dataset that was receiving at that moment, which is almost never
// the root. Measured by cutting a 3.4 GB send: the parent came out complete and the token
// appeared on the child. Looking only at the root said «there is nothing to resume» with 247
// MB already transferred.
struct Resume {
    std::string token;
    std::string heldBy;      // the dataset it was on

    bool any() const { return !token.empty(); }
};

// The RULE of which one wins, kept apart from going to fetch them.
//
// It takes «dataset<TAB>token» lines, with «-» where there is none. Who gathers them is
// `findResumeToken`, below; here it is only decided, which is why it can be tested with no
// machine at hand.
Resume resumeToken(const std::string& target, const std::string& tsvOutput);

// The address the SOURCE sees this machine at, pulled out of what `echo $SSH_CLIENT`
// returns. Empty when it is unusable.
//
// **It accepts IPv6 WITH a zone**: sshd can answer `fe80::d11d:24e3:5547:cbd6%enp1s0f0`,
// which is exactly what the test machine returned. A validation of hex and dots only rejected
// it and left the send with no address to come back to.
std::string sshClientAddress(const std::string& output);

// ── How the send command is composed ─────────────────────────────────────────

// Where it is actually received.
//
// Not the dataset that was clicked on: the SOURCE'S NAME is appended, so that sending «data»
// onto «backups» leaves «backups/data» and does not pour over it. Unless the target already
// ends in that name, in which case it is taken as-is — otherwise sending twice to the same
// place would create «backups/data/data».
//
// That same detail is what makes looking for the resume token on the clicked dataset find
// nothing: it has to be looked for on THIS one.
std::string actualDestination(const std::string& sourceDataset, const std::string& chosenTarget);

// `zfs send [flags] <snapshot>` and `zfs recv -Fus <target>`, unwrapped.
//
// The receiver's `-Fus` is not decorative: the «s» is what makes a cut leave a send SUSPENDED
// with its token instead of garbage. Without it there would be no resuming, and every cut
// would force sending everything again.
std::string sendCommand(const std::string& snapshot, const std::string& flags);
std::string receiveCommand(const std::string& target);

// `Montaje` and `montajeDe` used to live here: which of the three ways of joining the two
// sides applied —local pipeline, remote to remote directly, or passing the bytes through this
// machine—.
//
// They were withdrawn when Send and Level stopped having shell fallbacks. The three ways were
// ways of chaining `ssh` and pipes; with the transfer done by a daemon job there is nothing
// to build: the receiver opens a port and the sender connects. The rule has not been lost, it
// has ceased to exist.

// ── The async route: a job in the daemon ─────────────────────────────────────
//
// It is the only one usable by a client that cannot wait, because the daemon holds it up and
// not whoever launched it. Three steps: the receiver opens a port, the address the sender has
// to come back to is worked out, and the sender starts the send and returns an id.

// What `--zfs-recv-listen` answers: which port it waits on and with which token.
struct ReceiverListen {
    int port{0};
    std::string token;

    // The token is 64 characters. An answer of any other length is not a truncated one: it is
    // not the answer that was expected, and going on with it would leave the sender talking to
    // the wrong party.
    bool ok() const { return port > 0 && token.size() == 64; }
};

ReceiverListen readListen(const std::string& output);
std::string readJobId(const std::string& output);

// Why the job did not start. The five points where it can break, kept apart, because each one
// leads somewhere different: one is the receiver's, another the network's, another the
// sender's.
enum class JobFailure {
    None_,
    ReceiverNotListening,
    BadListenAnswer,
    NoReturnAddress,
    SenderDidNotStart,
    NoJobId,
};

std::string labelOf(JobFailure f);

struct Job {
    std::string id;
    JobFailure failure{JobFailure::None_};
    std::string detail;

    bool ok() const { return failure == JobFailure::None_ && !id.empty(); }
};

// How the agent of a machine is spoken to. **The caller supplies it, and not out of whim.**
//
// A LOCAL connection is not reached the same way as a remote one: the tunnelled RPC rejects
// outright anything that is not SSH, so the local one has to go through the daemon's socket
// with its TLS material. Each client already knows how to do that, and putting that
// distinction in here would force local-TLS discovery up into the base layer, where it does
// not belong.
//
// It got lost when this was extracted out of the window, and the first real test caught it:
// the job did not start because the target was «Local» and it was being spoken to as if it
// were remote.
using AgentCall = std::function<bool(const ConnectionProfile& machine,
                                           const std::vector<std::string>& args, int timeoutMs,
                                           std::string& output, std::string& err, int& rc)>;

// Starts the job. It returns as soon as it is launched: it does NOT wait for it to finish,
// which is precisely why it exists.
//
// With `resumeToken` set, the snapshot, the base and the flags go empty on purpose: `zfs send
// -t` carries inside it what to continue, and it will not be contradicted.
Job launchJob(TransportSession& ses, const AgentCall& call,
                     const ConnectionProfile& source, const ConnectionProfile& target,
                     const std::string& snapshot, const std::string& recvTarget,
                     const std::string& fromSnapshot, const std::string& flags,
                     const std::string& resumeToken, bool sameConnection, bool verbose);

// ── The part that does go and ask the machines ───────────────────────────────

// The same three-step dance, but carrying a FILE TREE instead of a snapshot: the target
// listens, the address the source sees it at is worked out, and the source sends.
//
// **What it is for: «FromDir» with no shell pipeline.** That action used to move the data with
// `ssh source 'tar -c' | ssh target 'agent --mutate-advanced-fromdir'`, that is, passing ALL
// the content through the driver's machine: copying 100 GB from one machine to another moved
// 200 GB through the one in the middle. Here it goes daemon to daemon.
//
// And along the way it gains what the `zfs send` stream already had: it is a job, so there is
// progress, it can be cancelled and it survives the window being closed. The copy is also
// incremental —it skips what is already identical, comparing size and mtime—, whereas the tar
// resent the whole tree on every pass.
//
// The target directory has to EXIST: the receiver checks and fails when it does not. To
// create it there is `advanced::argvFromDirPrepare`, which also mounts the dataset and
// resolves its real mountpoint.
//
// It requires a daemon at BOTH ends. The tar path only asked for one at the target, so this
// does not replace it: it takes precedence when it can.
//
// `asJob` decides whether the send is queued in the daemon —the window wants it that way: it
// does not block, it can be cancelled and it goes on if the window closes— or whether it
// waits for it to finish, which is what the shell does, because its command already returned
// the result and a script behind it counts on the files being there.
//
// **Careful with how the result is checked.** With `asJob` false there is no id to return, so
// `ok()` —which requires one— would say no even when everything went well: there, what to
// look at is `failure`. `ok()` means «there is a job to keep track of», not «it went well».
Job launchTreeJob(TransportSession& ses, const AgentCall& call,
                            const ConnectionProfile& source, const ConnectionProfile& target,
                            const std::string& sourceDirectory,
                            const std::string& targetDirectory, bool sameConnection,
                            bool verbose, bool asJob, bool deleteAtTarget = false,
                            bool dryRun = false, std::string* sendOutput = nullptr);

// Which address the SOURCE has to connect to in order to reach the TARGET.
//
// Empty when it cannot be worked out. Both the `zfs send` stream and the file tree use it;
// see the comment on the implementation for the Local-connection case, which is the one that
// gets lost the moment somebody copies this rule instead of calling it.
std::string whereItConnects(TransportSession& ses, const ConnectionProfile& source,
                              const ConnectionProfile& target, bool sameConnection,
                              bool verbose);

// Which address the SOURCE sees this machine at.
//
// It is ASKED rather than deduced: the machine may have several interfaces, sit behind NAT or
// arrive over a VPN, and only the other end knows which way the connection came in.
std::string howTheSourceSeesMe(TransportSession& ses, const ConnectionProfile& source,
                             bool verbose);

// Whichever resume token is on the target or on its descendants.
//
// That is N+1 queries —one per dataset—, which is what the interface does today. It is kept
// as-is on purpose: this phase changes no behaviour. With a verb that read a property
// recursively it would be a single one, and that is noted in the design.
Resume findResumeToken(TransportSession& ses, const ConnectionProfile& target,
                         const std::string& objective, bool verbose);

// ---------------------------------------------------------------------------
// Levelling: bringing the target up to date with the source WITHOUT sending it all again.
//
// **It is not sending.** Send ships a complete stream and receives into «<target>/<leaf>»;
// levelling ships an INCREMENTAL —`zfs send -I <base> <objective>`— and receives into the
// target dataset as-is. Confusing them is not a nuance: with the target already populated,
// the complete stream arrives with `zfs recv -Fus` and takes down whatever the source does
// not have.
//
// The common base is NOT looked up by name, it is looked up by GUID. Two snapshots can share
// a name on both machines without being related —having been created separately is enough—
// and sending an incremental against a false base is sending against a different history. The
// GUID already comes in `--dump-zfs-list-all`, so it costs no extra query.
//
// The three refusals are safety ones and come from the Qt interface, which has had them from
// the start: without them, levelling can throw away work at the target without warning.
struct Snapshot {
    std::string name;   // short, without the «dataset@»
    std::string guid;
};

enum class LevelFailure {
    None_,
    TargetNotAtSource,
    TargetHasNoSnapshots,
    BaseNotAtSource,
    TargetIsNewer,
    AlreadyLevel,
};

struct LevelPlan {
    std::string base;        // where from: the send's «-I»
    std::string objective;   // how far
    LevelFailure failure{LevelFailure::None_};
    bool ok() const { return failure == LevelFailure::None_; }
};

// Both lists go IN CREATION ORDER, which is how `zfs list -t snapshot` gives them. The order
// is what decides what «newer» means, so handing them in sorted any other way does not return
// an error: it returns a wrong answer.
LevelPlan makeLevelPlan(const std::vector<Snapshot>& source,
                          const std::vector<Snapshot>& target,
                          const std::string& objective);

std::string labelOf(LevelFailure f);

}  // namespace zfsmgr::base::transfer
