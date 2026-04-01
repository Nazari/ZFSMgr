#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSettings>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;
using mwhelpers::sshUserHostPort;
using mwhelpers::formatWindowsFsTypeDetail;
using mwhelpers::windowsPartitionTypeIsProtected;

void setRequiredLabelState(QLabel* label, bool required) {
    if (!label) {
        return;
    }
    label->setStyleSheet(required
                             ? QStringLiteral("QLabel { color: #b00020; font-weight: 600; }")
                             : QString());
}

void bindRequiredLineEditLabel(QLineEdit* edit, QLabel* label) {
    if (!edit || !label) {
        return;
    }
    auto refresh = [edit, label]() {
        setRequiredLabelState(label, edit->text().trimmed().isEmpty());
    };
    QObject::connect(edit, &QLineEdit::textChanged, label, [refresh](const QString&) {
        refresh();
    });
    refresh();
}

constexpr int kRoleDevicePath = Qt::UserRole;
constexpr int kRoleSelectable = Qt::UserRole + 1;
constexpr int kRoleMounted = Qt::UserRole + 2;
constexpr int kRoleMountpoint = Qt::UserRole + 3;
constexpr int kRoleDevType = Qt::UserRole + 4;
constexpr int kRoleNodeKind = Qt::UserRole + 5;
constexpr int kRoleVdevPrefix = Qt::UserRole + 6;
constexpr int kRoleIntrinsicSelectable = Qt::UserRole + 7;

QStringList draggedDevicePaths(const QMimeData* mimeData) {
    if (!mimeData || !mimeData->hasFormat(QStringLiteral("application/x-zfsmgr-device-paths"))) {
        return {};
    }
    const QString payload = QString::fromUtf8(
        mimeData->data(QStringLiteral("application/x-zfsmgr-device-paths")));
    return payload.split('\n', Qt::SkipEmptyParts);
}

class DeviceDragTreeWidget : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
};

class PoolLayoutTreeWidget : public QTreeWidget {
public:
    std::function<void(const QStringList&, QTreeWidgetItem*)> handleExternalDrop;
    std::function<void()> handleInternalDrop;
    std::function<bool(const QList<QTreeWidgetItem*>&, QTreeWidgetItem*)> canAcceptInternalDrop;

    using QTreeWidget::QTreeWidget;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event && event->mimeData()
            && event->mimeData()->hasFormat(QStringLiteral("application/x-zfsmgr-device-paths"))) {
            event->acceptProposedAction();
            return;
        }
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event && event->mimeData()
            && event->mimeData()->hasFormat(QStringLiteral("application/x-zfsmgr-device-paths"))) {
            event->acceptProposedAction();
            return;
        }
        if (event && canAcceptInternalDrop) {
            if (!canAcceptInternalDrop(selectedItems(), itemAt(event->position().toPoint()))) {
                event->ignore();
                return;
            }
        }
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        if (event && event->mimeData()
            && event->mimeData()->hasFormat(QStringLiteral("application/x-zfsmgr-device-paths"))) {
            if (handleExternalDrop) {
                handleExternalDrop(draggedDevicePaths(event->mimeData()), itemAt(event->position().toPoint()));
            }
            event->acceptProposedAction();
            return;
        }
        if (event && canAcceptInternalDrop
            && !canAcceptInternalDrop(selectedItems(), itemAt(event->position().toPoint()))) {
            event->ignore();
            return;
        }
        QTreeWidget::dropEvent(event);
        if (handleInternalDrop) {
            handleInternalDrop();
        }
    }
};

struct ZPoolCreationDefaults {
    bool force{true};
    QString altroot;
    QString ashift{QStringLiteral("12")};
    QString autotrim{QStringLiteral("on")};
    QString compatibility{QStringLiteral("openzfs-2.4-linux")};
    QString fsProps{
        QStringLiteral("acltype=posixacl,"
                       "xattr=sa,"
                       "dnodesize=auto,"
                       "compression=lz4,"
                       "normalization=formD,"
                       "relatime=on,"
                       "canmount=noauto,"
                       "mountpoint=none")};
};

ZPoolCreationDefaults loadZPoolCreationDefaults(const QString& iniPath) {
    ZPoolCreationDefaults d;
    if (iniPath.trimmed().isEmpty()) {
        return d;
    }
    QSettings ini(iniPath, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("ZPoolCreationDefaults"));
    bool touched = false;
    auto ensure = [&](const QString& key, const QVariant& value) {
        if (!ini.contains(key)) {
            ini.setValue(key, value);
            touched = true;
        }
    };
    ensure(QStringLiteral("force"), d.force);
    ensure(QStringLiteral("altroot"), d.altroot);
    ensure(QStringLiteral("ashift"), d.ashift);
    ensure(QStringLiteral("autotrim"), d.autotrim);
    ensure(QStringLiteral("compatibility"), d.compatibility);
    ensure(QStringLiteral("fs_properties"), d.fsProps);

    d.force = ini.value(QStringLiteral("force"), d.force).toBool();
    d.altroot = ini.value(QStringLiteral("altroot"), d.altroot).toString().trimmed();
    d.ashift = ini.value(QStringLiteral("ashift"), d.ashift).toString().trimmed();
    d.autotrim = ini.value(QStringLiteral("autotrim"), d.autotrim).toString().trimmed();
    d.compatibility = ini.value(QStringLiteral("compatibility"), d.compatibility).toString().trimmed();
    d.fsProps = ini.value(QStringLiteral("fs_properties"), d.fsProps).toString().trimmed();
    ini.endGroup();
    if (touched) {
        ini.sync();
    }
    return d;
}

bool isRootDevicePath(const QString& rawPath) {
    const QString p = rawPath.trimmed();
    if (p.isEmpty()) {
        return true;
    }
    static const QList<QRegularExpression> rootPatterns = {
        QRegularExpression(QStringLiteral(R"(^/dev/(sd[a-z]+|vd[a-z]+|xvd[a-z]+)$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/nvme\d+n\d+$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/mmcblk\d+$)")),
        QRegularExpression(QStringLiteral(R"(^/dev/disk\d+$)")),
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

QString deviceTreeParentPath(const QString& rawPath) {
    const QString path = rawPath.trimmed();
    if (path.isEmpty()) {
        return QString();
    }

    {
        static const QRegularExpression rx(
            QStringLiteral(R"(^(\\\\\.\\PhysicalDrive\d+)(?:\\Partition\d+)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            const QString parent = m.captured(1);
            return parent.compare(path, Qt::CaseInsensitive) == 0 ? QString() : parent;
        }
    }
    {
        static const QRegularExpression rx(
            QStringLiteral(R"(^(\\\\\?\\PhysicalDrive\d+)(?:\\Partition\d+)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            const QString parent = m.captured(1);
            return parent.compare(path, Qt::CaseInsensitive) == 0 ? QString() : parent;
        }
    }
    {
        static const QRegularExpression rx(QStringLiteral(R"(^(/dev/disk\d+s\d+)s\d+$)"));
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            return m.captured(1);
        }
    }
    {
        static const QRegularExpression rx(QStringLiteral(R"(^(/dev/disk\d+)s\d+$)"));
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            return m.captured(1);
        }
    }
    {
        static const QRegularExpression rx(QStringLiteral(R"(^(/dev/nvme\d+n\d+)p\d+$)"));
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            return m.captured(1);
        }
    }
    {
        static const QRegularExpression rx(QStringLiteral(R"(^(/dev/mmcblk\d+)p\d+$)"));
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            return m.captured(1);
        }
    }
    {
        static const QRegularExpression rx(QStringLiteral(R"(^(/dev/(?:sd[a-z]+|vd[a-z]+|xvd[a-z]+))\d+$)"));
        const QRegularExpressionMatch m = rx.match(path);
        if (m.hasMatch()) {
            return m.captured(1);
        }
    }
    return QString();
}

QString mountedTextForDevice(const QString& mountpoint, bool mounted) {
    if (mounted || (!mountpoint.isEmpty() && mountpoint != QStringLiteral("-"))) {
        return QStringLiteral("yes");
    }
    return QStringLiteral("no");
}
} // namespace

void MainWindow::createPoolForSelectedConnection() {
    if (actionsLocked()) {
        return;
    }
    const int idx = selectedConnectionIndexForPoolManagement();
    if (idx < 0 || idx >= m_profiles.size()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_poolcrt_auto001"), QStringLiteral("Seleccione una conexión para gestionar pools."),
                QStringLiteral("Select a connection to manage pools."),
                QStringLiteral("请选择一个连接来管理池。")));
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    const bool isMacConn = p.osType.trimmed().toLower().contains(QStringLiteral("mac"));
    beginUiBusy();
    struct UiBusyGuard {
        MainWindow* w{nullptr};
        bool active{false};
        ~UiBusyGuard() {
            if (active && w) {
                w->endUiBusy();
            }
        }
        void release() {
            if (active && w) {
                w->endUiBusy();
                active = false;
            }
        }
    } busyGuard{this, true};

    struct DeviceEntry {
        QString path;
        QString resolvedPath;
        QString byIdAlias;
        QString size;
        QString mountpoint;
        QString fsType;
        QString devType;
        bool zfsSignature{false};
        bool synthesized{false};
        bool inPool{false};
        bool mounted{false};
    };

    auto runRemote = [this, &p](const QString& cmd, int timeoutMs, QString& outText) -> bool {
        QString err;
        int rc = -1;
        outText.clear();
        return runSsh(p, cmd, timeoutMs, outText, err, rc) && rc == 0;
    };
    QStringList compatibilityNames;
    {
        QString compOut;
        QString compCmd;
        if (isWindowsConnection(p)) {
            compCmd = QStringLiteral(
                "$ErrorActionPreference='SilentlyContinue'; "
                "$dirs=@("
                "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\share\\\\zfs\\\\compatibility.d',"
                "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\compatibility.d',"
                "'C:\\\\msys64\\\\usr\\\\share\\\\zfs\\\\compatibility.d'"
                "); "
                "foreach($d in $dirs){ if(Test-Path -LiteralPath $d){ Get-ChildItem -LiteralPath $d -File | ForEach-Object { $_.Name } } }");
        } else {
            compCmd = QStringLiteral(
                "for d in /etc/zfs/compatibility.d /usr/share/zfs/compatibility.d /usr/local/zfs/share/zfs/compatibility.d; do "
                "  [ -d \"$d\" ] || continue; "
                "  for f in \"$d\"/*; do [ -f \"$f\" ] || continue; basename \"$f\"; done; "
                "done | sort -u");
        }
        if (runRemote(compCmd, 15000, compOut)) {
            const QStringList lines = compOut.split('\n', Qt::SkipEmptyParts);
            for (QString n : lines) {
                n = n.trimmed();
                if (!n.isEmpty()) {
                    compatibilityNames << n;
                }
            }
            compatibilityNames.removeDuplicates();
            std::sort(compatibilityNames.begin(), compatibilityNames.end());
        }
    }

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
            "  $vol = Get-Volume -Partition $_ -ErrorAction SilentlyContinue; "
            "  $fs = '-'; if($vol -and $vol.FileSystem){ $fs = [string]$vol.FileSystem }; "
            "  $gpt = '-'; if($_.GptType){ $gpt = [string]$_.GptType }; "
            "  $ptypeRaw = '-'; if($_.Type){ $ptypeRaw = [string]$_.Type }; "
            "  $mbr = '-'; if($_.MbrType){ $mbr = [string]$_.MbrType }; "
            "  $ptype = ($fs + '|gpt=' + $gpt + '|type=' + $ptypeRaw + '|mbr=' + $mbr); "
            "  Write-Output ($path + \"`t\" + $size + \"`t\" + $mp + \"`t\" + $path + \"`t\" + $ptype + \"`tpart\") "
            "}; "
            "Get-Disk | "
            "Where-Object { "
            "  $parts = @(Get-Partition -DiskNumber $_.Number -ErrorAction SilentlyContinue); "
            "  if ($parts.Count -eq 0) { $true } "
            "  else { "
            "    $nonEfi = @($parts | Where-Object { "
            "      $g = if($_.GptType){ $_.GptType.ToString().Trim('{}').ToLower() } else { '' }; "
            "      $t = if($_.Type){ $_.Type.ToString().Trim().ToLower() } else { '' }; "
            "      ($g -ne 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b') -and ($g -ne 'e3c9e316-0b5c-4db8-817d-f92df00215ae') -and ($t -ne 'reserved') "
            "    }); "
            "    $nonEfi.Count -eq 0 "
            "  } "
            "} | "
            "ForEach-Object { "
            "  $path = ('\\\\.\\PhysicalDrive' + $_.Number); "
            "  $size = if($_.Size){ [string]([math]::Round($_.Size/1GB,2)) + 'G' } else { '-' }; "
            "  $ptype = 'diskstyle=' + [string]$_.PartitionStyle + '|bus=' + [string]$_.BusType + '|model=' + [string]$_.FriendlyName + '|type=EFI_OR_RESERVED_ONLY_OR_EMPTY'; "
            "  Write-Output ($path + \"`t\" + $size + \"`t-`t\" + $path + \"`t\" + $ptype + \"`tdisk\") "
            "}");
    } else {
        devCmd = QStringLiteral(
            "if [ \"$(uname -s 2>/dev/null)\" = \"Darwin\" ]; then "
            "  { "
            "    if [ -d /private/var/run/disk/by-id ]; then "
            "      for id in /private/var/run/disk/by-id/*; do "
            "        [ -e \"$id\" ] || continue; "
            "        real=\"$(perl -MCwd=realpath -e 'print realpath($ARGV[0])' \"$id\" 2>/dev/null)\"; "
            "        [ -n \"$real\" ] || continue; "
            "        base=\"$(basename \"$real\")\"; "
            "        parent=\"${base%%s*}\"; "
            "        if diskutil list | awk -v d=\"$parent\" '$1==\"/dev/\"d { if (index(tolower($0),\"synthesized\")>0) { found=1 } } END{ exit(found?0:1) }'; then "
            "          continue; "
            "        fi; "
            "        info=\"$(diskutil info \"$real\" 2>/dev/null || true)\"; "
            "        ptype=\"$(diskutil list | awk -v id=\"$base\" '$NF==id { t=$2; if($2==\"APFS\" && ($3==\"Volume\" || $3==\"Snapshot\")){ t=$2\" \"$3 } print t; exit }')\"; "
            "        [ -n \"$ptype\" ] || ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Partition Type:/ {print $2; exit}')\"; "
            "        [ -n \"$ptype\" ] || ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Type \\(Bundle\\):/ {print $2; exit}')\"; "
            "        [ -n \"$ptype\" ] || ptype='-'; "
            "        mnt=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Mount Point:/ {print $2; exit}')\"; "
            "        [ -n \"$mnt\" ] || mnt='-'; "
            "        size=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Disk Size:/ {print $2; exit}')\"; "
            "        [ -n \"$size\" ] || size='-'; "
            "        printf '%s\\t%s\\t%s\\t%s\\t%s\\tpart\\n' \"$id\" \"$size\" \"$mnt\" \"$real\" \"$ptype\"; "
            "      done; "
            "    fi; "
            "    diskutil list | awk '"
            "      /^\\/dev\\/disk[0-9]+/ { "
            "        current=$1; sub(/:$/, \"\", current); "
            "        print current; "
            "      }"
            "    ' | while read -r real; do "
            "      [ -n \"$real\" ] || continue; "
            "      info=\"$(diskutil info \"$real\" 2>/dev/null || true)\"; "
            "      dtype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Device \\/ Media Name:/ {print $2; exit}')\"; "
            "      [ -n \"$dtype\" ] || dtype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Type \\(Bundle\\):/ {print $2; exit}')\"; "
            "      [ -n \"$dtype\" ] || dtype='disk'; "
            "      mnt=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Mount Point:/ {print $2; exit}')\"; "
            "      [ -n \"$mnt\" ] || mnt='-'; "
            "      size=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Disk Size:/ {print $2; exit}')\"; "
            "      [ -n \"$size\" ] || size='-'; "
            "      printf '%s\\t%s\\t%s\\t%s\\t%s\\tdisk\\n' \"$real\" \"$size\" \"$mnt\" \"$real\" \"$dtype\"; "
            "    done; "
            "    diskutil list | awk '"
            "      /^\\/dev\\/disk[0-9]+/ { "
            "        current=$1; sub(/:$/, \"\", current); "
            "        synth=(index(tolower($0),\"synthesized\")>0); "
            "        next; "
            "      } "
            "      { "
            "        id=$NF; "
            "        if(id ~ /^disk[0-9]+s[0-9]+$/){ "
            "          if(synth){ next; } "
            "          t=$2; "
            "          if(t==\"APFS\" && ($3==\"Volume\" || $3==\"Snapshot\")){ t=t\" \"$3 } "
            "          print \"/dev/\"id\"\\t-\\t-\\t/dev/\"id\"\\t\"t\"\\tpart\"; "
            "        } "
            "      }"
            "    '; "
            "    for real in /dev/disk*; do "
            "      [ -e \"$real\" ] || continue; "
            "      base=\"$(basename \"$real\")\"; "
            "      case \"$base\" in "
            "        disk[0-9]|disk[0-9][0-9]|disk[0-9][0-9][0-9]) "
            "          info=\"$(diskutil info \"$real\" 2>/dev/null || true)\"; "
            "          dtype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Device \\/ Media Name:/ {print $2; exit}')\"; "
            "          [ -n \"$dtype\" ] || dtype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Type \\(Bundle\\):/ {print $2; exit}')\"; "
            "          [ -n \"$dtype\" ] || dtype='disk'; "
            "          mnt=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Mount Point:/ {print $2; exit}')\"; "
            "          [ -n \"$mnt\" ] || mnt='-'; "
            "          size=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Disk Size:/ {print $2; exit}')\"; "
            "          [ -n \"$size\" ] || size='-'; "
            "          printf '/dev/%s\\t%s\\t%s\\t/dev/%s\\t%s\\tdisk\\n' \"$base\" \"$size\" \"$mnt\" \"$base\" \"$dtype\"; "
            "          continue ;; "
            "        disk[0-9]s[0-9]*|disk[0-9][0-9]s[0-9]*|disk[0-9][0-9][0-9]s[0-9]*) ;; "
            "        *) continue ;; "
            "      esac; "
            "      parent=\"${base%%s*}\"; "
            "      if diskutil list | awk -v d=\"$parent\" '$1==\"/dev/\"d { if (index(tolower($0),\"synthesized\")>0) { found=1 } } END{ exit(found?0:1) }'; then "
            "        continue; "
            "      fi; "
            "      info=\"$(diskutil info \"$real\" 2>/dev/null || true)\"; "
            "      ptype=\"$(diskutil list | awk -v id=\"$base\" '$NF==id { t=$2; if($2==\"APFS\" && ($3==\"Volume\" || $3==\"Snapshot\")){ t=$2\" \"$3 } print t; exit }')\"; "
            "      [ -n \"$ptype\" ] || ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Partition Type:/ {print $2; exit}')\"; "
            "      [ -n \"$ptype\" ] || ptype=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Type \\(Bundle\\):/ {print $2; exit}')\"; "
            "      [ -n \"$ptype\" ] || ptype='-'; "
            "      mnt=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Mount Point:/ {print $2; exit}')\"; "
            "      [ -n \"$mnt\" ] || mnt='-'; "
            "      size=\"$(printf '%s\\n' \"$info\" | awk -F': ' '/Disk Size:/ {print $2; exit}')\"; "
            "      [ -n \"$size\" ] || size='-'; "
            "      printf '/dev/%s\\t%s\\t%s\\t/dev/%s\\t%s\\tpart\\n' \"$base\" \"$size\" \"$mnt\" \"$base\" \"$ptype\"; "
            "    done; "
            "  } | awk -F '\\t' '!seen[$4]++'; "
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
            e.resolvedPath = resolved.isEmpty() ? path : resolved;
            e.path = path;
            if (path.startsWith(QStringLiteral("/private/var/run/disk/by-id/"))) {
                e.byIdAlias = QFileInfo(path).fileName();
            }
            if (e.path.startsWith(QStringLiteral("/private/var/run/disk/by-id/"))
                && !e.resolvedPath.isEmpty()) {
                e.path = e.resolvedPath;
            }
            e.size = size.isEmpty() ? QStringLiteral("-") : size;
            if (isMacConn && e.size != QStringLiteral("-")) {
                const QRegularExpression gbRx(QStringLiteral("(\\d+[\\.,]?\\d*)\\s*GB"),
                                              QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch mm = gbRx.match(e.size);
                if (mm.hasMatch()) {
                    e.size = mm.captured(1) + QStringLiteral(" GB");
                } else {
                    const QRegularExpression numRx(QStringLiteral("(\\d+[\\.,]?\\d*)"));
                    const QRegularExpressionMatch m2 = numRx.match(e.size);
                    if (m2.hasMatch()) {
                        e.size = m2.captured(1) + QStringLiteral(" GB");
                    }
                }
            }
            e.mountpoint = mp.isEmpty() ? QStringLiteral("-") : mp;
            e.fsType = fsType.isEmpty() ? QStringLiteral("-") : fsType;
            if (isWindowsConnection(p)) {
                e.fsType = formatWindowsFsTypeDetail(e.fsType);
            }
            e.devType = type.isEmpty() ? QStringLiteral("part") : type;
            QString fsLower = e.fsType.trimmed().toLower();
            fsLower.remove('{');
            fsLower.remove('}');
            if (e.fsType.compare(QStringLiteral("zfs_member"), Qt::CaseInsensitive) == 0
                || fsLower.contains(QStringLiteral("zfs"))
                || fsLower.contains(QStringLiteral("6a945a3b-1dd2-11b2-99a6-080020736631"))
                || fsLower.contains(QStringLiteral("6a898cc3-1dd2-11b2-99a6-080020736631"))) {
                e.zfsSignature = true;
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

    auto collectPoolTokens = [](const QString& text) -> QSet<QString> {
        QSet<QString> usedTokens;
        static const QSet<QString> skip = {
            QStringLiteral("NAME"), QStringLiteral("MIRROR"), QStringLiteral("RAIDZ"), QStringLiteral("RAIDZ1"),
            QStringLiteral("RAIDZ2"), QStringLiteral("RAIDZ3"), QStringLiteral("SPARE"),
            QStringLiteral("LOGS"), QStringLiteral("CACHE"), QStringLiteral("SPECIAL"), QStringLiteral("DEDUP"),
            QStringLiteral("ONLINE"), QStringLiteral("OFFLINE"), QStringLiteral("UNAVAIL"),
            QStringLiteral("UNAVAILABLE"), QStringLiteral("DEGRADED"), QStringLiteral("FAULTED"),
            QStringLiteral("REMOVED"), QStringLiteral("AVAIL")
        };
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            if (trimmed.startsWith(QStringLiteral("pool:")) || trimmed.startsWith(QStringLiteral("state:"))
                || trimmed.startsWith(QStringLiteral("scan:")) || trimmed.startsWith(QStringLiteral("config:"))
                || trimmed.startsWith(QStringLiteral("errors:")) || trimmed.startsWith(QStringLiteral("status:"))
                || trimmed.startsWith(QStringLiteral("action:")) || trimmed.startsWith(QStringLiteral("see:"))
                || trimmed == QStringLiteral("NAME")) {
                continue;
            }
            const QString token = trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).value(0).trimmed();
            if (token.isEmpty()) {
                continue;
            }
            if (skip.contains(token.toUpper()) || token.endsWith(':') || token.endsWith('-')) {
                continue;
            }
            usedTokens.insert(token);
            if (token.startsWith('/')) {
                usedTokens.insert(QFileInfo(token).fileName());
            }
            const QRegularExpression pdRx(QStringLiteral("PhysicalDrive\\d+"), QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch m = pdRx.match(token);
            if (m.hasMatch()) {
                usedTokens.insert(m.captured(0));
            }
        }
        return usedTokens;
    };
    auto normalizeWinPhysPart = [](const QString& raw) -> QString {
        const QString s = raw.trimmed();
        if (s.isEmpty()) {
            return QString();
        }
        static const QRegularExpression rx(
            QStringLiteral("physicaldrive\\s*(\\d+)(?:\\D+partition\\s*(\\d+))?"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = rx.match(s);
        if (!m.hasMatch()) {
            return QString();
        }
        const QString d = m.captured(1);
        const QString partNo = m.captured(2);
        return partNo.isEmpty() ? QStringLiteral("physicaldrive%1").arg(d)
                                : QStringLiteral("physicaldrive%1/partition%2").arg(d, partNo);
    };
    auto markDevicesInPoolByTokens = [&devicesByPath, &normalizeWinPhysPart](const QSet<QString>& usedTokens) {
        if (usedTokens.isEmpty()) {
            return;
        }
        for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
            const QString pth = it.value().path.trimmed();
            const QString real = it.value().resolvedPath.trimmed();
            const QString alias = it.value().byIdAlias.trimmed();
            const QString pthBase = QFileInfo(pth).fileName();
            const QString realBase = QFileInfo(real).fileName();
            const QString pthLower = pth.toLower();
            const QString realLower = real.toLower();
            const QString pthNorm = normalizeWinPhysPart(pthLower);
            const QString realNorm = normalizeWinPhysPart(realLower);
            for (const QString& tkRaw : usedTokens) {
                const QString tk = tkRaw.trimmed();
                if (tk.isEmpty()) {
                    continue;
                }
                const QString tkLower = tk.toLower();
                const QString tkNorm = normalizeWinPhysPart(tkLower);
                if (tk == pth || tk == real || tk == alias
                    || tk == pthBase || tk == realBase
                    || pthLower.contains(tkLower) || realLower.contains(tkLower)
                    || (!tkNorm.isEmpty() && (tkNorm == pthNorm || tkNorm == realNorm))) {
                    it.value().inPool = true;
                    break;
                }
            }
        }
    };

    auto countInPool = [&devicesByPath]() -> int {
        int n = 0;
        for (auto it = devicesByPath.cbegin(); it != devicesByPath.cend(); ++it) {
            if (it.value().inPool) {
                ++n;
            }
        }
        return n;
    };
    auto markPhysicalDriveWholeDisk = [&devicesByPath](const QString& text) {
        static const QRegularExpression pdRx(QStringLiteral("physicaldrive\\s*(\\d+)"),
                                             QRegularExpression::CaseInsensitiveOption);
        QSet<QString> disks;
        auto it = pdRx.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (m.hasMatch()) {
                disks.insert(m.captured(1));
            }
        }
        if (disks.isEmpty()) {
            return;
        }
        for (auto itDev = devicesByPath.begin(); itDev != devicesByPath.end(); ++itDev) {
            const QString blob = (itDev.value().path + QStringLiteral(" ") + itDev.value().resolvedPath).toLower();
            for (const QString& d : disks) {
                if (blob.contains(QStringLiteral("physicaldrive%1").arg(d))) {
                    itDev.value().inPool = true;
                    break;
                }
            }
        }
    };

    out.clear();
    const QString inPoolCmd = withSudo(p, QStringLiteral(
        "(zpool status -LP 2>/dev/null || zpool status -P 2>/dev/null || zpool status -L 2>/dev/null || zpool status 2>/dev/null)"));
    if (runRemote(inPoolCmd, 25000, out)) {
        markDevicesInPoolByTokens(collectPoolTokens(out));
        if (isWindowsConnection(p)) {
            markPhysicalDriveWholeDisk(out);
        }
    }
    QString outListV;
    const QString listVCmd = withSudo(p, QStringLiteral("(zpool list -v -P 2>/dev/null || zpool list -v 2>/dev/null)"));
    if (runRemote(listVCmd, 25000, outListV)) {
        markDevicesInPoolByTokens(collectPoolTokens(outListV));
        if (isWindowsConnection(p)) {
            markPhysicalDriveWholeDisk(outListV);
        }
    }
    // Windows: explicit ZFS GPT signature scan (covers imported pools when names do not map cleanly).
    if (isWindowsConnection(p)) {
        QString zfsPartOut;
        const QString zfsPartCmd = QStringLiteral(
            "$ErrorActionPreference='SilentlyContinue'; "
            "Get-Partition | "
            "Where-Object { $_.GptType -and @('6a945a3b-1dd2-11b2-99a6-080020736631','6a898cc3-1dd2-11b2-99a6-080020736631') -contains $_.GptType.ToString().Trim('{}').ToLower() } | "
            "ForEach-Object { Write-Output ('\\\\.\\PhysicalDrive' + $_.DiskNumber + '\\\\Partition' + $_.PartitionNumber) }");
        if (runRemote(zfsPartCmd, 20000, zfsPartOut)) {
            QSet<QString> zfsTokens;
            const QStringList lines = zfsPartOut.split('\n', Qt::SkipEmptyParts);
            for (const QString& raw : lines) {
                const QString token = raw.trimmed();
                if (!token.isEmpty()) {
                    zfsTokens.insert(token);
                    zfsTokens.insert(token.toLower());
                }
            }
            markDevicesInPoolByTokens(zfsTokens);
        }
    }
    // Also probe importable (not imported) pools and mark their devices as in-pool.
    out.clear();
    QString outImp;
    const QString importProbeCmd = withSudo(p, QStringLiteral("(zpool import 2>/dev/null; zpool import -s 2>/dev/null)"));
    if (runRemote(importProbeCmd, 25000, outImp)) {
        markDevicesInPoolByTokens(collectPoolTokens(outImp));
        if (isWindowsConnection(p)) {
            markPhysicalDriveWholeDisk(outImp);
        }
    }

    if (isWindowsConnection(p)) {
        const int marked = countInPool();
        if (marked == 0) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("%1: no device marked IN_POOL while probing zpool output in pool-create dialog")
                       .arg(p.name));
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

    QSet<QString> macSynthesizedRootPaths;
    if (isMacConn) {
        QString synthOut;
        const QString synthCmd = QStringLiteral(
            "diskutil list | awk '"
            "  /^\\/dev\\/disk[0-9]+/ { "
            "    current=$1; sub(/:$/, \"\", current); "
            "    if (index(tolower($0),\"synthesized\")>0) print current; "
            "  }'");
        if (runRemote(synthCmd, 15000, synthOut)) {
            const QStringList lines = synthOut.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QString path = line.trimmed();
                if (!path.isEmpty()) {
                    macSynthesizedRootPaths.insert(path);
                }
            }
        }
    }

    {
        QMap<QString, DeviceEntry> synthesizedRoots;
        for (auto it = devicesByPath.cbegin(); it != devicesByPath.cend(); ++it) {
            const DeviceEntry& e = it.value();
            const QString parentPath = deviceTreeParentPath(e.path);
            const QString parentResolved = deviceTreeParentPath(e.resolvedPath);
            QString rootPath = parentPath;
            if (rootPath.isEmpty()) {
                rootPath = parentResolved;
            }
            if (rootPath.isEmpty() || devicesByPath.contains(rootPath) || synthesizedRoots.contains(rootPath)) {
                continue;
            }
            DeviceEntry root;
            root.path = rootPath;
            root.resolvedPath = rootPath;
            root.size = QStringLiteral("-");
            root.mountpoint = QStringLiteral("-");
            root.fsType = QStringLiteral("disk");
            root.devType = QStringLiteral("disk");
            root.zfsSignature = e.zfsSignature;
            root.synthesized = macSynthesizedRootPaths.contains(rootPath);
            root.inPool = e.inPool;
            root.mounted = false;
            synthesizedRoots.insert(rootPath, root);
        }
        for (auto it = synthesizedRoots.cbegin(); it != synthesizedRoots.cend(); ++it) {
            devicesByPath.insert(it.key(), it.value());
        }
    }
    for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
        const QString rootPath = it.value().resolvedPath.isEmpty() ? it.value().path : it.value().resolvedPath;
        if (it.value().devType == QStringLiteral("disk") || isRootDevicePath(rootPath)) {
            it.value().synthesized = macSynthesizedRootPaths.contains(it.value().path)
                                     || macSynthesizedRootPaths.contains(rootPath);
        }
    }
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = devicesByPath.begin(); it != devicesByPath.end(); ++it) {
                if (it.value().inPool) {
                    QString parentPath = deviceTreeParentPath(it.value().path);
                    if (parentPath.isEmpty()) {
                        parentPath = deviceTreeParentPath(it.value().resolvedPath);
                    }
                    if (parentPath.isEmpty()) {
                        continue;
                    }
                    auto parentIt = devicesByPath.find(parentPath);
                    if (parentIt != devicesByPath.end() && !parentIt->inPool) {
                        parentIt->inPool = true;
                        changed = true;
                    }
                }
            }
        }
    }

    QDialog dlg(this);
    const QFont baseUiFont = QApplication::font();
    dlg.setFont(baseUiFont);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_poolcrt_auto002"), QStringLiteral("Crear pool en %1"), QStringLiteral("Create pool on %1"), QStringLiteral("在 %1 创建池"))
            .arg(p.name));
    dlg.setModal(true);
    dlg.resize(1180, 912);
    auto* lay = new QVBoxLayout(&dlg);

    auto* splitter = new QSplitter(Qt::Horizontal, &dlg);
    auto* leftPane = new QWidget(&dlg);
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(8);
    auto* baseBox = new QGroupBox(
        trk(QStringLiteral("t_poolcrt_auto003"), QStringLiteral("Parámetros del pool"), QStringLiteral("Pool parameters"), QStringLiteral("池参数")), leftPane);
    auto* form = new QFormLayout(baseBox);
    form->setContentsMargins(6, 6, 6, 6);
    form->setVerticalSpacing(3);
    form->setHorizontalSpacing(8);
    baseBox->setStyleSheet(QStringLiteral(
        "QGroupBox QLineEdit, QGroupBox QComboBox { min-height: 18px; max-height: 18px; }"));
    QLineEdit* poolNameEd = new QLineEdit(baseBox);
    QCheckBox* forceCb = new QCheckBox(QStringLiteral("-f"), baseBox);
    QCheckBox* dryRunCb = new QCheckBox(QStringLiteral("-n"), baseBox);
    QLineEdit* mountpointEd = new QLineEdit(baseBox);
    QLineEdit* altrootEd = new QLineEdit(baseBox);
    QComboBox* ashiftCb = new QComboBox(baseBox);
    ashiftCb->addItems({QString(), QStringLiteral("9"), QStringLiteral("10"), QStringLiteral("11"),
                        QStringLiteral("12"), QStringLiteral("13"), QStringLiteral("14"),
                        QStringLiteral("15"), QStringLiteral("16")});
    QComboBox* autotrimCb = new QComboBox(baseBox);
    autotrimCb->addItems({QString(), QStringLiteral("off"), QStringLiteral("on")});
    QComboBox* autoexpandCb = new QComboBox(baseBox);
    autoexpandCb->addItems({QString(), QStringLiteral("off"), QStringLiteral("on")});
    QComboBox* compatibilityCb = new QComboBox(baseBox);
    compatibilityCb->setEditable(false);
    compatibilityCb->addItem(QString());
    compatibilityCb->addItem(QStringLiteral("off"));
    compatibilityCb->addItem(QStringLiteral("legacy"));
    for (const QString& c : compatibilityNames) {
        if (compatibilityCb->findText(c) < 0) {
            compatibilityCb->addItem(c);
        }
    }
    QLineEdit* bootfsEd = new QLineEdit(baseBox);
    QLineEdit* poolOptsEd = new QLineEdit(baseBox);
    QLineEdit* fsPropsEd = new QLineEdit(baseBox);
    QLineEdit* extraEd = new QLineEdit(baseBox);
    QComboBox* canmountCb = new QComboBox(baseBox);
    canmountCb->addItems({QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("noauto")});
    QLineEdit* dsMountpointEd = new QLineEdit(baseBox);
    QComboBox* relatimeCb = new QComboBox(baseBox);
    relatimeCb->addItems({QString(), QStringLiteral("on"), QStringLiteral("off")});
    QComboBox* compressionCb = new QComboBox(baseBox);
    compressionCb->setEditable(true);
    compressionCb->addItems({QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("lz4"),
                             QStringLiteral("zstd"), QStringLiteral("gzip"),
                             QStringLiteral("zstd-1"), QStringLiteral("zstd-3"), QStringLiteral("zstd-5"),
                             QStringLiteral("zstd-9"), QStringLiteral("zstd-19"),
                             QStringLiteral("gzip-1"), QStringLiteral("gzip-6"), QStringLiteral("gzip-9")});
    QComboBox* normalizationCb = new QComboBox(baseBox);
    normalizationCb->addItems({QString(), QStringLiteral("none"), QStringLiteral("formC"),
                               QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")});
    QComboBox* acltypeCb = new QComboBox(baseBox);
    acltypeCb->addItems({QString(), QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4"), QStringLiteral("posixacl")});
    QComboBox* xattrCb = new QComboBox(baseBox);
    xattrCb->addItems({QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa"), QStringLiteral("dir")});
    QComboBox* dnodesizeCb = new QComboBox(baseBox);
    dnodesizeCb->setEditable(true);
    dnodesizeCb->addItems({QString(), QStringLiteral("legacy"), QStringLiteral("auto"),
                           QStringLiteral("1k"), QStringLiteral("2k"), QStringLiteral("4k"),
                           QStringLiteral("8k"), QStringLiteral("16k")});
    const ZPoolCreationDefaults zdefs = loadZPoolCreationDefaults(m_store.iniPath());
    forceCb->setChecked(zdefs.force);
    altrootEd->setText(zdefs.altroot);
    ashiftCb->setCurrentText(zdefs.ashift);
    autotrimCb->setCurrentText(zdefs.autotrim);
    if (!zdefs.compatibility.isEmpty() && compatibilityCb->findText(zdefs.compatibility) < 0) {
        compatibilityCb->addItem(zdefs.compatibility);
    }
    compatibilityCb->setCurrentText(zdefs.compatibility);

    QMap<QString, QString> fsDefaults;
    for (const QString& item : zdefs.fsProps.split(',', Qt::SkipEmptyParts)) {
        const QString t = item.trimmed();
        if (t.isEmpty()) {
            continue;
        }
        const int eq = t.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        fsDefaults.insert(t.left(eq).trimmed().toLower(), t.mid(eq + 1).trimmed());
    }
    auto applyFsDefaultCombo = [&fsDefaults](QComboBox* cb, const QString& key) {
        const QString val = fsDefaults.value(key.toLower()).trimmed();
        if (val.isEmpty()) {
            return;
        }
        if (cb->findText(val) < 0) {
            cb->addItem(val);
        }
        cb->setCurrentText(val);
    };
    auto applyFsDefaultEdit = [&fsDefaults](QLineEdit* ed, const QString& key) {
        const QString val = fsDefaults.value(key.toLower()).trimmed();
        if (!val.isEmpty()) {
            ed->setText(val);
        }
    };
    applyFsDefaultCombo(canmountCb, QStringLiteral("canmount"));
    applyFsDefaultEdit(dsMountpointEd, QStringLiteral("mountpoint"));
    applyFsDefaultCombo(relatimeCb, QStringLiteral("relatime"));
    applyFsDefaultCombo(compressionCb, QStringLiteral("compression"));
    applyFsDefaultCombo(normalizationCb, QStringLiteral("normalization"));
    applyFsDefaultCombo(acltypeCb, QStringLiteral("acltype"));
    applyFsDefaultCombo(xattrCb, QStringLiteral("xattr"));
    applyFsDefaultCombo(dnodesizeCb, QStringLiteral("dnodesize"));
    {
        QStringList others;
        for (const QString& item : zdefs.fsProps.split(',', Qt::SkipEmptyParts)) {
            const QString t = item.trimmed();
            if (t.isEmpty()) {
                continue;
            }
            const int eq = t.indexOf('=');
            if (eq <= 0) {
                others << t;
                continue;
            }
            const QString key = t.left(eq).trimmed().toLower();
            if (key == QStringLiteral("canmount")
                || key == QStringLiteral("mountpoint")
                || key == QStringLiteral("relatime")
                || key == QStringLiteral("compression")
                || key == QStringLiteral("normalization")
                || key == QStringLiteral("acltype")
                || key == QStringLiteral("xattr")
                || key == QStringLiteral("dnodesize")) {
                continue;
            }
            others << t;
        }
        fsPropsEd->setText(others.join(QStringLiteral(",")));
    }
    auto* poolNameLabel = new QLabel(
        trk(QStringLiteral("t_poolcrt_auto004"), QStringLiteral("Nombre"), QStringLiteral("Name"), QStringLiteral("名称")),
        baseBox);
    setRequiredLabelState(poolNameLabel, true);
    bindRequiredLineEditLabel(poolNameEd, poolNameLabel);
    form->addRow(poolNameLabel, poolNameEd);
    auto* flagsRow = new QHBoxLayout();
    flagsRow->addWidget(forceCb);
    flagsRow->addWidget(dryRunCb);
    flagsRow->addStretch(1);
    form->addRow(trk(QStringLiteral("t_poolcrt_auto006"), QStringLiteral("Flags"), QStringLiteral("Flags"), QStringLiteral("标志")), flagsRow);
    auto* poolCoreGrid = new QGridLayout();
    poolCoreGrid->setContentsMargins(0, 0, 0, 0);
    poolCoreGrid->setHorizontalSpacing(6);
    poolCoreGrid->setVerticalSpacing(3);
    poolCoreGrid->addWidget(new QLabel(QStringLiteral("mountpoint"), baseBox), 0, 0);
    poolCoreGrid->addWidget(mountpointEd, 0, 1);
    poolCoreGrid->addWidget(new QLabel(QStringLiteral("altroot"), baseBox), 0, 2);
    poolCoreGrid->addWidget(altrootEd, 0, 3);
    poolCoreGrid->addWidget(new QLabel(QStringLiteral("ashift"), baseBox), 1, 0);
    poolCoreGrid->addWidget(ashiftCb, 1, 1);
    poolCoreGrid->addWidget(new QLabel(QStringLiteral("autotrim"), baseBox), 1, 2);
    poolCoreGrid->addWidget(autotrimCb, 1, 3);
    poolCoreGrid->addWidget(new QLabel(QStringLiteral("autoexpand"), baseBox), 2, 0);
    poolCoreGrid->addWidget(autoexpandCb, 2, 1);
    poolCoreGrid->addWidget(new QLabel(QStringLiteral("compatibility"), baseBox), 2, 2);
    poolCoreGrid->addWidget(compatibilityCb, 2, 3);
    poolCoreGrid->addWidget(new QLabel(QStringLiteral("bootfs"), baseBox), 3, 0);
    poolCoreGrid->addWidget(bootfsEd, 3, 1, 1, 3);
    form->addRow(QStringLiteral("-o"), poolCoreGrid);
    form->addRow(QStringLiteral("otros -o"), poolOptsEd);

    auto* fsGrid = new QGridLayout();
    fsGrid->setContentsMargins(0, 0, 0, 0);
    fsGrid->setHorizontalSpacing(6);
    fsGrid->setVerticalSpacing(3);
    fsGrid->addWidget(new QLabel(QStringLiteral("canmount"), baseBox), 0, 0);
    fsGrid->addWidget(canmountCb, 0, 1);
    fsGrid->addWidget(new QLabel(QStringLiteral("mountpoint"), baseBox), 0, 2);
    fsGrid->addWidget(dsMountpointEd, 0, 3);
    fsGrid->addWidget(new QLabel(QStringLiteral("relatime"), baseBox), 1, 0);
    fsGrid->addWidget(relatimeCb, 1, 1);
    fsGrid->addWidget(new QLabel(QStringLiteral("compression"), baseBox), 1, 2);
    fsGrid->addWidget(compressionCb, 1, 3);
    fsGrid->addWidget(new QLabel(QStringLiteral("normalization"), baseBox), 2, 0);
    fsGrid->addWidget(normalizationCb, 2, 1);
    fsGrid->addWidget(new QLabel(QStringLiteral("acltype"), baseBox), 2, 2);
    fsGrid->addWidget(acltypeCb, 2, 3);
    fsGrid->addWidget(new QLabel(QStringLiteral("xattr"), baseBox), 3, 0);
    fsGrid->addWidget(xattrCb, 3, 1);
    fsGrid->addWidget(new QLabel(QStringLiteral("dnodesize"), baseBox), 3, 2);
    fsGrid->addWidget(dnodesizeCb, 3, 3);
    form->addRow(QStringLiteral("-O"), fsGrid);
    fsPropsEd->setPlaceholderText(QStringLiteral("otros -O: k=v,k=v"));
    form->addRow(QStringLiteral("otros -O"), fsPropsEd);
    form->addRow(QStringLiteral("extra args"), extraEd);
    if (baseBox->layout()) {
        baseBox->layout()->activate();
    }
    const QSize fixedBaseSize = baseBox->sizeHint();
    baseBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    baseBox->setFixedSize(fixedBaseSize);
    leftPane->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    leftPane->setMinimumWidth(fixedBaseSize.width());
    leftLay->addWidget(baseBox, 0);

    auto* vdevBox =
        new QGroupBox(trk(QStringLiteral("t_poolcrt_auto007"), QStringLiteral("Constructor de VDEV"),
                          QStringLiteral("VDEV builder"),
                          QStringLiteral("VDEV 构建器")),
                      leftPane);
    auto* vdevLay = new QVBoxLayout(vdevBox);
    auto* vdevButtonsRow = new QHBoxLayout();
    auto* addSelectedBtn = new QPushButton(
        trk(QStringLiteral("t_poolcrt_auto011b"),
            QStringLiteral("Añadir seleccionados"),
            QStringLiteral("Add selected"),
            QStringLiteral("添加所选设备")),
        vdevBox);
    auto* clearSelBtn = new QPushButton(trk(QStringLiteral("t_poolcrt_auto011"), QStringLiteral("Limpiar selección dispositivos"),
                                            QStringLiteral("Clear device selection"),
                                            QStringLiteral("清除设备选择")),
                                        vdevBox);
    addSelectedBtn->setFont(baseUiFont);
    clearSelBtn->setFont(baseUiFont);
    vdevButtonsRow->addWidget(addSelectedBtn, 0);
    vdevButtonsRow->addWidget(clearSelBtn, 0);
    vdevButtonsRow->addStretch(1);
    vdevLay->addLayout(vdevButtonsRow, 0);
    auto* vdevHelp =
        new QLabel(trk(QStringLiteral("t_poolcrt_auto013"),
                       QStringLiteral("Cree nodos con el menú contextual del árbol, marque block devices y pulse Añadir seleccionados."),
                       QStringLiteral("Create nodes with the tree context menu, check block devices, and click Add selected."),
                       QStringLiteral("通过树的上下文菜单创建节点，勾选块设备后点击添加所选设备。")),
                   vdevBox);
    vdevHelp->setWordWrap(true);
    vdevLay->addWidget(vdevHelp, 0);
    PoolLayoutTreeWidget* poolTree = new PoolLayoutTreeWidget(vdevBox);
    poolTree->setFont(baseUiFont);
    poolTree->setHeaderHidden(true);
    poolTree->setContextMenuPolicy(Qt::CustomContextMenu);
    poolTree->setDragEnabled(true);
    poolTree->setAcceptDrops(true);
    poolTree->setDropIndicatorShown(true);
    poolTree->setDragDropMode(QAbstractItemView::InternalMove);
    poolTree->setDefaultDropAction(Qt::MoveAction);
    if (poolTree->viewport()) {
        poolTree->viewport()->setAcceptDrops(true);
    }
    auto* poolRootItem = new QTreeWidgetItem(QStringList{QStringLiteral("Pool")});
    poolRootItem->setData(0, kRoleNodeKind, QStringLiteral("root"));
    poolRootItem->setFlags((poolRootItem->flags() | Qt::ItemIsDropEnabled) & ~Qt::ItemIsDragEnabled);
    poolTree->addTopLevelItem(poolRootItem);
    poolRootItem->setExpanded(true);
    vdevLay->addWidget(poolTree, 1);
    vdevBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    vdevBox->setMinimumWidth(fixedBaseSize.width());
    leftLay->addWidget(vdevBox, 1);

    auto* devicesBox = new QGroupBox(
        trk(QStringLiteral("t_poolcrt_auto015"), QStringLiteral("Block devices disponibles"),
            QStringLiteral("Available block devices"),
            QStringLiteral("可用块设备")),
        &dlg);
    auto* devicesLayout = new QVBoxLayout(devicesBox);
    DeviceDragTreeWidget* devicesTree = new DeviceDragTreeWidget(devicesBox);
    devicesTree->setFont(baseUiFont);
    devicesTree->setColumnCount(5);
    devicesTree->setHeaderLabels({
        trk(QStringLiteral("t_poolcrt_auto017"), QStringLiteral("Device"), QStringLiteral("Device"), QStringLiteral("设备")),
        trk(QStringLiteral("t_poolcrt_auto018"), QStringLiteral("Tamaño"), QStringLiteral("Size"), QStringLiteral("大小")),
        trk(QStringLiteral("t_poolcrt_auto034"), QStringLiteral("Tipo partición"), QStringLiteral("Partition type"), QStringLiteral("分区类型")),
        trk(QStringLiteral("t_poolcrt_auto035"), QStringLiteral("Montada"), QStringLiteral("Mounted"), QStringLiteral("已挂载")),
        trk(QStringLiteral("t_poolcrt_auto036"), QStringLiteral("En pool"), QStringLiteral("In pool"), QStringLiteral("在池中")),
    });
    for (int c = 0; c < devicesTree->columnCount(); ++c) {
        devicesTree->header()->setSectionResizeMode(c, QHeaderView::Interactive);
    }
    devicesTree->header()->setStretchLastSection(false);
    devicesTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    devicesTree->setSelectionMode(QAbstractItemView::SingleSelection);
    devicesTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    devicesTree->setRootIsDecorated(true);
    devicesTree->setUniformRowHeights(false);
    devicesTree->setDragEnabled(false);
    devicesTree->setAcceptDrops(false);
    devicesTree->setDropIndicatorShown(false);
    devicesTree->setDragDropMode(QAbstractItemView::NoDragDrop);
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
    QSet<QString> yellowDevicePaths;
    QMap<QString, DeviceRenderRow> renderRowsByPath;
    QMap<QString, QStringList> childPathsByRoot;
    QStringList paths = devicesByPath.keys();
    for (const QString& path : paths) {
        const DeviceEntry& e = devicesByPath[path];
        QString parentPath = deviceTreeParentPath(e.path);
        if (parentPath.isEmpty()) {
            parentPath = deviceTreeParentPath(e.resolvedPath);
        }
        if (!parentPath.isEmpty()) {
            childPathsByRoot[parentPath].append(e.path);
        }
    }
    auto isApfsLikeFsType = [](const QString& fsType) -> bool {
        const QString fs = fsType.trimmed().toLower();
        return fs.contains(QStringLiteral("apfs"));
    };
    QSet<QString> importedPoolNames;
    if (idx >= 0 && idx < m_states.size()) {
        for (const PoolImported& pool : m_states[idx].importedPools) {
            const QString name = pool.pool.trimmed().toLower();
            if (!name.isEmpty()) {
                importedPoolNames.insert(name);
            }
        }
    }
    auto hasProtectedSystemMount = [](const DeviceEntry& e) -> bool {
        const QString fs = e.fsType.trimmed().toLower();
        if (windowsPartitionTypeIsProtected(e.fsType)) {
            return true;
        }
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
        // macOS: classify any APFS* partition as SYSTEM (container volumes, snapshots, recovery, etc.).
        if (fs.contains(QStringLiteral("apfs"))) {
            return true;
        }
        QString mp = e.mountpoint;
        mp.replace(QStringLiteral("\\n"), QStringLiteral(" "));
        mp.replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QStringList toks = mp.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
        for (const QString& tRaw : toks) {
            const QString t = tRaw.trimmed();
            const QString tw = t.toUpper();
            // Windows: anything mounted on C: is system/protected and must stay red.
            if (tw == QStringLiteral("C:")
                || tw == QStringLiteral("C:\\")
                || tw.startsWith(QStringLiteral("C:\\"))) {
                return true;
            }
            if (t == QStringLiteral("/") || t == QStringLiteral("/boot") || t == QStringLiteral("/boot/efi")
                || t == QStringLiteral("[SWAP]") || t.startsWith(QStringLiteral("/System"))) {
                return true;
            }
        }
        return false;
    };
    for (const QString& path : paths) {
        const DeviceEntry e = devicesByPath.value(path);
        const QString realPath = e.resolvedPath.isEmpty() ? e.path : e.resolvedPath;
        DeviceRenderRow rr;
        rr.entry = e;
        const bool isDiskRoot = (e.devType == QStringLiteral("disk")
                                 || isRootDevicePath(e.path)
                                 || isRootDevicePath(realPath));
        bool macRootHasApfsChildren = false;
        if (isMacConn && isDiskRoot) {
            const QStringList childPaths = childPathsByRoot.value(e.path) + childPathsByRoot.value(realPath);
            for (const QString& childPath : childPaths) {
                const DeviceEntry child = devicesByPath.value(childPath);
                if (isApfsLikeFsType(child.fsType)) {
                    macRootHasApfsChildren = true;
                    break;
                }
            }
        }
        const bool isWinEfiOrReservedOnlyOrEmptyDisk =
            isDiskRoot && e.fsType.contains(QStringLiteral("type=EFI_OR_RESERVED_ONLY_OR_EMPTY"), Qt::CaseInsensitive);
        const bool protectedMount = hasProtectedSystemMount(e);
        const bool isMacDisk0Root =
            isMacConn && isDiskRoot
            && (e.path == QStringLiteral("/dev/disk0") || realPath == QStringLiteral("/dev/disk0"));
        const bool macImportedPoolVirtualDisk =
            isMacConn && isDiskRoot
            && importedPoolNames.contains(e.fsType.trimmed().toLower());
        const bool macApfsRootBlocked =
            isMacConn && isDiskRoot
            && (isMacDisk0Root || e.synthesized || macRootHasApfsChildren || isApfsLikeFsType(e.fsType));
        if (isWinEfiOrReservedOnlyOrEmptyDisk) {
            rr.stateText = trk(QStringLiteral("t_poolcrt_auto028"), QStringLiteral("LIBRE"), QStringLiteral("FREE"), QStringLiteral("空闲"));
            rr.detailText = trk(QStringLiteral("t_poolcrt_auto029"), QStringLiteral("Disponible"), QStringLiteral("Available"), QStringLiteral("可用"));
            rr.bgColor = stGreen;
            rr.colorRank = 0;
            rr.selectable = true;
        } else if (protectedMount || e.inPool || macApfsRootBlocked || macImportedPoolVirtualDisk) {
            rr.stateText = trk(QStringLiteral("t_poolcrt_auto022"), QStringLiteral("EN_POOL"), QStringLiteral("IN_POOL"), QStringLiteral("在池中"));
            if (protectedMount) {
                rr.stateText = trk(QStringLiteral("t_poolcrt_auto023"), QStringLiteral("SISTEMA"), QStringLiteral("SYSTEM"), QStringLiteral("系统"));
                rr.detailText = trk(QStringLiteral("t_poolcrt_auto024"), QStringLiteral("Dispositivo de sistema (/ /boot /boot/efi o SWAP)"),
                                    QStringLiteral("System device (/ /boot /boot/efi or SWAP)"),
                                    QStringLiteral("系统设备（/ /boot /boot/efi 或 SWAP）"));
            } else if (macImportedPoolVirtualDisk) {
                rr.stateText = QStringLiteral("POOL");
                rr.detailText = QStringLiteral("Disco virtual sintetizado por ZFS para un pool ya importado");
            } else if (isMacDisk0Root) {
                rr.stateText = QStringLiteral("APFS");
                rr.detailText = QStringLiteral("Disco físico interno de macOS no seleccionable");
            } else if (macApfsRootBlocked) {
                rr.stateText = QStringLiteral("APFS");
                rr.detailText = e.synthesized
                                    ? QStringLiteral("Disco APFS sintetizado por macOS")
                                    : QStringLiteral("Disco contenedor APFS no seleccionable");
            } else {
                rr.detailText = trk(QStringLiteral("t_poolcrt_auto026"), QStringLiteral("Ya pertenece a un pool"),
                                    QStringLiteral("Already part of a pool"),
                                    QStringLiteral("已属于某个池"));
            }
            rr.bgColor = stRed;
            rr.colorRank = 2;
            rr.selectable = false;
        } else if (e.mounted || (!e.mountpoint.isEmpty() && e.mountpoint != QStringLiteral("-"))) {
            rr.stateText = trk(QStringLiteral("t_poolcrt_auto027"), QStringLiteral("MONTADO"), QStringLiteral("MOUNTED"), QStringLiteral("已挂载"));
            rr.detailText = e.mountpoint;
            rr.bgColor = stYellow;
            rr.colorRank = 1;
            yellowDevicePaths.insert(e.path);
        } else {
            rr.stateText = trk(QStringLiteral("t_poolcrt_auto028"), QStringLiteral("LIBRE"), QStringLiteral("FREE"), QStringLiteral("空闲"));
            if (e.zfsSignature) {
                rr.detailText = QStringLiteral("Firma ZFS detectada, pero no aparece en zpool status/import");
            } else {
                rr.detailText = trk(QStringLiteral("t_poolcrt_auto029"), QStringLiteral("Disponible"), QStringLiteral("Available"), QStringLiteral("可用"));
            }
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
        renderRowsByPath.insert(e.path, rr);
    }
    std::sort(renderRows.begin(), renderRows.end(), [](const DeviceRenderRow& a, const DeviceRenderRow& b) {
        if (a.colorRank != b.colorRank) {
            return a.colorRank < b.colorRank; // green, yellow, red
        }
        return a.entry.path < b.entry.path;
    });

    auto createDeviceTreeItem = [&](const DeviceRenderRow& rr) -> QTreeWidgetItem* {
        const DeviceEntry& e = rr.entry;
        auto* item = new QTreeWidgetItem();
        item->setText(0, e.path);
        item->setText(1, e.size);
        item->setText(2, e.fsType);
        item->setText(4, e.inPool ? QStringLiteral("Sí") : QStringLiteral("No"));
        item->setData(0, kRoleDevicePath, e.path);
        item->setData(0, kRoleSelectable, rr.selectable);
        item->setData(0, kRoleIntrinsicSelectable, rr.selectable);
        item->setData(0, kRoleMounted, e.mounted || (!e.mountpoint.isEmpty() && e.mountpoint != QStringLiteral("-")));
        item->setData(0, kRoleMountpoint, e.mountpoint);
        item->setData(0, kRoleDevType, e.devType);
        Qt::ItemFlags flags = item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        if (rr.selectable) {
            flags |= Qt::ItemIsUserCheckable;
        } else {
            flags &= ~Qt::ItemIsUserCheckable;
        }
        flags &= ~Qt::ItemIsEditable;
        item->setFlags(flags);
        item->setCheckState(0, Qt::Unchecked);
        item->setToolTip(0, rr.detailText);
        item->setToolTip(2, rr.detailText);
        for (int c = 0; c < devicesTree->columnCount(); ++c) {
            item->setBackground(c, rr.bgColor);
        }
        return item;
    };

    auto updateMountedWidget = [&](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }
        auto* combo = qobject_cast<QComboBox*>(devicesTree->itemWidget(item, 3));
        if (!combo) {
            combo = new QComboBox(devicesTree->viewport());
            combo->addItem(QStringLiteral("No"));
            combo->addItem(QStringLiteral("Sí"));
            combo->setFocusPolicy(Qt::NoFocus);
            devicesTree->setItemWidget(item, 3, combo);
        }
        combo->setProperty("zfsmgr_device_path", item->data(0, kRoleDevicePath));
        const bool mounted = item->data(0, kRoleMounted).toBool();
        QSignalBlocker blocker(combo);
        combo->setCurrentText(mounted ? QStringLiteral("Sí") : QStringLiteral("No"));
        const QString devType = item->data(0, kRoleDevType).toString();
        combo->setEnabled(devType != QStringLiteral("disk") || isMacConn);
    };

    auto applyRowColor = [&](QTreeWidgetItem* item, const QColor& color) {
        if (!item) {
            return;
        }
        for (int c = 0; c < devicesTree->columnCount(); ++c) {
            item->setBackground(c, color);
        }
    };

    QMap<QString, QTreeWidgetItem*> rootItemsByPath;
    QVector<DeviceRenderRow> rootRows;
    QSet<QString> addedRootPaths;
    for (const DeviceRenderRow& rr : renderRows) {
        const QString parentPath = deviceTreeParentPath(rr.entry.path);
        const QString resolvedParentPath = deviceTreeParentPath(rr.entry.resolvedPath);
        if (!parentPath.isEmpty() && renderRowsByPath.contains(parentPath)) {
            continue;
        }
        if (!resolvedParentPath.isEmpty() && renderRowsByPath.contains(resolvedParentPath)) {
            continue;
        }
        const QString rootKey = rr.entry.path.trimmed();
        if (rootKey.isEmpty() || addedRootPaths.contains(rootKey)) {
            continue;
        }
        addedRootPaths.insert(rootKey);
        rootRows.push_back(rr);
    }
    std::sort(rootRows.begin(), rootRows.end(), [](const DeviceRenderRow& a, const DeviceRenderRow& b) {
        return a.entry.path.localeAwareCompare(b.entry.path) < 0;
    });
    for (const DeviceRenderRow& rr : rootRows) {
        auto* item = createDeviceTreeItem(rr);
        devicesTree->addTopLevelItem(item);
        updateMountedWidget(item);
        rootItemsByPath.insert(rr.entry.path, item);
        if (!rr.entry.resolvedPath.isEmpty()) {
            rootItemsByPath.insert(rr.entry.resolvedPath, item);
        }
    }
    for (const DeviceRenderRow& rr : renderRows) {
        const QString parentPath = deviceTreeParentPath(rr.entry.path);
        const QString resolvedParentPath = deviceTreeParentPath(rr.entry.resolvedPath);
        QTreeWidgetItem* parentItem = nullptr;
        if (!parentPath.isEmpty()) {
            parentItem = rootItemsByPath.value(parentPath, nullptr);
        }
        if (!parentItem && !resolvedParentPath.isEmpty()) {
            parentItem = rootItemsByPath.value(resolvedParentPath, nullptr);
        }
        if (!parentItem) {
            continue;
        }
        const QString itemPath = rr.entry.path;
        const QString parentRootPath = parentItem->data(0, kRoleDevicePath).toString();
        const QString parentResolved = devicesByPath.value(parentRootPath).resolvedPath;
        if (itemPath == parentRootPath || (!parentResolved.isEmpty() && itemPath == parentResolved)) {
            continue;
        }
        auto* childItem = createDeviceTreeItem(rr);
        parentItem->addChild(childItem);
        updateMountedWidget(childItem);
    }
    devicesTree->expandAll();
    devicesTree->setColumnWidth(0, 360);
    devicesTree->resizeColumnToContents(1);
    devicesTree->setColumnWidth(2, 180);
    devicesTree->setColumnWidth(3, 90);
    devicesTree->setColumnWidth(4, 90);
    devicesLayout->addWidget(devicesTree, 1);
    splitter->addWidget(leftPane);
    splitter->addWidget(devicesBox);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    lay->addWidget(splitter, 1);
    auto* poolCmdPreview = new QLabel(&dlg);
    poolCmdPreview->setFont(baseUiFont);
    poolCmdPreview->setWordWrap(true);
    poolCmdPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(poolCmdPreview, 0);

    auto handleMountedChoice = [&](QTreeWidgetItem* item, QComboBox* combo) {
        if (!item || !combo) {
            return;
        }
        const bool mounted = item->data(0, kRoleMounted).toBool();
        const QString chosen = combo->currentText().trimmed();
        if (!mounted && chosen == QStringLiteral("Sí")) {
            QSignalBlocker blocker(combo);
            combo->setCurrentText(QStringLiteral("No"));
            QMessageBox::information(&dlg, QStringLiteral("ZFSMgr"),
                                     QStringLiteral("Desde aquí solo se permite desmontar."));
            return;
        }
        if (!mounted || chosen != QStringLiteral("No")) {
            return;
        }

        const QString path = item->data(0, kRoleDevicePath).toString().trimmed();
        const QString devType = item->data(0, kRoleDevType).toString().trimmed();
        QString unmountCmd;
        if (isMacConn) {
            unmountCmd = (devType == QStringLiteral("disk"))
                             ? QStringLiteral("diskutil unmountDisk %1").arg(shSingleQuote(path))
                             : QStringLiteral("diskutil unmount %1").arg(shSingleQuote(path));
        } else if (!isWindowsConnection(p)) {
            unmountCmd = QStringLiteral("umount %1").arg(shSingleQuote(path));
        } else {
            QSignalBlocker blocker(combo);
            combo->setCurrentText(QStringLiteral("Sí"));
            return;
        }

        QString cmdOut;
        QString cmdErr;
        int rc = -1;
        const QString fullCmd = withSudo(p, unmountCmd);
        if (!runSsh(p, fullCmd, 30000, cmdOut, cmdErr, rc) || rc != 0) {
            QSignalBlocker blocker(combo);
            combo->setCurrentText(QStringLiteral("Sí"));
            QMessageBox::warning(
                &dlg, QStringLiteral("ZFSMgr"),
                QStringLiteral("No se pudo desmontar %1:\n%2")
                    .arg(path, cmdErr.isEmpty() ? QStringLiteral("exit %1").arg(rc) : cmdErr));
            return;
        }

        item->setData(0, kRoleMounted, false);
        item->setData(0, kRoleMountpoint, QStringLiteral("-"));
        yellowDevicePaths.remove(path);
        if (auto it = devicesByPath.find(path); it != devicesByPath.end()) {
            it->mounted = false;
            it->mountpoint = QStringLiteral("-");
        }
        const bool selectable = item->data(0, kRoleSelectable).toBool();
        const bool inPool = item->text(4) == QStringLiteral("Sí");
        applyRowColor(item, (selectable && !inPool) ? stGreen : stRed);
        item->setToolTip(0, QStringLiteral("Disponible | %1").arg(path));
        item->setToolTip(2, item->toolTip(2));
        QSignalBlocker blocker(combo);
        combo->setCurrentText(QStringLiteral("No"));
    };

    auto findDeviceItemByPath = [&](const QString& wantedPath) -> QTreeWidgetItem* {
        if (wantedPath.trimmed().isEmpty()) {
            return nullptr;
        }
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
            if (!node) {
                return nullptr;
            }
            if (node->data(0, kRoleDevicePath).toString().trimmed() == wantedPath) {
                return node;
            }
            for (int i = 0; i < node->childCount(); ++i) {
                if (QTreeWidgetItem* found = visit(node->child(i))) {
                    return found;
                }
            }
            return nullptr;
        };
        for (int i = 0; i < devicesTree->topLevelItemCount(); ++i) {
            if (QTreeWidgetItem* found = visit(devicesTree->topLevelItem(i))) {
                return found;
            }
        }
        return nullptr;
    };

    std::function<void(QTreeWidgetItem*)> bindMountedWidgets = [&](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }
        if (auto* combo = qobject_cast<QComboBox*>(devicesTree->itemWidget(item, 3))) {
            QObject::connect(combo, &QComboBox::currentTextChanged, &dlg, [&, combo](const QString&) {
                if (!devicesTree) {
                    return;
                }
                const QString devicePath = combo->property("zfsmgr_device_path").toString().trimmed();
                if (devicePath.isEmpty()) {
                    return;
                }
                QTreeWidgetItem* currentItem = findDeviceItemByPath(devicePath);
                if (!currentItem) {
                    return;
                }
                handleMountedChoice(currentItem, combo);
            });
        }
        for (int i = 0; i < item->childCount(); ++i) {
            bindMountedWidgets(item->child(i));
        }
    };
    for (int i = 0; i < devicesTree->topLevelItemCount(); ++i) {
        bindMountedWidgets(devicesTree->topLevelItem(i));
    }

    bool syncDeviceChecks = false;
    connect(devicesTree, &QTreeWidget::itemChanged, &dlg, [&](QTreeWidgetItem* item, int column) {
        if (!item || column != 0 || syncDeviceChecks) {
            return;
        }
        if (!item->data(0, kRoleSelectable).toBool()) {
            return;
        }
        syncDeviceChecks = true;
        const Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem* child = item->child(i);
            if (child && child->data(0, kRoleSelectable).toBool()) {
                child->setCheckState(0, state);
            }
        }
        if (QTreeWidgetItem* parent = item->parent()) {
            int checkedChildren = 0;
            int partialChildren = 0;
            int selectableChildren = 0;
            for (int i = 0; i < parent->childCount(); ++i) {
                QTreeWidgetItem* child = parent->child(i);
                if (!child || !child->data(0, kRoleSelectable).toBool()) {
                    continue;
                }
                ++selectableChildren;
                if (child->checkState(0) == Qt::Checked) {
                    ++checkedChildren;
                } else if (child->checkState(0) == Qt::PartiallyChecked) {
                    ++partialChildren;
                }
            }
            if (selectableChildren > 0) {
                if (checkedChildren == selectableChildren) {
                    parent->setCheckState(0, Qt::Checked);
                } else if (checkedChildren == 0 && partialChildren == 0) {
                    parent->setCheckState(0, Qt::Unchecked);
                } else {
                    parent->setCheckState(0, Qt::PartiallyChecked);
                }
            }
        }
        syncDeviceChecks = false;
    });
    auto checkedDevices = [devicesTree]() -> QStringList {
        QStringList selected;
        std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
            if (!item) {
                return;
            }
            const bool selectable = item->data(0, kRoleSelectable).toBool();
            const Qt::CheckState state = item->checkState(0);
            if (selectable && state == Qt::Checked) {
                const QString path = item->data(0, kRoleDevicePath).toString().trimmed();
                if (!path.isEmpty()) {
                    selected << path;
                }
            }
            for (int i = 0; i < item->childCount(); ++i) {
                visit(item->child(i));
            }
        };
        for (int i = 0; i < devicesTree->topLevelItemCount(); ++i) {
            visit(devicesTree->topLevelItem(i));
        }
        selected.removeDuplicates();
        return selected;
    };

    auto clearDeviceChecks = [&]() {
        QSignalBlocker blocker(devicesTree);
        std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
            if (!item) {
                return;
            }
            if (item->data(0, kRoleSelectable).toBool()) {
                item->setCheckState(0, Qt::Unchecked);
            }
            for (int i = 0; i < item->childCount(); ++i) {
                visit(item->child(i));
            }
        };
        for (int i = 0; i < devicesTree->topLevelItemCount(); ++i) {
            visit(devicesTree->topLevelItem(i));
        }
    };
    connect(clearSelBtn, &QPushButton::clicked, &dlg, clearDeviceChecks);

    auto poolNodeKind = [](QTreeWidgetItem* item) -> QString {
        return item ? item->data(0, kRoleNodeKind).toString() : QString();
    };
    auto poolNodePrefix = [](QTreeWidgetItem* item) -> QString {
        return item ? item->data(0, kRoleVdevPrefix).toString().trimmed().toLower() : QString();
    };
    auto isRedundantVdevPrefix = [&](const QString& prefix) -> bool {
        return prefix == QStringLiteral("mirror")
               || prefix == QStringLiteral("raidz")
               || prefix == QStringLiteral("raidz1")
               || prefix == QStringLiteral("raidz2")
               || prefix == QStringLiteral("raidz3");
    };
    auto isTopLevelClassPrefix = [&](const QString& prefix) -> bool {
        return prefix == QStringLiteral("log")
               || prefix == QStringLiteral("cache")
               || prefix == QStringLiteral("spare")
               || prefix == QStringLiteral("special")
               || prefix == QStringLiteral("dedup");
    };
    auto isNormalTopLevelVdevPrefix = [&](const QString& prefix) -> bool {
        return prefix == QStringLiteral("mirror")
               || prefix == QStringLiteral("raidz")
               || prefix == QStringLiteral("raidz1")
               || prefix == QStringLiteral("raidz2")
               || prefix == QStringLiteral("raidz3");
    };
    auto classAllowsNestedVdev = [&](const QString& classPrefix, const QString& childPrefix) -> bool {
        if (classPrefix == QStringLiteral("log")) {
            return childPrefix == QStringLiteral("mirror");
        }
        if (classPrefix == QStringLiteral("special") || classPrefix == QStringLiteral("dedup")) {
            return isRedundantVdevPrefix(childPrefix);
        }
        return false;
    };
    auto canParentPoolNode = [&](QTreeWidgetItem* parentNode, const QString& childKind, const QString& childPrefix) -> bool {
        const QString parentKind = poolNodeKind(parentNode);
        const QString parentPrefix = poolNodePrefix(parentNode);
        if (parentKind == QStringLiteral("root")) {
            if (childKind == QStringLiteral("device")) {
                return true;
            }
            if (childKind == QStringLiteral("class")) {
                return isTopLevelClassPrefix(childPrefix);
            }
            if (childKind == QStringLiteral("vdev")) {
                return isNormalTopLevelVdevPrefix(childPrefix);
            }
            return false;
        }
        if (parentKind == QStringLiteral("vdev")) {
            return childKind == QStringLiteral("device");
        }
        if (parentKind == QStringLiteral("class")) {
            if (childKind == QStringLiteral("device")) {
                return true;
            }
            if (childKind == QStringLiteral("vdev")) {
                return classAllowsNestedVdev(parentPrefix, childPrefix);
            }
            return false;
        }
        return false;
    };

    std::function<void(QTreeWidgetItem*, QStringList&)> collectPoolTreeTokens =
        [&](QTreeWidgetItem* node, QStringList& tokens) {
            if (!node) {
                return;
            }
            const QString kind = node->data(0, kRoleNodeKind).toString();
            if (kind == QStringLiteral("device")) {
                const QString path = node->data(0, kRoleDevicePath).toString().trimmed();
                if (!path.isEmpty()) {
                    tokens << path;
                }
                return;
            }
            if (kind == QStringLiteral("class") || kind == QStringLiteral("vdev")) {
                const QString prefix = node->data(0, kRoleVdevPrefix).toString().trimmed();
                if (!prefix.isEmpty()) {
                    tokens << prefix.split(' ', Qt::SkipEmptyParts);
                }
            }
            for (int i = 0; i < node->childCount(); ++i) {
                collectPoolTreeTokens(node->child(i), tokens);
            }
        };

    auto poolTreeSpecLines = [&]() -> QStringList {
        QStringList lines;
        for (int i = 0; i < poolRootItem->childCount(); ++i) {
            QTreeWidgetItem* group = poolRootItem->child(i);
            const QString kind = poolNodeKind(group);
            if (!group || (kind != QStringLiteral("vdev") && kind != QStringLiteral("class")
                           && kind != QStringLiteral("device"))) {
                continue;
            }
            QStringList tokens;
            if (kind == QStringLiteral("device")) {
                const QString path = group->data(0, kRoleDevicePath).toString().trimmed();
                if (!path.isEmpty()) {
                    tokens << path;
                }
            } else {
                collectPoolTreeTokens(group, tokens);
            }
            if (!tokens.isEmpty()) {
                lines << tokens.join(' ');
            }
        }
        return lines;
    };

    auto poolTreeContainsDevice = [&](const QString& path) -> bool {
        if (path.trimmed().isEmpty()) {
            return false;
        }
        std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) -> bool {
            if (!item) {
                return false;
            }
            if (item->data(0, kRoleNodeKind).toString() == QStringLiteral("device")
                && item->data(0, kRoleDevicePath).toString() == path) {
                return true;
            }
            for (int i = 0; i < item->childCount(); ++i) {
                if (visit(item->child(i))) {
                    return true;
                }
            }
            return false;
        };
        return visit(poolRootItem);
    };

    auto createPoolGroupNode = [&](QTreeWidgetItem* parentNode, const QString& label, const QString& prefix) -> QTreeWidgetItem* {
        if (!parentNode || poolNodeKind(parentNode) == QStringLiteral("device")) {
            parentNode = parentNode && parentNode->parent() ? parentNode->parent() : poolRootItem;
        }
        const QString childKind = isTopLevelClassPrefix(prefix) ? QStringLiteral("class") : QStringLiteral("vdev");
        if (!canParentPoolNode(parentNode, childKind, prefix)) {
            return nullptr;
        }
        auto* item = new QTreeWidgetItem(QStringList{label});
        item->setData(0, kRoleNodeKind, childKind);
        item->setData(0, kRoleVdevPrefix, prefix);
        item->setFlags(item->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled);
        parentNode->addChild(item);
        item->setExpanded(true);
        parentNode->setExpanded(true);
        return item;
    };

    auto usedPoolDevicePaths = [&]() -> QSet<QString> {
        QSet<QString> used;
        std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
            if (!item) {
                return;
            }
            if (poolNodeKind(item) == QStringLiteral("device")) {
                const QString path = item->data(0, kRoleDevicePath).toString().trimmed();
                if (!path.isEmpty()) {
                    used.insert(path);
                }
            }
            for (int i = 0; i < item->childCount(); ++i) {
                visit(item->child(i));
            }
        };
        visit(poolRootItem);
        return used;
    };

    auto syncDeviceAvailability = [&]() {
        const QSet<QString> used = usedPoolDevicePaths();
        std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
            if (!item) {
                return;
            }
            const bool intrinsicSelectable = item->data(0, kRoleIntrinsicSelectable).toBool();
            const QString path = item->data(0, kRoleDevicePath).toString().trimmed();
            const bool isUsed = !path.isEmpty() && used.contains(path);
            const bool effectiveSelectable = intrinsicSelectable && !isUsed;
            item->setData(0, kRoleSelectable, effectiveSelectable);
            Qt::ItemFlags flags = item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled;
            if (effectiveSelectable) {
                flags |= Qt::ItemIsUserCheckable;
            } else {
                flags &= ~Qt::ItemIsUserCheckable;
                item->setCheckState(0, Qt::Unchecked);
            }
            item->setFlags(flags);
            if (isUsed) {
                item->setCheckState(0, Qt::Unchecked);
                item->setToolTip(0, QStringLiteral("Ya usado en la estructura del pool"));
                for (int c = 0; c < devicesTree->columnCount(); ++c) {
                    item->setBackground(c, QColor("#e0e0e0"));
                }
            } else if (intrinsicSelectable) {
                const bool mounted = item->data(0, kRoleMounted).toBool();
                const bool inPool = item->text(4) == QStringLiteral("Sí");
                applyRowColor(item, inPool ? stRed : (mounted ? stYellow : stGreen));
            }
            for (int i = 0; i < item->childCount(); ++i) {
                visit(item->child(i));
            }
        };
        for (int i = 0; i < devicesTree->topLevelItemCount(); ++i) {
            visit(devicesTree->topLevelItem(i));
        }
    };

    std::function<int(QTreeWidgetItem*)> countEffectiveDevices = [&](QTreeWidgetItem* node) -> int {
        if (!node) {
            return 0;
        }
        const QString kind = node->data(0, kRoleNodeKind).toString();
        if (kind == QStringLiteral("device")) {
            return 1;
        }
        int total = 0;
        for (int i = 0; i < node->childCount(); ++i) {
            total += countEffectiveDevices(node->child(i));
        }
        return total;
    };

    std::function<bool(QTreeWidgetItem*, QString&)> validatePoolNode =
        [&](QTreeWidgetItem* node, QString& error) -> bool {
            if (!node) {
                error = QStringLiteral("Estructura vacía");
                return false;
            }
            const QString kind = poolNodeKind(node);
            if (kind == QStringLiteral("device")) {
                const QString path = node->data(0, kRoleDevicePath).toString().trimmed();
                if (path.isEmpty()) {
                    error = QStringLiteral("Hay un nodo device sin ruta");
                    return false;
                }
                return true;
            }

            if (kind == QStringLiteral("class")) {
                const QString prefix = poolNodePrefix(node);
                if (!isTopLevelClassPrefix(prefix)) {
                    error = QStringLiteral("Clase especial inválida '%1'").arg(node->text(0));
                    return false;
                }
                if (node->parent() != poolRootItem) {
                    error = QStringLiteral("La clase '%1' debe colgar de Pool").arg(node->text(0));
                    return false;
                }
                if ((prefix == QStringLiteral("cache") || prefix == QStringLiteral("spare"))
                    && node->childCount() == 0) {
                    error = QStringLiteral("La clase '%1' no contiene devices").arg(node->text(0));
                    return false;
                }
            }

            if (kind == QStringLiteral("vdev")) {
                const int deviceCount = countEffectiveDevices(node);
                if (deviceCount == 0) {
                    error = QStringLiteral("El nodo '%1' no contiene devices").arg(node->text(0));
                    return false;
                }
                const QString prefix = poolNodePrefix(node);
                const QString parentKind = poolNodeKind(node->parent());
                const QString parentPrefix = poolNodePrefix(node->parent());
                if (node->parent() == poolRootItem && !isNormalTopLevelVdevPrefix(prefix)) {
                    error = QStringLiteral("El VDEV '%1' no puede ser root").arg(node->text(0));
                    return false;
                }
                if (parentKind == QStringLiteral("vdev")) {
                    error = QStringLiteral("No se permiten VDEVs dentro de VDEVs").arg(node->text(0));
                    return false;
                }
                if (parentKind == QStringLiteral("class") && !classAllowsNestedVdev(parentPrefix, prefix)) {
                    error = QStringLiteral("El nodo '%1' no está permitido dentro de '%2'").arg(node->text(0), node->parent()->text(0));
                    return false;
                }
                if (parentKind != QStringLiteral("root") && parentKind != QStringLiteral("class")) {
                    error = QStringLiteral("Ubicación inválida para '%1'").arg(node->text(0));
                    return false;
                }
                if ((prefix == QStringLiteral("mirror"))
                    && deviceCount < 2) {
                    error = QStringLiteral("El nodo '%1' necesita al menos 2 devices").arg(node->text(0));
                    return false;
                }
                if (prefix == QStringLiteral("raidz") && deviceCount < 2) {
                    error = QStringLiteral("El nodo '%1' necesita al menos 2 devices").arg(node->text(0));
                    return false;
                }
                if (prefix == QStringLiteral("raidz2") && deviceCount < 3) {
                    error = QStringLiteral("El nodo '%1' necesita al menos 3 devices").arg(node->text(0));
                    return false;
                }
                if (prefix == QStringLiteral("raidz3") && deviceCount < 4) {
                    error = QStringLiteral("El nodo '%1' necesita al menos 4 devices").arg(node->text(0));
                    return false;
                }
            }

            for (int i = 0; i < node->childCount(); ++i) {
                if (!validatePoolNode(node->child(i), error)) {
                    return false;
                }
            }
            return true;
        };

    auto updatePoolCommandPreview = [&]() {
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
        auto addPreviewOpt = [&parts](const QString& key, const QString& value) {
            if (!value.trimmed().isEmpty()) {
                parts << QStringLiteral("-o") << shSingleQuote(key + QStringLiteral("=") + value.trimmed());
            }
        };
        addPreviewOpt(QStringLiteral("ashift"), ashiftCb->currentText());
        addPreviewOpt(QStringLiteral("autotrim"), autotrimCb->currentText());
        addPreviewOpt(QStringLiteral("autoexpand"), autoexpandCb->currentText());
        addPreviewOpt(QStringLiteral("compatibility"), compatibilityCb->currentText());
        addPreviewOpt(QStringLiteral("bootfs"), bootfsEd->text());
        for (const QString& item : poolOptsEd->text().split(',', Qt::SkipEmptyParts)) {
            const QString t = item.trimmed();
            if (!t.isEmpty()) {
                parts << QStringLiteral("-o") << shSingleQuote(t);
            }
        }
        auto addPreviewFsProp = [&parts](const QString& key, const QString& value) {
            if (!value.trimmed().isEmpty()) {
                parts << QStringLiteral("-O") << shSingleQuote(key + QStringLiteral("=") + value.trimmed());
            }
        };
        addPreviewFsProp(QStringLiteral("canmount"), canmountCb->currentText());
        addPreviewFsProp(QStringLiteral("mountpoint"), dsMountpointEd->text());
        addPreviewFsProp(QStringLiteral("relatime"), relatimeCb->currentText());
        addPreviewFsProp(QStringLiteral("compression"), compressionCb->currentText());
        addPreviewFsProp(QStringLiteral("normalization"), normalizationCb->currentText());
        addPreviewFsProp(QStringLiteral("acltype"), acltypeCb->currentText());
        addPreviewFsProp(QStringLiteral("xattr"), xattrCb->currentText());
        addPreviewFsProp(QStringLiteral("dnodesize"), dnodesizeCb->currentText());
        const QSet<QString> explicitFsKeys = {
            QStringLiteral("canmount"), QStringLiteral("mountpoint"), QStringLiteral("relatime"),
            QStringLiteral("compression"), QStringLiteral("normalization"), QStringLiteral("acltype"),
            QStringLiteral("xattr"), QStringLiteral("dnodesize")
        };
        for (const QString& item : fsPropsEd->text().split(',', Qt::SkipEmptyParts)) {
            const QString t = item.trimmed();
            if (t.isEmpty()) {
                continue;
            }
            const int eq = t.indexOf('=');
            if (eq > 0) {
                const QString k = t.left(eq).trimmed().toLower();
                if (explicitFsKeys.contains(k)) {
                    continue;
                }
            }
            parts << QStringLiteral("-O") << shSingleQuote(t);
        }
        const QString previewPool = poolNameEd->text().trimmed().isEmpty()
                                        ? QStringLiteral("<pool>")
                                        : poolNameEd->text().trimmed();
        parts << previewPool;
        parts << poolTreeSpecLines();
        if (!extraEd->text().trimmed().isEmpty()) {
            parts << extraEd->text().trimmed();
        }
        poolCmdPreview->setText(parts.join(' '));
        QString error;
        bool valid = !poolNameEd->text().trimmed().isEmpty();
        if (!valid) {
            error = QStringLiteral("El nombre del pool está vacío");
        } else if (poolRootItem->childCount() == 0) {
            valid = false;
            error = QStringLiteral("No hay ningún VDEV definido");
        } else {
            bool hasNormalRoot = false;
            for (int i = 0; i < poolRootItem->childCount(); ++i) {
                QTreeWidgetItem* child = poolRootItem->child(i);
                if (poolNodeKind(child) == QStringLiteral("vdev")
                    || poolNodeKind(child) == QStringLiteral("device")) {
                    hasNormalRoot = true;
                }
                if (!validatePoolNode(child, error)) {
                    valid = false;
                    break;
                }
            }
            if (valid && !hasNormalRoot) {
                valid = false;
                error = QStringLiteral("Debe existir al menos un VDEV de datos en la raíz");
            }
        }
        if (!valid) {
            poolCmdPreview->setStyleSheet(QStringLiteral("color: #b00020;"));
            poolCmdPreview->setToolTip(error);
        } else {
            poolCmdPreview->setStyleSheet(QString());
            poolCmdPreview->setToolTip(QString());
        }
    };

    auto removePoolNode = [&]() {
        if (QTreeWidgetItem* current = poolTree->currentItem()) {
            if (current == poolRootItem) {
                return;
            }
            delete current;
            syncDeviceAvailability();
            updatePoolCommandPreview();
        }
    };

    connect(poolTree, &QTreeWidget::customContextMenuRequested, &dlg, [&](const QPoint& pos) {
        QTreeWidgetItem* item = poolTree->itemAt(pos);
        if (!item) {
            item = poolRootItem;
        }
        if (poolNodeKind(item) == QStringLiteral("device")) {
            item = item->parent() ? item->parent() : poolRootItem;
        }

        QMenu menu(poolTree);
        struct GroupActionDef {
            QString label;
            QString prefix;
        };
        QVector<GroupActionDef> defs;
        const QString kind = poolNodeKind(item);
        const QString prefix = poolNodePrefix(item);
        if (kind == QStringLiteral("root")) {
            defs = {
                {QStringLiteral("Mirror"), QStringLiteral("mirror")},
                {QStringLiteral("RAIDZ"), QStringLiteral("raidz")},
                {QStringLiteral("RAIDZ2"), QStringLiteral("raidz2")},
                {QStringLiteral("RAIDZ3"), QStringLiteral("raidz3")},
                {QStringLiteral("Log"), QStringLiteral("log")},
                {QStringLiteral("Cache"), QStringLiteral("cache")},
                {QStringLiteral("Special"), QStringLiteral("special")},
                {QStringLiteral("Dedup"), QStringLiteral("dedup")},
                {QStringLiteral("Spare"), QStringLiteral("spare")},
            };
        } else if (kind == QStringLiteral("class")) {
            if (prefix == QStringLiteral("log")) {
                defs = {{QStringLiteral("Mirror"), QStringLiteral("mirror")}};
            } else if (prefix == QStringLiteral("special") || prefix == QStringLiteral("dedup")) {
                defs = {
                    {QStringLiteral("Mirror"), QStringLiteral("mirror")},
                    {QStringLiteral("RAIDZ"), QStringLiteral("raidz")},
                    {QStringLiteral("RAIDZ2"), QStringLiteral("raidz2")},
                    {QStringLiteral("RAIDZ3"), QStringLiteral("raidz3")},
                };
            }
        }
        for (const GroupActionDef& def : defs) {
            QAction* action = menu.addAction(def.label);
            QObject::connect(action, &QAction::triggered, &dlg, [&, def, item]() {
                if (createPoolGroupNode(item, def.label, def.prefix)) {
                    updatePoolCommandPreview();
                }
            });
        }
        if (poolTree->currentItem() && poolTree->currentItem() != poolRootItem) {
            menu.addSeparator();
            QAction* removeAction = menu.addAction(QStringLiteral("Eliminar nodo"));
            QObject::connect(removeAction, &QAction::triggered, &dlg, removePoolNode);
        }
        menu.exec(poolTree->viewport()->mapToGlobal(pos));
    });

    poolTree->handleExternalDrop = [&](const QStringList& pathsToAdd, QTreeWidgetItem* targetItem) {
        QTreeWidgetItem* groupItem = targetItem;
        if (!groupItem || groupItem == poolRootItem || poolNodeKind(groupItem) == QStringLiteral("root")) {
            groupItem = poolRootItem;
        } else if (poolNodeKind(groupItem) == QStringLiteral("device")) {
            groupItem = groupItem->parent();
        }
        if (!groupItem) {
            return;
        }
        const QString targetKind = poolNodeKind(groupItem);
        const QString targetPrefix = poolNodePrefix(groupItem);
        if (!canParentPoolNode(groupItem, QStringLiteral("device"), QString())) {
            if (targetKind == QStringLiteral("class") && (targetPrefix == QStringLiteral("cache")
                || targetPrefix == QStringLiteral("spare") || targetPrefix == QStringLiteral("log")
                || targetPrefix == QStringLiteral("special") || targetPrefix == QStringLiteral("dedup"))) {
                // allowed direct-device classes already covered above
            } else {
                return;
            }
        }
        if (targetKind != QStringLiteral("vdev") && targetKind != QStringLiteral("class")
            && targetKind != QStringLiteral("root")) {
            return;
        }
        for (const QString& rawPath : pathsToAdd) {
            const QString path = rawPath.trimmed();
            if (path.isEmpty() || poolTreeContainsDevice(path)) {
                continue;
            }
            auto* leaf = new QTreeWidgetItem(QStringList{path});
            leaf->setData(0, kRoleNodeKind, QStringLiteral("device"));
            leaf->setData(0, kRoleDevicePath, path);
            leaf->setFlags((leaf->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled & ~Qt::ItemIsUserCheckable);
            groupItem->addChild(leaf);
        }
        groupItem->setExpanded(true);
        syncDeviceAvailability();
        updatePoolCommandPreview();
    };
    connect(addSelectedBtn, &QPushButton::clicked, &dlg, [&]() {
        const QStringList selectedPaths = checkedDevices();
        if (selectedPaths.isEmpty()) {
            return;
        }
        QTreeWidgetItem* targetItem = poolTree->currentItem();
        if (!targetItem) {
            targetItem = poolRootItem;
        }
        poolTree->handleExternalDrop(selectedPaths, targetItem);
        clearDeviceChecks();
    });
    poolTree->canAcceptInternalDrop = [&](const QList<QTreeWidgetItem*>& draggedItems, QTreeWidgetItem* targetItem) -> bool {
        if (draggedItems.isEmpty()) {
            return false;
        }
        QTreeWidgetItem* effectiveTarget = targetItem ? targetItem : poolRootItem;
        if (poolNodeKind(effectiveTarget) == QStringLiteral("device")) {
            effectiveTarget = effectiveTarget->parent() ? effectiveTarget->parent() : poolRootItem;
        }
        for (QTreeWidgetItem* dragged : draggedItems) {
            if (!dragged || dragged == poolRootItem || dragged == effectiveTarget) {
                return false;
            }
            for (QTreeWidgetItem* p = effectiveTarget; p; p = p->parent()) {
                if (p == dragged) {
                    return false;
                }
            }
            const QString draggedKind = poolNodeKind(dragged);
            const QString draggedPrefix = poolNodePrefix(dragged);
            if (!canParentPoolNode(effectiveTarget, draggedKind, draggedPrefix)) {
                return false;
            }
        }
        return true;
    };
    poolTree->handleInternalDrop = [&]() {
        syncDeviceAvailability();
        updatePoolCommandPreview();
    };

    connect(poolNameEd, &QLineEdit::textChanged, &dlg, [&, updatePoolCommandPreview](const QString&) {
        updatePoolCommandPreview();
    });
    connect(forceCb, &QCheckBox::toggled, &dlg, [&, updatePoolCommandPreview](bool) {
        updatePoolCommandPreview();
    });
    connect(dryRunCb, &QCheckBox::toggled, &dlg, [&, updatePoolCommandPreview](bool) {
        updatePoolCommandPreview();
    });
    auto connectPreviewLineEdit = [&](QLineEdit* edit) {
        QObject::connect(edit, &QLineEdit::textChanged, &dlg, [&, updatePoolCommandPreview](const QString&) {
            updatePoolCommandPreview();
        });
    };
    auto connectPreviewCombo = [&](QComboBox* combo) {
        QObject::connect(combo, &QComboBox::currentTextChanged, &dlg, [&, updatePoolCommandPreview](const QString&) {
            updatePoolCommandPreview();
        });
    };
    connectPreviewLineEdit(mountpointEd);
    connectPreviewLineEdit(altrootEd);
    connectPreviewLineEdit(bootfsEd);
    connectPreviewLineEdit(poolOptsEd);
    connectPreviewLineEdit(dsMountpointEd);
    connectPreviewLineEdit(fsPropsEd);
    connectPreviewLineEdit(extraEd);
    connectPreviewCombo(ashiftCb);
    connectPreviewCombo(autotrimCb);
    connectPreviewCombo(autoexpandCb);
    connectPreviewCombo(compatibilityCb);
    connectPreviewCombo(canmountCb);
    connectPreviewCombo(relatimeCb);
    connectPreviewCombo(compressionCb);
    connectPreviewCombo(normalizationCb);
    connectPreviewCombo(acltypeCb);
    connectPreviewCombo(xattrCb);
    connectPreviewCombo(dnodesizeCb);
    syncDeviceAvailability();
    updatePoolCommandPreview();

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->setFont(baseUiFont);
    QObject::connect(&dlg, &QDialog::finished, &dlg, [devicesTree]() {
        if (!devicesTree) {
            return;
        }
        const auto combos = devicesTree->findChildren<QComboBox*>();
        for (QComboBox* combo : combos) {
            QObject::disconnect(combo, nullptr, nullptr, nullptr);
        }
        QObject::disconnect(devicesTree, nullptr, nullptr, nullptr);
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&]() {
        const QString poolName = poolNameEd->text().trimmed();
        if (poolName.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_poolcrt_auto031"), QStringLiteral("Nombre de pool vacío."),
                                     QStringLiteral("Pool name is empty."),
                                     QStringLiteral("池名称为空。")));
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
        addOpt(QStringLiteral("ashift"), ashiftCb->currentText());
        addOpt(QStringLiteral("autotrim"), autotrimCb->currentText());
        addOpt(QStringLiteral("autoexpand"), autoexpandCb->currentText());
        addOpt(QStringLiteral("compatibility"), compatibilityCb->currentText());
        addOpt(QStringLiteral("bootfs"), bootfsEd->text());
        for (const QString& item : poolOptsEd->text().split(',', Qt::SkipEmptyParts)) {
            const QString t = item.trimmed();
            if (!t.isEmpty()) {
                parts << QStringLiteral("-o") << shSingleQuote(t);
            }
        }
        auto addFsProp = [&parts](const QString& key, const QString& value) {
            if (!value.trimmed().isEmpty()) {
                parts << QStringLiteral("-O") << shSingleQuote(key + QStringLiteral("=") + value.trimmed());
            }
        };
        addFsProp(QStringLiteral("canmount"), canmountCb->currentText());
        addFsProp(QStringLiteral("mountpoint"), dsMountpointEd->text());
        addFsProp(QStringLiteral("relatime"), relatimeCb->currentText());
        addFsProp(QStringLiteral("compression"), compressionCb->currentText());
        addFsProp(QStringLiteral("normalization"), normalizationCb->currentText());
        addFsProp(QStringLiteral("acltype"), acltypeCb->currentText());
        addFsProp(QStringLiteral("xattr"), xattrCb->currentText());
        addFsProp(QStringLiteral("dnodesize"), dnodesizeCb->currentText());

        const QSet<QString> explicitFsKeys = {
            QStringLiteral("canmount"), QStringLiteral("mountpoint"), QStringLiteral("relatime"),
            QStringLiteral("compression"), QStringLiteral("normalization"), QStringLiteral("acltype"),
            QStringLiteral("xattr"), QStringLiteral("dnodesize")
        };
        for (const QString& item : fsPropsEd->text().split(',', Qt::SkipEmptyParts)) {
            const QString t = item.trimmed();
            if (!t.isEmpty()) {
                const int eq = t.indexOf('=');
                if (eq > 0) {
                    const QString k = t.left(eq).trimmed().toLower();
                    if (explicitFsKeys.contains(k)) {
                        continue;
                    }
                }
                parts << QStringLiteral("-O") << shSingleQuote(t);
            }
        }

        parts << shSingleQuote(poolName);
        QStringList specLines = poolTreeSpecLines();
        if (specLines.isEmpty()) {
            if (selectedDevices.isEmpty()) {
                QMessageBox::warning(
                    this, QStringLiteral("ZFSMgr"),
                    trk(QStringLiteral("t_poolcrt_auto032"), QStringLiteral("Defina la estructura del pool en el árbol o seleccione dispositivos para modo rápido."),
                        QStringLiteral("Build the pool layout in the tree or select devices for quick mode."),
                        QStringLiteral("请在树中构建池结构，或在快速模式中选择设备。")));
                return;
            }
            specLines << selectedDevices.join(' ');
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
        const QString createCmd = parts.join(' ');
        QString cmd = createCmd;
        if (isMacConn) {
            QStringList selectedYellow;
            for (const QString& d : selectedDevices) {
                if (yellowDevicePaths.contains(d)) {
                    selectedYellow << d;
                }
            }
            if (!selectedYellow.isEmpty()) {
                QStringList pre;
                for (const QString& d : selectedYellow) {
                    pre << QStringLiteral("diskutil unmount %1").arg(shSingleQuote(d));
                }
                cmd = QStringLiteral("set -e; %1; %2").arg(pre.join(QStringLiteral("; ")), createCmd);
            }
        }
        ConnectionProfile execProfile = p;
        if (isLocalConnection(execProfile) && !isWindowsConnection(execProfile)) {
            execProfile.useSudo = true;
            if (!ensureLocalSudoCredentials(execProfile)) {
                appLog(QStringLiteral("INFO"), QStringLiteral("Crear pool cancelada: faltan credenciales sudo locales"));
                return;
            }
        }
        cmd = withSudo(execProfile, cmd);
        const QString preview = QStringLiteral("[%1]\n%2")
                                    .arg(sshUserHostPort(execProfile))
                                    .arg(buildSshPreviewCommand(execProfile, cmd));
        if (!confirmActionExecution(QStringLiteral("Crear pool"), {preview})) {
            return;
        }
        DatasetSelectionContext refreshCtx;
        refreshCtx.valid = true;
        refreshCtx.connIdx = idx;
        refreshCtx.poolName = poolName;
        const QString connLabel = execProfile.name.trimmed().isEmpty() ? execProfile.id.trimmed() : execProfile.name.trimmed();
        QString errorText;
        if (!queuePendingShellAction(PendingShellActionDraft{
                QStringLiteral("%1::%2").arg(connLabel, poolName),
                QStringLiteral("Crear pool %1").arg(poolName),
                sshExecFromLocal(execProfile, cmd),
                120000,
                false,
                {},
                refreshCtx,
                PendingShellActionDraft::RefreshScope::TargetOnly}, &errorText)) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
            return;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Cambio pendiente añadido: %1::%2  Crear pool %3")
                   .arg(connLabel, poolName, poolName));
        updateApplyPropsButtonState();
        dlg.accept();
    });
    if (dlg.layout()) {
        dlg.layout()->activate();
    }
    for (QWidget* w : dlg.findChildren<QWidget*>()) {
        if (w) {
            w->setFont(baseUiFont);
        }
    }
    if (devicesTree->header()) {
        devicesTree->header()->setFont(baseUiFont);
    }
    if (poolTree->header()) {
        poolTree->header()->setFont(baseUiFont);
    }
    dlg.setMinimumHeight(dlg.sizeHint().height());

    busyGuard.release();
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
}
