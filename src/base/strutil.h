#pragma once

#include <string>
#include <vector>

// String utilities for the Qt-free layer.
//
// They exist so that client logic could be pulled out without dragging Qt along with it.
// They are the specific operations the ported code used —not one more—: trimming, replacing
// literals, formatting with positional markers and quoting for the shell.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

// Strips whitespace from both ends, like QString::trimmed().
std::string trim(const std::string& s);

// Replaces EVERY occurrence of `from` in `s`. It does nothing when `from` is empty, which
// would otherwise be an infinite loop.
void replaceAll(std::string& s, const std::string& from, const std::string& to);

// Formats by replacing %1..%99 with the arguments, in ONE single pass.
//
// The single pass is not an implementation detail, it is the semantics: QString::arg() with
// several arguments does NOT look again inside what it has just inserted, so an argument
// containing «%2» stays literal. Substituting in a chain would break any string with a
// percent sign inside it —passwords, Windows paths— and it would be a hard failure to spot.
// Verified against Qt: `API='%2%1'`.
//
// A marker whose number exceeds the arguments given is left as-is, again like Qt.
std::string format(const std::string& tmpl, const std::vector<std::string>& args);

// Quotes for passing as ONE argument to a POSIX shell. The single quote is closed, escaped
// in quotes and reopened: '"'"' — the only way of getting one inside.
std::string shSingleQuote(const std::string& s);

// --- Operations that in Qt are methods of QString.
//
// Free functions on purpose: wrapping std::string in a class with QString's API would make
// the port more comfortable today and leave the project with a home-made QString clone,
// which is the exact opposite of the goal.

// Collapses each run of whitespace into one and trims, like QString::simplified().
std::string simplify(const std::string& s);

// ASCII ONLY, and the name says so on purpose. Qt also changes the case of accented
// letters; here it does not, and that is what is wanted: these are used to compare property
// values («yes», «on»), GUIDs and verb names, all ASCII. A conversion with language rules
// introduces surprises —the Turkish I is the classic example— right where a decision is
// being made.
std::string toLowerAscii(const std::string& s);
std::string toUpperAscii(const std::string& s);

bool contains(const std::string& s, const std::string& sub);
bool startsWith(const std::string& s, const std::string& pre);
bool endsWith(const std::string& s, const std::string& suf);

// They return -1 when there is no match, like QString::indexOf().
long long indexOf(const std::string& s, const std::string& sub);
long long lastIndexOf(const std::string& s, const std::string& sub);

// Cuts by CHARACTERS, not by bytes, and forgiving with out-of-range positions.
//
// Counting bytes here would be a real bug, not an imprecision: `left(s, 220)` is what trims
// the log lines, and cutting in the middle of a UTF-8 character leaves invalid bytes. It was
// found by comparing against Qt with «áÉ». It matches Qt across the whole basic plane; it
// only diverges on characters outside it, where Qt counts UTF-16 units.
std::string left(const std::string& s, std::size_t nChars);
std::string mid(const std::string& s, std::size_t posChars);
std::string mid(const std::string& s, std::size_t posChars, std::size_t nChars);

// Index of the byte where character number `nChars` starts, or the size when it overruns.
std::size_t byteOfChar(const std::string& s, std::size_t nChars);

// UTF-8-aware case, for when the text is NOT ASCII and the decision depends on it. The case
// that forced it: `looksLikeSudoAuthFailure` compares against phrases with accents, and with
// «SUDO: 1 INTENTO DE CONTRASEÑA INCORRECTO» the ASCII version answered that there was no
// password failure. A rejection would have been classified as «could not check», which is
// the very failure that function's comments say it has already suffered.
//
// It covers ASCII, the Latin-1 supplement and Latin Extended-A: Spanish, French, German,
// Portuguese and much of Eastern Europe. It does NOT cover Greek, Cyrillic, the Turkish I or
// the ß->SS expansion; there it returns the character untouched. Checked against Qt across
// the whole U+0000..U+017F range.
std::string toLowerUtf8(const std::string& s);
std::string toUpperUtf8(const std::string& s);

// Is the character starting at byte `pos` a letter? ASCII plus the same Latin ranges as
// above.
bool isLetterAt(const std::string& s, std::size_t pos);

// Standard base64 (RFC 4648) with padding. `base64Decode` returns false when a character
// outside the alphabet turns up; whitespace is ignored and padding ends the input.
std::string base64Encode(const std::string& data);

// A size in bytes, spelled the way `zfs` spells it: one decimal digit below 10 and none
// above —«9.5G», «500G»—. Anything that is not a number is returned as-is, which is what is
// needed when the source already gave text.
//
// It lives here because more than one client writes it. It used to be inside the shell's
// table, and the window was going to need the same one to show what the agent now gives it
// in bytes.
std::string humanSize(const std::string& v);
bool base64Decode(const std::string& text, std::string& out);

// `skipEmpty` imitates Qt::SkipEmptyParts, which is how it is used almost everywhere.
std::vector<std::string> split(const std::string& s, const std::string& sep, bool skipEmpty);
std::string join(const std::vector<std::string>& parts, const std::string& sep);

}  // namespace zfsmgr::base
