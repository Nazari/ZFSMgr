#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "mainwindow_ui_logic.h"
#include "agentversion.h"
#include "daemonpayload.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QEventLoop>
#include <QDir>
#include <QGroupBox>
#include <QHeaderView>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QFile>
#include <QFileInfo>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QStackedWidget>
#include <QSplitter>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QJsonDocument>
#include <QJsonObject>

#include <QtConcurrent/QtConcurrent>

#include <chrono>

// Defined in mainwindow_remote.cpp — free function, thread-safe.
bool runSshRawNoLog(const ConnectionProfile& p,
                    const QString& remoteCmd,
                    int timeoutMs,
                    QString& out,
                    QString& err,
                    int& rc);

namespace {
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kIsConnectionRootRole = Qt::UserRole + 36;
constexpr int kConnRootSectionRole = Qt::UserRole + 37;
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;
QString connContentStateTokenForTree(QTreeWidget* tree) {
    if (!tree) {
        return QString();
    }
    auto tokenFromItem = [](QTreeWidgetItem* item) -> QString {
        if (!item) {
            return QString();
        }
        QTreeWidgetItem* owner = item;
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, kIsPoolRootRole).toBool()) {
            owner = owner->parent();
        }
        if (!owner) {
            return QString();
        }
        const int connIdx = owner->data(0, kConnIdxRole).toInt();
        const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
        if (connIdx < 0 || poolName.isEmpty()) {
            return QString();
        }
        return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    };
    if (QTreeWidgetItem* current = tree->currentItem()) {
        const QString token = tokenFromItem(current);
        if (!token.isEmpty()) {
            return token;
        }
    }
    // Unified trees (groupPoolsByConnectionRoots) have connection roots at the top
    // level, not pool roots. Walk into each connection root to find the first pool
    // root and derive a token from it so the state key is never empty.
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = tree->topLevelItem(i);
        if (!root) {
            continue;
        }
        if (root->data(0, kIsPoolRootRole).toBool()) {
            const QString token = tokenFromItem(root);
            if (!token.isEmpty()) {
                return token;
            }
        } else if (root->data(0, kIsConnectionRootRole).toBool()) {
            // Unified tree: look for pool roots among this connection root's children.
            for (int j = 0; j < root->childCount(); ++j) {
                QTreeWidgetItem* child = root->child(j);
                if (child && child->data(0, kIsPoolRootRole).toBool()) {
                    const QString token = tokenFromItem(child);
                    if (!token.isEmpty()) {
                        return token;
                    }
                }
            }
        }
    }
    return QString();
}

QString mergedConnectionCommandErrorText(const QString& out, const QString& err, int rc) {
    QStringList parts;
    const QString trimmedErr = err.trimmed();
    const QString trimmedOut = out.trimmed();
    if (!trimmedErr.isEmpty()) {
        parts << trimmedErr;
    }
    if (!trimmedOut.isEmpty()) {
        parts << trimmedOut;
    }
    if (parts.isEmpty()) {
        return QStringLiteral("exit %1").arg(rc);
    }
    return parts.join(QStringLiteral("\n\n"));
}

QString stripLeadingSudoForExecution(QString cmd) {
    cmd = cmd.trimmed();
    if (cmd.startsWith(QStringLiteral("sudo "))) {
        cmd = cmd.mid(5).trimmed();
    }
    cmd.replace(QStringLiteral("&& sudo "), QStringLiteral("&& "));
    cmd.replace(QStringLiteral("; sudo "), QStringLiteral("; "));
    cmd.replace(QStringLiteral("\n sudo "), QStringLiteral("\n "));
    return cmd.trimmed();
}

QString normalizedTargetPlatformId(bool isMac, bool isFreeBsd, bool isWindows) {
    if (isWindows) {
        return QStringLiteral("windows");
    }
    if (isMac) {
        return QStringLiteral("macos");
    }
    if (isFreeBsd) {
        return QStringLiteral("freebsd");
    }
    return QStringLiteral("linux");
}

QStringList archAliasesForTarget(const QString& archHintRaw) {
    const QString arch = archHintRaw.trimmed().toLower();
    if (arch.contains(QStringLiteral("aarch64")) || arch.contains(QStringLiteral("arm64"))) {
        return {QStringLiteral("arm64"), QStringLiteral("aarch64")};
    }
    if (arch.contains(QStringLiteral("x86_64")) || arch.contains(QStringLiteral("amd64"))) {
        return {QStringLiteral("x86_64"), QStringLiteral("amd64")};
    }
    if (arch.contains(QStringLiteral("i686")) || arch.contains(QStringLiteral("i386"))) {
        return {QStringLiteral("i686"), QStringLiteral("i386")};
    }
    QStringList out;
    if (!arch.isEmpty()) {
        out << arch;
    }
    out << QStringLiteral("x86_64") << QStringLiteral("amd64") << QStringLiteral("arm64") << QStringLiteral("aarch64");
    out.removeDuplicates();
    return out;
}

QString findDeployableAgentBinaryPath(const QString& platformId, const QString& archHintRaw) {
    const QString ext = (platformId == QStringLiteral("windows")) ? QStringLiteral(".exe") : QString();
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList roots = {
        appDir,
        QDir(appDir).absoluteFilePath(QStringLiteral("..")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../..")),
        QDir::currentPath()
    };
    for (QString& r : roots) {
        r = QDir::cleanPath(r);
    }
    roots.removeDuplicates();

    const QStringList archAliases = archAliasesForTarget(archHintRaw);

    // 1) Buscar primero en bundle empaquetado dentro de la propia app/instalación.
    for (const QString& arch : archAliases) {
        const QString rel = platformId + QStringLiteral("-") + arch + QStringLiteral("/zfsmgr_agent") + ext;
        const QStringList packagedCandidates = {
            QDir::cleanPath(appDir + QStringLiteral("/agents/") + rel),
            QDir::cleanPath(appDir + QStringLiteral("/../agents/") + rel),
            QDir::cleanPath(appDir + QStringLiteral("/../Resources/agents/") + rel),
            QDir::cleanPath(appDir + QStringLiteral("/../share/zfsmgr/agents/") + rel),
            QDir::cleanPath(appDir + QStringLiteral("/../../share/zfsmgr/agents/") + rel)
        };
        for (const QString& c : packagedCandidates) {
            const QFileInfo fi(c);
            if (fi.exists() && fi.isFile()) {
                return fi.absoluteFilePath();
            }
        }
    }

    // 2) Buscar en artefactos/builds locales del workspace (entorno dev).
    for (const QString& root : roots) {
        for (const QString& arch : archAliases) {
            const QString stable = QDir::cleanPath(
                root + QStringLiteral("/builds/agents/") + platformId + QStringLiteral("-") + arch
                + QStringLiteral("/zfsmgr_agent") + ext);
            const QFileInfo fi(stable);
            if (fi.exists() && fi.isFile()) {
                return fi.absoluteFilePath();
            }
        }
        for (const QString& arch : archAliases) {
            const QString crossDir =
                QDir::cleanPath(root + QStringLiteral("/builds/cross-") + platformId + QStringLiteral("-") + arch);
            const QFileInfo fi(QDir::cleanPath(crossDir + QStringLiteral("/zfsmgr_agent") + ext));
            if (fi.exists() && fi.isFile()) {
                return fi.absoluteFilePath();
            }
        }
        const QFileInfo genericCross(
            QDir::cleanPath(root + QStringLiteral("/builds/cross-") + platformId + QStringLiteral("/zfsmgr_agent") + ext));
        if (genericCross.exists() && genericCross.isFile()) {
            return genericCross.absoluteFilePath();
        }
    }

    const QFileInfo localFi(QDir::cleanPath(appDir + QStringLiteral("/zfsmgr_agent") + ext));
    if (localFi.exists() && localFi.isFile()) {
        return localFi.absoluteFilePath();
    }
    return QString();
}

QVector<int> versionOrderingKey(const QString& version) {
    QVector<int> out;
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)(?:rc(\\d+))?(?:[.-](\\d+))?$"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = rx.match(version.trimmed());
    if (!m.hasMatch()) {
        return out;
    }
    out << m.captured(1).toInt()
        << m.captured(2).toInt()
        << m.captured(3).toInt();
    if (m.captured(4).isEmpty()) {
        out << 999999;
    } else {
        out << m.captured(4).toInt();
    }
    out << (m.captured(5).isEmpty() ? 0 : m.captured(5).toInt());
    return out;
}

int compareAppVersions(const QString& a, const QString& b) {
    const QVector<int> ka = versionOrderingKey(a);
    const QVector<int> kb = versionOrderingKey(b);
    if (ka.isEmpty() || kb.isEmpty()) {
        return QString::compare(a.trimmed(), b.trimmed(), Qt::CaseInsensitive);
    }
    for (int i = 0; i < qMin(ka.size(), kb.size()); ++i) {
        if (ka[i] < kb[i]) {
            return -1;
        }
        if (ka[i] > kb[i]) {
            return 1;
        }
    }
    return 0;
}

QString escapePsSingleQuoted(QString s) {
    return s.replace('\'', QStringLiteral("''"));
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

} // namespace

int MainWindow::currentConnectionIndexFromUnifiedTree() const {
    if (!m_connContentTree
        || !m_connContentTree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()
        || !m_connContentTree->isVisible()) {
        return -1;
    }
    QTreeWidgetItem* item = m_connContentTree->currentItem();
    while (item && !item->data(0, kIsConnectionRootRole).toBool()
           && !item->data(0, kIsPoolRootRole).toBool()) {
        item = item->parent();
    }
    if (!item) {
        return -1;
    }
    return item->data(0, kConnIdxRole).toInt();
}

int MainWindow::currentConnectionIndexFromUi() const {
    const int treeIdx = currentConnectionIndexFromUnifiedTree();
    if (treeIdx >= 0 && treeIdx < m_profiles.size()) {
        return treeIdx;
    }
    return -1;
}

void MainWindow::setCurrentConnectionInUi(int connIdx) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    if (m_connContentTree
        && m_connContentTree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()) {
        for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* root = m_connContentTree->topLevelItem(i);
            if (!root || !root->data(0, kIsConnectionRootRole).toBool()) {
                continue;
            }
            if (root->data(0, kConnIdxRole).toInt() == connIdx) {
                m_connContentTree->setCurrentItem(root);
                break;
            }
        }
    }
}

QColor MainWindow::connectionStateRowColor(int connIdx) const {
    const QColor baseColor = palette().base().color();
    if (connIdx < 0 || connIdx >= m_states.size()) {
        return baseColor;
    }
    if (isConnectionDisconnected(connIdx)) {
        return QColor(QStringLiteral("#eef1f4"));
    }
    const ConnectionRuntimeState& st = m_states[connIdx];
    const QString status = st.status.trimmed().toUpper();
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = rx.match(st.zfsVersion.trimmed());
    const bool zfsTooOld = m.hasMatch()
                           && m.captured(1).toInt() == 2
                           && (m.captured(2).toInt() < 3
                               || (m.captured(2).toInt() == 3
                                   && (m.captured(3).isEmpty() ? 0 : m.captured(3).toInt()) < 3));
    if (status == QStringLiteral("OK")) {
        if (zfsTooOld) {
            return QColor(QStringLiteral("#f9dfdf"));
        }
        return st.missingUnixCommands.isEmpty() ? QColor(QStringLiteral("#e4f4e4"))
                                                : QColor(QStringLiteral("#fff1d9"));
    }
    if (!status.isEmpty()) {
        return QColor(QStringLiteral("#f9dfdf"));
    }
    return baseColor;
}

QString MainWindow::connectionStateColorReason(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()) {
        return QString();
    }
    const ConnectionRuntimeState& st = m_states[connIdx];
    if (isConnectionDisconnected(connIdx)) {
        return trk(QStringLiteral("t_conn_color_reason_off_001"),
                   QStringLiteral("La conexión está marcada como desconectada."),
                   QStringLiteral("The connection is marked as disconnected."),
                   QStringLiteral("该连接已标记为断开。"));
    }
    const QString stUp = st.status.trimmed().toUpper();
    if (stUp != QStringLiteral("OK")) {
        return st.detail.trimmed().isEmpty()
                   ? trk(QStringLiteral("t_conn_color_reason_err_001"),
                         QStringLiteral("La validación de la conexión ha fallado."),
                         QStringLiteral("Connection validation failed."),
                         QStringLiteral("连接校验失败。"))
                   : st.detail.trimmed();
    }
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = rx.match(st.zfsVersion.trimmed());
    const bool zfsTooOld = m.hasMatch()
                           && m.captured(1).toInt() == 2
                           && (m.captured(2).toInt() < 3
                               || (m.captured(2).toInt() == 3
                                   && (m.captured(3).isEmpty() ? 0 : m.captured(3).toInt()) < 3));
    if (zfsTooOld) {
        return trk(QStringLiteral("t_conn_color_reason_zfs_old_001"),
                   QStringLiteral("La versión de OpenZFS es demasiado antigua (mínimo recomendado: 2.3.3)."),
                   QStringLiteral("The OpenZFS version is too old (recommended minimum: 2.3.3)."),
                   QStringLiteral("OpenZFS 版本过旧（建议至少 2.3.3）。"));
    }
    if (!st.missingUnixCommands.isEmpty()) {
        return trk(QStringLiteral("t_conn_color_reason_cmds_001"),
                   QStringLiteral("Faltan comandos auxiliares requeridos: %1"),
                   QStringLiteral("Required helper commands are missing: %1"),
                   QStringLiteral("缺少必需的辅助命令：%1"))
            .arg(st.missingUnixCommands.join(QStringLiteral(", ")));
    }
    return QString();
}

QString MainWindow::connectionStateTooltipHtml(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()) {
        return QString();
    }
    const ConnectionProfile p = m_profiles[connIdx];
    const ConnectionRuntimeState& st = m_states[connIdx];
    const bool disconnected = isConnectionDisconnected(connIdx);
    const QString osHint = (p.osType + QStringLiteral(" ") + st.osLine).trimmed().toLower();
    const bool windowsSshConn =
        p.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0
        && osHint.contains(QStringLiteral("windows"));
    QStringList lines;
    lines << QStringLiteral("Host: %1").arg(p.host);
    lines << QStringLiteral("Port: %1").arg(p.port);
    lines << QStringLiteral("Estado: %1").arg(st.status.trimmed().isEmpty() ? QStringLiteral("-") : st.status.trimmed());
    const QString colorReason = connectionStateColorReason(connIdx);
    if (!colorReason.isEmpty()) {
        lines << QStringLiteral("Motivo del color: %1").arg(colorReason);
    }
    lines << QStringLiteral("Sistema operativo: %1")
                 .arg(st.osLine.trimmed().isEmpty() ? QStringLiteral("-") : st.osLine.trimmed());
    lines << QStringLiteral("Método de conexión: %1")
                 .arg(st.connectionMethod.trimmed().isEmpty() ? p.connType : st.connectionMethod.trimmed());
    lines << QStringLiteral("OpenZFS: %1")
                 .arg(st.zfsVersionFull.trimmed().isEmpty()
                          ? (st.zfsVersion.trimmed().isEmpty() ? QStringLiteral("-")
                                                               : QStringLiteral("OpenZFS %1").arg(st.zfsVersion.trimmed()))
                          : st.zfsVersionFull.trimmed());
    lines << QStringLiteral("Daemon: %1")
                 .arg(!st.daemonInstalled ? QStringLiteral("no instalado")
                                          : QStringLiteral("%1 | %2 | %3")
                                                .arg(st.daemonVersion.trimmed().isEmpty() ? QStringLiteral("-")
                                                                                           : st.daemonVersion.trimmed(),
                                                     st.daemonScheduler.trimmed().isEmpty() ? QStringLiteral("-")
                                                                                            : st.daemonScheduler.trimmed(),
                                                     st.daemonActive ? QStringLiteral("activo")
                                                                     : QStringLiteral("inactivo")));
    lines << QStringLiteral("API daemon: %1")
                 .arg(st.daemonApiVersion.trimmed().isEmpty() ? QStringLiteral("-")
                                                              : st.daemonApiVersion.trimmed());
    // Se informa de lo que la sonda encontró realmente, no de lo que se deduzca del
    // sistema operativo: Windows lleva daemon nativo como todos los demás.
    lines << QStringLiteral("Binario daemon: %1")
                 .arg(st.daemonNativeBinary ? QStringLiteral("nativo")
                                            : QStringLiteral("script/no nativo"));
    if (st.daemonNeedsAttention && !st.daemonAttentionReasons.isEmpty()) {
        lines << QStringLiteral("Atención daemon: %1")
                     .arg(st.daemonAttentionReasons.join(QStringLiteral(", ")));
    }
    lines << QStringLiteral("Comandos detectados: %1")
                 .arg(st.detectedUnixCommands.isEmpty() ? QStringLiteral("(ninguno)")
                                                        : st.detectedUnixCommands.join(QStringLiteral(", ")));
    lines << QStringLiteral("Comandos no detectados: %1")
                 .arg(st.missingUnixCommands.isEmpty() ? QStringLiteral("(ninguno)")
                                                       : st.missingUnixCommands.join(QStringLiteral(", ")));
    // Lo que esta conexión NO puede hacer, y por qué. Antes esto no se decía en
    // ninguna parte: el usuario lo descubría al pulsar y ver fallar la acción.
    {
        using F = zfsmgr::caps::Feature;
        const QVector<QPair<F, QString>> checked = {
            {F::DatasetPermissions, QStringLiteral("Permisos ZFS")},
            {F::AutoSnapshotsGsa, QStringLiteral("Instantáneas automáticas")},
            {F::BackgroundJobs, QStringLiteral("Trabajos en segundo plano")},
            {F::DirBreakdown, QStringLiteral("Desglosar")},
            {F::DirAssemble, QStringLiteral("Ensamblar")},
            {F::DirToDir, QStringLiteral("Hacia Dir")},
            {F::SendRecvStreaming, QStringLiteral("Copiar/Nivelar entre máquinas")},
            {F::RsyncSync, QStringLiteral("Sincronizar con rsync")},
            {F::RepairAltMountpoints, QStringLiteral("Reparar mountpoints temporales")},
            {F::AlternateMount, QStringLiteral("Montaje alternativo")},
        };
        QStringList unavailable;
        for (const auto& entry : checked) {
            QString why;
            if (!featureAvailable(connIdx, entry.first, &why)) {
                unavailable << QStringLiteral("%1 (%2)").arg(entry.second, why);
            }
        }
        if (!unavailable.isEmpty()) {
            lines << QStringLiteral("Funciones no disponibles: %1")
                         .arg(unavailable.join(QStringLiteral("; ")));
        }
    }
    lines << QStringLiteral("Plataforma instalación auxiliar: %1")
                 .arg(st.helperPlatformLabel.trimmed().isEmpty() ? QStringLiteral("-")
                                                                 : st.helperPlatformLabel.trimmed());
    lines << QStringLiteral("Gestor de paquetes: %1")
                 .arg(st.helperPackageManagerLabel.trimmed().isEmpty()
                          ? QStringLiteral("-")
                          : QStringLiteral("%1%2")
                                .arg(st.helperPackageManagerLabel.trimmed(),
                                     st.helperPackageManagerDetected ? QStringLiteral(" (detectado)")
                                                                     : QStringLiteral(" (no detectado)")));
    lines << QStringLiteral("Instalación asistida: %1")
                 .arg(st.helperInstallSupported ? QStringLiteral("sí") : QStringLiteral("no"));
    lines << QStringLiteral("Comandos instalables desde ZFSMgr: %1")
                 .arg(st.helperInstallableCommands.isEmpty()
                          ? QStringLiteral("(ninguno)")
                          : st.helperInstallableCommands.join(QStringLiteral(", ")));
    lines << QStringLiteral("Comandos no soportados por instalador: %1")
                 .arg(st.helperUnsupportedCommands.isEmpty()
                          ? QStringLiteral("(ninguno)")
                          : st.helperUnsupportedCommands.join(QStringLiteral(", ")));
    if (!st.helperInstallReason.trimmed().isEmpty()) {
        lines << QStringLiteral("Motivo instalación asistida: %1").arg(st.helperInstallReason.trimmed());
    }
    if (windowsSshConn && !disconnected
        && st.status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) != 0) {
        lines << QString();
        lines << QStringLiteral("PowerShell para habilitar OpenSSH Server:");
        lines << QStringLiteral("# Install OpenSSH Server");
        lines << QStringLiteral("Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0");
        lines << QString();
        lines << QStringLiteral("# Start and set to Automatic");
        lines << QStringLiteral("Start-Service sshd");
        lines << QStringLiteral("Set-Service -Name sshd -StartupType 'Automatic'");
    }
    QStringList nonImportable;
    for (const PoolImportable& pool : st.importablePools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        const QString stateUp = pool.state.trimmed().toUpper();
        const QString actionTxt = pool.action.trimmed();
        if (stateUp == QStringLiteral("ONLINE") && !actionTxt.isEmpty()) {
            continue;
        }
        QString reason = pool.reason.trimmed();
        if (reason.isEmpty()) {
            reason = QStringLiteral("-");
        }
        nonImportable << QStringLiteral("%1").arg(poolName);
        nonImportable << QStringLiteral("  Motivo: %1").arg(reason);
    }
    lines << QStringLiteral("Pools no importables:");
    if (nonImportable.isEmpty()) {
        lines << QStringLiteral("  (ninguno)");
    } else {
        lines += nonImportable;
    }
    const QString plain = lines.join(QStringLiteral("\n")).toHtmlEscaped();
    return QStringLiteral("<pre style=\"font-family:'SF Mono','Menlo','Monaco','Consolas','Liberation Mono',monospace; white-space:pre;\">%1</pre>").arg(plain);
}

QString MainWindow::connectionPersistKey(int idx) const {
    if (idx < 0 || idx >= m_profiles.size()) {
        return QString();
    }
    const QString id = m_profiles[idx].id.trimmed();
    if (!id.isEmpty()) {
        return id.toLower();
    }
    return m_profiles[idx].name.trimmed().toLower();
}

bool MainWindow::isConnectionDisconnected(int idx) const {
    const QString key = connectionPersistKey(idx);
    return !key.isEmpty() && m_disconnectedConnectionKeys.contains(key);
}

void MainWindow::setConnectionDisconnected(int idx, bool disconnected) {
    const QString key = connectionPersistKey(idx);
    if (key.isEmpty()) {
        return;
    }
    if (disconnected) {
        m_disconnectedConnectionKeys.insert(key);
    } else {
        m_disconnectedConnectionKeys.remove(key);
    }
    saveUiSettings();
}

bool MainWindow::isConnectionRedirectedToLocal(int idx) const {
    if (idx < 0 || idx >= m_profiles.size() || idx >= m_states.size()) {
        return false;
    }
    if (isLocalConnection(idx)) {
        return false;
    }
    const ConnectionRuntimeState& st = m_states[idx];
    if (st.status.trimmed().toUpper() != QStringLiteral("OK")) {
        return false;
    }

    QString localUuid = m_localMachineUuid.trimmed().toLower();
    if (localUuid.isEmpty()) {
        for (int i = 0; i < m_profiles.size() && i < m_states.size(); ++i) {
            if (!isLocalConnection(i)) {
                continue;
            }
            const QString cand = m_states[i].machineUuid.trimmed().toLower();
            if (!cand.isEmpty()) {
                localUuid = cand;
                break;
            }
        }
    }
    const QString remoteUuid = st.machineUuid.trimmed().toLower();
    if (!localUuid.isEmpty() && !remoteUuid.isEmpty()) {
        return localUuid == remoteUuid;
    }
    return isLocalHostForUi(m_profiles[idx].host);
}

namespace {
int connectionIndexForRow(const QTableWidget* table, int row) {
    if (!table || row < 0 || row >= table->rowCount()) {
        return -1;
    }
    for (int col = table->columnCount() - 1; col >= 0; --col) {
        const QTableWidgetItem* it = table->item(row, col);
        if (!it) {
            continue;
        }
        bool ok = false;
        const int idx = it->data(Qt::UserRole).toInt(&ok);
        if (ok) {
            return idx;
        }
    }
    return -1;
}

int selectedConnectionRow(const QTableWidget* table) {
    if (!table) {
        return -1;
    }
    int row = table->currentRow();
    if (row >= 0) {
        return connectionIndexForRow(table, row);
    }
    const auto ranges = table->selectedRanges();
    if (!ranges.isEmpty()) {
        return connectionIndexForRow(table, ranges.first().topRow());
    }
    return -1;
}

int rowForConnectionIndex(const QTableWidget* table, int connIdx) {
    if (!table || connIdx < 0) {
        return -1;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        if (connectionIndexForRow(table, row) == connIdx) {
            return row;
        }
    }
    return -1;
}

QString connPersistKeyFromProfiles(const QVector<ConnectionProfile>& profiles, int connIdx) {
    if (connIdx < 0 || connIdx >= profiles.size()) {
        return QString();
    }
    const QString id = profiles[connIdx].id.trimmed();
    if (!id.isEmpty()) {
        return id.toLower();
    }
    return profiles[connIdx].name.trimmed().toLower();
}

struct ConnectivityProbeResult {
    QString text;
    QString tooltip;
    QString detail;
    bool ok{false};
};

QString connectivityMatrixRemoteProbe(const ConnectionProfile& target, bool verbose = false) {
    const QString unixPathPrefix =
        QStringLiteral("PATH=\"/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin:$PATH\"; export PATH; ");
    const QString targetHost = mwhelpers::shSingleQuote(mwhelpers::sshUserHost(target));
    const QString targetCmd = mwhelpers::shSingleQuote(QStringLiteral("echo ZFSMGR_CONNECT_OK"));
    QString sshBase = mwhelpers::sshBaseCommand(target);
    if (verbose) {
        sshBase.replace(QStringLiteral("-o LogLevel=ERROR"),
                        QStringLiteral("-o LogLevel=DEBUG3 -vvv"));
    }
    if (!target.password.trimmed().isEmpty()) {
        return unixPathPrefix + QStringLiteral(
                   "if command -v sshpass >/dev/null 2>&1; then "
                   "SSHPASS=%1 sshpass -e %2 -o BatchMode=no "
                   "-o PreferredAuthentications=password,keyboard-interactive,publickey "
                   "-o NumberOfPasswordPrompts=1 %3 %4; "
                   "else echo %5 >&2; exit 127; fi")
            .arg(mwhelpers::shSingleQuote(target.password),
                 sshBase,
                 targetHost,
                 targetCmd,
                 mwhelpers::shSingleQuote(QStringLiteral("ZFSMgr: sshpass no disponible para esta prueba")));
    }
    return unixPathPrefix + sshBase + QStringLiteral(" ") + targetHost + QStringLiteral(" ") + targetCmd;
}

QString connectivityMatrixRsyncProbe(const ConnectionProfile& target) {
    const QString unixPathPrefix =
        QStringLiteral("PATH=\"/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin:$PATH\"; export PATH; ");
    const QString targetHost = mwhelpers::shSingleQuote(mwhelpers::sshUserHost(target));
    const QString targetCmd = mwhelpers::shSingleQuote(
        QStringLiteral("if command -v rsync >/dev/null 2>&1; then echo ZFSMGR_RSYNC_OK; else echo ZFSMGR_RSYNC_MISSING >&2; exit 127; fi"));
    const QString baseProbe = target.password.trimmed().isEmpty()
        ? unixPathPrefix + mwhelpers::sshBaseCommand(target) + QStringLiteral(" ") + targetHost + QStringLiteral(" ") + targetCmd
        : unixPathPrefix + QStringLiteral(
              "if command -v sshpass >/dev/null 2>&1; then "
              "SSHPASS=%1 sshpass -e %2 -o BatchMode=no "
              "-o PreferredAuthentications=password,keyboard-interactive,publickey "
              "-o NumberOfPasswordPrompts=1 %3 %4; "
              "else echo %5 >&2; exit 127; fi")
              .arg(mwhelpers::shSingleQuote(target.password),
                   mwhelpers::sshBaseCommand(target),
                   targetHost,
                   targetCmd,
                   mwhelpers::shSingleQuote(QStringLiteral("ZFSMgr: sshpass no disponible para esta prueba")));

    return QStringLiteral(
               "if command -v rsync >/dev/null 2>&1; then "
               "%1; "
               "else echo %2 >&2; exit 127; fi")
        .arg(baseProbe,
             mwhelpers::shSingleQuote(QStringLiteral("ZFSMgr: rsync no disponible en origen para esta prueba")));
}
}

QString MainWindow::connectionDisplayModeForIndex(int connIdx) const {
    if (connIdx < 0) {
        return QString();
    }
    return (connIdx == m_topDetailConnIdx) ? QStringLiteral("source") : QString();
}

int MainWindow::connectionIndexByNameOrId(const QString& value) const {
    const QString wanted = value.trimmed();
    if (wanted.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile p = m_profiles[i];
        if (p.name.trimmed().compare(wanted, Qt::CaseInsensitive) == 0
            || p.id.trimmed().compare(wanted, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

bool MainWindow::connectionsReferToSameMachine(int a, int b) const {
    if (a < 0 || a >= m_profiles.size() || b < 0 || b >= m_profiles.size()) {
        return false;
    }
    QString ua = m_profiles[a].machineUid.trimmed().toLower();
    QString ub = m_profiles[b].machineUid.trimmed().toLower();
    if (ua.isEmpty() && a < m_states.size()) {
        ua = m_states[a].machineUuid.trimmed().toLower();
    }
    if (ub.isEmpty() && b < m_states.size()) {
        ub = m_states[b].machineUuid.trimmed().toLower();
    }
    return !ua.isEmpty() && !ub.isEmpty() && ua == ub;
}

void MainWindow::withConnContentContext(QTreeWidget* tree,
                                        const QString& token,
                                        const std::function<void()>& fn) {
    if (!fn) {
        return;
    }
    QTreeWidget* prevTree = m_connContentTree;
    const QString prevToken = m_connContentToken;
    if (tree) {
        m_connContentTree = tree;
    }
    if (!token.isNull()) {
        m_connContentToken = token;
    }
    fn();
    m_connContentTree = prevTree;
    m_connContentToken = prevToken;
}

int MainWindow::equivalentSshForLocal(int localIdx) const {
    if (localIdx < 0 || localIdx >= m_profiles.size() || !isLocalConnection(localIdx)) {
        return -1;
    }
    QString localUid = m_profiles[localIdx].machineUid.trimmed().toLower();
    if (localUid.isEmpty() && localIdx < m_states.size()) {
        localUid = m_states[localIdx].machineUuid.trimmed().toLower();
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (i == localIdx || isLocalConnection(i)) {
            continue;
        }
        const ConnectionProfile candidate = m_profiles[i];
        if (candidate.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString candUid = candidate.machineUid.trimmed().toLower();
        if (!localUid.isEmpty() && !candUid.isEmpty() && candUid == localUid) {
            return i;
        }
        if (isConnectionRedirectedToLocal(i)) {
            return i;
        }
    }
    return -1;
}

void MainWindow::removeDuplicateMachineConnections(int keepIdx) {
    if (keepIdx < 0 || keepIdx >= m_profiles.size()) {
        return;
    }
    QString keepUid = m_profiles[keepIdx].machineUid.trimmed().toLower();
    if (keepUid.isEmpty() && keepIdx < m_states.size()) {
        keepUid = m_states[keepIdx].machineUuid.trimmed().toLower();
    }
    if (keepUid.isEmpty()) {
        return;
    }
    const QString keepId = m_profiles[keepIdx].id.trimmed();
    const bool keepIsLocal = isLocalConnection(keepIdx);
    QStringList toRemove;
    QStringList toRemoveNames;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (i == keepIdx) {
            continue;
        }
        // Never delete Local; but if keepIdx IS Local, SSH duplicates are fair game
        if (isLocalConnection(i) && !keepIsLocal) {
            continue;
        }
        QString uid = m_profiles[i].machineUid.trimmed().toLower();
        if (uid.isEmpty() && i < m_states.size()) {
            uid = m_states[i].machineUuid.trimmed().toLower();
        }
        if (!uid.isEmpty() && uid == keepUid && m_profiles[i].id.trimmed() != keepId) {
            toRemove << m_profiles[i].id.trimmed();
            toRemoveNames << m_profiles[i].name.trimmed();
        }
    }
    if (toRemove.isEmpty()) {
        return;
    }
    for (int k = 0; k < toRemove.size(); ++k) {
        QString err;
        if (m_store.deleteConnectionById(toRemove[k], err)) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Deduplicación: eliminada conexión '%1' (misma máquina que '%2')")
                       .arg(toRemoveNames[k], m_profiles[keepIdx].name.trimmed()));
        } else {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Deduplicación: no se pudo eliminar '%1': %2")
                       .arg(toRemoveNames[k], err));
        }
    }
    loadConnections();
}

bool MainWindow::canSshBetweenConnections(int rowIdx, int colIdx, QString* errorOut, int* effectiveDstIdxOut) {
    if (effectiveDstIdxOut) {
        *effectiveDstIdxOut = -1;
    }
    auto fail = [errorOut](const QString& msg) {
        if (errorOut) {
            *errorOut = msg;
        }
        return false;
    };
    if (rowIdx < 0 || rowIdx >= m_profiles.size() || colIdx < 0 || colIdx >= m_profiles.size()) {
        return fail(QStringLiteral("indices inválidos"));
    }
    const bool srcOk = rowIdx < m_states.size() && m_states[rowIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
    const bool dstOk = colIdx < m_states.size() && m_states[colIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
    if (!srcOk || !dstOk) {
        return fail(trk(QStringLiteral("t_connectivity_notready_001"),
                        QStringLiteral("La conexión origen o destino no está en estado OK."),
                        QStringLiteral("The source or target connection is not in OK state."),
                        QStringLiteral("源连接或目标连接不是 OK 状态。")));
    }
    int effectiveIdx = colIdx;
    if (isLocalConnection(colIdx)) {
        const int sshIdx = equivalentSshForLocal(colIdx);
        if (sshIdx < 0) {
            return fail(trk(QStringLiteral("t_connectivity_local_no_ssh_001"),
                            QStringLiteral("Local no tiene una conexión SSH equivalente para comprobarla remotamente."),
                            QStringLiteral("Local has no equivalent SSH connection for remote probing."),
                            QStringLiteral("本地连接没有可用于远程探测的等效 SSH 连接。")));
        }
        effectiveIdx = sshIdx;
    }
    if (effectiveDstIdxOut) {
        *effectiveDstIdxOut = effectiveIdx;
    }
    if (rowIdx == colIdx || rowIdx == effectiveIdx || connectionsReferToSameMachine(rowIdx, colIdx) || connectionsReferToSameMachine(rowIdx, effectiveIdx)) {
        if (errorOut) {
            errorOut->clear();
        }
        return true;
    }
    const ConnectionProfile src = m_profiles[rowIdx];
    const ConnectionProfile effectiveDst = m_profiles[effectiveIdx];
    if (effectiveDst.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        return fail(trk(QStringLiteral("t_connectivity_unsupported_target_001"),
                        QStringLiteral("Solo se comprueba conectividad SSH hacia conexiones SSH/Local."),
                        QStringLiteral("Only SSH connectivity to SSH/Local connections is checked."),
                        QStringLiteral("只检查到 SSH/本地连接的 SSH 连通性。")));
    }
    const QString sshCmd = connectivityMatrixRemoteProbe(effectiveDst);
    if (sshCmd.trimmed().isEmpty()) {
        return fail(QStringLiteral("probe SSH vacío"));
    }
    QString sshMerged;
    QString sshDetail;
    const bool sshOk = fetchConnectionProbeOutput(rowIdx,
                                                  QStringLiteral("Probe SSH"),
                                                  sshCmd,
                                                  &sshMerged,
                                                  &sshDetail,
                                                  12000);
    const bool sshProbeOk = sshOk && sshMerged.contains(QStringLiteral("ZFSMGR_CONNECT_OK"));
    if (!sshProbeOk) {
        return fail((sshDetail.isEmpty() ? sshMerged : sshDetail).left(300));
    }
    if (errorOut) {
        errorOut->clear();
    }
    return true;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);
    Q_UNUSED(event);
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::openConnectivityMatrixDialog() {
    if (m_profiles.isEmpty()) {
        QMessageBox::information(this,
                                 trk(QStringLiteral("t_connectivity_title_001"),
                                     QStringLiteral("Conectividad"),
                                     QStringLiteral("Connectivity"),
                                     QStringLiteral("连通性")),
                                 trk(QStringLiteral("t_connectivity_empty_001"),
                                     QStringLiteral("No hay conexiones definidas."),
                                     QStringLiteral("There are no defined connections."),
                                     QStringLiteral("没有已定义的连接。")));
        return;
    }

    auto sameMachine = [this](int a, int b) -> bool {
        if (a < 0 || a >= m_profiles.size() || b < 0 || b >= m_profiles.size()) {
            return false;
        }
        const QString ua = m_profiles[a].machineUid.trimmed().toLower();
        const QString ub = m_profiles[b].machineUid.trimmed().toLower();
        return !ua.isEmpty() && !ub.isEmpty() && ua == ub;
    };
    auto equivalentSshForLocal = [this](int localIdx) -> int {
        if (localIdx < 0 || localIdx >= m_profiles.size() || !isLocalConnection(localIdx)) {
            return -1;
        }
        QString localUid = m_profiles[localIdx].machineUid.trimmed().toLower();
        if (localUid.isEmpty() && localIdx < m_states.size()) {
            localUid = m_states[localIdx].machineUuid.trimmed().toLower();
        }
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (i == localIdx || isLocalConnection(i)) {
                continue;
            }
            const ConnectionProfile candidate = m_profiles[i];
            if (candidate.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString candUid = candidate.machineUid.trimmed().toLower();
            if (!localUid.isEmpty() && !candUid.isEmpty() && candUid == localUid) {
                return i;
            }
            if (isConnectionRedirectedToLocal(i)) {
                return i;
            }
        }
        return -1;
    };
    auto probeConnectivity = [&](int rowIdx, int colIdx) -> ConnectivityProbeResult {
        ConnectivityProbeResult result;
        auto composeText = [](const QString& sshState, const QString& rsyncState) -> QString {
            return QStringLiteral("SSH:%1\nrsync:%2").arg(sshState, rsyncState);
        };
        auto explainFailure = [this](const QString& raw, int rc) -> QString {
            const QString merged = raw.trimmed();
            if (merged.contains(QStringLiteral("sshpass no disponible"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_sshpass_001"),
                           QStringLiteral("Motivo: en la conexión origen no está instalado sshpass y el destino requiere autenticación por contraseña."),
                           QStringLiteral("Reason: sshpass is not installed on the source connection and the target requires password authentication."),
                           QStringLiteral("原因：源连接未安装 sshpass，而目标需要密码认证。"));
            }
            if (merged.contains(QStringLiteral("rsync no disponible"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("ZFSMGR_RSYNC_MISSING"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_rsync_001"),
                           QStringLiteral("Motivo: rsync no está disponible en origen o destino."),
                           QStringLiteral("Reason: rsync is not available on source or target."),
                           QStringLiteral("原因：源端或目标端不可用 rsync。"));
            }
            if (merged.contains(QStringLiteral("permission denied"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("publickey"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("authentication"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_auth_001"),
                           QStringLiteral("Motivo: fallo de autenticación SSH hacia el destino."),
                           QStringLiteral("Reason: SSH authentication to the target failed."),
                           QStringLiteral("原因：到目标的 SSH 认证失败。"));
            }
            if (merged.contains(QStringLiteral("could not resolve hostname"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("name or service not known"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_dns_001"),
                           QStringLiteral("Motivo: no se puede resolver el nombre del host destino."),
                           QStringLiteral("Reason: the target hostname cannot be resolved."),
                           QStringLiteral("原因：无法解析目标主机名。"));
            }
            if (merged.contains(QStringLiteral("connection refused"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_refused_001"),
                           QStringLiteral("Motivo: el puerto SSH destino rechaza la conexión."),
                           QStringLiteral("Reason: the target SSH port refused the connection."),
                           QStringLiteral("原因：目标 SSH 端口拒绝了连接。"));
            }
            if (merged.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("operation timed out"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_timeout_001"),
                           QStringLiteral("Motivo: tiempo de espera agotado al conectar con el destino."),
                           QStringLiteral("Reason: timed out while connecting to the target."),
                           QStringLiteral("原因：连接目标时超时。"));
            }
            if (!merged.isEmpty()) {
                return trk(QStringLiteral("t_connectivity_reason_raw_001"),
                           QStringLiteral("Motivo: %1"),
                           QStringLiteral("Reason: %1"),
                           QStringLiteral("原因：%1"))
                    .arg(merged.left(300));
            }
            return trk(QStringLiteral("t_connectivity_reason_exit_001"),
                       QStringLiteral("Motivo: la comprobación terminó con código %1."),
                       QStringLiteral("Reason: the probe finished with exit code %1."),
                       QStringLiteral("原因：探测以退出码 %1 结束。"))
                .arg(rc);
        };
        if (rowIdx < 0 || rowIdx >= m_profiles.size() || colIdx < 0 || colIdx >= m_profiles.size()) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            return result;
        }
        const ConnectionProfile src = m_profiles[rowIdx];
        const ConnectionProfile dst = m_profiles[colIdx];
        const bool srcOk = rowIdx < m_states.size() && m_states[rowIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
        const bool dstOk = colIdx < m_states.size() && m_states[colIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
        if (!srcOk || !dstOk) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            result.tooltip = trk(QStringLiteral("t_connectivity_notready_001"),
                                 QStringLiteral("La conexión origen o destino no está en estado OK."),
                                 QStringLiteral("The source or target connection is not in OK state."),
                                 QStringLiteral("源连接或目标连接不是 OK 状态。"));
            result.detail = result.tooltip;
            return result;
        }
        if (rowIdx == colIdx || sameMachine(rowIdx, colIdx)) {
            result.text = composeText(QStringLiteral("✓"), QStringLiteral("✓"));
            result.tooltip = trk(QStringLiteral("t_connectivity_same_machine_001"),
                                 QStringLiteral("Misma máquina."),
                                 QStringLiteral("Same machine."),
                                 QStringLiteral("同一台机器。"));
            result.detail = result.tooltip;
            result.ok = true;
            return result;
        }
        ConnectionProfile effectiveDst = dst;
        QString targetLabel = dst.name;
        if (dst.connType.trimmed().compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0) {
            const int sshIdx = equivalentSshForLocal(colIdx);
            if (sshIdx >= 0) {
                effectiveDst = m_profiles[sshIdx];
                targetLabel = m_profiles[sshIdx].name;
                if (rowIdx == sshIdx || sameMachine(rowIdx, sshIdx)) {
                    result.text = composeText(QStringLiteral("✓"), QStringLiteral("✓"));
                    result.tooltip = trk(QStringLiteral("t_connectivity_same_machine_001"),
                                         QStringLiteral("Misma máquina."),
                                         QStringLiteral("Same machine."),
                                         QStringLiteral("同一台机器。"));
                    result.detail = result.tooltip;
                    result.ok = true;
                    return result;
                }
            } else {
                result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
                result.tooltip = trk(QStringLiteral("t_connectivity_local_no_ssh_001"),
                                     QStringLiteral("Local no tiene una conexión SSH equivalente para comprobarla remotamente."),
                                     QStringLiteral("Local has no equivalent SSH connection for remote probing."),
                                     QStringLiteral("本地连接没有可用于远程探测的等效 SSH 连接。"));
                result.detail = result.tooltip;
                return result;
            }
        }
        if (effectiveDst.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            result.tooltip = trk(QStringLiteral("t_connectivity_unsupported_target_001"),
                                 QStringLiteral("Solo se comprueba conectividad SSH hacia conexiones SSH/Local."),
                                 QStringLiteral("Only SSH connectivity to SSH/Local connections is checked."),
                                 QStringLiteral("只检查到 SSH/本地连接的 SSH 连通性。"));
            result.detail = result.tooltip;
            return result;
        }
        const QString sshCmd = connectivityMatrixRemoteProbe(effectiveDst);
        if (sshCmd.trimmed().isEmpty()) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            return result;
        }
        QString sshMerged;
        QString sshDetail;
        const bool sshOk = fetchConnectionProbeOutput(rowIdx,
                                                      QStringLiteral("Probe SSH"),
                                                      sshCmd,
                                                      &sshMerged,
                                                      &sshDetail,
                                                      12000);
        const bool sshProbeOk = sshOk && sshMerged.contains(QStringLiteral("ZFSMGR_CONNECT_OK"));

        QString rsyncState = QStringLiteral("-");
        QStringList tooltipLines;
        if (sshProbeOk) {
            tooltipLines << trk(QStringLiteral("t_connectivity_ok_001"),
                                QStringLiteral("Conectividad SSH verificada hacia %1."),
                                QStringLiteral("SSH connectivity verified to %1."),
                                QStringLiteral("到 %1 的 SSH 连通性已验证。"))
                                .arg(targetLabel);
            const QString rsyncCmd = connectivityMatrixRsyncProbe(effectiveDst);
            if (!rsyncCmd.trimmed().isEmpty()) {
                QString rsyncMerged;
                QString rsyncDetail;
                const bool rsyncOk = fetchConnectionProbeOutput(rowIdx,
                                                                QStringLiteral("Probe rsync"),
                                                                rsyncCmd,
                                                                &rsyncMerged,
                                                                &rsyncDetail,
                                                                12000);
                if (rsyncOk && rsyncMerged.contains(QStringLiteral("ZFSMGR_RSYNC_OK"))) {
                    rsyncState = QStringLiteral("✓");
                    tooltipLines << trk(QStringLiteral("t_connectivity_rsync_ok_001"),
                                        QStringLiteral("rsync disponible en origen y destino."),
                                        QStringLiteral("rsync available on source and target."),
                                        QStringLiteral("源端和目标端均可用 rsync。"));
                } else {
                    rsyncState = QStringLiteral("✗");
                    tooltipLines << explainFailure(rsyncDetail.isEmpty() ? rsyncMerged : rsyncDetail, rsyncOk ? 0 : -1);
                }
            }
            result.text = composeText(QStringLiteral("✓"), rsyncState);
            result.tooltip = tooltipLines.join(QStringLiteral("\n"));
            result.detail = result.tooltip;
            result.ok = true;
            return result;
        }
        result.text = composeText(QStringLiteral("✗"), QStringLiteral("-"));
        result.tooltip = explainFailure(sshDetail.isEmpty() ? sshMerged : sshDetail, sshOk ? 0 : -1);
        QString sshVerboseMerged;
        QString sshVerboseDetail;
        const QString sshVerboseCmd = connectivityMatrixRemoteProbe(effectiveDst, true);
        if (!sshVerboseCmd.trimmed().isEmpty()) {
            fetchConnectionProbeOutput(rowIdx,
                                       QStringLiteral("Probe SSH verbose"),
                                       sshVerboseCmd,
                                       &sshVerboseMerged,
                                       &sshVerboseDetail,
                                       12000);
        }
        result.detail = QStringList{
                            result.tooltip,
                            QString(),
                            trk(QStringLiteral("t_connectivity_probe_log_001"),
                                QStringLiteral("Log del probe:"),
                                QStringLiteral("Probe log:"),
                                QStringLiteral("探测日志：")),
                            (sshDetail.isEmpty() ? sshMerged : sshDetail).trimmed(),
                            QString(),
                            QStringLiteral("ssh -vvv:"),
                            (sshVerboseDetail.isEmpty() ? sshVerboseMerged : sshVerboseDetail).trimmed()
                        }.join(QStringLiteral("\n"));
        return result;
    };

    QDialog dlg(this);
    dlg.setWindowTitle(trk(QStringLiteral("t_connectivity_title_001"),
                           QStringLiteral("Conectividad"),
                           QStringLiteral("Connectivity"),
                           QStringLiteral("连通性")));
    dlg.resize(760, 460);
    auto* layout = new QVBoxLayout(&dlg);
    auto* matrix = new QTableWidget(&dlg);
    matrix->setColumnCount(m_profiles.size());
    matrix->setRowCount(m_profiles.size());
    QStringList labels;
    for (const ConnectionProfile& p : m_profiles) {
        labels << (p.name.trimmed().isEmpty() ? p.id : p.name);
    }
    matrix->setHorizontalHeaderLabels(labels);
    matrix->setVerticalHeaderLabels(labels);
    matrix->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    matrix->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    matrix->setEditTriggers(QAbstractItemView::NoEditTriggers);
    matrix->setSelectionMode(QAbstractItemView::SingleSelection);
    matrix->setSelectionBehavior(QAbstractItemView::SelectItems);
    matrix->setAlternatingRowColors(false);
    auto* detailLabel = new QLabel(
        trk(QStringLiteral("t_connectivity_detail_title_001"),
            QStringLiteral("Detalle de la casilla seleccionada"),
            QStringLiteral("Selected cell detail"),
            QStringLiteral("所选单元格详情")),
        &dlg);
    auto* detailView = new QPlainTextEdit(&dlg);
    detailView->setReadOnly(true);
    detailView->setMaximumBlockCount(2000);
    detailView->setPlaceholderText(
        trk(QStringLiteral("t_connectivity_detail_ph_001"),
            QStringLiteral("Seleccione una celda para ver su detalle."),
            QStringLiteral("Select a cell to view its detail."),
            QStringLiteral("选择一个单元格以查看详情。")));
    beginUiBusy();
    m_connectivityMatrixInProgress = true;
    updateConnectivityMatrixButtonState();
    for (int r = 0; r < m_profiles.size(); ++r) {
        for (int c = 0; c < m_profiles.size(); ++c) {
            const ConnectivityProbeResult probe = probeConnectivity(r, c);
            auto* item = new QTableWidgetItem(probe.text);
            item->setTextAlignment(Qt::AlignCenter);
            item->setToolTip(probe.tooltip);
            item->setData(Qt::UserRole, probe.detail);
            if (probe.ok) {
                item->setForeground(QBrush(QColor(QStringLiteral("#1d6f42"))));
            } else if (probe.text.contains(QStringLiteral("✗"))) {
                item->setForeground(QBrush(QColor(QStringLiteral("#8b1e1e"))));
                item->setBackground(QBrush(QColor(QStringLiteral("#fde7e7"))));
            }
            matrix->setItem(r, c, item);
            qApp->processEvents();
        }
    }
    m_connectivityMatrixInProgress = false;
    updateConnectivityMatrixButtonState();
    endUiBusy();
    layout->addWidget(matrix, 1);
    layout->addWidget(detailLabel);
    layout->addWidget(detailView);
    connect(matrix, &QTableWidget::currentItemChanged, &dlg, [detailView](QTableWidgetItem* current, QTableWidgetItem*) {
        if (!detailView) {
            return;
        }
        detailView->setPlainText(current ? current->data(Qt::UserRole).toString() : QString());
    });
    if (matrix->rowCount() > 0 && matrix->columnCount() > 0) {
        matrix->setCurrentCell(0, 0);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::showConnectionContextMenu(int connIdx, const QPoint& globalPos, QTreeWidget* sourceTree) {
    const auto endBusy = [this]() { endUiBusy(); };
    const bool hasConn = (connIdx >= 0 && connIdx < m_profiles.size());
    if (hasConn) {
        setCurrentConnectionInUi(connIdx);
    }
    const bool isDisconnected = hasConn && isConnectionDisconnected(connIdx);
    const zfsmgr::uilogic::ConnectionContextMenuState menuState =
        zfsmgr::uilogic::buildConnectionContextMenuState(
            hasConn,
            isDisconnected,
            actionsLocked(),
            hasConn && isLocalConnection(connIdx),
            hasConn && isConnectionRedirectedToLocal(connIdx),
            hasConn && isWindowsConnection(connIdx));

    QMenu menu(this);
    QAction* aConnect = menu.addAction(
        trk(QStringLiteral("t_connect_ctx_001"),
            QStringLiteral("Conectar"),
            QStringLiteral("Connect"),
            QStringLiteral("连接")));
    QAction* aDisconnect = menu.addAction(
        trk(QStringLiteral("t_disconnect_ctx001"),
            QStringLiteral("Desconectar"),
            QStringLiteral("Disconnect"),
            QStringLiteral("断开连接")));
    QAction* aRefresh = menu.addAction(
        trk(QStringLiteral("t_refresh_conn_ctx001"),
            QStringLiteral("Refrescar"),
            QStringLiteral("Refresh"),
            QStringLiteral("刷新")));
    menu.addSeparator();
    QAction* aNewConn = menu.addAction(
        trk(QStringLiteral("t_new_conn_ctx001"),
            QStringLiteral("Nueva Conexión"),
            QStringLiteral("New Connection"),
            QStringLiteral("新建连接")));
    QAction* aEdit = menu.addAction(
        trk(QStringLiteral("t_edit_conn_ctx001"),
            QStringLiteral("Editar"),
            QStringLiteral("Edit"),
            QStringLiteral("编辑")));
    QAction* aDelete = menu.addAction(
        trk(QStringLiteral("t_del_conn_ctx001"),
            QStringLiteral("Borrar"),
            QStringLiteral("Delete"),
            QStringLiteral("删除")));
    menu.addSeparator();
    QAction* aNewPool = menu.addAction(
        trk(QStringLiteral("t_new_pool_ctx_001"),
            QStringLiteral("Nuevo Pool"),
            QStringLiteral("New Pool"),
            QStringLiteral("新建存储池")));
    menu.addSeparator();
    QAction* aInstallHelpers = menu.addAction(
        trk(QStringLiteral("t_install_helpers_ctx001"),
            QStringLiteral("Instalar comandos auxiliares"),
            QStringLiteral("Install helper commands"),
            QStringLiteral("安装辅助命令")));
    QAction* aInstallDaemon = menu.addAction(
        trk(QStringLiteral("t_install_daemon_ctx001"),
            QStringLiteral("Reinstalar/Actualizar daemon"),
            QStringLiteral("Reinstall/Update daemon"),
            QStringLiteral("重新安装/更新守护进程")));
    QAction* aRepairAltMountpoints = menu.addAction(
        trk(QStringLiteral("t_repair_altmp_ctx001"),
            QStringLiteral("Reparar mountpoints temporales"),
            QStringLiteral("Repair temporary mountpoints"),
            QStringLiteral("修复临时挂载点")));
    QAction* aExportTrustStore = menu.addAction(
        trk(QStringLiteral("t_export_trust_store_ctx001"),
            QStringLiteral("Exportar trust-store a esta conexión"),
            QStringLiteral("Export trust-store to this connection"),
            QStringLiteral("将 trust-store 导出到此连接")));
    const bool isThisSshConn = hasConn && !isWindowsConnection(connIdx)
                               && m_profiles[connIdx].connType.compare(
                                      QStringLiteral("SSH"), Qt::CaseInsensitive) == 0;
    QMenu* aAuthorizeKeyMenu = menu.addMenu(
        trk(QStringLiteral("t_authorize_key_menu_001"),
            QStringLiteral("Autorizar clave SSH en..."),
            QStringLiteral("Authorize SSH key on..."),
            QStringLiteral("授权 SSH 密钥到...")));
    QList<QPair<int, QAction*>> authorizeKeyActions;
    if (isThisSshConn && !isDisconnected && !actionsLocked()) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (i == connIdx) {
                continue;
            }
            if (m_profiles[i].connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (isConnectionDisconnected(i)) {
                continue;
            }
            QAction* a = aAuthorizeKeyMenu->addAction(m_profiles[i].name);
            authorizeKeyActions.append({i, a});
        }
    }
    aAuthorizeKeyMenu->setEnabled(isThisSshConn && !isDisconnected && !actionsLocked()
                                  && !authorizeKeyActions.isEmpty());
    menu.addSeparator();

    aConnect->setEnabled(menuState.canConnect);
    aDisconnect->setEnabled(menuState.canDisconnect);
    const bool canInstallHelpers =
        hasConn && !actionsLocked() && !isDisconnected
        && connIdx < m_states.size()
        && m_states[connIdx].helperInstallSupported;
    aInstallHelpers->setEnabled(canInstallHelpers);
    const bool daemonInstallable =
        hasConn && !actionsLocked() && !isDisconnected && !isLocalConnection(connIdx);
    aInstallDaemon->setEnabled(daemonInstallable);
    // Also available on the local connection: a local dataset can be left stranded too.
    aRepairAltMountpoints->setEnabled(hasConn && !actionsLocked() && !isDisconnected
                                      && !isWindowsConnection(connIdx));
    aExportTrustStore->setEnabled(hasConn && !actionsLocked() && !isDisconnected && !isLocalConnection(connIdx));
    aRefresh->setEnabled(menuState.canRefreshThis);
    aEdit->setEnabled(menuState.canEditDelete);
    aDelete->setEnabled(menuState.canEditDelete);
    aNewConn->setEnabled(menuState.canNewConnection);
    aNewPool->setEnabled(menuState.canNewPool);

    menu.addSeparator();
    const bool isSplitTree = sourceTree && sourceTree->property("zfsmgr.isSplitTree").toBool();
    QAction* aCloseSplit = nullptr;
    QAction* aSplitRight = nullptr;
    QAction* aSplitLeft = nullptr;
    QAction* aSplitBelow = nullptr;
    QAction* aSplitAbove = nullptr;
    if (isSplitTree) {
        aCloseSplit = menu.addAction(QStringLiteral("Close"));
    } else {
        QMenu* splitMenu = menu.addMenu(QStringLiteral("Split and root"));
        aSplitRight = splitMenu->addAction(
            trk(QStringLiteral("t_split_right_001"),
                QStringLiteral("Derecha"),
                QStringLiteral("Right"),
                QStringLiteral("向右")));
        aSplitLeft = splitMenu->addAction(
            trk(QStringLiteral("t_split_left_001"),
                QStringLiteral("Izquierda"),
                QStringLiteral("Left"),
                QStringLiteral("向左")));
        aSplitBelow = splitMenu->addAction(
            trk(QStringLiteral("t_split_below_001"),
                QStringLiteral("Abajo"),
                QStringLiteral("Below"),
                QStringLiteral("向下")));
        aSplitAbove = splitMenu->addAction(
            trk(QStringLiteral("t_split_above_001"),
                QStringLiteral("Arriba"),
                QStringLiteral("Above"),
                QStringLiteral("向上")));
    }

    endBusy();
    QAction* chosen = menu.exec(globalPos);
    if (!chosen) {
        return;
    }
    if (chosen == aCloseSplit) {
        closeSplitTree(sourceTree);
        return;
    }
    if (chosen == aSplitRight || chosen == aSplitLeft || chosen == aSplitBelow || chosen == aSplitAbove) {
        const Qt::Orientation orient =
            (chosen == aSplitRight || chosen == aSplitLeft) ? Qt::Horizontal : Qt::Vertical;
        const bool insertBefore = (chosen == aSplitLeft || chosen == aSplitAbove);
        splitAndRootConnContent(orient, insertBefore, connIdx, QString{}, QString{}, sourceTree);
        return;
    }
    if (chosen == aConnect && hasConn) {
        logUiAction(QStringLiteral("Conectar conexión (menú conexiones)"));
        beginTransientUiBusy(
            trk(QStringLiteral("t_connecting_conn_busy_001"),
                QStringLiteral("Conectando %1..."),
                QStringLiteral("Connecting %1..."),
                QStringLiteral("正在连接 %1...")).arg(m_profiles[connIdx].name));
        setConnectionDisconnected(connIdx, false);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Conexión marcada como conectada: %1").arg(m_profiles[connIdx].name));
        rebuildConnectionsTable();
        populateAllPoolsTables();
        refreshConnectionByIndex(connIdx);
        endTransientUiBusy();
    } else if (chosen == aDisconnect && hasConn) {
        setConnectionDisconnected(connIdx, true);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Conexión marcada como desconectada: %1").arg(m_profiles[connIdx].name));
        rebuildConnectionsTable();
        populateAllPoolsTables();
    } else if (chosen == aRefresh) {
        logUiAction(QStringLiteral("Refrescar conexión (menú conexiones)"));
        refreshSelectedConnection();
    } else if (chosen == aEdit) {
        logUiAction(QStringLiteral("Editar conexión (menú conexiones)"));
        editConnection();
    } else if (chosen == aDelete) {
        logUiAction(QStringLiteral("Borrar conexión (menú conexiones)"));
        deleteConnection();
    } else if (chosen == aInstallHelpers) {
        logUiAction(QStringLiteral("Instalar comandos auxiliares (menú conexiones)"));
        installHelperCommandsForSelectedConnection();
    } else if (chosen == aInstallDaemon) {
        logUiAction(QStringLiteral("Reinstalar/Actualizar daemon (menú conexiones)"));
        if (connIdx >= 0 && connIdx < m_profiles.size()) {
            (void)installOrUpdateDaemonForConnectionInternal(connIdx, true);
        }
    } else if (chosen == aRepairAltMountpoints) {
        logUiAction(QStringLiteral("Reparar mountpoints temporales (menú conexiones)"));
        repairAltMountpointsForSelectedConnection();
    } else if (chosen == aExportTrustStore) {
        logUiAction(QStringLiteral("Exportar trust-store (menú conexiones)"));
        exportTrustStoreToSelectedConnection();
    } else if (chosen == aNewConn) {
        logUiAction(QStringLiteral("Nueva conexión (menú conexiones)"));
        createConnection();
    } else if (chosen == aNewPool) {
        logUiAction(QStringLiteral("Nuevo pool (menú conexiones)"));
        createPoolForSelectedConnection();
    } else {
        for (const auto& [dstIdx, a] : authorizeKeyActions) {
            if (chosen == a) {
                logUiAction(QStringLiteral("Autorizar clave SSH (menú conexiones)"));
                authorizePublicKeyOnConnection(connIdx, dstIdx);
                break;
            }
        }
    }
}

void MainWindow::syncConnectionDisplaySelectors() {
    // Árbol unificado: ya no hay selectores O/D en la tabla.
}

void MainWindow::applyConnectionDisplayMode(int connIdx, const QString& modeRaw) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const QString mode = modeRaw.trimmed().toLower();
    if (isConnectionDisconnected(connIdx)) {
        return;
    }

    if (mode != QStringLiteral("source") && mode != QStringLiteral("both")) {
        return;
    }
    const int prevTop = m_topDetailConnIdx;
    if (prevTop >= 0 && prevTop != connIdx) {
        saveTopTreeStateForConnection(prevTop);
    }
    m_topDetailConnIdx = connIdx;
    m_forceRestoreTopStateConnIdx = connIdx;
    m_userSelectedConnectionKey = m_profiles[connIdx].id.trimmed().toLower();
    if (m_userSelectedConnectionKey.isEmpty()) {
        m_userSelectedConnectionKey = m_profiles[connIdx].name.trimmed().toLower();
    }
    rebuildConnectionEntityTabs();
    refreshConnectionNodeDetails();
    updateConnectionActionsState();
}

void MainWindow::refreshAllConnections() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_acci_n_en__6facd2"),
                   QStringLiteral("Acción en curso: refresh bloqueado"),
                   QStringLiteral("Action in progress: refresh blocked"),
                   QStringLiteral("操作进行中：刷新被阻止")));
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_refrescar__7f8af2"),
               QStringLiteral("Refrescar todas las conexiones"),
               QStringLiteral("Refresh all connections"),
               QStringLiteral("刷新所有连接")));
    if (m_profiles.isEmpty()) {
        if (!m_initialRefreshCompleted) {
            m_initialRefreshCompleted = true;
        }
        m_refreshInProgress = false;
        updateBusyCursor();
        updateStatus(QString());
        updateConnectivityMatrixButtonState();
        rebuildConnectionsTable();
        populateAllPoolsTables();
        return;
    }
    const int generation = ++m_refreshGeneration;
    int refreshable = 0;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (!isConnectionDisconnected(i)) {
            ++refreshable;
        }
    }
    m_refreshPending = refreshable;
    m_refreshTotal = refreshable;
    m_refreshInProgress = (m_refreshPending > 0);
    updateBusyCursor();
    updateConnectivityMatrixButtonState();
    if (refreshable <= 0) {
        if (!m_initialRefreshCompleted) {
            m_initialRefreshCompleted = true;
        }
        rebuildConnectionsTable();
        populateAllPoolsTables();
        updateStatus(QString());
        return;
    }

    auto launchRefreshForIndex = [this, generation](int idx) {
        if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
            return;
        }
        const ConnectionProfile profile = m_profiles[idx];
        (void)QtConcurrent::run([this, generation, idx, profile]() {
            const ConnectionRuntimeState state = refreshConnection(profile);
            QMetaObject::invokeMethod(this, [this, generation, idx, state, profile]() {
                onAsyncRefreshResult(generation, idx, profile.id, state);
            }, Qt::QueuedConnection);
        });
    };

    QVector<int> launchOrder;
    auto appendUnique = [&launchOrder](int idx) {
        if (idx < 0 || launchOrder.contains(idx)) {
            return;
        }
        launchOrder.push_back(idx);
    };
    appendUnique(currentConnectionIndexFromUi());
    appendUnique(m_topDetailConnIdx);
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (!isConnectionDisconnected(i)) {
            appendUnique(i);
        }
    }

    for (int idx : std::as_const(launchOrder)) {
        if (isConnectionDisconnected(idx)) {
            continue;
        }
        launchRefreshForIndex(idx);
    }
}

void MainWindow::refreshSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_acci_n_en__6facd2"),
                   QStringLiteral("Acción en curso: refresh bloqueado"),
                   QStringLiteral("Action in progress: refresh blocked"),
                   QStringLiteral("操作进行中：刷新被阻止")));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = 1;
    m_refreshTotal = 1;
    m_refreshInProgress = true;
    updateBusyCursor();
    updateConnectivityMatrixButtonState();
    const ConnectionProfile profile = m_profiles[idx];
    (void)QtConcurrent::run([this, generation, idx, profile]() {
        const ConnectionRuntimeState state = refreshConnection(profile);
        QMetaObject::invokeMethod(this, [this, generation, idx, state, profile]() {
            onAsyncRefreshResult(generation, idx, profile.id, state);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::stopAllDaemonEventWatchers() {}

void MainWindow::onAsyncRefreshResult(int generation, int idx, const QString& connId, const ConnectionRuntimeState& state) {
    if (generation != m_refreshGeneration) {
        return;
    }
    int targetIdx = -1;
    if (idx >= 0 && idx < m_profiles.size() && m_profiles[idx].id == connId) {
        targetIdx = idx;
    } else if (!connId.trimmed().isEmpty()) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].id == connId) {
                targetIdx = i;
                break;
            }
        }
    }
    if (targetIdx < 0 || targetIdx >= m_states.size()) {
        if (m_refreshPending > 0) {
            --m_refreshPending;
        }
        if (m_refreshPending == 0) {
            onAsyncRefreshDone(generation);
        }
        return;
    }
    const int selectedIdx = currentConnectionIndexFromUi();
    if (state.status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0
        && !state.machineUuid.trimmed().isEmpty()
        && (isLocalConnection(targetIdx)
            || m_profiles[targetIdx].connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0)) {
        const QString newMachineUid = state.machineUuid.trimmed();
        if (m_profiles[targetIdx].machineUid.trimmed().compare(newMachineUid, Qt::CaseInsensitive) != 0) {
            ConnectionProfile persisted = m_profiles[targetIdx];
            persisted.machineUid = newMachineUid;
            QString persistErr;
            if (m_store.upsertConnection(persisted, persistErr)) {
                m_profiles[targetIdx].machineUid = newMachineUid;
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("machine_uid persistido para %1: %2")
                           .arg(m_profiles[targetIdx].name,
                                newMachineUid));
                const int dedupIdx = targetIdx;
                QTimer::singleShot(0, this, [this, dedupIdx]() {
                    removeDuplicateMachineConnections(dedupIdx);
                });
            } else {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("No se pudo persistir machine_uid para %1: %2")
                           .arg(m_profiles[targetIdx].name,
                                persistErr.simplified()));
            }
        }
    }
    // ZED detection: compare previous daemonLastSeenZedEvent (from persisted m_states)
    // against the new value returned by refreshConnection(). The "@" sentinel means
    // "never polled" — once a first poll establishes any baseline (even empty),
    // subsequent ZED timestamps trigger an immediate follow-up refresh.
    {
        const QString prevZed = m_states[targetIdx].daemonLastSeenZedEvent;
        const QString newZed = state.daemonLastSeenZedEvent;
        if (!newZed.isEmpty()
            && newZed != prevZed
            && prevZed != QStringLiteral("@")) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("%1: ZED event detectado (ts=%2), programando refresh")
                       .arg(m_profiles[targetIdx].name, newZed));
            const QString profileId = m_profiles[targetIdx].id;
            QTimer::singleShot(0, this, [this, profileId]() {
                for (int i = 0; i < m_profiles.size(); ++i) {
                    if (m_profiles[i].id == profileId) {
                        refreshConnectionByIndex(i);
                        refreshConnectionDaemonLogAsync(i);
                        break;
                    }
                }
            });
        }
    }
    m_states[targetIdx] = state;
    {
        const QString connKey = m_profiles[targetIdx].id.trimmed().isEmpty()
                                    ? m_profiles[targetIdx].name.trimmed().toLower()
                                    : m_profiles[targetIdx].id.trimmed().toLower();
        if (state.daemonInstalled && !state.daemonNeedsAttention) {
            m_daemonBootstrapPromptedConnIds.remove(connKey);
        } else if (m_refreshTotal <= 1
                   && !connKey.isEmpty()
                   && state.status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0
                   && !isConnectionDisconnected(targetIdx)
                   && !m_daemonBootstrapPromptedConnIds.contains(connKey)) {
            m_daemonBootstrapPromptedConnIds.insert(connKey);
            QTimer::singleShot(0, this, [this, targetIdx]() {
                if (targetIdx < 0 || targetIdx >= m_profiles.size()) {
                    return;
                }
                if (!installOrUpdateDaemonForConnectionInternal(targetIdx, false)) {
                    setConnectionDisconnected(targetIdx, true);
                }
            });
        }
    }
    {
        const QString accountKey = connectionAccountCacheKey(targetIdx);
        if (!accountKey.isEmpty()) {
            m_connSystemUsersCacheByKey.remove(accountKey);
            m_connSystemGroupsCacheByKey.remove(accountKey);
            m_connSystemUsersLoadedKeys.remove(accountKey);
            m_connSystemGroupsLoadedKeys.remove(accountKey);
        }
    }
    invalidatePoolDetailsCacheForConnection(targetIdx);
    invalidatePoolAutoSnapshotInfoForConnection(targetIdx);
    cachePoolStatusTextsForConnection(targetIdx, state);
    rebuildConnInfoFor(targetIdx);
    preloadPoolAutoSnapshotInfoForConnection(targetIdx);
    // Scan for orphaned running jobs when the connection first becomes active with job support
    if (state.daemonActive && state.daemonJobsSupported
        && !m_states[targetIdx].daemonActive) {
        QTimer::singleShot(0, this, [this, targetIdx]() {
            scanOrphanedJobsForConnection(targetIdx);
        });
    }
    const bool bulkRefresh = (m_refreshTotal > 1);
    if (!bulkRefresh) {
        rebuildConnectionsTable();
        if (selectedIdx >= 0) {
            setCurrentConnectionInUi(selectedIdx);
        }
        populateAllPoolsTables();
    }
    if (m_refreshPending > 0) {
        --m_refreshPending;
    }
    if (m_refreshPending == 0) {
        onAsyncRefreshDone(generation);
    }
}

void MainWindow::pollDaemonZedAllConnections() {
    if (m_zedPollPending > 0) return;

    QVector<int> toCheck;
    for (int i = 0; i < m_profiles.size() && i < m_states.size(); ++i) {
        if (m_states[i].daemonActive
            && !isLocalConnection(i)
            && !isWindowsConnection(m_profiles[i])
            && m_profiles[i].connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0) {
            toCheck.append(i);
        }
    }

    if (toCheck.isEmpty()) {
        if (!m_closing && m_autoRefreshTimer) {
            m_autoRefreshTimer->start(10000);
        }
        return;
    }

    m_zedPollPending = toCheck.size();

    for (int idx : std::as_const(toCheck)) {
        const ConnectionProfile profile = m_profiles[idx];
        const QString healthCmd = mwhelpers::agentShellCommand(profile, {QStringLiteral("--health")});
        const QString connId = profile.id;

        (void)QtConcurrent::run([this, idx, connId, profile, healthCmd]() {
            QString hout, herr;
            int hrc = -1;
            const bool ok = runSsh(profile, healthCmd, 8000, hout, herr, hrc) && hrc == 0;
            QString newZed;
            bool statusOk = false;
            if (ok) {
                const QString combined = hout + QStringLiteral("\n") + herr;
                for (const QString& line : combined.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                    if (line.startsWith(QStringLiteral("ZED_LAST_EVENT_UTC="))) {
                        newZed = line.mid(19).trimmed();
                    } else if (line.startsWith(QStringLiteral("STATUS=OK"))) {
                        statusOk = true;
                    }
                }
            }
            const bool healthFailed = !ok || !statusOk;

            QMetaObject::invokeMethod(this, [this, idx, connId, newZed, healthFailed]() {
                int targetIdx = -1;
                if (idx >= 0 && idx < m_profiles.size() && m_profiles[idx].id == connId) {
                    targetIdx = idx;
                } else {
                    for (int i = 0; i < m_profiles.size(); ++i) {
                        if (m_profiles[i].id == connId) { targetIdx = i; break; }
                    }
                }

                if (targetIdx >= 0 && targetIdx < m_states.size()) {
                    if (healthFailed) {
                        appLog(QStringLiteral("INFO"),
                               QStringLiteral("%1: ZED poll: health falló, forzando refresh")
                                   .arg(m_profiles[targetIdx].name));
                        refreshConnectionByIndex(targetIdx);
                    } else {
                        const QString prevZed = m_states[targetIdx].daemonLastSeenZedEvent;
                        if (prevZed == QStringLiteral("@")) {
                            m_states[targetIdx].daemonLastSeenZedEvent = newZed;
                        } else if (!newZed.isEmpty() && newZed != prevZed) {
                            appLog(QStringLiteral("INFO"),
                                   QStringLiteral("%1: ZED poll detectó evento (ts=%2)")
                                       .arg(m_profiles[targetIdx].name, newZed));
                            m_states[targetIdx].daemonLastSeenZedEvent = newZed;
                            refreshConnectionByIndex(targetIdx);
                            refreshConnectionDaemonLogAsync(targetIdx);
                        }
                    }
                }

                if (--m_zedPollPending <= 0) {
                    m_zedPollPending = 0;
                    if (!m_closing && m_autoRefreshTimer && !m_refreshInProgress) {
                        m_autoRefreshTimer->start(10000);
                    }
                }
            }, Qt::QueuedConnection);
        });
    }
}

void MainWindow::onAsyncRefreshDone(int generation) {
    if (generation != m_refreshGeneration) {
        return;
    }
    if (!m_initialRefreshCompleted) {
        m_initialRefreshCompleted = true;
    }
    const int selectedIdx = currentConnectionIndexFromUi();
    rebuildConnectionsTable();
    if (selectedIdx >= 0) {
        setCurrentConnectionInUi(selectedIdx);
    }
    populateAllPoolsTables();

    if (currentConnectionIndexFromUi() < 0) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (!isConnectionDisconnected(i)) {
                setCurrentConnectionInUi(i);
                break;
            }
        }
    }
    refreshConnectionNodeDetails();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refresco paralelo finalizado"));

    // Daemon: auto-actualizar cuando haya desalineación de versión/API.
    // No reinstalar si la única razón es un backoff TLS: reinstalar en ese
    // caso regenera las claves TLS y perpetúa el bucle fallo→reinstall→fallo.
    for (int i = 0; i < m_profiles.size() && i < m_states.size(); ++i) {
        if (isConnectionDisconnected(i)) {
            continue;
        }
        const ConnectionRuntimeState& st = m_states[i];
        if (!st.daemonInstalled || !st.daemonNeedsAttention) {
            continue;
        }
        const bool hasVersionOrApiReason = std::any_of(
            st.daemonAttentionReasons.begin(), st.daemonAttentionReasons.end(),
            [](const QString& r) {
                return r.contains(QStringLiteral("versión")) || r.contains(QStringLiteral("API"));
            });
        if (!hasVersionOrApiReason) {
            continue;
        }
        const QString connName = m_profiles[i].name.trimmed().isEmpty()
                                     ? m_profiles[i].id.trimmed()
                                     : m_profiles[i].name.trimmed();
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Daemon requiere actualización en \"%1\": actualización automática").arg(connName));
        (void)installOrUpdateDaemonForConnectionInternal(i, false);
    }

    m_refreshInProgress = false;
    updateBusyCursor();
    updateStatus(QString());
    updateConnectivityMatrixButtonState();
    if (m_busyOnImportRefresh) {
        m_busyOnImportRefresh = false;
        endUiBusy();
    }

    // Schedule periodic auto-refresh for SSH daemon connections so the health
    // poll can detect ZED events (daemon zedThread updates zedLastEventUtc;
    // health poll sees the change and schedules an immediate follow-up refresh).
    bool hasDaemonSshConn = false;
    for (int i = 0; i < m_profiles.size() && i < m_states.size(); ++i) {
        if (m_states[i].daemonActive
            && !isLocalConnection(i)
            && m_profiles[i].connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0) {
            hasDaemonSshConn = true;
            break;
        }
    }
    if (hasDaemonSshConn) {
        if (!m_autoRefreshTimer) {
            m_autoRefreshTimer = new QTimer(this);
            m_autoRefreshTimer->setSingleShot(true);
            connect(m_autoRefreshTimer, &QTimer::timeout, this, [this]() {
                if (m_closing) return;
                if (m_refreshInProgress || m_actionsLocked) {
                    m_autoRefreshTimer->start(2000);
                    return;
                }
                pollDaemonZedAllConnections();
            });
        }
        m_autoRefreshTimer->start(10000);
    } else if (m_autoRefreshTimer) {
        m_autoRefreshTimer->stop();
    }
}

void MainWindow::onConnectionSelectionChanged() {
    if (m_connContentTree
        && m_connContentTree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()) {
        updatePoolManagementBoxTitle();
        return;
    }
    QWidget* paintRoot = m_poolDetailTabs ? m_poolDetailTabs : static_cast<QWidget*>(m_rightTabs);
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(false);
    }

    QString selectionKey;
    int idx = m_topDetailConnIdx;
    if (idx < 0) {
        idx = currentConnectionIndexFromUi();
    }
    if (idx >= 0 && isConnectionDisconnected(idx)) {
        idx = -1;
    }
    selectionKey = QStringLiteral("%1").arg(idx);
    if (!selectionKey.isEmpty() && selectionKey == m_lastConnectionSelectionKey) {
        // Evita reconstrucciones redundantes (y consultas SSH) cuando el usuario
        // vuelve a pinchar la misma conexión/tab ya cargada.
        auto detailLoadedFor = [this](int connIdx, QTreeWidget* tree) -> bool {
            if (connIdx < 0) {
                return true;
            }
            if (!tree) {
                return false;
            }
            return tree->topLevelItemCount() > 0;
        };
        const bool topLoaded = detailLoadedFor(m_topDetailConnIdx, m_connContentTree);
        if (topLoaded) {
            updatePoolManagementBoxTitle();
            if (paintRoot) {
                paintRoot->setUpdatesEnabled(true);
                paintRoot->update();
            }
            return;
        }
        // Si falta contenido (p.ej. tras refresh), repoblar una sola vez.
        rebuildConnectionEntityTabs();
        refreshConnectionNodeDetails();
        updatePoolManagementBoxTitle();
        if (paintRoot) {
            paintRoot->setUpdatesEnabled(true);
            paintRoot->update();
        }
        return;
    }
    m_lastConnectionSelectionKey = selectionKey;
    rebuildConnectionEntityTabs();
    populateAllPoolsTables();
    refreshConnectionNodeDetails();
    updatePoolManagementBoxTitle();
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(true);
        paintRoot->update();
    }
}

void MainWindow::rebuildConnContentDetailTree(QTreeWidget* tree,
                                              int connIdx,
                                              bool& rebuildingFlag,
                                              int* forceRestoreConnIdx,
                                              const std::function<void(int)>& saveTreeState,
                                              const std::function<void()>& clearPendingState) {
    if (!tree) {
        return;
    }
    QScopedValueRollback<bool> rebuildingGuard(rebuildingFlag, true);
    const QSignalBlocker blockTree(tree);
    const QString savedStateToken = connContentStateTokenForTree(tree);
    if (clearPendingState) {
        clearPendingState();
    }
    if (forceRestoreConnIdx && connIdx >= 0 && *forceRestoreConnIdx == connIdx) {
        *forceRestoreConnIdx = -1;
    }
    tree->clear();
    const bool unifiedTree = tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool();
    if (!unifiedTree
        && (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()
            || isConnectionDisconnected(connIdx))) {
        syncConnContentPropertyColumnsFor(tree, connContentTokenForTree(tree));
        return;
    }
    if (unifiedTree) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            const ConnectionRuntimeState state =
                (i < m_states.size()) ? m_states[i] : ConnectionRuntimeState{};
            populateConnectionPoolsIntoTree(tree, i, state);
        }
    } else {
        const ConnectionRuntimeState st = m_states[connIdx];
        populateConnectionPoolsIntoTree(tree, connIdx, st);
    }
    if (tree->topLevelItemCount() == 0) {
        auto* noPools = new QTreeWidgetItem();
        noPools->setText(0, trk(QStringLiteral("t_no_pools_001"),
                                QStringLiteral("Sin Pools"),
                                QStringLiteral("No Pools"),
                                QStringLiteral("无存储池")));
        QFont f = noPools->font(0);
        f.setItalic(true);
        noPools->setFont(0, f);
        noPools->setFlags((noPools->flags() & ~Qt::ItemIsSelectable) & ~Qt::ItemIsEnabled);
        tree->addTopLevelItem(noPools);
    }
    const QString restoreStateToken = !savedStateToken.isEmpty()
                                          ? savedStateToken
                                          : connContentStateTokenForTree(tree);
    if (!restoreStateToken.isEmpty()) {
        restoreConnContentTreeStateFor(tree, restoreStateToken);
    } else if (tree->topLevelItemCount() > 0) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (item && item->data(0, kIsPoolRootRole).toBool()) {
                item->setExpanded(true);
            }
        }
    }
    applyUserExpandedState(tree);
    applyDebugNodeIdsToTree(tree);
    if (saveTreeState) {
        saveTreeState(connIdx);
    }
    QString token;
    if (tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()) {
        token = connContentTokenForTree(tree);
    } else if (connIdx >= 0 && connIdx < m_profiles.size()) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* root = tree->topLevelItem(i);
            if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
                continue;
            }
            const int rootConnIdx = root->data(0, Qt::UserRole + 10).toInt();
            const QString poolName = root->data(0, Qt::UserRole + 11).toString().trimmed();
            if (rootConnIdx == connIdx && !poolName.isEmpty()) {
                token = QStringLiteral("%1::%2").arg(rootConnIdx).arg(poolName);
                break;
            }
        }
    }
    if (!token.isEmpty()) {
        syncConnContentPoolColumnsFor(tree, token);
    }
}

void MainWindow::updateSecondaryConnectionDetail() {
    // Árbol inferior eliminado en el rediseño global.
}

void MainWindow::saveTopTreeStateForConnection(int connIdx) {
    Q_UNUSED(connIdx);
}

void MainWindow::saveBottomTreeStateForConnection(int connIdx) {
    Q_UNUSED(connIdx);
}

void MainWindow::restoreTopTreeStateForConnection(int connIdx) {
    Q_UNUSED(connIdx);
    if (!m_connContentTree) {
        return;
    }
    const QString token = connContentTokenForTree(m_connContentTree);
    if (!token.isEmpty()) {
        restoreConnContentTreeStateFor(m_connContentTree, token);
    }
}

void MainWindow::restoreBottomTreeStateForConnection(int connIdx) {
    Q_UNUSED(connIdx);
}

void MainWindow::rebuildConnectionEntityTabs() {
    rebuildConnContentDetailTree(m_connContentTree,
                                 m_topDetailConnIdx,
                                 m_rebuildingTopConnContentTree,
                                 &m_forceRestoreTopStateConnIdx,
                                 [this](int connIdx) { saveTopTreeStateForConnection(connIdx); });
    rebuildAllSplitTrees();
}

void MainWindow::populateConnectionPoolsIntoTree(QTreeWidget* tree,
                                                 int connIdx,
                                                 const ConnectionRuntimeState& st) {
    if (!tree || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const bool unifiedTree = tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool();
    if (unifiedTree) {
        QTreeWidgetItem* connRoot = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (!item || !item->data(0, kIsConnectionRootRole).toBool()) {
                continue;
            }
            if (item->data(0, kConnIdxRole).toInt() == connIdx) {
                connRoot = item;
                break;
            }
        }
        if (!connRoot) {
            connRoot = new QTreeWidgetItem();
            connRoot->setData(0, kIsConnectionRootRole, true);
            connRoot->setData(0, kConnIdxRole, connIdx);
            connRoot->setFlags(connRoot->flags() & ~Qt::ItemIsUserCheckable);
            connRoot->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            tree->addTopLevelItem(connRoot);
        }
        QString connName = m_profiles[connIdx].name.trimmed().isEmpty()
                               ? m_profiles[connIdx].id.trimmed()
                               : m_profiles[connIdx].name.trimmed();
        if (connIdx < m_states.size() && m_states[connIdx].daemonNeedsAttention) {
            connName += QStringLiteral(" (*)");
        }
        const QString connPrefix =
            trk(QStringLiteral("t_tree_connection_prefix_001"),
                QStringLiteral("Conexión"),
                QStringLiteral("Connection"),
                QStringLiteral("连接"));
        connRoot->setText(0, QStringLiteral("%1 %2").arg(connPrefix, connName));
        connRoot->setBackground(0, QBrush(connectionStateRowColor(connIdx)));
        connRoot->setToolTip(0, connectionStateTooltipHtml(connIdx));
        QFont f = connRoot->font(0);
        f.setBold(true);
        f.setItalic(isConnectionDisconnected(connIdx));
        connRoot->setFont(0, f);
        ensureConnectionRootAuxNodes(tree, connRoot, connIdx);
        connRoot->setExpanded(true);
        if (isConnectionDisconnected(connIdx)) {
            // En desconectado no se deben mostrar los bloques auxiliares
            // "Properties" e "Info" bajo la conexión raíz.
            for (int c = connRoot->childCount() - 1; c >= 0; --c) {
                QTreeWidgetItem* child = connRoot->child(c);
                if (!child) {
                    continue;
                }
                const QString sectionId = child->data(0, kConnRootSectionRole).toString();
                if (sectionId == QStringLiteral("connection_properties")
                    || sectionId == QStringLiteral("connection_info")) {
                    delete connRoot->takeChild(c);
                }
            }
            return;
        }
    }
    const DatasetTreeRenderOptions options =
        datasetTreeRenderOptionsForTree(tree, DatasetTreeContext::ConnectionContentMulti);
    auto addPoolTree = [this, tree, &options](int cidx, const QString& poolName, bool allowRemoteLoadIfMissing) {
        appendDatasetTreeForPool(tree,
                                 cidx,
                                 poolName,
                                 DatasetTreeContext::ConnectionContentMulti,
                                 options,
                                 allowRemoteLoadIfMissing);
    };
    QSet<QString> seenPools;
    for (const PoolImported& pool : st.importedPools) {
        const QString poolName = pool.pool.trimmed();
        const QString poolKey = poolName.toLower();
        if (poolName.isEmpty() || seenPools.contains(poolKey)) {
            continue;
        }
        seenPools.insert(poolKey);
        addPoolTree(connIdx, poolName, true);
    }
    for (const PoolImportable& pool : st.importablePools) {
        const QString poolName = pool.pool.trimmed();
        const QString poolKey = poolName.toLower();
        if (poolName.isEmpty() || seenPools.contains(poolKey)) {
            continue;
        }
        const QString stateUp = pool.state.trimmed().toUpper();
        const QString actionTxt = pool.action.trimmed();
        if (stateUp != QStringLiteral("ONLINE") || actionTxt.isEmpty()) {
            continue;
        }
        seenPools.insert(poolKey);
        addPoolTree(connIdx, poolName, false);
    }
    if (unifiedTree) {
        // En árbol unificado, los pools cuelgan de cada conexión raíz.
        // Hay que deduplicar dentro de cada conexión (mismo connIdx + poolName).
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* connRoot = tree->topLevelItem(i);
            if (!connRoot || !connRoot->data(0, kIsConnectionRootRole).toBool()) {
                continue;
            }
            QSet<QString> seenPoolKeys;
            for (int c = connRoot->childCount() - 1; c >= 0; --c) {
                QTreeWidgetItem* child = connRoot->child(c);
                if (!child || !child->data(0, kIsPoolRootRole).toBool()) {
                    continue;
                }
                const int childConnIdx = child->data(0, kConnIdxRole).toInt();
                const QString poolKey = child->data(0, kPoolNameRole).toString().trimmed().toLower();
                const QString dedupeKey = QStringLiteral("%1::%2").arg(childConnIdx).arg(poolKey);
                if (poolKey.isEmpty() || !seenPoolKeys.contains(dedupeKey)) {
                    seenPoolKeys.insert(dedupeKey);
                    continue;
                }
                delete connRoot->takeChild(c);
            }
        }
    } else {
        for (int i = tree->topLevelItemCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (!item || !item->data(0, kIsPoolRootRole).toBool()) {
                continue;
            }
            const QString poolKey = item->data(0, kPoolNameRole).toString().trimmed().toLower();
            bool seenEarlier = false;
            for (int j = 0; j < i; ++j) {
                QTreeWidgetItem* prev = tree->topLevelItem(j);
                if (!prev || !prev->data(0, kIsPoolRootRole).toBool()) {
                    continue;
                }
                if (prev->data(0, kPoolNameRole).toString().trimmed().toLower() == poolKey) {
                    seenEarlier = true;
                    break;
                }
            }
            if (seenEarlier) {
                delete tree->takeTopLevelItem(i);
            }
        }
    }
    tree->expandToDepth(0);
    attachDatasetTreeSnapshotCombos(tree, DatasetTreeContext::ConnectionContent);
}

void MainWindow::refreshConnectionNodeDetails() {
    auto setConnectionActionButtonsVisible = [this](bool visible) {
        Q_UNUSED(visible);
    };
    auto setPoolActionButtonsVisible = [this](bool visible) {
        if (m_poolStatusRefreshBtn) {
            m_poolStatusRefreshBtn->setVisible(visible);
        }
        if (m_poolStatusImportBtn) {
            m_poolStatusImportBtn->setVisible(visible);
        }
        if (m_poolStatusExportBtn) {
            m_poolStatusExportBtn->setVisible(visible);
        }
        if (m_poolStatusScrubBtn) {
            m_poolStatusScrubBtn->setVisible(visible);
        }
        if (m_poolStatusDestroyBtn) {
            m_poolStatusDestroyBtn->setVisible(visible);
        }
    };

    auto resetPoolActionButtons = [this]() {
        if (m_poolStatusImportBtn) {
            m_poolStatusImportBtn->setEnabled(false);
        }
        if (m_poolStatusRefreshBtn) {
            m_poolStatusRefreshBtn->setProperty("zfsmgr_can_refresh", false);
            m_poolStatusRefreshBtn->setEnabled(false);
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
    };

    int connIdx = m_topDetailConnIdx;
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        setConnectionActionButtonsVisible(false);
        setPoolActionButtonsVisible(false);
        if (m_connPropsStack && m_connContentPage) {
            m_connPropsStack->setCurrentWidget(m_connContentPage);
        }
        if (m_poolPropsTable) {
            setTablePopulationMode(m_poolPropsTable, true);
            m_poolPropsTable->setRowCount(0);
            setTablePopulationMode(m_poolPropsTable, false);
        }
        if (m_poolStatusText) {
            m_poolStatusText->clear();
        }
        resetPoolActionButtons();
        if (m_connContentTree) {
            m_connContentTree->clear();
            syncConnContentPropertyColumnsFor(m_connContentTree, connContentTokenForTree(m_connContentTree));
        }
        if (m_connContentPropsTable) {
            setTablePopulationMode(m_connContentPropsTable, true);
            m_connContentPropsTable->setRowCount(0);
            setTablePopulationMode(m_connContentPropsTable, false);
        }
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    // Modo sin tabs de pools: el contenido se muestra directamente en el árbol
    // (múltiples raíces de pool). No vaciar ni repoblar aquí por "pool activo".
    if (m_connPropsStack && m_connContentPage) {
        m_connPropsStack->setCurrentWidget(m_connContentPage);
    }
    setConnectionActionButtonsVisible(false);
    setPoolActionButtonsVisible(false);
    updateConnectionActionsState();
    updateConnectionDetailTitlesForCurrentSelection();
    return;
}

void MainWindow::updateConnectionDetailTitlesForCurrentSelection() {
    // No-op: la vista de detalle ya no usa subpestañas internas ocultas.
}

int MainWindow::selectedConnectionIndexForPoolManagement() const {
    const int idx = currentConnectionIndexFromUi();
    if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
        return idx;
    }
    return -1;
}

void MainWindow::updatePoolManagementBoxTitle() {
    const int idx = selectedConnectionIndexForPoolManagement();
    const QString connText = (idx >= 0 && idx < m_profiles.size())
                                 ? m_profiles[idx].name
                                 : trk(QStringLiteral("t_empty_brkt_01"), QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]"));
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setTitle(
            trk(QStringLiteral("t_pool_mgmt_of01"),
                QStringLiteral("Gestión de Pools de %1"),
                QStringLiteral("Pool Management of %1"),
                QStringLiteral("%1 的池管理"))
                .arg(connText));
    }
}

void MainWindow::refreshConnectionByIndex(int idx) {
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    // Guard against re-entrant calls for the same connection index.
    // runSsh() calls processEvents() while waiting for SSH results; that can
    // dispatch a queued refreshConnectionByIndex (e.g. from the ZED watcher)
    // while ensureDatasetsLoaded holds a reference into m_poolDatasetCache.
    // A nested call would erase those entries, leaving a dangling reference.
    if (m_refreshingByIndex.contains(idx)) {
        return;
    }
    m_refreshingByIndex.insert(idx);
    struct RefreshGuard {
        QSet<int>& set;
        int key;
        ~RefreshGuard() { set.remove(key); }
    } guard{m_refreshingByIndex, idx};
    // Al refrescar una conexión, invalidar toda la caché asociada a todos sus pools.
    {
        const QString connPrefix = QStringLiteral("%1::").arg(idx);
        auto dsIt = m_poolDatasetCache.begin();
        while (dsIt != m_poolDatasetCache.end()) {
            if (dsIt.key().startsWith(connPrefix)) {
                dsIt = m_poolDatasetCache.erase(dsIt);
            } else {
                ++dsIt;
            }
        }
        // Reutiliza la invalidación existente para detalle de pool + props de dataset.
        invalidatePoolDetailsCacheForConnection(idx);
        invalidatePoolAutoSnapshotInfoForConnection(idx);
        // Limpiar caché de propiedades inline del árbol de contenido para esta conexión.
        const QString uiPrefix = QStringLiteral("%1::").arg(QString::number(idx));
        auto valIt = m_connContentPropValuesByObject.begin();
        while (valIt != m_connContentPropValuesByObject.end()) {
            if (valIt.key().startsWith(uiPrefix)) {
                valIt = m_connContentPropValuesByObject.erase(valIt);
            } else {
                ++valIt;
            }
        }
    }
    // Marcar el nodo de conexión como "Cargando" antes de las operaciones SSH.
    if (m_connContentTree) {
        QTreeWidgetItem* connRootItem = nullptr;
        for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = m_connContentTree->topLevelItem(i);
            if (item && item->data(0, kIsConnectionRootRole).toBool()
                && item->data(0, kConnIdxRole).toInt() == idx) {
                connRootItem = item;
                break;
            }
        }
        if (connRootItem) {
            const QString connName = (idx < m_profiles.size())
                ? (m_profiles[idx].name.trimmed().isEmpty()
                       ? m_profiles[idx].id.trimmed()
                       : m_profiles[idx].name.trimmed())
                : QStringLiteral("?");
            const QString prefix = trk(QStringLiteral("t_tree_connection_prefix_001"),
                                       QStringLiteral("Conexión"),
                                       QStringLiteral("Connection"),
                                       QStringLiteral("连接"));
            const QString loadingHint = trk(QStringLiteral("t_loading_001"),
                                            QStringLiteral("Cargando…"),
                                            QStringLiteral("Loading…"),
                                            QStringLiteral("加载中…"));
            connRootItem->setText(0, QStringLiteral("%1 %2 [%3]").arg(prefix, connName, loadingHint));
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
    // Copy, not a direct binding to the vector element: refreshConnection() holds its
    // parameter across ~16 runSsh() calls, each pumping the event loop, and
    // loadConnections() can reassign m_profiles wholesale meanwhile.
    const ConnectionProfile profileForRefresh = m_profiles[idx];
    const QString refreshedConnId = profileForRefresh.id.trimmed();
    const ConnectionRuntimeState refreshedState = refreshConnection(profileForRefresh);
    // Re-resolve the index before applying the result: a dedup pass can remove
    // connections while the refresh runs, shifting every index after it. Writing to a
    // stale index would apply this state to a different connection.
    const int targetIdx =
        refreshedConnId.isEmpty() ? idx : connectionIndexByNameOrId(refreshedConnId);
    if (targetIdx < 0 || targetIdx >= m_states.size()) {
        return;
    }
    m_states[targetIdx] = refreshedState;
    cachePoolStatusTextsForConnection(targetIdx, m_states[targetIdx]);
    rebuildConnInfoFor(targetIdx);
    preloadPoolAutoSnapshotInfoForConnection(targetIdx);
    rebuildConnectionsTable();
    populateAllPoolsTables();
}
void MainWindow::loadConnections() {
    QString prevSelectedConnId;
    {
        const int prevIdx = currentConnectionIndexFromUi();
        if (prevIdx >= 0 && prevIdx < m_profiles.size()) {
            prevSelectedConnId = m_profiles[prevIdx].id.trimmed();
        }
    }

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
    {
        QSet<QString> validKeys;
        for (int i = 0; i < m_profiles.size(); ++i) {
            const QString key = connectionPersistKey(i);
            if (!key.isEmpty()) {
                validKeys.insert(key);
            }
        }
        for (auto it = m_disconnectedConnectionKeys.begin(); it != m_disconnectedConnectionKeys.end();) {
            if (!validKeys.contains(*it)) {
                it = m_disconnectedConnectionKeys.erase(it);
            } else {
                ++it;
            }
        }
    }
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
    rebuildConnInfoModel();

    rebuildConnectionsTable();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }

    int targetConnIdx = -1;
    if (!prevSelectedConnId.trimmed().isEmpty()) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].id.trimmed().compare(prevSelectedConnId, Qt::CaseInsensitive) == 0) {
                targetConnIdx = i;
                break;
            }
        }
    }
    if (targetConnIdx < 0 && m_initialRefreshCompleted) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (!isConnectionDisconnected(i)) {
                targetConnIdx = i;
                break;
            }
        }
    }
    if (targetConnIdx >= 0) {
        setCurrentConnectionInUi(targetConnIdx);
    }

    syncConnectionLogTabs();
    updatePoolManagementBoxTitle();

    for (int i = 0; i < m_profiles.size(); ++i) {
        if (isLocalConnection(i) && !m_profiles[i].machineUid.trimmed().isEmpty()) {
            QTimer::singleShot(0, this, [this, i]() {
                removeDuplicateMachineConnections(i);
            });
            break;
        }
    }
}

void MainWindow::rebuildConnectionsTable() {
    auto connIdxFromPersistedKey = [this](const QString& wantedKey) -> int {
        const QString wanted = wantedKey.trimmed().toLower();
        if (wanted.isEmpty()) {
            return -1;
        }
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (isConnectionDisconnected(i)) {
                continue;
            }
            const QString key = connPersistKeyFromProfiles(m_profiles, i);
            if (!key.isEmpty() && key == wanted) {
                return i;
            }
        }
        return -1;
    };
    auto firstConnectedIndex = [this]() -> int {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (!isConnectionDisconnected(i)) {
                return i;
            }
        }
        return -1;
    };
    if (!m_connSelectorDefaultsInitialized) {
        if (m_topDetailConnIdx < 0) {
            m_topDetailConnIdx = connIdxFromPersistedKey(m_persistedTopDetailConnectionKey);
        }
        if (m_topDetailConnIdx < 0 || m_topDetailConnIdx >= m_profiles.size()
            || isConnectionDisconnected(m_topDetailConnIdx)) {
            m_topDetailConnIdx = firstConnectedIndex();
        }
        m_bottomDetailConnIdx = -1;
        m_connSelectorDefaultsInitialized = true;
    } else {
        if (m_topDetailConnIdx < 0 || m_topDetailConnIdx >= m_profiles.size()
            || isConnectionDisconnected(m_topDetailConnIdx)) {
            m_topDetailConnIdx = -1;
            m_topDetailConnIdx = firstConnectedIndex();
        }
    }

    rebuildConnectionEntityTabs();
    if (m_topDetailConnIdx >= 0) {
        setCurrentConnectionInUi(m_topDetailConnIdx);
    }
    syncConnectionLogTabs();
    refreshConnectionNodeDetails();
    updatePoolManagementBoxTitle();
}

void MainWindow::createConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: nueva conexión bloqueada"));
        return;
    }
    ConnectionDialog dlg(m_language, this);
    ConnectionProfile p;
    p.connType = QStringLiteral("SSH");
    p.osType = QStringLiteral("Linux");
    p.port = 22;
    dlg.setProfile(p);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile created = dlg.profile();
    {
        const QString newName = created.name.trimmed();
        for (const ConnectionProfile& cp : m_profiles) {
            if (cp.name.trimmed().compare(newName, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_conn_name_unique_01"),
                                         QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                                         QStringLiteral("Connection name already exists. It must be unique."),
                                         QStringLiteral("连接名称已存在，必须唯一。")));
                return;
            }
        }
    }
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
                              trk(QStringLiteral("t_conn_create_er1"),
                                  QStringLiteral("No se pudo crear conexión:\n%1"),
                                  QStringLiteral("Could not create connection:\n%1"),
                                  QStringLiteral("无法创建连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id.trimmed().compare(createdId, Qt::CaseInsensitive) == 0) {
            removeDuplicateMachineConnections(i);
            break;
        }
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == createdId) {
            setCurrentConnectionInUi(i);
            if (!isLocalConnection(i)) {
                QTimer::singleShot(0, this, [this, i]() {
                    if (i < 0 || i >= m_profiles.size()) {
                        return;
                    }
                    if (!installOrUpdateDaemonForConnectionInternal(i, false)) {
                        setConnectionDisconnected(i, true);
                    }
                });
            } else {
                refreshSelectedConnection();
            }
            break;
        }
    }
}

void MainWindow::repairAltMountpointsForSelectedConnection() {
    const int connIdx = selectedConnectionIndexForPoolManagement();
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const ConnectionProfile p = m_profiles[connIdx];
    if (!requireFeature(connIdx, zfsmgr::caps::Feature::RepairAltMountpoints)) {
        return;
    }

    const QString agentBin = daemonpayload::unixBinPath();
    // Report-only pass first: nothing is modified until the user confirms.
    QString out;
    QString failure;
    const bool ok = fetchConnectionCommandOutput(
        connIdx,
        QStringLiteral("Repair alt mountpoints (report)"),
        withSudo(p, mwhelpers::withUnixSearchPathCommand(
                        agentBin + QStringLiteral(" --repair-alt-mountpoints"))),
        &out,
        &failure,
        30000);
    if (!ok) {
        QMessageBox::warning(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_repair_altmp_fail001"),
                QStringLiteral("No se pudo consultar el estado: %1"),
                QStringLiteral("Could not query state: %1"),
                QStringLiteral("无法查询状态：%1")).arg(mwhelpers::oneLine(failure)));
        return;
    }

    QStringList stranded;
    for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
        if (line.trimmed().startsWith(QStringLiteral("STRANDED="))) {
            stranded << line.trimmed();
        }
    }
    if (stranded.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_repair_altmp_none001"),
                QStringLiteral("No hay datasets con mountpoint temporal pendiente en \"%1\"."),
                QStringLiteral("No datasets are left on a temporary mountpoint on \"%1\"."),
                QStringLiteral("“%1”上没有停留在临时挂载点的数据集。")).arg(p.name));
        return;
    }

    const QString detail =
        trk(QStringLiteral("t_repair_altmp_confirm001"),
            QStringLiteral("Se han encontrado %1 dataset(s) con el mountpoint relocalizado por una "
                           "sincronización interrumpida:\n\n%2\n\n¿Restaurar su mountpoint original?\n"
                           "Se desmontarán antes de restaurarlo."),
            QStringLiteral("Found %1 dataset(s) whose mountpoint was relocated by an interrupted "
                           "sync:\n\n%2\n\nRestore their original mountpoint?\n"
                           "They will be unmounted first."),
            QStringLiteral("发现 %1 个数据集的挂载点因同步中断而被重定位：\n\n%2\n\n是否恢复其原始挂载点？\n将先卸载它们。"))
            .arg(stranded.size())
            .arg(stranded.join(QLatin1Char('\n')));
    if (QMessageBox::question(this, QStringLiteral("ZFSMgr"), detail,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    QString applyOut;
    QString applyFail;
    const bool applied = fetchConnectionCommandOutput(
        connIdx,
        QStringLiteral("Repair alt mountpoints (apply)"),
        withSudo(p, mwhelpers::withUnixSearchPathCommand(
                        agentBin + QStringLiteral(" --repair-alt-mountpoints apply"))),
        &applyOut,
        &applyFail,
        60000);
    int repaired = 0;
    int failed = 0;
    for (const QString& line : applyOut.split('\n', Qt::SkipEmptyParts)) {
        const QString t = line.trimmed();
        if (t.startsWith(QStringLiteral("REPAIRED_COUNT="))) {
            repaired = t.section('=', 1).trimmed().toInt();
        } else if (t.startsWith(QStringLiteral("FAILED_COUNT="))) {
            failed = t.section('=', 1).trimmed().toInt();
        }
    }
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Reparación de mountpoints temporales en %1: %2 restaurados, %3 fallidos")
               .arg(p.name)
               .arg(repaired)
               .arg(failed));
    if (!applied || failed > 0) {
        QMessageBox::warning(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_repair_altmp_partial001"),
                QStringLiteral("Restaurados %1, fallidos %2.\nLos que fallaron conservan la marca y "
                               "se pueden reintentar.\n%3"),
                QStringLiteral("Restored %1, failed %2.\nThose that failed keep their marker and can "
                               "be retried.\n%3"),
                QStringLiteral("已恢复 %1 个，失败 %2 个。\n失败的仍保留标记，可以重试。\n%3"))
                .arg(repaired)
                .arg(failed)
                .arg(mwhelpers::oneLine(applyFail)));
    } else {
        QMessageBox::information(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_repair_altmp_ok001"),
                QStringLiteral("Restaurados %1 dataset(s)."),
                QStringLiteral("Restored %1 dataset(s)."),
                QStringLiteral("已恢复 %1 个数据集。")).arg(repaired));
    }
    refreshConnectionByIndex(connIdx);
}

void MainWindow::exportTrustStoreToSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: export trust-store bloqueado"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    if (isLocalConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_trust_export_local_001"),
                QStringLiteral("La conexión Local ya usa el trust-store local."),
                QStringLiteral("The Local connection already uses the local trust-store."),
                QStringLiteral("本地连接已使用本地 trust-store。")));
        return;
    }

    const QString trustPath = m_store.trustStorePath();
    QFile trustFile(trustPath);
    if (!trustFile.exists()) {
        // Fuerza migración de TLS legacy desde config.json si existe.
        const LoadResult loaded = m_store.loadConnections();
        for (const QString& warning : loaded.warnings) {
            appLog(QStringLiteral("WARN"), warning);
        }
        trustFile.setFileName(trustPath);
    }
    if (!trustFile.exists() || !trustFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_trust_export_missing_001"),
                QStringLiteral("No existe trust-store.json local para exportar."),
                QStringLiteral("There is no local trust-store.json to export."),
                QStringLiteral("没有可导出的本地 trust-store.json。")));
        return;
    }
    const QByteArray payload = trustFile.readAll();
    if (payload.trimmed().isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_trust_export_empty_001"),
                QStringLiteral("trust-store.json está vacío."),
                QStringLiteral("trust-store.json is empty."),
                QStringLiteral("trust-store.json 为空。")));
        return;
    }

    const ConnectionProfile p = m_profiles[idx];
    const int confirm = QMessageBox::question(
        this,
        QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_trust_export_confirm_001"),
            QStringLiteral("Se copiará el trust-store portable local a \"%1\".\n\n"
                           "El fichero remoto anterior se conservará como backup si existe. "
                           "La máquina remota podrá usar estos certificados si se desbloquea ZFSMgr con el mismo password maestro.\n\n"
                           "¿Continuar?"),
            QStringLiteral("The local portable trust-store will be copied to \"%1\".\n\n"
                           "The previous remote file will be kept as a backup if it exists. "
                           "The remote machine will be able to use these certificates when ZFSMgr is unlocked with the same master password.\n\n"
                           "Continue?"),
            QStringLiteral("本地便携 trust-store 将复制到“%1”。\n\n"
                           "如果远端文件已存在，将保留备份。使用相同主密码解锁 ZFSMgr 后，远端机器即可使用这些证书。\n\n"
                           "是否继续？"))
            .arg(p.name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    QString remoteCmd;
    if (isWindowsConnection(p)) {
        remoteCmd = QStringLiteral(
            "$ErrorActionPreference='Stop'; "
            "$dir=Join-Path $env:USERPROFILE '.config\\ZFSMgr'; "
            "New-Item -ItemType Directory -Force -Path $dir | Out-Null; "
            "$dst=Join-Path $dir 'trust-store.json'; "
            "if (Test-Path -LiteralPath $dst) { "
            "  $stamp=Get-Date -Format 'yyyyMMddHHmmss'; "
            "  Copy-Item -LiteralPath $dst -Destination ($dst + '.bak.' + $stamp) -Force; "
            "} "
            "$reader=[System.IO.StreamReader]::new([Console]::OpenStandardInput(), [System.Text.Encoding]::UTF8); "
            "$content=$reader.ReadToEnd(); "
            "[System.IO.File]::WriteAllText($dst, $content, [System.Text.UTF8Encoding]::new($false)); "
            "Write-Output ('trust-store exported to ' + $dst)");
    } else {
        const QString sudoPrefix = (!p.useSudo)
                                       ? QStringLiteral("exit 1")
                                       : (!p.password.isEmpty()
                                              ? QStringLiteral("printf '%s\\n' %1 | sudo -k -S -p ''")
                                                    .arg(mwhelpers::shSingleQuote(p.password))
                                              : QStringLiteral("sudo -n"));
        remoteCmd = QStringLiteral(
            "set -u; "
            "umask 077; "
            "user_name=\"$(id -un)\"; "
            "group_name=\"$(id -gn)\"; "
            "cfg=\"${XDG_CONFIG_HOME:-$HOME/.config}/ZFSMgr\"; "
            "dst=\"$cfg/trust-store.json\"; "
            "tmp=\"${TMPDIR:-/tmp}/zfsmgr-trust-store.$$\"; "
            "trap 'rm -f \"$tmp\"' EXIT HUP INT TERM; "
            "cat > \"$tmp\"; "
            "chmod 600 \"$tmp\" 2>/dev/null || true; "
            "if mkdir -p \"$cfg\" 2>/dev/null && [ -w \"$cfg\" ]; then "
            "  if [ -f \"$dst\" ]; then cp -p \"$dst\" \"$dst.bak.$(date +%Y%m%d%H%M%S)\" 2>/dev/null || true; fi; "
            "  mv -f \"$tmp\" \"$dst\"; chmod 600 \"$dst\" 2>/dev/null || true; "
            "else "
            "  sudo_script='cfg=\"$1\"; dst=\"$2\"; tmp=\"$3\"; user_name=\"$4\"; group_name=\"$5\"; "
            "mkdir -p \"$cfg\"; "
            "if [ -f \"$dst\" ]; then cp -p \"$dst\" \"$dst.bak.$(date +%Y%m%d%H%M%S)\" 2>/dev/null || true; fi; "
            "install -m 600 -o \"$user_name\" -g \"$group_name\" \"$tmp\" \"$dst\"; "
            "chown \"$user_name:$group_name\" \"$cfg\" 2>/dev/null || true; chmod 700 \"$cfg\" 2>/dev/null || true'; "
            "%1 sh -c \"$sudo_script\" sh \"$cfg\" \"$dst\" \"$tmp\" \"$user_name\" \"$group_name\"; "
            "fi; "
            "rm -f \"$tmp\"; "
            "trap - EXIT HUP INT TERM; "
            "printf 'trust-store exported to %s\\n' \"$dst\"")
            .arg(sudoPrefix);
    }

    beginUiBusy();
    struct TrustExportBusyGuard {
        MainWindow* w;
        ~TrustExportBusyGuard() { w->endUiBusy(); }
    } busyGuard{this};
    updateStatus(trk(QStringLiteral("t_trust_export_status_001"),
                     QStringLiteral("Exportando trust-store..."),
                     QStringLiteral("Exporting trust-store..."),
                     QStringLiteral("正在导出 trust-store...")));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);

    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, 30000, out, err, rc, {}, {}, {}, payload) && rc == 0;
    if (!ok) {
        const QString detail = mwhelpers::oneLine(err.trimmed().isEmpty() ? out : err);
        appLog(QStringLiteral("ERROR"),
               QStringLiteral("trust-store export fallo en %1: %2")
                   .arg(p.name, detail.isEmpty() ? QStringLiteral("rc=%1").arg(rc) : detail));
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_trust_export_fail_001"),
                QStringLiteral("No se pudo exportar trust-store a \"%1\".\n\n%2"),
                QStringLiteral("Could not export trust-store to \"%1\".\n\n%2"),
                QStringLiteral("无法将 trust-store 导出到“%1”。\n\n%2"))
                .arg(p.name,
                     detail.isEmpty() ? QStringLiteral("rc=%1").arg(rc) : detail));
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("trust-store exportado a %1: %2")
               .arg(p.name, mwhelpers::oneLine(out).trimmed()));
    QMessageBox::information(
        this,
        QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_trust_export_ok_001"),
            QStringLiteral("trust-store exportado a \"%1\"."),
            QStringLiteral("trust-store exported to \"%1\"."),
            QStringLiteral("trust-store 已导出到“%1”。"))
            .arg(p.name));
}

void MainWindow::editConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: edición bloqueada"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    if (isLocalConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_local_builtin_01"),
                QStringLiteral("La conexión local integrada no se puede editar."),
                QStringLiteral("Built-in local connection cannot be edited."),
                QStringLiteral("内置本地连接不可编辑。")));
        return;
    }
    const bool redirectedLocal = isConnectionRedirectedToLocal(idx);
    if (redirectedLocal) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_redirect_l2"),
                QStringLiteral("La conexión está redirigida a 'Local' y no se puede editar."),
                QStringLiteral("This connection is redirected to 'Local' and cannot be edited."),
                QStringLiteral("该连接已重定向到“本地”，不可编辑。")));
        return;
    }
    ConnectionDialog dlg(m_language, this);
    dlg.setProfile(m_profiles[idx]);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile edited = dlg.profile();
    edited.id = m_profiles[idx].id;
    {
        const QString editedName = edited.name.trimmed();
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (i == idx) {
                continue;
            }
            if (m_profiles[i].name.trimmed().compare(editedName, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_conn_name_unique_01"),
                                         QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                                         QStringLiteral("Connection name already exists. It must be unique."),
                                         QStringLiteral("连接名称已存在，必须唯一。")));
                return;
            }
        }
    }
    const QString editedId = edited.id.trimmed();
    QString err;
    if (!m_store.upsertConnection(edited, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_conn_update_er"),
                                  QStringLiteral("No se pudo actualizar conexión:\n%1"),
                                  QStringLiteral("Could not update connection:\n%1"),
                                  QStringLiteral("无法更新连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id.trimmed().compare(editedId, Qt::CaseInsensitive) == 0) {
            removeDuplicateMachineConnections(i);
            break;
        }
    }
}

void MainWindow::installHelperCommandsForSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: instalación de comandos auxiliares bloqueada"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const ConnectionProfile p = m_profiles[idx];
    if (isConnectionDisconnected(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_conn_disc_01"),
                QStringLiteral("La conexión está desconectada."),
                QStringLiteral("The connection is disconnected."),
                QStringLiteral("该连接已断开。")));
        return;
    }
    if (idx >= m_states.size()) {
        return;
    }
    const ConnectionRuntimeState& st = m_states[idx];
    if (!requireFeature(idx, zfsmgr::caps::Feature::HelperCommandInstall)) {
        return;
    }
    if (st.missingUnixCommands.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_none_missing_01"),
                QStringLiteral("No faltan comandos auxiliares en esta conexión."),
                QStringLiteral("No helper commands are missing on this connection."),
                QStringLiteral("该连接不缺少辅助命令。")));
        return;
    }
    if (!st.helperInstallSupported || st.helperInstallCommandPreview.trimmed().isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_not_supported_01"),
                QStringLiteral("No hay instalación asistida disponible para esta conexión.\n\n%1"),
                QStringLiteral("Assisted installation is not available for this connection.\n\n%1"),
                QStringLiteral("此连接不可用辅助安装。\n\n%1"))
                .arg(st.helperInstallReason.trimmed().isEmpty() ? QStringLiteral("-") : st.helperInstallReason.trimmed()));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_helper_install_title_01"),
            QStringLiteral("Instalar comandos auxiliares"),
            QStringLiteral("Install helper commands"),
            QStringLiteral("安装辅助命令")));
    dlg.resize(760, 520);

    auto* layout = new QVBoxLayout(&dlg);
    auto* summary = new QLabel(
        trk(QStringLiteral("t_helper_install_summary_01"),
            QStringLiteral("Se va a preparar la conexión \"%1\" para instalar los comandos auxiliares que faltan."),
            QStringLiteral("Connection \"%1\" will be prepared to install the missing helper commands."),
            QStringLiteral("将为连接 \"%1\" 准备缺失的辅助命令安装。")).arg(p.name),
        &dlg);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    QStringList metaLines;
    metaLines << QStringLiteral("Plataforma: %1").arg(st.helperPlatformLabel.trimmed().isEmpty()
                                                          ? QStringLiteral("-")
                                                          : st.helperPlatformLabel.trimmed());
    metaLines << QStringLiteral("Gestor de paquetes: %1").arg(st.helperPackageManagerLabel.trimmed().isEmpty()
                                                                  ? QStringLiteral("-")
                                                                  : st.helperPackageManagerLabel.trimmed());
    metaLines << QStringLiteral("Comandos faltantes: %1").arg(st.missingUnixCommands.join(QStringLiteral(", ")));
    metaLines << QStringLiteral("Comandos instalables: %1").arg(st.helperInstallableCommands.join(QStringLiteral(", ")));
    metaLines << QStringLiteral("Paquetes a instalar: %1").arg(st.helperInstallPackages.join(QStringLiteral(", ")));
    if (!st.helperUnsupportedCommands.isEmpty()) {
        metaLines << QStringLiteral("Comandos no soportados en esta fase: %1")
                         .arg(st.helperUnsupportedCommands.join(QStringLiteral(", ")));
    }
    if (!st.helperInstallReason.trimmed().isEmpty()) {
        metaLines << QStringLiteral("Notas: %1").arg(st.helperInstallReason.trimmed());
    }

    auto* metaBox = new QPlainTextEdit(metaLines.join(QStringLiteral("\n")), &dlg);
    metaBox->setReadOnly(true);
    metaBox->setMaximumBlockCount(200);
    layout->addWidget(metaBox);

    auto* previewLabel = new QLabel(
        trk(QStringLiteral("t_helper_cmd_preview_01"),
            QStringLiteral("Comando remoto previsto"),
            QStringLiteral("Planned remote command"),
            QStringLiteral("计划执行的远程命令")),
        &dlg);
    layout->addWidget(previewLabel);

    auto* preview = new QPlainTextEdit(st.helperInstallCommandPreview, &dlg);
    preview->setReadOnly(true);
    layout->addWidget(preview, 1);

    auto* refreshAfter = new QCheckBox(
        trk(QStringLiteral("t_helper_refresh_after_01"),
            QStringLiteral("Refrescar conexión al terminar"),
            QStringLiteral("Refresh connection when finished"),
            QStringLiteral("完成后刷新连接")),
        &dlg);
    refreshAfter->setChecked(true);
    layout->addWidget(refreshAfter);

    auto* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(
        trk(QStringLiteral("t_helper_cancel_01"),
            QStringLiteral("Cancelar"),
            QStringLiteral("Cancel"),
            QStringLiteral("取消")),
        QDialogButtonBox::RejectRole);
    QPushButton* installBtn = buttons->addButton(
        trk(QStringLiteral("t_helper_install_btn_01"),
            QStringLiteral("Instalar"),
            QStringLiteral("Install"),
            QStringLiteral("安装")),
        QDialogButtonBox::AcceptRole);
    Q_UNUSED(cancelBtn);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString installCmd = withSudo(p, stripLeadingSudoForExecution(st.helperInstallCommandPreview));
    beginUiBusy();
    updateStatus(
        trk(QStringLiteral("t_helper_install_busy_01"),
            QStringLiteral("Instalando comandos auxiliares en %1..."),
            QStringLiteral("Installing helper commands on %1..."),
            QStringLiteral("正在 %1 上安装辅助命令...")).arg(p.name));
    QString detail;
    const bool ok = executeConnectionCommand(idx, QStringLiteral("Instalar auxiliares"), installCmd, 1800000, &detail);
    endUiBusy();

    if (!ok) {
        detail = detail.left(1200);
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_install_fail_01"),
                QStringLiteral("No se pudieron instalar los comandos auxiliares en \"%1\".\n\n%2"),
                QStringLiteral("Could not install helper commands on \"%1\".\n\n%2"),
                QStringLiteral("无法在 \"%1\" 上安装辅助命令。\n\n%2"))
                .arg(p.name, detail));
        if (refreshAfter->isChecked()) {
            refreshConnectionByIndex(idx);
        }
        return;
    }

    if (refreshAfter->isChecked()) {
        refreshConnectionByIndex(idx);
    }

    QMessageBox::information(
        this,
        QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_helper_install_ok_01"),
            QStringLiteral("La instalación terminó correctamente en \"%1\"."),
            QStringLiteral("The installation finished successfully on \"%1\"."),
            QStringLiteral("\"%1\" 上的安装已成功完成。"))
            .arg(p.name));
}


bool MainWindow::installOrUpdateDaemonForConnectionInternal(int idx, bool interactive) {
    if (actionsLocked()) {
        return false;
    }
    if (idx < 0 || idx >= m_profiles.size()) {
        return false;
    }
    if (isConnectionDisconnected(idx)) {
        if (interactive) {
            QMessageBox::information(this,
                                     QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_daemon_conn_disc_001"),
                                         QStringLiteral("La conexión está desconectada."),
                                         QStringLiteral("The connection is disconnected."),
                                         QStringLiteral("该连接已断开。")));
        }
        return false;
    }

    const ConnectionProfile p = m_profiles[idx];
    if (interactive) {
        const auto confirm = QMessageBox::question(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_daemon_install_confirm_001"),
                QStringLiteral("ZFSMgr instalará o actualizará el daemon en \"%1\" y lo arrancará con el scheduler nativo.\n\n¿Continuar?"),
                QStringLiteral("ZFSMgr will install or update the daemon on \"%1\" and start it with the native scheduler.\n\nContinue?"),
                QStringLiteral("ZFSMgr 将在 \"%1\" 上安装或更新守护进程，并使用原生调度启动。\n\n是否继续？"))
                .arg(p.name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirm != QMessageBox::Yes) {
            return false;
        }
    }

    beginUiBusy();
    struct InstallDaemonBusyGuard {
        MainWindow* w;
        ~InstallDaemonBusyGuard() { w->endUiBusy(); }
    } busyGuard{this};
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);

    const QString daemonVersion = agentversion::currentVersion().trimmed();
    const QString apiVersion = agentversion::expectedApiVersion().trimmed();
    QString remoteCmd;
    QByteArray remoteStdinPayload;
    bool isMac = false;

    // Windows instala el daemon NATIVO, igual que en Unix. Verificado en un Windows 11
    // real que sirve TLS por el túnel y ejecuta ZFS.
    const bool windowsNativeAgent = isWindowsConnection(idx);
    if (windowsNativeAgent) {
        const QString localAgentPath =
            findDeployableAgentBinaryPath(QStringLiteral("windows"), QStringLiteral("x86_64"));
        QFile agentFile(localAgentPath);
        if (localAgentPath.isEmpty() || !agentFile.open(QIODevice::ReadOnly)) {
            const QString reason =
                trk(QStringLiteral("t_daemon_win_native_missing_001"),
                    QStringLiteral("No se encontró el binario nativo del daemon para Windows en este equipo."),
                    QStringLiteral("No native Windows daemon binary found on this machine."),
                    QStringLiteral("当前机器缺少适用于 Windows 的原生守护进程二进制文件。"));
            appLog(QStringLiteral("WARN"), QStringLiteral("Daemon deploy %1: %2").arg(p.name, reason));
            if (interactive) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"), reason);
            }
            refreshConnectionByIndex(idx);
            return false;
        }
        agentFile.close();
        // Se sube por scp ANTES de ejecutar nada: mandarlo por la entrada estándar no
        // funciona contra Windows —PowerShell no devuelve de ReadToEnd() con
        // megabytes— y la instalación se quedaba colgada hasta agotar el plazo.
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Daemon deploy %1: subiendo daemon nativo de Windows (%2 bytes) desde %3")
                   .arg(p.name)
                   .arg(QFileInfo(localAgentPath).size())
                   .arg(localAgentPath));
        const QString scpCmd = mwhelpers::scpUploadCommand(p, localAgentPath,
                                                           daemonpayload::windowsUploadPath());
        QProcess scpProc;
        scpProc.start(QStringLiteral("sh"), {QStringLiteral("-c"), scpCmd});
        const bool scpOk = scpProc.waitForStarted(5000)
                           && scpProc.waitForFinished(180000)
                           && scpProc.exitCode() == 0;
        if (!scpOk) {
            const QString detail = QString::fromUtf8(scpProc.readAllStandardError()).trimmed();
            const QString reason =
                trk(QStringLiteral("t_daemon_win_upload_fail_001"),
                    QStringLiteral("No se pudo subir el daemon a la máquina Windows: %1"),
                    QStringLiteral("Could not upload the daemon to the Windows machine: %1"),
                    QStringLiteral("无法将守护进程上传到 Windows 计算机：%1"))
                    .arg(detail.isEmpty() ? QStringLiteral("scp falló") : mwhelpers::oneLine(detail));
            appLog(QStringLiteral("WARN"), QStringLiteral("Daemon deploy %1: %2").arg(p.name, reason));
            if (interactive) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"), reason);
            }
            refreshConnectionByIndex(idx);
            return false;
        }
        remoteCmd = daemonpayload::windowsNativeInstallCommand();
    } else {
        const QString osHint = (p.osType + QStringLiteral(" ")
                                + ((idx < m_states.size()) ? m_states[idx].osLine : QString()))
                                   .trimmed()
                                   .toLower();
        isMac = osHint.contains(QStringLiteral("darwin"))
                || osHint.contains(QStringLiteral("macos"))
                || osHint.contains(QStringLiteral("launchd"));
        bool isFreeBsd = osHint.contains(QStringLiteral("freebsd"))
                         || osHint.contains(QStringLiteral("rc.d"));
        QString remoteArchHint;
        if (!isMac && !isFreeBsd) {
            QString osOut;
            QString osErr;
            int osRc = -1;
            if (runSsh(p, QStringLiteral("uname -s"), 8000, osOut, osErr, osRc) && osRc == 0) {
                const QString unameS = osOut.trimmed().toLower();
                if (unameS.contains(QStringLiteral("darwin"))) {
                    isMac = true;
                } else if (unameS.contains(QStringLiteral("freebsd"))) {
                    isFreeBsd = true;
                }
            }
        }
        {
            QString archOut;
            QString archErr;
            int archRc = -1;
            if (runSsh(p, QStringLiteral("uname -m"), 8000, archOut, archErr, archRc) && archRc == 0) {
                remoteArchHint = archOut.trimmed();
            }
        }
        const QString configPayload = daemonpayload::simpleConfigPayload(daemonVersion, apiVersion);
        const QString tlsBootstrap = daemonpayload::tlsBootstrapShellCommand();
        QString deployBinCmd;
        const QString platformId = normalizedTargetPlatformId(isMac, isFreeBsd, false);
        const QString localAgentPath = findDeployableAgentBinaryPath(platformId, remoteArchHint);
        const bool canDeployNativeLocalBinaryFromPath =
            !localAgentPath.isEmpty() && QFileInfo::exists(localAgentPath) && QFileInfo(localAgentPath).isFile();
        if (canDeployNativeLocalBinaryFromPath) {
            QFile localAgentFile(localAgentPath);
            if (!localAgentFile.open(QIODevice::ReadOnly)) {
                const QString reason = trk(
                                           QStringLiteral("t_daemon_native_read_fail_001"),
                                           QStringLiteral("No se pudo leer el binario nativo del daemon (%1)."),
                                           QStringLiteral("Could not read native daemon binary (%1)."),
                                           QStringLiteral("无法读取原生守护进程二进制文件（%1）。"))
                                           .arg(localAgentPath);
                appLog(QStringLiteral("WARN"), QStringLiteral("Daemon deploy %1: %2").arg(p.name, reason));
                if (interactive) {
                    QMessageBox::warning(this, QStringLiteral("ZFSMgr"), reason);
                }
                refreshConnectionByIndex(idx);
                return false;
            }
            remoteStdinPayload = localAgentFile.readAll();
            if (remoteStdinPayload.isEmpty()) {
                const QString reason = trk(
                                           QStringLiteral("t_daemon_native_empty_001"),
                                           QStringLiteral("El binario nativo del daemon está vacío (%1)."),
                                           QStringLiteral("Native daemon binary is empty (%1)."),
                                           QStringLiteral("原生守护进程二进制文件为空（%1）。"))
                                           .arg(localAgentPath);
                appLog(QStringLiteral("WARN"), QStringLiteral("Daemon deploy %1: %2").arg(p.name, reason));
                if (interactive) {
                    QMessageBox::warning(this, QStringLiteral("ZFSMgr"), reason);
                }
                refreshConnectionByIndex(idx);
                return false;
            }
            deployBinCmd = QStringLiteral(
                               "tmp_bin='/tmp/zfsmgr-agent.bin.$$'; "
                               "cat > \"$tmp_bin\"; "
                               "install -m 700 \"$tmp_bin\" %1; "
                               "rm -f \"$tmp_bin\"; ")
                               .arg(mwhelpers::shSingleQuote(daemonpayload::unixBinPath()));
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Daemon deploy %1: usando binario nativo local %2")
                       .arg(p.name, localAgentPath));
        } else {
            const QString reason = trk(
                                       QStringLiteral("t_daemon_native_missing_001"),
                                       QStringLiteral("No se encontró binario nativo del daemon para %1/%2 en este equipo. "
                                                      "No se instala fallback script porque no soporta daemon-rpc TLS."),
                                       QStringLiteral("No native daemon binary found for %1/%2 on this machine. "
                                                      "Script fallback is not installed because it does not support daemon-rpc TLS."),
                                       QStringLiteral("当前机器缺少适用于 %1/%2 的原生守护进程二进制文件。"
                                                      "不会安装脚本回退，因为其不支持 daemon-rpc TLS。"))
                                       .arg(platformId, remoteArchHint.isEmpty() ? QStringLiteral("?") : remoteArchHint);
            appLog(QStringLiteral("WARN"), QStringLiteral("Daemon deploy %1: %2").arg(p.name, reason));
            if (interactive) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"), reason);
            }
            refreshConnectionByIndex(idx);
            return false;
        }
        if (isMac) {
            const QString plistPayload = daemonpayload::macLaunchdPlist();
            remoteCmd = QStringLiteral(
                "mkdir -p /usr/local/libexec /etc/zfsmgr; "
                "%1 "
                "cat > %3 <<'EOF_AGENT_CONF'\n%4\nEOF_AGENT_CONF\n"
                "cat > %5 <<'EOF_AGENT_PLIST'\n%6\nEOF_AGENT_PLIST\n"
                "%7; "
                "chmod 600 %3; chmod 644 %5; "
                "chown root:wheel %2 %3 %5; "
                "chown root:wheel %8 %9 %10 %11 %12; "
                "launchctl bootout system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                "launchctl bootstrap system %5 >/dev/null 2>&1 || true; "
                "launchctl enable system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                "ok=0; "
                "i=0; "
                "while [ \"$i\" -lt 30 ]; do "
                "  if launchctl print system/org.zfsmgr.agent >/dev/null 2>&1; then ok=1; break; fi; "
                "  launchctl kickstart system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                "  i=$((i+1)); "
                "  sleep 1; "
                "done; "
                "if [ \"$ok\" -ne 1 ]; then "
                "  echo 'launchd agent not active after install' >&2; "
                "  exit 1; "
                "fi")
                            .arg(deployBinCmd,
                                 daemonpayload::unixBinPath(),
                                 daemonpayload::unixConfigPath(),
                                 configPayload,
                                 daemonpayload::macPlistPath(),
                                 plistPayload,
                                 tlsBootstrap,
                                 daemonpayload::tlsDirPath(),
                                 daemonpayload::tlsServerCertPath(),
                                 daemonpayload::tlsServerKeyPath(),
                                 daemonpayload::tlsClientCertPath(),
                                 daemonpayload::tlsClientKeyPath());
        } else if (isFreeBsd) {
            const QString rcPayload = daemonpayload::freeBsdRcScript();
            remoteCmd = QStringLiteral(
                "mkdir -p /usr/local/libexec /etc/zfsmgr /usr/local/etc/rc.d; "
                "%1 "
                "ldd_missing=$(ldd %2 2>&1 | grep 'not found' || true); "
                "if [ -n \"$ldd_missing\" ]; then "
                "  printf 'ERROR: el daemon tiene dependencias sin resolver:\\n%s\\n' \"$ldd_missing\" >&2; "
                "  printf 'Instala OpenSSL con: pkg install openssl\\n' >&2; "
                "  exit 1; "
                "fi; "
                "cat > %3 <<'EOF_AGENT_CONF'\n%4\nEOF_AGENT_CONF\n"
                "cat > %5 <<'EOF_AGENT_RC'\n%6\nEOF_AGENT_RC\n"
                "%7; "
                "chmod 700 %5; chmod 600 %3; "
                "chown root:wheel %2 %3 %5; "
                "chown root:wheel %8 %9 %10 %11 %12; "
                "service zfsmgr_agent stop >/dev/null 2>&1 || true; "
                "service zfsmgr_agent start; "
                "sleep 2; "
                "if ! service zfsmgr_agent onestatus >/dev/null 2>&1; then "
                "  printf 'ERROR: el daemon no permanece activo tras el arranque\\n' >&2; "
                "  exit 1; "
                "fi")
                            .arg(deployBinCmd,
                                 daemonpayload::unixBinPath(),
                                 daemonpayload::unixConfigPath(),
                                 configPayload,
                                 daemonpayload::freeBsdRcPath(),
                                 rcPayload,
                                 tlsBootstrap,
                                 daemonpayload::tlsDirPath(),
                                 daemonpayload::tlsServerCertPath(),
                                 daemonpayload::tlsServerKeyPath(),
                                 daemonpayload::tlsClientCertPath(),
                                 daemonpayload::tlsClientKeyPath());
        } else {
            const QString servicePayload = daemonpayload::linuxSystemdService();
            remoteCmd = QStringLiteral(
                "if ! command -v systemctl >/dev/null 2>&1; then echo 'systemd not available' >&2; exit 1; fi; "
                "mkdir -p /usr/local/libexec /etc/zfsmgr; "
                "%1 "
                "cat > %3 <<'EOF_AGENT_CONF'\n%4\nEOF_AGENT_CONF\n"
                "cat > %5 <<'EOF_AGENT_SERVICE'\n%6\nEOF_AGENT_SERVICE\n"
                "%7; "
                "chmod 600 %3; chmod 644 %5; "
                "chown root:root %2 %3 %5; "
                "chown root:root %8 %9 %10 %11 %12; "
                "systemctl daemon-reload; "
                "systemctl enable zfsmgr-agent.service; "
                "systemctl restart zfsmgr-agent.service")
                            .arg(deployBinCmd,
                                 daemonpayload::unixBinPath(),
                                 daemonpayload::unixConfigPath(),
                                 configPayload,
                                 daemonpayload::linuxServicePath(),
                                 servicePayload,
                                 tlsBootstrap,
                                 daemonpayload::tlsDirPath(),
                                 daemonpayload::tlsServerCertPath(),
                                 daemonpayload::tlsServerKeyPath(),
                                 daemonpayload::tlsClientCertPath(),
                                 daemonpayload::tlsClientKeyPath());
        }
        remoteCmd = remoteStdinPayload.isEmpty() ? withSudo(p, remoteCmd)
                                                 : withSudoStreamInput(p, remoteCmd);

    }

    updateStatus(trk(QStringLiteral("t_daemon_installing_001"),
                     QStringLiteral("Instalando/Actualizando daemon..."),
                     QStringLiteral("Installing/Updating daemon..."),
                     QStringLiteral("正在安装/更新守护进程...")));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    QString detail;
    const bool ok = executeConnectionCommand(idx,
                                             QStringLiteral("Instalar/Actualizar daemon"),
                                             remoteCmd,
                                             180000,
                                             &detail,
                                             remoteStdinPayload);
    if (!ok) {
        if (interactive) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_daemon_install_fail_001"),
                    QStringLiteral("No se pudo instalar/actualizar el daemon en \"%1\".\n\n%2"),
                    QStringLiteral("Could not install/update daemon on \"%1\".\n\n%2"),
                    QStringLiteral("无法在 \"%1\" 上安装/更新守护进程。\n\n%2"))
                    .arg(p.name, detail.simplified().left(500)));
        } else {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Actualización automática de daemon fallida en \"%1\": %2")
                       .arg(p.name, detail.simplified().left(500)));
        }
        refreshConnectionByIndex(idx);
        return false;
    }
    // Daemon was restarted — drop the stale RPC tunnel, clear the backoff,
    // and evict the in-memory TLS cache before fetching new certs.
    clearDaemonRpcStateForConnection(p);
    {
        QString tlsCacheErr;
        if (!cacheDaemonTlsMaterialForConnection(p, &tlsCacheErr)) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("No se pudo cachear TLS daemon para \"%1\": %2")
                       .arg(p.name, tlsCacheErr.simplified()));
        } else {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("TLS daemon cacheado en config para \"%1\"").arg(p.name));
        }
    }
    // Clear only the backoff (not the TLS cache) a second time: a concurrent
    // refresh thread in-flight during the window between the first
    // clearDaemonRpcStateForConnection and cacheDaemonTlsMaterialForConnection
    // may have found the cache empty, failed, and set a fresh 30-second backoff.
    // That would suppress the first RPC from the refresh launched below.
    clearDaemonRpcBackoffForConnection(p);
    refreshConnectionByIndex(idx);
    if (isMac && interactive) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_daemon_macos_full_disk_access_001"),
                QStringLiteral("Daemon instalado en \"%1\".\n\n"
                               "Para que el daemon pueda detectar pools disponibles para importar, "
                               "concédele acceso completo al disco en el Mac remoto:\n\n"
                               "Configuración del Sistema → Privacidad y Seguridad → "
                               "Acceso completo al disco → zfsmgr-agent"),
                QStringLiteral("Daemon installed on \"%1\".\n\n"
                               "To allow the daemon to detect pools available for import, "
                               "grant it Full Disk Access on the remote Mac:\n\n"
                               "System Settings → Privacy & Security → "
                               "Full Disk Access → zfsmgr-agent"),
                QStringLiteral("守护进程已安装在 \"%1\" 上。\n\n"
                               "为使守护进程能检测可导入的存储池，请在远程 Mac 上授予完整磁盘访问权限：\n\n"
                               "系统设置 → 隐私与安全性 → 完整磁盘访问权限 → zfsmgr-agent"))
                .arg(p.name));
    }
    return true;
}


void MainWindow::deleteConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: borrado bloqueado"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    if (isLocalConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_local_builtin_02"),
                QStringLiteral("La conexión local integrada no se puede borrar."),
                QStringLiteral("Built-in local connection cannot be deleted."),
                QStringLiteral("内置本地连接不可删除。")));
        return;
    }
    const bool redirectedLocal = isConnectionRedirectedToLocal(idx);
    if (redirectedLocal) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_redirect_l3"),
                QStringLiteral("La conexión está redirigida a 'Local' y no se puede borrar."),
                QStringLiteral("This connection is redirected to 'Local' and cannot be deleted."),
                QStringLiteral("该连接已重定向到“本地”，不可删除。")));
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        trk(QStringLiteral("t_del_conn_tit1"), QStringLiteral("Borrar conexión"), QStringLiteral("Delete connection"), QStringLiteral("删除连接")),
        trk(QStringLiteral("t_del_conn_q001"), QStringLiteral("¿Borrar conexión \"%1\"?"),
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
                              trk(QStringLiteral("t_del_conn_err1"),
                                  QStringLiteral("No se pudo borrar conexión:\n%1"),
                                  QStringLiteral("Could not delete connection:\n%1"),
                                  QStringLiteral("无法删除连接：\n%1")).arg(err));
        return;
    }
    // Invalidate in-flight async refresh callbacks that may still be reporting
    // using stale indices while the profile list is being rebuilt.
    ++m_refreshGeneration;
    m_refreshPending = 0;
    m_refreshTotal = 0;
    m_refreshInProgress = false;
    updateConnectivityMatrixButtonState();
    updateBusyCursor();
    loadConnections();
}

void MainWindow::authorizePublicKeyOnConnection(int srcIdx, int dstIdx)
{
    if (srcIdx < 0 || srcIdx >= m_profiles.size() || dstIdx < 0 || dstIdx >= m_profiles.size()) {
        return;
    }
    const ConnectionProfile sp = m_profiles[srcIdx];
    const ConnectionProfile dp = m_profiles[dstIdx];

    beginUiBusy();
    struct BusyGuard { MainWindow* w; ~BusyGuard() { w->endUiBusy(); } } g{this};

    // 1. Get the source machine's default SSH public key.
    const QString getPubKeyCmd = withSudo(
        sp,
        mwhelpers::withUnixSearchPathCommand(
            QStringLiteral("cat ~/.ssh/id_ed25519.pub 2>/dev/null "
                           "|| cat ~/.ssh/id_rsa.pub 2>/dev/null "
                           "|| cat ~/.ssh/id_ecdsa.pub 2>/dev/null "
                           "|| cat ~/.ssh/id_*.pub 2>/dev/null | head -1")));
    QString pubKey, err;
    int rc = -1;
    if (!runSsh(sp, getPubKeyCmd, 12000, pubKey, err, rc) || rc != 0 || pubKey.trimmed().isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_auth_key_no_pubkey_001"),
                QStringLiteral("No se encontró clave pública SSH en \"%1\".\n\n"
                               "Genera una con: ssh-keygen -t ed25519"),
                QStringLiteral("No SSH public key found on \"%1\".\n\n"
                               "Generate one with: ssh-keygen -t ed25519"),
                QStringLiteral("在 \"%1\" 上未找到 SSH 公钥。\n\n"
                               "请使用以下命令生成：ssh-keygen -t ed25519"))
                .arg(sp.name));
        return;
    }
    pubKey = pubKey.trimmed().split('\n').first().trimmed();

    // 2. Append the public key to the destination's authorized_keys (idempotent).
    const QString escapedKey = mwhelpers::shSingleQuote(pubKey);
    const QString installCmd = withSudo(
        dp,
        mwhelpers::withUnixSearchPathCommand(
            QStringLiteral("mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
                           "(grep -qF %1 ~/.ssh/authorized_keys 2>/dev/null "
                           " || printf '\\n%2\\n' >> ~/.ssh/authorized_keys) && "
                           "chmod 600 ~/.ssh/authorized_keys")
                .arg(escapedKey, pubKey)));
    QString out2, err2;
    int rc2 = -1;
    if (!runSsh(dp, installCmd, 12000, out2, err2, rc2) || rc2 != 0) {
        const QString detail = (err2.trimmed().isEmpty() ? out2 : err2).simplified().left(400);
        QMessageBox::warning(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_auth_key_install_fail_001"),
                QStringLiteral("No se pudo instalar la clave en \"%1\":\n\n%2"),
                QStringLiteral("Could not install the key on \"%1\":\n\n%2"),
                QStringLiteral("无法在 \"%1\" 上安装密钥：\n\n%2"))
                .arg(dp.name, detail));
        return;
    }

    QMessageBox::information(
        this, QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_auth_key_ok_001"),
            QStringLiteral("Clave pública de \"%1\" instalada en \"%2\".\n\n"
                           "Las transferencias directas entre estas conexiones "
                           "ya no necesitarán contraseña."),
            QStringLiteral("Public key from \"%1\" installed on \"%2\".\n\n"
                           "Direct transfers between these connections "
                           "will no longer require a password."),
            QStringLiteral("已将 \"%1\" 的公钥安装到 \"%2\"。\n\n"
                           "这两个连接之间的直接传输将不再需要密码。"))
            .arg(sp.name, dp.name));
}
