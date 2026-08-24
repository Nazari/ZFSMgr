#pragma once

#include <string>
#include <vector>

// What is ASKED of the agent, one function per thing.
//
// **Why this exists.** The name of a verb —«--dump-zpool-status»— is a contract between the
// daemon and its clients, and until now it was written out by hand in each one. Measured
// before writing this: of 59 verbs, **34 appeared verbatim in two or three clients**. Twenty
// of them in all three.
//
// That is not merely ugly, it is fragile in a specific way: along with the verb goes **how
// many arguments it takes and in which order**, and that is written down nowhere. When a
// verb gains an argument, the client that does not hear about it does not fail to compile:
// it fails at run time, against a machine, and with luck.
//
// Nothing else is decided here. No transport, no sudo, no output format: the argv and
// nothing more. Who runs it and how is each client's business, because a Local connection is
// not reached the same way as a remote one.
//
// Mutations that carry rules of their own are NOT here: they live where their rule lives
// —`pools`, `snapshots`, `datasets`, `advanced`, `zfsallow`—, because composing their argv
// requires knowing what each flag means. This is for what has no rule beyond its name.
namespace zfsmgr::commands::requests {

// ── Pool reads ───────────────────────────────────────────────────────────────

std::vector<std::string> poolList();
std::vector<std::string> poolStatus(const std::string& pool);
// The `-p` variant: the same data unrounded. Two verbs and not one flag, because the daemon
// serves them separately.
std::vector<std::string> poolStatusRaw(const std::string& pool);
std::vector<std::string> poolHistory(const std::string& pool);
std::vector<std::string> poolProperties(const std::string& pool);
std::vector<std::string> poolGuid(const std::string& pool);
// The pools that could be imported. It takes no arguments: it asks about all of them.
std::vector<std::string> importableProbe();

// ── Dataset reads ────────────────────────────────────────────────────────────

// The whole tree under an object, as ten-column TSV. See `listings::entries`.
std::vector<std::string> datasetList(const std::string& object);
// The names only, recursive.
std::vector<std::string> descendantNames(const std::string& object);
// The directories Breakdown can turn into datasets. It answers «__MP__=<mountpoint>» and
// then one relative path per line. It works on both platforms: it resolves the mountpoint
// from the REAL mounts, which on Windows is a drive letter.
std::vector<std::string> breakdownList(const std::string& dataset);
std::vector<std::string> datasetProperties(const std::string& object);
std::vector<std::string> datasetProperty(const std::string& property,
                                            const std::string& object);
std::vector<std::string> datasetExists(const std::string& object);
std::vector<std::string> guidMap(const std::string& object);
// Several properties of one object in a single query. The list goes COMMA-SEPARATED in one
// argument, not as loose arguments: asking one at a time is N round trips.
std::vector<std::string> specificProperties(const std::vector<std::string>& properties,
                                              const std::string& object);
// The permissions of several datasets at once.
std::vector<std::string> permissionsOfMany(const std::vector<std::string>& datasets);
// The GUID and the state of ALL pools at once, which is what a refresh needs. It takes no
// arguments: asking pool by pool was N round trips per refresh.
std::vector<std::string> poolGuidsAndStates();
std::vector<std::string> mounts();
// The drive letters of a pool, with their source —«local», «temporary» or inherited—.
//
// The source is NOT a detail: on Windows the descendants inherit the pool's letter and mount
// flat under that drive, so two datasets sharing an inherited letter is normal operation.
// Without the source, any pool with more than one dataset looked like it had duplicate
// letters. Verified against OldLau: «winpool Z: local», «winpool/sa z: temporary».
//
// Off Windows the verb exists but `zfs` answers that the property does not exist —on macOS,
// «invalid property 'driveletter'», verified— and returns a non-zero code. The caller reads
// that as «there are no letters», which is the truth.

std::vector<std::string> driveLetters(const std::string& pool);
std::vector<std::string> permissionsOf(const std::string& dataset);
// Several objects in one call: the verb takes them after it.
std::vector<std::string> holdsOf(const std::vector<std::string>& objects);
std::vector<std::string> diffBetween(const std::string& snapshotA,
                                         const std::string& snapshotB);

// ── Scheduled snapshots (GSA) ────────────────────────────────────────────────

std::vector<std::string> gsaOfDataset(const std::string& dataset);
std::vector<std::string> gsaOfAllPools();

// ── Files ────────────────────────────────────────────────────────────────────

std::vector<std::string> directoryContents(const std::string& path);
// `from` and `howMuch` in bytes; zero and zero means the whole file.
std::vector<std::string> fileContents(const std::string& path, unsigned long long from,
                                            unsigned long long howMuch);

// ── The agent itself ─────────────────────────────────────────────────────────

std::vector<std::string> health();
// The daemon's log: from which byte, and how many at most. Zero and zero is the whole file.
//
// They are BYTES, not lines, even though the verb's name does not say so: the daemon `seek`s
// into the file. Mistaking them for lines is what makes a client ask for «the last 200» and
// receive 200 bytes starting mid-word.
std::vector<std::string> daemonLog(unsigned long long fromByte, unsigned long long howMany);
std::vector<std::string> blockDevices();
std::vector<std::string> zfsVersion();
std::vector<std::string> availableTools();
std::vector<std::string> refreshBasics();
std::vector<std::string> peerList();

// ── Mutations with no rule beyond their shape ────────────────────────────────
//
// The ones that DO carry a rule —which flag means what, which scope is which— are composed
// in their own module: `pools`, `snapshots`, `datasets`, `advanced`, `zfsallow`. Here are
// only the ones that do no more than wrap an argv or pass a few arguments along.

// Any `zfs <op> …`, with the argv encoded. The daemon runs it with execvp and checks that
// `op` is on its allow-list.
std::vector<std::string> zfsGeneric(const std::string& encodedArgv);
std::vector<std::string> zpoolGeneric(const std::string& encodedArgv);
// Creating a dataset is a verb of its own and not a generic `zfs create` **because it can
// carry an encryption passphrase**: the daemon receives it in the RPC payload and hands it to
// `zfs` down a pipe. Through the generic path it would end up in argv, visible in a `ps`.
std::vector<std::string> createDataset(const std::string& encodedArgv);

// **These two carry their arguments in base64, and not out of whim.** A passphrase in argv
// is visible to anyone with a `ps` on the machine; encoded, it travels inside the RPC
// payload, which is encrypted. The caller passes plain text and the encoding happens here:
// leaving it to the caller was inviting one of them to forget.
std::vector<std::string> loadKey(const std::string& dataset, const std::string& phrase);
// An empty `newPhrase` means removing the key.
std::vector<std::string> changeKey(const std::string& dataset, const std::string& phrase,
                                     const std::string& newPhrase);

std::vector<std::string> repairAltMountpoints(const std::vector<std::string>& extras);
std::vector<std::string> setPeers(const std::string& payloadB64);
std::vector<std::string> setBindAddress(const std::string& address);
std::vector<std::string> rsyncCopy(const std::string& payloadB64);
std::vector<std::string> permissionsBatch(const std::string& payloadB64);

// ── Jobs ─────────────────────────────────────────────────────────────────────

// Enqueueing: the verb goes IN FRONT of the command being enqueued, not behind it.
//
// The daemon only accepts a few mutations for queueing —the long ones—, so an empty list, or
// a command that is not one of those, returns empty instead of sending something that is
// going to bounce.
std::vector<std::string> enqueue(const std::vector<std::string>& command);
bool canEnqueue(const std::string& verb);
std::vector<std::string> jobList();
std::vector<std::string> jobStatus(const std::string& id);
std::vector<std::string> cancelJob(const std::string& id);

}  // namespace zfsmgr::commands::requests
