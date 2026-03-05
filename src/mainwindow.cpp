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

void MainWindow::createConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: nueva conexión bloqueada"));
        return;
    }
    ConnectionDialog dlg(m_language, this);
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
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("No se pudo crear conexión:\n%1"),
                                  QStringLiteral("Could not create connection:\n%1"),
                                  QStringLiteral("无法创建连接：\n%1")).arg(err));
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
    ConnectionDialog dlg(m_language, this);
    dlg.setProfile(m_profiles[idx]);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile edited = dlg.profile();
    edited.id = m_profiles[idx].id;
    QString err;
    if (!m_store.upsertConnection(edited, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("No se pudo actualizar conexión:\n%1"),
                                  QStringLiteral("Could not update connection:\n%1"),
                                  QStringLiteral("无法更新连接：\n%1")).arg(err));
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
        tr3(QStringLiteral("Borrar conexión"), QStringLiteral("Delete connection"), QStringLiteral("删除连接")),
        tr3(QStringLiteral("¿Borrar conexión \"%1\"?"),
            QStringLiteral("Delete connection \"%1\"?"),
            QStringLiteral("删除连接“%1”？")).arg(m_profiles[idx].name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    QString err;
    if (!m_store.deleteConnectionById(m_profiles[idx].id, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("No se pudo borrar conexión:\n%1"),
                                  QStringLiteral("Could not delete connection:\n%1"),
                                  QStringLiteral("无法删除连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
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
        rows.push_back({QStringLiteral("estado"),
                        mountedYes ? tr3(QStringLiteral("Montado"), QStringLiteral("Mounted"), QStringLiteral("已挂载"))
                                   : tr3(QStringLiteral("Desmontado"), QStringLiteral("Unmounted"), QStringLiteral("未挂载")),
                        QString(),
                        QStringLiteral("true")});
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
        appLog(QStringLiteral("NORMAL"),
               tr3(QStringLiteral("No se pudo iniciar comando local"),
                   QStringLiteral("Could not start local command"),
                   QStringLiteral("无法启动本地命令")));
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
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Copiar: modo local remoto (origen y destino en la misma conexión)"),
                   QStringLiteral("Copy: remote-local mode (source and target on same connection)"),
                   QStringLiteral("复制：远端本地模式（源和目标在同一连接）")));
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
        appLog(QStringLiteral("INFO"), tr3(QStringLiteral("Nivelar omitido: selección incompleta (src.valid=%1 dst.valid=%2 src.dataset=%3 dst.dataset=%4 dst.snap=%5)"),
                                           QStringLiteral("Level skipped: incomplete selection (src.valid=%1 dst.valid=%2 src.dataset=%3 dst.dataset=%4 dst.snap=%5)"),
                                           QStringLiteral("同步快照已跳过：选择不完整（src.valid=%1 dst.valid=%2 src.dataset=%3 dst.dataset=%4 dst.snap=%5）"))
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
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Nivelar: modo local remoto (origen y destino en la misma conexión)"),
                   QStringLiteral("Level: remote-local mode (source and target on same connection)"),
                   QStringLiteral("同步快照：远端本地模式（源和目标在同一连接）")));
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
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudieron leer mountpoints para sincronizar."),
                                 QStringLiteral("Could not read mountpoints for synchronization."),
                                 QStringLiteral("无法读取用于同步的挂载点。")));
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
                                 tr3(QStringLiteral("No se pudo resolver el punto de montaje efectivo para sincronizar.\n"
                                                    "Origen: %1\nDestino: %2"),
                                     QStringLiteral("Could not resolve effective mountpoint for synchronization.\n"
                                                    "Source: %1\nTarget: %2"),
                                     QStringLiteral("无法解析用于同步的有效挂载点。\n"
                                                    "源：%1\n目标：%2"))
                                     .arg(srcEffectiveMp, dstEffectiveMp));
            return;
        }
        if (isWindowsConnection(sp) || isWindowsConnection(dp)) {
            const StreamCodec codec = chooseCodec();
            const QString srcTarCmd = buildSrcTarCmd(isWindowsConnection(sp), srcEffectiveMp, codec);
            const QString dstTarCmd = buildDstTarCmd(isWindowsConnection(dp), dstEffectiveMp, codec);
            QString command;
            if (sameConnection) {
                appLog(QStringLiteral("INFO"),
                       tr3(QStringLiteral("Sincronizar: modo local remoto (tar, misma conexión)"),
                           QStringLiteral("Sync: remote-local mode (tar, same connection)"),
                           QStringLiteral("同步：远端本地模式（tar，同一连接）")));
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
                   tr3(QStringLiteral("Sincronizar en Windows usa fallback tar/ssh (codec=%1, sin --delete)."),
                       QStringLiteral("Sync on Windows uses tar/ssh fallback (codec=%1, no --delete)."),
                       QStringLiteral("Windows 上同步使用 tar/ssh 回退（编码=%1，无 --delete）。")).arg(codecName(codec)));
            if (runLocalCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command, 0, false, true)) {
                invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
                reloadDatasetSide(QStringLiteral("dest"));
            }
            return;
        }
        QString command;
        if (sameConnection) {
            appLog(QStringLiteral("INFO"),
                   tr3(QStringLiteral("Sincronizar: modo local remoto (rsync, misma conexión)"),
                       QStringLiteral("Sync: remote-local mode (rsync, same connection)"),
                       QStringLiteral("同步：远端本地模式（rsync，同一连接）")));
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
                             tr3(QStringLiteral("Origen y destino deben estar montados para sincronizar,\n"
                                                "o bien tener canmount=off con subdatasets montados.\n"
                                                "Origen mounted=%1 canmount=%2\nDestino mounted=%3 canmount=%4"),
                                 QStringLiteral("Source and target must be mounted to synchronize,\n"
                                                "or canmount=off with mounted subdatasets.\n"
                                                "Source mounted=%1 canmount=%2\nTarget mounted=%3 canmount=%4"),
                                 QStringLiteral("源和目标必须已挂载才能同步，\n"
                                                "或设置 canmount=off 且子数据集已挂载。\n"
                                                "源 mounted=%1 canmount=%2\n目标 mounted=%3 canmount=%4"))
                                 .arg(srcMounted, srcCanmount, dstMounted, dstCanmount));
        return;
    }

    if (!ensureDatasetsLoaded(src.connIdx, src.poolName) || !ensureDatasetsLoaded(dst.connIdx, dst.poolName)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudieron cargar datasets para sincronización por subdatasets."),
                                 QStringLiteral("Could not load datasets for subdataset synchronization."),
                                 QStringLiteral("无法加载用于子数据集同步的数据集。")));
        return;
    }
    const QString srcKey = datasetCacheKey(src.connIdx, src.poolName);
    const QString dstKey = datasetCacheKey(dst.connIdx, dst.poolName);
    const auto srcCacheIt = m_poolDatasetCache.constFind(srcKey);
    const auto dstCacheIt = m_poolDatasetCache.constFind(dstKey);
    if (srcCacheIt == m_poolDatasetCache.constEnd() || dstCacheIt == m_poolDatasetCache.constEnd()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No hay caché de datasets para sincronización por subdatasets."),
                                 QStringLiteral("No dataset cache for subdataset synchronization."),
                                 QStringLiteral("没有用于子数据集同步的数据集缓存。")));
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
                             tr3(QStringLiteral("No se puede sincronizar por subdatasets.\n%1"),
                                 QStringLiteral("Cannot synchronize by subdatasets.\n%1"),
                                 QStringLiteral("无法按子数据集同步。\n%1")).arg(details.trimmed()));
        return;
    }
    if (syncPairs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No hay subdatasets montados equivalentes para sincronizar."),
                                     QStringLiteral("No equivalent mounted subdatasets to synchronize."),
                                     QStringLiteral("没有可同步的等效已挂载子数据集。")));
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
               tr3(QStringLiteral("Sincronizar subdatasets en Windows usa fallback tar/ssh (codec=%1, sin --delete)."),
                   QStringLiteral("Subdataset sync on Windows uses tar/ssh fallback (codec=%1, no --delete)."),
                   QStringLiteral("Windows 上子数据集同步使用 tar/ssh 回退（编码=%1，无 --delete）。")).arg(codecName(codec)));
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
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
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
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Desglosar cancelado o sin selección."),
                   QStringLiteral("Break down canceled or no selection."),
                   QStringLiteral("拆分已取消或无选择。")));
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
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
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
        appLog(QStringLiteral("INFO"),
               tr3(QStringLiteral("Ensamblar cancelado o sin selección."),
                   QStringLiteral("Assemble canceled or no selection."),
                   QStringLiteral("组装已取消或无选择。")));
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
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("Seleccione un dataset activo para aplicar cambios."),
                                 QStringLiteral("Select an active dataset to apply changes."),
                                 QStringLiteral("请选择一个活动数据集以应用更改。")));
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
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("Seleccione un dataset activo para aplicar cambios."),
                                 QStringLiteral("Select an active dataset to apply changes."),
                                 QStringLiteral("请选择一个活动数据集以应用更改。")));
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


int MainWindow::findConnectionIndexByName(const QString& name) const {
    const QString key = name.trimmed();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].name.compare(key, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
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
    QAction* mountAct = menu.addAction(tr3(QStringLiteral("Montar"), QStringLiteral("Mount"), QStringLiteral("挂载")));
    QAction* mountWithChildrenAct = menu.addAction(tr3(QStringLiteral("Montar con todos los hijos"),
                                                       QStringLiteral("Mount with all children"),
                                                       QStringLiteral("挂载并包含所有子项")));
    QAction* umountAct = menu.addAction(tr3(QStringLiteral("Desmontar"), QStringLiteral("Unmount"), QStringLiteral("卸载")));
    QAction* rollbackAct = nullptr;
    if (!ctx.snapshotName.isEmpty()) {
        rollbackAct = menu.addAction(QStringLiteral("Rollback"));
    }
    menu.addSeparator();
    QAction* createAct = menu.addAction(tr3(QStringLiteral("Crear hijo"), QStringLiteral("Create child"), QStringLiteral("创建子项")));
    QAction* deleteAct = menu.addAction(tr3(QStringLiteral("Borrar"), QStringLiteral("Delete"), QStringLiteral("删除")));
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
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              tr3(QStringLiteral("%1 falló:\n%2"),
                                  QStringLiteral("%1 failed:\n%2"),
                                  QStringLiteral("%1 失败：\n%2"))
                                  .arg(actionName, failureDetail));
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
        tr3(QStringLiteral("Confirmar borrado"), QStringLiteral("Confirm deletion"), QStringLiteral("确认删除")),
        tr3(QStringLiteral("Se va a borrar:\n%1\n¿Continuar?"),
            QStringLiteral("This will be deleted:\n%1\nContinue?"),
            QStringLiteral("将要删除：\n%1\n是否继续？")).arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm1 != QMessageBox::Yes) {
        return;
    }
    const auto confirm2 = QMessageBox::question(
        this,
        tr3(QStringLiteral("Confirmar borrado (2/2)"), QStringLiteral("Confirm deletion (2/2)"), QStringLiteral("确认删除（2/2）")),
        tr3(QStringLiteral("Confirmación final de borrado:\n%1"),
            QStringLiteral("Final deletion confirmation:\n%1"),
            QStringLiteral("最终删除确认：\n%1")).arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm2 != QMessageBox::Yes) {
        return;
    }

    const auto askRec = QMessageBox::question(
        this,
        tr3(QStringLiteral("Borrado recursivo"), QStringLiteral("Recursive deletion"), QStringLiteral("递归删除")),
        ctx.snapshotName.isEmpty()
            ? tr3(QStringLiteral("¿Borrar recursivamente datasets/snapshots hijos?"),
                  QStringLiteral("Delete child datasets/snapshots recursively?"),
                  QStringLiteral("是否递归删除子数据集/快照？"))
            : tr3(QStringLiteral("¿Borrar recursivamente este snapshot en hijos descendientes?"),
                  QStringLiteral("Delete this snapshot recursively on descendant children?"),
                  QStringLiteral("是否在后代子项上递归删除此快照？")),
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
    appLog(QStringLiteral("NORMAL"),
           tr3(QStringLiteral("Inicio refresh: %1 [%2]"),
               QStringLiteral("Refresh start: %1 [%2]"),
               QStringLiteral("开始刷新：%1 [%2]")).arg(p.name, p.connType));

    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = tr3(QStringLiteral("Tipo de conexión no soportado aún en cppqt"),
                           QStringLiteral("Connection type not supported yet in cppqt"),
                           QStringLiteral("cppqt 尚不支持该连接类型"));
        appLog(QStringLiteral("NORMAL"),
               tr3(QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(p.name, state.detail));
        return state;
    }
    if (p.host.isEmpty() || p.username.isEmpty()) {
        state.status = QStringLiteral("ERROR");
        state.detail = tr3(QStringLiteral("Host/usuario no definido"),
                           QStringLiteral("Host/user not defined"),
                           QStringLiteral("主机/用户未定义"));
        appLog(QStringLiteral("NORMAL"),
               tr3(QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(p.name, state.detail));
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
        appLog(QStringLiteral("NORMAL"),
               tr3(QStringLiteral("Fin refresh: %1 -> ERROR (%2)"),
                   QStringLiteral("Refresh end: %1 -> ERROR (%2)"),
                   QStringLiteral("刷新结束：%1 -> ERROR (%2)")).arg(p.name, state.detail));
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

    appLog(QStringLiteral("NORMAL"),
           tr3(QStringLiteral("Fin refresh: %1 -> OK (%2)"),
               QStringLiteral("Refresh end: %1 -> OK (%2)"),
               QStringLiteral("刷新结束：%1 -> OK (%2)")).arg(p.name, state.detail));
    return state;
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
