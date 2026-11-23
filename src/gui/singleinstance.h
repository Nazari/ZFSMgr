#pragma once

// One session of ZFSMgr per machine (src/base/sessionlock.h), from the interface's side.
//
// A second interface of the same user does not open: it asks the first to show itself and
// ends. Anything else holding the session —the CLI's shell, someone else's interface, or a
// first interface that does not answer— is said in a dialog that offers to close it.

class QObject;

namespace zfsmgr::base::session {
class Lock;
}

namespace zfsmgr::gui {

// True: the session is this one's (held by `lock` until the program ends), or it could not
// be checked. False: another one stays open, and this one must end.
bool takeSession(zfsmgr::base::session::Lock& lock);

// Answers a second launch of the same user by bringing this one's windows to the front.
void listenForRaise(QObject* parent);

}  // namespace zfsmgr::gui
