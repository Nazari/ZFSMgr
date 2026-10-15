#include "pools.h"

#include <cctype>
#include <map>
#include <set>

#include "strutil.h"

namespace zfsmgr::commands::pools {

const char* subcommand(Operation op) {
    switch (op) {
        case Operation::Scrub:      return "scrub";
        case Operation::Trim:       return "trim";
        case Operation::Initialize: return "initialize";
        case Operation::Clear:      return "clear";
        case Operation::Sync:       return "sync";
        case Operation::Export:     return "export";
        case Operation::Import:     return "import";
        case Operation::Destroy:    return "destroy";
        case Operation::Upgrade:    return "upgrade";
        case Operation::Reguid:     return "reguid";
    }
    return "";
}

bool acceptsPhase(Operation op) {
    switch (op) {
        case Operation::Scrub:
        case Operation::Trim:
        case Operation::Initialize:
            return true;
        case Operation::Clear:
        case Operation::Sync:
        case Operation::Export:
        case Operation::Import:
        case Operation::Destroy:
        case Operation::Upgrade:
        case Operation::Reguid:
            return false;
    }
    return false;
}

bool isIrreversible(Operation op) {
    switch (op) {
        case Operation::Destroy:   // se lleva el pool y sus datos
        case Operation::Upgrade:   // no se puede bajar la versión
        case Operation::Reguid:    // el identificador viejo no vuelve
            return true;
        case Operation::Scrub:
        case Operation::Trim:
        case Operation::Initialize:
        case Operation::Clear:
        case Operation::Sync:
        case Operation::Export:
        case Operation::Import:
            return false;
    }
    return false;
}

bool needsConfirmation(Operation op) {
    if (isIrreversible(op)) {
        return true;
    }
    switch (op) {
        // No borra datos, pero borra la CUENTA DE ERRORES del pool, y se teclea queriendo
        // limpiar el terminal.
        case Operation::Clear:
        // No borra nada, pero el pool DESAPARECE de esa máquina hasta que alguien lo
        // importe, y lo que estuviera usándolo se queda sin él.
        case Operation::Export:
            return true;
        case Operation::Scrub:
        case Operation::Trim:
        case Operation::Initialize:
        case Operation::Sync:
        case Operation::Import:
        case Operation::Destroy:
        case Operation::Upgrade:
        case Operation::Reguid:
            return false;
    }
    return false;
}

namespace {

// La letra de la fase, que NO es la misma para las tres.
const char* letraDeFase(Operation op, Phase fase) {
    if (fase == Phase::Start) {
        return nullptr;
    }
    if (op == Operation::Scrub) {
        return fase == Phase::Stop ? "-s" : "-p";
    }
    // trim e initialize: cancelar es `-c` y suspender es `-s`.
    return fase == Phase::Stop ? "-c" : "-s";
}

}  // namespace

std::vector<std::string> argv(Operation op, const std::string& pool, Phase fase,
                              const std::vector<std::string>& banderas,
                              const std::vector<std::string>& discos) {
    const std::string p = zfsmgr::base::trim(pool);
    if (p.empty()) {
        return {};
    }
    if (fase != Phase::Start && !acceptsPhase(op)) {
        return {};
    }
    std::vector<std::string> out{subcommand(op)};
    if (const char* letra = letraDeFase(op, fase)) {
        out.push_back(letra);
    }
    // Banderas ANTES del pool: detrás, zpool las ignora en silencio.
    for (const std::string& b : banderas) {
        const std::string t = zfsmgr::base::trim(b);
        if (!t.empty()) {
            out.push_back(t);
        }
    }
    out.push_back(p);
    // Y los discos al final: delante, zpool lee el primero como nombre de pool.
    for (const std::string& d : discos) {
        const std::string t = zfsmgr::base::trim(d);
        if (!t.empty()) {
            out.push_back(t);
        }
    }
    return out;
}

bool isValidPoolName(const std::string& nombre) {
    const std::string n = zfsmgr::base::trim(nombre);
    if (n.empty()) {
        return false;
    }
    if (std::isalpha(static_cast<unsigned char>(n[0])) == 0) {
        return false;
    }
    return n.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "0123456789_-.:") == std::string::npos;
}

namespace {

bool startsWith(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
}

// A value as the shell's grammar reads it back: bare when it is one word, in double quotes
// otherwise.
std::string quoted(const std::string& v) {
    if (!v.empty() && v.find_first_of(" \t\"'\\") == std::string::npos) {
        return v;
    }
    std::string out = "\"";
    for (const char c : v) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

}  // namespace

CopiedOptions optionsToCopy(const std::vector<SourcedProperty>& poolProps,
                            const std::vector<SourcedProperty>& rootProps,
                            const std::function<bool(const std::string&)>& rootSupported) {
    // A property set by hand to the value it would have anyway adds nothing to the line:
    // `-o failmode=wait` creates the same pool as without it. Measured, not taken from
    // memory: what `zpool get`/`zfs get` report with source «default» on Linux and on
    // FreeBSD (OpenZFS 2.4, 2026-10-15), keeping only the values both agree on. The ones
    // that differ by platform —acltype, xattr, dnodesize, relatime— are never «the default»
    // here, and neither is `compression`, whose default no machine at hand showed.
    static const std::map<std::string, std::string> kPoolDefaults = {
        {"ashift", "0"},          {"autoexpand", "off"},         {"autoreplace", "off"},
        {"autotrim", "off"},      {"compatibility", "off"},      {"comment", "-"},
        {"delegation", "on"},     {"dedup_table_quota", "auto"}, {"failmode", "wait"},
        {"listsnapshots", "off"}, {"multihost", "off"}};
    static const std::map<std::string, std::string> kRootDefaults = {{"aclinherit", "restricted"},
                                                                     {"aclmode", "discard"},
                                                                     {"atime", "on"},
                                                                     {"canmount", "on"},
                                                                     {"checksum", "on"},
                                                                     {"context", "none"},
                                                                     {"copies", "1"},
                                                                     {"dedup", "off"},
                                                                     {"defcontext", "none"},
                                                                     {"devices", "on"},
                                                                     {"direct", "standard"},
                                                                     {"exec", "on"},
                                                                     {"filesystem_limit", "none"},
                                                                     {"fscontext", "none"},
                                                                     {"logbias", "latency"},
                                                                     {"longname", "off"},
                                                                     {"mlslabel", "none"},
                                                                     {"nbmand", "off"},
                                                                     {"overlay", "on"},
                                                                     {"prefetch", "all"},
                                                                     {"primarycache", "all"},
                                                                     {"quota", "none"},
                                                                     {"readonly", "off"},
                                                                     {"recordsize", "128K"},
                                                                     {"redundant_metadata", "all"},
                                                                     {"refquota", "none"},
                                                                     {"refreservation", "none"},
                                                                     {"reservation", "none"},
                                                                     {"rootcontext", "none"},
                                                                     {"secondarycache", "all"},
                                                                     {"setuid", "on"},
                                                                     {"sharenfs", "off"},
                                                                     {"sharesmb", "off"},
                                                                     {"snapdev", "hidden"},
                                                                     {"snapdir", "hidden"},
                                                                     {"snapshot_limit", "none"},
                                                                     {"special_small_blocks", "0"},
                                                                     {"sync", "standard"},
                                                                     {"volmode", "default"},
                                                                     {"vscan", "off"}};
    const auto esPorDefecto = [](const std::map<std::string, std::string>& tabla,
                                 const SourcedProperty& p) {
        const auto it = tabla.find(p.name);
        return it != tabla.end()
               && (it->second == p.value || (it->second == "-" && p.value.empty()));
    };
    static const std::set<std::string> kPoolImportTime = {"altroot", "cachefile", "readonly"};
    static const std::set<std::string> kRootNever = {"mountpoint", "encryption", "keyformat",
                                                     "keylocation", "pbkdf2iters"};
    CopiedOptions out;
    for (const SourcedProperty& p : poolProps) {
        if (zfsmgr::base::toLowerAscii(p.source) != "local" || startsWith(p.name, "feature@")) {
            continue;
        }
        if (kPoolImportTime.count(p.name)) {
            out.skipped.push_back(p.name + "=" + p.value);
            continue;
        }
        if (!esPorDefecto(kPoolDefaults, p)) {
            out.pool.emplace_back(p.name, p.value);
        }
    }
    for (const SourcedProperty& p : rootProps) {
        if (zfsmgr::base::toLowerAscii(p.source) != "local") {
            continue;
        }
        if (kRootNever.count(p.name) || p.name.find(':') != std::string::npos
            || (rootSupported && !rootSupported(p.name))) {
            out.skipped.push_back(p.name + "=" + p.value);
            continue;
        }
        if (!esPorDefecto(kRootDefaults, p)) {
            out.rootFs.emplace_back(p.name, p.value);
        }
    }
    return out;
}

std::string rewriteWithOptions(const std::string& line, const CopiedOptions& opts,
                               const std::vector<std::pair<std::string, std::string>>& given) {
    // Split into words keeping each one as typed —quotes included—, so what is not touched
    // comes back exactly as it was written.
    std::vector<std::string> words;
    std::string cur;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            cur += c;
            if (c == '\\' && i + 1 < line.size()) {
                cur += line[++i];
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                words.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        }
        cur += c;
    }
    if (!cur.empty()) {
        words.push_back(cur);
    }
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (words[i] == "--opts-as") {
            ++i;  // and its value
            continue;
        }
        out += (out.empty() ? "" : " ") + words[i];
    }
    std::set<std::string> already;  // «-o:ashift», «-O:compression»
    for (const auto& g : given) {
        const std::size_t eq = g.second.find('=');
        already.insert((g.first == "-o" ? "-o:" : "-O:") + g.second.substr(0, eq));
    }
    for (const auto& kv : opts.pool) {
        if (!already.count("-o:" + kv.first)) {
            out += " -o " + quoted(kv.first + "=" + kv.second);
        }
    }
    for (const auto& kv : opts.rootFs) {
        if (!already.count("-O:" + kv.first)) {
            out += " -O " + quoted(kv.first + "=" + kv.second);
        }
    }
    return out;
}

}  // namespace zfsmgr::commands::pools
