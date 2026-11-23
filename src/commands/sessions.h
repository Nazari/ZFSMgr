#pragma once

// The other interactive sessions of ZFSMgr —interfaces and CLI shells on a terminal— as the
// daemons say (`--session-list`, see «Presencia» in docs/daemonize.md). Pure: what is asked
// and printed lives in the CLI (shell_sessions.cpp).

#include <string>
#include <vector>

namespace zfsmgr::commands::sessions {

struct Session {
    std::string id;
    std::string kind;  // «gui» or «cli»
    std::string host;  // the machine it runs on
    std::string user;
    long long pid{0};
    long long since{0};    // seconds since the epoch
    long long seenAgo{0};  // seconds since a daemon last heard of it: the freshest of them
    // The connections whose daemon knows it, in the order they were merged.
    std::vector<std::string> machines;
};

// The SESSION={…} lines of one daemon's answer; `machine` is the connection that gave it.
// Lines that cannot be read are skipped.
std::vector<Session> parseList(const std::string& out, const std::string& machine);

// Adds `more` to `into`: the same id is the same session, seen from another machine.
void merge(std::vector<Session>& into, const std::vector<Session>& more);

// Those not running on `ownHost`. There is one interactive session per machine
// (base/sessionlock.h), so the one on this machine is this one.
std::vector<Session> othersThan(const std::vector<Session>& all, const std::string& ownHost);

}  // namespace zfsmgr::commands::sessions
