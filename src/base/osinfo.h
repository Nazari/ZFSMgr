#pragma once

#include <string>

// Which operating system runs on an endpoint, from what that endpoint answers.
//
// **Only text is INTERPRETED here.** Who launches the process and how its output arrives is
// the caller's business: the daemon reads it off its own disk, the interface brings it over
// SSH from the other side. That separation is exactly what was missing, and it is why there
// were three different versions of the same thing:
//
//   - `daemon_main.cpp:detectOsLine()` opened `/etc/os-release` and parsed it in C++.
//   - `connectiondialog.cpp` and `mainwindow_refresh.cpp` each sent a
//     `sh -lc '. /etc/os-release; printf "%s %s" "$NAME" "$VERSION_ID"'`.
//
// The two Qt ones delegated to a shell a job the daemon's already did without one, and on
// top of that they disagreed at the edges: the shell `printf` leaves a stray space when
// `VERSION_ID` is absent —Arch and Gentoo do not carry it—, and it does not strip the quotes
// around the value, which `/etc/os-release` does use. The daemon's did both.
//
// With the parsing here, what travels over SSH becomes `cat /etc/os-release`: a command with
// nothing to interpret, instead of a script.
namespace zfsmgr::base::osinfo {

// The contents of `/etc/os-release` → «Fedora Linux 42».
//
// Empty when the file says nothing useful, so that the caller supplies its own fallback
// («Linux» on its own) rather than a half-finished string.
//
// It strips the quotes from the values: the format allows them —`NAME="Fedora Linux"`— and
// leaving them in showed up on the connection's card.
std::string fromOsRelease(const std::string& contents);

// The output of `system_profiler SPSoftwareDataType` → «macOS 15.5 (24F74)».
//
// The «System Version:» line is looked for and whatever follows is returned. This used to be
// a `sed -n "s/^ *System Version: //p" | head -1` inside the remote script.
std::string fromSystemProfiler(const std::string& output);

}  // namespace zfsmgr::base::osinfo
