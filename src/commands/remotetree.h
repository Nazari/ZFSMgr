#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fsmeta.h"

// Syncing a file tree BETWEEN TWO MACHINES, over the daemon-to-daemon socket.
//
// It is the remote sibling of `copytree`, and preserves the SAME things: owner (by name),
// mode, nanosecond timestamps, symbolic links (with their own owner and time), hard links
// (also of symlinks and special files), special files, holes, extended attributes and ACLs.
// What the other end cannot hold —a Linux POSIX ACL on FreeBSD, anything on Windows— is
// counted and reported, never lost in silence. Measured attribute by attribute against the
// local copy over the same tree.
//
// **Why not rsync.** rsync between machines would be launched by the source's daemon, which
// would need SSH of its own against the target —its key or its password, not the client's—
// and rsync installed on both sides. Over the daemon-to-daemon socket neither is needed:
// each daemon is spoken to over its mTLS channel, and the data socket authenticates with a
// single-use token.
//
// **The split**: everything that can be worked out without touching the network lives here
// —walking, comparing, deciding what to do, the wire format, and applying an entry to disk—.
// The sockets are the daemon's job, since it already has the relay built and hardened.
//
// **The conversation** (no compatibility with earlier versions of it):
//   1. sender → receiver: the token.
//   2. receiver → sender: `R <family>\n` and the manifest of what it has.
//   3. sender → receiver: which large files it wants signatures of; receiver answers them.
//   4. sender → receiver: the operations, then `E`.
//   5. receiver → sender: `OK <done>` / `PARCIAL <done> <failed> <first reason>` / `ERR …`.
namespace zfsmgr::remotetree {

enum class EntryKind {
    Directory,
    File,
    Symlink,      // symbolic; its target travels, not its contents
    HardLink,     // another path to the SAME inode, already sent earlier
    Special,      // FIFO, character or block device, socket
};

struct Entry {
    // Relative to the root, and ALWAYS with «/». Windows uses «\» on disk, but the wire does
    // not: if each end sent its own separator, no comparison would ever match.
    std::string path;
    EntryKind kind{EntryKind::File};
    std::uint64_t size{0};
    // Seconds since the epoch, and the nanoseconds within that second. They are compared with
    // `sameMtime`, which allows for filesystems that keep less (see there).
    std::int64_t mtime{0};
    std::uint32_t mtimeNsec{0};
    std::uint32_t mode{0};
    // For `Symlink`, where it points. For `HardLink`, the sibling path already sent.
    std::string target;

    // --- Propietario y grupo.
    //
    // Viajan los DOS números y los DOS nombres: entre máquinas los números no significan lo
    // mismo. En el destino se resuelve por NOMBRE y solo se cae al número si ese nombre no
    // existe allí, que es lo que hace rsync. Vacíos y a cero cuando el origen no tiene dueños
    // de Unix (Windows): entonces no se toca el dueño, que es mejor que ponerlo mal.
    std::uint32_t uid{0};
    std::uint32_t gid{0};
    std::string owner;
    std::string group;
    // Sender only: the numbers the receiver will put (see expectAtReceiver).
    std::uint32_t expectedUid{0};
    std::uint32_t expectedGid{0};
    bool expectedKnown{false};

    // `Special`: 'p' FIFO, 'c' character device, 'b' block device, 's' socket; and the device
    // numbers, by major and minor because `dev_t` is not the same thing on every system.
    char special{'-'};
    std::uint32_t devMajor{0};
    std::uint32_t devMinor{0};

    // Extended attributes and ACLs. They travel with each OPERATION; the manifest carries
    // only `metaDigest`, enough to tell whether they differ.
    fsmeta::Metadata meta;
    std::string metaDigest;
};

// --- Dueños por NOMBRE, comparados por NÚMERO.
//
// El receptor pone el dueño por nombre y, si ese nombre no existe allí, por número. Comparar
// por nombre en el emisor veía distinto, en CADA pasada, un fichero cuyo grupo («nogroup»)
// no existe en el destino y que allí se llama de otra forma («nobody»). Como rsync: el
// emisor pregunta al receptor qué número tiene allí cada nombre, y compara números.
struct OwnerMap {
    std::map<std::string, std::uint32_t> users;   // solo los que existen en el receptor
    std::map<std::string, std::uint32_t> groups;
};
// The names `entries` use, to ask the receiver about.
void namesUsed(const std::vector<Entry>& entries, std::vector<std::string>& users,
               std::vector<std::string>& groups);
// On the receiver: which of those names exist here, and with which number.
OwnerMap resolveHere(const std::vector<std::string>& users, const std::vector<std::string>& groups);
// The number each entry will end up with at the receiver (its name's number there, or its own
// when the name does not exist there), in `expectedUid`/`expectedGid`.
void expectAtReceiver(std::vector<Entry>& entries, const OwnerMap& m);

// Equal to the granularity the coarser end keeps: exact when both have nanoseconds; to the
// 100 ns when one of them looks like NTFS; to the second when one of them has no fraction
// (HFS+, FAT, a filesystem mounted without it). Comparing exactly between two different
// filesystems would say «different» every time and copy the whole tree on every pass; and
// comparing only seconds, as this used to, missed a same-size change within the same second.
bool sameMtime(std::int64_t s1, std::uint32_t ns1, std::int64_t s2, std::uint32_t ns2);

// Walks `root` and returns its contents, sorted by path, with their metadata.
//
// Hard links are detected by (device, inode) —of files, symbolic links and special files— and
// returned as `HardLink` pointing at the first path, alphabetically, that brought them in (on
// Windows by volume serial number and file index).
//
// A subdirectory that cannot be read does not stop the walk: it goes into `unreadable` (when
// given) and whoever deletes must not delete anything under it, or an unreadable directory
// would look empty and its copy would be emptied.
bool walk(const std::string& root, std::vector<Entry>& output, std::string& error,
          bool oneFileSystem = false, std::vector<std::string>* unreadable = nullptr);

// A single regular file as the one entry of a tree rooted at its parent directory: the
// entry's path is just the file name.
bool walkSingleFile(const std::string& file, Entry& output, std::string& error);

// Kind, size, time, mode, owner and metadata of ANY path, without following a symbolic link.
bool describePath(const std::string& path, Entry& output, std::string& error);

// The manifest: what the target already has. One line per entry.
std::string serializeManifest(const std::vector<Entry>& entries);
// How many bytes of names follow one header line of the manifest. Whoever reads the manifest
// off a socket needs it to know where the next line starts.
bool manifestNamesLength(const std::string& line, std::size_t& length);
bool parseManifest(const std::string& text, std::vector<Entry>& output, std::string& error);

enum class Action {
    MakeDirectory,
    Send,
    Symlink_,
    HardLink_,
    Special_,
    Metadata_,  // same contents: owner, mode, time or attributes only
    Delete,
};

// What happens to the entry, as the dry run shows it: the same marks as the copy within one
// machine (`+` new, `!` changes type, `>` contents, `.` metadata only, `-` deleted).
enum class Change { Created, Replaced, Content, Metadata, Deleted };

struct Operation {
    Action action{Action::Send};
    Entry entry;
    Change change{Change::Created};
    // Restores the time of a directory the receiver writes into. Not a change of the tree,
    // so not shown nor counted.
    bool silent{false};
};

struct Plan {
    std::vector<Operation> operations;
    std::uint64_t bytes{0};      // what would have to be transferred
    std::uint64_t unchanged{0};  // what was already right and is left alone
    // What could not be done: an entry whose place at the target is taken by a NON-EMPTY
    // directory, without deletion enabled (rsync refuses that too without --force).
    std::vector<std::string> refused;
};

// What has to happen for the target to end up like the source. `source` must carry
// `metaDigest` computed over what the target can hold (see fsmeta::fitFor).
//
// Deletions go LAST and deepest-first, and nothing is deleted under a directory listed in
// `unreadableSource`.
Plan makePlan(const std::vector<Entry>& source, const std::vector<Entry>& target,
              bool deleteExtraneous, const std::vector<std::string>& unreadableSource = {});

// One line per operation for the dry run: the mark and the path («/» behind a directory).
// Empty for a silent one.
std::string describe(const Operation& o);

// The header of one operation on the wire: a line of text, then the path, target, owner,
// group and serialized metadata run together, with their lengths in the line because a file
// name can carry newlines and spaces inside it.
std::string headerOf(const Operation& o);
struct HeaderLengths {
    std::size_t path{0};
    std::size_t target{0};
    std::size_t owner{0};
    std::size_t group{0};
    std::size_t meta{0};
    std::size_t total() const { return path + target + owner + group + meta; }
};
bool parseHeader(const std::string& line, Operation& output, HeaderLengths& lengths,
                 std::string& error);
// Fills the names of `o` from the bytes that followed its header.
bool takeNames(const std::string& names, const HeaderLengths& l, Operation& o,
               std::string& error);

// --- Contenido con huecos.
//
// Un fichero viaja por TRAMOS: de datos, con sus bytes detrás, y de hueco, que es solo una
// longitud. Así un disco virtual de 100 GB con 8 GB usados no manda 92 GB de ceros por la red
// ni llega ocupándolos.
//
// Calls `emit(data, n)` for each stretch, in order: `data` null means a hole of `n` bytes.
// Holes are what the filesystem reports (SEEK_DATA/SEEK_HOLE) and any 4 KiB block of zeros.
bool readStretches(const std::string& path, std::uint64_t size,
                   const std::function<bool(const char* data, std::size_t n)>& emit,
                   std::string& error);

// Writes a new file leaving holes: blocks of zeros are skipped instead of written, and
// `finish` sets the final size.
class SparseWriter {
public:
    SparseWriter() = default;
    SparseWriter(const SparseWriter&) = delete;
    SparseWriter& operator=(const SparseWriter&) = delete;
    ~SparseWriter();
    bool open(const std::string& path, std::string& error);
    bool write(const char* data, std::size_t n);
    void skip(std::uint64_t n) { pos_ += n; }
    bool finish(std::string& error);
    std::uint64_t position() const { return pos_; }

private:
#ifdef _WIN32
    void* file_{nullptr};
#else
    int fd_{-1};
#endif
    std::uint64_t pos_{0};
    std::string path_;
};

// Where to write a file before putting it in its place: next to it, with a short name (the
// target may already be 255 bytes long).
std::string tempPathNextTo(const std::string& path);

// --- Aplicar en el destino.

// Puts on `path` the owner, attributes, mode, ACL and time of `e`, in that order (see
// copytree's applyMetadata for why). Returns how many parts could NOT be applied.
int applyMetadata(const std::string& path, const Entry& e);

// Creates the special file `e` describes at `path`.
bool makeSpecial(const std::string& path, const Entry& e, std::string& error);

// A hard link that does not follow a symbolic link: a link to a symlink is a link to it.
bool makeHardLink(const std::string& existing, const std::string& link, std::string& error);

// ---------------------------------------------------------------------------
// DELTA transfer: sending only what changed inside a file.
//
// This is rsync's algorithm, and NOT xdelta. xdelta computes the difference between two
// files that are both on the same machine; here neither machine has both, which is the whole
// problem. rsync's is designed for exactly this shape:
//
//   1. The TARGET splits its copy into blocks and sends, per block, a weak rolling sum and a
//      strong hash.
//   2. The SOURCE slides a window byte by byte over its version. The weak sum updates in
//      O(1) per byte —that is the trick—, and when it matches a known one it is confirmed
//      with the strong hash.
//   3. It sends instructions: «copy N of your blocks from index i» or «here come these
//      bytes».
//
// Byte by byte and not block by block on purpose: if someone inserts one byte at the start of
// the file, comparing aligned blocks would recognise not a single one, whereas the sliding
// window recognises all of them, shifted.
// ---------------------------------------------------------------------------

// The size above which it pays off. Below it, the signatures and the network round trip cost
// more than sending the whole file; rsync applies a threshold for the same reason.
constexpr std::uint64_t kMinSizeForDelta = 1024 * 1024;

struct Signature {
    std::uint32_t weak{0};
    // Truncated SHA-256. Truncating is fine because the strong hash only confirms a match the
    // weak sum already proposed, and because the WHOLE file is checked at the end: a
    // collision here is caught there instead of corrupting silently.
    unsigned char strong[16]{};
};

// Pone el dueño y el grupo de una ruta, resolviendo POR NOMBRE y cayendo al número si ese
// nombre no existe aquí. Con el nombre y el número vacíos NO SE TOCA NADA. Devuelve false
// solo si se intentó y falló.
bool setOwner(const std::string& path, std::uint32_t uid, std::uint32_t gid,
              const std::string& owner, const std::string& group);

// How big a block is for a file of that size. In bands and not by square root: it is
// predictable, and both ends computing the SAME thing matters more than tuning it.
std::size_t blockSize(std::uint64_t fileSize);

// rsync's rolling sum over a chunk.
std::uint32_t rollingSum(const unsigned char* data, std::size_t n);

// The hash of the whole file, to check that what was reconstructed is what it had to be.
bool fileHash(const std::string& path, std::string& hexOut, std::string& error);

bool signaturesOf(const std::string& path, std::size_t blockSz, std::vector<Signature>& output,
              std::string& error);
std::string serializeSignatures(const std::vector<Signature>& f);
bool parseSignatures(const std::string& data, std::vector<Signature>& output, std::string& error);

enum class InstructionKind { Send, Literal };

struct Instruction {
    InstructionKind kind{InstructionKind::Literal};
    std::uint64_t block{0};    // Send: the target's first block
    std::uint64_t howMany{0};  // Send: how many consecutive blocks
    std::string data;          // Literal: the bytes
};

// What has to be sent for the target to reconstruct `path` from what it already has.
//
// `literalBytes` is what would actually travel: when it comes out close to the file's size,
// the delta bought nothing and the caller may prefer to send the file whole.
bool delta(const std::string& path, const std::vector<Signature>& signatures, std::size_t blockSz,
           std::vector<Instruction>& output, std::uint64_t& literalBytes, std::string& error);

// The time of `path` itself (a symbolic link's own, not its target's), to the nanosecond
// where the filesystem keeps it.
bool setMtime(const std::string& path, std::int64_t seconds, std::uint32_t nanoseconds = 0);
bool setMode(const std::string& path, std::uint32_t mode);

}  // namespace zfsmgr::remotetree
