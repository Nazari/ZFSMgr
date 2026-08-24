#pragma once

#include <string>

// Translation without Qt, reading the SAME files the interface uses.
//
// `i18n/<language>.json`, with a `translations` object mapping key to text. The existing
// catalogue is reused instead of inventing one for the CLI: same languages, same files, and
// they already travel inside the installer and the AppImage. Two translation systems in one
// program end up disagreeing.
//
// **When there is no translation, it returns the source text.** The Spanish is written in
// the code, so a catalogue that is missing, incomplete or corrupt degrades to «it comes out
// in Spanish» and never to «a key comes out» or «nothing comes out», which is what makes a
// tool unreadable.
namespace zfsmgr::base::i18n {

// Which language is in use. Empty or unknown = «es». «es-ES», «en_US» and the like are
// accepted: it keeps the first two letters.
void setLanguage(const std::string& language);
const std::string& language();

// Where to look for the catalogues, on top of the usual places. Set by whoever knows where
// the program is installed.
void addSearchPath(const std::string& dir);

// The translated text, or `fallback` when there is none.
const std::string& tr(const std::string& key, const std::string& fallback);

}  // namespace zfsmgr::base::i18n
