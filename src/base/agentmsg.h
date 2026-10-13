#pragma once

#include <string>
#include <vector>

// The agent's messages as CODES, not sentences.
//
// The agent used to answer every failure with a sentence already written —in Spanish up to
// 1.0, in English since then— and the client printed it as it came: it could not translate
// it, nor decide anything by it without comparing strings. Now the agent sends a stable CODE
// and its ARGUMENTS (`"code"` and `"args"` in the RPC answer) instead of the sentence, and the
// client writes the sentence in its own language. `stderr` only carries text that is not the
// agent's own —`zfs` output with no code around it—.
//
// What comes from OpenZFS cannot be coded: it is `zfs`'s own text. It goes as an ARGUMENT of
// a code («cannot destroy %1: %2»), so at least the context is translated.
namespace zfsmgr::base::agentmsg {

// The English sentence, with %1, %2… replaced. What the agent writes in `stderr` and in its
// log. Empty for an unknown code.
std::string inEnglish(const std::string& code, const std::vector<std::string>& args);

// The sentence in the client's language (`i18n::language()`). Empty for an unknown code: the
// caller keeps the text that came.
std::string inClientLanguage(const std::string& code, const std::vector<std::string>& args);

bool isKnown(const std::string& code);

// What the client shows: the sentence in its language, or —a code this client does not know—
// the code with its arguments. There is no other text to fall back on: the agent sends the
// code INSTEAD of a sentence.
std::string render(const std::string& code, const std::vector<std::string>& args);

// The same, with the arguments as the JSON array a job's status carries (`ERROR_ARGS=`,
// `error_args`). Empty when there is no code: the job did not fail with one.
std::string renderJsonArgs(const std::string& code, const std::string& argsJson);

// The code of the last answer read by the transport IN THIS THREAD, empty if it had none. For
// the few places that decide by the kind of failure («the daemon does not have this verb»)
// without changing the signature of everything in between, which only carries text.
const std::string& lastCode();
void setLastCode(const std::string& code);

}  // namespace zfsmgr::base::agentmsg
