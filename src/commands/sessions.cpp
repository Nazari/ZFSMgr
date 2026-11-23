#include "sessions.h"

#include "json.h"
#include "strutil.h"

#include <algorithm>

namespace zfsmgr::commands::sessions {

namespace J = zfsmgr::base::json;

std::vector<Session> parseList(const std::string& out, const std::string& machine) {
    std::vector<Session> list;
    for (const std::string& raw : zfsmgr::base::split(out, "\n", true)) {
        const std::string line = zfsmgr::base::trim(raw);
        if (line.rfind("SESSION=", 0) != 0) {
            continue;
        }
        J::Value v;
        if (!J::parse(line.substr(8), v) || !v["id"].isString()) {
            continue;
        }
        Session s;
        s.id = v["id"].toString();
        s.kind = v["kind"].toString();
        s.host = v["host"].toString();
        s.user = v["user"].toString();
        s.pid = v["pid"].toInt();
        s.since = v["since"].toInt();
        s.seenAgo = v["seen_ago"].toInt();
        s.machines.push_back(machine);
        list.push_back(std::move(s));
    }
    return list;
}

void merge(std::vector<Session>& into, const std::vector<Session>& more) {
    for (const Session& s : more) {
        const auto it =
            std::find_if(into.begin(), into.end(), [&](const Session& x) { return x.id == s.id; });
        if (it == into.end()) {
            into.push_back(s);
            continue;
        }
        it->seenAgo = std::min(it->seenAgo, s.seenAgo);
        for (const std::string& m : s.machines) {
            if (std::find(it->machines.begin(), it->machines.end(), m) == it->machines.end()) {
                it->machines.push_back(m);
            }
        }
    }
}

std::vector<Session> othersThan(const std::vector<Session>& all, const std::string& ownHost) {
    std::vector<Session> out;
    for (const Session& s : all) {
        if (s.host != ownHost) {
            out.push_back(s);
        }
    }
    return out;
}

}  // namespace zfsmgr::commands::sessions
