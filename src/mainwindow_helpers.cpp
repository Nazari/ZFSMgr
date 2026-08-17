#include "mainwindow_helpers.h"

#include "daemonpayload.h"

#include <map>
#include <vector>

#include "base/connectionprofile.h"
#include "base/helpers.h"
#include "base/strutil.h"

namespace B = zfsmgr::base::helpers;
namespace BP = zfsmgr::base;


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

// --- Adaptadores de las funciones que toman ConnectionProfile.
//
// `toBase` copia los 16 campos. Es un espejo completo a propósito: uno parcial invita a
// que alguien lea más adelante un campo silenciosamente vacío.
namespace {
// El conversor vive en connectionstore.h: era la SEGUNDA copia idéntica del espejo de 16
// campos, y una copia que se queda atrás hace que un campo llegue vacío sin que falle nada.
BP::ConnectionProfile toBase(const ConnectionProfile& p) { return toBaseProfile(p); }
std::vector<std::string> bl(const QStringList& v) {
    std::vector<std::string> o;
    o.reserve(static_cast<std::size_t>(v.size()));
    for (const QString& x : v) {
        o.push_back(b(x));
    }
    return o;
}
QStringList ql(const std::vector<std::string>& v) {
    QStringList o;
    o.reserve(static_cast<int>(v.size()));
    for (const std::string& x : v) {
        o << q(x);
    }
    return o;
}
}  // namespace

QString shPrintfOctalEscaped(const QString& s) { return q(B::shPrintfOctalEscaped(b(s))); }

QString maskedAgentArgvForLog(const QStringList& argv) { return q(B::maskedAgentArgvForLog(bl(argv))); }
QString normalizeDriveLetterValue(const QString& raw) { return q(B::normalizeDriveLetterValue(b(raw))); }
QString sshHostKeyProblemHint(const QString& e) { return q(B::sshHostKeyProblemHint(b(e))); }
bool isCliOnlyAgentCommand(const QString& verb) { return B::isCliOnlyAgentCommand(b(verb)); }
QString windowsGptTypeName(const QString& guid) { return q(B::windowsGptTypeName(b(guid))); }
QString formatWindowsFsTypeDetail(const QString& raw) { return q(B::formatWindowsFsTypeDetail(b(raw))); }
bool windowsPartitionTypeIsProtected(const QString& raw) { return B::windowsPartitionTypeIsProtected(b(raw)); }
QString asciiSafeShellCommand(const QString& cmd) { return q(B::asciiSafeShellCommand(b(cmd))); }
bool looksLikeSudoAuthFailure(const QString& text) { return B::looksLikeSudoAuthFailure(b(text)); }

QString maskCommandSecrets(const QString& input) { return q(B::maskCommandSecrets(b(input))); }
QString parseOpenZfsVersionText(const QString& text) { return q(B::parseOpenZfsVersionText(b(text))); }
QVector<ImportablePoolInfo> parseZpoolImportOutput(const QString& text) {
    QVector<ImportablePoolInfo> out;
    for (const B::ImportablePoolInfo& r : B::parseZpoolImportOutput(b(text))) {
        out.push_back(ImportablePoolInfo{q(r.pool), q(r.guid), q(r.state), q(r.reason)});
    }
    return out;
}


QStringList posixShellSplitArgs(const QString& s) { return ql(B::posixShellSplitArgs(b(s))); }

TransferButtonState computeTransferButtonState(const TransferButtonInputs& in) {
    B::TransferButtonInputs bi;
    bi.srcDatasetSelected = in.srcDatasetSelected;
    bi.srcSnapshotSelected = in.srcSnapshotSelected;
    bi.dstDatasetSelected = in.dstDatasetSelected;
    bi.dstSnapshotSelected = in.dstSnapshotSelected;
    bi.srcSelectionKey = b(in.srcSelectionKey);
    bi.dstSelectionKey = b(in.dstSelectionKey);
    bi.srcSelectionConsistent = in.srcSelectionConsistent;
    bi.dstSelectionConsistent = in.dstSelectionConsistent;
    bi.srcDatasetMounted = in.srcDatasetMounted;
    bi.dstDatasetMounted = in.dstDatasetMounted;
    const B::TransferButtonState bs = B::computeTransferButtonState(bi);
    TransferButtonState out;
    out.copyEnabled = bs.copyEnabled;
    out.levelEnabled = bs.levelEnabled;
    out.syncEnabled = bs.syncEnabled;
    return out;
}

namespace {
std::map<std::string, std::string> bm(const QMap<QString, QString>& m) {
    std::map<std::string, std::string> o;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        o.emplace(b(it.key()), b(it.value()));
    }
    return o;
}
std::map<std::string, std::vector<std::string>> bml(const QMap<QString, QStringList>& m) {
    std::map<std::string, std::vector<std::string>> o;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        o.emplace(b(it.key()), bl(it.value()));
    }
    return o;
}
std::vector<B::StorableSecret> bs(const QVector<StorableSecret>& v) {
    std::vector<B::StorableSecret> o;
    o.reserve(static_cast<std::size_t>(v.size()));
    for (const StorableSecret& s : v) {
        o.push_back(B::StorableSecret{b(s.key), b(s.secret)});
    }
    return o;
}
}  // namespace

QMap<QString, QStringList> duplicateMountpoints(const QMap<QString, QString>& datasetMountpoints) {
    QMap<QString, QStringList> out;
    for (const auto& [mp, datasets] : B::duplicateMountpoints(bm(datasetMountpoints))) {
        out.insert(q(mp), ql(datasets));
    }
    return out;
}

QVector<MountpointConflict> externalMountpointConflicts(const QMap<QString, QString>& target,
                                                        const QMap<QString, QStringList>& mounted) {
    QVector<MountpointConflict> out;
    for (const B::MountpointConflict& c : B::externalMountpointConflicts(bm(target), bml(mounted))) {
        out.push_back(MountpointConflict{q(c.mountpoint), q(c.mountedDataset), q(c.requestedDataset)});
    }
    return out;
}

QString redactSecretsForStorage(const QString& command, const QVector<StorableSecret>& secrets, bool* okOut) {
    return q(B::redactSecretsForStorage(b(command), bs(secrets), okOut));
}

QString restoreSecretsFromStorage(const QString& stored, const QVector<StorableSecret>& secrets) {
    return q(B::restoreSecretsFromStorage(b(stored), bs(secrets)));
}

QString sshControlPath() { return q(B::sshControlPath()); }
QString sshUserHost(const ConnectionProfile& p) { return q(B::sshUserHost(toBase(p))); }
QString sshUserHostPort(const ConnectionProfile& p) { return q(B::sshUserHostPort(toBase(p))); }
QString sshAddressFamilyOption(const ConnectionProfile& p) { return q(B::sshAddressFamilyOption(toBase(p))); }
QString sshBaseCommand(const ConnectionProfile& p) { return q(B::sshBaseCommand(toBase(p))); }
QString buildSshTargetPrefix(const ConnectionProfile& p) { return q(B::buildSshTargetPrefix(toBase(p))); }
QString buildSimpleSshInvocation(const ConnectionProfile& p, const QString& remoteCmd) {
    return q(B::buildSimpleSshInvocation(toBase(p), b(remoteCmd)));
}
QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd) {
    return q(B::buildSshPreviewCommandText(toBase(p), b(remoteCmd)));
}
QStringList scpUploadArgs(const ConnectionProfile& p, const QString& localPath,
                          const QString& remotePath, bool multiplex) {
    return ql(B::scpUploadArgs(toBase(p), b(localPath), b(remotePath), multiplex));
}
QString withSudoCommand(const ConnectionProfile& p, const QString& cmd) {
    return q(B::withSudoCommand(toBase(p), b(cmd)));
}
QString withSudoStreamInputCommand(const ConnectionProfile& p, const QString& cmd) {
    return q(B::withSudoStreamInputCommand(toBase(p), b(cmd)));
}
QString agentCommand(const ConnectionProfile& p, const QString& agentArgs) {
    return q(B::agentCommand(toBase(p), b(agentArgs)));
}
QString agentShellCommand(const ConnectionProfile& p, const QStringList& agentArgs) {
    return q(B::agentShellCommand(toBase(p), bl(agentArgs)));
}
QString agentShellCommandStreamInput(const ConnectionProfile& p, const QStringList& agentArgs) {
    return q(B::agentShellCommandStreamInput(toBase(p), bl(agentArgs)));
}



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











// Traduce el fallo de verificación de host de ssh a algo accionable.
//
// Con StrictHostKeyChecking=accept-new, un host cuya clave CAMBIA se rechaza, y ssh
// solo deja un "Host key verification failed" y sale con 255. Sin explicación, eso
// parece una avería de red. Y es justo el caso que importa: o el host se reinstaló,
// o alguien se está haciendo pasar por él.













// Invocación del agente adaptada a la plataforma.
//
// Existe para no repetir la ruta en cada sitio. En Windows el binario está en otro
// lugar y NO lleva sudo ni PATH de Unix; olvidarlo en un solo punto deja esa función
// muda o con rc=127, que es lo que pasó con el log del daemon, el latido y la
// comprobación de salud, cada uno descubierto por separado.

// Parser POSIX mínimo: entiende '...' y "..." y el patrón '"'"' de shSingleQuote.
// QProcess::splitCommand solo maneja "..." (no '...') y produce resultados incorrectos
// cuando los args vienen de shSingleQuote embebido en otro shSingleQuote.







namespace {
}  // namespace







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


} // namespace mwhelpers
