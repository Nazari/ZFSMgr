#include "mainwindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QFileDialog>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTabBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QStackedWidget>
#include <QSet>
#include <QSpinBox>
#include <algorithm>
#include <QVBoxLayout>
#include <QWidget>
#include <QComboBox>
#include <QTextStream>
#include <QApplication>
#include <QClipboard>
#include <QMetaObject>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QRegularExpression>
#include <QLocale>
#include <QFontMetrics>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QScrollArea>
#include <QAbstractScrollArea>
#include <QSettings>
#include <QElapsedTimer>
#include <QWheelEvent>
#include <functional>
#include <cmath>

#include <QtConcurrent/QtConcurrent>

namespace {

class PinnedSortItem final : public QTableWidgetItem {
public:
    explicit PinnedSortItem(const QString& text = QString()) : QTableWidgetItem(text) {}
    bool operator<(const QTableWidgetItem& other) const override {
        constexpr int kPinRole = Qt::UserRole + 501;
        const int a = data(kPinRole).toInt();
        const int b = other.data(kPinRole).toInt();
        Qt::SortOrder order = Qt::AscendingOrder;
        if (const QTableWidget* t = tableWidget()) {
            order = static_cast<Qt::SortOrder>(t->property("sort_order").toInt());
        }
        const bool aPinned = (a >= 0);
        const bool bPinned = (b >= 0);
        if (aPinned || bPinned) {
            if (aPinned && bPinned) {
                if (a != b) {
                    // Keep pinned rows in fixed top order even when descending is selected.
                    return (order == Qt::DescendingOrder) ? (a > b) : (a < b);
                }
            } else {
                // QTableWidget reverses comparison for descending sort.
                // Invert mismatch rule in descending so pinned rows still end up on top.
                return (order == Qt::DescendingOrder) ? (!aPinned) : aPinned;
            }
        }
        return QTableWidgetItem::operator<(other);
    }
};

class NoWheelComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent* event) override {
        // Prevent accidental value changes via mouse wheel; keep wheel for table scrolling.
        QWidget* p = parentWidget();
        while (p) {
            if (auto* area = qobject_cast<QAbstractScrollArea*>(p)) {
                QWheelEvent forwarded(
                    event->position(),
                    event->globalPosition(),
                    event->pixelDelta(),
                    event->angleDelta(),
                    event->buttons(),
                    event->modifiers(),
                    event->phase(),
                    event->inverted(),
                    event->source());
                QApplication::sendEvent(area->viewport(), &forwarded);
                event->accept();
                return;
            }
            p = p->parentWidget();
        }
        event->ignore();
    }
};

QString tsNow() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

QString oneLine(const QString& v) {
    QString x = v.simplified();
    return x.left(220);
}

QString normalizeHostToken(QString host) {
    host = host.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']') && host.size() > 2) {
        host = host.mid(1, host.size() - 2);
    }
    while (host.endsWith('.')) {
        host.chop(1);
    }
    return host;
}

bool isLocalHostForUi(const QString& host) {
    const QString h = normalizeHostToken(host);
    if (h.isEmpty()) {
        return false;
    }
    if (h == QStringLiteral("localhost")
        || h == QStringLiteral("127.0.0.1")
        || h == QStringLiteral("::1")) {
        return true;
    }

    static const QSet<QString> aliases = []() {
        QSet<QString> s;
        s.insert(QStringLiteral("localhost"));
        s.insert(QStringLiteral("127.0.0.1"));
        s.insert(QStringLiteral("::1"));
        const QString local = normalizeHostToken(QSysInfo::machineHostName());
        if (!local.isEmpty()) {
            s.insert(local);
            s.insert(local + QStringLiteral(".local"));
            const int dot = local.indexOf('.');
            if (dot > 0) {
                const QString shortName = local.left(dot);
                s.insert(shortName);
                s.insert(shortName + QStringLiteral(".local"));
            }
        }
        return s;
    }();
    return aliases.contains(h);
}

QString sanitizeWindowsCliXml(const QString& raw) {
    QString s = raw;
    if (s.isEmpty()) {
        return s;
    }
    s.replace(QStringLiteral("#< CLIXML"), QStringLiteral(""));
    const int xmlPos = s.indexOf(QStringLiteral("<Objs Version="), 0, Qt::CaseInsensitive);
    if (xmlPos >= 0) {
        s = s.left(xmlPos);
    }
    return s.trimmed();
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

QString parentDiskDevicePath(const QString& rawPath) {
    const QString path = rawPath.trimmed();
    if (path.isEmpty()) {
        return QString();
    }
    const QList<QRegularExpression> rules = {
        // Linux NVMe: /dev/nvme0n1p1 -> /dev/nvme0n1
        QRegularExpression(QStringLiteral(R"(^(.*\/nvme\d+n\d+)p\d+$)")),
        // Linux mmcblk: /dev/mmcblk0p1 -> /dev/mmcblk0
        QRegularExpression(QStringLiteral(R"(^(.*\/mmcblk\d+)p\d+$)")),
        // macOS disk slices: /dev/disk0s1 -> /dev/disk0
        QRegularExpression(QStringLiteral(R"(^(.*\/disk\d+)s\d+$)")),
        // SATA/virtio/xvd: /dev/sda1 -> /dev/sda, /dev/vda2 -> /dev/vda, /dev/xvda3 -> /dev/xvda
        QRegularExpression(QStringLiteral(R"(^(.*\/(?:sd[a-z]+|vd[a-z]+|xvd[a-z]+))\d+$)")),
    };
    for (const auto& rx : rules) {
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            return m.captured(1);
        }
    }
    return QString();
}

QString wrapDeviceDisplayText(const QString& raw, int softWidth = 42) {
    const QString s = raw;
    if (s.size() <= softWidth) {
        return s;
    }
    QString out;
    out.reserve(s.size() + (s.size() / softWidth) + 4);
    int col = 0;
    for (int i = 0; i < s.size(); ++i) {
        const QChar ch = s[i];
        out += ch;
        ++col;
        const bool breakChar = (ch == QChar('/') || ch == QChar('\\') || ch == QChar(' '));
        if ((breakChar && col >= (softWidth / 2)) || col >= softWidth) {
            if (i + 1 < s.size()) {
                out += QChar('\n');
            }
            col = 0;
        }
    }
    return out;
}

bool isRootDevicePath(const QString& rawPath) {
    const QString p = rawPath.trimmed();
    if (p.isEmpty()) {
        return true;
    }
    static const QList<QRegularExpression> rootPatterns = {
        // Linux whole disks
        QRegularExpression(QStringLiteral(R"(^/dev/(sd[a-z]+|vd[a-z]+|xvd[a-z]+)$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/nvme\d+n\d+$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/mmcblk\d+$)")),
        // macOS whole disks
        QRegularExpression(QStringLiteral(R"(^/dev/disk\d+$)")),
        // Windows physical root disks
        QRegularExpression(QStringLiteral(R"(^(\\\\\.\\PHYSICALDRIVE\d+|\\\\\?\\PhysicalDrive\d+)$)"),
                           QRegularExpression::CaseInsensitiveOption),
    };
    for (const auto& rx : rootPatterns) {
        if (rx.match(p).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool looksLikePowerShellScript(const QString& cmd) {
    const QString c = cmd.toLower();
    const QString t = c.trimmed();
    if (t.startsWith('[') || t.startsWith('$') || c.contains(QStringLiteral("::"))) {
        return true;
    }
    if (t.startsWith(QStringLiteral("zfs "))
        || t == QStringLiteral("zfs")
        || t.startsWith(QStringLiteral("zpool "))
        || t == QStringLiteral("zpool")
        || t.startsWith(QStringLiteral("where.exe "))) {
        return true;
    }
    return c.contains(QStringLiteral("out-null"))
        || c.contains(QStringLiteral("test-path"))
        || c.contains(QStringLiteral("resolve-path"))
        || c.contains(QStringLiteral("join-path"))
        || c.contains(QStringLiteral("new-item"))
        || c.contains(QStringLiteral("remove-item"))
        || c.contains(QStringLiteral("sort-object"))
        || c.contains(QStringLiteral("select-object"))
        || c.contains(QStringLiteral("get-childitem"))
        || c.contains(QStringLiteral("write-output"))
        || c.contains(QStringLiteral("invoke-expression"))
        || c.contains(QStringLiteral("foreach("))
        || c.contains(QStringLiteral("$lastexitcode"))
        || c.contains(QStringLiteral("[string]::"))
        || c.contains(QStringLiteral("$env:path"))
        || c.contains(QStringLiteral("powershell "));
}

QStringList zfsmgrUnixCommandSet() {
    return {
        QStringLiteral("uname"),
        QStringLiteral("sh"),
        QStringLiteral("sudo"),
        QStringLiteral("awk"),
        QStringLiteral("grep"),
        QStringLiteral("sort"),
        QStringLiteral("find"),
        QStringLiteral("mktemp"),
        QStringLiteral("printf"),
        QStringLiteral("cat"),
        QStringLiteral("tar"),
        QStringLiteral("gzip"),
        QStringLiteral("zstd"),
        QStringLiteral("pv"),
        QStringLiteral("rsync"),
        QStringLiteral("ssh"),
        QStringLiteral("zfs"),
        QStringLiteral("zpool"),
    };
}

QStringList zfsmgrPowershellCommandSet() {
    return {
        QStringLiteral("zfs"),
        QStringLiteral("zpool"),
        QStringLiteral("tar"),
        QStringLiteral("robocopy"),
        QStringLiteral("Get-ChildItem"),
        QStringLiteral("Test-Path"),
        QStringLiteral("Resolve-Path"),
        QStringLiteral("New-Item"),
        QStringLiteral("Remove-Item"),
        QStringLiteral("Sort-Object"),
        QStringLiteral("Select-Object"),
        QStringLiteral("Out-String"),
    };
}

bool isUserProperty(const QString& prop) {
    return prop.contains(':');
}

bool isDatasetPropertyEditable(const QString& propName, const QString& datasetType, const QString& source, const QString& readonly) {
    const QString prop = propName.trimmed().toLower();
    const QString dsType = datasetType.trimmed().toLower();
    const QString src = source.trimmed();
    const QString ro = readonly.trimmed().toLower();
    if (prop.isEmpty()) {
        return false;
    }
    if (ro == QStringLiteral("true") || ro == QStringLiteral("on") || ro == QStringLiteral("yes") || ro == QStringLiteral("1")) {
        return false;
    }
    if (src == QStringLiteral("-")) {
        return false;
    }
    if (isUserProperty(prop)) {
        return true;
    }

    static const QSet<QString> common = {
        QStringLiteral("atime"), QStringLiteral("relatime"), QStringLiteral("readonly"), QStringLiteral("compression"),
        QStringLiteral("checksum"), QStringLiteral("sync"), QStringLiteral("logbias"), QStringLiteral("primarycache"),
        QStringLiteral("secondarycache"), QStringLiteral("dedup"), QStringLiteral("copies"), QStringLiteral("acltype"),
        QStringLiteral("aclinherit"), QStringLiteral("xattr"), QStringLiteral("normalization"),
        QStringLiteral("casesensitivity"), QStringLiteral("utf8only"), QStringLiteral("keylocation"), QStringLiteral("comment")
    };
    static const QSet<QString> fs = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("mountpoint"), QStringLiteral("canmount"), QStringLiteral("recordsize"), QStringLiteral("quota"),
            QStringLiteral("reservation"), QStringLiteral("refquota"), QStringLiteral("refreservation"),
            QStringLiteral("snapdir"), QStringLiteral("exec"), QStringLiteral("setuid"), QStringLiteral("devices"),
            QStringLiteral("driveletter")
        });
        return s;
    }();
    static const QSet<QString> vol = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("volsize"), QStringLiteral("volblocksize"), QStringLiteral("reservation"),
            QStringLiteral("refreservation"), QStringLiteral("snapdev"), QStringLiteral("volmode")
        });
        return s;
    }();

    if (dsType == QStringLiteral("filesystem")) {
        return fs.contains(prop);
    }
    if (dsType == QStringLiteral("volume")) {
        return vol.contains(prop);
    }
    if (dsType == QStringLiteral("snapshot")) {
        return false;
    }
    return fs.contains(prop) || vol.contains(prop);
}

bool isMountedValueTrue(const QString& value) {
    const QString v = value.trimmed().toLower();
    return v == QStringLiteral("yes") || v == QStringLiteral("on") || v == QStringLiteral("true") || v == QStringLiteral("1");
}

QString parentDatasetName(const QString& dataset) {
    const int slash = dataset.lastIndexOf('/');
    if (slash <= 0) {
        return QString();
    }
    return dataset.left(slash);
}

QString shSingleQuote(const QString& s) {
    QString out = s;
    out.replace('\'', "'\"'\"'");
    return QStringLiteral("'") + out + QStringLiteral("'");
}

QString sshControlPath() {
    return QDir::tempPath() + QStringLiteral("/zfsmgr-ssh-%C");
}

QString sshBaseCommand(const ConnectionProfile& p) {
    QString cmd = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
                                 " -o ControlMaster=auto -o ControlPersist=300 -o ControlPath=%1")
                      .arg(shSingleQuote(sshControlPath()));
    if (p.port > 0) {
        cmd += QStringLiteral(" -p ") + QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        cmd += QStringLiteral(" -i ") + shSingleQuote(p.keyPath);
    }
    return cmd;
}

struct CreateDatasetOptions {
    QString datasetPath;
    QString dsType;
    QString volsize;
    QString blocksize;
    bool parents{true};
    bool sparse{false};
    bool nomount{false};
    bool snapshotRecursive{false};
    QStringList properties;
    QString extraArgs;
};

QString buildZfsCreateCmd(const CreateDatasetOptions& opt) {
    const QString dsType = opt.dsType.trimmed().toLower();
    if (dsType == QStringLiteral("snapshot")) {
        QStringList parts;
        parts << QStringLiteral("zfs") << QStringLiteral("snapshot");
        if (opt.snapshotRecursive) {
            parts << QStringLiteral("-r");
        }
        for (const QString& p : opt.properties) {
            const QString pp = p.trimmed();
            if (!pp.isEmpty()) {
                parts << QStringLiteral("-o") << shSingleQuote(pp);
            }
        }
        if (!opt.extraArgs.trimmed().isEmpty()) {
            parts << opt.extraArgs.trimmed();
        }
        parts << shSingleQuote(opt.datasetPath.trimmed());
        return parts.join(' ');
    }

    QStringList parts;
    parts << QStringLiteral("zfs") << QStringLiteral("create");
    if (opt.parents) {
        parts << QStringLiteral("-p");
    }
    if (opt.sparse) {
        parts << QStringLiteral("-s");
    }
    if (opt.nomount) {
        parts << QStringLiteral("-u");
    }
    if (!opt.blocksize.trimmed().isEmpty()) {
        parts << QStringLiteral("-b") << shSingleQuote(opt.blocksize.trimmed());
    }
    if (dsType == QStringLiteral("volume") && !opt.volsize.trimmed().isEmpty()) {
        parts << QStringLiteral("-V") << shSingleQuote(opt.volsize.trimmed());
    }
    for (const QString& p : opt.properties) {
        const QString pp = p.trimmed();
        if (!pp.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(pp);
        }
    }
    if (!opt.extraArgs.trimmed().isEmpty()) {
        parts << opt.extraArgs.trimmed();
    }
    parts << shSingleQuote(opt.datasetPath.trimmed());
    return parts.join(' ');
}

QString prettifyCommandText(const QString& cmd) {
    QString pretty = cmd.trimmed();
    pretty.replace(QStringLiteral(" && "), QStringLiteral(" &&\n  "));
    pretty.replace(QStringLiteral(" || "), QStringLiteral(" ||\n  "));
    pretty.replace(QStringLiteral(" | "), QStringLiteral(" |\n  "));
    pretty.replace(QStringLiteral("; "), QStringLiteral(";\n"));
    return pretty;
}

QString decodePowerShellEncodedCommand(const QString& encoded) {
    const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
    if (decoded.isEmpty()) {
        return QString();
    }
    QByteArray utf16 = decoded;
    if ((utf16.size() % 2) != 0) {
        utf16.chop(1);
    }
    if (utf16.isEmpty()) {
        return QString();
    }
    const QString script = QString::fromUtf16(reinterpret_cast<const char16_t*>(utf16.constData()),
                                              utf16.size() / 2);
    return script.trimmed();
}

QString pseudoStepForSegment(const QString& segmentRaw) {
    const QString segment = segmentRaw.trimmed();
    const QString s = segment.toLower();
    if (s.contains(QStringLiteral("ssh "))) {
        if (s.contains(QStringLiteral("zfs send"))) {
            return QStringLiteral("Conectar por SSH al origen y enviar stream ZFS (`zfs send`).");
        }
        if (s.contains(QStringLiteral("zfs recv"))) {
            return QStringLiteral("Conectar por SSH al destino y recibir stream ZFS (`zfs recv`).");
        }
        if (s.contains(QStringLiteral("zpool export"))) {
            return QStringLiteral("Conectar por SSH y exportar pool (`zpool export`).");
        }
        if (s.contains(QStringLiteral("zpool import"))) {
            return QStringLiteral("Conectar por SSH e importar pool (`zpool import`).");
        }
        return QStringLiteral("Conectar por SSH y ejecutar comando remoto.");
    }
    if (s.contains(QStringLiteral("pv -trab"))) {
        return QStringLiteral("Mostrar progreso de transferencia con `pv`.");
    }
    if (s.contains(QStringLiteral("zfs send"))) {
        return QStringLiteral("Generar stream ZFS desde snapshot/dataset (`zfs send`).");
    }
    if (s.contains(QStringLiteral("zfs recv"))) {
        return QStringLiteral("Aplicar stream ZFS en destino (`zfs recv`).");
    }
    if (s.contains(QStringLiteral("zfs rollback"))) {
        return QStringLiteral("Revertir dataset al snapshot seleccionado (`zfs rollback`).");
    }
    if (s.contains(QStringLiteral("zfs mount")) || s.contains(QStringLiteral("zfs unmount"))) {
        return QStringLiteral("Montar/desmontar dataset ZFS.");
    }
    if (s.contains(QStringLiteral("zfs set ")) || s.contains(QStringLiteral("zfs get "))) {
        return QStringLiteral("Modificar/consultar propiedades ZFS.");
    }
    if (s.contains(QStringLiteral("powershell "))) {
        return QStringLiteral("Ejecutar script PowerShell.");
    }
    if (s.contains(QStringLiteral("sudo "))) {
        return QStringLiteral("Elevar permisos con sudo y ejecutar comando.");
    }
    return QStringLiteral("Ejecutar subcomando: %1").arg(segment.left(120));
}

QString formatCommandPreview(const QString& input) {
    QString header;
    QString body = input;
    const int nl = input.indexOf('\n');
    if (nl >= 0) {
        header = input.left(nl).trimmed();
        body = input.mid(nl + 1).trimmed();
    } else {
        body = input.trimmed();
    }
    if (body.isEmpty()) {
        return input;
    }

    const QString pretty = prettifyCommandText(body);

    QStringList pseudo;
    const QStringList segments = body.split('|', Qt::SkipEmptyParts);
    pseudo.reserve(segments.size());
    for (const QString& seg : segments) {
        pseudo.push_back(pseudoStepForSegment(seg));
    }

    QStringList decodedBlocks;
    const QRegularExpression psRx(
        QStringLiteral("powershell\\s+[^\\n\\r]*?-EncodedCommand\\s+(['\\\"]?)([A-Za-z0-9+/=]+)\\1"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = psRx.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString enc = m.captured(2).trimmed();
        if (enc.isEmpty()) {
            continue;
        }
        const QString dec = decodePowerShellEncodedCommand(enc);
        if (!dec.isEmpty()) {
            decodedBlocks.push_back(prettifyCommandText(dec));
        }
    }

    QStringList out;
    if (!header.isEmpty()) {
        out.push_back(header);
    }
    out.push_back(QStringLiteral("Resumen legible:"));
    if (pseudo.isEmpty()) {
        out.push_back(QStringLiteral("  1. Ejecutar comando."));
    } else {
        for (int i = 0; i < pseudo.size(); ++i) {
            out.push_back(QStringLiteral("  %1. %2").arg(i + 1).arg(pseudo[i]));
        }
    }

    if (!decodedBlocks.isEmpty()) {
        out.push_back(QStringLiteral(""));
        out.push_back(QStringLiteral("PowerShell decodificado:"));
        for (int i = 0; i < decodedBlocks.size(); ++i) {
            out.push_back(QStringLiteral("  [script %1]").arg(i + 1));
            out.push_back(QStringLiteral("  ") + decodedBlocks[i]);
        }
    }

    out.push_back(QStringLiteral(""));
    out.push_back(QStringLiteral("Comando real (formateado):"));
    out.push_back(QStringLiteral("  ") + pretty);
    return out.join(QStringLiteral("\n"));
}

bool parseSizeToBytes(const QString& input, double& bytesOut) {
    const QString s = input.trimmed();
    if (s.isEmpty()) {
        return false;
    }
    bool ok = false;
    const qint64 rawBytes = s.toLongLong(&ok);
    if (ok) {
        bytesOut = static_cast<double>(rawBytes);
        return true;
    }

    const QRegularExpression rx(QStringLiteral("^\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?)(?:i?B)?\\s*$"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = rx.match(s);
    if (!m.hasMatch()) {
        return false;
    }
    const double value = m.captured(1).toDouble(&ok);
    if (!ok) {
        return false;
    }
    const QString unit = m.captured(2).toUpper();
    int power = 0;
    if (unit == QStringLiteral("K")) {
        power = 1;
    } else if (unit == QStringLiteral("M")) {
        power = 2;
    } else if (unit == QStringLiteral("G")) {
        power = 3;
    } else if (unit == QStringLiteral("T")) {
        power = 4;
    } else if (unit == QStringLiteral("P")) {
        power = 5;
    } else if (unit == QStringLiteral("E")) {
        power = 6;
    }
    bytesOut = value * std::pow(1024.0, power);
    return std::isfinite(bytesOut);
}

QString formatDatasetSize(const QString& rawUsed) {
    double bytes = 0.0;
    if (!parseSizeToBytes(rawUsed, bytes)) {
        return rawUsed.trimmed();
    }
    if (bytes < 0.0) {
        bytes = 0.0;
    }

    static const QStringList units = {
        QStringLiteral("B"),
        QStringLiteral("KB"),
        QStringLiteral("MB"),
        QStringLiteral("GB"),
        QStringLiteral("TB"),
    };
    int unitIndex = 0;
    double value = bytes;
    while (value >= 1024.0 && unitIndex < units.size() - 1) {
        value /= 1024.0;
        ++unitIndex;
    }

    int integerDigits = 1;
    if (value >= 1.0) {
        integerDigits = static_cast<int>(std::floor(std::log10(value))) + 1;
    }
    int decimals = qMax(0, 4 - integerDigits);
    decimals = qMin(decimals, 3);
    if (unitIndex == 0) {
        decimals = 0;
    }

    const QLocale locale = QLocale::system();
    QString number = locale.toString(value, 'f', decimals);
    if (decimals > 0) {
        const QString decimalPoint = locale.decimalPoint();
        while (number.endsWith(QLatin1Char('0'))) {
            number.chop(1);
        }
        if (number.endsWith(decimalPoint)) {
            number.chop(decimalPoint.size());
        }
    }
    return QStringLiteral("%1 %2").arg(number, units[unitIndex]);
}

} // namespace

MainWindow::MainWindow(const QString& masterPassword, const QString& language, QWidget* parent)
    : QMainWindow(parent)
    , m_store(QStringLiteral("ZFSMgr")) {
    m_language = language.trimmed().toLower();
    if (m_language.isEmpty()) {
        m_language = QStringLiteral("es");
    }
    loadUiSettings();
    if (!language.trimmed().isEmpty()) {
        m_language = language.trimmed().toLower();
        saveUiSettings();
    }
    m_store.setMasterPassword(masterPassword);
    initLogPersistence();
    buildUi();
    loadConnections();
    QTimer::singleShot(0, this, [this]() {
        refreshAllConnections();
    });
}

QString MainWindow::tr3(const QString& es, const QString& en, const QString& zh) const {
    if (m_language == QStringLiteral("en")) return en;
    if (m_language == QStringLiteral("zh")) return zh;
    return es;
}

void MainWindow::enableSortableHeader(QTableWidget* table) {
    if (!table || !table->horizontalHeader()) {
        return;
    }
    table->setSortingEnabled(true);
    table->setProperty("sort_col", -1);
    table->setProperty("sort_order", static_cast<int>(Qt::AscendingOrder));
    auto* header = table->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->setSortIndicator(-1, Qt::AscendingOrder);
    connect(header, &QHeaderView::sortIndicatorChanged, this, [table](int section, Qt::SortOrder order) {
        table->setProperty("sort_col", section);
        table->setProperty("sort_order", static_cast<int>(order));
    });
}

void MainWindow::setTablePopulationMode(QTableWidget* table, bool populating) {
    if (!table || !table->horizontalHeader()) {
        return;
    }
    if (populating) {
        beginUiBusy();
        const int colNow = table->horizontalHeader()->sortIndicatorSection();
        if (colNow >= 0) {
            table->setProperty("sort_col", colNow);
            table->setProperty("sort_order", static_cast<int>(table->horizontalHeader()->sortIndicatorOrder()));
        }
        table->setSortingEnabled(false);
        return;
    }
    const int col = table->property("sort_col").toInt();
    const auto order = static_cast<Qt::SortOrder>(table->property("sort_order").toInt());
    table->setSortingEnabled(true);
    if (col >= 0 && col < table->columnCount()) {
        table->sortItems(col, order);
        table->horizontalHeader()->setSortIndicator(col, order);
    } else {
        table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    }
    endUiBusy();
}

void MainWindow::loadUiSettings() {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("app"));
    const QString lang = ini.value(QStringLiteral("language"), m_language).toString().trimmed().toLower();
    if (!lang.isEmpty()) {
        m_language = lang;
    }
    m_actionConfirmEnabled = ini.value(QStringLiteral("confirm_actions"), true).toBool();
    m_logMaxSizeMb = ini.value(QStringLiteral("log_max_mb"), 10).toInt();
    if (m_logMaxSizeMb < 1) {
        m_logMaxSizeMb = 1;
    } else if (m_logMaxSizeMb > 1024) {
        m_logMaxSizeMb = 1024;
    }
    ini.endGroup();
}

void MainWindow::saveUiSettings() const {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("app"));
    ini.setValue(QStringLiteral("language"), m_language);
    ini.setValue(QStringLiteral("confirm_actions"), m_actionConfirmEnabled);
    ini.setValue(QStringLiteral("log_max_mb"), m_logMaxSizeMb);
    ini.endGroup();
    ini.sync();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("ZFSMgr (C++/Qt)"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
    resize(1200, 736);
    setMinimumSize(1120, 736);
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #f3f7fb; color: #14212b; }"
        "QTabWidget::pane { border: 1px solid #b8c7d6; background: #f8fbff; }"
        "QTabWidget::tab-bar { alignment: left; }"
        "QTabBar { background: #f3f7fb; }"
        "QTabBar::scroller { background: #f3f7fb; }"
        "QTabBar QToolButton { background: #f3f7fb; border: 1px solid #b8c7d6; color: #14212b; }"
        "QTabBar::tab { padding: 3px 10px; min-height: 18px; background: #e6edf4; border: 1px solid #b8c7d6; border-bottom: none; }"
        "QTabBar::tab:selected { font-weight: 700; min-height: 24px; background: #cfe5ff; color: #0b2f4f; border: 1px solid #6ea6dd; }"
        "QTabBar::tab:!selected { margin-top: 4px; background: #e6edf4; }"
        "QGroupBox { margin-top: 12px; border: 1px solid #b8c7d6; border-radius: 4px; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 8px; padding: 0 4px 0 4px; }"
        "QPushButton { background: #e8eff5; border: 1px solid #9db0c4; border-radius: 4px; padding: 3px 8px; }"
        "QPushButton:hover { background: #d6e6f2; }"
        "QPushButton:pressed { background: #c4d8e8; }"
        "QPushButton:disabled { background: #edf1f5; color: #8c99a6; border: 1px solid #c8d2dc; }"
        "QMenu { background: #ffffff; border: 1px solid #9db0c4; padding: 3px; }"
        "QMenu::item { padding: 4px 14px; color: #102233; }"
        "QMenu::item:selected { background: #cfe5ff; color: #0b2f4f; }"
        "QMenu::item:disabled { color: #8f9aa5; background: #f4f6f8; }"
        "QListWidget, QTableWidget, QTreeWidget { background: #ffffff; color: #102233; }"
        "QPlainTextEdit, QTextEdit, QComboBox, QLineEdit { background: #ffffff; color: #102233; }"
        "QLineEdit { border: 1px solid #9db0c4; border-radius: 3px; padding: 2px 4px; }"
        "QLineEdit:disabled { background: #edf1f5; color: #8c99a6; border: 1px solid #c8d2dc; }"
        "QComboBox QAbstractItemView { background: #ffffff; color: #102233; }"
        "QScrollBar:vertical { width: 8px; }"
        "QScrollBar:horizontal { height: 8px; }"
        "QTreeWidget::item:selected, QTableWidget::item:selected, QListWidget::item:selected {"
        "  background: #dcecff; color: #0d2438; font-weight: 600; }"
        "QHeaderView::section { background: #eaf1f7; border: 1px solid #c5d3e0; padding: 2px 4px; }"));

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* topArea = new QWidget(central);
    auto* topLayout = new QHBoxLayout(topArea);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(6);

    auto* leftPane = new QWidget(topArea);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
    m_leftTabs = new QTabWidget(leftPane);
    m_leftTabs->setDocumentMode(false);
    m_leftTabs->setTabPosition(QTabWidget::North);
    // Anchura fija basada en el texto real de botones para evitar solapes en macOS.
    const QFontMetrics fm(font());
    const int btnTextWidth = qMax(
        fm.horizontalAdvance(tr3(QStringLiteral("Refrescar todo"), QStringLiteral("Refresh all"), QStringLiteral("全部刷新"))),
        fm.horizontalAdvance(tr3(QStringLiteral("Configuración"), QStringLiteral("Configuration"), QStringLiteral("配置"))));
    const int leftBaseWidth = qMax(340, btnTextWidth + 190);
    const int leftFixedWidth = qMax(220, static_cast<int>(leftBaseWidth * 0.85 * 1.15 * 1.10 * 1.25));
    leftPane->setMinimumWidth(leftFixedWidth);
    leftPane->setMaximumWidth(leftFixedWidth);

    auto* connectionsTab = new QWidget(m_leftTabs);
    auto* connLayout = new QVBoxLayout(connectionsTab);
    connLayout->setContentsMargins(4, 4, 4, 4);
    connLayout->setSpacing(4);
    auto* connButtonsBox = new QGroupBox(
        tr3(QStringLiteral("Conexiones"), QStringLiteral("Connections"), QStringLiteral("连接")), connectionsTab);
    auto* connButtonsBoxLayout = new QVBoxLayout(connButtonsBox);
    connButtonsBoxLayout->setContentsMargins(8, 20, 8, 8);
    auto* connButtons = new QHBoxLayout();
    connButtons->setSpacing(8);
    m_btnNew = new QPushButton(tr3(QStringLiteral("Nueva"), QStringLiteral("New"), QStringLiteral("新建")), connectionsTab);
    m_btnRefreshAll = new QPushButton(tr3(QStringLiteral("Refrescar todo"), QStringLiteral("Refresh all"), QStringLiteral("全部刷新")), connectionsTab);
    m_btnConfig = new QPushButton(tr3(QStringLiteral("Configuración"), QStringLiteral("Configuration"), QStringLiteral("配置")), connectionsTab);
    m_btnNew->setMinimumHeight(34);
    m_btnRefreshAll->setMinimumHeight(34);
    m_btnConfig->setMinimumHeight(34);
    const int connBtnMinW = qMax(m_btnNew->sizeHint().width(),
                                 qMax(m_btnRefreshAll->sizeHint().width(), m_btnConfig->sizeHint().width()));
    m_btnNew->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnRefreshAll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConfig->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConfig->setVisible(true);
    connButtons->addWidget(m_btnNew);
    connButtons->addWidget(m_btnRefreshAll);
    connButtons->addWidget(m_btnConfig);
    connButtonsBoxLayout->addLayout(connButtons);
    connLayout->addWidget(connButtonsBox, 0);

    m_poolMgmtBox = new QGroupBox(
        tr3(QStringLiteral("Gestión de Pools de [vacío]"),
            QStringLiteral("Pool Management of [empty]"),
            QStringLiteral("[空] 的池管理")),
        connectionsTab);
    auto* poolMgmtLayout = new QVBoxLayout(m_poolMgmtBox);
    poolMgmtLayout->setContentsMargins(8, 20, 8, 8);
    auto* poolMgmtButtons = new QHBoxLayout();
    poolMgmtButtons->setSpacing(8);
    m_btnPoolNew =
        new QPushButton(tr3(QStringLiteral("Nuevo"), QStringLiteral("New"), QStringLiteral("新建")), m_poolMgmtBox);
    m_btnPoolNew->setMinimumHeight(34);
    m_btnPoolNew->setMinimumWidth(connBtnMinW);
    m_btnPoolNew->setMaximumWidth(connBtnMinW);
    m_btnPoolNew->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnPoolNew->setEnabled(false);
    poolMgmtButtons->addWidget(m_btnPoolNew);
    poolMgmtButtons->addStretch(1);
    poolMgmtLayout->addLayout(poolMgmtButtons);
    connLayout->addWidget(m_poolMgmtBox, 0);

    auto* connListBox = new QGroupBox(
        tr3(QStringLiteral("Listado"), QStringLiteral("List"), QStringLiteral("列表")), connectionsTab);
    auto* connListBoxLayout = new QVBoxLayout(connListBox);
    connListBoxLayout->setContentsMargins(8, 20, 8, 8);
    m_connectionsList = new QTreeWidget(connListBox);
    m_connectionsList->setHeaderHidden(true);
    m_connectionsList->setColumnCount(1);
    m_connectionsList->setRootIsDecorated(true);
    m_connectionsList->setItemsExpandable(true);
    m_connectionsList->setExpandsOnDoubleClick(true);
    m_connectionsList->setAlternatingRowColors(true);
    m_connectionsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_connectionsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connListBoxLayout->addWidget(m_connectionsList, 1);
    connLayout->addWidget(connListBox, 1);
    connectionsTab->setLayout(connLayout);

    auto* datasetsTab = new QWidget(m_leftTabs);
    auto* dsLeftTabLayout = new QVBoxLayout(datasetsTab);
    dsLeftTabLayout->setContentsMargins(4, 4, 4, 4);
    dsLeftTabLayout->setSpacing(4);
    m_transferBox = new QGroupBox(tr3(QStringLiteral("Origen-->Destino"), QStringLiteral("Source-->Target"), QStringLiteral("源-->目标")), datasetsTab);
    auto* transferLayout = new QVBoxLayout(m_transferBox);
    m_transferOriginLabel = new QLabel(QStringLiteral("Origen: Dataset (seleccione)"), m_transferBox);
    m_transferDestLabel = new QLabel(QStringLiteral("Destino: Dataset (seleccione)"), m_transferBox);
    m_transferOriginLabel->setWordWrap(true);
    m_transferDestLabel->setWordWrap(true);
    m_transferOriginLabel->setMinimumHeight(34);
    m_transferDestLabel->setMinimumHeight(34);
    m_transferOriginLabel->hide();
    m_transferDestLabel->hide();
    m_btnCopy = new QPushButton(tr3(QStringLiteral("Copiar"), QStringLiteral("Copy"), QStringLiteral("复制")), m_transferBox);
    m_btnLevel = new QPushButton(tr3(QStringLiteral("Nivelar"), QStringLiteral("Level"), QStringLiteral("同步快照")), m_transferBox);
    m_btnSync = new QPushButton(tr3(QStringLiteral("Sincronizar"), QStringLiteral("Sync"), QStringLiteral("同步文件")), m_transferBox);
    m_btnCopy->setToolTip(
        tr3(QStringLiteral("Envía un snapshot desde Origen a Destino mediante send/recv.\n"
                           "Requiere: snapshot seleccionado en Origen y dataset seleccionado en Destino."),
            QStringLiteral("Send one snapshot from Source to Target using send/recv.\n"
                           "Requires: snapshot selected in Source and dataset selected in Target."),
            QStringLiteral("通过 send/recv 将源端快照发送到目标端。\n"
                           "条件：源端选择快照，目标端选择数据集。")));
    m_btnLevel->setToolTip(
        tr3(QStringLiteral("Genera/aplica envío diferencial para igualar Origen->Destino.\n"
                           "Requiere: dataset o snapshot seleccionado en Origen y dataset en Destino."),
            QStringLiteral("Build/apply differential transfer to level Source->Target.\n"
                           "Requires: dataset or snapshot selected in Source and dataset in Target."),
            QStringLiteral("生成/应用差异传输以对齐源端到目标端。\n"
                           "条件：源端选择数据集或快照，目标端选择数据集。")));
    m_btnSync->setToolTip(
        tr3(QStringLiteral("Sincroniza contenido de dataset Origen a Destino con rsync.\n"
                           "Requiere: dataset seleccionado (no snapshot) en Origen y Destino."),
            QStringLiteral("Sync dataset contents from Source to Target with rsync.\n"
                           "Requires: dataset selected (not snapshot) in Source and Target."),
            QStringLiteral("使用 rsync 同步源端到目标端的数据集内容。\n"
                           "条件：源端和目标端都选择数据集（非快照）。")));
    m_btnCopy->setEnabled(false);
    m_btnLevel->setEnabled(false);
    m_btnSync->setEnabled(false);
    auto* transferButtonsRow = new QHBoxLayout();
    transferButtonsRow->setSpacing(8);
    transferButtonsRow->addWidget(m_btnCopy);
    transferButtonsRow->addWidget(m_btnLevel);
    transferButtonsRow->addWidget(m_btnSync);
    transferLayout->addLayout(transferButtonsRow);
    dsLeftTabLayout->addWidget(m_transferBox);
    auto* datasetsInfoTabs = new QTabWidget(datasetsTab);
    datasetsInfoTabs->setDocumentMode(false);
    auto* mountedLeftTab = new QWidget(datasetsInfoTabs);
    auto* mountedLeftLayout = new QVBoxLayout(mountedLeftTab);
    m_mountedDatasetsTableLeft = new QTableWidget(mountedLeftTab);
    m_mountedDatasetsTableLeft->setColumnCount(2);
    m_mountedDatasetsTableLeft->setHorizontalHeaderLabels(
        {tr3(QStringLiteral("Dataset"), QStringLiteral("Dataset"), QStringLiteral("数据集")),
         QStringLiteral("mountpoint")});
    m_mountedDatasetsTableLeft->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_mountedDatasetsTableLeft->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_mountedDatasetsTableLeft->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mountedDatasetsTableLeft->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mountedDatasetsTableLeft->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mountedDatasetsTableLeft->verticalHeader()->setVisible(false);
    m_mountedDatasetsTableLeft->verticalHeader()->setDefaultSectionSize(22);
    m_mountedDatasetsTableLeft->setWordWrap(false);
    m_mountedDatasetsTableLeft->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_mountedDatasetsTableLeft->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    {
        QFont f = m_mountedDatasetsTableLeft->font();
        f.setPointSize(qMax(6, f.pointSize() - 2));
        m_mountedDatasetsTableLeft->setFont(f);
        QFont hf = f;
        hf.setPointSize(f.pointSize());
        hf.setBold(false);
        m_mountedDatasetsTableLeft->horizontalHeader()->setFont(hf);
    }
    m_mountedDatasetsTableLeft->setStyleSheet(
        QStringLiteral("QScrollBar:vertical{width:8px;} "
                       "QScrollBar:horizontal{height:8px;}"));
    m_mountedDatasetsTableLeft->setColumnWidth(0, 180);
    m_mountedDatasetsTableLeft->setColumnWidth(1, 220);
    enableSortableHeader(m_mountedDatasetsTableLeft);
    mountedLeftLayout->addWidget(m_mountedDatasetsTableLeft, 1);
    auto* propsLeftTab = new QWidget(datasetsInfoTabs);
    auto* propsLeftLayout = new QVBoxLayout(propsLeftTab);
    propsLeftLayout->setContentsMargins(0, 0, 0, 0);
    propsLeftLayout->setSpacing(4);
    m_datasetPropsTable = new QTableWidget(propsLeftTab);
    m_datasetPropsTable->setColumnCount(3);
    m_datasetPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Inherit")});
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_datasetPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_datasetPropsTable->verticalHeader()->setVisible(false);
    m_datasetPropsTable->verticalHeader()->setDefaultSectionSize(22);
    m_datasetPropsTable->setProperty("pinned_rows", 5);
    m_datasetPropsTable->setFont(m_mountedDatasetsTableLeft->font());
    {
        QFont hf = m_datasetPropsTable->font();
        hf.setBold(false);
        m_datasetPropsTable->horizontalHeader()->setFont(hf);
    }
    m_btnApplyDatasetProps = new QPushButton(
        tr3(QStringLiteral("Aplicar cambios"), QStringLiteral("Apply changes"), QStringLiteral("应用更改")),
        propsLeftTab);
    m_btnApplyDatasetProps->setEnabled(false);
    enableSortableHeader(m_datasetPropsTable);
    propsLeftLayout->addWidget(m_datasetPropsTable, 1);
    propsLeftLayout->addWidget(m_btnApplyDatasetProps, 0, Qt::AlignRight);
    datasetsInfoTabs->addTab(
        propsLeftTab,
        tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")));
    datasetsInfoTabs->addTab(
        mountedLeftTab,
        tr3(QStringLiteral("Montados"), QStringLiteral("Mounted"), QStringLiteral("已挂载")));
    dsLeftTabLayout->addWidget(datasetsInfoTabs, 1);
    datasetsTab->setLayout(dsLeftTabLayout);

    auto* advancedTab = new QWidget(m_leftTabs);
    auto* advLeftTabLayout = new QVBoxLayout(advancedTab);
    advLeftTabLayout->setContentsMargins(4, 4, 4, 4);
    advLeftTabLayout->setSpacing(4);
    m_advCommandsBox = new QGroupBox(tr3(QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]")), advancedTab);
    auto* commandsLayout = new QVBoxLayout(m_advCommandsBox);
    commandsLayout->setSpacing(10);
    m_btnAdvancedBreakdown = new QPushButton(tr3(QStringLiteral("Desglosar"), QStringLiteral("Break down"), QStringLiteral("拆分")), m_advCommandsBox);
    m_btnAdvancedAssemble = new QPushButton(tr3(QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("组装")), m_advCommandsBox);
    m_btnAdvancedFromDir = new QPushButton(tr3(QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")), m_advCommandsBox);
    m_btnAdvancedToDir = new QPushButton(tr3(QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")), m_advCommandsBox);
    m_btnAdvancedBreakdown->setToolTip(
        tr3(QStringLiteral("Construye datasets a partir de directorios. "
                           "Requiere dataset y descendientes montados. "
                           "Permite seleccionar directorios a desglosar. "
                           "No se ejecuta si hay conflictos de mountpoint."),
            QStringLiteral("Builds datasets from directories. "
                           "Requires dataset and descendants mounted. "
                           "Lets you select directories to split. "
                           "Will not run if mountpoint conflicts exist."),
            QStringLiteral("从目录构建数据集。"
                           "要求数据集及其后代已挂载。"
                           "可选择要拆分的目录。"
                           "若存在挂载点冲突则不会执行。")));
    m_btnAdvancedAssemble->setToolTip(
        tr3(QStringLiteral("Convierte datasets en directorios. "
                           "Requiere dataset y descendientes montados. "
                           "Permite seleccionar subdatasets a ensamblar. "
                           "zfs destroy solo se ejecuta si rsync finaliza OK."),
            QStringLiteral("Converts datasets into directories. "
                           "Requires dataset and descendants mounted. "
                           "Lets you select child datasets to assemble. "
                           "zfs destroy runs only if rsync succeeds."),
            QStringLiteral("将数据集转换为目录。"
                           "要求数据集及其后代已挂载。"
                           "可选择要组装的子数据集。"
                           "仅当 rsync 成功时才执行 zfs destroy。")));
    m_btnAdvancedFromDir->setToolTip(
        tr3(QStringLiteral("Crea un dataset hijo usando un directorio local como mountpoint.\n"
                           "Requiere dataset seleccionado en Avanzado."),
            QStringLiteral("Create a child dataset using a local directory as mountpoint.\n"
                           "Requires a dataset selected in Advanced."),
            QStringLiteral("使用本地目录作为挂载点创建子数据集。\n"
                           "需要在高级页选择一个数据集。")));
    m_btnAdvancedToDir->setToolTip(
        tr3(QStringLiteral("Hace lo contrario de Desde Dir: copia el contenido del dataset a un directorio local\n"
                           "y elimina el dataset al finalizar correctamente."),
            QStringLiteral("Inverse of From Dir: copy dataset content to a local directory\n"
                           "and remove dataset when finished successfully."),
            QStringLiteral("与“来自目录”相反：将数据集内容复制到本地目录，\n"
                           "成功后删除该数据集。")));
    const int transferBtnH = m_btnCopy ? m_btnCopy->sizeHint().height() : m_btnAdvancedBreakdown->sizeHint().height();
    m_btnAdvancedBreakdown->setFixedHeight(transferBtnH);
    m_btnAdvancedAssemble->setFixedHeight(transferBtnH);
    m_btnAdvancedFromDir->setFixedHeight(transferBtnH);
    m_btnAdvancedToDir->setFixedHeight(transferBtnH);
    m_btnAdvancedBreakdown->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedAssemble->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedFromDir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedToDir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedBreakdown->setEnabled(false);
    m_btnAdvancedAssemble->setEnabled(false);
    m_btnAdvancedFromDir->setEnabled(false);
    m_btnAdvancedToDir->setEnabled(false);
    auto* commandsButtonsRow = new QHBoxLayout();
    commandsButtonsRow->setSpacing(8);
    commandsButtonsRow->addWidget(m_btnAdvancedBreakdown);
    commandsButtonsRow->addWidget(m_btnAdvancedAssemble);
    commandsButtonsRow->addWidget(m_btnAdvancedFromDir);
    commandsButtonsRow->addWidget(m_btnAdvancedToDir);
    commandsLayout->addLayout(commandsButtonsRow);
    // Igualar altura de las cajas de acciones de panel izquierdo:
    // Conexiones, Datasets (Origen-->Destino) y Avanzado (Comandos).
    const int actionsBoxHeight = qMax(72, connButtonsBox->sizeHint().height());
    connButtonsBox->setFixedHeight(actionsBoxHeight);
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setFixedHeight(actionsBoxHeight);
    }
    m_transferBox->setFixedHeight(actionsBoxHeight);
    m_advCommandsBox->setFixedHeight(actionsBoxHeight);

    auto* advancedInfoTabs = new QTabWidget(advancedTab);
    advancedInfoTabs->setDocumentMode(false);
    auto* mountedAdvTab = new QWidget(advancedInfoTabs);
    auto* mountedAdvLayout = new QVBoxLayout(mountedAdvTab);
    m_mountedDatasetsTableAdv = new QTableWidget(mountedAdvTab);
    m_mountedDatasetsTableAdv->setColumnCount(2);
    m_mountedDatasetsTableAdv->setHorizontalHeaderLabels(
        {tr3(QStringLiteral("Dataset"), QStringLiteral("Dataset"), QStringLiteral("数据集")),
         QStringLiteral("mountpoint")});
    m_mountedDatasetsTableAdv->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_mountedDatasetsTableAdv->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_mountedDatasetsTableAdv->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mountedDatasetsTableAdv->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mountedDatasetsTableAdv->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mountedDatasetsTableAdv->verticalHeader()->setVisible(false);
    m_mountedDatasetsTableAdv->verticalHeader()->setDefaultSectionSize(22);
    m_mountedDatasetsTableAdv->setWordWrap(false);
    m_mountedDatasetsTableAdv->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_mountedDatasetsTableAdv->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    {
        QFont f = m_mountedDatasetsTableAdv->font();
        f.setPointSize(qMax(6, f.pointSize() - 2));
        m_mountedDatasetsTableAdv->setFont(f);
        QFont hf = f;
        hf.setPointSize(f.pointSize());
        hf.setBold(false);
        m_mountedDatasetsTableAdv->horizontalHeader()->setFont(hf);
    }
    m_mountedDatasetsTableAdv->setStyleSheet(
        QStringLiteral("QScrollBar:vertical{width:8px;} "
                       "QScrollBar:horizontal{height:8px;}"));
    m_mountedDatasetsTableAdv->setColumnWidth(0, 180);
    m_mountedDatasetsTableAdv->setColumnWidth(1, 220);
    enableSortableHeader(m_mountedDatasetsTableAdv);
    mountedAdvLayout->addWidget(m_mountedDatasetsTableAdv, 1);
    auto* propsAdvTab = new QWidget(advancedInfoTabs);
    auto* propsAdvLayout = new QVBoxLayout(propsAdvTab);
    propsAdvLayout->setContentsMargins(0, 0, 0, 0);
    propsAdvLayout->setSpacing(4);
    m_advPropsTable = new QTableWidget(propsAdvTab);
    m_advPropsTable->setColumnCount(3);
    m_advPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Inherit")});
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_advPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_advPropsTable->verticalHeader()->setVisible(false);
    m_advPropsTable->verticalHeader()->setDefaultSectionSize(22);
    m_advPropsTable->setProperty("pinned_rows", 5);
    m_advPropsTable->setFont(m_mountedDatasetsTableAdv->font());
    {
        QFont hf = m_advPropsTable->font();
        hf.setBold(false);
        m_advPropsTable->horizontalHeader()->setFont(hf);
    }
    m_btnApplyAdvancedProps = new QPushButton(
        tr3(QStringLiteral("Aplicar cambios"), QStringLiteral("Apply changes"), QStringLiteral("应用更改")),
        propsAdvTab);
    m_btnApplyAdvancedProps->setEnabled(false);
    enableSortableHeader(m_advPropsTable);
    propsAdvLayout->addWidget(m_advPropsTable, 1);
    propsAdvLayout->addWidget(m_btnApplyAdvancedProps, 0, Qt::AlignRight);
    advancedInfoTabs->addTab(
        propsAdvTab,
        tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")));
    advancedInfoTabs->addTab(
        mountedAdvTab,
        tr3(QStringLiteral("Montados"), QStringLiteral("Mounted"), QStringLiteral("已挂载")));
    advLeftTabLayout->setSpacing(8);
    advLeftTabLayout->addWidget(m_advCommandsBox);
    advLeftTabLayout->addWidget(advancedInfoTabs, 1);
    advancedTab->setLayout(advLeftTabLayout);

    m_leftTabs->addTab(connectionsTab, tr3(QStringLiteral("Conexiones"), QStringLiteral("Connections"), QStringLiteral("连接")));
    m_leftTabs->addTab(datasetsTab, tr3(QStringLiteral("Datasets"), QStringLiteral("Datasets"), QStringLiteral("数据集")));
    m_leftTabs->addTab(advancedTab, tr3(QStringLiteral("Avanzado"), QStringLiteral("Advanced"), QStringLiteral("高级")));
    leftLayout->addWidget(m_leftTabs, 1);

    auto* rightPane = new QWidget(topArea);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    m_rightStack = new QStackedWidget(rightPane);

    auto* rightConnectionsPage = new QWidget(m_rightStack);
    auto* rightConnectionsLayout = new QVBoxLayout(rightConnectionsPage);
    rightConnectionsLayout->setContentsMargins(0, 0, 0, 0);
    rightConnectionsLayout->setSpacing(4);
    m_rightTabs = new QTabWidget(rightConnectionsPage);
    m_rightTabs->setDocumentMode(false);

    auto* importedTab = new QWidget(m_rightTabs);
    auto* importedLayout = new QVBoxLayout(importedTab);
    m_importedPoolsTable = new QTableWidget(importedTab);
    m_importedPoolsTable->setColumnCount(5);
    m_importedPoolsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Conexión"), QStringLiteral("Pool"), QStringLiteral("Estado"), QStringLiteral("Importado"), QStringLiteral("Motivo")});
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_importedPoolsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_importedPoolsTable->setContextMenuPolicy(Qt::NoContextMenu);
    m_importedPoolsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_importedPoolsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_importedPoolsTable->verticalHeader()->setVisible(false);
    {
        QFont f = m_importedPoolsTable->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_importedPoolsTable->setFont(f);
    }
    m_importedPoolsTable->verticalHeader()->setDefaultSectionSize(22);
    m_importedPoolsTable->setStyleSheet(QStringLiteral("QTableWidget::item{padding:1px 3px;}"));
    enableSortableHeader(m_importedPoolsTable);
    importedLayout->addWidget(m_importedPoolsTable, 1);
    m_rightTabs->addTab(importedTab, tr3(QStringLiteral("Pools"), QStringLiteral("Pools"), QStringLiteral("存储池")));

    m_poolDetailTabs = new QTabWidget(rightConnectionsPage);
    m_poolDetailTabs->setDocumentMode(false);
    auto* propsPoolTab = new QWidget(m_poolDetailTabs);
    auto* propsPoolLayout = new QVBoxLayout(propsPoolTab);
    m_poolPropsTable = new QTableWidget(propsPoolTab);
    m_poolPropsTable->setColumnCount(3);
    m_poolPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Origen")});
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_poolPropsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_poolPropsTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_poolPropsTable->verticalHeader()->setVisible(false);
    m_poolPropsTable->verticalHeader()->setDefaultSectionSize(22);
    enableSortableHeader(m_poolPropsTable);
    propsPoolLayout->addWidget(m_poolPropsTable, 1);

    auto* statusPoolTab = new QWidget(m_poolDetailTabs);
    auto* statusPoolLayout = new QVBoxLayout(statusPoolTab);
    auto* statusBody = new QHBoxLayout();
    statusBody->setContentsMargins(0, 0, 0, 0);
    statusBody->setSpacing(6);
    auto* statusActionsWrap = new QWidget(statusPoolTab);
    auto* statusActions = new QVBoxLayout(statusActionsWrap);
    statusActions->setContentsMargins(0, 0, 0, 0);
    statusActions->setSpacing(6);
    m_poolStatusRefreshBtn = new QPushButton(QStringLiteral("Actualizar"), statusPoolTab);
    m_poolStatusRefreshBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    statusActions->addWidget(m_poolStatusRefreshBtn);
    m_poolStatusImportBtn = new QPushButton(QStringLiteral("Importar"), statusPoolTab);
    m_poolStatusImportBtn->setEnabled(false);
    m_poolStatusImportBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    statusActions->addWidget(m_poolStatusImportBtn);
    m_poolStatusExportBtn = new QPushButton(QStringLiteral("Exportar"), statusPoolTab);
    m_poolStatusExportBtn->setEnabled(false);
    m_poolStatusExportBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    statusActions->addWidget(m_poolStatusExportBtn);
    m_poolStatusScrubBtn = new QPushButton(QStringLiteral("Scrub"), statusPoolTab);
    m_poolStatusScrubBtn->setEnabled(false);
    m_poolStatusScrubBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    statusActions->addWidget(m_poolStatusScrubBtn);
    m_poolStatusDestroyBtn = new QPushButton(QStringLiteral("Destroy"), statusPoolTab);
    m_poolStatusDestroyBtn->setEnabled(false);
    m_poolStatusDestroyBtn->setStyleSheet(QStringLiteral("QPushButton { color: #b00020; font-weight: 700; }"));
    m_poolStatusDestroyBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    statusActions->addWidget(m_poolStatusDestroyBtn);
    const int statusButtonsWidth = qMax(m_poolStatusRefreshBtn->sizeHint().width(),
                                        qMax(m_poolStatusImportBtn->sizeHint().width(),
                                             qMax(m_poolStatusExportBtn->sizeHint().width(),
                                                  qMax(m_poolStatusScrubBtn->sizeHint().width(),
                                                       m_poolStatusDestroyBtn->sizeHint().width()))));
    m_poolStatusRefreshBtn->setMinimumWidth(statusButtonsWidth);
    m_poolStatusImportBtn->setMinimumWidth(statusButtonsWidth);
    m_poolStatusExportBtn->setMinimumWidth(statusButtonsWidth);
    m_poolStatusScrubBtn->setMinimumWidth(statusButtonsWidth);
    m_poolStatusDestroyBtn->setMinimumWidth(statusButtonsWidth);
    statusActions->addStretch(1);
    m_poolStatusText = new QPlainTextEdit(statusPoolTab);
    m_poolStatusText->setReadOnly(true);
    m_poolStatusText->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_poolStatusText->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    {
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSize(9);
        m_poolStatusText->setFont(mono);
    }
    statusBody->addWidget(statusActionsWrap, 0, Qt::AlignTop);
    statusBody->addWidget(m_poolStatusText, 1);
    statusPoolLayout->addLayout(statusBody, 1);

    m_poolDetailTabs->addTab(statusPoolTab, tr3(QStringLiteral("Estado"), QStringLiteral("Status"), QStringLiteral("状态")));
    m_poolDetailTabs->addTab(propsPoolTab, tr3(QStringLiteral("Propiedades del pool"), QStringLiteral("Pool properties"), QStringLiteral("存储池属性")));

    auto* connDetailSplit = new QSplitter(Qt::Vertical, rightConnectionsPage);
    connDetailSplit->setChildrenCollapsible(false);
    connDetailSplit->setHandleWidth(1);
    connDetailSplit->addWidget(m_rightTabs);
    connDetailSplit->addWidget(m_poolDetailTabs);
    connDetailSplit->setStretchFactor(0, 55);
    connDetailSplit->setStretchFactor(1, 45);
    rightConnectionsLayout->setSpacing(0);
    rightConnectionsLayout->addWidget(connDetailSplit, 1);

    auto* rightDatasetsPage = new QWidget(m_rightStack);
    auto* rightDatasetsLayout = new QVBoxLayout(rightDatasetsPage);
    rightDatasetsLayout->setContentsMargins(0, 0, 0, 0);
    rightDatasetsLayout->setSpacing(4);
    auto* dsLeft = new QWidget(rightDatasetsPage);
    auto* dsLeftLayout = new QVBoxLayout(dsLeft);
    dsLeftLayout->setContentsMargins(0, 0, 0, 0);
    dsLeftLayout->setSpacing(4);

    auto* originPane = new QWidget(dsLeft);
    auto* originLayout = new QVBoxLayout(originPane);
    originLayout->setContentsMargins(0, 0, 0, 0);
    originLayout->setSpacing(4);
    auto* originTop = new QHBoxLayout();
    originTop->setContentsMargins(0, 0, 0, 0);
    originTop->setSpacing(6);
    auto* originLabel = new QLabel(tr3(QStringLiteral("Origen"), QStringLiteral("Source"), QStringLiteral("源")), originPane);
    m_originPoolCombo = new QComboBox(originPane);
    m_originPoolCombo->setMinimumContentsLength(8);
    m_originPoolCombo->setMaximumWidth(140);
    m_originPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_originTree = new QTreeWidget(originPane);
    m_originTree->setColumnCount(4);
    m_originTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot"), QStringLiteral("Montado"), QStringLiteral("Mountpoint")});
    m_originTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_originTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_originTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_originTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_originTree->header()->setStretchLastSection(false);
    m_originTree->setColumnWidth(0, 250);
    m_originTree->setColumnWidth(1, 90);
    m_originTree->setColumnWidth(2, 72);
    m_originTree->setColumnWidth(3, 180);
    m_originTree->setUniformRowHeights(true);
    m_originTree->setRootIsDecorated(true);
    m_originTree->setItemsExpandable(true);
    {
        QFont f = m_originTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_originTree->setFont(f);
    }
    m_originTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_originTree->setStyle(fusion);
    }
#endif
    m_originSelectionLabel = new QLabel(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")), originPane);
    m_originSelectionLabel->setWordWrap(true);
    m_originSelectionLabel->setMinimumHeight(36);
    originTop->addWidget(originLabel, 0);
    originTop->addWidget(m_originPoolCombo, 0);
    originTop->addWidget(m_originSelectionLabel, 1);
    originLayout->addLayout(originTop);
    originLayout->addWidget(m_originTree, 1);

    auto* destPane = new QWidget(dsLeft);
    auto* destLayout = new QVBoxLayout(destPane);
    destLayout->setContentsMargins(0, 0, 0, 0);
    destLayout->setSpacing(4);
    auto* destTop = new QHBoxLayout();
    destTop->setContentsMargins(0, 0, 0, 0);
    destTop->setSpacing(6);
    auto* destLabel = new QLabel(tr3(QStringLiteral("Destino"), QStringLiteral("Target"), QStringLiteral("目标")), destPane);
    m_destPoolCombo = new QComboBox(destPane);
    m_destPoolCombo->setMinimumContentsLength(8);
    m_destPoolCombo->setMaximumWidth(140);
    m_destPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_destTree = new QTreeWidget(destPane);
    m_destTree->setColumnCount(4);
    m_destTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot"), QStringLiteral("Montado"), QStringLiteral("Mountpoint")});
    m_destTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_destTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_destTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_destTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_destTree->header()->setStretchLastSection(false);
    m_destTree->setColumnWidth(0, 250);
    m_destTree->setColumnWidth(1, 90);
    m_destTree->setColumnWidth(2, 72);
    m_destTree->setColumnWidth(3, 180);
    m_destTree->setUniformRowHeights(true);
    m_destTree->setRootIsDecorated(true);
    m_destTree->setItemsExpandable(true);
    {
        QFont f = m_destTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_destTree->setFont(f);
    }
    m_destTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_destTree->setStyle(fusion);
    }
#endif
    m_destSelectionLabel = new QLabel(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")), destPane);
    m_destSelectionLabel->setWordWrap(true);
    m_destSelectionLabel->setMinimumHeight(36);
    destTop->addWidget(destLabel, 0);
    destTop->addWidget(m_destPoolCombo, 0);
    destTop->addWidget(m_destSelectionLabel, 1);
    destLayout->addLayout(destTop);
    destLayout->addWidget(m_destTree, 1);

    dsLeftLayout->addWidget(originPane, 1);
    dsLeftLayout->addWidget(destPane, 1);
    rightDatasetsLayout->addWidget(dsLeft, 1);

    auto* rightAdvancedPage = new QWidget(m_rightStack);
    auto* rightAdvancedLayout = new QVBoxLayout(rightAdvancedPage);
    rightAdvancedLayout->setContentsMargins(0, 0, 0, 0);
    rightAdvancedLayout->setSpacing(4);
    auto* advLeft = new QWidget(rightAdvancedPage);
    auto* advLeftLayout = new QVBoxLayout(advLeft);
    m_advPoolCombo = new QComboBox(rightAdvancedPage);
    m_advPoolCombo->setMinimumContentsLength(6);
    m_advPoolCombo->setMaximumWidth(110);
    m_advPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_advTree = new QTreeWidget(rightAdvancedPage);
    m_advTree->setColumnCount(4);
    m_advTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot"), QStringLiteral("Montado"), QStringLiteral("Mountpoint")});
    m_advTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_advTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_advTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_advTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_advTree->header()->setStretchLastSection(false);
    m_advTree->setColumnWidth(0, 250);
    m_advTree->setColumnWidth(1, 90);
    m_advTree->setColumnWidth(2, 72);
    m_advTree->setColumnWidth(3, 180);
    m_advTree->setUniformRowHeights(true);
    m_advTree->setRootIsDecorated(true);
    m_advTree->setItemsExpandable(true);
    {
        QFont f = m_advTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_advTree->setFont(f);
    }
    m_advTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_advTree->setStyle(fusion);
    }
#endif
    m_advSelectionLabel = new QLabel(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")), rightAdvancedPage);
    m_advSelectionLabel->setWordWrap(true);
    m_advSelectionLabel->setMinimumHeight(36);
    auto* advTop = new QHBoxLayout();
    advTop->addWidget(m_advPoolCombo, 0);
    advTop->addWidget(m_advSelectionLabel, 1);
    advLeftLayout->addLayout(advTop);
    advLeftLayout->addWidget(m_advTree, 1);
    rightAdvancedLayout->addWidget(advLeft, 1);

    m_rightStack->addWidget(rightConnectionsPage);
    m_rightStack->addWidget(rightDatasetsPage);
    m_rightStack->addWidget(rightAdvancedPage);
    rightLayout->addWidget(m_rightStack, 1);

    topLayout->addWidget(leftPane, 0);
    topLayout->addWidget(rightPane, 1);
    root->addWidget(topArea, 81);

    auto* logBox = new QGroupBox(tr3(QStringLiteral("Log combinado"), QStringLiteral("Combined log"), QStringLiteral("组合日志")), central);
    auto* logLayout = new QVBoxLayout(logBox);
    logLayout->setContentsMargins(6, 6, 6, 6);
    logLayout->setSpacing(4);
    auto* logBody = new QHBoxLayout();

    auto* leftInfo = new QWidget(logBox);
    auto* leftInfoLayout = new QVBoxLayout(leftInfo);
    leftInfoLayout->setContentsMargins(0, 0, 0, 0);
    leftInfoLayout->setSpacing(4);
    auto* statusTitle = new QLabel(tr3(QStringLiteral("Estado"), QStringLiteral("Status"), QStringLiteral("状态")), leftInfo);
    QFont smallTitle = statusTitle->font();
    smallTitle.setBold(true);
    smallTitle.setPointSize(qMax(6, smallTitle.pointSize() - 1));
    statusTitle->setFont(smallTitle);
    m_statusText = new QTextEdit(leftInfo);
    m_statusText->setReadOnly(true);
    m_statusText->setAcceptRichText(false);
    m_statusText->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_statusText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_statusText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    {
        QFont f = m_statusText->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_statusText->setFont(f);
    }
    auto* detailTitle = new QLabel(tr3(QStringLiteral("Detalle"), QStringLiteral("Detail"), QStringLiteral("详情")), leftInfo);
    detailTitle->setFont(smallTitle);
    m_lastDetailText = new QTextEdit(leftInfo);
    m_lastDetailText->setReadOnly(true);
    m_lastDetailText->setAcceptRichText(false);
    m_lastDetailText->setLineWrapMode(QTextEdit::WidgetWidth);
    m_lastDetailText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    {
        QFont f = m_lastDetailText->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_lastDetailText->setFont(f);
    }
    statusTitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    detailTitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    statusTitle->setMinimumWidth(48);
    detailTitle->setMinimumWidth(48);
    statusTitle->setMaximumWidth(64);
    detailTitle->setMaximumWidth(64);
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(6);
    statusRow->addWidget(statusTitle, 0);
    statusRow->addWidget(m_statusText, 1);
    auto* detailRow = new QHBoxLayout();
    detailRow->setContentsMargins(0, 0, 0, 0);
    detailRow->setSpacing(6);
    detailRow->addWidget(detailTitle, 0);
    detailRow->addWidget(m_lastDetailText, 1);
    leftInfoLayout->addLayout(statusRow, 1);
    leftInfoLayout->addLayout(detailRow, 1);

    auto* rightLogs = new QWidget(logBox);
    auto* rightLogsLayout = new QVBoxLayout(rightLogs);
    rightLogsLayout->setContentsMargins(0, 0, 0, 0);
    rightLogsLayout->setSpacing(4);
    auto* rightLogsBody = new QHBoxLayout();
    rightLogsBody->setContentsMargins(0, 0, 0, 0);
    rightLogsBody->setSpacing(6);
    m_logsTabs = new QTabWidget(rightLogs);
    m_logsTabs->setDocumentMode(false);
    m_logsTabs->setStyleSheet(
        QStringLiteral("QTabWidget::tab-bar { alignment: left; }"
                       "QTabBar::tab { padding: 1px 8px; min-height: 10px; }"
                       "QTabBar::tab:selected { min-height: 12px; }"
                       "QTabBar::tab:!selected { margin-top: 2px; }"));
    auto* appTab = new QWidget(m_logsTabs);
    auto* appTabLayout = new QVBoxLayout(appTab);
    m_logView = new QPlainTextEdit(appTab);
    m_logView->setReadOnly(true);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(9);
    m_logView->setFont(mono);
    appTabLayout->addWidget(m_logView, 1);
    m_logsTabs->addTab(appTab, tr3(QStringLiteral("Aplicación"), QStringLiteral("Application"), QStringLiteral("应用")));
    rightLogsBody->addWidget(m_logsTabs, 1);

    auto* controlsPane = new QWidget(rightLogs);
    controlsPane->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* logControls = new QVBoxLayout(controlsPane);
    logControls->setContentsMargins(0, 0, 0, 0);
    logControls->setSpacing(4);
    m_logLevelCombo = new QComboBox(rightLogs);
    m_logLevelCombo->addItems({QStringLiteral("normal"), QStringLiteral("info"), QStringLiteral("debug")});
    m_logLevelCombo->setCurrentText(QStringLiteral("normal"));
    m_logMaxLinesCombo = new QComboBox(rightLogs);
    m_logMaxLinesCombo->addItems({QStringLiteral("100"), QStringLiteral("200"), QStringLiteral("500"), QStringLiteral("1000")});
    m_logMaxLinesCombo->setCurrentText(QStringLiteral("500"));
    m_logClearBtn = new QPushButton(tr3(QStringLiteral("Limpiar"), QStringLiteral("Clear"), QStringLiteral("清空")), rightLogs);
    m_logCopyBtn = new QPushButton(tr3(QStringLiteral("Copiar"), QStringLiteral("Copy"), QStringLiteral("复制")), rightLogs);
    m_logCancelBtn = new QPushButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), rightLogs);
    m_logCancelBtn->setVisible(false);
    m_logLevelCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_logMaxLinesCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_logClearBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_logCopyBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_logCancelBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    {
        QFont cf = m_logLevelCombo->font();
        cf.setPointSize(qMax(6, cf.pointSize() - 1));
        m_logLevelCombo->setFont(cf);
        m_logMaxLinesCombo->setFont(cf);
        m_logClearBtn->setFont(cf);
        m_logCopyBtn->setFont(cf);
        m_logCancelBtn->setFont(cf);
    }
    int ctrlW = 0;
    ctrlW = qMax(ctrlW, m_logLevelCombo->sizeHint().width());
    ctrlW = qMax(ctrlW, m_logMaxLinesCombo->sizeHint().width());
    ctrlW = qMax(ctrlW, m_logClearBtn->sizeHint().width());
    ctrlW = qMax(ctrlW, m_logCopyBtn->sizeHint().width());
    ctrlW = qMax(ctrlW, m_logCancelBtn->sizeHint().width());
    ctrlW = qMax(ctrlW, 84);
    m_logLevelCombo->setFixedWidth(ctrlW);
    m_logMaxLinesCombo->setFixedWidth(ctrlW);
    m_logClearBtn->setFixedWidth(ctrlW);
    m_logCopyBtn->setFixedWidth(ctrlW);
    m_logCancelBtn->setFixedWidth(ctrlW);
    controlsPane->setFixedWidth(ctrlW + 8);
    logControls->addWidget(m_logLevelCombo, 0);
    logControls->addWidget(m_logMaxLinesCombo, 0);
    logControls->addWidget(m_logClearBtn, 0);
    logControls->addWidget(m_logCopyBtn, 0);
    logControls->addWidget(m_logCancelBtn, 0);
    logControls->addStretch(1);
    rightLogsBody->addWidget(controlsPane, 0);
    rightLogsLayout->addLayout(rightLogsBody, 1);

    logBody->addWidget(leftInfo, 1);
    logBody->addWidget(rightLogs, 2);
    logLayout->addLayout(logBody, 1);
    root->addWidget(logBox, 19);

    setCentralWidget(central);

    connect(m_btnRefreshAll, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Refrescar todo (botón)"));
        refreshAllConnections();
    });
    connect(m_btnNew, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Nueva conexión (botón)"));
        createConnection();
    });
    connect(m_btnConfig, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Configuración (botón)"));
        openConfigurationDialog();
    });
    connect(m_btnPoolNew, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Nuevo pool (botón)"));
        createPoolForSelectedConnection();
    });
    connect(m_connectionsList, &QTreeWidget::itemSelectionChanged, this, [this]() { onConnectionSelectionChanged(); });
    m_connectionsList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_connectionsList, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onConnectionListContextMenuRequested(pos);
    });
    connect(m_leftTabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx >= 0 && idx < m_rightStack->count()) {
            m_rightStack->setCurrentIndex(idx);
        }
        if (idx == 0) {
            populateAllPoolsTables();
        } else if (idx == 2) {
            onAdvancedPoolChanged();
        }
    });
    connect(m_importedPoolsTable, &QTableWidget::cellClicked, this, [this](int row, int col) {
        Q_UNUSED(row);
        Q_UNUSED(col);
        onPoolsSelectionChanged();
    });
    connect(m_importedPoolsTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        onPoolsSelectionChanged();
    });
    connect(m_poolStatusRefreshBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Actualizar estado de pool (botón)"));
        if (m_importedPoolsTable && !m_importedPoolsTable->selectedItems().isEmpty()) {
            refreshSelectedPoolDetails();
        }
    });
    connect(m_poolStatusImportBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Importar pool (botón Estado)"));
        importPoolFromRow(sel.first()->row());
    });
    connect(m_poolStatusExportBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Exportar pool (botón Estado)"));
        exportPoolFromRow(sel.first()->row());
    });
    connect(m_poolStatusScrubBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Scrub pool (botón Estado)"));
        scrubPoolFromRow(sel.first()->row());
    });
    connect(m_poolStatusDestroyBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Destroy pool (botón Estado)"));
        destroyPoolFromRow(sel.first()->row());
    });
    connect(m_originPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onOriginPoolChanged(); });
    connect(m_destPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onDestPoolChanged(); });
    connect(m_advPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onAdvancedPoolChanged(); });
    connect(m_originTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onOriginTreeSelectionChanged(); });
    connect(m_destTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onDestTreeSelectionChanged(); });
    connect(m_originTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        onDatasetTreeItemChanged(m_originTree, item, col, QStringLiteral("origin"));
    });
    connect(m_destTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        onDatasetTreeItemChanged(m_destTree, item, col, QStringLiteral("dest"));
    });
    connect(m_advTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        onDatasetTreeItemChanged(m_advTree, item, col, QStringLiteral("advanced"));
    });
    connect(m_originTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        onOriginTreeItemDoubleClicked(item, col);
    });
    connect(m_destTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        onDestTreeItemDoubleClicked(item, col);
    });
    m_originTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_destTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_advTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_originTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onOriginTreeContextMenuRequested(pos);
    });
    connect(m_destTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onDestTreeContextMenuRequested(pos);
    });
    connect(m_advTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showDatasetContextMenu(QStringLiteral("advanced"), m_advTree, pos);
    });
    connect(m_datasetPropsTable, &QTableWidget::cellChanged, this, [this](int row, int col) {
        onDatasetPropsCellChanged(row, col);
    });
    connect(m_btnApplyDatasetProps, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Aplicar propiedades dataset (botón)"));
        applyDatasetPropertyChanges();
    });
    connect(m_advPropsTable, &QTableWidget::cellChanged, this, [this](int row, int col) {
        onAdvancedPropsCellChanged(row, col);
    });
    connect(m_btnApplyAdvancedProps, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Aplicar propiedades avanzadas (botón)"));
        applyAdvancedDatasetPropertyChanges();
    });
    connect(m_btnCopy, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Copiar snapshot (botón)"));
        actionCopySnapshot();
    });
    connect(m_btnLevel, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Nivelar snapshot (botón)"));
        actionLevelSnapshot();
    });
    connect(m_btnSync, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Sincronizar datasets (botón)"));
        actionSyncDatasets();
    });
    connect(m_logClearBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Limpiar log (botón)"));
        clearAppLog();
    });
    connect(m_logCopyBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Copiar log (botón)"));
        copyAppLogToClipboard();
    });
    connect(m_logCancelBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Cancelar acción (botón)"));
        requestCancelRunningAction();
    });
    connect(m_logMaxLinesCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        trimLogWidget(m_logView);
    });
    connect(m_advTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        const auto selected = m_advTree->selectedItems();
        if (selected.isEmpty()) {
            updateAdvancedSelectionUi(QString(), QString());
            refreshDatasetProperties(QStringLiteral("advanced"));
            updateTransferButtonsState();
            return;
        }
        auto* it = selected.first();
        const QString ds = it->data(0, Qt::UserRole).toString();
        const QString snap = it->data(1, Qt::UserRole).toString();
        updateAdvancedSelectionUi(ds, snap);
        refreshDatasetProperties(QStringLiteral("advanced"));
        updateTransferButtonsState();
    });
    connect(m_advTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        Q_UNUSED(item);
        Q_UNUSED(col);
    });
    connect(m_btnAdvancedBreakdown, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Desglosar (botón)"));
        actionAdvancedBreakdown();
    });
    connect(m_btnAdvancedAssemble, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Ensamblar (botón)"));
        actionAdvancedAssemble();
    });
    connect(m_btnAdvancedFromDir, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Desde Dir (botón)"));
        actionAdvancedCreateFromDir();
    });
    connect(m_btnAdvancedToDir, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Hacia Dir (botón)"));
        actionAdvancedToDir();
    });
}

void MainWindow::loadConnections() {
    QMap<QString, ConnectionRuntimeState> prevById;
    QMap<QString, ConnectionRuntimeState> prevByName;
    const int oldCount = qMin(m_profiles.size(), m_states.size());
    for (int i = 0; i < oldCount; ++i) {
        const QString idKey = m_profiles[i].id.trimmed().toLower();
        const QString nameKey = m_profiles[i].name.trimmed().toLower();
        if (!idKey.isEmpty()) {
            prevById[idKey] = m_states[i];
        }
        if (!nameKey.isEmpty()) {
            prevByName[nameKey] = m_states[i];
        }
    }

    const LoadResult loaded = m_store.loadConnections();
    m_profiles = loaded.profiles;
    m_states.clear();
    m_states.resize(m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i) {
        const QString idKey = m_profiles[i].id.trimmed().toLower();
        const QString nameKey = m_profiles[i].name.trimmed().toLower();
        if (!idKey.isEmpty() && prevById.contains(idKey)) {
            m_states[i] = prevById.value(idKey);
            continue;
        }
        if (!nameKey.isEmpty() && prevByName.contains(nameKey)) {
            m_states[i] = prevByName.value(nameKey);
        }
    }

    rebuildConnectionList();
    updateStatus(tr3(QStringLiteral("Estado: %1 conexiones cargadas"),
                     QStringLiteral("Status: %1 connections loaded"),
                     QStringLiteral("状态：已加载 %1 个连接"))
                     .arg(m_profiles.size()));
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }
    rebuildDatasetPoolSelectors();
    syncConnectionLogTabs();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildConnectionList() {
    beginUiBusy();
    m_connectionsList->clear();
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        const auto& s = m_states[i];

        const QString line1 = QStringLiteral("%1").arg(p.name);
        QString zfsTxt = s.zfsVersion.trimmed();
        if (zfsTxt.isEmpty()) {
            zfsTxt = QStringLiteral("?");
        }
        QString statusTag;
        QColor rowColor("#14212b");
        const QString st = s.status.trimmed().toUpper();
        if (st == QStringLiteral("OK")) {
            statusTag = isLocalHostForUi(p.host) ? QStringLiteral("[OK/Local] ") : QStringLiteral("[OK] ");
            rowColor = QColor("#1f7a1f");
        } else if (!st.isEmpty()) {
            statusTag = QStringLiteral("[KO] ");
            rowColor = QColor("#a12a2a");
        }
        QString line = QStringLiteral("%1%2").arg(statusTag, line1);

        auto* item = new QTreeWidgetItem(m_connectionsList);
        item->setText(0, line);
        item->setData(0, Qt::UserRole, i);
        item->setForeground(0, QBrush(rowColor));
        item->setToolTip(0, QStringLiteral("Host: %1\nPort: %2\nEstado: %3\nDetalle: %4")
                                .arg(p.host)
                                .arg(p.port)
                                .arg(s.status)
                                .arg(s.detail));
        item->setExpanded(false);

        const QString osLine = !s.osLine.trimmed().isEmpty()
                                   ? s.osLine.trimmed()
                                   : QStringLiteral("%1").arg(p.osType);
        auto* osChild = new QTreeWidgetItem(item);
        osChild->setText(0, tr3(QStringLiteral("Sistema operativo: %1"),
                                QStringLiteral("Operating system: %1"),
                                QStringLiteral("操作系统：%1")).arg(osLine));
        osChild->setData(0, Qt::UserRole, i);

        const QString method = !s.connectionMethod.trimmed().isEmpty() ? s.connectionMethod.trimmed() : p.connType;
        auto* methodChild = new QTreeWidgetItem(item);
        methodChild->setText(0, tr3(QStringLiteral("Método de conexión: %1"),
                                    QStringLiteral("Connection method: %1"),
                                    QStringLiteral("连接方式：%1")).arg(method));
        methodChild->setData(0, Qt::UserRole, i);

        const QString zfsFull = !s.zfsVersionFull.trimmed().isEmpty() ? s.zfsVersionFull.trimmed()
                                                                       : (zfsTxt == QStringLiteral("?") ? QStringLiteral("-")
                                                                                                        : QStringLiteral("OpenZFS %1").arg(zfsTxt));
        auto* zfsChild = new QTreeWidgetItem(item);
        zfsChild->setText(0, tr3(QStringLiteral("OpenZFS: %1"),
                                 QStringLiteral("OpenZFS: %1"),
                                 QStringLiteral("OpenZFS：%1")).arg(zfsFull));
        zfsChild->setData(0, Qt::UserRole, i);

        auto* commandsNode = new QTreeWidgetItem(item);
        commandsNode->setText(0, tr3(QStringLiteral("Comandos detectados"),
                                     QStringLiteral("Detected commands"),
                                     QStringLiteral("检测到的命令")));
        commandsNode->setData(0, Qt::UserRole, i);

        if (!s.detectedUnixCommands.isEmpty() || !s.missingUnixCommands.isEmpty()) {
            for (const QString& c : s.detectedUnixCommands) {
                auto* cc = new QTreeWidgetItem(commandsNode);
                cc->setText(0, c);
                cc->setForeground(0, QBrush(QColor("#1f7a1f")));
            }
            for (const QString& c : s.missingUnixCommands) {
                auto* cc = new QTreeWidgetItem(commandsNode);
                cc->setText(0, c);
                cc->setForeground(0, QBrush(QColor("#a12a2a")));
            }
        } else if (!s.powershellFallbackCommands.isEmpty()) {
            auto* psHeader = new QTreeWidgetItem(commandsNode);
            psHeader->setText(0, tr3(QStringLiteral("PowerShell (fallback):"),
                                     QStringLiteral("PowerShell (fallback):"),
                                     QStringLiteral("PowerShell（回退）：")));
            for (const QString& c : s.powershellFallbackCommands) {
                auto* cc = new QTreeWidgetItem(commandsNode);
                cc->setText(0, c);
                cc->setForeground(0, QBrush(QColor("#5a4a00")));
            }
        } else {
            auto* none = new QTreeWidgetItem(commandsNode);
            none->setText(0, tr3(QStringLiteral("(sin datos)"),
                                 QStringLiteral("(no data)"),
                                 QStringLiteral("（无数据）")));
        }
    }
    m_connectionsList->collapseAll();
    endUiBusy();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildDatasetPoolSelectors() {
    m_originPoolCombo->blockSignals(true);
    m_destPoolCombo->blockSignals(true);
    m_advPoolCombo->blockSignals(true);

    const QString originPrev = m_originPoolCombo->currentData().toString();
    const QString destPrev = m_destPoolCombo->currentData().toString();
    const QString advPrev = m_advPoolCombo->currentData().toString();

    m_originPoolCombo->clear();
    m_destPoolCombo->clear();
    m_advPoolCombo->clear();

    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& p : st.importedPools) {
            if (p.pool.isEmpty() || p.pool == QStringLiteral("Sin pools")) {
                continue;
            }
            const QString token = QStringLiteral("%1::%2").arg(i).arg(p.pool);
            const QString label = QStringLiteral("%1::%2").arg(m_profiles[i].name, p.pool);
            m_originPoolCombo->addItem(label, token);
            m_destPoolCombo->addItem(label, token);
            m_advPoolCombo->addItem(label, token);
        }
    }

    auto restoreCurrent = [](QComboBox* combo, const QString& token) {
        if (combo->count() <= 0) {
            return;
        }
        int idx = combo->findData(token);
        if (idx < 0) {
            idx = 0;
        }
        combo->setCurrentIndex(idx);
    };
    restoreCurrent(m_originPoolCombo, originPrev);
    restoreCurrent(m_destPoolCombo, destPrev);
    restoreCurrent(m_advPoolCombo, advPrev);

    m_originPoolCombo->blockSignals(false);
    m_destPoolCombo->blockSignals(false);
    m_advPoolCombo->blockSignals(false);
    onOriginPoolChanged();
    onDestPoolChanged();
    onAdvancedPoolChanged();
}

void MainWindow::refreshAllConnections() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: refresh bloqueado"));
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refrescar todas las conexiones"));
    if (m_profiles.isEmpty()) {
        m_refreshInProgress = false;
        updateBusyCursor();
        rebuildConnectionList();
        rebuildDatasetPoolSelectors();
        populateAllPoolsTables();
        updateStatus(tr3(QStringLiteral("Estado: refresco finalizado"),
                         QStringLiteral("Status: refresh finished"),
                         QStringLiteral("状态：刷新完成")));
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = m_profiles.size();
    m_refreshTotal = m_profiles.size();
    m_refreshInProgress = (m_refreshPending > 0);
    updateBusyCursor();
    updateStatus(tr3(QStringLiteral("Estado: refrescando 0/%1"),
                     QStringLiteral("Status: refreshing 0/%1"),
                     QStringLiteral("状态：刷新中 0/%1"))
                     .arg(m_refreshTotal));

    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile profile = m_profiles[i];
        (void)QtConcurrent::run([this, generation, i, profile]() {
            const ConnectionRuntimeState state = refreshConnection(profile);
            QMetaObject::invokeMethod(this, [this, generation, i, state]() {
                onAsyncRefreshResult(generation, i, state);
            }, Qt::QueuedConnection);
        });
    }
}

void MainWindow::refreshSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: refresh bloqueado"));
        return;
    }
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    QTreeWidgetItem* selItem = selected.first();
    while (selItem && selItem->parent()) {
        selItem = selItem->parent();
    }
    const int idx = selItem ? selItem->data(0, Qt::UserRole).toInt() : -1;
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = 1;
    m_refreshTotal = 1;
    m_refreshInProgress = true;
    updateBusyCursor();
    updateStatus(tr3(QStringLiteral("Estado: refrescando 0/1"),
                     QStringLiteral("Status: refreshing 0/1"),
                     QStringLiteral("状态：刷新中 0/1")));
    const ConnectionProfile profile = m_profiles[idx];
    (void)QtConcurrent::run([this, generation, idx, profile]() {
        const ConnectionRuntimeState state = refreshConnection(profile);
        QMetaObject::invokeMethod(this, [this, generation, idx, state]() {
            onAsyncRefreshResult(generation, idx, state);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onAsyncRefreshResult(int generation, int idx, const ConnectionRuntimeState& state) {
    if (generation != m_refreshGeneration) {
        return;
    }
    if (idx < 0 || idx >= m_states.size()) {
        return;
    }
    int selectedIdx = -1;
    const auto selected = m_connectionsList ? m_connectionsList->selectedItems() : QList<QTreeWidgetItem*>{};
    if (!selected.isEmpty()) {
        QTreeWidgetItem* selItem = selected.first();
        while (selItem && selItem->parent()) {
            selItem = selItem->parent();
        }
        selectedIdx = selItem ? selItem->data(0, Qt::UserRole).toInt() : -1;
    }
    m_states[idx] = state;
    rebuildConnectionList();
    if (selectedIdx >= 0 && selectedIdx < m_connectionsList->topLevelItemCount()) {
        if (QTreeWidgetItem* top = m_connectionsList->topLevelItem(selectedIdx)) {
            m_connectionsList->setCurrentItem(top);
        }
    }
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
    if (m_refreshPending > 0) {
        --m_refreshPending;
    }
    const int done = qMax(0, m_refreshTotal - m_refreshPending);
    updateStatus(tr3(QStringLiteral("Estado: refrescando %1/%2"),
                     QStringLiteral("Status: refreshing %1/%2"),
                     QStringLiteral("状态：刷新中 %1/%2"))
                     .arg(done)
                     .arg(qMax(1, m_refreshTotal)));
    if (m_refreshPending == 0) {
        onAsyncRefreshDone(generation);
    }
}

void MainWindow::onAsyncRefreshDone(int generation) {
    if (generation != m_refreshGeneration) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refresco paralelo finalizado"));
    m_refreshInProgress = false;
    updateBusyCursor();
    updateStatus(tr3(QStringLiteral("Estado: refresco finalizado"),
                     QStringLiteral("Status: refresh finished"),
                     QStringLiteral("状态：刷新完成")));
}

void MainWindow::createConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: nueva conexión bloqueada"));
        return;
    }
    ConnectionDialog dlg(this);
    ConnectionProfile p;
    p.connType = QStringLiteral("SSH");
    p.transport = QStringLiteral("SSH");
    p.osType = QStringLiteral("Linux");
    p.port = 22;
    dlg.setProfile(p);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile created = dlg.profile();
    QString createdId = created.id.trimmed();
    if (createdId.isEmpty()) {
        createdId = created.name.trimmed().toLower();
        createdId.replace(' ', '_');
        createdId.replace(':', '_');
        createdId.replace('/', '_');
    }
    QString err;
    if (!m_store.upsertConnection(created, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudo crear conexión:\n%1").arg(err));
        return;
    }
    loadConnections();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == createdId) {
            if (QTreeWidgetItem* top = m_connectionsList->topLevelItem(i)) {
                m_connectionsList->setCurrentItem(top);
            }
            refreshSelectedConnection();
            break;
        }
    }
}

void MainWindow::editConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: edición bloqueada"));
        return;
    }
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    QTreeWidgetItem* selItem = selected.first();
    while (selItem && selItem->parent()) {
        selItem = selItem->parent();
    }
    const int idx = selItem ? selItem->data(0, Qt::UserRole).toInt() : -1;
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    ConnectionDialog dlg(this);
    dlg.setProfile(m_profiles[idx]);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile edited = dlg.profile();
    edited.id = m_profiles[idx].id;
    QString err;
    if (!m_store.upsertConnection(edited, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudo actualizar conexión:\n%1").arg(err));
        return;
    }
    loadConnections();
}

void MainWindow::deleteConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: borrado bloqueado"));
        return;
    }
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    QTreeWidgetItem* selItem = selected.first();
    while (selItem && selItem->parent()) {
        selItem = selItem->parent();
    }
    const int idx = selItem ? selItem->data(0, Qt::UserRole).toInt() : -1;
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        QStringLiteral("Borrar conexión"),
        QStringLiteral("¿Borrar conexión \"%1\"?").arg(m_profiles[idx].name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    QString err;
    if (!m_store.deleteConnectionById(m_profiles[idx].id, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudo borrar conexión:\n%1").arg(err));
        return;
    }
    loadConnections();
}

void MainWindow::onConnectionSelectionChanged() {
    const auto selected = m_connectionsList->selectedItems();
    if (!selected.isEmpty()) {
        QTreeWidgetItem* selItem = selected.first();
        while (selItem && selItem->parent()) {
            selItem = selItem->parent();
        }
        if (selItem && m_connectionsList->currentItem() != selItem) {
            m_connectionsList->setCurrentItem(selItem);
        }
    }
    if (m_leftTabs->currentIndex() == 0) {
        populateAllPoolsTables();
    }
    updatePoolManagementBoxTitle();
}

void MainWindow::onPoolsSelectionChanged() {
    refreshSelectedPoolDetails();
    const int idx = selectedConnectionIndexForPoolManagement();
    if (idx >= 0 && idx < m_profiles.size() && m_connectionsList) {
        for (int i = 0; i < m_connectionsList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = m_connectionsList->topLevelItem(i);
            if (!top) {
                continue;
            }
            if (top->data(0, Qt::UserRole).toInt() == idx) {
                m_connectionsList->setCurrentItem(top);
                break;
            }
        }
    }
    updatePoolManagementBoxTitle();
}

void MainWindow::onConnectionListContextMenuRequested(const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    QTreeWidgetItem* item = m_connectionsList->itemAt(pos);
    if (item) {
        while (item->parent()) {
            item = item->parent();
        }
        m_connectionsList->setCurrentItem(item);
    }
    const bool hasSel = (m_connectionsList->currentItem() != nullptr);

    QMenu menu(this);
    QAction* refreshAct = menu.addAction(QStringLiteral("Refrescar"));
    menu.addSeparator();
    QAction* editAct = menu.addAction(QStringLiteral("Editar"));
    QAction* deleteAct = menu.addAction(QStringLiteral("Borrar"));
    refreshAct->setEnabled(hasSel);
    editAct->setEnabled(hasSel);
    deleteAct->setEnabled(hasSel);

    QAction* picked = menu.exec(m_connectionsList->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == refreshAct) {
        logUiAction(QStringLiteral("Refrescar conexión (menú)"));
        refreshSelectedConnection();
    } else if (picked == editAct) {
        logUiAction(QStringLiteral("Editar conexión (menú)"));
        editConnection();
    } else if (picked == deleteAct) {
        logUiAction(QStringLiteral("Borrar conexión (menú)"));
        deleteConnection();
    }
}

void MainWindow::onImportedPoolsContextMenuRequested(const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    const QModelIndex idx = m_importedPoolsTable->indexAt(pos);
    if (!idx.isValid()) {
        return;
    }
    const int row = idx.row();
    m_importedPoolsTable->selectRow(row);
    QMenu menu(this);
    QAction* exportAct = menu.addAction(QStringLiteral("Exportar"));
    QAction* picked = menu.exec(m_importedPoolsTable->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == exportAct) {
        logUiAction(QStringLiteral("Exportar pool (menú)"));
        exportPoolFromRow(row);
    }
}

void MainWindow::onImportablePoolsContextMenuRequested(const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    const QModelIndex idx = m_importablePoolsTable->indexAt(pos);
    if (!idx.isValid()) {
        return;
    }
    const int row = idx.row();
    m_importablePoolsTable->selectRow(row);
    QMenu menu(this);
    QAction* importAct = menu.addAction(QStringLiteral("Importar"));
    const QString state = m_importablePoolsTable->item(row, 3) ? m_importablePoolsTable->item(row, 3)->text().trimmed().toUpper() : QString();
    importAct->setEnabled(state == QStringLiteral("ONLINE") || state == QStringLiteral("ACTIVE"));
    QAction* picked = menu.exec(m_importablePoolsTable->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == importAct) {
        logUiAction(QStringLiteral("Importar pool (menú)"));
        importPoolFromRow(row);
    }
}

void MainWindow::onOriginPoolChanged() {
    m_originSelectedDataset.clear();
    m_originSelectedSnapshot.clear();
    const QString token = m_originPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_originTree->clear();
        m_originSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_originTree, connIdx, poolName, QStringLiteral("origin"));
    refreshDatasetProperties(QStringLiteral("origin"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onDestPoolChanged() {
    m_destSelectedDataset.clear();
    m_destSelectedSnapshot.clear();
    const QString token = m_destPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_destTree->clear();
        m_destSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_destTree, connIdx, poolName, QStringLiteral("dest"));
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onAdvancedPoolChanged() {
    QString prevDataset;
    QString prevSnapshot;
    if (m_advTree) {
        const auto selected = m_advTree->selectedItems();
        if (!selected.isEmpty()) {
            prevDataset = selected.first()->data(0, Qt::UserRole).toString();
            prevSnapshot = selected.first()->data(1, Qt::UserRole).toString();
        }
    }

    const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        if (m_advTree) {
            m_advTree->clear();
        }
        updateAdvancedSelectionUi(QString(), QString());
        refreshDatasetProperties(QStringLiteral("advanced"));
        updateTransferButtonsState();
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_advTree, connIdx, poolName, QStringLiteral("advanced"));

    QTreeWidgetItem* restored = nullptr;
    if (m_advTree && !prevDataset.isEmpty()) {
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findMatch = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
            if (!node) {
                return nullptr;
            }
            const QString ds = node->data(0, Qt::UserRole).toString();
            const QString snap = node->data(1, Qt::UserRole).toString();
            if (ds == prevDataset && (prevSnapshot.isEmpty() || snap == prevSnapshot)) {
                return node;
            }
            for (int i = 0; i < node->childCount(); ++i) {
                if (auto* found = findMatch(node->child(i))) {
                    return found;
                }
            }
            return nullptr;
        };
        for (int i = 0; i < m_advTree->topLevelItemCount() && !restored; ++i) {
            restored = findMatch(m_advTree->topLevelItem(i));
        }
    }

    if (m_advTree && restored) {
        for (QTreeWidgetItem* p = restored->parent(); p; p = p->parent()) {
            m_advTree->expandItem(p);
        }
        m_advTree->setCurrentItem(restored);
        m_advTree->scrollToItem(restored, QAbstractItemView::PositionAtCenter);
    } else {
        updateAdvancedSelectionUi(QString(), QString());
    }
    refreshDatasetProperties(QStringLiteral("advanced"));
    updateTransferButtonsState();
}

void MainWindow::onOriginTreeSelectionChanged() {
    const auto selected = m_originTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("origin"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("origin"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onDestTreeSelectionChanged() {
    const auto selected = m_destTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("dest"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("dest"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onOriginTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    Q_UNUSED(item);
    Q_UNUSED(col);
}

void MainWindow::onDestTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    Q_UNUSED(item);
    Q_UNUSED(col);
}

void MainWindow::onOriginTreeContextMenuRequested(const QPoint& pos) {
    showDatasetContextMenu(QStringLiteral("origin"), m_originTree, pos);
}

void MainWindow::onDestTreeContextMenuRequested(const QPoint& pos) {
    showDatasetContextMenu(QStringLiteral("dest"), m_destTree, pos);
}

bool MainWindow::runSsh(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs, QString& out, QString& err, int& rc) {
    out.clear();
    err.clear();
    rc = -1;

    const bool hasPassword = !p.password.trimmed().isEmpty();
    QString program = QStringLiteral("ssh");
    QStringList args;
    bool usingSshpass = false;
    if (hasPassword) {
        const QString sshpassExe = QStandardPaths::findExecutable(QStringLiteral("sshpass"));
        if (!sshpassExe.isEmpty()) {
            program = sshpassExe;
            args << "-p" << p.password << "ssh";
            usingSshpass = true;
        }
    }

    args << "-o" << "BatchMode=yes";
    args << "-o" << "ConnectTimeout=10";
    args << "-o" << "LogLevel=ERROR";
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
    args << "-o" << "ControlMaster=auto";
    args << "-o" << "ControlPersist=300";
    args << "-o" << QStringLiteral("ControlPath=%1").arg(sshControlPath());
    if (hasPassword && usingSshpass) {
        args << "-o" << "BatchMode=no";
        args << "-o" << "PreferredAuthentications=password,keyboard-interactive,publickey";
        args << "-o" << "NumberOfPasswordPrompts=1";
    }
    if (p.port > 0) {
        args << "-p" << QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        args << "-i" << p.keyPath;
    }
    args << QStringLiteral("%1@%2").arg(p.username, p.host);
    const QString wrappedCmd = wrapRemoteCommand(p, remoteCmd);
    args << wrappedCmd;

    const QString cmdLine = QStringLiteral("%1@%2:%3 $ %4")
                                .arg(p.username, p.host)
                                .arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22"))
                                .arg(wrappedCmd);
    appLog(QStringLiteral("INFO"), cmdLine);
    appendConnectionLog(p.id, cmdLine);
    if (hasPassword && !usingSshpass) {
        appendConnectionLog(p.id, QStringLiteral("Password guardado, pero sshpass no está disponible; se usará SSH no interactivo."));
    }

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(4000)) {
        err = QStringLiteral("No se pudo iniciar %1").arg(program);
        appendConnectionLog(p.id, err);
        return false;
    }
    const bool finished = (timeoutMs <= 0) ? proc.waitForFinished(-1) : proc.waitForFinished(timeoutMs);
    if (!finished) {
        proc.kill();
        proc.waitForFinished(1000);
        err = QStringLiteral("Timeout");
        appendConnectionLog(p.id, err);
        return false;
    }

    rc = proc.exitCode();
    out = QString::fromUtf8(proc.readAllStandardOutput());
    err = QString::fromUtf8(proc.readAllStandardError());
    if (isWindowsConnection(p)) {
        out = sanitizeWindowsCliXml(out);
        err = sanitizeWindowsCliXml(err);
    }
    if (!out.trimmed().isEmpty()) {
        appendConnectionLog(p.id, oneLine(out));
    }
    if (!err.trimmed().isEmpty()) {
        appendConnectionLog(p.id, oneLine(err));
    }
    return true;
}

QString MainWindow::withSudo(const ConnectionProfile& p, const QString& cmd) const {
    if (isWindowsConnection(p)) {
        return cmd;
    }
    if (!p.useSudo) {
        return cmd;
    }
    if (!p.password.isEmpty()) {
        return QStringLiteral("printf '%s\\n' %1 | sudo -S -p '' sh -lc %2")
            .arg(shSingleQuote(p.password), shSingleQuote(cmd));
    }
    return QStringLiteral("sudo -n ") + cmd;
}

QString MainWindow::withSudoStreamInput(const ConnectionProfile& p, const QString& cmd) const {
    if (isWindowsConnection(p)) {
        return cmd;
    }
    if (!p.useSudo) {
        return cmd;
    }
    if (!p.password.isEmpty()) {
        return QStringLiteral("{ printf '%s\\n' %1; cat; } | sudo -S -p '' sh -lc %2")
            .arg(shSingleQuote(p.password), shSingleQuote(cmd));
    }
    return QStringLiteral("sudo -n sh -lc %1").arg(shSingleQuote(cmd));
}

bool MainWindow::isWindowsConnection(const ConnectionProfile& p) const {
    return p.osType.trimmed().toLower().contains(QStringLiteral("windows"));
}

bool MainWindow::isWindowsConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    return isWindowsConnection(m_profiles[connIdx]);
}

QString MainWindow::wrapRemoteCommand(const ConnectionProfile& p, const QString& remoteCmd) const {
    if (!isWindowsConnection(p)) {
        return remoteCmd;
    }
    const QString trimmed = remoteCmd.trimmed();
    const bool psLike = looksLikePowerShellScript(trimmed);
    const QString psEscaped = QString(trimmed).replace('\'', QStringLiteral("''"));
    QString script = QStringLiteral(
        "$zfsPaths=@("
        "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin',"
        "'C:\\\\Program Files\\\\OpenZFS On Windows',"
        "'C:\\\\msys64\\\\usr\\\\bin',"
        "'C:\\\\msys64\\\\mingw64\\\\bin',"
        "'C:\\\\msys64\\\\mingw32\\\\bin',"
        "'C:\\\\MinGW\\\\bin',"
        "'C:\\\\mingw64\\\\bin'"
        "); "
        "foreach($p in $zfsPaths){ "
        "  if(Test-Path -LiteralPath $p){ "
        "    if(-not (($env:Path -split ';') -contains $p)){ $env:Path = $p + ';' + $env:Path } "
        "  } "
        "}; ");
    if (psLike) {
        script += trimmed;
    } else {
        script += QStringLiteral(
            "$cmd='%1'; "
            "$unixShells=@("
            "'C:\\\\msys64\\\\usr\\\\bin\\\\bash.exe',"
            "'C:\\\\msys64\\\\usr\\\\bin\\\\sh.exe',"
            "'C:\\\\msys64\\\\mingw64\\\\bin\\\\bash.exe',"
            "'C:\\\\msys64\\\\mingw32\\\\bin\\\\bash.exe',"
            "'C:\\\\MinGW\\\\msys\\\\1.0\\\\bin\\\\sh.exe'"
            "); "
            "$shell=$null; foreach($s in $unixShells){ if(Test-Path -LiteralPath $s){ $shell=$s; break } }; "
            "if($shell){ & $shell -lc $cmd; exit $LASTEXITCODE } "
            "Invoke-Expression $cmd; exit $LASTEXITCODE;")
                      .arg(psEscaped);
    }
    const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
    const QString b64 = QString::fromLatin1(utf16.toBase64());
    // Windows command-line length can be hit with very large EncodedCommand payloads.
    // For long PowerShell-native scripts, fallback to -Command to avoid UTF-16+Base64 expansion.
    if (psLike && b64.size() > 7000) {
        QString inlineScript = script;
        inlineScript.replace(QStringLiteral("\""), QStringLiteral("`\""));
        return QStringLiteral("powershell -NoProfile -NonInteractive -Command \"& { %1 }\"")
            .arg(inlineScript);
    }
    return QStringLiteral("powershell -NoProfile -NonInteractive -EncodedCommand %1").arg(b64);
}

QString MainWindow::sshExecFromLocal(const ConnectionProfile& p, const QString& remoteCmd) const {
    const QString sshBase = sshBaseCommand(p);
    const QString target = shSingleQuote(p.username + QStringLiteral("@") + p.host);
    const QString wrapped = wrapRemoteCommand(p, remoteCmd);
    return sshBase + QStringLiteral(" ") + target + QStringLiteral(" ") + shSingleQuote(wrapped);
}

bool MainWindow::getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut) {
    valueOut.clear();
    if (connIdx < 0 || connIdx >= m_profiles.size() || dataset.isEmpty() || prop.isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd =
        QStringLiteral("zfs get -H -o value %1 %2").arg(shSingleQuote(prop), shSingleQuote(dataset));
    cmd = withSudo(p, cmd);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 15000, out, err, rc) || rc != 0) {
        return false;
    }
    valueOut = out.trimmed();
    return true;
}

QString MainWindow::effectiveMountPath(int connIdx,
                                       const QString& poolName,
                                       const QString& datasetName,
                                       const QString& mountpointHint,
                                       const QString& mountedValue) {
    if (!isWindowsConnection(connIdx)) {
        return mountpointHint.trimmed();
    }
    if (!isMountedValueTrue(mountedValue)) {
        return mountpointHint.trimmed();
    }
    if (poolName.isEmpty() || datasetName.isEmpty()) {
        return mountpointHint.trimmed();
    }
    if (!(datasetName == poolName || datasetName.startsWith(poolName + QStringLiteral("/")))) {
        return mountpointHint.trimmed();
    }

    QString anchor = datasetName;
    QString drive;
    while (!anchor.isEmpty()) {
        QString rawDrive;
        if (getDatasetProperty(connIdx, anchor, QStringLiteral("driveletter"), rawDrive)) {
            drive = normalizeDriveLetterValue(rawDrive);
        } else {
            drive.clear();
        }
        if (!drive.isEmpty()) {
            break;
        }
        if (anchor == poolName) {
            break;
        }
        const QString parent = parentDatasetName(anchor);
        if (parent.isEmpty()) {
            anchor.clear();
            break;
        }
        anchor = parent;
    }
    if (drive.isEmpty()) {
        return QString();
    }
    QString base = QStringLiteral("%1:\\").arg(drive);
    if (datasetName == anchor) {
        return base;
    }
    QString rel = datasetName.mid(anchor.size());
    if (rel.startsWith('/')) {
        rel.remove(0, 1);
    }
    rel.replace('/', '\\');
    return rel.isEmpty() ? base : (base + rel);
}

QString MainWindow::datasetCacheKey(int connIdx, const QString& poolName) const {
    return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
}

bool MainWindow::ensureDatasetsLoaded(int connIdx, const QString& poolName) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    PoolDatasetCache& cache = m_poolDatasetCache[key];
    if (cache.loaded) {
        return true;
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd = QStringLiteral(
        "zfs list -H -p -t filesystem,volume,snapshot "
        "-o name,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r %1")
                      .arg(poolName);
    cmd = withSudo(p, cmd);

    QString out;
    QString err;
    int rc = -1;
    appLog(QStringLiteral("INFO"), QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));
    if (!runSsh(p, cmd, 35000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("WARN"), QStringLiteral("Failed datasets %1::%2 -> %3")
                                        .arg(p.name, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        return false;
    }

    QMap<QString, QVector<QPair<QString, QString>>> snapshotMetaByDataset;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList f = line.split('\t');
        if (f.size() < 9) {
            continue;
        }
        const QString name = f[0].trimmed();
        if (name.isEmpty()) {
            continue;
        }
        DatasetRecord rec{name, f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]};
        if (name.contains('@')) {
            const QString ds = name.section('@', 0, 0);
            const QString snap = name.section('@', 1);
            snapshotMetaByDataset[ds].push_back(qMakePair(rec.creation, snap));
        } else {
            cache.datasets.push_back(rec);
            cache.recordByName[name] = rec;
        }
    }
    for (auto it = snapshotMetaByDataset.begin(); it != snapshotMetaByDataset.end(); ++it) {
        auto rows = it.value();
        std::sort(rows.begin(), rows.end(), [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
            bool aOk = false;
            bool bOk = false;
            const qlonglong av = a.first.toLongLong(&aOk);
            const qlonglong bv = b.first.toLongLong(&bOk);
            if (aOk && bOk && av != bv) {
                return av > bv; // más nuevo primero
            }
            if (a.first != b.first) {
                return a.first > b.first; // fallback textual desc
            }
            return a.second > b.second; // fallback por nombre desc
        });
        QStringList sortedSnaps;
        sortedSnaps.reserve(rows.size());
        for (const auto& row : rows) {
            sortedSnaps.push_back(row.second);
        }
        cache.snapshotsByDataset.insert(it.key(), sortedSnaps);
    }
    cache.driveletterByDataset.clear();
    if (isWindowsConnection(connIdx)) {
        QString dOut;
        QString dErr;
        int dRc = -1;
        const QString dCmd = withSudo(
            p,
            QStringLiteral("zfs get -H -o name,value -r driveletter %1").arg(shSingleQuote(poolName)));
        if (runSsh(p, dCmd, 20000, dOut, dErr, dRc) && dRc == 0) {
            QMap<QString, QStringList> byDrive;
            const QStringList dLines = dOut.split('\n', Qt::SkipEmptyParts);
            for (const QString& ln : dLines) {
                const QStringList f = ln.split('\t');
                if (f.size() < 2) {
                    continue;
                }
                const QString ds = f[0].trimmed();
                const QString drive = normalizeDriveLetterValue(f[1]);
                cache.driveletterByDataset[ds] = drive;
                if (!drive.isEmpty()) {
                    byDrive[drive].push_back(ds);
                }
            }
            for (auto it = byDrive.constBegin(); it != byDrive.constEnd(); ++it) {
                if (it.value().size() > 1) {
                    appLog(QStringLiteral("WARN"),
                           QStringLiteral("%1::%2 driveletter duplicado %3 en datasets: %4")
                               .arg(p.name, poolName, it.key(), it.value().join(QStringLiteral(", "))));
                }
            }
        } else if (!dErr.trimmed().isEmpty()) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: no se pudieron cargar driveletters -> %2").arg(p.name, oneLine(dErr)));
        }
    }
    cache.loaded = true;
    appLog(QStringLiteral("DEBUG"), QStringLiteral("Datasets loaded %1::%2 (%3)")
                                     .arg(p.name)
                                     .arg(poolName)
                                     .arg(cache.datasets.size()));
    return true;
}

void MainWindow::populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side) {
    beginUiBusy();
    m_loadingDatasetTrees = true;
    tree->clear();
    if (!ensureDatasetsLoaded(connIdx, poolName)) {
        m_loadingDatasetTrees = false;
        endUiBusy();
        return;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    const PoolDatasetCache& cache = m_poolDatasetCache[key];
    constexpr int snapshotListRole = Qt::UserRole + 1;

    QMap<QString, QTreeWidgetItem*> byName;
    for (const DatasetRecord& rec : cache.datasets) {
        auto* item = new QTreeWidgetItem();
        const QString displayName = rec.name.contains('/')
                                        ? rec.name.section('/', -1, -1)
                                        : rec.name;
        item->setText(0, displayName);
        const QStringList snaps = cache.snapshotsByDataset.value(rec.name);
        item->setText(1, snaps.isEmpty() ? QString() : QStringLiteral("(ninguno)"));
        item->setData(1, Qt::UserRole, QString());
        item->setData(0, Qt::UserRole, rec.name);
        item->setData(1, snapshotListRole, snaps);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const QString mounted = rec.mounted.trimmed().toLower();
        const bool isMounted = (mounted == QStringLiteral("yes")
                                || mounted == QStringLiteral("on")
                                || mounted == QStringLiteral("true")
                                || mounted == QStringLiteral("1"));
        item->setCheckState(2, isMounted ? Qt::Checked : Qt::Unchecked);
        const QString effectiveMp = effectiveMountPath(connIdx, poolName, rec.name, rec.mountpoint, rec.mounted);
        item->setText(3, effectiveMp.isEmpty() ? rec.mountpoint.trimmed() : effectiveMp);
        byName.insert(rec.name, item);
    }

    for (const DatasetRecord& rec : cache.datasets) {
        QTreeWidgetItem* item = byName.value(rec.name, nullptr);
        if (!item) {
            continue;
        }
        const QString parent = parentDatasetName(rec.name);
        QTreeWidgetItem* parentItem = byName.value(parent, nullptr);
        if (parentItem) {
            parentItem->addChild(item);
        } else {
            tree->addTopLevelItem(item);
        }
    }
    tree->expandToDepth(0);
    // Dropdown embebido en celda Snapshot, sin seleccionar ninguno al inicio.
    std::function<void(QTreeWidgetItem*)> attachCombos = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QStringList snaps = n->data(1, snapshotListRole).toStringList();
        if (!snaps.isEmpty()) {
            QStringList options;
            options << QStringLiteral("(ninguno)");
            options += snaps;
            auto* combo = new QComboBox(tree);
            combo->addItems(options);
            combo->setCurrentIndex(0);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
            combo->setMinimumHeight(22);
            combo->setMaximumHeight(22);
            combo->setFont(tree->font());
            combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 2px; margin:0px;}"));
            tree->setItemWidget(n, 1, combo);
            QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, n, side](const QString& txt) {
                onSnapshotComboChanged(tree, n, side, txt);
            });
        } else {
            tree->setItemWidget(n, 1, nullptr);
            n->setText(1, QString());
            n->setData(1, Qt::UserRole, QString());
        }
        for (int i = 0; i < n->childCount(); ++i) {
            attachCombos(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        attachCombos(tree->topLevelItem(i));
    }

    if (side == QStringLiteral("origin")) {
        m_originSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
    } else if (side == QStringLiteral("dest")) {
        m_destSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
    }
    m_loadingDatasetTrees = false;
    endUiBusy();
}

void MainWindow::clearOtherSnapshotSelections(QTreeWidget* tree, QTreeWidgetItem* keepItem) {
    std::function<void(QTreeWidgetItem*)> clearRec = [&](QTreeWidgetItem* n) {
        if (!n || n == keepItem) {
            return;
        }
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(n, 1))) {
            QSignalBlocker b(cb);
            cb->setCurrentIndex(0);
        }
        n->setData(1, Qt::UserRole, QString());
        for (int i = 0; i < n->childCount(); ++i) {
            clearRec(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        clearRec(tree->topLevelItem(i));
    }
}

void MainWindow::onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, const QString& side, const QString& chosen) {
    if (!tree || !item) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString();
    const QString snap = (chosen == QStringLiteral("(ninguno)")) ? QString() : chosen.trimmed();
    if (!snap.isEmpty()) {
        clearOtherSnapshotSelections(tree, item);
    }
    item->setData(1, Qt::UserRole, snap);
    tree->setCurrentItem(item);
    if (side == QStringLiteral("advanced")) {
        updateAdvancedSelectionUi(ds, snap);
        refreshDatasetProperties(QStringLiteral("advanced"));
        updateTransferButtonsState();
        return;
    }
    setSelectedDataset(side, ds, snap);
}

void MainWindow::onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, const QString& side) {
    if (!tree || !item || m_loadingDatasetTrees || actionsLocked()) {
        return;
    }
    if (col != 2) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const Qt::CheckState desired = item->checkState(2);
    QString token;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();
    } else {
        token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        m_loadingDatasetTrees = true;
        item->setCheckState(2, desired == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        m_loadingDatasetTrees = false;
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = token.left(sep).toInt();
    ctx.poolName = token.mid(sep + 2);
    ctx.datasetName = ds;
    ctx.snapshotName = item->data(1, Qt::UserRole).toString();
    if (!ctx.snapshotName.isEmpty()) {
        m_loadingDatasetTrees = true;
        item->setCheckState(2, desired == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        m_loadingDatasetTrees = false;
        return;
    }

    bool ok = false;
    m_loadingDatasetTrees = true;
    if (desired == Qt::Checked) {
        ok = mountDataset(side, ctx);
    } else {
        ok = umountDataset(side, ctx);
    }
    m_loadingDatasetTrees = false;
    if (!ok) {
        auto findByDataset = [&](auto&& self, QTreeWidgetItem* n, const QString& name) -> QTreeWidgetItem* {
            if (!n) {
                return nullptr;
            }
            if (n->data(0, Qt::UserRole).toString() == name) {
                return n;
            }
            for (int i = 0; i < n->childCount(); ++i) {
                if (QTreeWidgetItem* f = self(self, n->child(i), name)) {
                    return f;
                }
            }
            return nullptr;
        };
        QTreeWidgetItem* safeItem = nullptr;
        for (int i = 0; i < tree->topLevelItemCount() && !safeItem; ++i) {
            safeItem = findByDataset(findByDataset, tree->topLevelItem(i), ds);
        }
        if (safeItem) {
            m_loadingDatasetTrees = true;
            safeItem->setCheckState(2, desired == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            m_loadingDatasetTrees = false;
        }
    }
}

void MainWindow::refreshDatasetProperties(const QString& side) {
    beginUiBusy();
    QString dataset;
    QString snapshot;
    if (side == QStringLiteral("origin")) {
        dataset = m_originSelectedDataset;
        snapshot = m_originSelectedSnapshot;
    } else if (side == QStringLiteral("dest")) {
        dataset = m_destSelectedDataset;
        snapshot = m_destSelectedSnapshot;
    } else {
        const auto selected = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
        if (!selected.isEmpty()) {
            dataset = selected.first()->data(0, Qt::UserRole).toString();
            snapshot = selected.first()->data(1, Qt::UserRole).toString();
        }
    }
    QTableWidget* table = (side == QStringLiteral("advanced")) ? m_advPropsTable : m_datasetPropsTable;
    if (!table) {
        endUiBusy();
        return;
    }
    if (dataset.isEmpty()) {
        setTablePopulationMode(table, true);
        table->setRowCount(0);
        setTablePopulationMode(table, false);
        if (side == QStringLiteral("advanced")) {
            m_advPropsDataset.clear();
            m_advPropsOriginalValues.clear();
            m_advPropsOriginalInherit.clear();
            m_advPropsDirty = false;
        } else {
            m_propsDataset.clear();
            m_propsSide = side;
            m_propsOriginalValues.clear();
            m_propsOriginalInherit.clear();
            m_propsDirty = false;
        }
        updateApplyPropsButtonState();
        endUiBusy();
        return;
    }

    QString token;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo->currentData().toString();
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo->currentData().toString();
    } else {
        token = m_advPoolCombo->currentData().toString();
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        endUiBusy();
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const QString key = datasetCacheKey(connIdx, poolName);
    const auto it = m_poolDatasetCache.constFind(key);
    if (it == m_poolDatasetCache.constEnd()) {
        endUiBusy();
        return;
    }
    const PoolDatasetCache& cache = it.value();
    const auto recIt = cache.recordByName.constFind(dataset);
    if (recIt == cache.recordByName.constEnd()) {
        endUiBusy();
        return;
    }
    const DatasetRecord& rec = recIt.value();
    const QString objectName = snapshot.isEmpty() ? dataset : QStringLiteral("%1@%2").arg(dataset, snapshot);
    const ConnectionProfile& p = m_profiles[connIdx];

    QString datasetType = objectName.contains('@') ? QStringLiteral("snapshot") : QStringLiteral("filesystem");
    {
        QString tOut, tErr;
        int tRc = -1;
        const QString typeCmd = withSudo(
            p,
            QStringLiteral("zfs get -H -o value type %1").arg(shSingleQuote(objectName)));
        if (runSsh(p, typeCmd, 12000, tOut, tErr, tRc) && tRc == 0) {
            const QString t = tOut.trimmed().toLower();
            if (!t.isEmpty()) {
                datasetType = t;
            }
        }
    }

    struct PropRow {
        QString prop;
        QString value;
        QString source;
        QString readonly;
    };
    QVector<PropRow> rawRows;
    rawRows.push_back({QStringLiteral("dataset"), objectName, QString(), QStringLiteral("true")});

    QString out;
    QString err;
    int rc = -1;
    QString propsCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o property,value,source,readonly all %1").arg(shSingleQuote(objectName)));
    if (!runSsh(p, propsCmd, 20000, out, err, rc) || rc != 0) {
        propsCmd = withSudo(
            p,
            QStringLiteral("zfs get -H -o property,value,source all %1").arg(shSingleQuote(objectName)));
        out.clear();
        err.clear();
        rc = -1;
        runSsh(p, propsCmd, 20000, out, err, rc);
    }
    if (rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            QString prop, val, source, ro;
            const QStringList parts = raw.split('\t');
            if (parts.size() >= 4) {
                prop = parts[0].trimmed();
                val = parts[1].trimmed();
                source = parts[2].trimmed();
                ro = parts[3].trimmed().toLower();
            } else if (parts.size() >= 3) {
                prop = parts[0].trimmed();
                val = parts[1].trimmed();
                source = parts[2].trimmed();
                ro.clear();
            } else {
                const QStringList sp = raw.simplified().split(' ');
                if (sp.size() < 3) {
                    continue;
                }
                prop = sp[0].trimmed();
                val = sp[1].trimmed();
                source = sp[2].trimmed();
                ro = (sp.size() > 3) ? sp[3].trimmed().toLower() : QString();
            }
            if (!isDatasetPropertyEditable(prop, datasetType, source, ro)) {
                continue;
            }
            rawRows.push_back({prop, val, source, ro});
        }
    }

    QMap<QString, PropRow> byProp;
    for (const PropRow& row : rawRows) {
        byProp[row.prop] = row;
    }
    // Orden solicitado:
    // Unix/macOS: dataset, mountpoint, canmount, estado, Tamaño y luego resto.
    // Windows: dataset, mountpoint, canmount, estado, Tamaño, driveletter y luego resto.
    QVector<PropRow> rows;
    rows.reserve(byProp.size() + 2);
    const bool windowsConn = isWindowsConnection(connIdx);
    if (byProp.contains(QStringLiteral("dataset"))) {
        rows.push_back(byProp.take(QStringLiteral("dataset")));
    }
    if (snapshot.isEmpty()) {
        if (byProp.contains(QStringLiteral("mountpoint"))) {
            rows.push_back(byProp.take(QStringLiteral("mountpoint")));
        } else {
            rows.push_back({QStringLiteral("mountpoint"), rec.mountpoint.trimmed(), QString(), QStringLiteral("true")});
        }
        if (byProp.contains(QStringLiteral("canmount"))) {
            rows.push_back(byProp.take(QStringLiteral("canmount")));
        } else {
            rows.push_back({QStringLiteral("canmount"), rec.canmount.trimmed(), QString(), QStringLiteral("true")});
        }
        const QString mountedRaw = rec.mounted.trimmed().toLower();
        const bool mountedYes = (mountedRaw == QStringLiteral("yes")
                                 || mountedRaw == QStringLiteral("on")
                                 || mountedRaw == QStringLiteral("true")
                                 || mountedRaw == QStringLiteral("1"));
        rows.push_back({QStringLiteral("estado"), mountedYes ? QStringLiteral("Montado") : QStringLiteral("Desmontado"), QString(), QStringLiteral("true")});
        rows.push_back({QStringLiteral("Tamaño"), formatDatasetSize(rec.used.trimmed()), QString(), QStringLiteral("true")});
        if (windowsConn) {
            if (byProp.contains(QStringLiteral("driveletter"))) {
                rows.push_back(byProp.take(QStringLiteral("driveletter")));
            } else {
                rows.push_back({QStringLiteral("driveletter"), QString(), QString(), QStringLiteral("true")});
            }
        }
    } else {
        rows.push_back({QStringLiteral("estado"), QStringLiteral("Snapshot"), QString(), QStringLiteral("true")});
    }
    const QStringList remainingProps = byProp.keys();
    for (const QString& prop : remainingProps) {
        rows.push_back(byProp.value(prop));
    }

    m_loadingPropsTable = true;
    setTablePopulationMode(table, true);
    table->setRowCount(0);
    if (side == QStringLiteral("advanced")) {
        m_advPropsOriginalValues.clear();
        m_advPropsOriginalInherit.clear();
        m_advPropsDataset = objectName;
    } else {
        m_propsOriginalValues.clear();
        m_propsOriginalInherit.clear();
        m_propsSide = side;
        m_propsDataset = objectName;
    }
    const QSet<QString> inheritableProps = {QStringLiteral("mountpoint"), QStringLiteral("canmount")};
    const QMap<QString, QStringList> enumValues = {
        {QStringLiteral("atime"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("readonly"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("compression"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("lz4"), QStringLiteral("zstd"), QStringLiteral("gzip"), QStringLiteral("zle"), QStringLiteral("lzjb")}},
        {QStringLiteral("checksum"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("fletcher2"), QStringLiteral("fletcher4"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr"), QStringLiteral("blake3")}},
        {QStringLiteral("sync"), {QStringLiteral("standard"), QStringLiteral("always"), QStringLiteral("disabled")}},
        {QStringLiteral("logbias"), {QStringLiteral("latency"), QStringLiteral("throughput")}},
        {QStringLiteral("primarycache"), {QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("secondarycache"), {QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("dedup"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("verify"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr"), QStringLiteral("blake3")}},
        {QStringLiteral("copies"), {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}},
        {QStringLiteral("acltype"), {QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), {QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("xattr"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa"), QStringLiteral("dir")}},
        {QStringLiteral("normalization"), {QStringLiteral("none"), QStringLiteral("formC"), QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")}},
        {QStringLiteral("casesensitivity"), {QStringLiteral("sensitive"), QStringLiteral("insensitive"), QStringLiteral("mixed")}},
        {QStringLiteral("utf8only"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("canmount"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("noauto")}},
        {QStringLiteral("snapdir"), {QStringLiteral("hidden"), QStringLiteral("visible")}},
        {QStringLiteral("exec"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("setuid"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("devices"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("snapdev"), {QStringLiteral("hidden"), QStringLiteral("visible")}},
        {QStringLiteral("volmode"), {QStringLiteral("default"), QStringLiteral("full"), QStringLiteral("dev"), QStringLiteral("none"), QStringLiteral("geom")}},
    };
    const int pinnedCount = (!snapshot.isEmpty() ? 2 : (windowsConn ? 6 : 5));
    table->setProperty("pinned_rows", pinnedCount);
    for (const PropRow& row : rows) {
        const int r = table->rowCount();
        table->insertRow(r);
        auto* k = new PinnedSortItem(row.prop);
        k->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        table->setItem(r, 0, k);
        auto* v = new PinnedSortItem(row.value);
        v->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        if (row.prop == QStringLiteral("dataset") || row.prop == QStringLiteral("estado") || row.prop == QStringLiteral("Tamaño")) {
            v->setFlags(v->flags() & ~Qt::ItemIsEditable);
        }
        table->setItem(r, 1, v);
        const QString propLower = row.prop.trimmed().toLower();
        const auto enumIt = enumValues.constFind(propLower);
        if ((v->flags() & Qt::ItemIsEditable) && enumIt != enumValues.constEnd()) {
            auto* combo = new NoWheelComboBox(table);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
            QStringList options = enumIt.value();
            const QString current = row.value.trimmed();
            if (!current.isEmpty() && !options.contains(current)) {
                options.prepend(current);
            }
            combo->addItems(options);
            if (!current.isEmpty()) {
                combo->setCurrentText(current);
            }
            table->setCellWidget(r, 1, combo);
            QObject::connect(combo, &QComboBox::currentTextChanged, table, [this, table, combo](const QString& txt) {
                for (int rr = 0; rr < table->rowCount(); ++rr) {
                    if (table->cellWidget(rr, 1) != combo) {
                        continue;
                    }
                    if (QTableWidgetItem* item = table->item(rr, 1)) {
                        item->setText(txt);
                    }
                    if (table == m_advPropsTable) {
                        onAdvancedPropsCellChanged(rr, 1);
                    } else {
                        onDatasetPropsCellChanged(rr, 1);
                    }
                    break;
                }
            });
        }
        auto* inh = new PinnedSortItem();
        inh->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        if (inheritableProps.contains(row.prop)) {
            inh->setFlags((inh->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) & ~Qt::ItemIsEditable);
            inh->setCheckState(Qt::Unchecked);
        } else {
            inh->setFlags(Qt::ItemIsEnabled);
            inh->setText(QStringLiteral("-"));
        }
        table->setItem(r, 2, inh);
        if (r < pinnedCount) {
            QFont f0 = k->font();
            f0.setBold(true);
            k->setFont(f0);
            QFont f1 = v->font();
            f1.setBold(true);
            v->setFont(f1);
            QFont f2 = inh->font();
            f2.setBold(true);
            inh->setFont(f2);
            if (QComboBox* cb = qobject_cast<QComboBox*>(table->cellWidget(r, 1))) {
                QFont cf = cb->font();
                cf.setBold(true);
                cb->setFont(cf);
            }
        }
        if (side == QStringLiteral("advanced")) {
            m_advPropsOriginalValues[row.prop] = row.value;
            m_advPropsOriginalInherit[row.prop] = false;
        } else {
            m_propsOriginalValues[row.prop] = row.value;
            m_propsOriginalInherit[row.prop] = false;
        }
    }
    if (side == QStringLiteral("advanced")) {
        m_advPropsDirty = false;
    } else {
        m_propsDirty = false;
    }
    setTablePopulationMode(table, false);
    m_loadingPropsTable = false;
    updateApplyPropsButtonState();
    endUiBusy();
}

void MainWindow::setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName) {
    if (side == QStringLiteral("origin")) {
        m_originSelectedDataset = datasetName;
        m_originSelectedSnapshot = snapshotName;
        if (datasetName.isEmpty()) {
            m_originSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        } else if (snapshotName.isEmpty()) {
            m_originSelectionLabel->setText(datasetName);
        } else {
            m_originSelectionLabel->setText(QStringLiteral("%1@%2").arg(datasetName, snapshotName));
        }
        refreshDatasetProperties(QStringLiteral("origin"));
        refreshTransferSelectionLabels();
        updateTransferButtonsState();
        return;
    }
    m_destSelectedDataset = datasetName;
    m_destSelectedSnapshot = snapshotName;
    if (datasetName.isEmpty()) {
        m_destSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
    } else if (snapshotName.isEmpty()) {
        m_destSelectionLabel->setText(datasetName);
    } else {
        m_destSelectionLabel->setText(QStringLiteral("%1@%2").arg(datasetName, snapshotName));
    }
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::refreshTransferSelectionLabels() {
    QString originText;
    if (!m_originSelectedDataset.isEmpty()) {
        if (!m_originSelectedSnapshot.isEmpty()) {
            originText = QStringLiteral("%1@%2").arg(m_originSelectedDataset, m_originSelectedSnapshot);
        } else {
            originText = m_originSelectedDataset;
        }
    } else {
        originText = tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）"));
    }
    if (m_transferOriginLabel) {
        m_transferOriginLabel->setText(originText);
    }
    if (m_originSelectionLabel) {
        m_originSelectionLabel->setText(originText);
    }

    QString destText;
    if (!m_destSelectedDataset.isEmpty()) {
        if (!m_destSelectedSnapshot.isEmpty()) {
            destText = QStringLiteral("%1@%2").arg(m_destSelectedDataset, m_destSelectedSnapshot);
        } else {
            destText = m_destSelectedDataset;
        }
    } else {
        destText = tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）"));
    }
    if (m_transferDestLabel) {
        m_transferDestLabel->setText(destText);
    }
    if (m_destSelectionLabel) {
        m_destSelectionLabel->setText(destText);
    }

    if (m_transferBox) {
        const QString emptyToken = tr3(QStringLiteral("[vacío]"),
                                       QStringLiteral("[empty]"),
                                       QStringLiteral("[空]"));
        const QString originTitle = m_originSelectedDataset.isEmpty() ? emptyToken : originText;
        const QString destTitle = m_destSelectedDataset.isEmpty() ? emptyToken : destText;
        m_transferBox->setTitle(QStringLiteral("%1-->%2").arg(originTitle, destTitle));
    }
}

void MainWindow::updateAdvancedSelectionUi(const QString& datasetName, const QString& snapshotName) {
    QString text;
    if (datasetName.isEmpty()) {
        text = tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）"));
    } else if (snapshotName.isEmpty()) {
        text = datasetName;
    } else {
        text = QStringLiteral("%1@%2").arg(datasetName, snapshotName);
    }
    if (m_advSelectionLabel) {
        m_advSelectionLabel->setText(text);
    }
    if (m_advCommandsBox) {
        const QString emptyTitle = tr3(QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]"));
        m_advCommandsBox->setTitle(datasetName.isEmpty() ? emptyTitle : text);
    }
}

void MainWindow::updateTransferButtonsState() {
    if (actionsLocked()) {
        if (m_btnCopy) m_btnCopy->setEnabled(false);
        if (m_btnLevel) m_btnLevel->setEnabled(false);
        if (m_btnSync) m_btnSync->setEnabled(false);
        if (m_btnAdvancedBreakdown) m_btnAdvancedBreakdown->setEnabled(false);
        if (m_btnAdvancedAssemble) m_btnAdvancedAssemble->setEnabled(false);
        if (m_btnAdvancedFromDir) m_btnAdvancedFromDir->setEnabled(false);
        if (m_btnAdvancedToDir) m_btnAdvancedToDir->setEnabled(false);
        return;
    }
    const bool srcDs = !m_originSelectedDataset.isEmpty();
    const bool srcSnap = !m_originSelectedSnapshot.isEmpty();
    const bool dstDs = !m_destSelectedDataset.isEmpty();
    const bool dstSnap = !m_destSelectedSnapshot.isEmpty();
    const QString srcSel = srcDs ? (srcSnap ? QStringLiteral("%1@%2").arg(m_originSelectedDataset, m_originSelectedSnapshot)
                                            : m_originSelectedDataset)
                                 : QString();
    const QString dstSel = dstDs ? (dstSnap ? QStringLiteral("%1@%2").arg(m_destSelectedDataset, m_destSelectedSnapshot)
                                            : m_destSelectedDataset)
                                 : QString();
    const bool sameSelection = !srcSel.isEmpty() && (srcSel == dstSel);
    const DatasetSelectionContext srcCtx = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dstCtx = currentDatasetSelection(QStringLiteral("dest"));
    const bool srcSelectionConsistent = srcCtx.valid
        && srcCtx.datasetName == m_originSelectedDataset
        && srcCtx.snapshotName == m_originSelectedSnapshot;
    const bool dstSelectionConsistent = dstCtx.valid
        && dstCtx.datasetName == m_destSelectedDataset
        && dstCtx.snapshotName == m_destSelectedSnapshot;
    auto datasetMountedInCache = [this](const DatasetSelectionContext& c) -> bool {
        if (!c.valid || c.datasetName.isEmpty()) {
            return false;
        }
        const QString key = datasetCacheKey(c.connIdx, c.poolName);
        const auto it = m_poolDatasetCache.constFind(key);
        if (it == m_poolDatasetCache.constEnd() || !it->loaded) {
            return false;
        }
        const auto recIt = it->recordByName.constFind(c.datasetName);
        if (recIt == it->recordByName.constEnd()) {
            return false;
        }
        return isMountedValueTrue(recIt->mounted);
    };
    const bool syncReady = srcDs && !srcSnap && dstDs && !dstSnap && !sameSelection
        && srcSelectionConsistent && dstSelectionConsistent
        && datasetMountedInCache(srcCtx) && datasetMountedInCache(dstCtx);
    m_btnCopy->setEnabled(srcDs && srcSnap && dstDs && !dstSnap);
    m_btnLevel->setEnabled(srcDs && dstDs && !dstSnap && !sameSelection);
    m_btnSync->setEnabled(syncReady);
    const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("advanced"));
    bool advDatasetOnly = actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty();
    if (advDatasetOnly) {
        const QString key = datasetCacheKey(actx.connIdx, actx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
            advDatasetOnly = false;
        } else {
            bool allMounted = true;
            const QString base = actx.datasetName;
            const QString pref = base + QStringLiteral("/");
            for (auto it = cacheIt->recordByName.constBegin(); it != cacheIt->recordByName.constEnd(); ++it) {
                const QString& ds = it.key();
                if (ds != base && !ds.startsWith(pref)) {
                    continue;
                }
                if (!isMountedValueTrue(it.value().mounted)) {
                    allMounted = false;
                    break;
                }
            }
            advDatasetOnly = allMounted;
        }
    }
    if (m_btnAdvancedBreakdown) {
        m_btnAdvancedBreakdown->setEnabled(advDatasetOnly);
    }
    if (m_btnAdvancedAssemble) {
        m_btnAdvancedAssemble->setEnabled(advDatasetOnly);
    }
    if (m_btnAdvancedFromDir) {
        m_btnAdvancedFromDir->setEnabled(actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty());
    }
    if (m_btnAdvancedToDir) {
        m_btnAdvancedToDir->setEnabled(actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty());
    }
}

bool MainWindow::runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs, bool forceConfirmDialog, bool streamProgress) {
    if (!confirmActionExecution(displayLabel, {QStringLiteral("[local]\n%1").arg(command)}, forceConfirmDialog)) {
        return false;
    }
    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1").arg(displayLabel));
    appLog(QStringLiteral("INFO"), QStringLiteral("$ %1").arg(command));
    QProcess proc;
    m_cancelActionRequested = false;
    m_activeLocalProcess = &proc;
    proc.start(QStringLiteral("sh"), QStringList{QStringLiteral("-lc"), command});
    if (!proc.waitForStarted(4000)) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("No se pudo iniciar comando local"));
        m_activeLocalProcess = nullptr;
        m_activeLocalPid = -1;
        setActionsLocked(false);
        return false;
    }
    m_activeLocalPid = static_cast<qint64>(proc.processId());
    QString outBuf;
    QString errBuf;
    QString outRemainder;
    QString errRemainder;
    QElapsedTimer progressTimer;
    progressTimer.start();
    QElapsedTimer heartbeatTimer;
    heartbeatTimer.start();
    int lastProgressPercent = -1;
    QString lastProgressSnippet;
    auto flushLines = [&](QString& remainder, const QString& chunk, const QString& level, bool progressAware) {
        if (chunk.isEmpty()) {
            return;
        }
        QString data = remainder + chunk;
        data.replace('\r', '\n');
        const QStringList parts = data.split('\n');
        if (!data.endsWith('\n')) {
            remainder = parts.isEmpty() ? data : parts.last();
        } else {
            remainder.clear();
        }
        const int limit = data.endsWith('\n') ? parts.size() : qMax(0, parts.size() - 1);
        for (int i = 0; i < limit; ++i) {
            const QString ln = parts[i].trimmed();
            if (ln.isEmpty()) {
                continue;
            }
            if (progressAware) {
                const QString low = ln.toLower();
                const bool looksLikeProgress = ln.contains('%')
                    || low.contains(QStringLiteral("ib/s"))
                    || low.contains(QStringLiteral("b/s"))
                    || low.contains(QStringLiteral("to-chk"))
                    || low.contains(QStringLiteral("xfr#"));
                if (looksLikeProgress) {
                    const QRegularExpression pctRx(QStringLiteral("(\\d{1,3})%"));
                    const QRegularExpressionMatch pctM = pctRx.match(ln);
                    if (pctM.hasMatch()) {
                        bool okPct = false;
                        const int pct = pctM.captured(1).toInt(&okPct);
                        if (okPct && pct >= 0 && pct <= 100) {
                            if (lastProgressPercent >= 0 && pct <= lastProgressPercent) {
                                continue;
                            }
                            if (lastProgressPercent >= 0 && (pct - lastProgressPercent) < 1) {
                                continue;
                            }
                            lastProgressPercent = pct;
                            appLog(QStringLiteral("INFO"), QStringLiteral("[progress] %1").arg(ln));
                            continue;
                        }
                    }
                    if (progressTimer.elapsed() < 700) {
                        continue;
                    }
                    progressTimer.restart();
                    lastProgressSnippet = ln;
                    appLog(QStringLiteral("INFO"), QStringLiteral("[progress] %1").arg(ln));
                    continue;
                }
            }
            appLog(level, oneLine(ln));
        }
        if (progressAware) {
            const QString partial = remainder.trimmed();
            if (!partial.isEmpty()) {
                const QString low = partial.toLower();
                const bool looksLikeProgress = partial.contains('%')
                    || low.contains(QStringLiteral("ib/s"))
                    || low.contains(QStringLiteral("b/s"))
                    || low.contains(QStringLiteral("to-chk"))
                    || low.contains(QStringLiteral("xfr#"));
                if (looksLikeProgress && progressTimer.elapsed() >= 900 && partial != lastProgressSnippet) {
                    progressTimer.restart();
                    lastProgressSnippet = partial;
                    appLog(QStringLiteral("INFO"), QStringLiteral("[progress] %1").arg(partial));
                }
            }
        }
    };

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (m_cancelActionRequested) {
            appLog(QStringLiteral("NORMAL"), tr3(QStringLiteral("Cancelando acción en curso..."),
                                                 QStringLiteral("Canceling running action..."),
                                                 QStringLiteral("正在取消执行中的操作...")));
            terminateProcessTree(m_activeLocalPid);
            proc.terminate();
            if (!proc.waitForFinished(800)) {
                proc.kill();
                proc.waitForFinished(800);
            }
            appLog(QStringLiteral("NORMAL"), tr3(QStringLiteral("Acción cancelada por el usuario."),
                                                 QStringLiteral("Action canceled by user."),
                                                 QStringLiteral("操作已被用户取消。")));
            m_activeLocalProcess = nullptr;
            m_activeLocalPid = -1;
            m_cancelActionRequested = false;
            setActionsLocked(false);
            return false;
        }
        if (timeoutMs > 0 && (QDateTime::currentMSecsSinceEpoch() - startMs) > timeoutMs) {
            terminateProcessTree(m_activeLocalPid);
            proc.kill();
            proc.waitForFinished(1000);
            appLog(QStringLiteral("NORMAL"), QStringLiteral("Timeout en comando local"));
            m_activeLocalProcess = nullptr;
            m_activeLocalPid = -1;
            setActionsLocked(false);
            return false;
        }
        proc.waitForReadyRead(200);
        const QString outChunk = QString::fromUtf8(proc.readAllStandardOutput());
        const QString errChunk = QString::fromUtf8(proc.readAllStandardError());
        outBuf += outChunk;
        errBuf += errChunk;
        if (streamProgress) {
            flushLines(outRemainder, outChunk, QStringLiteral("INFO"), true);
            flushLines(errRemainder, errChunk, QStringLiteral("INFO"), true);
            if (heartbeatTimer.elapsed() >= 2000) {
                heartbeatTimer.restart();
                appLog(QStringLiteral("INFO"), QStringLiteral("[progress] running..."));
            }
        }
    }
    if (streamProgress) {
        flushLines(outRemainder, QStringLiteral("\n"), QStringLiteral("INFO"), true);
        flushLines(errRemainder, QStringLiteral("\n"), QStringLiteral("INFO"), true);
    }

    const int rc = proc.exitCode();
    const QString out = outBuf.trimmed();
    const QString err = errBuf.trimmed();
    if (!out.isEmpty() && !streamProgress) {
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    if (!err.isEmpty() && !streamProgress) {
        appLog(QStringLiteral("INFO"), oneLine(err));
    }
    if (rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizó con error %1").arg(rc));
        m_activeLocalProcess = nullptr;
        m_activeLocalPid = -1;
        setActionsLocked(false);
        return false;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizado correctamente"));
    m_activeLocalProcess = nullptr;
    m_activeLocalPid = -1;
    setActionsLocked(false);
    return true;
}

void MainWindow::actionCopySnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const bool sameConnection = (src.connIdx == dst.connIdx);
    const QString srcSnap = src.datasetName + QStringLiteral("@") + src.snapshotName;
    // Con send -R, el usuario espera que el dataset origen cuelgue del dataset destino
    // seleccionado (como padre), salvo cuando ya seleccionó explícitamente ese mismo dataset.
    const QString srcLeaf = src.datasetName.section('/', -1);
    QString recvTarget = dst.datasetName;
    if (!srcLeaf.isEmpty()) {
        const QString dstLeaf = dst.datasetName.section('/', -1);
        if (!dst.datasetName.endsWith(QStringLiteral("/") + srcLeaf) && dstLeaf != srcLeaf) {
            recvTarget = dst.datasetName + QStringLiteral("/") + srcLeaf;
        }
    }

    const QString srcSsh = sshBaseCommand(sp) + QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    const QString dstSsh = sshBaseCommand(dp) + QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    const QString sendRawCmd = QStringLiteral("zfs send -wLecR %1").arg(shSingleQuote(srcSnap));
    const QString recvRawCmd = QStringLiteral("zfs recv -Fus %1").arg(shSingleQuote(recvTarget));
    QString sendCmd = withSudo(sp, sendRawCmd);
    QString recvCmd = withSudoStreamInput(dp, recvRawCmd);

    QString pipeline;
    if (sameConnection) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Copiar: modo local remoto (origen y destino en la misma conexión)"));
        // Important: run full pipeline under one sudo context so password never contaminates ZFS stream.
        const QString remotePipe = withSudo(sp, sendRawCmd
            + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
            + recvRawCmd);
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else {
        pipeline =
            srcSsh + QStringLiteral(" ") + shSingleQuote(sendCmd)
            + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
            + dstSsh + QStringLiteral(" ") + shSingleQuote(recvCmd);
    }

    if (runLocalCommand(QStringLiteral("Copiar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0, false, true)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionLevelSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty() || !dst.snapshotName.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Nivelar omitido: selección incompleta (src.valid=%1 dst.valid=%2 src.dataset=%3 dst.dataset=%4 dst.snap=%5)")
                                      .arg(src.valid ? QStringLiteral("1") : QStringLiteral("0"))
                                      .arg(dst.valid ? QStringLiteral("1") : QStringLiteral("0"))
                                      .arg(src.datasetName.isEmpty() ? QStringLiteral("0") : QStringLiteral("1"))
                                      .arg(dst.datasetName.isEmpty() ? QStringLiteral("0") : QStringLiteral("1"))
                                      .arg(dst.snapshotName.isEmpty() ? QStringLiteral("0") : QStringLiteral("1")));
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Para Nivelar debe seleccionar:\n"
                               "- En Origen: un dataset o snapshot\n"
                               "- En Destino: un dataset (sin snapshot)"),
                QStringLiteral("To Level you must select:\n"
                               "- Source: a dataset or snapshot\n"
                               "- Target: a dataset (without snapshot)"),
                QStringLiteral("执行“同步快照”前请先选择：\n"
                               "- 源：数据集或快照\n"
                               "- 目标：数据集（不带快照）")));
        return;
    }
    if (!ensureDatasetsLoaded(src.connIdx, src.poolName) || !ensureDatasetsLoaded(dst.connIdx, dst.poolName)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudieron cargar snapshots para Nivelar."),
                                 QStringLiteral("Could not load snapshots for Level."),
                                 QStringLiteral("无法加载用于同步快照的快照列表。")));
        return;
    }
    const QString srcKey = datasetCacheKey(src.connIdx, src.poolName);
    const QString dstKey = datasetCacheKey(dst.connIdx, dst.poolName);
    const QStringList srcSnaps = m_poolDatasetCache.value(srcKey).snapshotsByDataset.value(src.datasetName);
    const QStringList dstSnaps = m_poolDatasetCache.value(dstKey).snapshotsByDataset.value(dst.datasetName);
    if (srcSnaps.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Origen no tiene snapshots para Nivelar."),
                                     QStringLiteral("Source has no snapshots to Level."),
                                     QStringLiteral("源数据集没有可用于同步的快照。")));
        return;
    }
    if (dstSnaps.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("Destino no tiene snapshots. Nivelar siempre se hace con diferencial desde el snapshot más reciente de destino."),
                                 QStringLiteral("Target has no snapshots. Level always uses a differential send from target latest snapshot."),
                                 QStringLiteral("目标数据集没有快照。“同步快照”始终要求从目标最新快照开始做增量发送。")));
        return;
    }

    const QString targetSnapName = src.snapshotName.isEmpty() ? srcSnaps.first() : src.snapshotName;
    const int targetIdxInSrc = srcSnaps.indexOf(targetSnapName);
    if (targetIdxInSrc < 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("El snapshot objetivo (%1) no existe en origen.").arg(targetSnapName),
                                 QStringLiteral("Target snapshot (%1) does not exist in source.").arg(targetSnapName),
                                 QStringLiteral("目标快照（%1）在源端不存在。").arg(targetSnapName)));
        return;
    }

    const QString dstLatestSnap = dstSnaps.first();
    const int baseIdxInSrc = srcSnaps.indexOf(dstLatestSnap);
    if (baseIdxInSrc < 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("El snapshot más reciente de destino (%1) no existe en origen.\nNo se puede nivelar con diferencial.")
                                     .arg(dstLatestSnap),
                                 QStringLiteral("Target latest snapshot (%1) does not exist in source.\nCannot level with differential send.")
                                     .arg(dstLatestSnap),
                                 QStringLiteral("目标最新快照（%1）在源端不存在，无法执行增量同步。")
                                     .arg(dstLatestSnap)));
        return;
    }
    if (baseIdxInSrc < targetIdxInSrc) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("Destino tiene un snapshot más moderno (%1) que el snapshot objetivo (%2).\nNivelar cancelado.")
                                     .arg(dstLatestSnap, targetSnapName),
                                 QStringLiteral("Target has a newer snapshot (%1) than target snapshot to send (%2).\nLevel canceled.")
                                     .arg(dstLatestSnap, targetSnapName),
                                 QStringLiteral("目标端存在比要发送目标快照（%2）更“新”的快照（%1），已取消同步。")
                                     .arg(dstLatestSnap, targetSnapName)));
        return;
    }
    if (baseIdxInSrc == targetIdxInSrc) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Destino ya está nivelado en el snapshot objetivo (%1).")
                                         .arg(targetSnapName),
                                     QStringLiteral("Target is already leveled at target snapshot (%1).")
                                         .arg(targetSnapName),
                                     QStringLiteral("目标已处于目标快照（%1），无需同步。")
                                         .arg(targetSnapName)));
        return;
    }

    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const bool sameConnection = (src.connIdx == dst.connIdx);
    const QString fromSnap = src.datasetName + QStringLiteral("@") + dstLatestSnap;
    const QString srcSnap = src.datasetName + QStringLiteral("@") + targetSnapName;
    const QString sendRawCmd = QStringLiteral("zfs send -wLecR -I %1 %2").arg(shSingleQuote(fromSnap), shSingleQuote(srcSnap));
    QString sendCmd = withSudo(sp, sendRawCmd);
    const QString recvTarget = dst.datasetName;

    const QString srcSsh = sshBaseCommand(sp) + QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    const QString dstSsh = sshBaseCommand(dp) + QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    const QString recvRawCmd = QStringLiteral("zfs recv -Fus %1").arg(shSingleQuote(recvTarget));
    QString recvCmd = withSudoStreamInput(dp, recvRawCmd);

    QString pipeline;
    if (sameConnection) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Nivelar: modo local remoto (origen y destino en la misma conexión)"));
        // Important: run full pipeline under one sudo context so password never contaminates ZFS stream.
        const QString remotePipe = withSudo(sp, sendRawCmd
            + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
            + recvRawCmd);
        pipeline = sshExecFromLocal(sp, remotePipe);
    } else {
        pipeline =
            srcSsh + QStringLiteral(" ") + shSingleQuote(sendCmd)
            + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
            + dstSsh + QStringLiteral(" ") + shSingleQuote(recvCmd);
    }

    if (runLocalCommand(QStringLiteral("Nivelar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0, false, true)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionSyncDatasets() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const bool sameConnection = (src.connIdx == dst.connIdx);

    QString srcMp;
    QString dstMp;
    QString srcMounted;
    QString dstMounted;
    QString srcCanmount;
    QString dstCanmount;
    if (!getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mountpoint"), srcMp)
        || !getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mounted"), srcMounted)
        || !getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("canmount"), srcCanmount)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mountpoint"), dstMp)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mounted"), dstMounted)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("canmount"), dstCanmount)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudieron leer mountpoints para sincronizar."));
        return;
    }
    const bool srcRootMounted = isMountedValueTrue(srcMounted);
    const bool dstRootMounted = isMountedValueTrue(dstMounted);
    const bool srcCanmountOff = (srcCanmount.trimmed().toLower() == QStringLiteral("off"));
    const bool dstCanmountOff = (dstCanmount.trimmed().toLower() == QStringLiteral("off"));

    const QString srcSsh = sshBaseCommand(sp) + QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);

    // Build rsync options adaptively: macOS/BSD rsync may not support -A/-X short flags.
    const QString rsyncOptsProbe = QStringLiteral(
        "RSYNC_PROGRESS='--info=progress2'; "
        "rsync --help 2>/dev/null | grep -q -- '--info' || RSYNC_PROGRESS='--progress'; "
        "RSYNC_OPTS='-aHWS --delete'; "
        "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
        "if rsync -X --version >/dev/null 2>&1; then "
        "  RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
        "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then "
        "  RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; "
        "fi");
    const QString dstSsh = sshBaseCommand(dp) + QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);
    const QString srcEffectiveMp = effectiveMountPath(src.connIdx, src.poolName, src.datasetName, srcMp, srcMounted);
    const QString dstEffectiveMp = effectiveMountPath(dst.connIdx, dst.poolName, dst.datasetName, dstMp, dstMounted);
    auto isUsableMountPath = [&](int cidx, const QString& path) -> bool {
        const QString pth = path.trimmed();
        if (pth.isEmpty() || pth == QStringLiteral("none")) {
            return false;
        }
        if (isWindowsConnection(cidx)) {
            return pth.contains(':');
        }
        return pth.startsWith('/');
    };
    auto remoteHasTool = [&](int connIdx, const QString& tool) -> bool {
        if (connIdx < 0 || connIdx >= m_profiles.size() || tool.trimmed().isEmpty()) {
            return false;
        }
        const ConnectionProfile& p = m_profiles[connIdx];
        QString out;
        QString err;
        int rc = -1;
        const QString probeCmd = isWindowsConnection(connIdx)
                                     ? QStringLiteral("if (Get-Command %1 -ErrorAction SilentlyContinue) { Write-Output 1 } else { Write-Output 0 }")
                                           .arg(tool)
                                     : QStringLiteral("command -v %1 >/dev/null 2>&1 && echo 1 || echo 0").arg(tool);
        if (!runSsh(p, probeCmd, 15000, out, err, rc) || rc != 0) {
            return false;
        }
        return out.contains(QStringLiteral("1"));
    };
    enum class StreamCodec { Zstd, Gzip, None };
    auto codecName = [](StreamCodec c) -> QString {
        switch (c) {
            case StreamCodec::Zstd: return QStringLiteral("zstd-fast");
            case StreamCodec::Gzip: return QStringLiteral("gzip-fast");
            case StreamCodec::None:
            default: return QStringLiteral("none");
        }
    };
    auto chooseCodec = [&]() -> StreamCodec {
        const bool zstdBoth = remoteHasTool(src.connIdx, QStringLiteral("zstd")) && remoteHasTool(dst.connIdx, QStringLiteral("zstd"));
        if (zstdBoth) {
            return StreamCodec::Zstd;
        }
        const bool gzipBoth = remoteHasTool(src.connIdx, QStringLiteral("gzip")) && remoteHasTool(dst.connIdx, QStringLiteral("gzip"));
        if (gzipBoth) {
            return StreamCodec::Gzip;
        }
        return StreamCodec::None;
    };
    auto buildSrcTarCmd = [&](bool isWin, const QString& mountPath, StreamCodec codec) -> QString {
        switch (codec) {
            case StreamCodec::Zstd:
                return isWin
                           ? QStringLiteral("$p=%1; tar -cf - -C $p . | zstd -1 -T0 -q -c").arg(shSingleQuote(mountPath))
                           : QStringLiteral("tar --acls --xattrs -cpf - -C %1 . | zstd -1 -T0 -q -c").arg(shSingleQuote(mountPath));
            case StreamCodec::Gzip:
                return isWin
                           ? QStringLiteral("$p=%1; tar -cf - -C $p . | gzip -1 -c").arg(shSingleQuote(mountPath))
                           : QStringLiteral("tar --acls --xattrs -cpf - -C %1 . | gzip -1 -c").arg(shSingleQuote(mountPath));
            case StreamCodec::None:
            default:
                return isWin
                           ? QStringLiteral("$p=%1; tar -cf - -C $p .").arg(shSingleQuote(mountPath))
                           : QStringLiteral("tar --acls --xattrs -cpf - -C %1 .").arg(shSingleQuote(mountPath));
        }
    };
    auto buildDstTarCmd = [&](bool isWin, const QString& mountPath, StreamCodec codec) -> QString {
        const QString decodePipe =
            (codec == StreamCodec::Zstd) ? QStringLiteral("zstd -d -q -c - | ")
            : (codec == StreamCodec::Gzip) ? QStringLiteral("gzip -d -c - | ")
            : QString();
        if (isWin) {
            return QStringLiteral("$ProgressPreference='SilentlyContinue'; $p=%1; if (!(Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null }; %2tar -xpf - -C $p")
                .arg(shSingleQuote(mountPath), decodePipe);
        }
        return QStringLiteral("mkdir -p %1 && %2tar --acls --xattrs -xpf - -C %1")
            .arg(shSingleQuote(mountPath), decodePipe);
    };

    if (srcRootMounted && dstRootMounted) {
        if (!isUsableMountPath(src.connIdx, srcEffectiveMp) || !isUsableMountPath(dst.connIdx, dstEffectiveMp)) {
            QMessageBox::warning(this,
                                 QStringLiteral("ZFSMgr"),
                                 QStringLiteral("No se pudo resolver el punto de montaje efectivo para sincronizar.\n"
                                                "Origen: %1\nDestino: %2")
                                     .arg(srcEffectiveMp, dstEffectiveMp));
            return;
        }
        if (isWindowsConnection(sp) || isWindowsConnection(dp)) {
            const StreamCodec codec = chooseCodec();
            const QString srcTarCmd = buildSrcTarCmd(isWindowsConnection(sp), srcEffectiveMp, codec);
            const QString dstTarCmd = buildDstTarCmd(isWindowsConnection(dp), dstEffectiveMp, codec);
            QString command;
            if (sameConnection) {
                appLog(QStringLiteral("INFO"), QStringLiteral("Sincronizar: modo local remoto (tar, misma conexión)"));
                const QString remotePipe = srcTarCmd
                    + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
                    + dstTarCmd;
                command = sshExecFromLocal(sp, remotePipe);
            } else {
                command = sshExecFromLocal(sp, srcTarCmd)
                    + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
                    + sshExecFromLocal(dp, dstTarCmd);
            }
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Sincronizar en Windows usa fallback tar/ssh (codec=%1, sin --delete).").arg(codecName(codec)));
            if (runLocalCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command, 0, false, true)) {
                invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
                reloadDatasetSide(QStringLiteral("dest"));
            }
            return;
        }
        QString command;
        if (sameConnection) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Sincronizar: modo local remoto (rsync, misma conexión)"));
            QString remoteRsync =
                QStringLiteral("%1; rsync $RSYNC_OPTS $RSYNC_PROGRESS %2/ %3/")
                    .arg(rsyncOptsProbe,
                         shSingleQuote(srcEffectiveMp),
                         shSingleQuote(dstEffectiveMp));
            remoteRsync = withSudo(sp, remoteRsync);
            command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        } else {
            QString remoteRsync =
                QStringLiteral("%1; rsync $RSYNC_OPTS $RSYNC_PROGRESS -e %2 %3/ %4:%5/")
                    .arg(rsyncOptsProbe,
                         shSingleQuote(sshBaseCommand(dp)),
                         shSingleQuote(srcEffectiveMp),
                         shSingleQuote(dp.username + QStringLiteral("@") + dp.host),
                         shSingleQuote(dstEffectiveMp));
            remoteRsync = withSudo(sp, remoteRsync);
            command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        }
        if (runLocalCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command, 0, false, true)) {
            invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
            reloadDatasetSide(QStringLiteral("dest"));
        }
        return;
    }

    // Modo datasets raíz no montables: solo permitido cuando no están montados por canmount=off
    // y existen subdatasets montados equivalentes en origen y destino.
    if ((!srcRootMounted && !srcCanmountOff) || (!dstRootMounted && !dstCanmountOff)) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             QStringLiteral("Origen y destino deben estar montados para sincronizar,\n"
                                            "o bien tener canmount=off con subdatasets montados.\n"
                                            "Origen mounted=%1 canmount=%2\nDestino mounted=%3 canmount=%4")
                                 .arg(srcMounted, srcCanmount, dstMounted, dstCanmount));
        return;
    }

    if (!ensureDatasetsLoaded(src.connIdx, src.poolName) || !ensureDatasetsLoaded(dst.connIdx, dst.poolName)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             QStringLiteral("No se pudieron cargar datasets para sincronización por subdatasets."));
        return;
    }
    const QString srcKey = datasetCacheKey(src.connIdx, src.poolName);
    const QString dstKey = datasetCacheKey(dst.connIdx, dst.poolName);
    const auto srcCacheIt = m_poolDatasetCache.constFind(srcKey);
    const auto dstCacheIt = m_poolDatasetCache.constFind(dstKey);
    if (srcCacheIt == m_poolDatasetCache.constEnd() || dstCacheIt == m_poolDatasetCache.constEnd()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             QStringLiteral("No hay caché de datasets para sincronización por subdatasets."));
        return;
    }

    const QString srcPrefix = src.datasetName + QStringLiteral("/");
    QStringList missingInDest;
    QStringList notMountedPairs;
    QVector<QPair<QString, QString>> syncPairs; // srcMp, dstMp

    for (auto it = srcCacheIt->recordByName.constBegin(); it != srcCacheIt->recordByName.constEnd(); ++it) {
        const QString& srcDsName = it.key();
        if (!srcDsName.startsWith(srcPrefix)) {
            continue;
        }
        const DatasetRecord& srcRec = it.value();
        if (!isMountedValueTrue(srcRec.mounted)) {
            continue;
        }
        const QString srcRecMp = effectiveMountPath(src.connIdx, src.poolName, srcDsName, srcRec.mountpoint, srcRec.mounted);
        if (!isUsableMountPath(src.connIdx, srcRecMp)) {
            continue;
        }
        const QString rel = srcDsName.mid(srcPrefix.size());
        if (rel.isEmpty()) {
            continue;
        }
        const QString dstDsName = dst.datasetName + QStringLiteral("/") + rel;
        const auto dstRecIt = dstCacheIt->recordByName.constFind(dstDsName);
        if (dstRecIt == dstCacheIt->recordByName.constEnd()) {
            missingInDest << dstDsName;
            continue;
        }
        const DatasetRecord& dstRec = dstRecIt.value();
        const QString dstRecMp = effectiveMountPath(dst.connIdx, dst.poolName, dstDsName, dstRec.mountpoint, dstRec.mounted);
        if (!isMountedValueTrue(dstRec.mounted) || !isUsableMountPath(dst.connIdx, dstRecMp)) {
            notMountedPairs << QStringLiteral("%1 -> %2").arg(srcDsName, dstDsName);
            continue;
        }
        syncPairs.push_back(qMakePair(srcRecMp, dstRecMp));
    }

    if (!missingInDest.isEmpty() || !notMountedPairs.isEmpty()) {
        QString details;
        if (!missingInDest.isEmpty()) {
            details += QStringLiteral("Faltan en destino:\n") + missingInDest.join('\n') + QStringLiteral("\n\n");
        }
        if (!notMountedPairs.isEmpty()) {
            details += QStringLiteral("No montados en ambos extremos:\n") + notMountedPairs.join('\n');
        }
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             QStringLiteral("No se puede sincronizar por subdatasets.\n%1").arg(details.trimmed()));
        return;
    }
    if (syncPairs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 QStringLiteral("No hay subdatasets montados equivalentes para sincronizar."));
        return;
    }

    const QString sshTransport = sshBaseCommand(dp);
    QStringList rsyncCommands;
    if (isWindowsConnection(sp) || isWindowsConnection(dp)) {
        const StreamCodec codec = chooseCodec();
        QStringList tarPipelines;
        tarPipelines.reserve(syncPairs.size());
        for (const auto& pair : syncPairs) {
            const QString srcTarCmd = buildSrcTarCmd(isWindowsConnection(sp), pair.first, codec);
            const QString dstTarCmd = buildDstTarCmd(isWindowsConnection(dp), pair.second, codec);
            if (sameConnection) {
                tarPipelines << sshExecFromLocal(
                    sp,
                    srcTarCmd
                        + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
                        + dstTarCmd);
            } else {
                tarPipelines << (sshExecFromLocal(sp, srcTarCmd)
                    + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab -f) || cat) | ")
                    + sshExecFromLocal(dp, dstTarCmd));
            }
        }
        const QString command = tarPipelines.join(QStringLiteral(" && "));
        appLog(QStringLiteral("WARN"),
               QStringLiteral("Sincronizar subdatasets en Windows usa fallback tar/ssh (codec=%1, sin --delete).").arg(codecName(codec)));
        if (runLocalCommand(QStringLiteral("Sincronizar subdatasets %1 -> %2 (%3)")
                                .arg(src.datasetName, dst.datasetName)
                                .arg(syncPairs.size()),
                            command,
                            0,
                            false,
                            true)) {
            invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
            reloadDatasetSide(QStringLiteral("dest"));
        }
    } else {
        rsyncCommands.reserve(syncPairs.size());
        for (const auto& pair : syncPairs) {
            if (sameConnection) {
                rsyncCommands << QStringLiteral("rsync $RSYNC_OPTS $RSYNC_PROGRESS %1/ %2/")
                                     .arg(shSingleQuote(pair.first), shSingleQuote(pair.second));
            } else {
                rsyncCommands << QStringLiteral("rsync $RSYNC_OPTS $RSYNC_PROGRESS -e %1 %2/ %3:%4/")
                                     .arg(shSingleQuote(sshTransport),
                                          shSingleQuote(pair.first),
                                          shSingleQuote(dp.username + QStringLiteral("@") + dp.host),
                                          shSingleQuote(pair.second));
            }
        }
        QString remoteRsync = QStringLiteral("set -e; %1; %2")
                                  .arg(rsyncOptsProbe, rsyncCommands.join(QStringLiteral(" && ")));
        remoteRsync = withSudo(sp, remoteRsync);
        const QString command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
        if (runLocalCommand(QStringLiteral("Sincronizar subdatasets %1 -> %2 (%3)")
                                .arg(src.datasetName, dst.datasetName)
                                .arg(syncPairs.size()),
                            command,
                            0,
                            false,
                            true)) {
            invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
            reloadDatasetSide(QStringLiteral("dest"));
        }
    }
}

void MainWindow::actionAdvancedBreakdown() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset en Avanzado."));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const ConnectionProfile& p = m_profiles[connIdx];
    QString mountOut;
    QString mountErr;
    int mountRc = -1;
    const QString mountCheckCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o name,value -r mounted %1").arg(shSingleQuote(ds)));
    if (!runSsh(p, mountCheckCmd, 25000, mountOut, mountErr, mountRc) || mountRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    QStringList unmounted;
    for (const QString& ln : mountOut.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = ln.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString name = parts[0].trimmed();
        const QString mountedVal = parts[1].trimmed();
        if (name.isEmpty() || name.contains('@')) {
            continue; // ignorar snapshots
        }
        if (!isMountedValueTrue(mountedVal)) {
            unmounted << name;
        }
    }
    if (!unmounted.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Desglosar requiere dataset y descendientes montados.\nNo montados:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("Break down requires dataset and descendants mounted.\nNot mounted:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("拆分要求数据集及其所有后代已挂载。\n未挂载：\n%1")
                    .arg(unmounted.join('\n'))));
        return;
    }

    QStringList dirs;
    QString resolvedMp;
    QString listOut;
    QString listErr;
    int listRc = -1;
    if (isWindowsConnection(p)) {
        QString dsPs = ds;
        dsPs.replace('\'', QStringLiteral("''"));
        const QString listCmd = QStringLiteral(
                                    "$ds='%1'; "
                                    "function Resolve-Mp([string]$name){ "
                                    "  $cur=$name; $anchor=$null; $drive=''; "
                                    "  while($true){ "
                                    "    $dl=(zfs get -H -o value driveletter $cur 2>$null | Out-String).Trim(); "
                                    "    if(-not [string]::IsNullOrWhiteSpace($dl) -and $dl -ne '-' -and $dl.ToLower() -ne 'none'){ $drive=$dl; $anchor=$cur; break }; "
                                    "    if($cur -notmatch '/'){ break }; "
                                    "    $cur=$cur.Substring(0,$cur.LastIndexOf('/')); "
                                    "  }; "
                                    "  if([string]::IsNullOrWhiteSpace($drive)){ return '' }; "
                                    "  $drive=$drive.Trim(); "
                                    "  if($drive.Length -gt 0 -and $drive[$drive.Length-1] -eq ':'){ $drive=$drive.Substring(0,$drive.Length-1) }; "
                                    "  $drive=$drive.Substring(0,1).ToUpper(); "
                                    "  $base=$drive + ':\\\\'; "
                                    "  if($name -eq $anchor){ return $base }; "
                                    "  $suffix=$name.Substring($anchor.Length).TrimStart('/'); "
                                    "  if([string]::IsNullOrWhiteSpace($suffix)){ return $base }; "
                                    "  return ($base + ($suffix -replace '/','\\\\')) "
                                    "}; "
                                    "$mp=Resolve-Mp $ds; "
                                    "if (-not [string]::IsNullOrWhiteSpace($mp)) { Write-Output ('__MP__=' + $mp) }; "
                                    "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { exit 0 }; "
                                    "Get-ChildItem -LiteralPath $mp -Directory -Force | Select-Object -ExpandProperty Name | Sort-Object -Unique")
                                    .arg(dsPs);
        if (!runSsh(p, withSudo(p, listCmd), 25000, listOut, listErr, listRc) || listRc != 0) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No se pudieron listar directorios para desglosar."),
                                     QStringLiteral("Could not list directories for breakdown."),
                                     QStringLiteral("无法列出可拆分目录。")));
            return;
        }
        const QStringList lines = listOut.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString ln = raw.trimmed();
            if (ln.startsWith(QStringLiteral("__MP__="))) {
                resolvedMp = ln.mid(QStringLiteral("__MP__=").size()).trimmed();
            } else if (!ln.isEmpty()) {
                dirs << ln;
            }
        }
    } else {
        const QString listCmd = withSudo(
            p,
            QStringLiteral("set -e; DATASET=%1; "
                           "resolve_mp(){ ds=\"$1\"; mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$ds\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"; }; "
                           "MP=$(resolve_mp \"$DATASET\"); "
                           "[ -n \"$MP\" ] || exit 0; "
                           "[ -d \"$MP\" ] || exit 0; "
                           "for d in \"$MP\"/.[!.]* \"$MP\"/..?* \"$MP\"/*; do [ -d \"$d\" ] || continue; bn=$(basename \"$d\"); [ -n \"$bn\" ] && printf '%s\\n' \"$bn\"; done | sort -u")
                .arg(shSingleQuote(ds)));
        if (!runSsh(p, listCmd, 25000, listOut, listErr, listRc) || listRc != 0) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No se pudieron listar directorios para desglosar."),
                                     QStringLiteral("Could not list directories for breakdown."),
                                     QStringLiteral("无法列出可拆分目录。")));
            return;
        }
        dirs = listOut.split('\n', Qt::SkipEmptyParts);
        for (QString& d : dirs) {
            d = d.trimmed();
        }
        dirs.removeAll(QString());
        QString mpOut;
        QString mpErr;
        int mpRc = -1;
        const QString mpCmd = withSudo(
            p,
            QStringLiteral("DATASET=%1; "
                           "mp=$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$DATASET\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"")
                .arg(shSingleQuote(ds)));
        runSsh(p, mpCmd, 20000, mpOut, mpErr, mpRc);
        resolvedMp = mpOut.trimmed();
    }

    QString dsListOut;
    QString dsListErr;
    int dsListRc = -1;
    QStringList datasetsDetected;
    QSet<QString> childDatasetNames;
    const QString dsListCmd = withSudo(
        p,
        QStringLiteral("zfs list -H -o name -r %1")
            .arg(shSingleQuote(ds)));
    if (runSsh(p, dsListCmd, 25000, dsListOut, dsListErr, dsListRc) && dsListRc == 0) {
        datasetsDetected = dsListOut.split('\n', Qt::SkipEmptyParts);
        for (QString& n : datasetsDetected) {
            n = n.trimmed();
        }
        datasetsDetected.removeAll(QString());
        const QString prefix = ds + QStringLiteral("/");
        for (const QString& datasetName : datasetsDetected) {
            if (!datasetName.startsWith(prefix)) {
                continue;
            }
            const QString childPath = datasetName.mid(prefix.size());
            const QString childName = childPath.section('/', 0, 0).trimmed();
            if (!childName.isEmpty()) {
                childDatasetNames.insert(childName);
            }
        }
    }
    if (!childDatasetNames.isEmpty()) {
        dirs.erase(std::remove_if(dirs.begin(), dirs.end(),
                                  [&](const QString& d) { return childDatasetNames.contains(d); }),
                   dirs.end());
    }
    QStringList mountedDescMountpoints;
    if (!isWindowsConnection(p)) {
        QString mountedDescOut;
        QString mountedDescErr;
        int mountedDescRc = -1;
        const QString mountedDescCmd = withSudo(
            p,
            QStringLiteral("DATASET=%1; zfs mount | awk -v pfx=\"$DATASET/\" 'index($1,pfx)==1 {print $2}'")
                .arg(shSingleQuote(ds)));
        if (runSsh(p, mountedDescCmd, 25000, mountedDescOut, mountedDescErr, mountedDescRc) && mountedDescRc == 0) {
            mountedDescMountpoints = mountedDescOut.split('\n', Qt::SkipEmptyParts);
            for (QString& mp : mountedDescMountpoints) {
                mp = mp.trimmed();
            }
            mountedDescMountpoints.removeAll(QString());
        }
    }
    if (!resolvedMp.isEmpty() && !mountedDescMountpoints.isEmpty()) {
        QString baseMp = resolvedMp;
        while (baseMp.endsWith('/')) {
            baseMp.chop(1);
        }
        dirs.erase(std::remove_if(dirs.begin(), dirs.end(),
                                  [&](const QString& d) {
                                      const QString dirPath = baseMp + QStringLiteral("/") + d;
                                      const QString dirPathWithSlash = dirPath + QStringLiteral("/");
                                      for (const QString& childMp : mountedDescMountpoints) {
                                          if (childMp == dirPath || childMp.startsWith(dirPathWithSlash)) {
                                              return true;
                                          }
                                      }
                                      return false;
                                  }),
                   dirs.end());
    }
    if (dirs.isEmpty()) {
        const QString dirsText = dirs.isEmpty()
                                     ? tr3(QStringLiteral("(ninguno)"), QStringLiteral("(none)"), QStringLiteral("（无）"))
                                     : dirs.join('\n');
        const QString datasetsText = datasetsDetected.isEmpty()
                                         ? tr3(QStringLiteral("(ninguno)"), QStringLiteral("(none)"), QStringLiteral("（无）"))
                                         : datasetsDetected.join('\n');
        const QString mpText = resolvedMp.isEmpty()
                                   ? tr3(QStringLiteral("(sin resolver)"), QStringLiteral("(unresolved)"), QStringLiteral("（未解析）"))
                                   : resolvedMp;

        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No hay directorios para desglosar en el dataset seleccionado.\n\n"
                                                    "Dataset: %1\n"
                                                    "Mountpoint resuelto: %2\n\n"
                                                    "Directorios detectados:\n%3\n\n"
                                                    "Datasets detectados:\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText),
                                     QStringLiteral("No directories available to break down in selected dataset.\n\n"
                                                    "Dataset: %1\n"
                                                    "Resolved mountpoint: %2\n\n"
                                                    "Detected directories:\n%3\n\n"
                                                    "Detected datasets:\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText),
                                     QStringLiteral("所选数据集中没有可拆分目录。\n\n"
                                                    "数据集：%1\n"
                                                    "解析后的挂载点：%2\n\n"
                                                    "检测到的目录：\n%3\n\n"
                                                    "检测到的数据集：\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText)));
        return;
    }
    QStringList selectedDirs;
    if (!selectItemsDialog(
            tr3(QStringLiteral("Desglosar: seleccionar directorios"),
                QStringLiteral("Break down: select directories"),
                QStringLiteral("拆分：选择目录")),
            tr3(QStringLiteral("Seleccione los directorios que desea desglosar en subdatasets."),
                QStringLiteral("Select directories to split into subdatasets."),
                QStringLiteral("请选择要拆分为子数据集的目录。")),
            dirs,
            selectedDirs)) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Desglosar cancelado o sin selección."));
        return;
    }

    QString cmd;
    bool allowWindowsScript = false;
    if (isWindowsConnection(p)) {
        QStringList selectedPs;
        selectedPs.reserve(selectedDirs.size());
        for (QString d : selectedDirs) {
            d.replace('\'', QStringLiteral("''"));
            selectedPs << QStringLiteral("'%1'").arg(d);
        }
        QString dsPs = ds;
        dsPs.replace('\'', QStringLiteral("''"));
        cmd = QStringLiteral(
                  "$ds='%1'; "
                  "$selected=@(%2); "
                  "function Resolve-Mp([string]$name){ "
                  "  $cur=$name; $anchor=$null; $drive=''; "
                  "  while($true){ "
                  "    $dl=(zfs get -H -o value driveletter $cur 2>$null | Out-String).Trim(); "
                  "    if(-not [string]::IsNullOrWhiteSpace($dl) -and $dl -ne '-' -and $dl.ToLower() -ne 'none'){ $drive=$dl; $anchor=$cur; break }; "
                  "    if($cur -notmatch '/'){ break }; "
                  "    $cur=$cur.Substring(0,$cur.LastIndexOf('/')); "
                  "  }; "
                  "  if([string]::IsNullOrWhiteSpace($drive)){ return '' }; "
                  "  $drive=$drive.Trim(); if($drive.Length -gt 0 -and $drive[$drive.Length-1] -eq ':'){ $drive=$drive.Substring(0,$drive.Length-1) }; "
                  "  $drive=$drive.Substring(0,1).ToUpper(); "
                  "  $base=$drive + ':\\\\'; "
                  "  if($name -eq $anchor){ return $base }; "
                  "  $suffix=$name.Substring($anchor.Length).TrimStart('/'); "
                  "  if([string]::IsNullOrWhiteSpace($suffix)){ return $base }; "
                  "  return ($base + ($suffix -replace '/','\\\\')) "
                  "}; "
                  "$mp=Resolve-Mp $ds; "
                  "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { throw 'mountpoint=none' }; "
                  "foreach($bn in $selected){ "
                  "  $src=Join-Path $mp $bn; if (-not (Test-Path -LiteralPath $src -PathType Container)) { continue }; "
                  "  $child=\"$ds/$bn\"; "
                  "  zfs list -H -o name $child 2>$null | Out-Null; if ($LASTEXITCODE -ne 0) { zfs create $child | Out-Null; if($LASTEXITCODE -ne 0){ throw \"zfs create failed for $child\" } }; "
                  "  zfs mount $child 2>$null | Out-Null; "
                  "  $cmp=Resolve-Mp $child; if ([string]::IsNullOrWhiteSpace($cmp)) { throw \"cannot resolve mountpoint for $child\" }; "
                  "  if (-not (Test-Path -LiteralPath $cmp)) { New-Item -ItemType Directory -Force -Path $cmp | Out-Null }; "
                  "  $srcNorm=[System.IO.Path]::GetFullPath($src).TrimEnd('\\\\'); "
                  "  $cmpNorm=[System.IO.Path]::GetFullPath($cmp).TrimEnd('\\\\'); "
                  "  if ($srcNorm.Equals($cmpNorm,[System.StringComparison]::OrdinalIgnoreCase)) { throw \"unsafe breakdown paths (same): $srcNorm\" }; "
                  "  if ($cmpNorm.StartsWith($srcNorm + '\\\\',[System.StringComparison]::OrdinalIgnoreCase)) { throw \"unsafe breakdown paths (dst under src): $srcNorm -> $cmpNorm\" }; "
                  "  if ($srcNorm.StartsWith($cmpNorm + '\\\\',[System.StringComparison]::OrdinalIgnoreCase)) { throw \"unsafe breakdown paths (src under dst): $srcNorm -> $cmpNorm\" }; "
                  "  robocopy $src $cmp /E /MOVE /COPYALL /R:1 /W:1 /NFL /NDL /NP | Out-Null; "
                  "  $r=$LASTEXITCODE; if ($r -ge 8) { throw \"robocopy failed ($r) for $bn\" }; "
                  "}")
                  .arg(dsPs, selectedPs.join(QStringLiteral(",")));
        allowWindowsScript = true;
    } else {
        QStringList selectedQuoted;
        selectedQuoted.reserve(selectedDirs.size());
        for (const QString& d : selectedDirs) {
            selectedQuoted << shSingleQuote(d);
        }
        const QString selectedList = selectedQuoted.join(' ');
        cmd =
            QStringLiteral("set -e; DATASET=%1; "
                           "RSYNC_OPTS='-aHWS'; "
                           "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                           "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                           "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                           "resolve_mp(){ ds=\"$1\"; mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$ds\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"; }; "
                           "MP=$(resolve_mp \"$DATASET\"); "
                           "[ -n \"$MP\" ] || { echo \"mountpoint=none\"; exit 2; }; "
                           "SELECTED_DIRS=(%2); is_selected_dir(){ for s in \"${SELECTED_DIRS[@]}\"; do [ \"$s\" = \"$1\" ] && return 0; done; return 1; }; "
                           "for d in \"$MP\"/.[!.]* \"$MP\"/..?* \"$MP\"/*; do [ -d \"$d\" ] || continue; bn=$(basename \"$d\"); is_selected_dir \"$bn\" || continue; "
                           "child=\"$DATASET/$bn\"; "
                           "zfs list -H -o name \"$child\" >/dev/null 2>&1 && { echo \"child_exists=$child\"; continue; }; "
                           "FINAL_MP=\"$MP/$bn\"; "
                           "TMP_CHILD_MP=$(mktemp -d /tmp/zfsmgr-breakdown-child-XXXXXX); "
                           "zfs create -o mountpoint=\"$TMP_CHILD_MP\" \"$child\"; "
                           "zfs mount \"$child\" >/dev/null 2>&1 || true; "
                           "try=0; "
                           "while :; do "
                           "  rsync $RSYNC_OPTS \"$d\"/ \"$TMP_CHILD_MP\"/; "
                           "  PENDING=$(rsync -rni --ignore-existing \"$d\"/ \"$TMP_CHILD_MP\"/ | awk 'length($0)>11{c=substr($0,1,11); if(substr(c,1,1)==\">\" && substr(c,2,1)==\"f\") n++} END{print n+0}'); "
                           "  [ \"$PENDING\" = \"0\" ] && break; "
                           "  try=$((try+1)); "
                           "  [ \"$try\" -lt 5 ] || break; "
                           "done; "
                           "[ \"$PENDING\" = \"0\" ] || { echo \"verify_failed=$child pending=$PENDING\"; exit 42; }; "
                           "rm -rf \"$d\"; "
                           "zfs set mountpoint=\"$FINAL_MP\" \"$child\"; "
                           "zfs mount \"$child\" >/dev/null 2>&1 || true; "
                           "rmdir \"$TMP_CHILD_MP\" >/dev/null 2>&1 || true; "
                           "done")
                .arg(shSingleQuote(ds), selectedList);
    }
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Desglosar"), ctx, cmd, 0, allowWindowsScript)) {
        updateAdvancedSelectionUi(ds, QString());
    }
}

void MainWindow::actionAdvancedAssemble() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset en Avanzado."));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const ConnectionProfile& p = m_profiles[connIdx];
    QString mountOut;
    QString mountErr;
    int mountRc = -1;
    const QString mountCheckCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o name,value -r mounted %1").arg(shSingleQuote(ds)));
    if (!runSsh(p, mountCheckCmd, 25000, mountOut, mountErr, mountRc) || mountRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    QStringList unmounted;
    for (const QString& ln : mountOut.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = ln.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString name = parts[0].trimmed();
        const QString mountedVal = parts[1].trimmed();
        if (name.isEmpty() || name.contains('@')) {
            continue; // ignorar snapshots
        }
        if (!isMountedValueTrue(mountedVal)) {
            unmounted << name;
        }
    }
    if (!unmounted.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Ensamblar requiere dataset y descendientes montados.\nNo montados:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("Assemble requires dataset and descendants mounted.\nNot mounted:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("组装要求数据集及其所有后代已挂载。\n未挂载：\n%1")
                    .arg(unmounted.join('\n'))));
        return;
    }

    QString listOut;
    QString listErr;
    int listRc = -1;
    const QString listCmd = withSudo(
        p,
        QStringLiteral("zfs list -H -o name -r %1")
            .arg(shSingleQuote(ds)));
    if (!runSsh(p, listCmd, 25000, listOut, listErr, listRc) || listRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudieron listar subdatasets para ensamblar."),
                                 QStringLiteral("Could not list child datasets for assemble."),
                                 QStringLiteral("无法列出可组装子数据集。")));
        return;
    }
    QStringList children = listOut.split('\n', Qt::SkipEmptyParts);
    for (QString& c : children) {
        c = c.trimmed();
    }
    children.removeAll(QString());
    children.erase(std::remove(children.begin(), children.end(), ds), children.end());
    if (children.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No hay subdatasets para ensamblar."),
                                     QStringLiteral("No child datasets available to assemble."),
                                     QStringLiteral("没有可组装的子数据集。")));
        return;
    }
    QStringList selectedChildren;
    if (!selectItemsDialog(
            tr3(QStringLiteral("Ensamblar: seleccionar subdatasets"),
                QStringLiteral("Assemble: select child datasets"),
                QStringLiteral("组装：选择子数据集")),
            tr3(QStringLiteral("Seleccione los subdatasets que desea ensamblar en el dataset padre."),
                QStringLiteral("Select child datasets to assemble into parent dataset."),
                QStringLiteral("请选择要组装回父数据集的子数据集。")),
            children,
            selectedChildren)) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Ensamblar cancelado o sin selección."));
        return;
    }
    QString cmd;
    bool allowWindowsScript = false;
    if (isWindowsConnection(p)) {
        QStringList selectedPs;
        selectedPs.reserve(selectedChildren.size());
        for (QString c : selectedChildren) {
            c.replace('\'', QStringLiteral("''"));
            selectedPs << QStringLiteral("'%1'").arg(c);
        }
        QString dsPs = ds;
        dsPs.replace('\'', QStringLiteral("''"));
        cmd = QStringLiteral(
                  "$parent='%1'; "
                  "$selected=@(%2); "
                  "function Resolve-Mp([string]$name){ "
                  "  $cur=$name; $anchor=$null; $drive=''; "
                  "  while($true){ "
                  "    $dl=(zfs get -H -o value driveletter $cur 2>$null | Out-String).Trim(); "
                  "    if(-not [string]::IsNullOrWhiteSpace($dl) -and $dl -ne '-' -and $dl.ToLower() -ne 'none'){ $drive=$dl; $anchor=$cur; break }; "
                  "    if($cur -notmatch '/'){ break }; "
                  "    $cur=$cur.Substring(0,$cur.LastIndexOf('/')); "
                  "  }; "
                  "  if([string]::IsNullOrWhiteSpace($drive)){ return '' }; "
                  "  $drive=$drive.Trim(); if($drive.Length -gt 0 -and $drive[$drive.Length-1] -eq ':'){ $drive=$drive.Substring(0,$drive.Length-1) }; "
                  "  $drive=$drive.Substring(0,1).ToUpper(); "
                  "  $base=$drive + ':\\\\'; "
                  "  if($name -eq $anchor){ return $base }; "
                  "  $suffix=$name.Substring($anchor.Length).TrimStart('/'); "
                  "  if([string]::IsNullOrWhiteSpace($suffix)){ return $base }; "
                  "  return ($base + ($suffix -replace '/','\\\\')) "
                  "}; "
                  "$pmp=Resolve-Mp $parent; "
                  "if ([string]::IsNullOrWhiteSpace($pmp) -or -not (Test-Path -LiteralPath $pmp)) { throw 'mountpoint=none' }; "
                  "foreach($child in $selected){ "
                  "  $bn=($child -split '/')[($child -split '/').Count-1]; "
                  "  $cmp=Resolve-Mp $child; if ([string]::IsNullOrWhiteSpace($cmp) -or -not (Test-Path -LiteralPath $cmp)) { continue }; "
                  "  $tmp=Join-Path $env:TEMP ('zfsmgr-assemble-' + [guid]::NewGuid().ToString('N')); "
                  "  New-Item -ItemType Directory -Force -Path $tmp | Out-Null; "
                  "  robocopy $cmp $tmp /E /COPYALL /R:1 /W:1 /NFL /NDL /NP | Out-Null; "
                  "  $r=$LASTEXITCODE; if ($r -ge 8) { throw \"robocopy (to tmp) failed ($r) for $child\" }; "
                  "  zfs destroy -r $child | Out-Null; if ($LASTEXITCODE -ne 0) { throw \"zfs destroy failed for $child\" }; "
                  "  $dst=Join-Path $pmp $bn; if (-not (Test-Path -LiteralPath $dst)) { New-Item -ItemType Directory -Force -Path $dst | Out-Null }; "
                  "  robocopy $tmp $dst /E /COPYALL /R:1 /W:1 /NFL /NDL /NP | Out-Null; "
                  "  $r=$LASTEXITCODE; if ($r -ge 8) { throw \"robocopy (tmp->dst) failed ($r) for $child\" }; "
                  "  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue; "
                  "}")
                  .arg(dsPs, selectedPs.join(QStringLiteral(",")));
        allowWindowsScript = true;
    } else {
        QStringList selectedQuoted;
        selectedQuoted.reserve(selectedChildren.size());
        for (const QString& c : selectedChildren) {
            selectedQuoted << shSingleQuote(c);
        }
        const QString selectedList = selectedQuoted.join(' ');
        cmd =
            QStringLiteral("set -e; DATASET=%1; "
                           "RSYNC_OPTS='-aHWS'; "
                           "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                           "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                           "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                           "resolve_mp(){ ds=\"$1\"; mp=$(zfs get -H -o value mountpoint \"$ds\" 2>/dev/null || true); "
                           "case \"$mp\" in \"\"|none|legacy|-) mp=$(zfs mount | awk -v d=\"$ds\" '$1==d{print $2; exit}');; esac; "
                           "printf '%s' \"$mp\"; }; "
                           "MP=$(resolve_mp \"$DATASET\"); "
                           "[ -n \"$MP\" ] || { echo \"mountpoint=none\"; exit 2; }; "
                           "SELECTED_CHILDREN=(%2); "
                           "for child in \"${SELECTED_CHILDREN[@]}\"; do bn=${child##*/}; "
                           "CMP=$(resolve_mp \"$child\"); [ -n \"$CMP\" ] || continue; "
                           "TMP=$(mktemp -d /tmp/zfsmgr-assemble-XXXXXX); "
                           "rsync $RSYNC_OPTS \"$CMP\"/ \"$TMP\"/; "
                           "zfs destroy -r \"$child\"; "
                           "mkdir -p \"$MP/$bn\"; "
                           "rsync $RSYNC_OPTS \"$TMP\"/ \"$MP/$bn\"/; "
                           "rm -rf \"$TMP\"; "
                           "done")
                .arg(shSingleQuote(ds), selectedList);
    }
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Ensamblar"), ctx, cmd, 0, allowWindowsScript)) {
        updateAdvancedSelectionUi(ds, QString());
        refreshConnectionByIndex(ctx.connIdx);
    }
}

void MainWindow::actionAdvancedCreateFromDir() {
    if (actionsLocked()) {
        return;
    }
    const auto selected = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString().trimmed();
    const QString snap = selected.first()->data(1, Qt::UserRole).toString().trimmed();
    if (ds.isEmpty() || !snap.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un dataset (no snapshot)."),
                                     QStringLiteral("You must select a dataset (not a snapshot)."),
                                     QStringLiteral("必须选择数据集（不能是快照）。")));
        return;
    }

    const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = token.left(sep).toInt();
    ctx.poolName = token.mid(sep + 2);
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    struct PropSpec {
        QString name;
        QString kind;
        QStringList values;
    };
    struct PropEditor {
        QString name;
        QLineEdit* edit{nullptr};
        QComboBox* combo{nullptr};
    };

    QDialog dlg(this);
    dlg.setWindowTitle(tr3(QStringLiteral("Crear dataset desde directorio"),
                           QStringLiteral("Create dataset from directory"),
                           QStringLiteral("从目录创建数据集")));
    dlg.setModal(true);
    dlg.resize(900, 760);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QWidget* formWidget = new QWidget(&dlg);
    QGridLayout* form = new QGridLayout(formWidget);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    int row = 0;

    QLabel* pathLabel = new QLabel(tr3(QStringLiteral("Path"), QStringLiteral("Path"), QStringLiteral("路径")), formWidget);
    QLineEdit* pathEdit = new QLineEdit(formWidget);
    pathEdit->setText(ds + QStringLiteral("/new_dataset"));
    form->addWidget(pathLabel, row, 0);
    form->addWidget(pathEdit, row, 1, 1, 3);
    row++;

    QLabel* typeLabel = new QLabel(tr3(QStringLiteral("Tipo"), QStringLiteral("Type"), QStringLiteral("类型")), formWidget);
    QComboBox* typeCombo = new QComboBox(formWidget);
    typeCombo->addItem(QStringLiteral("filesystem"), QStringLiteral("filesystem"));
    typeCombo->setCurrentIndex(0);
    typeCombo->setEnabled(false);
    form->addWidget(typeLabel, row, 0);
    form->addWidget(typeCombo, row, 1);
    row++;

    QLabel* mountDirLabel = new QLabel(tr3(QStringLiteral("Directorio local"),
                                           QStringLiteral("Local directory"),
                                           QStringLiteral("本地目录")),
                                       formWidget);
    QLineEdit* mountDirEdit = new QLineEdit(formWidget);
    QPushButton* browseDirBtn = new QPushButton(
        tr3(QStringLiteral("Seleccionar..."), QStringLiteral("Select..."), QStringLiteral("选择...")),
        formWidget);
    form->addWidget(mountDirLabel, row, 0);
    form->addWidget(mountDirEdit, row, 1, 1, 2);
    form->addWidget(browseDirBtn, row, 3);
    row++;
    QObject::connect(browseDirBtn, &QPushButton::clicked, &dlg, [&]() {
        if (ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
            QMessageBox::warning(
                &dlg,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("Conexión inválida para explorar directorios remotos."),
                    QStringLiteral("Invalid connection for remote directory browsing."),
                    QStringLiteral("用于远程目录浏览的连接无效。")));
            return;
        }
        const ConnectionProfile& prof = m_profiles[ctx.connIdx];
        const bool isWinRemote = isWindowsConnection(ctx.connIdx);

        auto parentPath = [&](const QString& current) -> QString {
            if (isWinRemote) {
                QString p = current.trimmed();
                if (p.isEmpty()) {
                    return QStringLiteral("C:\\");
                }
                p.replace('/', '\\');
                const QRegularExpression rootRx(QStringLiteral("^[A-Za-z]:\\\\?$"));
                if (rootRx.match(p).hasMatch()) {
                    return p.endsWith('\\') ? p : (p + QStringLiteral("\\"));
                }
                const int pos = p.lastIndexOf('\\');
                if (pos <= 2) {
                    return (p.size() >= 2) ? (p.left(2) + QStringLiteral("\\")) : QStringLiteral("C:\\");
                }
                return p.left(pos);
            }
            const QString p = current.trimmed();
            if (p.isEmpty() || p == QStringLiteral("/")) {
                return QStringLiteral("/");
            }
            const int pos = p.lastIndexOf('/');
            if (pos <= 0) {
                return QStringLiteral("/");
            }
            return p.left(pos);
        };

        auto joinPath = [&](const QString& base, const QString& name) -> QString {
            if (isWinRemote) {
                QString b = base.trimmed();
                if (b.isEmpty()) {
                    b = QStringLiteral("C:\\");
                }
                b.replace('/', '\\');
                return b.endsWith('\\') ? (b + name) : (b + QStringLiteral("\\") + name);
            }
            QString b = base.trimmed();
            if (b.isEmpty()) {
                b = QStringLiteral("/");
            }
            if (b == QStringLiteral("/")) {
                return b + name;
            }
            return b.endsWith('/') ? (b + name) : (b + QStringLiteral("/") + name);
        };

        auto listRemoteDirs = [&](const QString& basePath, QString& resolvedPath, QStringList& children, QString& errorMsg) -> bool {
            const QString fallbackRoot = isWinRemote ? QStringLiteral("C:\\") : QStringLiteral("/");
            const QString requested = basePath.trimmed().isEmpty() ? fallbackRoot : basePath.trimmed();
            QString remoteCmd;
            if (isWinRemote) {
                auto psSingle = [](const QString& v) {
                    QString out = v;
                    out.replace(QStringLiteral("'"), QStringLiteral("''"));
                    return QStringLiteral("'") + out + QStringLiteral("'");
                };
                remoteCmd = QStringLiteral(
                                "$base=%1; "
                                "if (Test-Path -LiteralPath $base -PathType Container) { $p=(Resolve-Path -LiteralPath $base).Path } else { $p='C:\\' }; "
                                "Write-Output $p; "
                                "Get-ChildItem -LiteralPath $p -Directory -Name -ErrorAction SilentlyContinue | Sort-Object")
                                .arg(psSingle(requested));
            } else {
                remoteCmd = QStringLiteral(
                                "BASE=%1; "
                                "if [ -d \"$BASE\" ]; then cd \"$BASE\" 2>/dev/null || cd /; else cd /; fi; "
                                "pwd; "
                                "ls -1A 2>/dev/null | while IFS= read -r e; do [ -d \"$e\" ] && echo \"$e\"; done | sort")
                                .arg(shSingleQuote(requested));
            }
            QString out;
            QString err;
            int rc = -1;
            if (!runSsh(prof, remoteCmd, 20000, out, err, rc) || rc != 0) {
                errorMsg = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
                return false;
            }
            QStringList lines = out.split('\n', Qt::KeepEmptyParts);
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
                lines.removeLast();
            }
            if (lines.isEmpty()) {
                errorMsg = QStringLiteral("empty remote response");
                return false;
            }
            resolvedPath = lines.takeFirst().trimmed();
            children.clear();
            for (const QString& ln : lines) {
                const QString v = ln.trimmed();
                if (!v.isEmpty()) {
                    children << v;
                }
            }
            return true;
        };

        QDialog picker(&dlg);
        picker.setModal(true);
        picker.resize(640, 460);
        picker.setWindowTitle(
            tr3(QStringLiteral("Seleccionar directorio remoto"),
                QStringLiteral("Select remote directory"),
                QStringLiteral("选择远程目录")));
        QVBoxLayout* pv = new QVBoxLayout(&picker);
        pv->setContentsMargins(10, 10, 10, 10);
        pv->setSpacing(8);
        QLabel* connInfo = new QLabel(
            tr3(QStringLiteral("Conexión: %1").arg(prof.name),
                QStringLiteral("Connection: %1").arg(prof.name),
                QStringLiteral("连接：%1").arg(prof.name)),
            &picker);
        pv->addWidget(connInfo);
        QLineEdit* currentPathEdit = new QLineEdit(&picker);
        currentPathEdit->setReadOnly(true);
        pv->addWidget(currentPathEdit);
        QListWidget* dirList = new QListWidget(&picker);
        dirList->setSelectionMode(QAbstractItemView::SingleSelection);
        pv->addWidget(dirList, 1);
        QHBoxLayout* navRow = new QHBoxLayout();
        QPushButton* upBtn = new QPushButton(tr3(QStringLiteral("Subir"), QStringLiteral("Up"), QStringLiteral("上级")), &picker);
        QPushButton* refreshBtn = new QPushButton(tr3(QStringLiteral("Actualizar"), QStringLiteral("Refresh"), QStringLiteral("刷新")), &picker);
        navRow->addWidget(upBtn);
        navRow->addWidget(refreshBtn);
        navRow->addStretch(1);
        pv->addLayout(navRow);
        QDialogButtonBox* pb = new QDialogButtonBox(&picker);
        QPushButton* cancelBtn = pb->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
        QPushButton* selectBtn = pb->addButton(tr3(QStringLiteral("Seleccionar"), QStringLiteral("Select"), QStringLiteral("选择")), QDialogButtonBox::AcceptRole);
        pv->addWidget(pb);
        QObject::connect(cancelBtn, &QPushButton::clicked, &picker, &QDialog::reject);

        QString current = mountDirEdit->text().trimmed();
        if (current.isEmpty()) {
            current = isWinRemote ? QStringLiteral("C:\\") : QStringLiteral("/");
        }
        std::function<void()> reloadDir;
        reloadDir = [&]() {
            QString resolved;
            QStringList children;
            QString errMsg;
            dirList->clear();
            if (!listRemoteDirs(current, resolved, children, errMsg)) {
                QMessageBox::warning(
                    &picker,
                    QStringLiteral("ZFSMgr"),
                    tr3(QStringLiteral("No se pudo listar directorios remotos:\n%1").arg(errMsg),
                        QStringLiteral("Could not list remote directories:\n%1").arg(errMsg),
                        QStringLiteral("无法列出远程目录：\n%1").arg(errMsg)));
                return;
            }
            current = resolved;
            currentPathEdit->setText(current);
            const QString par = parentPath(current);
            if (par != current) {
                QListWidgetItem* upItem = new QListWidgetItem(QStringLiteral(".."), dirList);
                upItem->setData(Qt::UserRole, par);
            }
            for (const QString& ch : children) {
                QListWidgetItem* it = new QListWidgetItem(ch, dirList);
                it->setData(Qt::UserRole, joinPath(current, ch));
            }
            if (dirList->count() > 0) {
                dirList->setCurrentRow(0);
            }
        };
        QObject::connect(refreshBtn, &QPushButton::clicked, &picker, [&]() { reloadDir(); });
        QObject::connect(upBtn, &QPushButton::clicked, &picker, [&]() {
            current = parentPath(current);
            reloadDir();
        });
        QObject::connect(dirList, &QListWidget::itemDoubleClicked, &picker, [&](QListWidgetItem* item) {
            if (!item) {
                return;
            }
            const QString next = item->data(Qt::UserRole).toString().trimmed();
            if (next.isEmpty()) {
                return;
            }
            current = next;
            reloadDir();
        });
        QObject::connect(selectBtn, &QPushButton::clicked, &picker, [&]() {
            mountDirEdit->setText(currentPathEdit->text().trimmed());
            picker.accept();
        });
        reloadDir();
        picker.exec();
    });

    QLabel* blocksizeLabel = new QLabel(tr3(QStringLiteral("Blocksize"), QStringLiteral("Blocksize"), QStringLiteral("块大小")), formWidget);
    QLineEdit* blocksizeEdit = new QLineEdit(formWidget);
    form->addWidget(blocksizeLabel, row, 0);
    form->addWidget(blocksizeEdit, row, 1);
    row++;

    QWidget* optsWidget = new QWidget(formWidget);
    QHBoxLayout* optsLay = new QHBoxLayout(optsWidget);
    optsLay->setContentsMargins(0, 0, 0, 0);
    optsLay->setSpacing(12);
    QCheckBox* parentsChk = new QCheckBox(tr3(QStringLiteral("Crear padres (-p)"), QStringLiteral("Create parents (-p)"), QStringLiteral("创建父级(-p)")), optsWidget);
    parentsChk->setChecked(true);
    optsLay->addWidget(parentsChk);
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(tr3(QStringLiteral("Argumentos extra"), QStringLiteral("Extra args"), QStringLiteral("额外参数")), formWidget);
    QLineEdit* extraEdit = new QLineEdit(formWidget);
    form->addWidget(extraLabel, row, 0);
    form->addWidget(extraEdit, row, 1, 1, 3);
    row++;

    root->addWidget(formWidget);

    QGroupBox* propsGroup = new QGroupBox(tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")), &dlg);
    QVBoxLayout* propsGroupLay = new QVBoxLayout(propsGroup);
    propsGroupLay->setContentsMargins(6, 6, 6, 6);
    propsGroupLay->setSpacing(4);

    QScrollArea* propsScroll = new QScrollArea(propsGroup);
    propsScroll->setWidgetResizable(true);
    QWidget* propsContainer = new QWidget(propsScroll);
    QGridLayout* propsGrid = new QGridLayout(propsContainer);
    propsGrid->setHorizontalSpacing(8);
    propsGrid->setVerticalSpacing(4);

    const QList<PropSpec> propSpecs = {
        {QStringLiteral("compression"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("lz4"), QStringLiteral("gzip"), QStringLiteral("zstd"), QStringLiteral("zle")}},
        {QStringLiteral("atime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("xattr"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa")}},
        {QStringLiteral("acltype"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), QStringLiteral("combo"), {QString(), QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("recordsize"), QStringLiteral("entry"), {}},
        {QStringLiteral("quota"), QStringLiteral("entry"), {}},
        {QStringLiteral("reservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("refquota"), QStringLiteral("entry"), {}},
        {QStringLiteral("refreservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("copies"), QStringLiteral("combo"), {QString(), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}},
        {QStringLiteral("checksum"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("fletcher2"), QStringLiteral("fletcher4"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr")}},
        {QStringLiteral("sync"), QStringLiteral("combo"), {QString(), QStringLiteral("standard"), QStringLiteral("always"), QStringLiteral("disabled")}},
        {QStringLiteral("logbias"), QStringLiteral("combo"), {QString(), QStringLiteral("latency"), QStringLiteral("throughput")}},
        {QStringLiteral("primarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("secondarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("dedup"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("verify"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein")}},
        {QStringLiteral("normalization"), QStringLiteral("combo"), {QString(), QStringLiteral("none"), QStringLiteral("formC"), QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")}},
        {QStringLiteral("casesensitivity"), QStringLiteral("combo"), {QString(), QStringLiteral("sensitive"), QStringLiteral("insensitive"), QStringLiteral("mixed")}},
        {QStringLiteral("utf8only"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
    };

    QList<PropEditor> propEditors;
    propEditors.reserve(propSpecs.size());
    for (int i = 0; i < propSpecs.size(); ++i) {
        const PropSpec& spec = propSpecs[i];
        const int r = i / 2;
        const int cBase = (i % 2) * 2;
        QLabel* lbl = new QLabel(spec.name, propsContainer);
        propsGrid->addWidget(lbl, r, cBase);
        PropEditor editor;
        editor.name = spec.name;
        if (spec.kind == QStringLiteral("combo")) {
            QComboBox* cb = new QComboBox(propsContainer);
            cb->addItems(spec.values);
            if (spec.name == QStringLiteral("compression")) {
                // Permite niveles, por ejemplo: zstd-19, gzip-9.
                cb->setEditable(true);
                cb->setInsertPolicy(QComboBox::NoInsert);
            }
            editor.combo = cb;
            propsGrid->addWidget(cb, r, cBase + 1);
        } else {
            QLineEdit* le = new QLineEdit(propsContainer);
            editor.edit = le;
            propsGrid->addWidget(le, r, cBase + 1);
        }
        propEditors.push_back(editor);
    }
    propsContainer->setLayout(propsGrid);
    propsScroll->setWidget(propsContainer);
    propsGroupLay->addWidget(propsScroll);
    root->addWidget(propsGroup, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        tr3(QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")),
        QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    bool accepted = false;
    CreateDatasetOptions opt;
    QString selectedMountDir;
    QObject::connect(createBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString path = pathEdit->text().trimmed();
        const QString mountDir = mountDirEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe indicar el path del dataset."),
                                     QStringLiteral("Dataset path is required."),
                                     QStringLiteral("必须指定数据集路径。")));
            return;
        }
        if (mountDir.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un directorio local."),
                                     QStringLiteral("You must select a local directory."),
                                     QStringLiteral("必须选择本地目录。")));
            return;
        }

        QStringList properties;
        for (const PropEditor& pe : propEditors) {
            QString v;
            if (pe.combo) {
                v = pe.combo->currentText().trimmed();
            } else if (pe.edit) {
                v = pe.edit->text().trimmed();
            }
            if (!v.isEmpty()) {
                properties.push_back(pe.name + QStringLiteral("=") + v);
            }
        }

        opt.datasetPath = path;
        opt.dsType = QStringLiteral("filesystem");
        opt.volsize.clear();
        opt.blocksize = blocksizeEdit->text().trimmed();
        opt.parents = parentsChk->isChecked();
        opt.sparse = false;
        opt.nomount = false;
        opt.snapshotRecursive = false;
        opt.properties = properties;
        opt.extraArgs = extraEdit->text().trimmed();
        selectedMountDir = mountDir;
        accepted = true;
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted || !accepted) {
        return;
    }

    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString createCmd = buildZfsCreateCmd(opt);
    QString cmd;
    bool allowWindowsScript = false;
    if (!isWin) {
        cmd = QStringLiteral(
                  "set -e; "
                  "DATASET=%1; "
                  "SRC_DIR=%2; "
                  "RSYNC_OPTS='-aHWS'; "
                  "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                  "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                  "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                  "TMP_MP=$(mktemp -d /tmp/zfsmgr-fromdir-mp-XXXXXX); "
                  "BACKUP_DIR=''; "
                  "cleanup(){ "
                  "  rc=$?; "
                  "  if [ $rc -ne 0 ]; then "
                  "    if [ -n \"$BACKUP_DIR\" ] && [ ! -e \"$SRC_DIR\" ] && [ -d \"$BACKUP_DIR\" ]; then mv \"$BACKUP_DIR\" \"$SRC_DIR\" || true; fi; "
                  "  fi; "
                  "  rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; "
                  "  exit $rc; "
                  "}; "
                  "trap cleanup EXIT INT TERM; "
                  "[ -d \"$SRC_DIR\" ] || { echo 'source directory does not exist'; exit 2; }; "
                  "if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx \"$SRC_DIR\" >/dev/null 2>&1; then "
                  "  echo 'mountpoint already in use'; exit 3; "
                  "fi; "
                  "%3; "
                  "zfs set canmount=on \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$TMP_MP\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "ACTIVE_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "[ \"$ACTIVE_MP\" = \"$TMP_MP\" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 4; }; "
                  "rsync $RSYNC_OPTS \"$SRC_DIR\"/ \"$TMP_MP\"/; "
                  "BACKUP_DIR=\"$SRC_DIR.zfsmgr-bak-$$\"; "
                  "i=0; "
                  "while [ -e \"$BACKUP_DIR\" ]; do i=$((i+1)); BACKUP_DIR=\"$SRC_DIR.zfsmgr-bak-$$-$i\"; done; "
                  "mv \"$SRC_DIR\" \"$BACKUP_DIR\"; "
                  "mkdir -p \"$SRC_DIR\"; "
                  "zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$SRC_DIR\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "FINAL_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "if [ \"$FINAL_MP\" != \"$SRC_DIR\" ]; then "
                  "  zfs set mountpoint=\"$TMP_MP\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  rm -rf \"$SRC_DIR\"; "
                  "  mv \"$BACKUP_DIR\" \"$SRC_DIR\"; "
                  "  echo 'failed to switch mountpoint to destination directory'; "
                  "  exit 5; "
                  "fi; "
                  "rm -rf \"$BACKUP_DIR\"; "
                  "BACKUP_DIR=''; "
                  "trap - EXIT INT TERM; "
                  "rmdir \"$TMP_MP\" >/dev/null 2>&1 || true")
                  .arg(shSingleQuote(opt.datasetPath),
                       shSingleQuote(selectedMountDir),
                       createCmd);
    } else {
        allowWindowsScript = true;
        auto psSingle = [](const QString& v) {
            QString out = v;
            out.replace(QStringLiteral("'"), QStringLiteral("''"));
            return QStringLiteral("'") + out + QStringLiteral("'");
        };
        cmd = QStringLiteral(
                  "$ErrorActionPreference = 'Stop'; "
                  "$dataset = %1; "
                  "$srcDir = %2; "
                  "if (-not (Test-Path -LiteralPath $srcDir -PathType Container)) { throw 'source directory does not exist'; } "
                  "$srcDir = (Resolve-Path -LiteralPath $srcDir).Path; "
                  "%3; "
                  "zfs set canmount=on $dataset 2>$null | Out-Null; "
                  "zfs mount $dataset 2>$null | Out-Null; "
                  "$activeMp = ''; "
                  "foreach ($line in @(zfs mount 2>$null)) { "
                  "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "    if ($Matches[1] -eq $dataset) { $activeMp = $Matches[2].Trim(); break; } "
                  "  } "
                  "} "
                  "if ([string]::IsNullOrWhiteSpace($activeMp)) { throw 'could not resolve effective mountpoint'; } "
                  "if ([string]::Equals($activeMp, $srcDir, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'mountpoint already in use'; } "
                  "$null = robocopy $srcDir $activeMp /E /COPYALL /R:1 /W:1 /NFL /NDL /NP; "
                  "if ($LASTEXITCODE -ge 8) { throw ('robocopy failed with code ' + $LASTEXITCODE); } "
                  "Write-Output ('[FROMDIR] copied to effective mountpoint: ' + $activeMp)")
                  .arg(psSingle(opt.datasetPath),
                       psSingle(selectedMountDir),
                       createCmd);
    }
    executeDatasetAction(QStringLiteral("advanced"),
                         tr3(QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")),
                         ctx,
                         cmd,
                         90000,
                         allowWindowsScript);
}

void MainWindow::actionAdvancedToDir() {
    if (actionsLocked()) {
        return;
    }
    const auto selected = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString().trimmed();
    const QString snap = selected.first()->data(1, Qt::UserRole).toString().trimmed();
    if (ds.isEmpty() || !snap.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un dataset (no snapshot)."),
                                     QStringLiteral("You must select a dataset (not a snapshot)."),
                                     QStringLiteral("必须选择数据集（不能是快照）。")));
        return;
    }

    const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = token.left(sep).toInt();
    ctx.poolName = token.mid(sep + 2);
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    QDialog dlg(this);
    dlg.setWindowTitle(tr3(QStringLiteral("Exportar dataset a directorio"),
                           QStringLiteral("Export dataset to directory"),
                           QStringLiteral("导出数据集到目录")));
    dlg.setModal(true);
    dlg.resize(720, 180);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QLabel* intro = new QLabel(
        tr3(QStringLiteral("Se copiará el contenido de %1 a un directorio local y luego se destruirá el dataset.")
                .arg(ds),
            QStringLiteral("Contents of %1 will be copied to a local directory and dataset will then be destroyed.")
                .arg(ds),
            QStringLiteral("将把 %1 的内容复制到本地目录，随后销毁该数据集。")
                .arg(ds)),
        &dlg);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QHBoxLayout* dirRow = new QHBoxLayout();
    QLabel* dirLabel = new QLabel(tr3(QStringLiteral("Directorio local"),
                                      QStringLiteral("Local directory"),
                                      QStringLiteral("本地目录")),
                                  &dlg);
    QLineEdit* dirEdit = new QLineEdit(&dlg);
    QPushButton* browseBtn = new QPushButton(
        tr3(QStringLiteral("Seleccionar..."), QStringLiteral("Select..."), QStringLiteral("选择...")),
        &dlg);
    dirRow->addWidget(dirLabel, 0);
    dirRow->addWidget(dirEdit, 1);
    dirRow->addWidget(browseBtn, 0);
    root->addLayout(dirRow);

    QObject::connect(browseBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString picked = QFileDialog::getExistingDirectory(
            &dlg,
            tr3(QStringLiteral("Seleccionar directorio local"),
                QStringLiteral("Select local directory"),
                QStringLiteral("选择本地目录")),
            dirEdit->text().trimmed());
        if (!picked.trimmed().isEmpty()) {
            dirEdit->setText(picked);
        }
    });

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* acceptBtn = buttons->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(acceptBtn, &QPushButton::clicked, &dlg, [&]() {
        if (dirEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe seleccionar un directorio local."),
                                     QStringLiteral("You must select a local directory."),
                                     QStringLiteral("必须选择本地目录。")));
            return;
        }
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QString localDir = dirEdit->text().trimmed();

    const bool isWin = isWindowsConnection(ctx.connIdx);
    QString cmd;
    bool allowWindowsScript = false;
    if (!isWin) {
        cmd = QStringLiteral(
                  "set -e; "
                  "DATASET=%1; "
                  "DST_DIR=%2; "
                  "RSYNC_OPTS='-aHWS'; "
                  "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                  "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                  "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                  "TMP_MP=$(mktemp -d /tmp/zfsmgr-todir-mp-XXXXXX); "
                  "TMP_OUT=$(mktemp -d /tmp/zfsmgr-todir-out-XXXXXX); "
                  "BACKUP_DIR=''; RESTORE_NEEDED=0; "
                  "OLD_MP=$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true); "
                  "OLD_MOUNTED=$(zfs get -H -o value mounted \"$DATASET\" 2>/dev/null || true); "
                  "cleanup(){ "
                  "  rc=$?; "
                  "  if [ $rc -ne 0 ]; then "
                  "    if [ \"$RESTORE_NEEDED\" = \"1\" ] && [ -n \"$BACKUP_DIR\" ] && [ -d \"$BACKUP_DIR\" ]; then "
                  "      rm -rf \"$DST_DIR\" >/dev/null 2>&1 || true; "
                  "      mv \"$BACKUP_DIR\" \"$DST_DIR\" >/dev/null 2>&1 || true; "
                  "    fi; "
                  "    if zfs list -H -o name \"$DATASET\" >/dev/null 2>&1; then "
                  "      zfs set mountpoint=\"$OLD_MP\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "      if [ \"$OLD_MOUNTED\" = \"yes\" ] || [ \"$OLD_MOUNTED\" = \"on\" ]; then zfs mount \"$DATASET\" >/dev/null 2>&1 || true; fi; "
                  "    fi; "
                  "  fi; "
                  "  rm -rf \"$TMP_MP\" >/dev/null 2>&1 || true; "
                  "  rm -rf \"$TMP_OUT\" >/dev/null 2>&1 || true; "
                  "  exit $rc; "
                  "}; "
                  "trap cleanup EXIT INT TERM; "
                  "if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx \"$DST_DIR\" >/dev/null 2>&1; then "
                  "  echo 'destination directory is already a zfs mountpoint'; exit 2; "
                  "fi; "
                  "zfs set canmount=on \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$TMP_MP\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "ACTIVE_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "[ \"$ACTIVE_MP\" = \"$TMP_MP\" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 3; }; "
                  "rsync $RSYNC_OPTS \"$TMP_MP\"/ \"$TMP_OUT\"/; "
                  "if [ -e \"$DST_DIR\" ]; then "
                  "  BACKUP_DIR=\"$DST_DIR.zfsmgr-bak-$$\"; "
                  "  i=0; while [ -e \"$BACKUP_DIR\" ]; do i=$((i+1)); BACKUP_DIR=\"$DST_DIR.zfsmgr-bak-$$-$i\"; done; "
                  "  mv \"$DST_DIR\" \"$BACKUP_DIR\"; "
                  "  RESTORE_NEEDED=1; "
                  "else "
                  "  mkdir -p \"$(dirname \"$DST_DIR\")\"; "
                  "fi; "
                  "mv \"$TMP_OUT\" \"$DST_DIR\"; "
                  "zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs destroy -r \"$DATASET\"; "
                  "if [ -n \"$BACKUP_DIR\" ]; then rm -rf \"$BACKUP_DIR\"; fi; "
                  "RESTORE_NEEDED=0; "
                  "trap - EXIT INT TERM; "
                  "rm -rf \"$TMP_MP\" >/dev/null 2>&1 || true")
                  .arg(shSingleQuote(ds),
                       shSingleQuote(localDir));
    } else {
        allowWindowsScript = true;
        auto psSingle = [](const QString& v) {
            QString out = v;
            out.replace(QStringLiteral("'"), QStringLiteral("''"));
            return QStringLiteral("'") + out + QStringLiteral("'");
        };
        cmd = QStringLiteral(
                  "$ErrorActionPreference = 'Stop'; "
                  "$dataset = %1; "
                  "$dstDir = %2; "
                  "$dstParent = Split-Path -Parent $dstDir; "
                  "if ([string]::IsNullOrWhiteSpace($dstParent)) { throw 'invalid destination directory'; } "
                  "if (-not (Test-Path -LiteralPath $dstParent)) { New-Item -ItemType Directory -Force -Path $dstParent | Out-Null; } "
                  "$mountRows = @(zfs mount 2>$null); "
                  "$used = $false; "
                  "foreach ($line in $mountRows) { "
                  "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "    $mp = $Matches[2].Trim(); "
                  "    if ([string]::Equals($mp, $dstDir, [System.StringComparison]::OrdinalIgnoreCase)) { $used = $true; break; } "
                  "  } "
                  "} "
                  "if ($used) { throw 'destination directory is already a zfs mountpoint'; } "
                  "$oldMp = (zfs get -H -o value mountpoint $dataset 2>$null | Select-Object -First 1); "
                  "$oldMounted = (zfs get -H -o value mounted $dataset 2>$null | Select-Object -First 1); "
                  "$tmpMp = Join-Path $env:TEMP ('zfsmgr-todir-mp-' + [Guid]::NewGuid().ToString('N')); "
                  "$tmpOut = Join-Path $env:TEMP ('zfsmgr-todir-out-' + [Guid]::NewGuid().ToString('N')); "
                  "New-Item -ItemType Directory -Force -Path $tmpMp | Out-Null; "
                  "New-Item -ItemType Directory -Force -Path $tmpOut | Out-Null; "
                  "$backupDir = ''; "
                  "$restoreNeeded = $false; "
                  "try { "
                  "  zfs set canmount=on $dataset 2>$null | Out-Null; "
                  "  zfs set mountpoint=$tmpMp $dataset | Out-Null; "
                  "  zfs mount $dataset 2>$null | Out-Null; "
                  "  $activeMp = ''; "
                  "  foreach ($line in @(zfs mount 2>$null)) { "
                  "    if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "      if ($Matches[1] -eq $dataset) { $activeMp = $Matches[2].Trim(); break; } "
                  "    } "
                  "  } "
                  "  if (-not [string]::Equals($activeMp, $tmpMp, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'could not mount dataset on temporary mountpoint'; } "
                  "  $rc = (robocopy $tmpMp $tmpOut /E /COPYALL /R:1 /W:1 /NFL /NDL /NP); "
                  "  if ($LASTEXITCODE -ge 8) { throw ('robocopy failed with code ' + $LASTEXITCODE); } "
                  "  if (Test-Path -LiteralPath $dstDir) { "
                  "    $backupDir = $dstDir + '.zfsmgr-bak-' + [Guid]::NewGuid().ToString('N'); "
                  "    Move-Item -LiteralPath $dstDir -Destination $backupDir; "
                  "    $restoreNeeded = $true; "
                  "  } "
                  "  Move-Item -LiteralPath $tmpOut -Destination $dstDir; "
                  "  zfs unmount $dataset 2>$null | Out-Null; "
                  "  zfs destroy -r $dataset | Out-Null; "
                  "  if ($backupDir -and (Test-Path -LiteralPath $backupDir)) { Remove-Item -LiteralPath $backupDir -Recurse -Force; } "
                  "  $restoreNeeded = $false; "
                  "} catch { "
                  "  if ($restoreNeeded -and $backupDir -and (Test-Path -LiteralPath $backupDir)) { "
                  "    if (Test-Path -LiteralPath $dstDir) { Remove-Item -LiteralPath $dstDir -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "    Move-Item -LiteralPath $backupDir -Destination $dstDir -ErrorAction SilentlyContinue; "
                  "  } "
                  "  if (zfs list -H -o name $dataset 2>$null) { "
                  "    if ($oldMp) { zfs set mountpoint=$oldMp $dataset 2>$null | Out-Null; } "
                  "    if ($oldMounted -match '^(yes|on)$') { zfs mount $dataset 2>$null | Out-Null; } "
                  "  } "
                  "  throw; "
                  "} finally { "
                  "  if (Test-Path -LiteralPath $tmpMp) { Remove-Item -LiteralPath $tmpMp -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "  if (Test-Path -LiteralPath $tmpOut) { Remove-Item -LiteralPath $tmpOut -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "}")
                  .arg(psSingle(ds),
                       psSingle(localDir));
    }

    executeDatasetAction(QStringLiteral("advanced"),
                         tr3(QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")),
                         ctx,
                         cmd,
                         0,
                         allowWindowsScript);
}

void MainWindow::onDatasetPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || (col != 1 && col != 2)) {
        return;
    }
    QTableWidgetItem* pk = m_datasetPropsTable->item(row, 0);
    QTableWidgetItem* pv = m_datasetPropsTable->item(row, 1);
    QTableWidgetItem* pi = m_datasetPropsTable->item(row, 2);
    if (!pk || !pv || !pi) {
        return;
    }
    Q_UNUSED(pk);
    Q_UNUSED(pv);
    Q_UNUSED(pi);
    m_propsDirty = false;
    for (int r = 0; r < m_datasetPropsTable->rowCount(); ++r) {
        QTableWidgetItem* rk = m_datasetPropsTable->item(r, 0);
        QTableWidgetItem* rv = m_datasetPropsTable->item(r, 1);
        QTableWidgetItem* ri = m_datasetPropsTable->item(r, 2);
        if (!rk || !rv || !ri) {
            continue;
        }
        const QString key = rk->text().trimmed();
        const bool inh = (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
        if (inh != m_propsOriginalInherit.value(key, false)
            || rv->text() != m_propsOriginalValues.value(key)) {
            m_propsDirty = true;
            break;
        }
    }
    updateApplyPropsButtonState();
}

void MainWindow::onAdvancedPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || (col != 1 && col != 2) || !m_advPropsTable) {
        return;
    }
    Q_UNUSED(row);
    m_advPropsDirty = false;
    for (int r = 0; r < m_advPropsTable->rowCount(); ++r) {
        QTableWidgetItem* rk = m_advPropsTable->item(r, 0);
        QTableWidgetItem* rv = m_advPropsTable->item(r, 1);
        QTableWidgetItem* ri = m_advPropsTable->item(r, 2);
        if (!rk || !rv || !ri) {
            continue;
        }
        const QString key = rk->text().trimmed();
        const bool inh = (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
        if (inh != m_advPropsOriginalInherit.value(key, false)
            || rv->text() != m_advPropsOriginalValues.value(key)) {
            m_advPropsDirty = true;
            break;
        }
    }
    updateApplyPropsButtonState();
}

void MainWindow::applyDatasetPropertyChanges() {
    if (actionsLocked()) {
        return;
    }
    if (!m_propsDirty || m_propsDataset.isEmpty() || m_propsSide.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    if (!ctx.valid || ctx.datasetName != m_propsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset activo para aplicar cambios."));
        return;
    }

    QStringList subcmds;
    for (int r = 0; r < m_datasetPropsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = m_datasetPropsTable->item(r, 0);
        QTableWidgetItem* pv = m_datasetPropsTable->item(r, 1);
        QTableWidgetItem* pi = m_datasetPropsTable->item(r, 2);
        if (!pk || !pv || !pi) {
            continue;
        }
        const QString prop = pk->text().trimmed();
        if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado") || prop == QStringLiteral("Tamaño")) {
            continue;
        }
        const bool inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
        if (inheritChecked) {
            subcmds << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(prop), shSingleQuote(ctx.datasetName));
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_propsOriginalValues.value(prop).trimmed();
        if (now == old) {
            continue;
        }
        const QString assign = prop + QStringLiteral("=") + now;
        subcmds << QStringLiteral("zfs set %1 %2").arg(shSingleQuote(assign), shSingleQuote(ctx.datasetName));
    }
    if (subcmds.isEmpty()) {
        m_propsDirty = false;
        updateApplyPropsButtonState();
        return;
    }

    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString cmd = isWin ? subcmds.join(QStringLiteral("; "))
                              : QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
    if (executeDatasetAction(m_propsSide, QStringLiteral("Aplicar propiedades"), ctx, cmd, 60000, isWin)) {
        m_propsDirty = false;
        updateApplyPropsButtonState();
    }
}

void MainWindow::applyAdvancedDatasetPropertyChanges() {
    if (actionsLocked()) {
        return;
    }
    if (!m_advPropsDirty || m_advPropsDataset.isEmpty() || !m_advPropsTable) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("advanced"));
    if (!ctx.valid || ctx.datasetName != m_advPropsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset activo para aplicar cambios."));
        return;
    }

    QStringList subcmds;
    for (int r = 0; r < m_advPropsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = m_advPropsTable->item(r, 0);
        QTableWidgetItem* pv = m_advPropsTable->item(r, 1);
        QTableWidgetItem* pi = m_advPropsTable->item(r, 2);
        if (!pk || !pv || !pi) {
            continue;
        }
        const QString prop = pk->text().trimmed();
        if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado") || prop == QStringLiteral("Tamaño")) {
            continue;
        }
        const bool inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
        if (inheritChecked) {
            subcmds << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(prop), shSingleQuote(ctx.datasetName));
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_advPropsOriginalValues.value(prop).trimmed();
        if (now == old) {
            continue;
        }
        const QString assign = prop + QStringLiteral("=") + now;
        subcmds << QStringLiteral("zfs set %1 %2").arg(shSingleQuote(assign), shSingleQuote(ctx.datasetName));
    }
    if (subcmds.isEmpty()) {
        m_advPropsDirty = false;
        updateApplyPropsButtonState();
        return;
    }

    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString cmd = isWin ? subcmds.join(QStringLiteral("; "))
                              : QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
    if (executeDatasetAction(QStringLiteral("advanced"), QStringLiteral("Aplicar propiedades"), ctx, cmd, 60000, isWin)) {
        m_advPropsDirty = false;
        updateApplyPropsButtonState();
    }
}

void MainWindow::updateApplyPropsButtonState() {
    const DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    const bool eligible = ctx.valid && ctx.snapshotName.isEmpty() && (ctx.datasetName == m_propsDataset);
    auto hasEffectiveChanges = [](QTableWidget* table,
                                  const QMap<QString, QString>& originals,
                                  const QMap<QString, bool>& originalInherit) -> bool {
        if (!table) {
            return false;
        }
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem* pk = table->item(r, 0);
            QTableWidgetItem* pv = table->item(r, 1);
            QTableWidgetItem* pi = table->item(r, 2);
            if (!pk || !pv || !pi) {
                continue;
            }
            const QString prop = pk->text().trimmed();
            if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado")) {
                continue;
            }
            const bool inh = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
            const QString now = pv->text();
            if (inh != originalInherit.value(prop, false) || now != originals.value(prop)) {
                return true;
            }
        }
        return false;
    };
    const bool hasChanges = hasEffectiveChanges(m_datasetPropsTable, m_propsOriginalValues, m_propsOriginalInherit);
    m_btnApplyDatasetProps->setEnabled(m_propsDirty && eligible && hasChanges);
    const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("advanced"));
    const bool aok = actx.valid && actx.snapshotName.isEmpty() && (actx.datasetName == m_advPropsDataset);
    if (m_btnApplyAdvancedProps) {
        const bool advHasChanges =
            hasEffectiveChanges(m_advPropsTable, m_advPropsOriginalValues, m_advPropsOriginalInherit);
        m_btnApplyAdvancedProps->setEnabled(m_advPropsDirty && aok && advHasChanges);
    }
}

void MainWindow::initLogPersistence() {
    const QString dir = m_store.configDir();
    if (dir.isEmpty()) {
        return;
    }
    m_appLogPath = dir + "/application.log";
    rotateLogIfNeeded();
}

void MainWindow::rotateLogIfNeeded() {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    const qint64 maxBytes = qint64(qMax(1, m_logMaxSizeMb)) * 1024LL * 1024LL;
    constexpr int backups = 5;

    QFileInfo fi(m_appLogPath);
    if (!fi.exists() || fi.size() < maxBytes) {
        return;
    }

    for (int i = backups; i >= 1; --i) {
        const QString src = (i == 1) ? m_appLogPath : (m_appLogPath + "." + QString::number(i - 1));
        const QString dst = m_appLogPath + "." + QString::number(i);
        if (QFile::exists(dst)) {
            QFile::remove(dst);
        }
        if (QFile::exists(src)) {
            QFile::rename(src, dst);
        }
    }
}

void MainWindow::appendLogToFile(const QString& line) {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    rotateLogIfNeeded();
    QFile f(m_appLogPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << maskSecrets(line) << '\n';
    ts.flush();
}

void MainWindow::clearAppLog() {
    m_logView->clear();
    for (auto it = m_connectionLogViews.begin(); it != m_connectionLogViews.end(); ++it) {
        if (it.value()) {
            it.value()->clear();
        }
    }
    if (m_lastDetailText) {
        m_lastDetailText->clear();
    }
    if (!m_appLogPath.isEmpty()) {
        QFile f(m_appLogPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.close();
        }
    }
}

void MainWindow::copyAppLogToClipboard() {
    QClipboard* cb = QApplication::clipboard();
    if (!cb) {
        return;
    }
    QString text = QStringLiteral("[Aplicación]\n") + m_logView->toPlainText();
    for (auto it = m_connectionLogViews.constBegin(); it != m_connectionLogViews.constEnd(); ++it) {
        const QString connId = it.key();
        const QPlainTextEdit* view = it.value();
        QString connName = connId;
        for (const auto& p : m_profiles) {
            if (p.id == connId) {
                connName = p.name;
                break;
            }
        }
        text += QStringLiteral("\n\n[%1]\n%2").arg(connName, view ? view->toPlainText() : QString());
    }
    cb->setText(text);
}

int MainWindow::findConnectionIndexByName(const QString& name) const {
    const QString key = name.trimmed();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].name.compare(key, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

int MainWindow::selectedConnectionIndexForPoolManagement() const {
    if (m_importedPoolsTable) {
        const auto sel = m_importedPoolsTable->selectedItems();
        if (!sel.isEmpty()) {
            const int row = sel.first()->row();
            if (QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0)) {
                const int idx = findConnectionIndexByName(connItem->text().trimmed());
                if (idx >= 0 && idx < m_profiles.size()) {
                    return idx;
                }
            }
        }
    }
    if (m_connectionsList) {
        const auto selected = m_connectionsList->selectedItems();
        if (!selected.isEmpty()) {
            QTreeWidgetItem* item = selected.first();
            while (item && item->parent()) {
                item = item->parent();
            }
            if (item) {
                const int idx = item->data(0, Qt::UserRole).toInt();
                if (idx >= 0 && idx < m_profiles.size()) {
                    return idx;
                }
            }
        }
    }
    return -1;
}

void MainWindow::updatePoolManagementBoxTitle() {
    if (!m_poolMgmtBox) {
        return;
    }
    const int idx = selectedConnectionIndexForPoolManagement();
    const QString connText = (idx >= 0 && idx < m_profiles.size())
                                 ? m_profiles[idx].name
                                 : tr3(QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]"));
    m_poolMgmtBox->setTitle(
        tr3(QStringLiteral("Gestión de Pools de %1"),
            QStringLiteral("Pool Management of %1"),
            QStringLiteral("%1 的池管理"))
            .arg(connText));
    if (m_btnPoolNew) {
        m_btnPoolNew->setEnabled(!actionsLocked() && idx >= 0 && idx < m_profiles.size());
    }
}

void MainWindow::refreshConnectionByIndex(int idx) {
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    m_states[idx] = refreshConnection(m_profiles[idx]);
    rebuildConnectionList();
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
}

void MainWindow::exportPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    if (!connItem || !poolItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    const QString action = stateItem ? stateItem->data(Qt::UserRole + 1).toString().trimmed() : QString();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        QStringLiteral("Exportar pool"),
        QStringLiteral("¿Exportar pool %1 en %2?").arg(poolName, connName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, QStringLiteral("zpool export %1").arg(shSingleQuote(poolName)));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Exportar"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio exportar %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error exportando %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("Exportar falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin exportar %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    appLog(QStringLiteral("INFO"), QStringLiteral("Refrescando conexión y listado de pools tras exportar: %1").arg(connName));
    refreshConnectionByIndex(idx);
    // Refuerzo explícito del refresco visual global de pools tras mutación.
    populateAllPoolsTables();
    refreshSelectedPoolDetails();
}

void MainWindow::importPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    if (!connItem || !poolItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    const QString poolState = stateItem ? stateItem->text().trimmed().toUpper() : QString();
    const QString action = stateItem ? stateItem->data(Qt::UserRole + 1).toString().trimmed() : QString();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    if (poolState != QStringLiteral("ONLINE")) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Importar omitido %1::%2 (state=%3)")
                                      .arg(connName, poolName, poolState.isEmpty() ? QStringLiteral("UNKNOWN") : poolState));
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Importar pool: %1").arg(poolName));
    dlg.setModal(true);
    auto* lay = new QVBoxLayout(&dlg);

    auto* flagsBox = new QGroupBox(QStringLiteral("Flags"), &dlg);
    auto* flagsLay = new QGridLayout(flagsBox);
    QCheckBox* forceCb = new QCheckBox(QStringLiteral("-f force"), flagsBox);
    QCheckBox* missingLogCb = new QCheckBox(QStringLiteral("-m missing log"), flagsBox);
    QCheckBox* noMountCb = new QCheckBox(QStringLiteral("-N do not mount"), flagsBox);
    QCheckBox* rewindCb = new QCheckBox(QStringLiteral("-F rewind"), flagsBox);
    QCheckBox* dryRunCb = new QCheckBox(QStringLiteral("-n dry run"), flagsBox);
    QCheckBox* destroyedCb = new QCheckBox(QStringLiteral("-D destroyed"), flagsBox);
    QCheckBox* extremeCb = new QCheckBox(QStringLiteral("-X extreme rewind"), flagsBox);
    QCheckBox* loadKeysCb = new QCheckBox(QStringLiteral("-l load keys"), flagsBox);
    flagsLay->addWidget(forceCb, 0, 0);
    flagsLay->addWidget(missingLogCb, 0, 1);
    flagsLay->addWidget(noMountCb, 1, 0);
    flagsLay->addWidget(rewindCb, 1, 1);
    flagsLay->addWidget(dryRunCb, 2, 0);
    flagsLay->addWidget(destroyedCb, 2, 1);
    flagsLay->addWidget(extremeCb, 3, 0);
    flagsLay->addWidget(loadKeysCb, 3, 1);
    lay->addWidget(flagsBox);

    auto* fieldsBox = new QGroupBox(QStringLiteral("Valores"), &dlg);
    auto* form = new QFormLayout(fieldsBox);
    QLineEdit* cachefileEd = new QLineEdit(fieldsBox);
    QLineEdit* altrootEd = new QLineEdit(fieldsBox);
    QLineEdit* dirsEd = new QLineEdit(fieldsBox);
    QLineEdit* mntoptsEd = new QLineEdit(fieldsBox);
    QLineEdit* propsEd = new QLineEdit(fieldsBox);
    QLineEdit* txgEd = new QLineEdit(fieldsBox);
    QLineEdit* newNameEd = new QLineEdit(fieldsBox);
    QLineEdit* extraEd = new QLineEdit(fieldsBox);
    form->addRow(QStringLiteral("cachefile"), cachefileEd);
    form->addRow(QStringLiteral("altroot"), altrootEd);
    form->addRow(QStringLiteral("directories (, )"), dirsEd);
    form->addRow(QStringLiteral("mntopts"), mntoptsEd);
    form->addRow(QStringLiteral("properties (, )"), propsEd);
    form->addRow(QStringLiteral("txg"), txgEd);
    form->addRow(QStringLiteral("new name"), newNameEd);
    form->addRow(QStringLiteral("extra args"), extraEd);
    lay->addWidget(fieldsBox);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    QStringList parts;
    parts << QStringLiteral("zpool import");
    if (forceCb->isChecked()) {
        parts << QStringLiteral("-f");
    }
    if (missingLogCb->isChecked()) {
        parts << QStringLiteral("-m");
    }
    if (noMountCb->isChecked()) {
        parts << QStringLiteral("-N");
    }
    if (rewindCb->isChecked()) {
        parts << QStringLiteral("-F");
    }
    if (dryRunCb->isChecked()) {
        parts << QStringLiteral("-n");
    }
    if (destroyedCb->isChecked()) {
        parts << QStringLiteral("-D");
    }
    if (extremeCb->isChecked()) {
        parts << QStringLiteral("-X");
    }
    if (loadKeysCb->isChecked()) {
        parts << QStringLiteral("-l");
    }
    if (!cachefileEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-c") << shSingleQuote(cachefileEd->text().trimmed());
    }
    if (!altrootEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-R") << shSingleQuote(altrootEd->text().trimmed());
    }
    for (const QString& d : dirsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString dd = d.trimmed();
        if (!dd.isEmpty()) {
            parts << QStringLiteral("-d") << shSingleQuote(dd);
        }
    }
    if (!mntoptsEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-o") << shSingleQuote(mntoptsEd->text().trimmed());
    }
    for (const QString& pval : propsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString pp = pval.trimmed();
        if (!pp.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(pp);
        }
    }
    if (!txgEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-T") << shSingleQuote(txgEd->text().trimmed());
    }
    parts << shSingleQuote(poolName);
    if (!newNameEd->text().trimmed().isEmpty()) {
        parts << shSingleQuote(newNameEd->text().trimmed());
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }

    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, parts.join(' '));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Importar"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio importar %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error importando %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("Importar falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin importar %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
}

void MainWindow::scrubPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    if (!connItem || !poolItem || !stateItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    const QString poolState = stateItem->text().trimmed().toUpper();
    const QString action = stateItem->data(Qt::UserRole + 1).toString().trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (poolState != QStringLiteral("ONLINE")) {
        return;
    }
    if (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        QStringLiteral("Scrub pool"),
        QStringLiteral("¿Iniciar scrub en pool %1 de %2?").arg(poolName, connName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, QStringLiteral("zpool scrub %1").arg(shSingleQuote(poolName)));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Scrub"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio scrub %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error scrub %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              QStringLiteral("Scrub falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin scrub %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
}

void MainWindow::destroyPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    if (!connItem || !poolItem || !stateItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    const QString action = stateItem->data(Qt::UserRole + 1).toString().trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }
    const auto confirm = QMessageBox::warning(
        this,
        QStringLiteral("Destroy pool"),
        QStringLiteral("ATENCIÓN: se va a destruir el pool %1 en %2.\n¿Desea continuar?")
            .arg(poolName, connName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, QStringLiteral("zpool destroy %1").arg(shSingleQuote(poolName)));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Destroy"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio destroy %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 60000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error destroy %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              QStringLiteral("Destroy falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin destroy %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
    populateAllPoolsTables();
    refreshSelectedPoolDetails();
}

void MainWindow::createPoolForSelectedConnection() {
    if (actionsLocked()) {
        return;
    }
    const int idx = selectedConnectionIndexForPoolManagement();
    if (idx < 0 || idx >= m_profiles.size()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Seleccione una conexión para gestionar pools."),
                QStringLiteral("Select a connection to manage pools."),
                QStringLiteral("请选择一个连接来管理池。")));
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];

    struct DeviceEntry {
        QString path;
        QString resolvedPath;
        QString size;
        QString mountpoint;
        QString fsType;
        QString devType;
        bool inPool{false};
        bool mounted{false};
        bool childBusy{false};
    };

    auto runRemote = [this, &p](const QString& cmd, int timeoutMs, QString& outText) -> bool {
        QString err;
        int rc = -1;
        outText.clear();
        if (!runSsh(p, cmd, timeoutMs, outText, err, rc) || rc != 0) {
            return false;
        }
        return true;
    };

    QMap<QString, DeviceEntry> devicesByPath;
    QString out;
    QString devCmd;
    if (isWindowsConnection(p)) {
        devCmd = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "Get-Partition | "
            "ForEach-Object { "
            "  $path = ('\\\\.\\PhysicalDrive' + $_.DiskNumber + '\\\\Partition' + $_.PartitionNumber); "
            "  $size = if($_.Size){ [string]([math]::Round($_.Size/1GB,2)) + 'G' } else { '-' }; "
            "  $mp = '-'; "
            "  if ($_.DriveLetter) { $mp = ($_.DriveLetter + ':\\') } "
            "  Write-Output ($path + \"`t\" + $size + \"`t\" + $mp + \"`t\" + $path + \"`t-`tpart\") "
            "}");
    } else {
        devCmd = QStringLiteral(
            "if [ \"$(uname -s 2>/dev/null)\" = \"Darwin\" ] && [ -d /private/var/run/disk/by-id ]; then "
            "  for id in /private/var/run/disk/by-id/*; do "
            "    [ -e \"$id\" ] || continue; "
            "    real=\"$(perl -MCwd=realpath -e 'print realpath($ARGV[0])' \"$id\" 2>/dev/null)\"; "
            "    [ -n \"$real\" ] || continue; "
            "    info=\"$(diskutil info \"$real\" 2>/dev/null || true)\"; "
            "    ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Partition Type:/ {print $2; exit}')\"; "
            "    [ -n \"$ptype\" ] || ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Type \\(Bundle\\):/ {print $2; exit}')\"; "
            "    [ -n \"$ptype\" ] || ptype='-'; "
            "    mnt=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Mount Point:/ {print $2; exit}')\"; "
            "    [ -n \"$mnt\" ] || mnt='-'; "
            "    size=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Disk Size:/ {print $2; exit}')\"; "
            "    [ -n \"$size\" ] || size='-'; "
            "    printf '%s\\t%s\\t%s\\t%s\\t%s\\tpart\\n' \"$id\" \"$size\" \"$mnt\" \"$real\" \"$ptype\"; "
            "  done | awk -F '\\t' '!seen[$4]++'; "
            "elif command -v lsblk >/dev/null 2>&1; then "
            "  lsblk -fpPno NAME,SIZE,FSTYPE,MOUNTPOINTS,TYPE; "
            "elif command -v diskutil >/dev/null 2>&1; then "
            "  diskutil list | awk '"
            "    /^\\/dev\\/disk[0-9]+/ { "
            "      current=$1; sub(/:$/, \"\", current); "
            "      synth=(index(tolower($0),\"synthesized\")>0); "
            "      next; "
            "    } "
            "    { "
            "      id=$NF; "
            "      if(id ~ /^disk[0-9]+s[0-9]+$/){ "
            "        if(synth){ next; } "
            "        t=$2; "
            "        if(t==\"APFS\" && ($3==\"Volume\" || $3==\"Snapshot\")){ t=t\" \"$3 } "
            "        print \"/dev/\"id\"\\t-\\t-\\t/dev/\"id\"\\t\"t\"\\tpart\"; "
            "      } "
            "    }"
            "  '; "
            "else "
            "  for d in /dev/sd?* /dev/vd?* /dev/xvd?* /dev/nvme*n* /dev/disk?s*; do [ -e \"$d\" ] && printf \"%s\\t-\\t-\\t%s\\t-\\tpart\\n\" \"$d\" \"$d\"; done; "
            "fi");
    }
    if (runRemote(devCmd, 25000, out)) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        QSet<QString> dedupeKeys;
        auto parseKv = [](const QString& line, const QString& key) -> QString {
            const QRegularExpression rx(QStringLiteral("\\b%1=\"([^\"]*)\"").arg(QRegularExpression::escape(key)));
            const QRegularExpressionMatch m = rx.match(line);
            return m.hasMatch() ? m.captured(1) : QString();
        };
        for (const QString& line : lines) {
            QString path;
            QString size;
            QString mp;
            QString resolved;
            QString fsType;
            QString type;
            if (line.contains(QStringLiteral("NAME=\"")) && line.contains(QStringLiteral("TYPE=\""))) {
                path = parseKv(line, QStringLiteral("NAME")).trimmed();
                size = parseKv(line, QStringLiteral("SIZE")).trimmed();
                fsType = parseKv(line, QStringLiteral("FSTYPE")).trimmed();
                mp = parseKv(line, QStringLiteral("MOUNTPOINTS")).trimmed();
                type = parseKv(line, QStringLiteral("TYPE")).trimmed().toLower();
                resolved = path;
                if (type != QStringLiteral("disk") && type != QStringLiteral("part")) {
                    continue;
                }
            } else {
                const QStringList cols = line.split('\t');
                if (cols.isEmpty()) {
                    continue;
                }
                path = cols.value(0).trimmed();
                size = cols.value(1).trimmed();
                mp = cols.value(2).trimmed();
                resolved = cols.value(3).trimmed();
                fsType = cols.value(4).trimmed();
                type = cols.value(5).trimmed().toLower();
                if (type.isEmpty()) {
                    type = QStringLiteral("part");
                }
            }
            if (path.isEmpty()) {
                continue;
            }
            DeviceEntry e;
            e.path = path;
            e.resolvedPath = resolved.isEmpty() ? path : resolved;
            e.size = size.isEmpty() ? QStringLiteral("-") : size;
            e.mountpoint = mp.isEmpty() ? QStringLiteral("-") : mp;
            e.fsType = fsType.isEmpty() ? QStringLiteral("-") : fsType;
            e.devType = type.isEmpty() ? QStringLiteral("part") : type;
            if (e.fsType.compare(QStringLiteral("zfs_member"), Qt::CaseInsensitive) == 0) {
                e.inPool = true;
            }
            const QString dedupeKey = (e.resolvedPath.isEmpty() ? e.path : e.resolvedPath).trimmed();
            if (!dedupeKey.isEmpty()) {
                if (dedupeKeys.contains(dedupeKey)) {
                    continue;
                }
                dedupeKeys.insert(dedupeKey);
            }
            devicesByPath[path] = e;
        }
    }

    out.clear();
    const QString inPoolCmd = withSudo(p, QStringLiteral("zpool status -P 2>/dev/null | awk '($1 ~ /^\\//){print $1}' | sort -u"));
    if (runRemote(inPoolCmd, 20000, out)) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString path = line.trimmed();
            if (path.isEmpty()) {
                continue;
            }
            if (!devicesByPath.contains(path)) {
                DeviceEntry e;
                e.path = path;
                e.resolvedPath = path;
                e.size = QStringLiteral("-");
                e.mountpoint = QStringLiteral("-");
                e.fsType = QStringLiteral("-");
                e.devType = QStringLiteral("part");
                devicesByPath.insert(path, e);
            }
            for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
                const QString real = it.value().resolvedPath.isEmpty() ? it.value().path : it.value().resolvedPath;
                if (it.value().path == path || real == path) {
                    it.value().inPool = true;
                }
            }
        }
    }

    out.clear();
    QString mountedCmd;
    if (isWindowsConnection(p)) {
        mountedCmd = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "Get-CimInstance Win32_LogicalDisk | "
            "Where-Object { $_.DriveType -eq 3 } | "
            "ForEach-Object { Write-Output ($_.DeviceID + \"`t\" + $_.DeviceID + \"\\\\\") }");
    } else {
        mountedCmd = QStringLiteral("mount | awk '{print $1\"\\t\"$3}'");
    }
    if (runRemote(mountedCmd, 20000, out)) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList cols = line.split('\t');
            if (cols.isEmpty()) {
                continue;
            }
            const QString dev = cols.value(0).trimmed();
            const QString mp = cols.value(1).trimmed();
            if (dev.isEmpty()) {
                continue;
            }
            for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
                const QString real = it.value().resolvedPath.isEmpty() ? it.value().path : it.value().resolvedPath;
                if (it.value().path == dev || real == dev) {
                    it.value().mounted = true;
                    if (!mp.isEmpty()) {
                        it.value().mountpoint = mp;
                    }
                }
            }
        }
    }

    // Propagate partition usage to parent disk:
    // if /dev/disk0s1 or /dev/sda1 is mounted/in-pool, /dev/disk0 or /dev/sda must not appear free.
    const QStringList allPaths = devicesByPath.keys();
    for (const QString& path : allPaths) {
        const DeviceEntry child = devicesByPath.value(path);
        if (!child.mounted && !child.inPool) {
            continue;
        }
        const QString childReal = child.resolvedPath.isEmpty() ? child.path : child.resolvedPath;
        const QString parent = parentDiskDevicePath(childReal);
        if (parent.isEmpty()) {
            continue;
        }
        for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
            const QString real = it.value().resolvedPath.isEmpty() ? it.value().path : it.value().resolvedPath;
            if (it.value().path == parent || real == parent) {
                it.value().childBusy = true;
            }
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(
        tr3(QStringLiteral("Crear pool en %1"), QStringLiteral("Create pool on %1"), QStringLiteral("在 %1 创建池"))
            .arg(p.name));
    dlg.setModal(true);
    dlg.resize(1180, 760);
    auto* lay = new QVBoxLayout(&dlg);

    auto* body = new QHBoxLayout();
    body->setSpacing(8);
    auto* leftPane = new QWidget(&dlg);
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(8);

    auto* baseBox = new QGroupBox(
        tr3(QStringLiteral("Parámetros del pool"), QStringLiteral("Pool parameters"), QStringLiteral("池参数")), leftPane);
    auto* form = new QFormLayout(baseBox);
    QLineEdit* poolNameEd = new QLineEdit(baseBox);
    QComboBox* quickLayoutCb = new QComboBox(baseBox);
    quickLayoutCb->addItems({QStringLiteral("stripe"), QStringLiteral("mirror"), QStringLiteral("raidz"),
                             QStringLiteral("raidz2"), QStringLiteral("raidz3")});
    QCheckBox* forceCb = new QCheckBox(QStringLiteral("-f"), baseBox);
    QCheckBox* dryRunCb = new QCheckBox(QStringLiteral("-n"), baseBox);
    QLineEdit* mountpointEd = new QLineEdit(baseBox);
    QLineEdit* altrootEd = new QLineEdit(baseBox);
    QLineEdit* ashiftEd = new QLineEdit(baseBox);
    QLineEdit* autotrimEd = new QLineEdit(baseBox);
    QLineEdit* autoexpandEd = new QLineEdit(baseBox);
    QLineEdit* compatibilityEd = new QLineEdit(baseBox);
    QLineEdit* bootfsEd = new QLineEdit(baseBox);
    QLineEdit* poolOptsEd = new QLineEdit(baseBox);
    QLineEdit* fsPropsEd = new QLineEdit(baseBox);
    QLineEdit* extraEd = new QLineEdit(baseBox);
    form->addRow(tr3(QStringLiteral("Nombre"), QStringLiteral("Name"), QStringLiteral("名称")), poolNameEd);
    form->addRow(tr3(QStringLiteral("Tipo rápido (si no hay spec vdev)"),
                     QStringLiteral("Quick type (if no vdev spec)"),
                     QStringLiteral("快速类型（若无 vdev 规格）")),
                 quickLayoutCb);
    auto* flagsRow = new QHBoxLayout();
    flagsRow->addWidget(forceCb);
    flagsRow->addWidget(dryRunCb);
    flagsRow->addStretch(1);
    form->addRow(tr3(QStringLiteral("Flags"), QStringLiteral("Flags"), QStringLiteral("标志")), flagsRow);
    form->addRow(QStringLiteral("mountpoint (-m)"), mountpointEd);
    form->addRow(QStringLiteral("altroot (-R)"), altrootEd);
    form->addRow(QStringLiteral("ashift (-o ashift=)"), ashiftEd);
    form->addRow(QStringLiteral("autotrim (-o autotrim=)"), autotrimEd);
    form->addRow(QStringLiteral("autoexpand (-o autoexpand=)"), autoexpandEd);
    form->addRow(QStringLiteral("compatibility (-o compatibility=)"), compatibilityEd);
    form->addRow(QStringLiteral("bootfs (-o bootfs=)"), bootfsEd);
    form->addRow(QStringLiteral("-o (coma: k=v,k=v)"), poolOptsEd);
    form->addRow(QStringLiteral("-O (coma: k=v,k=v)"), fsPropsEd);
    form->addRow(QStringLiteral("extra args"), extraEd);
    leftLay->addWidget(baseBox, 0);

    auto* vdevBox =
        new QGroupBox(tr3(QStringLiteral("Constructor de VDEV"),
                          QStringLiteral("VDEV builder"),
                          QStringLiteral("VDEV 构建器")),
                      leftPane);
    auto* vdevLay = new QVBoxLayout(vdevBox);
    auto* row1 = new QHBoxLayout();
    QComboBox* vdevClassCb = new QComboBox(vdevBox);
    vdevClassCb->addItem(QStringLiteral("data"));
    vdevClassCb->addItem(QStringLiteral("special"));
    vdevClassCb->addItem(QStringLiteral("log"));
    vdevClassCb->addItem(QStringLiteral("cache"));
    vdevClassCb->addItem(QStringLiteral("dedup"));
    vdevClassCb->addItem(QStringLiteral("spare"));
    QComboBox* vdevTypeCb = new QComboBox(vdevBox);
    vdevTypeCb->addItems({QStringLiteral("stripe"), QStringLiteral("mirror"), QStringLiteral("raidz"),
                          QStringLiteral("raidz2"), QStringLiteral("raidz3")});
    auto* addVdevBtn =
        new QPushButton(tr3(QStringLiteral("Añadir con seleccionados"),
                            QStringLiteral("Add from selected"),
                            QStringLiteral("从已选添加")),
                        vdevBox);
    row1->addWidget(new QLabel(tr3(QStringLiteral("Clase"), QStringLiteral("Class"), QStringLiteral("类别")), vdevBox));
    row1->addWidget(vdevClassCb);
    row1->addWidget(new QLabel(tr3(QStringLiteral("Tipo"), QStringLiteral("Type"), QStringLiteral("类型")), vdevBox));
    row1->addWidget(vdevTypeCb);
    row1->addWidget(addVdevBtn, 1);
    vdevLay->addLayout(row1);
    auto* row2 = new QHBoxLayout();
    auto* clearSelBtn = new QPushButton(tr3(QStringLiteral("Limpiar selección dispositivos"),
                                            QStringLiteral("Clear device selection"),
                                            QStringLiteral("清除设备选择")),
                                        vdevBox);
    auto* clearSpecBtn =
        new QPushButton(tr3(QStringLiteral("Limpiar spec"), QStringLiteral("Clear spec"), QStringLiteral("清除规格")), vdevBox);
    row2->addWidget(clearSelBtn);
    row2->addWidget(clearSpecBtn);
    row2->addStretch(1);
    vdevLay->addLayout(row2);
    auto* vdevHelp =
        new QLabel(tr3(QStringLiteral("Puede construir algo como: raidz2 d1 d2 d3 d4 d5 d6 | raidz2 d7 d8 d9 d10 d11 d12 | special mirror nvme0 nvme1 | log mirror nvme2 nvme3 | cache nvme4 | dedup mirror nvme5 nvme6 | spare d13"),
                       QStringLiteral("Build specs like: raidz2 d1 d2 d3 d4 d5 d6 | raidz2 d7 d8 d9 d10 d11 d12 | special mirror nvme0 nvme1 | log mirror nvme2 nvme3 | cache nvme4 | dedup mirror nvme5 nvme6 | spare d13"),
                       QStringLiteral("可构建如下规格：raidz2 d1 d2 d3 d4 d5 d6 | raidz2 d7 d8 d9 d10 d11 d12 | special mirror nvme0 nvme1 | log mirror nvme2 nvme3 | cache nvme4 | dedup mirror nvme5 nvme6 | spare d13")),
                   vdevBox);
    vdevHelp->setWordWrap(true);
    vdevLay->addWidget(vdevHelp, 0);
    QPlainTextEdit* vdevSpecEdit = new QPlainTextEdit(vdevBox);
    vdevSpecEdit->setPlaceholderText(
        tr3(QStringLiteral("Una línea por grupo o use '|' en una línea.\nEjemplo:\nraidz2 /dev/sda /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf\nraidz2 /dev/sdg /dev/sdh /dev/sdi /dev/sdj /dev/sdk /dev/sdl\nspecial mirror /dev/nvme0n1 /dev/nvme1n1\nlog mirror /dev/nvme2n1 /dev/nvme3n1\ncache /dev/nvme4n1\ndedup mirror /dev/nvme5n1 /dev/nvme6n1\nspare /dev/sdm"),
            QStringLiteral("One line per group or use '|' in one line.\nExample:\nraidz2 /dev/sda /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf\nraidz2 /dev/sdg /dev/sdh /dev/sdi /dev/sdj /dev/sdk /dev/sdl\nspecial mirror /dev/nvme0n1 /dev/nvme1n1\nlog mirror /dev/nvme2n1 /dev/nvme3n1\ncache /dev/nvme4n1\ndedup mirror /dev/nvme5n1 /dev/nvme6n1\nspare /dev/sdm"),
            QStringLiteral("每行一个组，或在一行里用“|”。\n示例：\nraidz2 /dev/sda /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf\nraidz2 /dev/sdg /dev/sdh /dev/sdi /dev/sdj /dev/sdk /dev/sdl\nspecial mirror /dev/nvme0n1 /dev/nvme1n1\nlog mirror /dev/nvme2n1 /dev/nvme3n1\ncache /dev/nvme4n1\ndedup mirror /dev/nvme5n1 /dev/nvme6n1\nspare /dev/sdm")));
    vdevLay->addWidget(vdevSpecEdit, 1);
    leftLay->addWidget(vdevBox, 1);

    auto* devicesBox = new QGroupBox(
        tr3(QStringLiteral("Block devices disponibles"),
            QStringLiteral("Available block devices"),
            QStringLiteral("可用块设备")),
        leftPane);
    auto* devicesLayout = new QVBoxLayout(devicesBox);
    QTableWidget* devicesTable = new QTableWidget(devicesBox);
    devicesTable->setColumnCount(6);
    devicesTable->setHorizontalHeaderLabels({
        tr3(QStringLiteral("Usar"), QStringLiteral("Use"), QStringLiteral("使用")),
        tr3(QStringLiteral("Device"), QStringLiteral("Device"), QStringLiteral("设备")),
        tr3(QStringLiteral("Tamaño"), QStringLiteral("Size"), QStringLiteral("大小")),
        tr3(QStringLiteral("Mount"), QStringLiteral("Mount"), QStringLiteral("挂载")),
        tr3(QStringLiteral("Estado"), QStringLiteral("State"), QStringLiteral("状态")),
        tr3(QStringLiteral("Detalle"), QStringLiteral("Detail"), QStringLiteral("详情")),
    });
    devicesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    devicesTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    devicesTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    devicesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    devicesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    devicesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    devicesTable->setWordWrap(true);
    devicesTable->setTextElideMode(Qt::ElideNone);
    devicesTable->verticalHeader()->setVisible(false);
    devicesTable->verticalHeader()->setDefaultSectionSize(22);
    const QColor stRed("#ffcccc");
    const QColor stYellow("#fff5bf");
    const QColor stGreen("#d7f7d7");

    struct DeviceRenderRow {
        DeviceEntry entry;
        QString stateText;
        QString detailText;
        QColor bgColor;
        int colorRank{2}; // 0=green, 1=yellow, 2=red
        bool selectable{true};
    };
    QVector<DeviceRenderRow> renderRows;
    renderRows.reserve(devicesByPath.size());
    QStringList paths = devicesByPath.keys();
    auto hasProtectedSystemMount = [](const DeviceEntry& e) -> bool {
        const QString fs = e.fsType.trimmed().toLower();
        if (fs == QStringLiteral("swap")) {
            return true;
        }
        static const QSet<QString> macSystemTypes = {
            QStringLiteral("apple_apfs_isc"),
            QStringLiteral("apple_apfs"),
            QStringLiteral("efi"),
            QStringLiteral("apfs volume"),
            QStringLiteral("apfs snapshot"),
            QStringLiteral("apple_apfs_recovery"),
        };
        if (macSystemTypes.contains(fs)) {
            return true;
        }
        QString mp = e.mountpoint;
        mp.replace(QStringLiteral("\\n"), QStringLiteral(" "));
        mp.replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QStringList toks = mp.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
        for (const QString& tRaw : toks) {
            const QString t = tRaw.trimmed();
            if (t == QStringLiteral("/") || t == QStringLiteral("/boot") || t == QStringLiteral("/boot/efi")
                || t == QStringLiteral("[SWAP]")) {
                return true;
            }
        }
        return false;
    };
    for (const QString& path : paths) {
        const DeviceEntry e = devicesByPath.value(path);
        const QString realPath = e.resolvedPath.isEmpty() ? e.path : e.resolvedPath;
        if (e.devType == QStringLiteral("disk") || isRootDevicePath(e.path) || isRootDevicePath(realPath)) {
            continue;
        }
        DeviceRenderRow rr;
        rr.entry = e;
        const bool protectedMount = hasProtectedSystemMount(e);
        if (protectedMount || e.inPool || e.childBusy) {
            rr.stateText = tr3(QStringLiteral("EN_POOL"), QStringLiteral("IN_POOL"), QStringLiteral("在池中"));
            if (protectedMount) {
                rr.stateText = tr3(QStringLiteral("SISTEMA"), QStringLiteral("SYSTEM"), QStringLiteral("系统"));
                rr.detailText = tr3(QStringLiteral("Dispositivo de sistema (/ /boot /boot/efi o SWAP)"),
                                    QStringLiteral("System device (/ /boot /boot/efi or SWAP)"),
                                    QStringLiteral("系统设备（/ /boot /boot/efi 或 SWAP）"));
            } else if (e.childBusy) {
                rr.detailText = tr3(QStringLiteral("Alguna partición está montada o pertenece a un pool"),
                                    QStringLiteral("A child partition is mounted or belongs to a pool"),
                                    QStringLiteral("某个子分区已挂载或属于池"));
            } else {
                rr.detailText = tr3(QStringLiteral("Ya pertenece a un pool"),
                                    QStringLiteral("Already part of a pool"),
                                    QStringLiteral("已属于某个池"));
            }
            rr.bgColor = stRed;
            rr.colorRank = 2;
            rr.selectable = false;
        } else if (e.mounted || (!e.mountpoint.isEmpty() && e.mountpoint != QStringLiteral("-"))) {
            rr.stateText = tr3(QStringLiteral("MONTADO"), QStringLiteral("MOUNTED"), QStringLiteral("已挂载"));
            rr.detailText = e.mountpoint;
            rr.bgColor = stYellow;
            rr.colorRank = 1;
        } else {
            rr.stateText = tr3(QStringLiteral("LIBRE"), QStringLiteral("FREE"), QStringLiteral("空闲"));
            rr.detailText = tr3(QStringLiteral("Disponible"), QStringLiteral("Available"), QStringLiteral("可用"));
            rr.bgColor = stGreen;
            rr.colorRank = 0;
        }
        if (!e.fsType.isEmpty() && e.fsType != QStringLiteral("-")) {
            rr.detailText = rr.detailText + QStringLiteral(" | fstype=") + e.fsType;
        }
        if (!e.resolvedPath.isEmpty() && e.resolvedPath != e.path) {
            rr.detailText = rr.detailText + QStringLiteral(" | ") + e.resolvedPath;
        }
        renderRows.push_back(rr);
    }
    std::sort(renderRows.begin(), renderRows.end(), [](const DeviceRenderRow& a, const DeviceRenderRow& b) {
        if (a.colorRank != b.colorRank) {
            return a.colorRank < b.colorRank; // green, yellow, red
        }
        return a.entry.path < b.entry.path;
    });
    for (const DeviceRenderRow& rr : renderRows) {
        const DeviceEntry& e = rr.entry;
        const int row = devicesTable->rowCount();
        devicesTable->insertRow(row);

        auto* useItem = new QTableWidgetItem();
        Qt::ItemFlags uf = (useItem->flags() & ~Qt::ItemIsEditable);
        if (rr.selectable) {
            uf |= Qt::ItemIsUserCheckable;
            useItem->setCheckState(Qt::Unchecked);
        } else {
            uf &= ~Qt::ItemIsUserCheckable;
            useItem->setText(QStringLiteral("—"));
        }
        useItem->setFlags(uf);
        devicesTable->setItem(row, 0, useItem);

        auto* devItem = new QTableWidgetItem(wrapDeviceDisplayText(e.path));
        devItem->setData(Qt::UserRole, e.path);
        devItem->setToolTip(e.path);
        devicesTable->setItem(row, 1, devItem);
        devicesTable->setItem(row, 2, new QTableWidgetItem(e.size));
        devicesTable->setItem(row, 3, new QTableWidgetItem(e.mountpoint));
        auto* stateItem = new QTableWidgetItem(rr.stateText);
        if (!e.fsType.isEmpty() && e.fsType != QStringLiteral("-")) {
            stateItem->setToolTip(QStringLiteral("fstype=%1").arg(e.fsType));
        }
        auto* detailItem = new QTableWidgetItem(rr.detailText);
        devicesTable->setItem(row, 4, stateItem);
        devicesTable->setItem(row, 5, detailItem);
        for (int c = 0; c < devicesTable->columnCount(); ++c) {
            if (QTableWidgetItem* cell = devicesTable->item(row, c)) {
                cell->setBackground(rr.bgColor);
            }
        }
    }
    devicesTable->resizeRowsToContents();
    devicesLayout->addWidget(devicesTable, 1);
    body->addWidget(leftPane, 3);
    body->addWidget(devicesBox, 2);
    lay->addLayout(body, 1);

    auto checkedDevices = [devicesTable]() -> QStringList {
        QStringList selected;
        for (int r = 0; r < devicesTable->rowCount(); ++r) {
            QTableWidgetItem* use = devicesTable->item(r, 0);
            QTableWidgetItem* dev = devicesTable->item(r, 1);
            if (!use || !dev) {
                continue;
            }
            if (use->checkState() == Qt::Checked) {
                QString d = dev->data(Qt::UserRole).toString().trimmed();
                if (d.isEmpty()) {
                    d = dev->text();
                    d.replace('\n', QString());
                    d = d.trimmed();
                }
                if (!d.isEmpty()) {
                    selected << d;
                }
            }
        }
        return selected;
    };
    connect(clearSelBtn, &QPushButton::clicked, &dlg, [devicesTable]() {
        for (int r = 0; r < devicesTable->rowCount(); ++r) {
            if (QTableWidgetItem* use = devicesTable->item(r, 0)) {
                use->setCheckState(Qt::Unchecked);
            }
        }
    });
    connect(clearSpecBtn, &QPushButton::clicked, &dlg, [vdevSpecEdit]() {
        vdevSpecEdit->clear();
    });
    connect(addVdevBtn, &QPushButton::clicked, &dlg, [&]() {
        QStringList selected = checkedDevices();
        if (selected.isEmpty()) {
            QMessageBox::information(
                &dlg, QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("Seleccione dispositivos en la tabla de la derecha."),
                    QStringLiteral("Select devices in the table on the right."),
                    QStringLiteral("请在右侧表格中选择设备。")));
            return;
        }
        QStringList line;
        const QString klass = vdevClassCb->currentText().trimmed().toLower();
        const QString vtype = vdevTypeCb->currentText().trimmed().toLower();
        if (klass != QStringLiteral("data")) {
            line << klass;
        }
        const bool noRedundancyClass = (klass == QStringLiteral("cache") || klass == QStringLiteral("spare"));
        if (!noRedundancyClass && vtype != QStringLiteral("stripe")) {
            line << vtype;
        }
        for (const QString& d : selected) {
            line << d;
        }
        const QString textLine = line.join(' ');
        QString cur = vdevSpecEdit->toPlainText().trimmed();
        if (!cur.isEmpty()) {
            cur += QLatin1Char('\n');
        }
        cur += textLine;
        vdevSpecEdit->setPlainText(cur);
        for (int r = 0; r < devicesTable->rowCount(); ++r) {
            QTableWidgetItem* use = devicesTable->item(r, 0);
            if (use) {
                use->setCheckState(Qt::Unchecked);
            }
        }
    });

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QString poolName = poolNameEd->text().trimmed();
    if (poolName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Nombre de pool vacío."));
        return;
    }

    QStringList selectedDevices = checkedDevices();

    QStringList parts;
    parts << QStringLiteral("zpool create");
    if (forceCb->isChecked()) {
        parts << QStringLiteral("-f");
    }
    if (dryRunCb->isChecked()) {
        parts << QStringLiteral("-n");
    }
    if (!mountpointEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-m") << shSingleQuote(mountpointEd->text().trimmed());
    }
    if (!altrootEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-R") << shSingleQuote(altrootEd->text().trimmed());
    }
    auto addOpt = [&parts](const QString& key, const QString& value) {
        if (!value.trimmed().isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(key + QStringLiteral("=") + value.trimmed());
        }
    };
    addOpt(QStringLiteral("ashift"), ashiftEd->text());
    addOpt(QStringLiteral("autotrim"), autotrimEd->text());
    addOpt(QStringLiteral("autoexpand"), autoexpandEd->text());
    addOpt(QStringLiteral("compatibility"), compatibilityEd->text());
    addOpt(QStringLiteral("bootfs"), bootfsEd->text());
    for (const QString& item : poolOptsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString t = item.trimmed();
        if (!t.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(t);
        }
    }
    for (const QString& item : fsPropsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString t = item.trimmed();
        if (!t.isEmpty()) {
            parts << QStringLiteral("-O") << shSingleQuote(t);
        }
    }

    parts << shSingleQuote(poolName);
    QString vdevSpec = vdevSpecEdit->toPlainText();
    QStringList specLines;
    for (QString ln : vdevSpec.split('\n')) {
        ln = ln.trimmed();
        if (ln.isEmpty()) {
            continue;
        }
        const int hash = ln.indexOf('#');
        if (hash >= 0) {
            ln = ln.left(hash).trimmed();
        }
        if (ln.isEmpty()) {
            continue;
        }
        ln.replace('|', '\n');
        const QStringList splitByPipe = ln.split('\n', Qt::SkipEmptyParts);
        for (QString seg : splitByPipe) {
            seg = seg.trimmed();
            if (!seg.isEmpty()) {
                specLines << seg;
            }
        }
    }
    if (specLines.isEmpty()) {
        if (selectedDevices.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("Defina una especificación VDEV o seleccione dispositivos para modo rápido."),
                    QStringLiteral("Provide a VDEV spec or select devices for quick mode."),
                    QStringLiteral("请提供 VDEV 规格，或在快速模式中选择设备。")));
            return;
        }
        QStringList quick;
        const QString quickType = quickLayoutCb->currentText().trimmed().toLower();
        if (!quickType.isEmpty() && quickType != QStringLiteral("stripe")) {
            quick << quickType;
        }
        quick.append(selectedDevices);
        specLines << quick.join(' ');
    }
    for (const QString& line : specLines) {
        const QStringList toks = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString& tok : toks) {
            parts << shSingleQuote(tok.trimmed());
        }
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    const QString cmd = withSudo(p, parts.join(' '));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3")
                                         .arg(p.username, p.host)
                                         .arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Crear pool"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio crear pool en %1: %2").arg(p.name, poolName));
    setActionsLocked(true);
    QString cmdOut;
    QString cmdErr;
    int rc = -1;
    if (!runSsh(p, cmd, 120000, cmdOut, cmdErr, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error creando pool en %1::%2 -> %3")
                   .arg(p.name, poolName, oneLine(cmdErr.isEmpty() ? QStringLiteral("exit %1").arg(rc) : cmdErr)));
        QMessageBox::critical(
            this, QStringLiteral("ZFSMgr"),
            QStringLiteral("Crear pool falló:\n%1").arg(cmdErr.isEmpty() ? QStringLiteral("exit %1").arg(rc) : cmdErr));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin crear pool en %1: %2").arg(p.name, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
    populateAllPoolsTables();
    refreshSelectedPoolDetails();
}

MainWindow::DatasetSelectionContext MainWindow::currentDatasetSelection(const QString& side) const {
    DatasetSelectionContext ctx;
    QString token;
    QString ds;
    QString snap;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo->currentData().toString();
        ds = m_originSelectedDataset;
        snap = m_originSelectedSnapshot;
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo->currentData().toString();
        ds = m_destSelectedDataset;
        snap = m_destSelectedSnapshot;
    } else {
        token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
        if (m_advTree) {
            const auto selected = m_advTree->selectedItems();
            if (!selected.isEmpty()) {
                ds = selected.first()->data(0, Qt::UserRole).toString();
                snap = selected.first()->data(1, Qt::UserRole).toString();
            }
        }
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return ctx;
    }
    const int connIdx = token.left(sep).toInt();
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return ctx;
    }
    const QString pool = token.mid(sep + 2);
    if (ds.isEmpty()) {
        return ctx;
    }
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = pool;
    ctx.datasetName = ds;
    ctx.snapshotName = snap;
    return ctx;
}

void MainWindow::showDatasetContextMenu(const QString& side, QTreeWidget* tree, const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    QTreeWidgetItem* item = tree->itemAt(pos);
    if (!item) {
        return;
    }
    tree->setCurrentItem(item);
    if (side == QStringLiteral("origin")) {
        onOriginTreeSelectionChanged();
    } else if (side == QStringLiteral("dest")) {
        onDestTreeSelectionChanged();
    } else {
        refreshDatasetProperties(QStringLiteral("advanced"));
        const QString ds = item->data(0, Qt::UserRole).toString();
        const QString snap = item->data(1, Qt::UserRole).toString();
        updateAdvancedSelectionUi(ds, snap);
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }

    QMenu menu(this);
    QAction* mountAct = menu.addAction(QStringLiteral("Montar"));
    QAction* mountWithChildrenAct = menu.addAction(QStringLiteral("Montar con todos los hijos"));
    QAction* umountAct = menu.addAction(QStringLiteral("Desmontar"));
    QAction* rollbackAct = nullptr;
    if (!ctx.snapshotName.isEmpty()) {
        rollbackAct = menu.addAction(QStringLiteral("Rollback"));
    }
    menu.addSeparator();
    QAction* createAct = menu.addAction(QStringLiteral("Crear hijo"));
    QAction* deleteAct = menu.addAction(QStringLiteral("Borrar"));
    const bool isWinConn = isWindowsConnection(ctx.connIdx);

    if (!ctx.snapshotName.isEmpty()) {
        mountAct->setEnabled(false);
        mountWithChildrenAct->setEnabled(false);
        umountAct->setEnabled(false);
        createAct->setEnabled(false);
    } else {
        bool knownMounted = false;
        bool isMounted = false;
        const QString key = datasetCacheKey(ctx.connIdx, ctx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt != m_poolDatasetCache.constEnd()) {
            const auto recIt = cacheIt->recordByName.constFind(ctx.datasetName);
            if (recIt != cacheIt->recordByName.constEnd()) {
                const QString m = recIt->mounted.trimmed().toLower();
                if (m == QStringLiteral("yes") || m == QStringLiteral("on") || m == QStringLiteral("true") || m == QStringLiteral("1")) {
                    knownMounted = true;
                    isMounted = true;
                } else if (m == QStringLiteral("no") || m == QStringLiteral("off") || m == QStringLiteral("false") || m == QStringLiteral("0")) {
                    knownMounted = true;
                    isMounted = false;
                }
            }
        }
        if (knownMounted) {
            mountAct->setEnabled(!isMounted);
            umountAct->setEnabled(isMounted);
        }
    }
    Q_UNUSED(isWinConn);

    QAction* picked = menu.exec(tree->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == mountAct) {
        logUiAction(QStringLiteral("Montar dataset (menú)"));
        actionMountDataset(side);
    } else if (picked == mountWithChildrenAct) {
        logUiAction(QStringLiteral("Montar dataset con hijos (menú)"));
        actionMountDatasetWithChildren(side);
    } else if (picked == umountAct) {
        logUiAction(QStringLiteral("Desmontar dataset (menú)"));
        actionUmountDataset(side);
    } else if (picked == createAct) {
        logUiAction(QStringLiteral("Crear hijo dataset (menú)"));
        actionCreateChildDataset(side);
    } else if (rollbackAct && picked == rollbackAct) {
        const QString snapObj = QStringLiteral("%1@%2").arg(ctx.datasetName, ctx.snapshotName);
        const auto confirm = QMessageBox::question(
            this,
            QStringLiteral("Rollback"),
            QStringLiteral("¿Confirmar rollback de snapshot?\n%1").arg(snapObj),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirm != QMessageBox::Yes) {
            return;
        }
        logUiAction(QStringLiteral("Rollback snapshot (menú)"));
        const QString cmd = QStringLiteral("zfs rollback %1").arg(shSingleQuote(snapObj));
        executeDatasetAction(side, QStringLiteral("Rollback"), ctx, cmd, 90000);
    } else if (picked == deleteAct) {
        logUiAction(QStringLiteral("Borrar dataset/snapshot (menú)"));
        actionDeleteDatasetOrSnapshot(side);
    }
}

bool MainWindow::executeDatasetAction(const QString& side, const QString& actionName, const DatasetSelectionContext& ctx, const QString& cmd, int timeoutMs, bool allowWindowsScript) {
    if (!ctx.valid) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    if (isWindowsConnection(p) && !allowWindowsScript) {
        const QString c = cmd.toLower();
        const bool unixScriptLike = c.contains(QStringLiteral("&&"))
            || c.contains(QStringLiteral(" set -e"))
            || c.contains(QStringLiteral("trap "))
            || c.contains(QStringLiteral("awk "))
            || c.contains(QStringLiteral("grep "))
            || c.contains(QStringLiteral("mktemp "))
            || c.contains(QStringLiteral("rsync "))
            || c.contains(QStringLiteral("tail "));
        if (unixScriptLike) {
            QMessageBox::information(
                this,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("La acción \"%1\" usa shell Unix y no está disponible en conexiones Windows por ahora.")
                        .arg(actionName),
                    QStringLiteral("Action \"%1\" uses Unix shell and is not available on Windows connections yet.")
                        .arg(actionName),
                    QStringLiteral("操作“%1”依赖 Unix shell，当前在 Windows 连接中不可用。")
                        .arg(actionName)));
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Acción no soportada en Windows: %1 (%2)").arg(actionName, p.name));
            return false;
        }
    }
    QString remoteCmd = withSudo(p, cmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, remoteCmd));
    if (!confirmActionExecution(actionName, {preview})) {
        return false;
    }
    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 %2::%3").arg(actionName, p.name, ctx.datasetName));
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, remoteCmd, timeoutMs, out, err, rc) || rc != 0) {
        QString failureDetail = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
        if (!out.trimmed().isEmpty()) {
            failureDetail += QStringLiteral("\n\nstdout:\n%1").arg(out.trimmed());
        }
        if (actionName == QStringLiteral("Desmontar")) {
            const QString diag = diagnoseUmountFailure(ctx).trimmed();
            if (!diag.isEmpty()) {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("Diagnóstico de desmontaje %1::%2 -> %3")
                           .arg(p.name, ctx.datasetName, oneLine(diag)));
                failureDetail += QStringLiteral("\n\nProcesos/diagnóstico sobre el mountpoint:\n%1").arg(diag);
            }
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error en %1: %2")
                   .arg(actionName, oneLine(failureDetail)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("%1 falló:\n%2").arg(actionName, failureDetail));
        setActionsLocked(false);
        return false;
    }
    if (!out.trimmed().isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 finalizado").arg(actionName));
    invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
    const bool needsDeferredRefresh =
        (actionName == QStringLiteral("Montar")
         || actionName == QStringLiteral("Montar con todos los hijos")
         || actionName == QStringLiteral("Desmontar")
         || actionName == QStringLiteral("Desde Dir")
         || actionName == QStringLiteral("Desglosar"));
    if (needsDeferredRefresh) {
        const int refreshIdx = ctx.connIdx;
        QTimer::singleShot(0, this, [this, refreshIdx]() {
            refreshConnectionByIndex(refreshIdx);
            setActionsLocked(false);
        });
        return true;
    }
    reloadDatasetSide(side);
    setActionsLocked(false);
    return true;
}

QString MainWindow::diagnoseUmountFailure(const DatasetSelectionContext& ctx) {
    if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
        return QString();
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString mp;
    QString mpHint;
    QString mountedValue;
    getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mountpoint"), mpHint);
    getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mounted"), mountedValue);
    mp = effectiveMountPath(ctx.connIdx, ctx.poolName, ctx.datasetName, mpHint, mountedValue).trimmed();
    if (mp.isEmpty()) {
        mp = mpHint.trimmed();
    }
    if (mp.isEmpty()) {
        return tr3(QStringLiteral("No se pudo resolver el mountpoint para diagnóstico."),
                   QStringLiteral("Could not resolve mountpoint for diagnostics."),
                   QStringLiteral("无法解析用于诊断的挂载点。"));
    }

    QString out;
    QString err;
    int rc = -1;
    QString diagCmd;
    if (isWindowsConnection(p)) {
        QString dsPs = ctx.datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        QString mpPs = mp;
        mpPs.replace('\'', QStringLiteral("''"));
        diagCmd = QStringLiteral(
                      "$ds='%1'; $mp='%2'; "
                      "Write-Output ('dataset=' + $ds); "
                      "Write-Output ('mountpoint=' + $mp); "
                      "Write-Output 'No hay lsof/fuser por defecto en Windows para identificar el proceso bloqueante.'")
                      .arg(dsPs, mpPs);
    } else {
        const QString mpQ = shSingleQuote(mp);
        diagCmd = QStringLiteral(
                      "MP=%1; "
                      "echo \"mountpoint=$MP\"; "
                      "if command -v lsof >/dev/null 2>&1; then "
                      "  echo \"--- lsof ---\"; "
                      "  lsof +f -- \"$MP\" 2>/dev/null | head -n 80; "
                      "else "
                      "  echo \"lsof no disponible\"; "
                      "fi; "
                      "if command -v fuser >/dev/null 2>&1; then "
                      "  echo \"--- fuser ---\"; "
                      "  fuser -vm \"$MP\" 2>/dev/null | head -n 80; "
                      "else "
                      "  echo \"fuser no disponible\"; "
                      "fi")
                      .arg(mpQ);
    }

    if (!runSsh(p, withSudo(p, diagCmd), 15000, out, err, rc)) {
        return tr3(QStringLiteral("No se pudo ejecutar el diagnóstico remoto."),
                   QStringLiteral("Could not execute remote diagnostics."),
                   QStringLiteral("无法执行远程诊断。"));
    }
    if (rc != 0 && out.trimmed().isEmpty()) {
        return err.trimmed().isEmpty() ? QStringLiteral("diagnostic exit %1").arg(rc) : err.trimmed();
    }
    if (!out.trimmed().isEmpty()) {
        return out.trimmed();
    }
    return err.trimmed();
}

void MainWindow::invalidateDatasetCacheForPool(int connIdx, const QString& poolName) {
    m_poolDatasetCache.remove(datasetCacheKey(connIdx, poolName));
}

void MainWindow::reloadDatasetSide(const QString& side) {
    if (side == QStringLiteral("origin")) {
        onOriginPoolChanged();
    } else if (side == QStringLiteral("dest")) {
        onDestPoolChanged();
    } else {
        const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep > 0) {
            const int connIdx = token.left(sep).toInt();
            const QString poolName = token.mid(sep + 2);
            populateDatasetTree(m_advTree, connIdx, poolName, QStringLiteral("advanced"));
            refreshDatasetProperties(QStringLiteral("advanced"));
        }
    }
}

void MainWindow::actionMountDataset(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    mountDataset(side, ctx);
}

bool MainWindow::mountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return false;
    }
    if (!ensureParentMountedBeforeMount(ctx, side)) {
        return false;
    }
    if (!ensureNoMountpointConflictsBeforeMount(ctx, false)) {
        return false;
    }
    if (isWindowsConnection(ctx.connIdx)) {
        const ConnectionProfile& p = m_profiles[ctx.connIdx];
        QString effectiveMp = effectiveMountPath(ctx.connIdx,
                                                 ctx.poolName,
                                                 ctx.datasetName,
                                                 QString(),
                                                 QStringLiteral("yes"));
        if (effectiveMp.trimmed().isEmpty()) {
            QString mpRaw;
            if (getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("mountpoint"), mpRaw)) {
                effectiveMp = mpRaw.trimmed();
            }
        }
        QString dsPs = ctx.datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        QString mpPs = effectiveMp.trimmed();
        mpPs.replace('\'', QStringLiteral("''"));
        const QString precheckCmd = QStringLiteral(
            "$ds='%1'; "
            "$mp='%2'; "
            "if ([string]::IsNullOrWhiteSpace($mp) -or $mp -eq '-' -or $mp -eq 'none') { "
            "  throw ('mountpoint efectivo no resuelto para ' + $ds) "
            "}; "
            "$exists = Test-Path -LiteralPath $mp; "
            "$mapped = $false; "
            "foreach ($line in @(zfs mount 2>$null)) { "
            "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
            "    $d = $Matches[1].Trim(); "
            "    $m = $Matches[2].Trim(); "
            "    if ([string]::Equals($m, $mp, [System.StringComparison]::OrdinalIgnoreCase)) { "
            "      if ($d -eq $ds) { $mapped = $true }; "
            "      break; "
            "    } "
            "  } "
            "}; "
            "if ($exists -and -not $mapped) { "
            "  throw ('mountpoint ocupado por ruta existente no-ZFS: ' + $mp) "
            "}")
                                       .arg(dsPs, mpPs);
        QString preOut;
        QString preErr;
        int preRc = -1;
        if (!runSsh(p, withSudo(p, precheckCmd), 15000, preOut, preErr, preRc) || preRc != 0) {
            const QString reason = oneLine((preErr.isEmpty() ? preOut : preErr));
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("No se puede montar %1.\n%2").arg(ctx.datasetName, reason),
                    QStringLiteral("Cannot mount %1.\n%2").arg(ctx.datasetName, reason),
                    QStringLiteral("无法挂载 %1。\n%2").arg(ctx.datasetName, reason)));
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Precheck montar falló %1::%2 -> %3")
                       .arg(p.name, ctx.datasetName, reason));
            return false;
        }
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    const QString cmd = QStringLiteral("zfs mount %1").arg(dsQ);
    return executeDatasetAction(side, QStringLiteral("Montar"), ctx, cmd);
}

void MainWindow::actionMountDatasetWithChildren(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    if (!ensureParentMountedBeforeMount(ctx, side)) {
        return;
    }
    if (!ensureNoMountpointConflictsBeforeMount(ctx, true)) {
        return;
    }
    if (isWindowsConnection(ctx.connIdx)) {
        QString dsPs = ctx.datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        const QString cmd = QStringLiteral(
                                "$ds='%1'; "
                                "$items = @(zfs list -H -o name -r $ds 2>$null); "
                                "if ($LASTEXITCODE -ne 0) { throw 'zfs list failed' }; "
                                "foreach ($child in $items) { "
                                "  if ([string]::IsNullOrWhiteSpace($child)) { continue }; "
                                "  $m = (zfs get -H -o value mounted $child 2>$null | Out-String).Trim().ToLower(); "
                                "  if ($m -ne 'yes' -and $m -ne 'on' -and $m -ne 'true' -and $m -ne '1') { "
                                "    zfs mount $child 2>$null | Out-Null "
                                "  } "
                                "}")
                                .arg(dsPs);
        executeDatasetAction(side, QStringLiteral("Montar con todos los hijos"), ctx, cmd, 90000, true);
        return;
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    const QString cmd = QStringLiteral(
                            "set -e; DATASET=%1; "
                            "zfs list -H -o name -r \"$DATASET\" | "
                            "while IFS= read -r child; do "
                            "  [ -n \"$child\" ] || continue; "
                            "  mounted=$(zfs get -H -o value mounted \"$child\" 2>/dev/null || true); "
                            "  case \"$mounted\" in yes|on|true|1) : ;; *) zfs mount \"$child\" ;; esac; "
                            "done")
                            .arg(dsQ);
    executeDatasetAction(side, QStringLiteral("Montar con todos los hijos"), ctx, cmd);
}

bool MainWindow::ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx, const QString& side) {
    Q_UNUSED(side);
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        return false;
    }
    const QString parent = parentDatasetName(ctx.datasetName);
    if (parent.isEmpty()) {
        return true;
    }

    QString parentMountpoint;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("mountpoint"), parentMountpoint)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar mountpoint del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mountpoint %1").arg(parent),
                                 QStringLiteral("无法检查父数据集 mountpoint：%1").arg(parent)));
        return false;
    }
    const QString mp = parentMountpoint.trimmed().toLower();
    QString parentCanmount;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("canmount"), parentCanmount)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar canmount del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent canmount %1").arg(parent),
                                 QStringLiteral("无法检查父数据集 canmount：%1").arg(parent)));
        return false;
    }
    const QString canmount = parentCanmount.trimmed().toLower();
    if (mp.isEmpty() || mp == QStringLiteral("none") || canmount == QStringLiteral("off")) {
        return true;
    }

    QString parentMounted;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("mounted"), parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar estado mounted del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mounted state %1").arg(parent),
                                 QStringLiteral("无法检查父数据集挂载状态：%1").arg(parent)));
        return false;
    }
    if (!isMountedValueTrue(parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("El dataset padre %1 no está montado, móntelo antes por favor").arg(parent),
                                 QStringLiteral("Parent dataset %1 is not mounted, mount it first").arg(parent),
                                 QStringLiteral("父数据集 %1 未挂载，请先挂载").arg(parent)));
        return false;
    }
    return true;
}

bool MainWindow::ensureNoMountpointConflictsBeforeMount(const DatasetSelectionContext& ctx, bool includeDescendants) {
    if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size() || ctx.datasetName.isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];

    if (!ensureDatasetsLoaded(ctx.connIdx, ctx.poolName)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar conflictos de mountpoint."),
                                 QStringLiteral("Could not validate mountpoint conflicts."),
                                 QStringLiteral("无法检查挂载点冲突。")));
        return false;
    }
    const QString key = datasetCacheKey(ctx.connIdx, ctx.poolName);
    const auto cacheIt = m_poolDatasetCache.constFind(key);
    if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar conflictos de mountpoint."),
                                 QStringLiteral("Could not validate mountpoint conflicts."),
                                 QStringLiteral("无法检查挂载点冲突。")));
        return false;
    }
    const PoolDatasetCache& cache = cacheIt.value();

    QMap<QString, QString> targetMpByDs;
    QMap<QString, QStringList> targetDsByMp;
    const QString prefix = ctx.datasetName + QStringLiteral("/");
    for (auto it = cache.recordByName.constBegin(); it != cache.recordByName.constEnd(); ++it) {
        const QString ds = it.key();
        if (ds != ctx.datasetName) {
            if (!includeDescendants || !ds.startsWith(prefix)) {
                continue;
            }
        }
        const DatasetRecord& rec = it.value();
        const QString mp = effectiveMountPath(ctx.connIdx, ctx.poolName, ds, rec.mountpoint, rec.mounted);
        const QString mpl = mp.trimmed().toLower();
        if (ds.isEmpty() || mp.isEmpty() || mpl == QStringLiteral("none") || mpl == QStringLiteral("-")) {
            continue;
        }
        targetMpByDs[ds] = mp;
        targetDsByMp[mp].push_back(ds);
    }

    for (auto it = targetDsByMp.constBegin(); it != targetDsByMp.constEnd(); ++it) {
        const QStringList dsList = it.value();
        if (dsList.size() > 1) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("Conflicto de mountpoint dentro de la selección.\nMountpoint: %1\nDatasets:\n%2")
                        .arg(it.key(), dsList.join('\n')),
                    QStringLiteral("Mountpoint conflict inside selection.\nMountpoint: %1\nDatasets:\n%2")
                        .arg(it.key(), dsList.join('\n')),
                    QStringLiteral("所选项内部存在挂载点冲突。\n挂载点：%1\n数据集：\n%2")
                        .arg(it.key(), dsList.join('\n'))));
            return false;
        }
    }

    QString mountedOut;
    QString mountedErr;
    int mountedRc = -1;
    const QString mountedCmd = withSudo(p, QStringLiteral("zfs mount"));
    if (!runSsh(p, mountedCmd, 20000, mountedOut, mountedErr, mountedRc) || mountedRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo leer datasets montados."),
                                 QStringLiteral("Could not read mounted datasets."),
                                 QStringLiteral("无法读取已挂载数据集。")));
        return false;
    }

    QMap<QString, QStringList> mountedByMp;
    for (const QString& ln : mountedOut.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = ln.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const int sp = trimmed.indexOf(' ');
        if (sp <= 0) {
            continue;
        }
        const QString ds = trimmed.left(sp).trimmed();
        const QString mp = trimmed.mid(sp + 1).trimmed();
        if (ds.isEmpty() || mp.isEmpty()) {
            continue;
        }
        mountedByMp[mp].push_back(ds);
    }

    for (auto it = targetMpByDs.constBegin(); it != targetMpByDs.constEnd(); ++it) {
        const QString targetDs = it.key();
        const QString mp = it.value();
        const QStringList mountedDs = mountedByMp.value(mp);
        for (const QString& dsMounted : mountedDs) {
            if (dsMounted != targetDs) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("ZFSMgr"),
                    tr3(QStringLiteral("No se permite montar más de un dataset en el mismo directorio.\nMountpoint: %1\nMontado: %2\nSolicitado: %3")
                            .arg(mp, dsMounted, targetDs),
                        QStringLiteral("Only one mounted dataset per directory is allowed.\nMountpoint: %1\nMounted: %2\nRequested: %3")
                            .arg(mp, dsMounted, targetDs),
                        QStringLiteral("同一目录不允许挂载多个数据集。\n挂载点：%1\n已挂载：%2\n请求：%3")
                            .arg(mp, dsMounted, targetDs)));
                return false;
            }
        }
    }
    return true;
}

void MainWindow::actionUmountDataset(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    umountDataset(side, ctx);
}

bool MainWindow::umountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return false;
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    QString hasChildrenCmd;
    const bool isWin = isWindowsConnection(ctx.connIdx);
    if (isWin) {
        QString dsPs = ctx.datasetName;
        dsPs.replace('\'', QStringLiteral("''"));
        hasChildrenCmd = QStringLiteral(
                             "$ds='%1'; "
                             "$has=$false; "
                             "$children=@(zfs list -H -o name -r $ds 2>$null); "
                             "if ($LASTEXITCODE -ne 0) { exit 2 }; "
                             "foreach ($c in $children) { "
                             "  if ([string]::IsNullOrWhiteSpace($c) -or $c -eq $ds) { continue }; "
                             "  $m=(zfs get -H -o value mounted $c 2>$null | Out-String).Trim().ToLower(); "
                             "  if ($m -eq 'yes' -or $m -eq 'on' -or $m -eq 'true' -or $m -eq '1') { $has=$true; break } "
                             "}; "
                             "if ($has) { exit 0 } else { exit 1 }")
                             .arg(dsPs);
    } else {
        hasChildrenCmd = QStringLiteral("zfs mount | awk '{print $1}' | grep -E '^%1/' -q").arg(ctx.datasetName);
    }

    QString out;
    QString err;
    int rc = -1;
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString checkCmd = withSudo(p, hasChildrenCmd);
    const bool ran = runSsh(p, checkCmd, 12000, out, err, rc);
    bool hasChildrenMounted = ran && rc == 0;
    QString cmd;
    if (hasChildrenMounted) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("ZFSMgr"),
            QStringLiteral("Hay hijos montados bajo %1.\n¿Desmontar recursivamente?").arg(ctx.datasetName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Desmontar abortado por usuario"));
            return false;
        }
        if (isWin) {
            QString dsPs = ctx.datasetName;
            dsPs.replace('\'', QStringLiteral("''"));
            cmd = QStringLiteral(
                      "$ds='%1'; "
                      "$list=@(zfs list -H -o name -r $ds 2>$null); "
                      "if ($LASTEXITCODE -ne 0) { throw 'zfs list failed' }; "
                      "$sorted = $list | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object { $_.Length } -Descending; "
                      "foreach ($d in $sorted) { zfs unmount $d 2>$null | Out-Null }")
                      .arg(dsPs);
        } else {
            cmd = QStringLiteral(
                "zfs mount | awk '{print $1}' | grep -E '^%1(/|$)' | awk '{print length, $0}' | sort -rn | cut -d' ' -f2- | "
                "while IFS= read -r ds; do [ -n \"$ds\" ] && zfs umount \"$ds\"; done")
                      .arg(ctx.datasetName);
        }
    } else {
        cmd = isWin ? QStringLiteral("zfs unmount %1").arg(dsQ)
                    : QStringLiteral("zfs umount %1").arg(dsQ);
    }
    return executeDatasetAction(side, QStringLiteral("Desmontar"), ctx, cmd, 90000, isWin);
}

void MainWindow::actionCreateChildDataset(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    struct PropSpec {
        QString name;
        QString kind;
        QStringList values;
    };
    struct PropEditor {
        QString name;
        QLineEdit* edit{nullptr};
        QComboBox* combo{nullptr};
    };

    QDialog dlg(this);
    dlg.setWindowTitle(tr3(QStringLiteral("Crear dataset"), QStringLiteral("Create dataset"), QStringLiteral("创建数据集")));
    dlg.setModal(true);
    dlg.resize(900, 760);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QWidget* formWidget = new QWidget(&dlg);
    QGridLayout* form = new QGridLayout(formWidget);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    int row = 0;

    QLabel* pathLabel = new QLabel(tr3(QStringLiteral("Path"), QStringLiteral("Path"), QStringLiteral("路径")), formWidget);
    QLineEdit* pathEdit = new QLineEdit(formWidget);
    pathEdit->setText(ctx.datasetName + QStringLiteral("/new_dataset"));
    form->addWidget(pathLabel, row, 0);
    form->addWidget(pathEdit, row, 1, 1, 3);
    row++;

    QLabel* typeLabel = new QLabel(tr3(QStringLiteral("Tipo"), QStringLiteral("Type"), QStringLiteral("类型")), formWidget);
    QComboBox* typeCombo = new QComboBox(formWidget);
    typeCombo->addItem(QStringLiteral("filesystem"), QStringLiteral("filesystem"));
    typeCombo->addItem(QStringLiteral("volume"), QStringLiteral("volume"));
    typeCombo->addItem(QStringLiteral("snapshot"), QStringLiteral("snapshot"));
    form->addWidget(typeLabel, row, 0);
    form->addWidget(typeCombo, row, 1);
    row++;

    QLabel* volsizeLabel = new QLabel(tr3(QStringLiteral("Volsize"), QStringLiteral("Volsize"), QStringLiteral("卷大小")), formWidget);
    QLineEdit* volsizeEdit = new QLineEdit(formWidget);
    form->addWidget(volsizeLabel, row, 0);
    form->addWidget(volsizeEdit, row, 1);
    row++;

    QLabel* blocksizeLabel = new QLabel(tr3(QStringLiteral("Blocksize"), QStringLiteral("Blocksize"), QStringLiteral("块大小")), formWidget);
    QLineEdit* blocksizeEdit = new QLineEdit(formWidget);
    form->addWidget(blocksizeLabel, row, 0);
    form->addWidget(blocksizeEdit, row, 1);
    row++;

    QWidget* optsWidget = new QWidget(formWidget);
    QHBoxLayout* optsLay = new QHBoxLayout(optsWidget);
    optsLay->setContentsMargins(0, 0, 0, 0);
    optsLay->setSpacing(12);
    QCheckBox* parentsChk = new QCheckBox(tr3(QStringLiteral("Crear padres (-p)"), QStringLiteral("Create parents (-p)"), QStringLiteral("创建父级(-p)")), optsWidget);
    QCheckBox* sparseChk = new QCheckBox(tr3(QStringLiteral("Sparse (-s)"), QStringLiteral("Sparse (-s)"), QStringLiteral("稀疏(-s)")), optsWidget);
    QCheckBox* nomountChk = new QCheckBox(tr3(QStringLiteral("No montar (-u)"), QStringLiteral("Do not mount (-u)"), QStringLiteral("不挂载(-u)")), optsWidget);
    parentsChk->setChecked(true);
    optsLay->addWidget(parentsChk);
    optsLay->addWidget(sparseChk);
    optsLay->addWidget(nomountChk);
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QCheckBox* snapRecursiveChk = new QCheckBox(
        tr3(QStringLiteral("Snapshot recursivo (-r)"), QStringLiteral("Recursive snapshot (-r)"), QStringLiteral("递归快照(-r)")),
        formWidget);
    form->addWidget(snapRecursiveChk, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(tr3(QStringLiteral("Argumentos extra"), QStringLiteral("Extra args"), QStringLiteral("额外参数")), formWidget);
    QLineEdit* extraEdit = new QLineEdit(formWidget);
    form->addWidget(extraLabel, row, 0);
    form->addWidget(extraEdit, row, 1, 1, 3);
    row++;

    root->addWidget(formWidget);

    QGroupBox* propsGroup = new QGroupBox(tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")), &dlg);
    QVBoxLayout* propsGroupLay = new QVBoxLayout(propsGroup);
    propsGroupLay->setContentsMargins(6, 6, 6, 6);
    propsGroupLay->setSpacing(4);

    QScrollArea* propsScroll = new QScrollArea(propsGroup);
    propsScroll->setWidgetResizable(true);
    QWidget* propsContainer = new QWidget(propsScroll);
    QGridLayout* propsGrid = new QGridLayout(propsContainer);
    propsGrid->setHorizontalSpacing(8);
    propsGrid->setVerticalSpacing(4);

    const QList<PropSpec> propSpecs = {
        {QStringLiteral("mountpoint"), QStringLiteral("entry"), {}},
        {QStringLiteral("canmount"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("noauto")}},
        {QStringLiteral("compression"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("lz4"), QStringLiteral("gzip"), QStringLiteral("zstd"), QStringLiteral("zle")}},
        {QStringLiteral("atime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("xattr"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa")}},
        {QStringLiteral("acltype"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), QStringLiteral("combo"), {QString(), QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("recordsize"), QStringLiteral("entry"), {}},
        {QStringLiteral("volblocksize"), QStringLiteral("entry"), {}},
        {QStringLiteral("quota"), QStringLiteral("entry"), {}},
        {QStringLiteral("reservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("refquota"), QStringLiteral("entry"), {}},
        {QStringLiteral("refreservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("copies"), QStringLiteral("combo"), {QString(), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}},
        {QStringLiteral("checksum"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("fletcher2"), QStringLiteral("fletcher4"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr")}},
        {QStringLiteral("sync"), QStringLiteral("combo"), {QString(), QStringLiteral("standard"), QStringLiteral("always"), QStringLiteral("disabled")}},
        {QStringLiteral("logbias"), QStringLiteral("combo"), {QString(), QStringLiteral("latency"), QStringLiteral("throughput")}},
        {QStringLiteral("primarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("secondarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("dedup"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("verify"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein")}},
        {QStringLiteral("encryption"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("aes-128-ccm"), QStringLiteral("aes-192-ccm"), QStringLiteral("aes-256-ccm"), QStringLiteral("aes-128-gcm"), QStringLiteral("aes-192-gcm"), QStringLiteral("aes-256-gcm")}},
        {QStringLiteral("keyformat"), QStringLiteral("combo"), {QString(), QStringLiteral("passphrase"), QStringLiteral("raw"), QStringLiteral("hex")}},
        {QStringLiteral("keylocation"), QStringLiteral("entry"), {}},
        {QStringLiteral("normalization"), QStringLiteral("combo"), {QString(), QStringLiteral("none"), QStringLiteral("formC"), QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")}},
        {QStringLiteral("casesensitivity"), QStringLiteral("combo"), {QString(), QStringLiteral("sensitive"), QStringLiteral("insensitive"), QStringLiteral("mixed")}},
        {QStringLiteral("utf8only"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
    };

    QList<PropEditor> propEditors;
    propEditors.reserve(propSpecs.size());
    for (int i = 0; i < propSpecs.size(); ++i) {
        const PropSpec& spec = propSpecs[i];
        const int r = i / 2;
        const int cBase = (i % 2) * 2;
        QLabel* lbl = new QLabel(spec.name, propsContainer);
        propsGrid->addWidget(lbl, r, cBase);
        PropEditor editor;
        editor.name = spec.name;
        if (spec.kind == QStringLiteral("combo")) {
            QComboBox* cb = new QComboBox(propsContainer);
            cb->addItems(spec.values);
            if (spec.name == QStringLiteral("compression")) {
                // Permite niveles, por ejemplo: zstd-19, gzip-9.
                cb->setEditable(true);
                cb->setInsertPolicy(QComboBox::NoInsert);
            }
            editor.combo = cb;
            propsGrid->addWidget(cb, r, cBase + 1);
        } else {
            QLineEdit* le = new QLineEdit(propsContainer);
            editor.edit = le;
            propsGrid->addWidget(le, r, cBase + 1);
        }
        propEditors.push_back(editor);
    }
    propsContainer->setLayout(propsGrid);
    propsScroll->setWidget(propsContainer);
    propsGroupLay->addWidget(propsScroll);
    root->addWidget(propsGroup, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        tr3(QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")),
        QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    auto setSuggestedPath = [&]() {
        const QString t = typeCombo->currentData().toString();
        const bool isSnapshot = t == QStringLiteral("snapshot");
        QString suggested = QStringLiteral("new_dataset");
        if (t == QStringLiteral("volume")) {
            suggested = QStringLiteral("new_volume");
        } else if (isSnapshot) {
            suggested = QStringLiteral("new_snapshot");
        }

        QString current = pathEdit->text().trimmed();
        QString noSnap = current;
        const int atPos = noSnap.indexOf('@');
        if (atPos >= 0) {
            noSnap = noSnap.left(atPos);
        }
        QString prefix;
        const int slash = noSnap.lastIndexOf('/');
        if (slash >= 0) {
            prefix = noSnap.left(slash + 1);
        } else if (!ctx.datasetName.isEmpty()) {
            prefix = ctx.datasetName + QStringLiteral("/");
        }

        const QString newPath = isSnapshot ? (prefix + suggested + QStringLiteral("@snap"))
                                           : (prefix + suggested);
        pathEdit->setText(newPath);
        pathEdit->setFocus();
        pathEdit->setSelection(prefix.size(), suggested.size());
    };

    auto applyTypeUi = [&]() {
        const QString t = typeCombo->currentData().toString();
        const bool isVolume = t == QStringLiteral("volume");
        const bool isSnapshot = t == QStringLiteral("snapshot");

        volsizeLabel->setVisible(isVolume);
        volsizeEdit->setVisible(isVolume);
        if (!isVolume) {
            volsizeEdit->clear();
        }

        blocksizeLabel->setVisible(!isSnapshot);
        blocksizeEdit->setVisible(!isSnapshot);
        if (isSnapshot) {
            blocksizeEdit->clear();
            parentsChk->setChecked(false);
            sparseChk->setChecked(false);
            nomountChk->setChecked(false);
        }

        parentsChk->setVisible(!isSnapshot);
        sparseChk->setVisible(!isSnapshot);
        nomountChk->setVisible(!isSnapshot);
        snapRecursiveChk->setVisible(isSnapshot);
        if (!isSnapshot) {
            snapRecursiveChk->setChecked(false);
        }
        setSuggestedPath();
    };
    QObject::connect(typeCombo, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&]() { applyTypeUi(); });
    applyTypeUi();

    bool accepted = false;
    CreateDatasetOptions opt;
    QObject::connect(createBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString path = pathEdit->text().trimmed();
        const QString dsType = typeCombo->currentData().toString().trimmed().toLower();
        const QString volsize = volsizeEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe indicar el path del dataset."),
                                     QStringLiteral("Dataset path is required."),
                                     QStringLiteral("必须指定数据集路径。")));
            return;
        }
        if (dsType == QStringLiteral("snapshot") && !path.contains('@')) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Para snapshot, el path debe incluir '@'."),
                                     QStringLiteral("For snapshot, path must include '@'."),
                                     QStringLiteral("快照路径必须包含'@'。")));
            return;
        }
        if (dsType == QStringLiteral("volume") && volsize.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Para volume, Volsize es obligatorio."),
                                     QStringLiteral("For volume, Volsize is required."),
                                     QStringLiteral("卷类型必须填写 Volsize。")));
            return;
        }

        QStringList properties;
        for (const PropEditor& pe : propEditors) {
            QString v;
            if (pe.combo) {
                v = pe.combo->currentText().trimmed();
            } else if (pe.edit) {
                v = pe.edit->text().trimmed();
            }
            if (!v.isEmpty()) {
                properties.push_back(pe.name + QStringLiteral("=") + v);
            }
        }

        opt.datasetPath = path;
        opt.dsType = dsType;
        opt.volsize = volsize;
        opt.blocksize = blocksizeEdit->text().trimmed();
        opt.parents = parentsChk->isChecked();
        opt.sparse = sparseChk->isChecked();
        opt.nomount = nomountChk->isChecked();
        opt.snapshotRecursive = snapRecursiveChk->isChecked();
        opt.properties = properties;
        opt.extraArgs = extraEdit->text().trimmed();
        accepted = true;
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted || !accepted) {
        return;
    }

    const QString actionLabel = (opt.dsType == QStringLiteral("snapshot"))
                                    ? tr3(QStringLiteral("Crear snapshot"), QStringLiteral("Create snapshot"), QStringLiteral("创建快照"))
                                    : tr3(QStringLiteral("Crear dataset"), QStringLiteral("Create dataset"), QStringLiteral("创建数据集"));
    const QString cmd = buildZfsCreateCmd(opt);
    executeDatasetAction(side, actionLabel, ctx, cmd);
}

void MainWindow::actionDeleteDatasetOrSnapshot(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }
    const QString target = ctx.snapshotName.isEmpty() ? ctx.datasetName : (ctx.datasetName + QStringLiteral("@") + ctx.snapshotName);
    const auto confirm1 = QMessageBox::question(
        this,
        QStringLiteral("Confirmar borrado"),
        QStringLiteral("Se va a borrar:\n%1\n¿Continuar?").arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm1 != QMessageBox::Yes) {
        return;
    }
    const auto confirm2 = QMessageBox::question(
        this,
        QStringLiteral("Confirmar borrado (2/2)"),
        QStringLiteral("Confirmación final de borrado:\n%1").arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm2 != QMessageBox::Yes) {
        return;
    }

    const auto askRec = QMessageBox::question(
        this,
        QStringLiteral("Borrado recursivo"),
        ctx.snapshotName.isEmpty()
            ? QStringLiteral("¿Borrar recursivamente datasets/snapshots hijos?")
            : QStringLiteral("¿Borrar recursivamente este snapshot en hijos descendientes?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    const bool recursive = (askRec == QMessageBox::Yes);
    QString cmd;
    cmd = recursive ? QStringLiteral("zfs destroy -r %1").arg(shSingleQuote(target))
                    : QStringLiteral("zfs destroy %1").arg(shSingleQuote(target));
    executeDatasetAction(side, QStringLiteral("Borrar"), ctx, cmd, 90000);
}

MainWindow::ConnectionRuntimeState MainWindow::refreshConnection(const ConnectionProfile& p) {
    ConnectionRuntimeState state;
    state.connectionMethod = p.connType;
    state.powershellFallbackCommands = zfsmgrPowershellCommandSet();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio refresh: %1 [%2]").arg(p.name, p.connType));

    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Tipo de conexión no soportado aún en cppqt");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }
    if (p.host.isEmpty() || p.username.isEmpty()) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Host/usuario no definido");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }

    QString out;
    QString err;
    int rc = -1;
    const QString osProbeCmd = isWindowsConnection(p)
                                   ? QStringLiteral("[System.Environment]::OSVersion.VersionString")
                                   : QStringLiteral("uname -a");
    if (!runSsh(p, osProbeCmd, 12000, out, err, rc) || rc != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }
    state.status = QStringLiteral("OK");
    state.detail = oneLine(out);
    state.osLine = oneLine(out);
    state.zfsVersion.clear();

    if (isWindowsConnection(p)) {
        auto logWhere = [&](const QString& exeName) {
            QString wOut, wErr;
            int wRc = -1;
            const QString whereCmd = QStringLiteral("where.exe %1").arg(exeName);
            if (runSsh(p, whereCmd, 12000, wOut, wErr, wRc) && wRc == 0) {
                const QStringList lines = wOut.split('\n', Qt::SkipEmptyParts);
                const QString firstPath = lines.isEmpty() ? QStringLiteral("(sin salida)") : lines.first().trimmed();
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("%1: where %2 -> %3").arg(p.name, exeName, oneLine(firstPath)));
            } else {
                const QString reason = oneLine(wErr.isEmpty() ? QStringLiteral("not found (rc=%1)").arg(wRc) : wErr);
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("%1: where %2 -> %3").arg(p.name, exeName, reason));
            }
        };
        logWhere(QStringLiteral("zfs"));
        logWhere(QStringLiteral("zpool"));
    }

    out.clear();
    err.clear();
    rc = -1;
    const QStringList zfsVersionCandidates = {
        QStringLiteral("zfs version"),
        QStringLiteral("zfs --version"),
        QStringLiteral("zpool --version"),
    };
    for (const QString& cand : zfsVersionCandidates) {
        out.clear();
        err.clear();
        rc = -1;
        const QString zfsVersionCmd = withSudo(p, cand);
        if (!runSsh(p, zfsVersionCmd, 12000, out, err, rc)) {
            continue;
        }
        const QString parsed = parseOpenZfsVersionText(out + QStringLiteral("\n") + err);
        if (!parsed.isEmpty()) {
            state.zfsVersion = parsed;
            state.zfsVersionFull = oneLine((out + QStringLiteral(" ") + err).simplified());
            break;
        }
        if (rc == 0) {
            // Algunos builds de OpenZFS en Windows imprimen solo el número de versión.
            const QString merged = out + QStringLiteral("\n") + err;
            const QRegularExpression simpleVerRx(QStringLiteral("\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b"));
            const QRegularExpressionMatch m = simpleVerRx.match(merged);
            if (m.hasMatch()) {
                const QString ver = m.captured(1);
                const int major = ver.section('.', 0, 0).toInt();
                if (major <= 10) {
                    state.zfsVersion = ver;
                    state.zfsVersionFull = oneLine(merged);
                    break;
                }
            }
        }
    }
    if (state.zfsVersion.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: ZFS version not detected").arg(p.name));
    } else if (state.zfsVersionFull.trimmed().isEmpty()) {
        state.zfsVersionFull = QStringLiteral("OpenZFS %1").arg(state.zfsVersion);
    }

    // Detección de comandos disponibles para mostrar en detalle de conexión.
    {
        const QStringList wanted = zfsmgrUnixCommandSet();
        QStringList detected;
        QStringList missing;
        if (isWindowsConnection(p)) {
            QString dout, derr;
            int drc = -1;
            const QString roots = QStringLiteral(
                "$roots=@('C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin','C:\\\\Program Files\\\\OpenZFS On Windows','C:\\\\msys64\\\\usr\\\\bin','C:\\\\msys64\\\\mingw64\\\\bin','C:\\\\msys64\\\\mingw32\\\\bin','C:\\\\MinGW\\\\bin','C:\\\\mingw64\\\\bin'); "
                "$present=@(); foreach($r in $roots){ if(Test-Path -LiteralPath $r){ $present += $r } }; "
                "if($present.Count -eq 0){ Write-Output '__NO_UNIX_LAYER__'; exit 0 }; "
                "$cmds='%1'.Split(' '); "
                "foreach($c in $cmds){ $ok=$false; foreach($r in $present){ if(Test-Path -LiteralPath (Join-Path $r ($c + '.exe'))){ $ok=$true; break } }; "
                "if($ok){ Write-Output ('OK:' + $c) } else { Write-Output ('KO:' + $c) } }")
                    .arg(wanted.join(' '));
            if (runSsh(p, roots, 15000, dout, derr, drc) && drc == 0) {
                const QStringList lines = dout.split('\n', Qt::SkipEmptyParts);
                bool noLayer = false;
                for (const QString& raw : lines) {
                    const QString line = raw.trimmed();
                    if (line == QStringLiteral("__NO_UNIX_LAYER__")) {
                        noLayer = true;
                        break;
                    }
                    if (line.startsWith(QStringLiteral("OK:"))) {
                        detected << line.mid(3).trimmed();
                    } else if (line.startsWith(QStringLiteral("KO:"))) {
                        missing << line.mid(3).trimmed();
                    }
                }
                if (!noLayer && (!detected.isEmpty() || !missing.isEmpty())) {
                    state.unixFromMsysOrMingw = true;
                    state.detectedUnixCommands = detected;
                    state.missingUnixCommands = missing;
                } else {
                    state.unixFromMsysOrMingw = false;
                    state.detectedUnixCommands.clear();
                    state.missingUnixCommands.clear();
                }
            }
        } else {
            QString dout, derr;
            int drc = -1;
            const QString checkCmd = QStringLiteral(
                "PATH=\"$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin\"; "
                "for c in %1; do "
                "  found=0; "
                "  command -v \"$c\" >/dev/null 2>&1 && found=1; "
                "  if [ \"$found\" -eq 0 ] && command -v which >/dev/null 2>&1; then "
                "    which \"$c\" >/dev/null 2>&1 && found=1; "
                "  fi; "
                "  if [ \"$found\" -eq 0 ]; then "
                "    for d in /opt/homebrew/bin /opt/homebrew/sbin /usr/local/bin /usr/local/sbin /usr/local/zfs/bin /usr/sbin /sbin /usr/bin /bin; do "
                "      [ -x \"$d/$c\" ] && found=1 && break; "
                "    done; "
                "  fi; "
                "  if [ \"$found\" -eq 1 ]; then echo \"OK:$c\"; else echo \"KO:$c\"; fi; "
                "done")
                    .arg(wanted.join(' '));
            if (runSsh(p, checkCmd, 12000, dout, derr, drc) && drc == 0) {
                const QStringList lines = dout.split('\n', Qt::SkipEmptyParts);
                for (const QString& raw : lines) {
                    const QString line = raw.trimmed();
                    if (line.startsWith(QStringLiteral("OK:"))) {
                        detected << line.mid(3).trimmed();
                    } else if (line.startsWith(QStringLiteral("KO:"))) {
                        missing << line.mid(3).trimmed();
                    }
                }
            } else {
                missing = wanted;
            }
            state.detectedUnixCommands = detected;
            state.missingUnixCommands = missing;
            state.unixFromMsysOrMingw = false;
        }
    }

    QString zpoolListCmd = withSudo(p, QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio"));

    out.clear();
    err.clear();
    rc = -1;
    if (runSsh(p, zpoolListCmd, 18000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString poolName = line.section('\t', 0, 0).trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            state.importedPools.push_back(PoolImported{p.name, poolName, QStringLiteral("Exportar")});
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zpool list -> %2").arg(p.name, oneLine(err)));
    }

    auto parseImportableStructured = [&](const QString& text) -> QVector<PoolImportable> {
        QVector<PoolImportable> rows;
        const QRegularExpression poolNameRx(QStringLiteral("^[A-Za-z0-9_.:-]+$"));
        QString currentPool;
        QString currentState;
        QString currentReason;
        bool collectingStatus = false;
        auto flushCurrent = [&]() {
            if (currentPool.isEmpty()) {
                return;
            }
            if (!poolNameRx.match(currentPool).hasMatch()) {
                currentPool.clear();
                currentState.clear();
                currentReason.clear();
                collectingStatus = false;
                return;
            }
            // Evita falsos positivos: un bloque válido debe tener al menos state o status.
            if (currentState.isEmpty() && currentReason.isEmpty()) {
                currentPool.clear();
                collectingStatus = false;
                return;
            }
            rows.push_back(PoolImportable{
                p.name,
                currentPool,
                currentState.isEmpty() ? QStringLiteral("UNKNOWN") : currentState,
                currentReason,
                QStringLiteral("Importar"),
            });
            currentPool.clear();
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
    };

    const QStringList importProbeArgs = {
        QStringLiteral("zpool import"),
        QStringLiteral("zpool import -s"),
    };
    bool importablesFound = false;
    for (const QString& probe : importProbeArgs) {
        out.clear();
        err.clear();
        rc = -1;
        const QString cmd = withSudo(p, probe);
        if (!runSsh(p, cmd, 18000, out, err, rc)) {
            continue;
        }
        const QString merged = out + QStringLiteral("\n") + err;
        QVector<PoolImportable> parsed = parseImportableStructured(merged);
        if (!parsed.isEmpty()) {
            state.importablePools = parsed;
            importablesFound = true;
            break;
        }
        if (!err.isEmpty()) {
            appLog(QStringLiteral("INFO"), QStringLiteral("%1: %2 -> %3").arg(p.name, probe, oneLine(err)));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    const QString mountedCmd = withSudo(p, QStringLiteral("zfs mount"));
    if (runSsh(p, mountedCmd, 18000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString ln = raw.trimmed();
            if (ln.isEmpty()) {
                continue;
            }
            const int sp = ln.indexOf(' ');
            if (sp <= 0) {
                continue;
            }
            const QString ds = ln.left(sp).trimmed();
            const QString mp = ln.mid(sp + 1).trimmed();
            if (!ds.isEmpty() && !mp.isEmpty()) {
                state.mountedDatasets.push_back(qMakePair(ds, mp));
            }
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zfs mount -> %2").arg(p.name, oneLine(err)));
    }

    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> OK (%2)").arg(p.name, state.detail));
    return state;
}

void MainWindow::populateAllPoolsTables() {
    setTablePopulationMode(m_importedPoolsTable, true);
    m_importedPoolsTable->setRowCount(0);
    for (int i = 0; i < m_states.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& pool : st.importedPools) {
            const int row = m_importedPoolsTable->rowCount();
            m_importedPoolsTable->insertRow(row);
            m_importedPoolsTable->setItem(row, 0, new QTableWidgetItem(pool.connection));
            m_importedPoolsTable->setItem(row, 1, new QTableWidgetItem(pool.pool));
            auto* state = new QTableWidgetItem(QStringLiteral("ONLINE"));
            state->setForeground(QBrush(QColor("#1f7a1f")));
            state->setData(Qt::UserRole + 1, QStringLiteral("Exportar"));
            m_importedPoolsTable->setItem(row, 2, state);
            m_importedPoolsTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("Sí")));
            m_importedPoolsTable->setItem(row, 4, new QTableWidgetItem(QString()));
        }
        for (const PoolImportable& pool : st.importablePools) {
            const int row = m_importedPoolsTable->rowCount();
            m_importedPoolsTable->insertRow(row);
            m_importedPoolsTable->setItem(row, 0, new QTableWidgetItem(pool.connection));
            m_importedPoolsTable->setItem(row, 1, new QTableWidgetItem(pool.pool));
            auto* state = new QTableWidgetItem(pool.state);
            const QString up = pool.state.trimmed().toUpper();
            state->setForeground(QBrush((up == QStringLiteral("ONLINE")) ? QColor("#1f7a1f") : QColor("#a12a2a")));
            const QString action = (up == QStringLiteral("ONLINE")) ? pool.action : QString();
            state->setData(Qt::UserRole + 1, action);
            m_importedPoolsTable->setItem(row, 2, state);
            m_importedPoolsTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("No")));
            m_importedPoolsTable->setItem(row, 4, new QTableWidgetItem(pool.reason));
        }
    }
    setTablePopulationMode(m_importedPoolsTable, false);
    refreshSelectedPoolDetails();
    populateMountedDatasetsTables();
    updatePoolManagementBoxTitle();
}

void MainWindow::populateMountedDatasetsTables() {
    auto fill = [this](QTableWidget* table) {
        if (!table) {
            return;
        }
        setTablePopulationMode(table, true);
        table->setRowCount(0);
        QMap<QString, int> mountpointCountByConn;
        struct RowData {
            QString conn;
            QString dataset;
            QString mountpoint;
        };
        QVector<RowData> allRows;
        for (int i = 0; i < m_states.size() && i < m_profiles.size(); ++i) {
            const QString connName = m_profiles[i].name;
            const auto& rows = m_states[i].mountedDatasets;
            for (const auto& pair : rows) {
                allRows.push_back({connName, pair.first, pair.second});
                mountpointCountByConn[connName + QStringLiteral("::") + pair.second] += 1;
            }
        }
        for (const RowData& row : allRows) {
            const int r = table->rowCount();
            table->insertRow(r);
            auto* dsItem = new QTableWidgetItem(QStringLiteral("%1::%2").arg(row.conn, row.dataset));
            auto* mpItem = new QTableWidgetItem(row.mountpoint);
            const bool duplicated = mountpointCountByConn.value(row.conn + QStringLiteral("::") + row.mountpoint, 0) > 1;
            if (duplicated) {
                const QColor redWarn(QStringLiteral("#b22a2a"));
                dsItem->setForeground(QBrush(redWarn));
                mpItem->setForeground(QBrush(redWarn));
            }
            table->setItem(r, 0, dsItem);
            table->setItem(r, 1, mpItem);
        }
        setTablePopulationMode(table, false);
    };
    fill(m_mountedDatasetsTableLeft);
    fill(m_mountedDatasetsTableAdv);
}

void MainWindow::refreshSelectedPoolDetails() {
    if (!m_poolPropsTable || !m_poolStatusText || !m_importedPoolsTable) {
        return;
    }
    setTablePopulationMode(m_poolPropsTable, true);
    m_poolPropsTable->setRowCount(0);
    m_poolStatusText->clear();
    if (m_poolStatusImportBtn) {
        m_poolStatusImportBtn->setEnabled(false);
    }
    if (m_poolStatusExportBtn) {
        m_poolStatusExportBtn->setEnabled(false);
    }
    if (m_poolStatusScrubBtn) {
        m_poolStatusScrubBtn->setEnabled(false);
    }
    if (m_poolStatusDestroyBtn) {
        m_poolStatusDestroyBtn->setEnabled(false);
    }

    const auto sel = m_importedPoolsTable->selectedItems();
    if (sel.isEmpty()) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const int row = sel.first()->row();
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    if (!connItem || !poolItem) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    const QString poolState = stateItem ? stateItem->text().trimmed().toUpper() : QString();
    const QString action = stateItem ? stateItem->data(Qt::UserRole + 1).toString().trimmed() : QString();
    const bool canExport = (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0);
    const bool canImport = (action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0
                            && poolState == QStringLiteral("ONLINE"));
    const bool canScrub = canExport && poolState == QStringLiteral("ONLINE");
    const bool canDestroy = canExport;
    if (m_poolStatusExportBtn) {
        m_poolStatusExportBtn->setEnabled(!actionsLocked() && canExport);
    }
    if (m_poolStatusImportBtn) {
        m_poolStatusImportBtn->setEnabled(!actionsLocked() && canImport);
    }
    if (m_poolStatusScrubBtn) {
        m_poolStatusScrubBtn->setEnabled(!actionsLocked() && canScrub);
    }
    if (m_poolStatusDestroyBtn) {
        m_poolStatusDestroyBtn->setEnabled(!actionsLocked() && canDestroy);
    }
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    if (!canExport) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0 || idx >= m_profiles.size()) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];

    QString out;
    QString err;
    int rc = -1;
    const QString propsCmd = withSudo(
        p, QStringLiteral("zpool get -H -o property,value,source all %1").arg(shSingleQuote(poolName)));
    if (runSsh(p, propsCmd, 20000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList parts = line.split('\t');
            if (parts.size() < 3) {
                continue;
            }
            const int r = m_poolPropsTable->rowCount();
            m_poolPropsTable->insertRow(r);
            m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(parts[0].trimmed()));
            m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(parts[1].trimmed()));
            m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(parts[2].trimmed()));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    const QString stCmd = withSudo(
        p, QStringLiteral("zpool status -v %1").arg(shSingleQuote(poolName)));
    if (runSsh(p, stCmd, 20000, out, err, rc) && rc == 0) {
        m_poolStatusText->setPlainText(out.trimmed());
    } else {
        m_poolStatusText->setPlainText(err.trimmed());
    }
    setTablePopulationMode(m_poolPropsTable, false);
}

void MainWindow::updateStatus(const QString& text) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, text]() {
            updateStatus(text);
        }, Qt::QueuedConnection);
        return;
    }
    if (m_statusText) {
        m_statusText->setPlainText(maskSecrets(text));
    }
}

bool MainWindow::actionsLocked() const {
    return m_actionsLocked;
}

void MainWindow::requestCancelRunningAction() {
    if (!m_actionsLocked || !m_activeLocalProcess) {
        return;
    }
    m_cancelActionRequested = true;
}

void MainWindow::terminateProcessTree(qint64 rootPid) {
    if (rootPid <= 0) {
        return;
    }
    QProcess::execute(QStringLiteral("sh"), QStringList{
        QStringLiteral("-lc"),
        QStringLiteral("pkill -TERM -P %1 >/dev/null 2>&1 || true; sleep 0.2; pkill -KILL -P %1 >/dev/null 2>&1 || true")
            .arg(rootPid)
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_actionsLocked) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Hay una acción en ejecución. Cancele la acción antes de cerrar la aplicación."),
                QStringLiteral("An action is running. Cancel it before closing the application."),
                QStringLiteral("当前有操作正在执行。请先取消操作再关闭应用。")));
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

QString MainWindow::buildSshPreviewCommand(const ConnectionProfile& p, const QString& remoteCmd) const {
    QStringList parts;
    parts << QStringLiteral("ssh");
    parts << QStringLiteral("-o BatchMode=yes");
    parts << QStringLiteral("-o ConnectTimeout=10");
    parts << QStringLiteral("-o LogLevel=ERROR");
    parts << QStringLiteral("-o StrictHostKeyChecking=no");
    parts << QStringLiteral("-o UserKnownHostsFile=/dev/null");
    parts << QStringLiteral("-o ControlMaster=auto");
    parts << QStringLiteral("-o ControlPersist=300");
    parts << QStringLiteral("-o ControlPath=%1").arg(shSingleQuote(sshControlPath()));
    if (p.port > 0) {
        parts << QStringLiteral("-p %1").arg(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        parts << QStringLiteral("-i %1").arg(shSingleQuote(p.keyPath));
    }
    parts << QStringLiteral("%1@%2").arg(p.username, p.host);
    parts << shSingleQuote(remoteCmd);
    return parts.join(' ');
}

bool MainWindow::confirmActionExecution(const QString& actionName, const QStringList& commands, bool forceDialog) {
    if (!forceDialog && !m_actionConfirmEnabled) {
        return true;
    }
    if (commands.isEmpty()) {
        return true;
    }
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(980, 520);
    dlg.setWindowTitle(tr3(QStringLiteral("Confirmar ejecución"), QStringLiteral("Confirm execution"), QStringLiteral("确认执行")));

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    QLabel* intro = new QLabel(
        tr3(QStringLiteral("Se van a ejecutar estos comandos para la acción: %1")
                .arg(actionName),
            QStringLiteral("These commands will be executed for action: %1")
                .arg(actionName),
            QStringLiteral("将为该操作执行以下命令：%1")
                .arg(actionName)),
        &dlg);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QStringList rendered;
    rendered.reserve(commands.size());
    for (const QString& cmd : commands) {
        rendered.push_back(formatCommandPreview(cmd));
    }

    QPlainTextEdit* txt = new QPlainTextEdit(&dlg);
    txt->setReadOnly(true);
    txt->setPlainText(rendered.join(QStringLiteral("\n\n")));
    root->addWidget(txt, 1);

    QDialogButtonBox* box = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = box->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* okBtn = box->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(box);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    const bool accepted = (dlg.exec() == QDialog::Accepted);
    if (!accepted) {
        appLog(QStringLiteral("INFO"), tr3(QStringLiteral("Acción cancelada por el usuario: %1").arg(actionName),
                                           QStringLiteral("Action canceled by user: %1").arg(actionName),
                                           QStringLiteral("用户已取消操作：%1").arg(actionName)));
    }
    return accepted;
}

bool MainWindow::selectItemsDialog(const QString& title, const QString& intro, const QStringList& items, QStringList& selected) {
    selected.clear();
    if (items.isEmpty()) {
        return false;
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(640, 520);
    dlg.setWindowTitle(title);
    QVBoxLayout* root = new QVBoxLayout(&dlg);

    QLabel* introLbl = new QLabel(intro, &dlg);
    introLbl->setWordWrap(true);
    root->addWidget(introLbl);

    QListWidget* list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    for (const QString& item : items) {
        auto* lw = new QListWidgetItem(item, list);
        lw->setFlags(lw->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        lw->setCheckState(Qt::Checked);
    }
    root->addWidget(list, 1);

    QHBoxLayout* tools = new QHBoxLayout();
    QPushButton* allBtn = new QPushButton(tr3(QStringLiteral("Seleccionar todo"), QStringLiteral("Select all"), QStringLiteral("全选")), &dlg);
    QPushButton* noneBtn = new QPushButton(tr3(QStringLiteral("Deseleccionar todo"), QStringLiteral("Clear all"), QStringLiteral("全不选")), &dlg);
    tools->addWidget(allBtn);
    tools->addWidget(noneBtn);
    tools->addStretch(1);
    root->addLayout(tools);

    QObject::connect(allBtn, &QPushButton::clicked, &dlg, [list]() {
        for (int i = 0; i < list->count(); ++i) {
            list->item(i)->setCheckState(Qt::Checked);
        }
    });
    QObject::connect(noneBtn, &QPushButton::clicked, &dlg, [list]() {
        for (int i = 0; i < list->count(); ++i) {
            list->item(i)->setCheckState(Qt::Unchecked);
        }
    });

    QDialogButtonBox* box = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = box->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* okBtn = box->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(box);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem* it = list->item(i);
        if (it && it->checkState() == Qt::Checked) {
            selected.push_back(it->text());
        }
    }
    return !selected.isEmpty();
}

void MainWindow::openConfigurationDialog() {
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(tr3(QStringLiteral("Configuración"), QStringLiteral("Configuration"), QStringLiteral("配置")));
    dlg.resize(500, 240);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    QFormLayout* form = new QFormLayout();

    QComboBox* langCombo = new QComboBox(&dlg);
    langCombo->addItem(QStringLiteral("Español"), QStringLiteral("es"));
    langCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    langCombo->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    int idx = langCombo->findData(m_language);
    langCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    form->addRow(tr3(QStringLiteral("Idioma"), QStringLiteral("Language"), QStringLiteral("语言")), langCombo);

    QCheckBox* confirmChk = new QCheckBox(
        tr3(QStringLiteral("Mostrar confirmación antes de ejecutar acciones"),
            QStringLiteral("Show confirmation before executing actions"),
            QStringLiteral("执行操作前显示确认")),
        &dlg);
    confirmChk->setChecked(m_actionConfirmEnabled);
    form->addRow(QString(), confirmChk);

    QSpinBox* logSizeSpin = new QSpinBox(&dlg);
    logSizeSpin->setRange(1, 1024);
    logSizeSpin->setSuffix(QStringLiteral(" MB"));
    logSizeSpin->setValue(qMax(1, m_logMaxSizeMb));
    form->addRow(tr3(QStringLiteral("Tamaño máximo log rotativo"),
                     QStringLiteral("Max rotating log size"),
                     QStringLiteral("滚动日志最大大小")),
                 logSizeSpin);

    root->addLayout(form);
    root->addStretch(1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* okBtn = buttons->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString newLang = langCombo->currentData().toString().trimmed().toLower();
    const bool newConfirm = confirmChk->isChecked();
    const int newLogMaxMb = qMax(1, logSizeSpin->value());
    const bool langChanged = (newLang != m_language);
    m_language = newLang.isEmpty() ? QStringLiteral("es") : newLang;
    m_actionConfirmEnabled = newConfirm;
    m_logMaxSizeMb = newLogMaxMb;
    saveUiSettings();
    appLog(QStringLiteral("INFO"),
           QStringLiteral("Configuración actualizada: idioma=%1, confirmación=%2, log_max_mb=%3")
               .arg(m_language,
                    m_actionConfirmEnabled ? QStringLiteral("on") : QStringLiteral("off"),
                    QString::number(m_logMaxSizeMb)));
    if (langChanged) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Idioma guardado. Se aplicará completamente al reiniciar la aplicación."),
                QStringLiteral("Language saved. It will be fully applied after restarting the application."),
                QStringLiteral("语言已保存。重启应用后将完全生效。")));
    }
}

void MainWindow::beginUiBusy() {
    ++m_uiBusyDepth;
    updateBusyCursor();
}

void MainWindow::endUiBusy() {
    if (m_uiBusyDepth > 0) {
        --m_uiBusyDepth;
    }
    updateBusyCursor();
}

void MainWindow::updateBusyCursor() {
    const bool shouldShow = m_actionsLocked || m_refreshInProgress || (m_uiBusyDepth > 0);
    if (shouldShow) {
        if (!m_waitCursorActive) {
            QApplication::setOverrideCursor(Qt::BusyCursor);
            m_waitCursorActive = true;
        }
    } else if (m_waitCursorActive) {
        QApplication::restoreOverrideCursor();
        m_waitCursorActive = false;
    }
}

void MainWindow::setActionsLocked(bool locked) {
    m_actionsLocked = locked;
    updateBusyCursor();
    if (m_logCancelBtn) {
        m_logCancelBtn->setVisible(locked);
        m_logCancelBtn->setEnabled(locked);
    }
    if (m_btnNew) m_btnNew->setEnabled(!locked);
    if (m_btnRefreshAll) m_btnRefreshAll->setEnabled(!locked);
    if (m_btnConfig) m_btnConfig->setEnabled(!locked);
    if (m_btnPoolNew) m_btnPoolNew->setEnabled(!locked && selectedConnectionIndexForPoolManagement() >= 0);
    if (m_poolStatusRefreshBtn) m_poolStatusRefreshBtn->setEnabled(!locked);
    if (m_poolStatusImportBtn) m_poolStatusImportBtn->setEnabled(!locked && m_poolStatusImportBtn->isEnabled());
    if (m_poolStatusExportBtn) m_poolStatusExportBtn->setEnabled(!locked && m_poolStatusExportBtn->isEnabled());
    if (m_poolStatusScrubBtn) m_poolStatusScrubBtn->setEnabled(!locked && m_poolStatusScrubBtn->isEnabled());
    if (m_poolStatusDestroyBtn) m_poolStatusDestroyBtn->setEnabled(!locked && m_poolStatusDestroyBtn->isEnabled());
    if (m_btnApplyDatasetProps) m_btnApplyDatasetProps->setEnabled(!locked && m_btnApplyDatasetProps->isEnabled());
    if (m_btnApplyAdvancedProps) m_btnApplyAdvancedProps->setEnabled(!locked && m_btnApplyAdvancedProps->isEnabled());
    if (locked) {
        if (m_btnCopy) m_btnCopy->setEnabled(false);
        if (m_btnLevel) m_btnLevel->setEnabled(false);
        if (m_btnSync) m_btnSync->setEnabled(false);
        if (m_btnAdvancedBreakdown) m_btnAdvancedBreakdown->setEnabled(false);
        if (m_btnAdvancedAssemble) m_btnAdvancedAssemble->setEnabled(false);
        if (m_btnAdvancedFromDir) m_btnAdvancedFromDir->setEnabled(false);
        if (m_btnAdvancedToDir) m_btnAdvancedToDir->setEnabled(false);
    } else {
        updateTransferButtonsState();
        updateApplyPropsButtonState();
        refreshSelectedPoolDetails();
        updatePoolManagementBoxTitle();
    }
}

QString MainWindow::maskSecrets(const QString& text) const {
    if (text.isEmpty()) {
        return text;
    }
    QString out = text;
    out.replace(
        QRegularExpression(QStringLiteral("printf\\s+'%s\\\\n'\\s+.+?\\s+\\|\\s+sudo\\s+-S\\s+-p\\s+''")),
        QStringLiteral("printf '%s\\n' [secret] | sudo -S -p ''"));
    out.replace(
        QRegularExpression(QStringLiteral("(?i)(password\\s*[:=]\\s*)\\S+")),
        QStringLiteral("\\1[secret]"));
    for (const ConnectionProfile& p : m_profiles) {
        if (!p.password.isEmpty()) {
            out.replace(p.password, QStringLiteral("[secret]"));
        }
    }
    return out;
}

void MainWindow::logUiAction(const QString& action) {
    appLog(QStringLiteral("INFO"), QStringLiteral("UI action: %1").arg(action));
}

void MainWindow::appLog(const QString& level, const QString& msg) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, level, msg]() {
            appLog(level, msg);
        }, Qt::QueuedConnection);
        return;
    }
    const QString line = QStringLiteral("[%1] [%2] %3").arg(tsNow(), level, maskSecrets(msg));
    const QString current = m_logLevelCombo ? m_logLevelCombo->currentText().toLower() : QStringLiteral("normal");
    auto rank = [](const QString& l) -> int {
        const QString x = l.toLower();
        if (x == QStringLiteral("debug")) {
            return 2;
        }
        if (x == QStringLiteral("info")) {
            return 1;
        }
        return 0;
    };
    const QString lvl = level.toLower();
    const bool always = (lvl == QStringLiteral("warn") || lvl == QStringLiteral("error"));
    if (always || rank(lvl) <= rank(current)) {
        m_logView->appendPlainText(line);
        trimLogWidget(m_logView);
    }
    if (m_lastDetailText) {
        m_lastDetailText->setPlainText(line);
    }
    appendLogToFile(line);
}

int MainWindow::maxLogLines() const {
    bool ok = false;
    const int v = m_logMaxLinesCombo ? m_logMaxLinesCombo->currentText().toInt(&ok) : 500;
    if (!ok || v <= 0) {
        return 500;
    }
    return v;
}

void MainWindow::trimLogWidget(QPlainTextEdit* widget) {
    if (!widget) {
        return;
    }
    QTextDocument* doc = widget->document();
    if (!doc) {
        return;
    }
    const int limit = maxLogLines();
    while (doc->blockCount() > limit) {
        QTextCursor c(doc);
        c.movePosition(QTextCursor::Start);
        c.select(QTextCursor::LineUnderCursor);
        c.removeSelectedText();
        c.deleteChar();
    }
}

void MainWindow::syncConnectionLogTabs() {
    if (!m_logsTabs) {
        return;
    }
    QSet<QString> wanted;
    for (const auto& p : m_profiles) {
        wanted.insert(p.id);
        if (m_connectionLogViews.contains(p.id)) {
            continue;
        }
        auto* tab = new QWidget(m_logsTabs);
        auto* lay = new QVBoxLayout(tab);
        auto* view = new QPlainTextEdit(tab);
        view->setReadOnly(true);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSize(8);
        view->setFont(mono);
        lay->addWidget(view, 1);
        m_logsTabs->addTab(tab, p.name);
        m_connectionLogViews.insert(p.id, view);
    }

    for (auto it = m_connectionLogViews.begin(); it != m_connectionLogViews.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        QWidget* tab = it.value() ? it.value()->parentWidget() : nullptr;
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->removeTab(idx);
        }
        if (tab) {
            tab->deleteLater();
        }
        it = m_connectionLogViews.erase(it);
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        QWidget* tab = m_connectionLogViews.value(m_profiles[i].id)
                           ? m_connectionLogViews.value(m_profiles[i].id)->parentWidget()
                           : nullptr;
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->setTabText(idx, m_profiles[i].name);
        }
    }
}

void MainWindow::appendConnectionLog(const QString& connId, const QString& line) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, connId, line]() {
            appendConnectionLog(connId, line);
        }, Qt::QueuedConnection);
        return;
    }
    QPlainTextEdit* view = m_connectionLogViews.value(connId, nullptr);
    if (!view) {
        return;
    }
    view->appendPlainText(QStringLiteral("[%1] %2").arg(tsNow(), maskSecrets(line)));
    trimLogWidget(view);
}
