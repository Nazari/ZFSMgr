#include "mainwindow.h"
#include "i18nmanager.h"

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

QString tr3l(const QString& lang, const QString& es, const QString& en, const QString& zh) {
    return I18nManager::instance().translate(lang, es, en, zh);
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
#ifdef Q_OS_MAC
    // macOS impone un límite corto para sockets Unix; usar /tmp evita rutas largas en /var/folders/...
    return QStringLiteral("/tmp/zfsmgr-%C");
#else
    return QDir::tempPath() + QStringLiteral("/zfsmgr-ssh-%C");
#endif
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

QString pseudoStepForSegment(const QString& segmentRaw, const QString& lang) {
    const QString segment = segmentRaw.trimmed();
    const QString s = segment.toLower();
    if (s.contains(QStringLiteral("ssh "))) {
        if (s.contains(QStringLiteral("zfs send"))) {
            return tr3l(lang,
                        QStringLiteral("Conectar por SSH al origen y enviar stream ZFS (`zfs send`)."),
                        QStringLiteral("Connect over SSH to source and send ZFS stream (`zfs send`)."),
                        QStringLiteral("通过 SSH 连接源端并发送 ZFS 数据流（`zfs send`）。"));
        }
        if (s.contains(QStringLiteral("zfs recv"))) {
            return tr3l(lang,
                        QStringLiteral("Conectar por SSH al destino y recibir stream ZFS (`zfs recv`)."),
                        QStringLiteral("Connect over SSH to target and receive ZFS stream (`zfs recv`)."),
                        QStringLiteral("通过 SSH 连接目标端并接收 ZFS 数据流（`zfs recv`）。"));
        }
        if (s.contains(QStringLiteral("zpool export"))) {
            return tr3l(lang,
                        QStringLiteral("Conectar por SSH y exportar pool (`zpool export`)."),
                        QStringLiteral("Connect over SSH and export pool (`zpool export`)."),
                        QStringLiteral("通过 SSH 连接并导出池（`zpool export`）。"));
        }
        if (s.contains(QStringLiteral("zpool import"))) {
            return tr3l(lang,
                        QStringLiteral("Conectar por SSH e importar pool (`zpool import`)."),
                        QStringLiteral("Connect over SSH and import pool (`zpool import`)."),
                        QStringLiteral("通过 SSH 连接并导入池（`zpool import`）。"));
        }
        return tr3l(lang,
                    QStringLiteral("Conectar por SSH y ejecutar comando remoto."),
                    QStringLiteral("Connect over SSH and execute remote command."),
                    QStringLiteral("通过 SSH 连接并执行远程命令。"));
    }
    if (s.contains(QStringLiteral("pv -trab"))) {
        return tr3l(lang,
                    QStringLiteral("Mostrar progreso de transferencia con `pv`."),
                    QStringLiteral("Show transfer progress with `pv`."),
                    QStringLiteral("用 `pv` 显示传输进度。"));
    }
    if (s.contains(QStringLiteral("zfs send"))) {
        return tr3l(lang,
                    QStringLiteral("Generar stream ZFS desde snapshot/dataset (`zfs send`)."),
                    QStringLiteral("Generate ZFS stream from snapshot/dataset (`zfs send`)."),
                    QStringLiteral("从快照/数据集生成 ZFS 数据流（`zfs send`）。"));
    }
    if (s.contains(QStringLiteral("zfs recv"))) {
        return tr3l(lang,
                    QStringLiteral("Aplicar stream ZFS en destino (`zfs recv`)."),
                    QStringLiteral("Apply ZFS stream on target (`zfs recv`)."),
                    QStringLiteral("在目标端应用 ZFS 数据流（`zfs recv`）。"));
    }
    if (s.contains(QStringLiteral("zfs rollback"))) {
        return tr3l(lang,
                    QStringLiteral("Revertir dataset al snapshot seleccionado (`zfs rollback`)."),
                    QStringLiteral("Rollback dataset to selected snapshot (`zfs rollback`)."),
                    QStringLiteral("将数据集回滚到选定快照（`zfs rollback`）。"));
    }
    if (s.contains(QStringLiteral("zfs mount")) || s.contains(QStringLiteral("zfs unmount"))) {
        return tr3l(lang,
                    QStringLiteral("Montar/desmontar dataset ZFS."),
                    QStringLiteral("Mount/unmount ZFS dataset."),
                    QStringLiteral("挂载/卸载 ZFS 数据集。"));
    }
    if (s.contains(QStringLiteral("zfs set ")) || s.contains(QStringLiteral("zfs get "))) {
        return tr3l(lang,
                    QStringLiteral("Modificar/consultar propiedades ZFS."),
                    QStringLiteral("Modify/query ZFS properties."),
                    QStringLiteral("修改/查询 ZFS 属性。"));
    }
    if (s.contains(QStringLiteral("powershell "))) {
        return tr3l(lang,
                    QStringLiteral("Ejecutar script PowerShell."),
                    QStringLiteral("Execute PowerShell script."),
                    QStringLiteral("执行 PowerShell 脚本。"));
    }
    if (s.contains(QStringLiteral("sudo "))) {
        return tr3l(lang,
                    QStringLiteral("Elevar permisos con sudo y ejecutar comando."),
                    QStringLiteral("Elevate with sudo and execute command."),
                    QStringLiteral("通过 sudo 提权并执行命令。"));
    }
    return tr3l(lang,
                QStringLiteral("Ejecutar subcomando: %1"),
                QStringLiteral("Execute subcommand: %1"),
                QStringLiteral("执行子命令：%1")).arg(segment.left(120));
}

QString formatCommandPreview(const QString& input, const QString& lang) {
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
        pseudo.push_back(pseudoStepForSegment(seg, lang));
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
    out.push_back(tr3l(lang,
                       QStringLiteral("Resumen legible:"),
                       QStringLiteral("Readable summary:"),
                       QStringLiteral("可读摘要：")));
    if (pseudo.isEmpty()) {
        out.push_back(tr3l(lang,
                           QStringLiteral("  1. Ejecutar comando."),
                           QStringLiteral("  1. Execute command."),
                           QStringLiteral("  1. 执行命令。")));
    } else {
        for (int i = 0; i < pseudo.size(); ++i) {
            out.push_back(QStringLiteral("  %1. %2").arg(i + 1).arg(pseudo[i]));
        }
    }

    if (!decodedBlocks.isEmpty()) {
        out.push_back(QStringLiteral(""));
        out.push_back(tr3l(lang,
                           QStringLiteral("PowerShell decodificado:"),
                           QStringLiteral("Decoded PowerShell:"),
                           QStringLiteral("解码后的 PowerShell：")));
        for (int i = 0; i < decodedBlocks.size(); ++i) {
            out.push_back(QStringLiteral("  [script %1]").arg(i + 1));
            out.push_back(QStringLiteral("  ") + decodedBlocks[i]);
        }
    }

    out.push_back(QStringLiteral(""));
    out.push_back(tr3l(lang,
                       QStringLiteral("Comando real (formateado):"),
                       QStringLiteral("Actual command (formatted):"),
                       QStringLiteral("实际命令（已格式化）：")));
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
    m_store.setLanguage(m_language);
    m_store.setMasterPassword(masterPassword);
    initLogPersistence();
    buildUi();
    loadConnections();
    QTimer::singleShot(0, this, [this]() {
        refreshAllConnections();
    });
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

