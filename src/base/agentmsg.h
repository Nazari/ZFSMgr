#pragma once

#include <string>
#include <vector>

// The agent's messages as CODES, not sentences.
//
// The agent used to answer every failure with a sentence already written —in Spanish up to
// 1.0, in English since then— and the client printed it as it came: it could not translate
// it, nor decide anything by it without comparing strings. Now the agent sends, next to that
// English sentence, a stable CODE and its ARGUMENTS (`"code"` and `"args"` in the RPC answer),
// and the client writes the sentence in its own language.
//
// Both directions keep working with the other side old:
//   - an old client ignores the new fields and prints the English sentence, as always;
//   - a new client talking to an old agent gets no code and prints the sentence it got.
// A code this client does not know —a newer agent— also leaves the English sentence.
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

// The code of the last answer read by the transport IN THIS THREAD, empty if it had none. For
// the few places that decide by the kind of failure («the daemon does not have this verb»)
// without changing the signature of everything in between, which only carries text.
const std::string& lastCode();
void setLastCode(const std::string& code);

}  // namespace zfsmgr::base::agentmsg
