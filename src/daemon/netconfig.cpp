// The network's configuration, kept by every daemon (docs/diseno_red_unica.md, phase 2).
//
// The daemon keeps the document as it comes: its secrets are under a data key no daemon has,
// and its HMAC is checked by the clients, which can. What the daemon checks is the shape
// (network::fromJson) and the revisions: a client's write (`--net-put`) is accepted only on top
// of the revision kept —two writers do not overwrite each other—, and a peer's offer
// (`--net-offer`) only if it wins (network::offeredWins). Whatever is adopted is offered on to
// the peers, and a thread catches up with them at start and every few minutes, both ways: a
// machine that was off gets the network when it comes back.
//
// The last versions are kept beside it (network.d/rev-<n>.json): a daemon broken into could
// serve garbage with a higher revision —the clients reject it, it does not verify— and the
// good one must still be somewhere.

#include "agent_internal.h"

#include "network.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace zfsmgr::agent {

namespace {

namespace NW = zfsmgr::base::network;
namespace J = zfsmgr::base::json;

std::mutex g_net;

// A document in base64 is at most this long: ~40 KB today for six machines.
constexpr std::size_t kMaxB64 = 2 * 1024 * 1024;
constexpr int kKeepVersions = 5;
constexpr auto kSyncEvery = std::chrono::minutes(5);

struct Kept {
    long long revision{0};
    std::string mac;
    std::string text;  // the document as stored, compact JSON
};

Kept readKeptLocked() {
    Kept k;
    std::ifstream f(networkPath());
    if (!f.is_open()) {
        return k;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    J::Value v;
    NW::Document d;
    std::string err;
    if (!J::parse(ss.str(), v, &err) || !NW::fromJson(v, d, err)) {
        daemonLog("WARN", "network.json unreadable, taken as empty: " + err);
        return k;
    }
    k.revision = d.revision;
    k.mac = d.mac;
    k.text = J::toCompact(v);
    return k;
}

// The document received, in base64: decoded and its shape checked. `why` when it is not one.
bool decode(const std::string& b64, J::Value& v, NW::Document& d, std::string& why) {
    if (b64.size() > kMaxB64) {
        why = "too large";
        return false;
    }
    std::string text;
    if (!zfsmgr::base::base64Decode(b64, text)) {
        why = "not base64";
        return false;
    }
    if (!J::parse(text, v, &why) || !NW::fromJson(v, d, why)) {
        return false;
    }
    if (d.revision < 1 || d.mac.empty()) {
        why = "no revision or no mac";
        return false;
    }
    return true;
}

void restrictToRoot(const std::filesystem::path& p) {
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
#else
    (void)p;
#endif
}

// Writes it in place, atomically, after keeping the one it replaces among the last versions.
bool writeLocked(const J::Value& v, long long revision, const Kept& previous) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path(networkPath());
    fs::create_directories(path.parent_path(), ec);
    if (previous.revision > 0) {
        const fs::path dir = path.parent_path() / "network.d";
        fs::create_directories(dir, ec);
        const fs::path old = dir / ("rev-" + std::to_string(previous.revision) + ".json");
        {
            std::ofstream f(old, std::ios::trunc);
            f << previous.text;
        }
        restrictToRoot(old);
        std::vector<std::pair<long long, fs::path>> versions;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (n.rfind("rev-", 0) == 0 && n.size() > 9) {
                try {
                    versions.push_back({std::stoll(n.substr(4, n.size() - 9)), e.path()});
                } catch (...) {
                }
            }
        }
        std::sort(versions.begin(), versions.end());
        while (static_cast<int>(versions.size()) > kKeepVersions) {
            fs::remove(versions.front().second, ec);
            versions.erase(versions.begin());
        }
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) {
            daemonLog("ERROR", "network: cannot write " + tmp.string());
            return false;
        }
        f << J::toCompact(v);
    }
    restrictToRoot(tmp);
    fs::rename(tmp, path, ec);
    if (ec) {
        daemonLog("ERROR", "network: cannot put " + path.string() + " in place: " + ec.message());
        return false;
    }
    daemonLog("INFO", "network: revision " + std::to_string(revision) + " kept");
    return true;
}

// The document offered to every peer, in a thread of its own: the client's write is answered at
// once, and a peer that is off costs nothing but its own attempt.
void offerToPeers(const std::string& text) {
    const std::string b64 = zfsmgr::base::base64Encode(text);
    std::thread([b64]() {
        for (const std::string& id : gsaIdsDePares()) {
            GsaPeer peer;
            if (!gsaCargaPeer(id, peer)) {
                continue;
            }
            std::string out;
            std::string err;
            if (gsaPeerRpc(peer, "--net-offer", {b64}, out, err) != GsaRpc::Ok) {
                daemonLog("INFO", "network: could not offer it to " + id + ": " + err);
            }
        }
    }).detach();
}

// A peer's document, adopted if it wins. True when adopted (and then offered on).
bool adoptIfWins(const J::Value& v, const NW::Document& d, const std::string& from) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_net);
        const Kept kept = readKeptLocked();
        if (!NW::offeredWins(kept.revision, kept.mac, d.revision, d.mac)) {
            return false;
        }
        if (!writeLocked(v, d.revision, kept)) {
            return false;
        }
        text = J::toCompact(v);
    }
    daemonLog("INFO", "network: revision " + std::to_string(d.revision) + " adopted from " + from);
    offerToPeers(text);
    return true;
}

}  // namespace

const std::string& networkPath() {
    static const std::string p = [] {
        const char* e = std::getenv("ZFSMGR_NETWORK_FILE");
#ifdef _WIN32
        return std::string(e && *e ? e : "C:\\ProgramData\\ZFSMgr\\agent\\network.json");
#else
        return std::string(e && *e ? e : "/etc/zfsmgr/network.json");
#endif
    }();
    return p;
}

const std::string& peersPath() {
    static const std::string p = [] {
        const char* e = std::getenv("ZFSMGR_PEERS_FILE");
#ifdef _WIN32
        return std::string(e && *e ? e : "C:\\ProgramData\\ZFSMgr\\agent\\peers.json");
#else
        return std::string(e && *e ? e : "/etc/zfsmgr/peers.json");
#endif
    }();
    return p;
}

// `--net-get`: REVISION, MAC and, when there is one, DOC (base64). REVISION=0: none yet.
ExecResult rpcNetGet(const std::vector<std::string>&) {
    ExecResult r;
    Kept kept;
    {
        std::lock_guard<std::mutex> lock(g_net);
        kept = readKeptLocked();
    }
    r.rc = 0;
    r.out = "REVISION=" + std::to_string(kept.revision) + "\nMAC=" + kept.mac + "\n";
    if (kept.revision > 0) {
        r.out += "DOC=" + zfsmgr::base::base64Encode(kept.text) + "\n";
    }
    return r;
}

// `--net-put <base-revision> <document-base64>`: a client's write, on top of the revision it
// started from. Another one written meanwhile: net_conflict with the revision kept, for the
// client to fetch it, apply its change again and retry.
ExecResult rpcNetPut(const std::vector<std::string>& args) {
    ExecResult r;
    long long base = -1;
    if (args.size() != 2) {
        r.rc = 1;
        ponCodigo(r, "usage", {"--net-put <base-revision> <document-base64>"});
        return r;
    }
    try {
        base = std::stoll(args[0]);
    } catch (...) {
        base = -1;
    }
    J::Value v;
    NW::Document d;
    std::string why;
    if (base < 0 || !decode(args[1], v, d, why)) {
        r.rc = 1;
        ponCodigo(r, "net_invalid", {base < 0 ? std::string("base revision") : why});
        return r;
    }
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_net);
        const Kept kept = readKeptLocked();
        if (kept.revision != base || d.revision <= kept.revision) {
            r.rc = 1;
            ponCodigo(r, "net_conflict", {std::to_string(kept.revision)});
            return r;
        }
        if (!writeLocked(v, d.revision, kept)) {
            r.rc = 1;
            ponCodigo(r, "cannot_put_in_place", {networkPath()});
            return r;
        }
        text = J::toCompact(v);
    }
    offerToPeers(text);
    r.rc = 0;
    r.out = "REVISION=" + std::to_string(d.revision) + "\n";
    return r;
}

// `--net-offer <document-base64>`: a peer's. Kept if it wins; ADOPTED says whether it did.
ExecResult rpcNetOffer(const std::vector<std::string>& args) {
    ExecResult r;
    J::Value v;
    NW::Document d;
    std::string why;
    if (args.size() != 1 || !decode(args[0], v, d, why)) {
        r.rc = 1;
        ponCodigo(r, "net_invalid", {args.size() != 1 ? std::string("usage") : why});
        return r;
    }
    const bool adopted = adoptIfWins(v, d, "a peer");
    long long now = 0;
    {
        std::lock_guard<std::mutex> lock(g_net);
        now = readKeptLocked().revision;
    }
    r.rc = 0;
    r.out = std::string("ADOPTED=") + (adopted ? "1" : "0") + "\nREVISION=" + std::to_string(now) + "\n";
    return r;
}

void runNetworkSyncThread() {
    // A little after start: the peers' daemons may be starting too.
    std::this_thread::sleep_for(std::chrono::seconds(20));
    while (true) {
        for (const std::string& id : gsaIdsDePares()) {
            GsaPeer peer;
            if (!gsaCargaPeer(id, peer)) {
                continue;
            }
            std::string out;
            std::string err;
            if (gsaPeerRpc(peer, "--net-get", {}, out, err) != GsaRpc::Ok) {
                continue;
            }
            long long theirs = 0;
            std::string theirMac;
            std::string doc;
            std::istringstream in(out);
            for (std::string line; std::getline(in, line);) {
                if (line.rfind("REVISION=", 0) == 0) {
                    try {
                        theirs = std::stoll(line.substr(9));
                    } catch (...) {
                    }
                } else if (line.rfind("MAC=", 0) == 0) {
                    theirMac = line.substr(4);
                } else if (line.rfind("DOC=", 0) == 0) {
                    doc = line.substr(4);
                }
            }
            Kept kept;
            {
                std::lock_guard<std::mutex> lock(g_net);
                kept = readKeptLocked();
            }
            if (theirs > 0 && NW::offeredWins(kept.revision, kept.mac, theirs, theirMac)) {
                J::Value v;
                NW::Document d;
                std::string why;
                if (decode(doc, v, d, why)) {
                    adoptIfWins(v, d, id);
                }
            } else if (kept.revision > 0 && NW::offeredWins(theirs, theirMac, kept.revision, kept.mac)) {
                // Theirs is behind: they get ours (a machine that was off when it changed).
                gsaPeerRpc(peer, "--net-offer", {zfsmgr::base::base64Encode(kept.text)}, out, err);
            }
        }
        std::this_thread::sleep_for(kSyncEvery);
    }
}

}  // namespace zfsmgr::agent
