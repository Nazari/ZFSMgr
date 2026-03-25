#include "helperinstallcatalog.h"

#include <QMap>
#include <QSet>

namespace helperinstall {
namespace {

QStringList normalizeUnique(const QStringList& values) {
    QStringList out;
    QSet<QString> seen;
    for (const QString& raw : values) {
        const QString v = raw.trimmed();
        if (v.isEmpty()) {
            continue;
        }
        const QString key = v.toLower();
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        out.push_back(v);
    }
    return out;
}

QMap<QString, QString> packageMapFor(const QString& packageManagerId) {
    const QString pm = packageManagerId.trimmed().toLower();
    if (pm == QStringLiteral("apt") || pm == QStringLiteral("pacman") || pm == QStringLiteral("zypper")) {
        return {
            {QStringLiteral("sshpass"), QStringLiteral("sshpass")},
            {QStringLiteral("rsync"), QStringLiteral("rsync")},
            {QStringLiteral("pv"), QStringLiteral("pv")},
            {QStringLiteral("mbuffer"), QStringLiteral("mbuffer")},
            {QStringLiteral("tar"), QStringLiteral("tar")},
            {QStringLiteral("gzip"), QStringLiteral("gzip")},
            {QStringLiteral("zstd"), QStringLiteral("zstd")},
            {QStringLiteral("grep"), QStringLiteral("grep")},
            {QStringLiteral("sed"), QStringLiteral("sed")},
            {QStringLiteral("gawk"), QStringLiteral("gawk")},
        };
    }
    if (pm == QStringLiteral("brew")) {
        return {
            {QStringLiteral("sshpass"), QStringLiteral("hudochenkov/sshpass/sshpass")},
            {QStringLiteral("rsync"), QStringLiteral("rsync")},
            {QStringLiteral("pv"), QStringLiteral("pv")},
            {QStringLiteral("mbuffer"), QStringLiteral("mbuffer")},
            {QStringLiteral("zstd"), QStringLiteral("zstd")},
            {QStringLiteral("grep"), QStringLiteral("grep")},
            {QStringLiteral("sed"), QStringLiteral("gnu-sed")},
            {QStringLiteral("gawk"), QStringLiteral("gawk")},
        };
    }
    if (pm == QStringLiteral("pkg")) {
        return {
            {QStringLiteral("sshpass"), QStringLiteral("sshpass")},
            {QStringLiteral("rsync"), QStringLiteral("rsync")},
            {QStringLiteral("pv"), QStringLiteral("pv")},
            {QStringLiteral("mbuffer"), QStringLiteral("mbuffer")},
            {QStringLiteral("gzip"), QStringLiteral("gzip")},
            {QStringLiteral("zstd"), QStringLiteral("zstd")},
            {QStringLiteral("grep"), QStringLiteral("grep")},
            {QStringLiteral("gawk"), QStringLiteral("gawk")},
        };
    }
    if (pm == QStringLiteral("msys2")) {
        return {
            {QStringLiteral("tar"), QStringLiteral("tar")},
            {QStringLiteral("gzip"), QStringLiteral("gzip")},
            {QStringLiteral("zstd"), QStringLiteral("zstd")},
            {QStringLiteral("rsync"), QStringLiteral("rsync")},
            {QStringLiteral("grep"), QStringLiteral("grep")},
            {QStringLiteral("sed"), QStringLiteral("sed")},
            {QStringLiteral("gawk"), QStringLiteral("gawk")},
            {QStringLiteral("pv"), QStringLiteral("pv")},
            {QStringLiteral("mbuffer"), QStringLiteral("mbuffer")},
        };
    }
    return {};
}

QString buildCommandPreview(const QString& packageManagerId, const QStringList& packages, bool useSudo) {
    const QStringList pkgs = normalizeUnique(packages);
    if (pkgs.isEmpty()) {
        return QString();
    }
    const QString joined = pkgs.join(QLatin1Char(' '));
    const QString sudoPrefix = useSudo ? QStringLiteral("sudo ") : QString();
    const QString pm = packageManagerId.trimmed().toLower();
    if (pm == QStringLiteral("apt")) {
        return QStringLiteral("%1apt-get update && %1apt-get install -y %2").arg(sudoPrefix, joined);
    }
    if (pm == QStringLiteral("pacman")) {
        return QStringLiteral("%1pacman -Sy --noconfirm %2").arg(sudoPrefix, joined);
    }
    if (pm == QStringLiteral("zypper")) {
        return QStringLiteral("%1zypper --non-interactive install %2").arg(sudoPrefix, joined);
    }
    if (pm == QStringLiteral("brew")) {
        return QStringLiteral("brew install %1").arg(joined);
    }
    if (pm == QStringLiteral("pkg")) {
        return QStringLiteral("%1pkg install -y %2").arg(sudoPrefix, joined);
    }
    if (pm == QStringLiteral("msys2")) {
        return QStringLiteral("pacman --noconfirm -Sy --needed %1").arg(joined);
    }
    return QString();
}

} // namespace

QStringList trackedInstallableCommands() {
    return {
        QStringLiteral("sshpass"),
        QStringLiteral("rsync"),
        QStringLiteral("pv"),
        QStringLiteral("mbuffer"),
        QStringLiteral("tar"),
        QStringLiteral("gzip"),
        QStringLiteral("zstd"),
        QStringLiteral("grep"),
        QStringLiteral("sed"),
        QStringLiteral("gawk"),
    };
}

PlatformInfo detectPlatform(const ConnectionProfile& profile, const QString& osLine) {
    PlatformInfo info;
    const QString os = osLine.trimmed().toLower();
    const QString connType = profile.connType.trimmed().toLower();
    if (connType == QStringLiteral("psrp") || profile.osType.trimmed().toLower().contains(QStringLiteral("windows"))
        || os.contains(QStringLiteral("windows"))) {
        info.platformId = QStringLiteral("windows");
        info.platformLabel = QStringLiteral("Windows");
        info.packageManagerId = QStringLiteral("msys2");
        info.packageManagerLabel = QStringLiteral("MSYS2");
        info.supportedByDesign = true;
        info.windowsUsesMsys2 = true;
        return info;
    }
    if (os.contains(QStringLiteral("freebsd"))) {
        info.platformId = QStringLiteral("freebsd");
        info.platformLabel = QStringLiteral("FreeBSD");
        info.packageManagerId = QStringLiteral("pkg");
        info.packageManagerLabel = QStringLiteral("pkg");
        info.supportedByDesign = true;
        return info;
    }
    if (os.contains(QStringLiteral("darwin")) || os.contains(QStringLiteral("mac os")) || os.contains(QStringLiteral("macos"))) {
        info.platformId = QStringLiteral("macos");
        info.platformLabel = QStringLiteral("macOS");
        info.packageManagerId = QStringLiteral("brew");
        info.packageManagerLabel = QStringLiteral("Homebrew");
        info.supportedByDesign = true;
        return info;
    }
    if (os.contains(QStringLiteral("ubuntu"))) {
        info.platformId = QStringLiteral("ubuntu");
        info.platformLabel = QStringLiteral("Ubuntu");
        info.packageManagerId = QStringLiteral("apt");
        info.packageManagerLabel = QStringLiteral("apt-get");
        info.supportedByDesign = true;
        return info;
    }
    if (os.contains(QStringLiteral("debian"))) {
        info.platformId = QStringLiteral("debian");
        info.platformLabel = QStringLiteral("Debian");
        info.packageManagerId = QStringLiteral("apt");
        info.packageManagerLabel = QStringLiteral("apt-get");
        info.supportedByDesign = true;
        return info;
    }
    if (os.contains(QStringLiteral("arch"))) {
        info.platformId = QStringLiteral("arch");
        info.platformLabel = QStringLiteral("Arch Linux");
        info.packageManagerId = QStringLiteral("pacman");
        info.packageManagerLabel = QStringLiteral("pacman");
        info.supportedByDesign = true;
        return info;
    }
    if (os.contains(QStringLiteral("opensuse")) || os.contains(QStringLiteral("suse"))) {
        info.platformId = QStringLiteral("suse");
        info.platformLabel = QStringLiteral("openSUSE / SUSE");
        info.packageManagerId = QStringLiteral("zypper");
        info.packageManagerLabel = QStringLiteral("zypper");
        info.supportedByDesign = true;
        return info;
    }
    if (os.contains(QStringLiteral("linux"))) {
        info.platformId = QStringLiteral("linux");
        info.platformLabel = QStringLiteral("Linux");
        info.reason = QStringLiteral("Distribución Linux no reconocida");
        return info;
    }
    info.platformId = QStringLiteral("unknown");
    info.platformLabel = osLine.trimmed().isEmpty() ? QStringLiteral("Desconocido") : osLine.trimmed();
    info.reason = QStringLiteral("Plataforma no soportada para instalación asistida");
    return info;
}

InstallPlan buildInstallPlan(const PlatformInfo& platform, const QStringList& missingCommands, bool useSudo) {
    InstallPlan plan;
    plan.platformId = platform.platformId;
    plan.platformLabel = platform.platformLabel;
    plan.packageManagerId = platform.packageManagerId;
    plan.packageManagerLabel = platform.packageManagerLabel;
    plan.requestedCommands = normalizeUnique(missingCommands);

    if (!platform.supportedByDesign) {
        if (!platform.reason.trimmed().isEmpty()) {
            plan.warnings.push_back(platform.reason.trimmed());
        }
        return plan;
    }

    const QMap<QString, QString> map = packageMapFor(platform.packageManagerId);
    QSet<QString> pkgSeen;
    for (const QString& raw : plan.requestedCommands) {
        const QString cmd = raw.trimmed();
        if (cmd.isEmpty()) {
            continue;
        }
        const QString pkg = map.value(cmd.toLower());
        if (pkg.isEmpty()) {
            plan.unsupportedCommands.push_back(cmd);
            continue;
        }
        plan.supportedCommands.push_back(cmd);
        const QString key = pkg.trimmed().toLower();
        if (!key.isEmpty() && !pkgSeen.contains(key)) {
            pkgSeen.insert(key);
            plan.packages.push_back(pkg);
        }
    }

    if (platform.packageManagerId == QStringLiteral("brew") && plan.supportedCommands.contains(QStringLiteral("sshpass"))) {
        plan.warnings.push_back(QStringLiteral("sshpass en macOS puede requerir un tap adicional de Homebrew"));
    }
    if (platform.packageManagerId == QStringLiteral("pkg")
        && (plan.requestedCommands.contains(QStringLiteral("tar")) || plan.requestedCommands.contains(QStringLiteral("sed")))) {
        plan.warnings.push_back(QStringLiteral("En FreeBSD se evita forzar gtar/gsed salvo que sean realmente necesarios"));
    }

    plan.commandPreview = buildCommandPreview(platform.packageManagerId, plan.packages, useSudo);
    plan.supported = !plan.supportedCommands.isEmpty() && !plan.commandPreview.trimmed().isEmpty();
    return plan;
}

} // namespace helperinstall
