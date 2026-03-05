#include "mainwindow_helpers.h"

#include <iostream>

namespace {
int fail(const char* msg) {
    std::cerr << "[FAIL] " << msg << "\n";
    return 1;
}
} // namespace

int main() {
    using namespace mwhelpers;

    if (oneLine("  hola   mundo  ", 64) != "hola mundo") {
        return fail("oneLine should simplify spaces");
    }
    if (oneLine("abcdef", 3) != "abc") {
        return fail("oneLine should truncate to maxLen");
    }

    if (shSingleQuote("abc") != "'abc'") {
        return fail("shSingleQuote basic quoting failed");
    }
    if (shSingleQuote("a'b") != "'a'\"'\"'b'") {
        return fail("shSingleQuote quote escaping failed");
    }

    if (!isMountedValueTrue("yes") || !isMountedValueTrue("ON") || isMountedValueTrue("no")) {
        return fail("isMountedValueTrue values mismatch");
    }

    if (parentDatasetName("pool/a/b") != "pool/a") {
        return fail("parentDatasetName nested path failed");
    }
    if (!parentDatasetName("pool").isEmpty()) {
        return fail("parentDatasetName root should be empty");
    }

    if (normalizeDriveLetterValue("z:\\") != "Z") {
        return fail("normalizeDriveLetterValue drive path failed");
    }
    if (!normalizeDriveLetterValue("none").isEmpty()) {
        return fail("normalizeDriveLetterValue none should be empty");
    }

    if (!looksLikePowerShellScript("Get-ChildItem -Path C:\\")) {
        return fail("looksLikePowerShellScript should detect powershell verbs");
    }
    if (looksLikePowerShellScript("echo hello")) {
        return fail("looksLikePowerShellScript false positive");
    }

    ConnectionProfile p;
    p.port = 22;
    p.keyPath = "/tmp/id_rsa";
    const QString sshCmd = sshBaseCommand(p);
    if (!sshCmd.contains("ControlPath=") || !sshCmd.contains("-i '/tmp/id_rsa'")) {
        return fail("sshBaseCommand missing expected options");
    }

    std::cout << "[OK] helpers tests passed\n";
    return 0;
}

