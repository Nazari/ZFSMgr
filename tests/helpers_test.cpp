#include "mainwindow_helpers.h"

#include <QRegularExpression>

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
    if (windowsGptTypeName("6A898CC3-1DD2-11B2-99A6-080020736631").isEmpty()) {
        return fail("windowsGptTypeName known guid should resolve");
    }
    if (!windowsGptTypeName("{516E7CBA-6ECF-11D6-8FF8-00022D09712B}").contains("FreeBSD/ZFS")) {
        return fail("windowsGptTypeName should accept braces");
    }
    const QString fsDetail = formatWindowsFsTypeDetail("NTFS|gpt={6A898CC3-1DD2-11B2-99A6-080020736631}|type=Unknown|mbr=-");
    if (!fsDetail.contains("gpt=Mac OS X/ZFS / Solaris/usr")) {
        return fail("formatWindowsFsTypeDetail should include mapped GPT name");
    }
    if (fsDetail.contains("{6A898CC3-1DD2-11B2-99A6-080020736631}")) {
        return fail("formatWindowsFsTypeDetail should hide GUID when mapping exists");
    }
    if (!formatWindowsFsTypeDetail("NTFS|gpt={AAAAAAAA-0000-0000-0000-000000000000}|type=Unknown|mbr=-")
             .contains("gpt={AAAAAAAA-0000-0000-0000-000000000000}")) {
        return fail("formatWindowsFsTypeDetail should keep unknown GPT unchanged");
    }
    if (!windowsPartitionTypeIsProtected("NTFS|gpt=-|type=System|mbr=-")
        || !windowsPartitionTypeIsProtected("NTFS|gpt=-|type=Recovery|mbr=-")
        || !windowsPartitionTypeIsProtected("NTFS|gpt=-|type=Reserved|mbr=-")
        || windowsPartitionTypeIsProtected("NTFS|gpt=-|type=Basic|mbr=-")) {
        return fail("windowsPartitionTypeIsProtected mismatch");
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
        const QString mountOut =
            "pool/a /mnt/a\n"
            "pool/b /mnt/with spaces/path\n"
            "   pool/c    /mnt/c   \n"
            "invalid_line_without_mp\n";
        const QVector<QPair<QString, QString>> rows = parseZfsMountOutput(mountOut);
        if (rows.size() != 3) {
            return fail("parseZfsMountOutput should parse valid rows only");
        }
        if (rows[0].first != "pool/a" || rows[0].second != "/mnt/a") {
            return fail("parseZfsMountOutput first row mismatch");
        }
        if (rows[1].first != "pool/b" || rows[1].second != "/mnt/with spaces/path") {
            return fail("parseZfsMountOutput should preserve mountpoint spaces");
        }
        if (rows[2].first != "pool/c" || rows[2].second != "/mnt/c") {
            return fail("parseZfsMountOutput should trim spacing");
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

    {
        if (buildSingleMountCommand("pool/a") != "zfs mount 'pool/a'") {
            return fail("buildSingleMountCommand mismatch");
        }
        const QString cmdUnix = buildMountChildrenCommand(false, "pool/a");
        if (!cmdUnix.contains("DATASET='pool/a'") || !cmdUnix.contains("zfs list -H -o name -r")) {
            return fail("buildMountChildrenCommand unix mismatch");
        }
        const QString cmdWin = buildMountChildrenCommand(true, "pool/a");
        if (!cmdWin.contains("$items = @(zfs list -H -o name -r $ds") || !cmdWin.contains("zfs mount $child")) {
            return fail("buildMountChildrenCommand windows mismatch");
        }
        const QString precheck = buildWindowsMountPrecheckCommand("pool/a", "Z:\\Data");
        if (!precheck.contains("$ds='pool/a'") || !precheck.contains("$mp='Z:\\Data'")
            || !precheck.contains("mountpoint ocupado por ruta existente no-ZFS")) {
            return fail("buildWindowsMountPrecheckCommand mismatch");
        }
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
    if (sshUserHost(p) != "u@h") {
        return fail("sshUserHost mismatch");
    }
    if (sshUserHostPort(p) != "u@h:22") {
        return fail("sshUserHostPort explicit port mismatch");
    }
    ConnectionProfile pDefPort = p;
    pDefPort.port = 0;
    if (sshUserHostPort(pDefPort) != "u@h:22") {
        return fail("sshUserHostPort default port mismatch");
    }
    if (!buildSshTargetPrefix(p).contains("u@h")) {
        return fail("buildSshTargetPrefix mismatch");
    }
    const QString sshCmd = sshBaseCommand(p);
    if (!sshCmd.contains("ControlPath=") || !sshCmd.contains("-i '/tmp/id_rsa'")) {
        return fail("sshBaseCommand missing expected options");
    }
    const QString simpleSsh = buildSimpleSshInvocation(p, "zpool list");
    if (!simpleSsh.contains("'u@h'") || !simpleSsh.contains("'zpool list'")) {
        return fail("buildSimpleSshInvocation mismatch");
    }
    if (!streamProgressPipeFilter().contains("pv -trab -f")) {
        return fail("streamProgressPipeFilter mismatch");
    }
    const QString piped = buildPipedTransferCommand("A", "B");
    if (!piped.contains("A | ") || !piped.contains(" | B") || !piped.contains("pv -trab -f")) {
        return fail("buildPipedTransferCommand mismatch");
    }
    if (streamCodecName(StreamCodec::Zstd) != "zstd-fast"
        || streamCodecName(StreamCodec::Gzip) != "gzip-fast"
        || streamCodecName(StreamCodec::None) != "none") {
        return fail("streamCodecName mismatch");
    }
    if (chooseStreamCodec(true, true) != StreamCodec::Zstd
        || chooseStreamCodec(false, true) != StreamCodec::Gzip
        || chooseStreamCodec(false, false) != StreamCodec::None) {
        return fail("chooseStreamCodec mismatch");
    }
    const QString tarSrcUnix = buildTarSourceCommand(false, "/mnt/data", StreamCodec::Zstd);
    if (!tarSrcUnix.contains("tar --acls --xattrs -cpf - -C '/mnt/data' . | zstd -1 -T0 -q -c")) {
        return fail("buildTarSourceCommand unix zstd mismatch");
    }
    const QString tarSrcWin = buildTarSourceCommand(true, "Z:\\Data", StreamCodec::Gzip);
    if (!tarSrcWin.contains("$p='Z:\\Data'; tar -cf - -C $p . | gzip -1 -c")) {
        return fail("buildTarSourceCommand windows gzip mismatch");
    }
    const QString tarDstUnix = buildTarDestinationCommand(false, "/mnt/dst", StreamCodec::None);
    if (!tarDstUnix.contains("mkdir -p '/mnt/dst' && tar --acls --xattrs -xpf - -C '/mnt/dst'")) {
        return fail("buildTarDestinationCommand unix none mismatch");
    }
    const QString tarDstWin = buildTarDestinationCommand(true, "Z:\\Dst", StreamCodec::Zstd);
    if (!tarDstWin.contains("$p='Z:\\Dst'") || !tarDstWin.contains("zstd -d -q -c - | tar -xpf - -C $p")) {
        return fail("buildTarDestinationCommand windows zstd mismatch");
    }
    const QString sshPreview = buildSshPreviewCommandText(p, "zpool list");
    if (!sshPreview.contains("ssh") || !sshPreview.contains("u@h") || !sshPreview.contains("'zpool list'")) {
        return fail("buildSshPreviewCommandText mismatch");
    }

    ConnectionProfile sudoLinux;
    sudoLinux.osType = "Linux";
    sudoLinux.useSudo = true;
    sudoLinux.password = "pw";
    // Matched by pattern, not by the literal "sudo -S": the password is fed on
    // stdin through extra hardening flags (-k to drop any cached credential, -p ''
    // to silence the prompt), and pinning the exact spelling made this test break
    // the moment those were added.
    const QRegularExpression sudoStdinRx(QStringLiteral("\\bsudo (?:-\\w+ )*-S\\b"));
    const QString sudoCmd = withSudoCommand(sudoLinux, "zpool list");
    if (!sudoStdinRx.match(sudoCmd).hasMatch() || !sudoCmd.contains("zpool list")) {
        return fail("withSudoCommand linux/password mismatch");
    }
    const QString sudoStream = withSudoStreamInputCommand(sudoLinux, "zfs recv pool");
    if (!sudoStream.contains("cat;") || !sudoStdinRx.match(sudoStream).hasMatch()) {
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
    if (!parseOpenZfsVersionText("Darwin Kernel Version 25.3.0").isEmpty()) {
        return fail("parseOpenZfsVersionText should ignore OS/kernel versions");
    }
    if (parseOpenZfsVersionText("zpool version: 2.3.1\nzfs version: 2.3.1") != "2.3.1") {
        return fail("parseOpenZfsVersionText should parse zpool/zfs version output");
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

    const QString importTextNoisy =
        "pool: good_pool\n"
        "state: ONLINE\n"
        "status: First line.\n"
        " second status line.\n"
        "action: do stuff\n"
        "\n"
        "pool: bad pool name\n"
        "state: ONLINE\n"
        "status: Should be ignored because pool name is invalid.\n";
    const QVector<ImportablePoolInfo> noisyPools = parseZpoolImportOutput(importTextNoisy);
    if (noisyPools.size() != 1 || noisyPools[0].pool != "good_pool") {
        return fail("parseZpoolImportOutput should ignore invalid pool names");
    }
    if (!noisyPools[0].reason.contains("First line") || !noisyPools[0].reason.contains("second status line")) {
        return fail("parseZpoolImportOutput should preserve multiline status reason");
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

    // Ida y vuelta del renderizado a cadena. Es el test que define el problema que
    // motivó la migración a argv: un argumento con ';', '&' o '|' —un directorio
    // elegido por el usuario, por ejemplo— truncaba la orden al recuperarla.
    {
        ConnectionProfile unixProfile;
        unixProfile.osType = QStringLiteral("Linux");
        unixProfile.useSudo = false;
        const QVector<QStringList> corpus = {
            {QStringLiteral("--mutate-advanced-todir"), QStringLiteral("tank/x"),
             QStringLiteral("/home/x/Copias & Backups"), QStringLiteral("1")},
            {QStringLiteral("--dump-x"), QStringLiteral("a;b")},
            {QStringLiteral("--dump-x"), QStringLiteral("a|b")},
            {QStringLiteral("--dump-x"), QStringLiteral("con 'comilla'")},
            {QStringLiteral("--dump-x"), QStringLiteral("")},
            {QStringLiteral("--dump-x"), QStringLiteral("ñ 漢字")},
            {QStringLiteral("--dump-x"), QStringLiteral("$HOME")},
            {QStringLiteral("--dump-x"), QStringLiteral("a\\b")},
        };
        for (const QStringList& argv : corpus) {
            const QString rendered = mwhelpers::agentShellCommand(unixProfile, argv);
            const int marker = rendered.lastIndexOf(QStringLiteral("zfsmgr-agent"));
            if (marker < 0) {
                return fail("agentShellCommand debe contener la ruta del agente");
            }
            const QString tail = rendered.mid(marker + QStringLiteral("zfsmgr-agent").size()).trimmed();
            const QStringList back = mwhelpers::posixShellSplitArgs(tail);
            if (back != argv) {
                std::cout << "  esperado: " << argv.join(QLatin1Char('|')).toStdString() << "\n";
                std::cout << "  obtenido: " << back.join(QLatin1Char('|')).toStdString() << "\n";
                return fail("el renderizado a cadena no conserva los argumentos");
            }
        }
    }

    // Los verbos que transportan flujos por la entrada estándar nunca deben ir por RPC.
    {
        if (!mwhelpers::isCliOnlyAgentCommand(QStringLiteral("--mutate-shell-generic"))) {
            return fail("--mutate-shell-generic debe ser solo-CLI");
        }
        if (mwhelpers::isCliOnlyAgentCommand(QStringLiteral("--mutate-advanced-todir"))) {
            return fail("--mutate-advanced-todir sí es servible por RPC");
        }
    }

    // Reconocer que sudo rechaza la contraseña es lo que permite ofrecer el arreglo
    // donde el usuario se entera. Y NO debe confundirse con un problema de sudoers,
    // que reintroducir la contraseña no arregla.
    if (!looksLikeSudoAuthFailure("sudo: 1 incorrect password attempt")) {
        return fail("looksLikeSudoAuthFailure should detect a wrong password");
    }
    if (!looksLikeSudoAuthFailure("Sorry, try again.")) {
        return fail("looksLikeSudoAuthFailure should detect 'Sorry, try again'");
    }
    if (!looksLikeSudoAuthFailure("sudo: a password is required")) {
        return fail("looksLikeSudoAuthFailure should detect a missing password");
    }
    // Salidas REALES capturadas de sudo 1.9 en Ubuntu con LANG=es_ES.UTF-8 y de
    // macOS 26. La lista original traía traducciones inventadas que no casaban.
    if (!looksLikeSudoAuthFailure(QString::fromUtf8("Lo siento, pruebe otra vez."))) {
        return fail("looksLikeSudoAuthFailure should detect the real Spanish sudo output");
    }
    if (!looksLikeSudoAuthFailure(QString::fromUtf8("sudo: 1 intento de contraseña incorrecto"))) {
        return fail("looksLikeSudoAuthFailure should detect the Spanish attempt counter");
    }
    if (!looksLikeSudoAuthFailure(QString::fromUtf8("sudo: se requiere una contraseña"))) {
        return fail("looksLikeSudoAuthFailure should detect the Spanish -n refusal");
    }
    // Un «Timeout» PELADO no dice nada, y así llegaba el fallo de instalar el daemon:
    // sudo rechazaba la contraseña a los cuatro segundos, la orden se colgaba hasta los
    // 180 s y arriba solo subía esa palabra. Ahora el motivo llega con la salida pegada,
    // que es lo que este detector sabe leer.
    if (looksLikeSudoAuthFailure(QStringLiteral("Timeout"))) {
        return fail("looksLikeSudoAuthFailure should not invent a cause from a bare timeout");
    }
    if (!looksLikeSudoAuthFailure(
            QStringLiteral("Timeout: sudo: 3 incorrect password attempts\ncat: stdout: Broken pipe"))) {
        return fail("looksLikeSudoAuthFailure should read the cause kept next to the timeout");
    }
    if (looksLikeSudoAuthFailure("user is not in the sudoers file")) {
        return fail("looksLikeSudoAuthFailure must not fire on a sudoers problem");
    }
    if (looksLikeSudoAuthFailure("linarese is not allowed to execute /bin/zfs")) {
        return fail("looksLikeSudoAuthFailure must not fire on an authorization problem");
    }
    if (looksLikeSudoAuthFailure("")) {
        return fail("looksLikeSudoAuthFailure must not fire on empty output");
    }
    if (looksLikeSudoAuthFailure("cannot open 'tank': dataset does not exist")) {
        return fail("looksLikeSudoAuthFailure false positive on an unrelated error");
    }

    // La contraseña de sudo viaja como escapes octales porque en macOS Qt descompone
    // los caracteres al pasar la orden al intérprete: una ñ precompuesta (c3 b1)
    // llegaba como n + tilde combinante (6e cc 83) y sudo la rechazaba.
    if (shPrintfOctalEscaped("abc") != "\\0141\\0142\\0143") {
        return fail("shPrintfOctalEscaped ASCII mismatch");
    }
    // ñ = U+00F1 = c3 b1 -> \0303\0261
    if (shPrintfOctalEscaped(QString::fromUtf8("\xC3\xB1")) != "\\0303\\0261") {
        return fail("shPrintfOctalEscaped should encode UTF-8 bytes, not code points");
    }
    // La comilla simple deja de necesitar entrecomillado especial: es \047.
    if (shPrintfOctalEscaped("'") != "\\0047") {
        return fail("shPrintfOctalEscaped single quote mismatch");
    }
    if (!shPrintfOctalEscaped(QString::fromUtf8("añ'b")).toLatin1().isEmpty()) {
        // Todo el resultado debe ser ASCII: es lo que lo hace inmune a la
        // normalización de macOS.
        const QString enc = shPrintfOctalEscaped(QString::fromUtf8("añ'b"));
        for (const QChar c : enc) {
            if (c.unicode() > 127) {
                return fail("shPrintfOctalEscaped must produce pure ASCII");
            }
        }
    }
    if (!shPrintfOctalEscaped(QString()).isEmpty()) {
        return fail("shPrintfOctalEscaped of an empty string must be empty");
    }
    {
        // Y la orden real debe usar %b con la contraseña ya codificada.
        ConnectionProfile sp;
        sp.osType = "Linux";
        sp.useSudo = true;
        sp.password = QString::fromUtf8("añ'b");
        const QString c = withSudoCommand(sp, "zfs list");
        if (!c.contains("printf \'%b\\n\' \'\\0141\\0303\\0261\\0047\\0142\'")) {
            return fail("withSudoCommand should embed the password as octal escapes");
        }
        if (c.contains(QString::fromUtf8("ñ"))) {
            return fail("withSudoCommand must not embed the password verbatim");
        }
        const QString cs = withSudoStreamInputCommand(sp, "zfs list");
        if (!cs.contains("printf \'%b\\n\'") || cs.contains(QString::fromUtf8("ñ"))) {
            return fail("withSudoStreamInputCommand should do the same");
        }
    }

    {
        // sudo SIN contraseña (clave + NOPASSWD). withUnixSearchPathCommand antepone
        // `PATH="..."; export PATH; `, así que concatenar `sudo -n ` delante partía la
        // línea por los punto y coma: sudo se quedaba sin mandato y el agente se
        // ejecutaba sin privilegios. La orden debe ir entrecomillada tras `sh -c`.
        ConnectionProfile np;
        np.osType = "Linux";
        np.useSudo = true;
        np.password.clear();
        const QString c = withSudoCommand(np, "/usr/local/libexec/zfsmgr-agent --health");
        if (!c.startsWith("sudo -n sh -c ")) {
            return fail("withSudoCommand without a password must use sudo -n sh -c");
        }
        // Nada de punto y coma fuera de las comillas: todo lo que sigue a `sh -c` es
        // un único argumento.
        const QString afterShC = c.mid(QString("sudo -n sh -c ").size());
        if (!afterShC.startsWith('\'') || !afterShC.endsWith('\'')) {
            return fail("withSudoCommand payload must be single-quoted as one argument");
        }
        const QString outside = c.left(QString("sudo -n sh -c ").size());
        if (outside.contains(';')) {
            return fail("withSudoCommand must not leave a ';' outside the quoted command");
        }
        // Y debe seguir llevando dentro tanto el PATH como el mandato.
        if (!c.contains("export PATH") || !c.contains("zfsmgr-agent --health")) {
            return fail("withSudoCommand lost the PATH prefix or the command");
        }
        // La variante con entrada por tubería ya lo hacía bien; que siga igual.
        const QString cs = withSudoStreamInputCommand(np, "/usr/local/libexec/zfsmgr-agent --health");
        if (!cs.startsWith("sudo -n sh -c ")) {
            return fail("withSudoStreamInputCommand without a password regressed");
        }
    }

    {
        // asciiSafeShellCommand: intocable si ya es ASCII (el caso normal), y en ASCII
        // puro si lleva algo que macOS descompondría al pasar por los argumentos de un
        // proceso.
        const QString plain = "zfs list -H -o name tank/datos";
        if (asciiSafeShellCommand(plain) != plain) {
            return fail("asciiSafeShellCommand must leave an ASCII command untouched");
        }
        const QString accented = QString::fromUtf8("zfs list -H -o name tanque/documentación");
        const QString safe = asciiSafeShellCommand(accented);
        if (safe == accented) {
            return fail("asciiSafeShellCommand must rewrite a command with non-ASCII");
        }
        for (const QChar c : safe) {
            if (c.unicode() > 127) {
                return fail("asciiSafeShellCommand output must be pure ASCII");
            }
        }
        if (!safe.startsWith("eval \"$(printf '%b' '") || !safe.endsWith("')\"")) {
            return fail("asciiSafeShellCommand wrapper shape changed");
        }
        // Y los bytes de dentro deben ser los del original en UTF-8: es lo único que
        // garantiza que el nombre del dataset llegue igual al otro lado.
        if (!safe.contains(shPrintfOctalEscaped(accented))) {
            return fail("asciiSafeShellCommand must embed the exact UTF-8 bytes");
        }
        if (asciiSafeShellCommand(QString()) != QString()) {
            return fail("asciiSafeShellCommand of an empty command must stay empty");
        }
    }

    {
        // La subida del daemon se lanza SIN intérprete: con `sh -c` no arrancaba
        // siquiera cuando la aplicación corre en Windows, y el error salía vacío.
        ConnectionProfile wp;
        wp.username = "eladi";
        wp.host = "192.168.122.229";
        wp.port = 22;
        wp.keyPath = "/home/u/.ssh/id_ed25519";
        const QStringList a = scpUploadArgs(wp, "/tmp/agent.exe", "C:/Users/Public/x.upload", false);
        if (a.last() != "eladi@192.168.122.229:C:/Users/Public/x.upload") {
            return fail("scpUploadArgs destination mismatch");
        }
        if (a.at(a.size() - 2) != "/tmp/agent.exe") {
            return fail("scpUploadArgs source must precede the destination");
        }
        // El puerto de scp es -P mayúscula, no -p.
        if (!a.contains("-P") || a.contains("-p")) {
            return fail("scpUploadArgs must use -P for the port");
        }
        // Sin multiplexar no debe colarse ninguna opción de ControlMaster: el OpenSSH
        // de Windows no las admite.
        for (const QString& x : a) {
            if (x.startsWith("Control")) {
                return fail("scpUploadArgs(multiplex=false) leaked a Control* option");
            }
        }
        const QStringList m = scpUploadArgs(wp, "/tmp/agent.exe", "C:/x", true);
        if (!m.contains("ControlMaster=auto")) {
            return fail("scpUploadArgs(multiplex=true) should enable multiplexing");
        }
        // Y ningún argumento puede llevar comillas de shell: van directos al proceso.
        for (const QString& x : a) {
            if (x.contains('\'')) {
                return fail("scpUploadArgs must not shell-quote: there is no shell");
            }
        }
    }

    // ── maskCommandSecrets ────────────────────────────────────────────────────
    // Dos fugas reales han salido de aquí, las dos por el mismo motivo: los patrones
    // estaban duplicados y ninguno cubría la forma nueva. Cada caso de abajo es una
    // orden que la aplicación construye de verdad.
    {
        const QString sudoOld =
            "printf '%s\\n' 'Mi-Clave' | sudo -k -S -p '' sh -c 'zfs list'";
        const QString sudoOct =
            "printf '%b\\n' '\\0115\\0151' | sudo -k -S -p '' sh -c 'zfs list'";
        const QString passphrase =
            "printf '%s\\n%s\\n' 'Fr4se-Secreta' 'Fr4se-Secreta' | zfs create "
            "-o encryption=on -o keyformat=passphrase pool/nuevo";
        const QString passphrasePs =
            "$pp='Fr4se-Secreta'; $payload=$pp + \"`n\" + $pp; zfs create pool/nuevo";

        struct Case { QString name; QString cmd; QString leak; };
        const QVector<Case> cases = {
            {"sudo literal", sudoOld, "Mi-Clave"},
            {"sudo octal", sudoOct, "\\0115\\0151"},
            {"frase de cifrado", passphrase, "Fr4se-Secreta"},
            {"frase de cifrado (PowerShell)", passphrasePs, "Fr4se-Secreta"},
        };
        for (const Case& c : cases) {
            const QString masked = maskCommandSecrets(c.cmd);
            if (masked.contains(c.leak)) {
                return fail(qPrintable(QStringLiteral(
                    "maskCommandSecrets deja escapar el secreto en: %1").arg(c.name)));
            }
            if (!masked.contains("[secret]")) {
                return fail(qPrintable(QStringLiteral(
                    "maskCommandSecrets no marcó nada en: %1").arg(c.name)));
            }
        }
        // Y no debe tapar de más: una orden sin secretos tiene que salir intacta.
        const QString innocuous = "zfs list -H -o name,mountpoint -r testpool";
        if (maskCommandSecrets(innocuous) != innocuous) {
            return fail("maskCommandSecrets alteró una orden sin secretos");
        }
    }

    // La frase de cifrado viaja como último argumento en base64 de tres verbos. Base64
    // no es cifrado: apareció en el registro y se descodifica de cabeza.
    {
        const QStringList argv{QStringLiteral("--mutate-zfs-load-key"),
                               QStringLiteral("dGVzdHBvb2wvZmQx"),
                               QStringLiteral("c2VjcmV0YXNv")};
        const QString masked = maskedAgentArgvForLog(argv);
        if (masked.contains(QStringLiteral("c2VjcmV0YXNv"))) {
            return fail("maskedAgentArgvForLog deja escapar la frase de load-key");
        }
        if (!masked.contains(QStringLiteral("dGVzdHBvb2wvZmQx"))) {
            return fail("maskedAgentArgvForLog no debe tapar el dataset");
        }
        const QStringList createArgv{QStringLiteral("--mutate-zfs-create"),
                                     QStringLiteral("W10="),
                                     QStringLiteral("c2VjcmV0YXNv")};
        if (maskedAgentArgvForLog(createArgv).contains(QStringLiteral("c2VjcmV0YXNv"))) {
            return fail("maskedAgentArgvForLog deja escapar la frase de create");
        }
        // Un verbo sin secreto sale intacto.
        const QStringList plain{QStringLiteral("--dump-zfs-list-all"), QStringLiteral("testpool")};
        if (maskedAgentArgvForLog(plain) != QStringLiteral("--dump-zfs-list-all testpool")) {
            return fail("maskedAgentArgvForLog alteró una invocación sin secretos");
        }
        // Y la misma orden ya renderizada a cadena, con el entrecomillado anidado que
        // produce meterla dentro de otro shSingleQuote.
        const QString rendered =
            "zfsmgr-agent '\"'\"'--mutate-zfs-load-key'\"'\"' "
            "'\"'\"'dGVzdHBvb2wvZmQx'\"'\"' '\"'\"'c2VjcmV0YXNv'\"'\"'";
        if (maskCommandSecrets(rendered).contains(QStringLiteral("c2VjcmV0YXNv"))) {
            return fail("maskCommandSecrets deja escapar la frase en la orden renderizada");
        }
    }

    // Redacción para guardar la lista de acciones en disco. Esto es lo único que separa
    // la contraseña de sudo de quedar escrita en claro en config.json, así que se
    // comprueba que sale, que vuelve, y que el guardián detecta una fuga.
    {
        ConnectionProfile p;
        p.password = QStringLiteral("rpq231");
        const QVector<StorableSecret> secrets{StorableSecret{QStringLiteral("fc16"), p.password}};

        // La forma real: la que produce withSudoCommand.
        const QString real = withSudoCommand(
            [&]() {
                ConnectionProfile prof;
                prof.password = p.password;
                prof.useSudo = true;
                prof.osType = QStringLiteral("linux");
                return prof;
            }(),
            QStringLiteral("zfs list -H"));
        if (!real.contains(shPrintfOctalEscaped(p.password))) {
            return fail("el caso de prueba no reproduce la forma octal de withSudoCommand");
        }

        bool ok = false;
        const QString stored = redactSecretsForStorage(real, secrets, &ok);
        if (!ok) {
            return fail("redactSecretsForStorage rechazó una orden que sí puede redactar");
        }
        if (stored.contains(p.password) || stored.contains(shPrintfOctalEscaped(p.password))) {
            return fail("redactSecretsForStorage dejó la contraseña en el texto a guardar");
        }
        if (!stored.contains(storedSecretMarkerPrefix())) {
            return fail("redactSecretsForStorage no dejó marcador");
        }
        if (restoreSecretsFromStorage(stored, secrets) != real) {
            return fail("el ciclo redactar/restaurar no devuelve la orden original");
        }

        // Un perfil que ya no existe deja el marcador sin resolver: quien restaura tiene
        // que poder verlo y descartar la entrada en vez de ejecutar el marcador literal.
        const QString orphan = restoreSecretsFromStorage(stored, {});
        if (!orphan.contains(storedSecretMarkerPrefix())) {
            return fail("un marcador sin perfil debería sobrevivir para poder detectarlo");
        }

        // Y el guardián: si la contraseña aparece de una forma que no se sustituyó, la
        // función debe negarse en vez de devolver algo casi limpio.
        const QVector<StorableSecret> wrongKey{StorableSecret{QString(), p.password}};
        bool ok2 = true;
        const QString refused = redactSecretsForStorage(real, wrongKey, &ok2);
        if (ok2 || !refused.isEmpty()) {
            return fail("redactSecretsForStorage debería negarse si la contraseña sobrevive");
        }
    }

    // Discos enteros de Windows: el de arranque y el del sistema, protegidos; un disco
    // de datos, ofrecible aunque tenga particiones. Es lo que permite dedicarlo a ZFS,
    // que en Windows es obligatorio: OpenZFS no abre una partición suelta.
    {
        if (!windowsPartitionTypeIsProtected(
                QStringLiteral("diskstyle=GPT|bus=SATA|model=X|isboot=True|issystem=True|parts=4|type=WHOLEDISK"))) {
            return fail("el disco de arranque debe salir protegido");
        }
        if (windowsPartitionTypeIsProtected(
                QStringLiteral("diskstyle=GPT|bus=SATA|model=X|isboot=False|issystem=False|parts=2|type=WHOLEDISK"))) {
            return fail("un disco de datos con particiones NO debe salir protegido");
        }
        if (!windowsPartitionTypeIsProtected(QStringLiteral("NTFS|gpt=...|type=system|mbr=-"))) {
            return fail("una partición de sistema debe seguir protegida");
        }
    }

    std::cout << "[OK] helpers tests passed\n";
    return 0;
}
