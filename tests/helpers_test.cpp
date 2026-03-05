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

    if (!parentMountCheckRequired("/mnt/a", "on")) {
        return fail("parentMountCheckRequired should require mounted parent");
    }
    if (parentMountCheckRequired("none", "on")) {
        return fail("parentMountCheckRequired should skip when mountpoint=none");
    }
    if (parentMountCheckRequired("/mnt/a", "off")) {
        return fail("parentMountCheckRequired should skip when canmount=off");
    }
    if (!parentAllowsChildMount("/mnt/a", "on", "yes")) {
        return fail("parentAllowsChildMount mounted parent should pass");
    }
    if (parentAllowsChildMount("/mnt/a", "on", "no")) {
        return fail("parentAllowsChildMount unmounted parent should fail");
    }
    if (!parentAllowsChildMount("none", "on", "no")) {
        return fail("parentAllowsChildMount should bypass none mountpoint");
    }

    {
        QMap<QString, QString> mps;
        mps.insert("pool/a", "/mnt/x");
        mps.insert("pool/b", "/mnt/x");
        mps.insert("pool/c", "/mnt/y");
        const QMap<QString, QStringList> dup = duplicateMountpoints(mps);
        if (!dup.contains("/mnt/x") || dup.value("/mnt/x").size() != 2) {
            return fail("duplicateMountpoints should report duplicated mountpoint");
        }
        if (dup.contains("/mnt/y")) {
            return fail("duplicateMountpoints should not include unique mountpoint");
        }
    }

    {
        QMap<QString, QString> targetMps;
        targetMps.insert("pool/a", "/mnt/x");
        targetMps.insert("pool/b", "/mnt/y");
        QMap<QString, QStringList> mountedByMp;
        mountedByMp.insert("/mnt/x", QStringList{"pool/a"});
        mountedByMp.insert("/mnt/y", QStringList{"pool/other"});
        const QVector<MountpointConflict> conflicts = externalMountpointConflicts(targetMps, mountedByMp);
        if (conflicts.size() != 1) {
            return fail("externalMountpointConflicts should detect one conflict");
        }
        if (conflicts[0].mountpoint != "/mnt/y" || conflicts[0].mountedDataset != "pool/other"
            || conflicts[0].requestedDataset != "pool/b") {
            return fail("externalMountpointConflicts conflict payload mismatch");
        }
    }

    {
        const QString cmdUnix = buildHasMountedChildrenCommand(false, "pool/a");
        if (!cmdUnix.contains("DATASET='pool/a'") || !cmdUnix.contains("index($1, ds \"/\")==1")) {
            return fail("buildHasMountedChildrenCommand unix mismatch");
        }
        const QString cmdWin = buildHasMountedChildrenCommand(true, "pool/a");
        if (!cmdWin.contains("$children=@(zfs list -H -o name -r $ds")) {
            return fail("buildHasMountedChildrenCommand windows mismatch");
        }
    }

    {
        const QString cmdUnix = buildRecursiveUmountCommand(false, "pool/alpha");
        if (!cmdUnix.contains("DATASET='pool/alpha'") || !cmdUnix.contains("zfs umount")) {
            return fail("buildRecursiveUmountCommand unix mismatch");
        }
        const QString cmdWin = buildRecursiveUmountCommand(true, "pool/alpha");
        if (!cmdWin.contains("Sort-Object") || !cmdWin.contains("zfs unmount")) {
            return fail("buildRecursiveUmountCommand windows mismatch");
        }
    }

    {
        if (buildSingleUmountCommand(false, "pool/a") != "zfs umount 'pool/a'") {
            return fail("buildSingleUmountCommand unix mismatch");
        }
        if (buildSingleUmountCommand(true, "pool/a") != "zfs unmount 'pool/a'") {
            return fail("buildSingleUmountCommand windows mismatch");
        }
    }

    if (!looksLikePowerShellScript("Get-ChildItem -Path C:\\")) {
        return fail("looksLikePowerShellScript should detect powershell verbs");
    }
    if (looksLikePowerShellScript("echo hello")) {
        return fail("looksLikePowerShellScript false positive");
    }
    if (!isWindowsOsType("Windows 11")) {
        return fail("isWindowsOsType should detect windows");
    }
    if (isWindowsOsType("Linux")) {
        return fail("isWindowsOsType false positive");
    }

    ConnectionProfile p;
    p.username = "u";
    p.host = "h";
    p.port = 22;
    p.keyPath = "/tmp/id_rsa";
    const QString sshCmd = sshBaseCommand(p);
    if (!sshCmd.contains("ControlPath=") || !sshCmd.contains("-i '/tmp/id_rsa'")) {
        return fail("sshBaseCommand missing expected options");
    }
    const QString sshPreview = buildSshPreviewCommandText(p, "zpool list");
    if (!sshPreview.contains("ssh") || !sshPreview.contains("u@h") || !sshPreview.contains("'zpool list'")) {
        return fail("buildSshPreviewCommandText mismatch");
    }

    ConnectionProfile sudoLinux;
    sudoLinux.osType = "Linux";
    sudoLinux.useSudo = true;
    sudoLinux.password = "pw";
    const QString sudoCmd = withSudoCommand(sudoLinux, "zpool list");
    if (!sudoCmd.contains("sudo -S") || !sudoCmd.contains("zpool list")) {
        return fail("withSudoCommand linux/password mismatch");
    }
    const QString sudoStream = withSudoStreamInputCommand(sudoLinux, "zfs recv pool");
    if (!sudoStream.contains("cat;") || !sudoStream.contains("sudo -S")) {
        return fail("withSudoStreamInputCommand linux/password mismatch");
    }
    ConnectionProfile win;
    win.osType = "Windows";
    win.useSudo = true;
    if (withSudoCommand(win, "cmd") != "cmd" || withSudoStreamInputCommand(win, "cmd2") != "cmd2") {
        return fail("withSudo* should no-op for windows");
    }

    if (parseOpenZfsVersionText("zfs-2.3.4-1") != "2.3.4") {
        return fail("parseOpenZfsVersionText should parse zfs-* string");
    }
    if (parseOpenZfsVersionText("OpenZFS version: 2.4.0") != "2.4.0") {
        return fail("parseOpenZfsVersionText should parse OpenZFS text");
    }
    if (!parseOpenZfsVersionText("no version here").isEmpty()) {
        return fail("parseOpenZfsVersionText should return empty when no version");
    }

    const QString importText =
        "pool: tank\n"
        "state: ONLINE\n"
        "status: The pool can be imported.\n"
        "action: Import the pool using 'zpool import'.\n"
        "\n"
        "pool: backup\n"
        "state: UNAVAIL\n"
        "status: One or more devices are unavailable.\n"
        " cannot import 'backup': no such pool available\n";
    const QVector<ImportablePoolInfo> pools = parseZpoolImportOutput(importText);
    if (pools.size() != 2) {
        return fail("parseZpoolImportOutput should parse two pool blocks");
    }
    if (pools[0].pool != "tank" || pools[0].state != "ONLINE") {
        return fail("parseZpoolImportOutput first pool mismatch");
    }
    if (pools[1].pool != "backup" || pools[1].state != "UNAVAIL") {
        return fail("parseZpoolImportOutput second pool mismatch");
    }
    if (!pools[1].reason.contains("cannot import")) {
        return fail("parseZpoolImportOutput should keep detailed reason");
    }

    {
        TransferButtonInputs in;
        in.srcDatasetSelected = true;
        in.srcSnapshotSelected = true;
        in.dstDatasetSelected = true;
        in.dstSnapshotSelected = false;
        in.srcSelectionKey = "pool/a@s1";
        in.dstSelectionKey = "pool/b";
        in.srcSelectionConsistent = true;
        in.dstSelectionConsistent = true;
        in.srcDatasetMounted = true;
        in.dstDatasetMounted = true;
        const TransferButtonState st = computeTransferButtonState(in);
        if (!st.copyEnabled || !st.levelEnabled || st.syncEnabled) {
            return fail("computeTransferButtonState snapshot copy/level mismatch");
        }
    }

    {
        TransferButtonInputs in;
        in.srcDatasetSelected = true;
        in.srcSnapshotSelected = false;
        in.dstDatasetSelected = true;
        in.dstSnapshotSelected = false;
        in.srcSelectionKey = "pool/a";
        in.dstSelectionKey = "pool/b";
        in.srcSelectionConsistent = true;
        in.dstSelectionConsistent = true;
        in.srcDatasetMounted = true;
        in.dstDatasetMounted = true;
        const TransferButtonState st = computeTransferButtonState(in);
        if (st.copyEnabled || !st.levelEnabled || !st.syncEnabled) {
            return fail("computeTransferButtonState dataset sync mismatch");
        }
    }

    {
        TransferButtonInputs in;
        in.srcDatasetSelected = true;
        in.srcSnapshotSelected = false;
        in.dstDatasetSelected = true;
        in.dstSnapshotSelected = false;
        in.srcSelectionKey = "pool/a";
        in.dstSelectionKey = "pool/a"; // same selection disables level/sync
        in.srcSelectionConsistent = true;
        in.dstSelectionConsistent = true;
        in.srcDatasetMounted = true;
        in.dstDatasetMounted = true;
        const TransferButtonState st = computeTransferButtonState(in);
        if (st.levelEnabled || st.syncEnabled) {
            return fail("computeTransferButtonState same-selection gate mismatch");
        }
    }

    {
        TransferButtonInputs in;
        in.srcDatasetSelected = true;
        in.srcSnapshotSelected = false;
        in.dstDatasetSelected = true;
        in.dstSnapshotSelected = false;
        in.srcSelectionKey = "pool/a";
        in.dstSelectionKey = "pool/b";
        in.srcSelectionConsistent = true;
        in.dstSelectionConsistent = true;
        in.srcDatasetMounted = false; // mount required for sync
        in.dstDatasetMounted = true;
        const TransferButtonState st = computeTransferButtonState(in);
        if (st.syncEnabled) {
            return fail("computeTransferButtonState sync mount gate mismatch");
        }
    }

    std::cout << "[OK] helpers tests passed\n";
    return 0;
}
