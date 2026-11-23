#pragma once

#include <QString>

namespace zfsmgr::gui {

// This build's fingerprint, «1.4.3.123456789»: the same string the zfsmgr-cli and the agents
// of the same build carry. Shown in Administration and by `--version`.
QString buildStamp();
// «ZFSMgr 1.4.3.123456789»: the title of every window of its own, so a screenshot or a
// question always says which build it was.
QString appTitle();

}  // namespace zfsmgr::gui
