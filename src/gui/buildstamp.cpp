#include "buildstamp.h"

#include "zfsmgr/agentversion_generated.h"

namespace zfsmgr::gui {

namespace {

// Whole and in one piece inside the binary, with a prefix that can be searched for: the
// packager (scripts/empaqueta.sh) looks for this build's fingerprint as text in every program
// it bundles, and refuses an interface from another build. Read through a pointer the compiler
// cannot follow, so the string has to exist in the data —the reason is in
// src/base/agentversion.cpp, where the CLI does the same—.
#if defined(_MSC_VER)
const char kStamp[] = "ZFSMGR_HUELLA=" ZFSMGR_AGENT_VERSION_STRING;
#else
__attribute__((used)) const char kStamp[] = "ZFSMGR_HUELLA=" ZFSMGR_AGENT_VERSION_STRING;
#endif

}  // namespace

QString buildStamp() {
#if defined(_MSC_VER)
    const char* volatile opaque = kStamp;
    const char* p = opaque;
#else
    const char* p = kStamp;
    asm volatile("" : "+r"(p));
#endif
    return QString::fromLatin1(p + sizeof("ZFSMGR_HUELLA=") - 1);
}

}  // namespace zfsmgr::gui
