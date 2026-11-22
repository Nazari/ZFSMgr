#pragma once

#include <QString>

namespace zfsmgr::gui {

// This build's fingerprint, «1.4.3.123456789»: the same string the zfsmgr-cli and the agents
// of the same build carry. Shown in Administration and by `--version`.
QString buildStamp();

}  // namespace zfsmgr::gui
