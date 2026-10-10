#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Syncing a file tree BETWEEN TWO MACHINES, over the daemon-to-daemon socket.
//
// It is the remote sibling of `copytree`. It exists because between machines there was no
// decent way to sync with a Windows endpoint: rsync is not there, and the tar fallback
// copies but does NOT delete and cannot simulate, so it does not sync. See the assessment in
// docs/diseno_tecnico_transferencias.md.
//
// **Why not rsync.** rsync between machines would be launched by the source's daemon, which
// would need SSH of its own against the target —its key or its password, not the client's—
// and rsync installed on both sides. Over the daemon-to-daemon socket neither is needed:
// each daemon is spoken to over its mTLS channel, and the data socket authenticates with a
// single-use token.
//
// **The split**: everything that can be worked out without touching the network lives here
// —walking, comparing, deciding what to do, and the wire format—. The sockets are the
// daemon's job, since it already has the relay built and hardened.
namespace zfsmgr::remotetree {

enum class EntryKind {
    Directory,
    File,
    Symlink,      // symbolic; its target travels, not its contents
    HardLink,     // another path to the SAME file, already sent earlier
};

struct Entry {
    // Relative to the root, and ALWAYS with «/». Windows uses «\» on disk, but the wire does
    // not: if each end sent its own separator, no comparison would ever match.
    std::string path;
    EntryKind kind{EntryKind::File};
    std::uint64_t size{0};
    // Whole seconds since the epoch.
    //
    // **Not laziness: the only thing that compares.** NTFS keeps 100 ns, ext4 nanoseconds and
    // HFS+ one second: comparing the exact stamp between any two of them says «different»
    // every time, and every pass would copy the whole tree again. It is the same granularity
    // `rsync --modify-window=1` uses, and it has the same consequence: a change within the
    // same second that also preserves the size goes undetected.
    std::int64_t mtime{0};
    std::uint32_t mode{0};
    // For `Symlink`, where it points. For `HardLink`, the sibling path already sent.
    std::string target;

    // --- Propietario y grupo.
    //
    // Viajan los DOS números y los DOS nombres, y no es redundancia: entre máquinas los
    // números no significan lo mismo. El uid 1000 es «linarese» aquí y puede ser cualquiera
    // allí, así que copiar el número a ciegas entrega los ficheros a quien no toca. En el
    // destino se resuelve por NOMBRE y solo se cae al número si ese nombre no existe allí.
    // Es lo que hace rsync, que tiene exactamente el mismo problema.
    //
    // Vacíos o a cero cuando el manifiesto viene de un agente anterior a esto: entonces no
    // se toca el dueño, que es mejor que ponerlo mal.
    std::uint32_t uid{0};
    std::uint32_t gid{0};
    std::string owner;
    std::string group;

    // Lleva ACL o atributos extendidos (ver hasExtraMetadata). NO viaja en el manifiesto:
    // solo lo usa el emisor para avisar de que eso no se conserva.
    bool extraMetadata{false};
};

// Whether a path carries ACLs or extended attributes that a copy between machines does not
// preserve. Ignores what every file carries without it mattering: the SELinux label, macOS
// provenance, and FreeBSD's trivial NFSv4 ACL (the mode in another form).
bool hasExtraMetadata(const std::string& path);

// Walks `root` and returns its contents, sorted by path.
//
// Hard links are detected by (device, inode) and returned as `HardLink` pointing at the first
// path that brought them in. On Windows they are NOT detected and travel as separate files:
// the API exists but is expensive, and they are rare there. It is said here so that whoever
// reads the result does not believe they were preserved.
bool walk(const std::string& root, std::vector<Entry>& output, std::string& error,
             bool oneFileSystem = false);

// A single regular file as the one entry of a tree rooted at its parent directory: the
// entry's path is just the file name. It is how one file travels between machines with
// the same machinery as a tree, without the receiver having to know the difference.
bool walkSingleFile(const std::string& file, Entry& output, std::string& error);

// Kind, size, time, mode and owner of ANY path —directory, file, link or device— without
// following a symbolic link. For listing and describing a loose path (`#path`); unlike
// walkSingleFile it does not insist on a regular file.
bool describePath(const std::string& path, Entry& output, std::string& error);

// The manifest: what the target already has. One line per entry.
std::string serializeManifest(const std::vector<Entry>& entries);
// How many bytes of names follow one header line of the manifest: path, target and, when
// the line carries them, owner and group. Whoever reads the manifest off a socket needs it
// to know where the next line starts. False if the line is not a manifest entry header.
bool manifestNamesLength(const std::string& line, std::size_t& length);
bool parseManifest(const std::string& text, std::vector<Entry>& output,
                       std::string& error);

enum class Action {
    MakeDirectory,
    Send,
    Symlink_,
    HardLink_,
    Delete,
};

struct Operation {
    Action action{Action::Send};
    Entry entry;
};

struct Plan {
    std::vector<Operation> operations;
    std::uint64_t bytes{0};      // what would have to be transferred
    std::uint64_t unchanged{0};  // what was already right and is left alone
};

// What has to happen for the target to end up like the source.
//
// Deletions go LAST and deepest-first, so a directory is deleted after its contents. The
// other way round, deleting a directory with things inside fails and the error does not
// explain why.
Plan makePlan(const std::vector<Entry>& source, const std::vector<Entry>& target,
            bool deleteExtraneous);

// One readable line per operation, in the style of `rsync -i`. It is what whoever asks for a
// dry run sees, so it says WHAT and on what, not how many.
std::string describe(const Operation& o);

// The header of one operation on the wire: a line of text and, when it is a file, its raw
// bytes behind it.
//
// Format: `<letter> <mode> <mtime> <size> <pathLen> <targetLen>\n` followed by the path and
// the target run together, with no separator. The lengths are explicit because a file name
// can carry newlines and spaces inside it.
std::string headerOf(const Operation& o);
bool parseHeader(const std::string& line, Operation& output, std::size_t& pathLen,
                 std::size_t& targetLen,
                 // Los nombres de dueño y grupo también viajan por la cabecera, y
                 // quien la lee tiene que saber cuántos bytes vienen detrás. Cero
                 // cuando la manda un agente anterior a esto.
                 std::size_t& ownerLen, std::size_t& groupLen, std::string& error);

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

// How big a block is for a file of that size. In bands and not by square root: it is
// predictable, and both ends computing the SAME thing matters more than tuning it.
// Pone el dueño y el grupo de una ruta, resolviendo POR NOMBRE.
//
// El número no vale entre máquinas: el uid 1000 es una persona aquí y otra allí, así que
// copiarlo a ciegas entrega los ficheros a quien no toca. Se busca el nombre en el sistema
// de destino y solo si no existe se cae al número, que es lo que hace rsync.
//
// Con el nombre y el número vacíos NO SE TOCA NADA: es lo que llega de un agente anterior a
// esto, y dejar el dueño como esté vale más que ponerlo mal.
//
// Devuelve false solo si se intentó y falló. Cambiar el dueño exige ser root: el agente lo
// es, pero si alguna vez no lo fuera, esto lo dice en vez de callarlo.
bool setOwner(const std::string& path, std::uint32_t uid, std::uint32_t gid,
              const std::string& owner, const std::string& group);

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

// Putting on the target the mtime and the mode the source carried.
//
// The mtime has to be set ALWAYS after writing a file: leaving the one from the moment of the
// copy makes the next pass see it as different and fetch it whole again. It is the difference
// between syncing and copying over and over.
bool setMtime(const std::string& path, std::int64_t seconds);
bool setMode(const std::string& path, std::uint32_t mode);

}  // namespace zfsmgr::remotetree
