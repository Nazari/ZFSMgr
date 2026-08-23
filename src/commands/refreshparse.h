#pragma once

#include <map>
#include <string>
#include <vector>

// Parsing of the answers the agent gives when a connection is refreshed. No Qt.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::refresh {

// The tools the application checks for on the remote host.
//
// The list is deliberately short. It used to probe awk, grep, sort, find, mktemp, printf,
// cat, gzip, pv and sudo as well: those were needed by the shell pipelines the application
// used to send, and the daemon uses none of them. They were left over from the previous
// model, and probing them only served to show a list of «missing» tools that affected
// nothing.
std::vector<std::string> zfsmgrUnixCommandSet();

// Lowercase, and without the braces the Windows registry puts around it.
std::string normalizeMachineUuid(std::string s);

// Pulls a machine id out of free-form output: first the hyphenated form, then 32 straight
// digits, and if neither is there it settles for the first line.
std::string extractMachineUuid(const std::string& text);

// «KEY=value», one per line. The key is upper-cased; the value is kept as-is after
// trimming, because it can carry anything —an '=' included—.
std::map<std::string, std::string> parseKeyValueOutput(const std::string& text);

struct PoolGuidStatus {
    std::string guid;
    std::string status;
};

// Splits the batched pool-status answer, delimited by `__ZFSMGR_*__` markers. The status
// lines are kept WITHOUT trimming each one: the indentation of `zpool status` is part of
// what gets shown.
std::map<std::string, PoolGuidStatus> parsePoolGuidStatusBatch(const std::string& text);

}  // namespace zfsmgr::base::refresh
