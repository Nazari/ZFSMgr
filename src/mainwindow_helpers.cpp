#include "mainwindow_helpers.h"

#include "daemonpayload.h"

#include "base/helpers.h"
#include "base/strutil.h"

namespace B = zfsmgr::base::helpers;

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

namespace mwhelpers {

// --- Adaptadores hacia la capa base.
//
// La lógica de estas funciones vive en `src/base/helpers.cpp`, que no depende de Qt.
// Aquí solo se convierte en la frontera, para no tener que tocar los puntos de llamada
// del cliente en el mismo commit. Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace {
inline QString q(const std::string& s) { return QString::fromStdString(s); }
inline std::string b(const QString& s) { return s.toStdString(); }
inline B::StreamCodec bc(StreamCodec c) {
    switch (c) {
        case StreamCodec::Zstd: return B::StreamCodec::Zstd;
        case StreamCodec::Gzip: return B::StreamCodec::Gzip;
        default:                return B::StreamCodec::None;
    }
}
inline StreamCodec qc(B::StreamCodec c) {
    switch (c) {
        case B::StreamCodec::Zstd: return StreamCodec::Zstd;
        case B::StreamCodec::Gzip: return StreamCodec::Gzip;
        default:                   return StreamCodec::None;
    }
}
}  // namespace

QString shSingleQuote(const QString& s) { return q(zfsmgr::base::shSingleQuote(b(s))); }
QString oneLine(const QString& v, int maxLen) { return q(B::oneLine(b(v), maxLen)); }
bool isMountedValueTrue(const QString& value) { return B::isMountedValueTrue(b(value)); }
QString parentDatasetName(const QString& dataset) { return q(B::parentDatasetName(b(dataset))); }
bool isWindowsOsType(const QString& osType) { return B::isWindowsOsType(b(osType)); }
bool parentMountCheckRequired(const QString& mp, const QString& canmount) {
    return B::parentMountCheckRequired(b(mp), b(canmount));
}
bool parentAllowsChildMount(const QString& mp, const QString& canmount, const QString& mounted) {
    return B::parentAllowsChildMount(b(mp), b(canmount), b(mounted));
}
QString buildHasMountedChildrenCommand(bool isWindows, const QString& ds) {
    return q(B::buildHasMountedChildrenCommand(isWindows, b(ds)));
}
QString buildRecursiveUmountCommand(bool isWindows, const QString& ds) {
    return q(B::buildRecursiveUmountCommand(isWindows, b(ds)));
}
QString buildSingleUmountCommand(bool isWindows, const QString& ds) {
    return q(B::buildSingleUmountCommand(isWindows, b(ds)));
}
QString buildSingleMountCommand(const QString& ds) { return q(B::buildSingleMountCommand(b(ds))); }
QString buildMountChildrenCommand(bool isWindows, const QString& ds) {
    return q(B::buildMountChildrenCommand(isWindows, b(ds)));
}
QString buildWindowsMountPrecheckCommand(const QString& ds, const QString& mp) {
    return q(B::buildWindowsMountPrecheckCommand(b(ds), b(mp)));
}
QString streamProgressPipeFilter() { return q(B::streamProgressPipeFilter()); }
QString buildPipedTransferCommand(const QString& send, const QString& recv) {
    return q(B::buildPipedTransferCommand(b(send), b(recv)));
}
QString streamCodecName(StreamCodec codec) { return q(B::streamCodecName(bc(codec))); }
StreamCodec chooseStreamCodec(bool zstd, bool gzip) { return qc(B::chooseStreamCodec(zstd, gzip)); }
QString buildTarSourceCommand(bool isWindows, const QString& mp, StreamCodec codec) {
    return q(B::buildTarSourceCommand(isWindows, b(mp), bc(codec)));
}
QString buildTarDestinationCommand(bool isWindows, const QString& mp, StreamCodec codec) {
    return q(B::buildTarDestinationCommand(isWindows, b(mp), bc(codec)));
}
QString withUnixSearchPathCommand(const QString& cmd) { return q(B::withUnixSearchPathCommand(b(cmd))); }
QString rpcTunnelBusyReason() { return q(B::rpcTunnelBusyReason()); }
QString storedSecretMarkerPrefix() { return q(B::storedSecretMarkerPrefix()); }
QString stripToJson(const QString& output) { return q(B::stripToJson(b(output))); }


QString findLocalExecutable(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    const QString fromPath = QStandardPaths::findExecutable(trimmed);
    if (!fromPath.isEmpty()) {
        return fromPath;
    }

    QStringList fallbackDirs;
#if defined(Q_OS_MACOS)
    fallbackDirs << QStringLiteral("/opt/homebrew/bin")
                 << QStringLiteral("/opt/homebrew/sbin")
                 << QStringLiteral("/usr/local/bin")
                 << QStringLiteral("/usr/local/sbin");
#endif
    fallbackDirs << QStringLiteral("/usr/bin")
                 << QStringLiteral("/usr/sbin")
                 << QStringLiteral("/bin")
                 << QStringLiteral("/sbin");

    for (const QString& dir : fallbackDirs) {
        const QString candidate = QDir(dir).filePath(trimmed);
        const QFileInfo fi(candidate);
        if (fi.exists() && fi.isFile() && fi.isExecutable()) {
            return fi.absoluteFilePath();
        }
    }
    return QString();
}

// Tapa los secretos que van EMBEBIDOS en una orden, independientemente de dónde se
// vaya a mostrar. Estaba duplicado en dos sitios —maskSecrets(), para el registro, y
// maskSecretsForPreview(), para el diálogo de confirmación— con formas distintas y
// ligeramente desalineadas. Esa duplicación ya ha costado dos fugas: la contraseña de
// sudo en la 0.90.8, y la frase de cifrado de `zfs create`, que no tapaba NINGUNO de
// los seis patrones que había porque usa `'%s\n%s\n'` con dos argumentos y la tubería
// va a `zfs`, no a `sudo`.
//
// Un solo sitio, y con test.
QString maskCommandSecrets(const QString& input) {
    QString out = input;
    const auto sub = [&out](const QString& pattern, const QString& replacement) {
        out.replace(QRegularExpression(pattern), replacement);
    };

    // Contraseña de sudo, forma antigua (literal) y actual (escapes octales para %b).
    sub(QStringLiteral("(printf\\s+'%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'(?=\\s*\\|\\s*sudo)"),
        QStringLiteral("\\1'[secret]'"));
    sub(QStringLiteral("(printf\\s+'%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'(?=\\s*;\\s*cat)"),
        QStringLiteral("\\1'[secret]'"));
    sub(QStringLiteral("(printf\\s+'%b\\\\n'\\s+)'(?:\\\\0[0-7]{1,3})*'"),
        QStringLiteral("\\1'[secret]'"));

    // Frase de cifrado al crear un dataset: `zfs create -o keyformat=passphrase` la
    // pide DOS veces por la entrada estándar, de ahí el formato con dos %s.
    sub(QStringLiteral("(printf\\s+'%s\\\\n%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'\\s+'(?:[^'\\\\]|\\\\.)*'"),
        QStringLiteral("\\1'[secret]' '[secret]'"));
    // Su equivalente en PowerShell, que la mete en una variable.
    sub(QStringLiteral("(\\$pp\\s*=\\s*)'(?:[^']|'')*'"), QStringLiteral("\\1'[secret]'"));

    // Verbos del agente cuyo ÚLTIMO argumento es un secreto en base64. Los separadores
    // se aceptan como clase de caracteres porque la orden puede venir entrecomillada una
    // o dos veces —`'"'"'` cuando va dentro de otro shSingleQuote—, y escribir cada
    // variante a mano es justo lo que ya falló antes con la frase de `zfs create`.
    sub(QStringLiteral("(--mutate-zfs-(?:load-key|change-key|create)[\'\" \\\\]+"
                       "[A-Za-z0-9+/=]+[\'\" \\\\]+)[A-Za-z0-9+/=]+"),
        QStringLiteral("\\1[secret]"));

    // Cualquier `password=` / `password:` suelto.
    sub(QStringLiteral("(?i)(password\\s*[:=]\\s*)\\S+"), QStringLiteral("\\1[secret]"));
    return out;
}

QString maskedAgentArgvForLog(const QStringList& argv) {
    QStringList masked = argv;
    for (int i = 0; i < masked.size(); ++i) {
        const QString verb = masked.at(i).trimmed();
        if (verb != QStringLiteral("--mutate-zfs-load-key")
            && verb != QStringLiteral("--mutate-zfs-change-key")
            && verb != QStringLiteral("--mutate-zfs-create")) {
            continue;
        }
        // El secreto es el ÚLTIMO argumento del verbo: para load-key/change-key va tras
        // el dataset, y para create tras el argv de zfs. Se tapa todo lo que siga a ese
        // primer argumento, que nunca es más de uno.
        for (int j = i + 2; j < masked.size(); ++j) {
            masked[j] = QStringLiteral("[secret]");
        }
        break;
    }
    return masked.join(QLatin1Char(' '));
}





QString normalizeDriveLetterValue(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty() || s == QStringLiteral("-") || s.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    s.replace(QStringLiteral(":\\"), QString());
    s.replace(':', QString());
    s.replace('\\', QString());
    s.replace('/', QString());
    s = s.trimmed().toUpper();
    if (s.isEmpty()) {
        return QString();
    }
    const QChar d = s[0];
    if (!d.isLetter()) {
        return QString();
    }
    return QString(d);
}


QString windowsGptTypeName(const QString& guid) {
    QString g = guid.trimmed();
    if (g.startsWith('{') && g.endsWith('}') && g.size() > 2) {
        g = g.mid(1, g.size() - 2);
    }
    g = g.toLower();
    static const QMap<QString, QString> kMap = {
        {QStringLiteral("00000000-0000-0000-0000-000000000000"), QStringLiteral("Unused entry")},
        {QStringLiteral("024dee41-33e7-11d3-9d69-0008c781f39f"), QStringLiteral("MBR partition scheme")},
        {QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), QStringLiteral("EFI System Partition")},
        {QStringLiteral("21686148-6449-6e6f-744e-656564454649"), QStringLiteral("BIOS Boot Partition")},
        {QStringLiteral("b334117e-118d-11de-9b0f-001cc0952d53"), QStringLiteral("gdisk unknown")},
        {QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae"), QStringLiteral("Windows/Reserved")},
        {QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"), QStringLiteral("Windows/Basic Data / Linux/Data")},
        {QStringLiteral("5808c8aa-7e8f-42e0-85d2-e1e90434cfb3"), QStringLiteral("Windows/LDM metadata")},
        {QStringLiteral("af9b60a0-1431-4f62-bc68-3311714a69ad"), QStringLiteral("Windows/LDM data")},
        {QStringLiteral("75894c1e-3aeb-11d3-b7c1-7b03a0000000"), QStringLiteral("HP-UX/Data")},
        {QStringLiteral("e2a1e728-32e3-11d6-a682-7b03a0000000"), QStringLiteral("HP-UX/Service")},
        {QStringLiteral("a19d880f-05fc-4d3b-a006-743f0f84911e"), QStringLiteral("Linux/RAID")},
        {QStringLiteral("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"), QStringLiteral("Linux/Swap")},
        {QStringLiteral("e6d6d379-f507-44c2-a23c-238f2a3df928"), QStringLiteral("Linux/LVM")},
        {QStringLiteral("8da63339-0007-60c0-c436-083ac8230908"), QStringLiteral("Linux/Reserved")},
        {QStringLiteral("83bd6b9d-7f41-11dc-be0b-001560b84f0f"), QStringLiteral("FreeBSD/Boot")},
        {QStringLiteral("516e7cb4-6ecf-11d6-8ff8-00022d09712b"), QStringLiteral("FreeBSD/Data")},
        {QStringLiteral("516e7cb5-6ecf-11d6-8ff8-00022d09712b"), QStringLiteral("FreeBSD/Swap")},
        {QStringLiteral("516e7cb6-6ecf-11d6-8ff8-00022d09712b"), QStringLiteral("FreeBSD/UFS")},
        {QStringLiteral("516e7cb8-6ecf-11d6-8ff8-00022d09712b"), QStringLiteral("FreeBSD/Vinum")},
        {QStringLiteral("516e7cba-6ecf-11d6-8ff8-00022d09712b"), QStringLiteral("FreeBSD/ZFS")},
        {QStringLiteral("48465300-0000-11aa-aa11-00306543ecac"), QStringLiteral("Mac OS X/HFS+")},
        {QStringLiteral("55465300-0000-11aa-aa11-00306543ecac"), QStringLiteral("Mac OS X/Apple UFS")},
        {QStringLiteral("6a898cc3-1dd2-11b2-99a6-080020736631"), QStringLiteral("Mac OS X/ZFS / Solaris/usr")},
        {QStringLiteral("52414944-0000-11aa-aa11-00306543ecac"), QStringLiteral("Mac OS X/RAID")},
        {QStringLiteral("52414944-5f4f-11aa-aa11-00306543ecac"), QStringLiteral("Mac OS X/Offline RAID")},
        {QStringLiteral("426f6f74-0000-11aa-aa11-00306543ecac"), QStringLiteral("Mac OS X/Boot")},
        {QStringLiteral("4c616265-6c00-11aa-aa11-00306543ecac"), QStringLiteral("Mac OS X/Label")},
        {QStringLiteral("5265636f-7665-11aa-aa11-00306543ecac"), QStringLiteral("Mac OS X/Apple TV Recovery")},
        {QStringLiteral("6a82cb45-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Boot")},
        {QStringLiteral("6a85cf4d-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Root")},
        {QStringLiteral("6a87c46f-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Swap")},
        {QStringLiteral("6a8b642b-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Backup")},
        {QStringLiteral("6a8ef2e9-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/var")},
        {QStringLiteral("6a90ba39-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/home")},
        {QStringLiteral("6a9283a5-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/EFI_ALTSCTR")},
        {QStringLiteral("6a945a3b-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Reserved")},
        {QStringLiteral("6a9630d1-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Reserved")},
        {QStringLiteral("6a980767-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Reserved")},
        {QStringLiteral("6a96237f-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Reserved")},
        {QStringLiteral("6a8d2ac7-1dd2-11b2-99a6-080020736631"), QStringLiteral("Solaris/Reserved")},
        {QStringLiteral("49f48d32-b10e-11dc-b99b-0019d1879648"), QStringLiteral("NetBSD/Swap")},
        {QStringLiteral("49f48d5a-b10e-11dc-b99b-0019d1879648"), QStringLiteral("NetBSD/FFS")},
        {QStringLiteral("49f48d82-b10e-11dc-b99b-0019d1879648"), QStringLiteral("NetBSD/LFS")},
        {QStringLiteral("49f48daa-b10e-11dc-b99b-0019d1879648"), QStringLiteral("NetBSD/RAID")},
        {QStringLiteral("2db519c4-b10f-11dc-b99b-0019d1879648"), QStringLiteral("NetBSD/concatenated")},
        {QStringLiteral("2db519ec-b10f-11dc-b99b-0019d1879648"), QStringLiteral("NetBSD/encrypted")},
    };
    return kMap.value(g);
}

QString formatWindowsFsTypeDetail(const QString& rawFsType) {
    const QString raw = rawFsType.trimmed();
    if (raw.isEmpty() || raw == QStringLiteral("-")) {
        return rawFsType;
    }
    QStringList parts = raw.split('|');
    bool changed = false;
    for (QString& part : parts) {
        const QString p = part.trimmed();
        if (!p.startsWith(QStringLiteral("gpt="), Qt::CaseInsensitive)) {
            continue;
        }
        const QString guidRaw = p.mid(4).trimmed();
        if (guidRaw.isEmpty() || guidRaw == QStringLiteral("-")) {
            continue;
        }
        const QString name = windowsGptTypeName(guidRaw);
        if (name.isEmpty()) {
            continue;
        }
        part = QStringLiteral("gpt=%1").arg(name);
        changed = true;
    }
    return changed ? parts.join('|') : rawFsType;
}

bool windowsPartitionTypeIsProtected(const QString& rawFsType) {
    if (rawFsType.trimmed().isEmpty()) {
        return false;
    }
    const QStringList parts = rawFsType.split('|');
    for (const QString& partRaw : parts) {
        const QString part = partRaw.trimmed();
        if (!part.startsWith(QStringLiteral("type="), Qt::CaseInsensitive)) {
            continue;
        }
        const QString v = part.mid(5).trimmed().toLower();
        if (v == QStringLiteral("system") || v == QStringLiteral("recovery") || v == QStringLiteral("reserved")) {
            return true;
        }
    }
    // Discos enteros: el de arranque y el del sistema van protegidos.
    //
    // Hasta ahora el disco completo solo se ofrecía cuando no tenía nada aprovechable
    // dentro, así que no hacía falta protegerlo. Al pasar a ofrecerlo siempre —que es lo
    // que necesita OpenZFS on Windows, incapaz de usar una partición suelta— hay que
    // distinguir el disco que se puede entregar a ZFS del que arranca el sistema.
    for (const QString& partRaw : parts) {
        const QString part = partRaw.trimmed().toLower();
        if (part == QStringLiteral("isboot=true") || part == QStringLiteral("issystem=true")) {
            return true;
        }
    }
    return false;
}

QString parseOpenZfsVersionText(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    const QString lower = text.toLower();
    const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("\\bzfs(?:-kmod)?[-\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
        QRegularExpression(QStringLiteral("\\bopenzfs(?:[-\\s]+version)?[:\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
        QRegularExpression(QStringLiteral("\\b(?:zfs|zpool)[^\\r\\n]*?\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
    };
    for (const QRegularExpression& rx : patterns) {
        const QRegularExpressionMatch m = rx.match(lower);
        if (m.hasMatch()) {
            const QString ver = m.captured(1);
            const int major = ver.section('.', 0, 0).toInt();
            if (major <= 10) {
                return ver;
            }
        }
    }
    return QString();
}

QVector<ImportablePoolInfo> parseZpoolImportOutput(const QString& text) {
    QVector<ImportablePoolInfo> rows;
    const QRegularExpression poolNameRx(QStringLiteral("^[A-Za-z0-9_.:-]+$"));
    QString currentPool;
    QString currentGuid;
    QString currentState;
    QString currentReason;
    bool collectingStatus = false;

    auto flushCurrent = [&]() {
        if (currentPool.isEmpty()) {
            return;
        }
        if (!poolNameRx.match(currentPool).hasMatch()) {
            currentPool.clear();
            currentGuid.clear();
            currentState.clear();
            currentReason.clear();
            collectingStatus = false;
            return;
        }
        if (currentState.isEmpty() && currentReason.isEmpty()) {
            currentPool.clear();
            currentGuid.clear();
            collectingStatus = false;
            return;
        }
        rows.push_back(ImportablePoolInfo{
            currentPool,
            currentGuid,
            currentState.isEmpty() ? QStringLiteral("UNKNOWN") : currentState,
            currentReason,
        });
        currentPool.clear();
        currentGuid.clear();
        currentState.clear();
        currentReason.clear();
        collectingStatus = false;
    };

    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("pool: "))) {
            flushCurrent();
            currentPool = line.mid(QStringLiteral("pool: ").size()).trimmed();
            continue;
        }
        if (currentPool.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("state: "))) {
            currentState = line.mid(QStringLiteral("state: ").size()).trimmed();
            collectingStatus = false;
            continue;
        }
        if (line.startsWith(QStringLiteral("id: "))) {
            currentGuid = line.mid(QStringLiteral("id: ").size()).trimmed();
            continue;
        }
        if (line.startsWith(QStringLiteral("status: "))) {
            currentReason = line.mid(QStringLiteral("status: ").size()).trimmed();
            collectingStatus = true;
            continue;
        }
        if (collectingStatus) {
            if (line.startsWith(QStringLiteral("action:")) || line.startsWith(QStringLiteral("see:")) || line.startsWith(QStringLiteral("config:"))) {
                collectingStatus = false;
            } else if (!line.isEmpty()) {
                currentReason = (currentReason + QStringLiteral(" ") + line).trimmed();
                continue;
            }
        }
        if (line.startsWith(QStringLiteral("cannot import"))) {
            if (!currentReason.isEmpty()) {
                currentReason += QStringLiteral(" ");
            }
            currentReason += line;
        }
    }
    flushCurrent();
    return rows;
}

TransferButtonState computeTransferButtonState(const TransferButtonInputs& in) {
    TransferButtonState out;
    const bool sameSelection = !in.srcSelectionKey.isEmpty() && (in.srcSelectionKey == in.dstSelectionKey);
    out.copyEnabled = in.srcDatasetSelected && in.srcSnapshotSelected && in.dstDatasetSelected && !in.dstSnapshotSelected;
    out.levelEnabled = in.srcDatasetSelected && in.dstDatasetSelected && !in.dstSnapshotSelected && !sameSelection;
    out.syncEnabled = in.srcDatasetSelected
        && !in.srcSnapshotSelected
        && in.dstDatasetSelected
        && !in.dstSnapshotSelected
        && !sameSelection
        && in.srcSelectionConsistent
        && in.dstSelectionConsistent
        && in.srcDatasetMounted
        && in.dstDatasetMounted;
    return out;
}



QMap<QString, QStringList> duplicateMountpoints(const QMap<QString, QString>& datasetMountpoints) {
    QMap<QString, QStringList> grouped;
    for (auto it = datasetMountpoints.constBegin(); it != datasetMountpoints.constEnd(); ++it) {
        const QString dataset = it.key();
        const QString mp = it.value().trimmed();
        const QString mpl = mp.toLower();
        if (dataset.isEmpty() || mp.isEmpty() || mpl == QStringLiteral("none") || mpl == QStringLiteral("-")) {
            continue;
        }
        grouped[mp].push_back(dataset);
    }
    QMap<QString, QStringList> out;
    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        if (it.value().size() > 1) {
            out.insert(it.key(), it.value());
        }
    }
    return out;
}

QVector<MountpointConflict> externalMountpointConflicts(const QMap<QString, QString>& targetDatasetMountpoints,
                                                        const QMap<QString, QStringList>& mountedByMountpoint) {
    QVector<MountpointConflict> out;
    for (auto it = targetDatasetMountpoints.constBegin(); it != targetDatasetMountpoints.constEnd(); ++it) {
        const QString requestedDataset = it.key();
        const QString mountpoint = it.value().trimmed();
        if (requestedDataset.isEmpty() || mountpoint.isEmpty()) {
            continue;
        }
        const QStringList mountedDatasets = mountedByMountpoint.value(mountpoint);
        for (const QString& mountedDs : mountedDatasets) {
            if (mountedDs.isEmpty() || mountedDs == requestedDataset) {
                continue;
            }
            out.push_back(MountpointConflict{mountpoint, mountedDs, requestedDataset});
        }
    }
    return out;
}

QVector<QPair<QString, QString>> parseZfsMountOutput(const QString& text) {
    QVector<QPair<QString, QString>> out;
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    static const QRegularExpression rowRx(QStringLiteral("^\\s*(\\S+)\\s+(.+?)\\s*$"));
    for (const QString& raw : lines) {
        const QString ln = raw.trimmed();
        if (ln.isEmpty()) {
            continue;
        }
        const QRegularExpressionMatch m = rowRx.match(ln);
        if (!m.hasMatch()) {
            continue;
        }
        const QString ds = m.captured(1).trimmed();
        const QString mp = m.captured(2).trimmed();
        if (ds.isEmpty() || mp.isEmpty()) {
            continue;
        }
        out.push_back(qMakePair(ds, mp));
    }
    return out;
}

QVector<QPair<QString, QString>> parseZfsMountJsonOutput(const QString& text) {
    QVector<QPair<QString, QString>> out;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) {
        return out;
    }
    const QJsonObject root = doc.object();
    auto extractMountpoint = [](const QJsonValue& v) -> QString {
        if (v.isString()) {
            return v.toString().trimmed();
        }
        if (!v.isObject()) {
            return QString();
        }
        const QJsonObject o = v.toObject();
        if (o.contains(QStringLiteral("value"))) {
            return o.value(QStringLiteral("value")).toString().trimmed();
        }
        if (o.contains(QStringLiteral("mountpoint"))) {
            const QJsonValue inner = o.value(QStringLiteral("mountpoint"));
            if (inner.isString()) {
                return inner.toString().trimmed();
            }
            if (inner.isObject()) {
                return inner.toObject().value(QStringLiteral("value")).toString().trimmed();
            }
        }
        return QString();
    };
    auto appendFromMap = [&out, &extractMountpoint](const QJsonObject& mapObj) {
        for (auto it = mapObj.constBegin(); it != mapObj.constEnd(); ++it) {
            const QString ds = it.key().trimmed();
            if (ds.isEmpty()) {
                continue;
            }
            const QString mp = extractMountpoint(it.value());
            if (mp.isEmpty()) {
                continue;
            }
            out.push_back(qMakePair(ds, mp));
        }
    };
    if (root.contains(QStringLiteral("datasets")) && root.value(QStringLiteral("datasets")).isObject()) {
        appendFromMap(root.value(QStringLiteral("datasets")).toObject());
    } else if (root.contains(QStringLiteral("mounts")) && root.value(QStringLiteral("mounts")).isObject()) {
        appendFromMap(root.value(QStringLiteral("mounts")).toObject());
    } else {
        appendFromMap(root);
    }
    return out;
}







QString sshControlPath() {
#ifdef Q_OS_MAC
    return QStringLiteral("/tmp/zfsmgr-%C");
#else
    return QDir::tempPath() + QStringLiteral("/zfsmgr-ssh-%C");
#endif
}

QString sshUserHost(const ConnectionProfile& p) {
    return QStringLiteral("%1@%2").arg(p.username, p.host);
}

QString sshUserHostPort(const ConnectionProfile& p) {
    const QString port = (p.port > 0) ? QString::number(p.port) : QStringLiteral("22");
    return QStringLiteral("%1:%2").arg(sshUserHost(p), port);
}

QString sshAddressFamilyOption(const ConnectionProfile& p) {
    const QString family = p.sshAddressFamily.trimmed().toLower();
    if (family == QStringLiteral("ipv4")) {
        return QStringLiteral("-4");
    }
    if (family == QStringLiteral("ipv6")) {
        return QStringLiteral("-6");
    }
    return QString();
}

// Traduce el fallo de verificación de host de ssh a algo accionable.
//
// Con StrictHostKeyChecking=accept-new, un host cuya clave CAMBIA se rechaza, y ssh
// solo deja un "Host key verification failed" y sale con 255. Sin explicación, eso
// parece una avería de red. Y es justo el caso que importa: o el host se reinstaló,
// o alguien se está haciendo pasar por él.
QString sshHostKeyProblemHint(const QString& sshStderr) {
    if (sshStderr.contains(QStringLiteral("REMOTE HOST IDENTIFICATION HAS CHANGED"))
        || sshStderr.contains(QStringLiteral("Host key verification failed"))) {
        return QStringLiteral(
            "La clave del host SSH no coincide con la registrada en ~/.ssh/known_hosts. "
            "Si reinstaló o reemplazó esa máquina, elimine su línea de ese fichero "
            "(ssh-keygen -R <host>) y vuelva a conectar. Si no ha cambiado nada, "
            "no continúe: alguien podría estar suplantando al host.");
    }
    if (sshStderr.contains(QStringLiteral("Bad configuration option: stricthostkeychecking"))) {
        return QStringLiteral(
            "Su cliente SSH es demasiado antiguo para 'accept-new' (necesita OpenSSH 7.6 o superior).");
    }
    return QString();
}

QString sshBaseCommand(const ConnectionProfile& p) {
    // accept-new y SIN UserKnownHostsFile: se usa el ~/.ssh/known_hosts del usuario.
    // Antes iba StrictHostKeyChecking=no con UserKnownHostsFile=/dev/null, que no solo
    // no verifica al host: descarta la memoria, así que cada conexión aceptaba
    // cualquier clave para siempre. Como el material TLS del daemon se trae POR SSH,
    // eso permitía que un intermediario entregara su propio certificado, que la
    // aplicación fijaría tan tranquila.
    // Sin multiplexado cuando la aplicación corre en Windows: su OpenSSH responde
    // `getsockname failed: Not a socket`, y ControlPersist deja un maestro de fondo que
    // no suelta las tuberías heredadas. Mismo motivo que en runSsh.
#ifdef Q_OS_WIN
    QString cmd = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR"
                                 " -o StrictHostKeyChecking=accept-new");
#else
    QString cmd = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=accept-new"
                                 " -o ControlMaster=auto -o ControlPersist=yes -o ControlPath=%1")
                      .arg(shSingleQuote(sshControlPath()));
#endif
    const QString familyOpt = sshAddressFamilyOption(p);
    if (!familyOpt.isEmpty()) {
        cmd += QStringLiteral(" ") + familyOpt;
    }
    if (p.port > 0) {
        cmd += QStringLiteral(" -p ") + QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        cmd += QStringLiteral(" -i ") + shSingleQuote(p.keyPath);
    }
    return cmd;
}


QStringList scpUploadArgs(const ConnectionProfile& p,
                          const QString& localPath,
                          const QString& remotePath,
                          bool multiplex) {
    QStringList args;
    args << QStringLiteral("-q")
         << QStringLiteral("-o") << QStringLiteral("BatchMode=yes")
         << QStringLiteral("-o") << QStringLiteral("LogLevel=ERROR")
         << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=accept-new");
    if (multiplex) {
        args << QStringLiteral("-o") << QStringLiteral("ControlMaster=auto")
             << QStringLiteral("-o") << QStringLiteral("ControlPersist=yes")
             << QStringLiteral("-o") << QStringLiteral("ControlPath=%1").arg(sshControlPath());
    }
    const QString familyOpt = sshAddressFamilyOption(p).trimmed();
    if (!familyOpt.isEmpty()) {
        args << familyOpt;
    }
    if (p.port > 0) {
        // scp usa -P mayúscula para el puerto, no -p como ssh.
        args << QStringLiteral("-P") << QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        args << QStringLiteral("-i") << p.keyPath;
    }
    args << localPath
         << QStringLiteral("%1:%2").arg(sshUserHost(p), remotePath);
    return args;
}

QString buildSshTargetPrefix(const ConnectionProfile& p) {
    return sshBaseCommand(p)
        + QStringLiteral(" ")
        + shSingleQuote(sshUserHost(p));
}

QString buildSimpleSshInvocation(const ConnectionProfile& p, const QString& remoteCmd) {
    return buildSshTargetPrefix(p)
        + QStringLiteral(" ")
        + shSingleQuote(remoteCmd);
}








// Invocación del agente adaptada a la plataforma.
//
// Existe para no repetir la ruta en cada sitio. En Windows el binario está en otro
// lugar y NO lleva sudo ni PATH de Unix; olvidarlo en un solo punto deja esa función
// muda o con rc=127, que es lo que pasó con el log del daemon, el latido y la
// comprobación de salud, cada uno descubierto por separado.
QString agentCommand(const ConnectionProfile& p, const QString& agentArgs) {
    if (isWindowsOsType(p.osType)) {
        // El "&" no es decorativo: en PowerShell una cadena entrecomillada al principio
        // de una sentencia es una expresión, no un comando, así que sin el operador de
        // llamada la ruta se evalúa como texto y el primer argumento revienta el parseo
        // ("Token 'health' inesperado"). Además, quien invoque esto debe forzar
        // WindowsCommandMode::PowerShellNative: en Auto el envoltorio lo toma por shell
        // Unix y lo ejecuta con el bash de MSYS2, que se come las barras invertidas.
        return QStringLiteral("& \"") + daemonpayload::windowsBinPath() + QStringLiteral("\" ") + agentArgs;
    }
    // Sin withUnixSearchPathCommand aquí: withSudoCommand ya lo aplica, y hacerlo dos
    // veces dejaba el prefijo PATH duplicado en la orden y, de paso, en el diálogo de
    // confirmación, donde no ayuda a decidir nada.
    return withSudoCommand(p, daemonpayload::unixBinPath() + QStringLiteral(" ") + agentArgs);
}

// Parser POSIX mínimo: entiende '...' y "..." y el patrón '"'"' de shSingleQuote.
// QProcess::splitCommand solo maneja "..." (no '...') y produce resultados incorrectos
// cuando los args vienen de shSingleQuote embebido en otro shSingleQuote.
QStringList posixShellSplitArgs(const QString& s) {
    QStringList result;
    QString current;
    bool inSQ = false, inDQ = false, started = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inSQ) {
            if (c == QLatin1Char('\'')) { inSQ = false; }
            else { current += c; }
        } else if (inDQ) {
            if (c == QLatin1Char('"')) { inDQ = false; }
            else if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                const QChar next = s.at(++i);
                if (next == QLatin1Char('"') || next == QLatin1Char('\\')
                    || next == QLatin1Char('$') || next == QLatin1Char('`')) {
                    current += next;
                } else {
                    current += c;
                    current += next;
                }
            } else {
                current += c;
            }
        } else {
            // "started" distingue un token vacío ESCRITO ('') de la ausencia de token.
            // Sin esto, un argumento vacío desaparecía al recuperarlo, y --zfs-send-to-peer
            // pasa vacíos a menudo (snapshot base y flags), con lo que los siguientes
            // argumentos se corrían de posición.
            if (c == QLatin1Char('\'')) { inSQ = true; started = true; }
            else if (c == QLatin1Char('"')) { inDQ = true; started = true; }
            else if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                current += s.at(++i);
            } else if (c.isSpace()) {
                if (!current.isEmpty() || started) { result += current; current.clear(); started = false; }
            } else {
                current += c;
            }
        }
    }
    if (!current.isEmpty() || started) { result += current; }
    return result;
}

QString agentShellCommand(const ConnectionProfile& p, const QStringList& agentArgs) {
    QStringList quoted;
    quoted.reserve(agentArgs.size());
    for (const QString& a : agentArgs) {
        quoted << (isWindowsOsType(p.osType) ? a : shSingleQuote(a));
    }
    return agentCommand(p, quoted.join(QLatin1Char(' ')));
}

QString agentShellCommandStreamInput(const ConnectionProfile& p, const QStringList& agentArgs) {
    if (isWindowsOsType(p.osType)) {
        return agentShellCommand(p, agentArgs);
    }
    QStringList quoted;
    quoted.reserve(agentArgs.size());
    for (const QString& a : agentArgs) {
        quoted << shSingleQuote(a);
    }
    return withSudoStreamInputCommand(
        p, withUnixSearchPathCommand(daemonpayload::unixBinPath() + QStringLiteral(" ")
                                     + quoted.join(QLatin1Char(' '))));
}

bool isCliOnlyAgentCommand(const QString& verb) {
    // Estos cuatro no se sirven por RPC a propósito: transportan flujos por la entrada
    // y la salida estándar, que el canal RPC no lleva.
    static const QSet<QString> kCliOnly = {
        QStringLiteral("--mutate-shell-generic"),
        QStringLiteral("--mutate-advanced-fromdir"),
        QStringLiteral("--mutate-sync-temp-tar-source"),
        QStringLiteral("--mutate-sync-temp-tar-dest"),
    };
    return kCliOnly.contains(verb.trimmed());
}

QString shPrintfOctalEscaped(const QString& s) {
    const QByteArray utf8 = s.toUtf8();
    QString out;
    out.reserve(utf8.size() * 4);
    for (const char rawByte : utf8) {
        const unsigned char b = static_cast<unsigned char>(rawByte);
        out += QStringLiteral("\\0%1").arg(static_cast<uint>(b), 3, 8, QLatin1Char('0'));
    }
    return out;
}



namespace {
QString storedSecretMarker(const QString& key) {
    return storedSecretMarkerPrefix() + key + QStringLiteral("@@");
}
}  // namespace

QString redactSecretsForStorage(const QString& command,
                                const QVector<StorableSecret>& secrets,
                                bool* okOut) {
    if (okOut) {
        *okOut = true;
    }
    QString out = command;
    for (const StorableSecret& s : secrets) {
        if (s.secret.isEmpty() || s.key.trimmed().isEmpty()) {
            continue;
        }
        const QString marker = storedSecretMarker(s.key);
        // La octal primero: es la que produce withSudoCommand y la que contiene a la
        // literal como caso raro. Sustituir al revés dejaría trozos de la octal sueltos.
        out.replace(shPrintfOctalEscaped(s.secret), marker);
        out.replace(s.secret, marker);
    }
    for (const StorableSecret& s : secrets) {
        if (s.secret.isEmpty()) {
            continue;
        }
        if (out.contains(s.secret) || out.contains(shPrintfOctalEscaped(s.secret))) {
            if (okOut) {
                *okOut = false;
            }
            return QString();
        }
    }
    return out;
}

QString restoreSecretsFromStorage(const QString& stored,
                                  const QVector<StorableSecret>& secrets) {
    QString out = stored;
    for (const StorableSecret& s : secrets) {
        if (s.key.trimmed().isEmpty()) {
            continue;
        }
        out.replace(storedSecretMarker(s.key), shPrintfOctalEscaped(s.secret));
    }
    return out;
}

QString asciiSafeShellCommand(const QString& cmd) {
    bool hasNonAscii = false;
    for (const QChar c : cmd) {
        if (c.unicode() > 127) {
            hasNonAscii = true;
            break;
        }
    }
    if (!hasNonAscii) {
        return cmd;
    }
    // `eval "$(printf '%b' '...')"`: los escapes octales son ASCII, printf reconstruye
    // los bytes originales y eval ejecuta la orden tal cual era. La entrada estándar
    // queda libre, que hace falta porque por ahí viajan cargas útiles (el binario del
    // agente, la passphrase de un dataset cifrado).
    return QStringLiteral("eval \"$(printf '%b' '%1')\"").arg(shPrintfOctalEscaped(cmd));
}

QString withSudoCommand(const ConnectionProfile& p, const QString& cmd) {
    if (isWindowsOsType(p.osType)) {
        return cmd;
    }
    const QString preparedCmd = withUnixSearchPathCommand(cmd);
    if (!p.useSudo) {
        return preparedCmd;
    }
    if (!p.password.isEmpty()) {
        // %b + escapes octales: la contraseña viaja en ASCII puro. Ver
        // shPrintfOctalEscaped: en macOS, Qt descompone los caracteres al pasar la
        // orden al intérprete y sudo recibía otros bytes.
        return QStringLiteral("printf '%b\\n' '%1' | sudo -k -S -p '' sh -c %2")
            .arg(shPrintfOctalEscaped(p.password), shSingleQuote(preparedCmd));
    }
    // `sh -c` con la orden entrecomillada, igual que withSudoStreamInputCommand.
    // Concatenar `sudo -n ` delante no valía: withUnixSearchPathCommand antepone
    // `PATH="..."; export PATH; `, y los punto y coma partían la línea en tres —
    //   sudo -n PATH="..."      <- sudo sin mandato: responde con su mensaje de uso
    //   export PATH
    //   /usr/local/libexec/zfsmgr-agent --health   <- SIN sudo: "Permiso denegado",
    //                                                 porque el binario es 0700 root
    // Con lo que la aplicación concluía que el agente no estaba instalado en una
    // máquina donde sí lo está. Afectaba a toda conexión con sudo sin contraseña.
    return QStringLiteral("sudo -n sh -c %1").arg(shSingleQuote(preparedCmd));
}

QString withSudoStreamInputCommand(const ConnectionProfile& p, const QString& cmd) {
    if (isWindowsOsType(p.osType)) {
        return cmd;
    }
    const QString preparedCmd = withUnixSearchPathCommand(cmd);
    if (!p.useSudo) {
        return preparedCmd;
    }
    if (!p.password.isEmpty()) {
        return QStringLiteral("{ printf '%b\\n' '%1'; cat; } | sudo -k -S -p '' sh -c %2")
            .arg(shPrintfOctalEscaped(p.password), shSingleQuote(preparedCmd));
    }
    return QStringLiteral("sudo -n sh -c %1").arg(shSingleQuote(preparedCmd));
}

bool looksLikeSudoAuthFailure(const QString& text) {
    const QString t = text.trimmed().toLower();
    if (t.isEmpty()) {
        return false;
    }
    // "no está en sudoers" y "no se permite ejecutar" son de autorización, no de
    // autenticación: reintroducir la contraseña no los arregla, así que no se ofrece.
    if (t.contains(QStringLiteral("not in the sudoers"))
        || t.contains(QStringLiteral("no está en el fichero sudoers"))
        || t.contains(QStringLiteral("is not allowed to execute"))) {
        return false;
    }
    // Cadenas OBSERVADAS, no supuestas. Las de español salen de un sudo 1.9 real en
    // Ubuntu con LANG=es_ES.UTF-8: dice "Lo siento, pruebe otra vez", no "inténtelo
    // de nuevo", que es lo que había aquí escrito de memoria y no casaba con nada.
    // El resultado era que un rechazo de contraseña se clasificaba como "no se pudo
    // comprobar" y el usuario no recibía el aviso.
    static const QStringList kNeedles = {
        QStringLiteral("sorry, try again"),
        QStringLiteral("incorrect password attempt"),
        QStringLiteral("authentication failure"),
        QStringLiteral("a password is required"),
        QStringLiteral("lo siento, pruebe otra vez"),
        QStringLiteral("intento de contraseña incorrecto"),
        QStringLiteral("intentos de contraseña incorrectos"),
        QStringLiteral("se requiere una contraseña"),
        QStringLiteral("se necesita una contraseña"),
        QStringLiteral("fallo de autenticación"),
    };
    for (const QString& needle : kNeedles) {
        if (t.contains(needle)) {
            return true;
        }
    }
    return false;
}

SudoCheck checkLocalSudoPassword(const QString& password, QString* detailOut) {
    if (detailOut) {
        detailOut->clear();
    }
#ifdef Q_OS_WIN
    Q_UNUSED(password);
    return SudoCheck::Ok;
#else
    // Por ruta absoluta: una aplicación lanzada desde el escritorio hereda un PATH
    // mínimo, y en macOS un .app abierto desde el Finder ni siquiera trae el del
    // intérprete de órdenes. Resolverlo solo por PATH hacía fallar la comprobación
    // en el sitio donde más se usa.
    QString sudoBin = findLocalExecutable(QStringLiteral("sudo"));
    if (sudoBin.isEmpty()) {
        for (const QString& candidate : {QStringLiteral("/usr/bin/sudo"),
                                         QStringLiteral("/bin/sudo"),
                                         QStringLiteral("/usr/local/bin/sudo")}) {
            if (QFileInfo(candidate).isExecutable()) {
                sudoBin = candidate;
                break;
            }
        }
    }
    if (sudoBin.isEmpty()) {
        if (detailOut) {
            *detailOut = QStringLiteral("no se encontró el ejecutable sudo");
        }
        return SudoCheck::CouldNotCheck;
    }

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    // Mismos argumentos que withSudoCommand, para que lo que se valida sea lo que
    // después se ejecuta. `-p ''` deja el prompt fuera del error.
    proc.start(sudoBin,
               {QStringLiteral("-k"), QStringLiteral("-S"), QStringLiteral("-p"), QString(),
                QStringLiteral("true")});
    if (!proc.waitForStarted(5000)) {
        if (detailOut) {
            *detailOut = QStringLiteral("no se pudo ejecutar %1").arg(sudoBin);
        }
        return SudoCheck::CouldNotCheck;
    }
    proc.write((password + QStringLiteral("\n")).toUtf8());
    proc.closeWriteChannel();
    // Con la contraseña equivocada sudo reintenta hasta tres veces; al cerrarle la
    // entrada estándar agota los intentos y sale enseguida, así que este plazo solo
    // cubre un sudo que se quede colgado (LDAP, PAM lento).
    if (!proc.waitForFinished(20000)) {
        proc.kill();
        proc.waitForFinished(2000);
        if (detailOut) {
            *detailOut = QStringLiteral("sudo no respondió");
        }
        return SudoCheck::CouldNotCheck;
    }
    if (proc.exitStatus() != QProcess::NormalExit) {
        if (detailOut) {
            *detailOut = QStringLiteral("sudo terminó de forma anómala");
        }
        return SudoCheck::CouldNotCheck;
    }
    if (proc.exitCode() == 0) {
        return SudoCheck::Ok;
    }
    const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    // Un rechazo de sudo que NO es de contraseña —no estar en sudoers, no poder
    // ejecutar el mandato— tampoco se arregla reescribiéndola, así que no se
    // presenta como contraseña incorrecta.
    if (!err.isEmpty() && !looksLikeSudoAuthFailure(err)) {
        if (detailOut) {
            *detailOut = oneLine(err);
        }
        return SudoCheck::CouldNotCheck;
    }
    if (detailOut) {
        *detailOut = err.isEmpty() ? QStringLiteral("sudo devolvió %1").arg(proc.exitCode())
                                   : oneLine(err);
    }
    return SudoCheck::WrongPassword;
#endif
}

QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd) {
    QStringList parts;
    parts << QStringLiteral("ssh");
    const QString familyOpt = sshAddressFamilyOption(p);
    if (!familyOpt.isEmpty()) {
        parts << familyOpt;
    }
    parts << QStringLiteral("-o BatchMode=yes");
    parts << QStringLiteral("-o ConnectTimeout=10");
    parts << QStringLiteral("-o LogLevel=ERROR");
    // Ver la nota en sshBaseCommand: se verifica contra ~/.ssh/known_hosts.
    parts << QStringLiteral("-o StrictHostKeyChecking=accept-new");
    parts << QStringLiteral("-o ControlMaster=auto");
    parts << QStringLiteral("-o ControlPersist=yes");
    parts << QStringLiteral("-o ControlPath=%1").arg(shSingleQuote(sshControlPath()));
    if (p.port > 0) {
        parts << QStringLiteral("-p %1").arg(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        parts << QStringLiteral("-i %1").arg(shSingleQuote(p.keyPath));
    }
    parts << sshUserHost(p);
    parts << shSingleQuote(remoteCmd);
    return parts.join(' ');
}

} // namespace mwhelpers
