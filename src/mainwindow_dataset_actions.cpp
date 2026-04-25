#include "mainwindow.h"
#include "agentversion.h"
#include "mainwindow_helpers.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::oneLine;
using mwhelpers::parentDatasetName;
using mwhelpers::shSingleQuote;

QString normalizeHostTokenLocal(QString host) {
    host = host.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']') && host.size() > 2) {
        host = host.mid(1, host.size() - 2);
    }
    while (host.endsWith('.')) {
        host.chop(1);
    }
    return host;
}

bool isLocalHostTokenLocal(const QString& host) {
    const QString h = normalizeHostTokenLocal(host);
    if (h.isEmpty()) {
        return false;
    }
    if (h == QStringLiteral("localhost") || h == QStringLiteral("127.0.0.1") || h == QStringLiteral("::1")) {
        return true;
    }
    static const QSet<QString> aliases = []() {
        QSet<QString> s;
        s.insert(QStringLiteral("localhost"));
        s.insert(QStringLiteral("127.0.0.1"));
        s.insert(QStringLiteral("::1"));
        const QString local = normalizeHostTokenLocal(QSysInfo::machineHostName());
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

QString currentLocalMachineUidFallback() {
#if defined(Q_OS_MACOS)
    QProcess proc;
    proc.start(QStringLiteral("sh"),
               QStringList{QStringLiteral("-lc"),
                           QStringLiteral("ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F\\\" '/IOPlatformUUID/{print $(NF-1); exit}'")});
    if (proc.waitForFinished(3000)) {
        const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) {
            return out;
        }
    }
#endif
    return QString::fromLatin1(QSysInfo::machineUniqueId().toHex()).trimmed();
}

QStringList datasetSizePropertyNames() {
    return {
        QStringLiteral("used"),
        QStringLiteral("available"),
        QStringLiteral("referenced"),
        QStringLiteral("logicalused"),
        QStringLiteral("logicalreferenced"),
        QStringLiteral("written"),
        QStringLiteral("quota"),
        QStringLiteral("refquota"),
        QStringLiteral("reservation"),
        QStringLiteral("refreservation"),
        QStringLiteral("volsize"),
    };
}

QStringList poolSizePropertyNames() {
    return {
        QStringLiteral("size"),
        QStringLiteral("alloc"),
        QStringLiteral("free"),
        QStringLiteral("cap"),
        QStringLiteral("fragmentation"),
    };
}

bool containsAnyToken(const QString& haystackLower, const QStringList& needles) {
    for (const QString& token : needles) {
        if (haystackLower.contains(token)) {
            return true;
        }
    }
    return false;
}

struct DaemonMutationPlan {
    bool matched{false};
    bool embedsStdin{false};
    QString daemonCmd;
};

bool isAllowedGenericZfsMutationOpClient(const QString& opRaw) {
    const QString op = opRaw.trimmed().toLower();
    static const QSet<QString> allowed = {
        QStringLiteral("create"),
        QStringLiteral("destroy"),
        QStringLiteral("rollback"),
        QStringLiteral("clone"),
        QStringLiteral("rename"),
        QStringLiteral("set"),
        QStringLiteral("inherit"),
        QStringLiteral("mount"),
        QStringLiteral("unmount"),
        QStringLiteral("hold"),
        QStringLiteral("release"),
        QStringLiteral("load-key"),
        QStringLiteral("unload-key"),
        QStringLiteral("change-key"),
        QStringLiteral("promote"),
        QStringLiteral("allow"),
        QStringLiteral("unallow"),
    };
    return allowed.contains(op);
}

DaemonMutationPlan daemonMutationPlanForCommand(const QString& rawCmd, const QByteArray& stdinPayload = {}) {
    DaemonMutationPlan plan;
    const QString trimmedRaw = rawCmd.trimmed();

    // Detect semicolon-joined sequences of zfs allow/unallow and route via
    // --mutate-shell-generic to keep them as a single atomic daemon call.
    if (trimmedRaw.contains(QStringLiteral("; "))) {
        const QStringList subCmds = trimmedRaw.split(QStringLiteral("; "), Qt::SkipEmptyParts);
        bool allAllowUnallow = !subCmds.isEmpty();
        for (const QString& sub : subCmds) {
            const QStringList subParts = QProcess::splitCommand(sub.trimmed());
            if (subParts.size() < 2 || subParts.at(0) != QStringLiteral("zfs")) {
                allAllowUnallow = false;
                break;
            }
            const QString subOp = subParts.at(1).trimmed().toLower();
            if (subOp != QStringLiteral("allow") && subOp != QStringLiteral("unallow")) {
                allAllowUnallow = false;
                break;
            }
        }
        if (allAllowUnallow) {
            const QString payloadB64 =
                QString::fromLatin1(trimmedRaw.toUtf8().toBase64());
            plan.matched = true;
            plan.daemonCmd =
                QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-shell-generic %1")
                    .arg(shSingleQuote(payloadB64));
            return plan;
        }
    }

    const QStringList parts = QProcess::splitCommand(trimmedRaw);
    if (parts.size() < 3) {
        return plan;
    }
    if (parts.at(0) != QStringLiteral("zfs")) {
        return plan;
    }
    const QString op = parts.at(1).trimmed().toLower();
    auto lastTarget = [&]() -> QString {
        for (int i = parts.size() - 1; i >= 2; --i) {
            const QString token = parts.at(i).trimmed();
            if (token.isEmpty() || token.startsWith(QLatin1Char('-'))) {
                continue;
            }
            return token;
        }
        return QString();
    };

    if (op == QStringLiteral("snapshot")) {
        const QString target = lastTarget().trimmed();
        if (target.isEmpty() || !target.contains(QLatin1Char('@'))) {
            return plan;
        }
        bool recursive = false;
        for (int i = 2; i < parts.size(); ++i) {
            const QString token = parts.at(i).trimmed();
            if (token == QStringLiteral("-r")) {
                recursive = true;
            }
        }
        plan.matched = true;
        plan.daemonCmd = QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zfs-snapshot %1 %2")
                             .arg(shSingleQuote(target),
                                  recursive ? QStringLiteral("1") : QStringLiteral("0"));
        return plan;
    }

    if (op == QStringLiteral("destroy")) {
        const QString target = lastTarget().trimmed();
        if (target.isEmpty() || !target.contains(QLatin1Char('@'))) {
            return plan;
        }
        bool force = false;
        QString recursiveMode = QStringLiteral("none");
        for (int i = 2; i < parts.size(); ++i) {
            const QString token = parts.at(i).trimmed();
            if (token == QStringLiteral("-f")) {
                force = true;
            } else if (token == QStringLiteral("-R")) {
                recursiveMode = QStringLiteral("R");
            } else if (token == QStringLiteral("-r") && recursiveMode != QStringLiteral("R")) {
                recursiveMode = QStringLiteral("r");
            }
        }
        plan.matched = true;
        plan.daemonCmd = QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zfs-destroy %1 %2 %3")
                             .arg(shSingleQuote(target),
                                  force ? QStringLiteral("1") : QStringLiteral("0"),
                                  shSingleQuote(recursiveMode));
        return plan;
    }

    if (op == QStringLiteral("rollback")) {
        const QString target = lastTarget().trimmed();
        if (target.isEmpty() || !target.contains(QLatin1Char('@'))) {
            return plan;
        }
        bool force = false;
        QString recursiveMode = QStringLiteral("none");
        for (int i = 2; i < parts.size(); ++i) {
            const QString token = parts.at(i).trimmed();
            if (token == QStringLiteral("-f")) {
                force = true;
            } else if (token == QStringLiteral("-R")) {
                recursiveMode = QStringLiteral("R");
            } else if (token == QStringLiteral("-r") && recursiveMode != QStringLiteral("R")) {
                recursiveMode = QStringLiteral("r");
            }
        }
        plan.matched = true;
        plan.daemonCmd = QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zfs-rollback %1 %2 %3")
                             .arg(shSingleQuote(target),
                                  force ? QStringLiteral("1") : QStringLiteral("0"),
                                  shSingleQuote(recursiveMode));
        return plan;
    }

    // load-key with passphrase in stdin: route via --mutate-zfs-load-key
    if (op == QStringLiteral("load-key") && !stdinPayload.isEmpty()) {
        const QString target = lastTarget().trimmed();
        if (!target.isEmpty()) {
            const QString datasetB64  = QString::fromLatin1(target.toUtf8().toBase64());
            const QString passphraseB64 = QString::fromLatin1(stdinPayload.toBase64());
            plan.matched = true;
            plan.embedsStdin = true;
            plan.daemonCmd = QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zfs-load-key %1 %2")
                                 .arg(shSingleQuote(datasetB64), shSingleQuote(passphraseB64));
            return plan;
        }
    }

    // change-key with passphrase in stdin: route via --mutate-zfs-change-key
    if (op == QStringLiteral("change-key") && !stdinPayload.isEmpty()) {
        const QString target = lastTarget().trimmed();
        if (!target.isEmpty()) {
            QStringList flagTokens;
            for (int i = 2; i < parts.size() - 1; ++i) {
                flagTokens << parts.at(i);
            }
            const QString datasetB64  = QString::fromLatin1(target.toUtf8().toBase64());
            const QString passphraseB64 = QString::fromLatin1(stdinPayload.toBase64());
            const QString flagsB64 = QString::fromLatin1(flagTokens.join(QLatin1Char(' ')).toUtf8().toBase64());
            plan.matched = true;
            plan.embedsStdin = true;
            plan.daemonCmd = QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zfs-change-key %1 %2 %3")
                                 .arg(shSingleQuote(datasetB64), shSingleQuote(passphraseB64), shSingleQuote(flagsB64));
            return plan;
        }
    }

    if (isAllowedGenericZfsMutationOpClient(op)) {
        QJsonArray arr;
        for (int i = 1; i < parts.size(); ++i) {
            arr.push_back(parts.at(i));
        }
        if (arr.isEmpty()) {
            return plan;
        }
        const QString payloadB64 = QString::fromUtf8(
            QJsonDocument(arr).toJson(QJsonDocument::Compact).toBase64());
        plan.matched = true;
        plan.daemonCmd = QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zfs-generic %1")
                             .arg(shSingleQuote(payloadB64));
        return plan;
    }

    return plan;
}
} // namespace

bool MainWindow::ensureLocalSudoCredentials(ConnectionProfile& profile) {
    if (!isLocalConnection(profile) || isWindowsConnection(profile) || !profile.useSudo) {
        return true;
    }

    QString localUid = m_localMachineUuid.trimmed();
    if (localUid.isEmpty()) {
        localUid = currentLocalMachineUidFallback();
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        if (isLocalConnection(i) || i >= m_states.size()) {
            continue;
        }
        const ConnectionProfile& candidate = m_profiles[i];
        if (!isConnectionRedirectedToLocal(i)) {
            continue;
        }
        if (candidate.username.trimmed().isEmpty() || candidate.password.isEmpty()) {
            continue;
        }
        profile.username = candidate.username;
        profile.password = candidate.password;
        m_localSudoUsername = candidate.username;
        m_localSudoPassword = candidate.password;
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Credenciales sudo locales tomadas de conexión redirigida: %1").arg(candidate.name));
        return true;
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile& candidate = m_profiles[i];
        if (!isLocalConnection(candidate)) {
            continue;
        }
        const QString candUid = candidate.machineUid.trimmed();
        if (!localUid.isEmpty() && !candUid.isEmpty() && candUid.compare(localUid, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (candidate.username.trimmed().isEmpty() || candidate.password.isEmpty()) {
            continue;
        }
        profile.username = candidate.username;
        profile.password = candidate.password;
        m_localSudoUsername = candidate.username;
        m_localSudoPassword = candidate.password;
        appLog(QStringLiteral("INFO"), QStringLiteral("Credenciales sudo locales tomadas de config.json (Local)"));
        return true;
    }

    bool hasConfiguredRedirect = false;
    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile& candidate = m_profiles[i];
        if (isLocalConnection(candidate)) {
            continue;
        }
        const bool byUid = !localUid.isEmpty() && !candidate.machineUid.trimmed().isEmpty()
                           && candidate.machineUid.trimmed().compare(localUid, Qt::CaseInsensitive) == 0;
        const bool byHost = isLocalHostTokenLocal(candidate.host);
        if (!byUid && !byHost) {
            continue;
        }
        hasConfiguredRedirect = true;
        if (!candidate.username.trimmed().isEmpty() && !candidate.password.isEmpty()) {
            profile.username = candidate.username;
            profile.password = candidate.password;
            m_localSudoUsername = candidate.username;
            m_localSudoPassword = candidate.password;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Credenciales sudo locales tomadas de redirección configurada: %1").arg(candidate.name));
            return true;
        }
    }

    if (hasConfiguredRedirect) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_local_sudo_cfg1"),
                QStringLiteral("Hay una conexión redirigida a Local en config.json, pero no tiene usuario/password.\n"
                               "Edite esa conexión o complete sus credenciales."),
                QStringLiteral("There is a connection redirected to Local in config.json, but it has no user/password.\n"
                               "Edit that connection or complete its credentials."),
                QStringLiteral("config.json 中存在重定向到本机的连接，但缺少用户/密码。\n"
                               "请编辑该连接并补全凭据。")));
        appLog(QStringLiteral("WARN"), QStringLiteral("Credenciales sudo locales no disponibles: redirección configurada sin credenciales"));
        return false;
    }

    if (!m_localSudoUsername.trimmed().isEmpty() && !m_localSudoPassword.isEmpty()) {
        profile.username = m_localSudoUsername;
        profile.password = m_localSudoPassword;
        return true;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(trk(QStringLiteral("t_local_sudo_dlg1"),
                           QStringLiteral("Credenciales sudo locales"),
                           QStringLiteral("Local sudo credentials"),
                           QStringLiteral("本地 sudo 凭据")));
    dlg.setModal(true);
    auto* form = new QFormLayout(&dlg);
    auto* userEdit = new QLineEdit(&dlg);
    auto* passEdit = new QLineEdit(&dlg);
    passEdit->setEchoMode(QLineEdit::Password);
    const QString envUser = qEnvironmentVariable("USER").trimmed();
    const QString envUserWin = qEnvironmentVariable("USERNAME").trimmed();
    userEdit->setText(!envUser.isEmpty() ? envUser : envUserWin);
    form->addRow(trk(QStringLiteral("t_usuario_d31f58"),
                     QStringLiteral("Usuario"),
                     QStringLiteral("User"),
                     QStringLiteral("用户")),
                 userEdit);
    form->addRow(trk(QStringLiteral("t_password_8be3c9"),
                     QStringLiteral("Password"),
                     QStringLiteral("Password"),
                     QStringLiteral("密码")),
                 passEdit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    if (userEdit->text().trimmed().isEmpty() || passEdit->text().isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_local_sudo_req1"),
                                 QStringLiteral("Usuario y password sudo son obligatorios."),
                                 QStringLiteral("Sudo user and password are required."),
                                 QStringLiteral("必须提供 sudo 用户和密码。")));
        return false;
    }
    m_localSudoUsername = userEdit->text().trimmed();
    m_localSudoPassword = passEdit->text();
    profile.username = m_localSudoUsername;
    profile.password = m_localSudoPassword;
    appLog(QStringLiteral("INFO"), QStringLiteral("Credenciales sudo locales guardadas en memoria"));

    ConnectionProfile localCfg;
    bool foundLocal = false;
    for (const ConnectionProfile& p : m_profiles) {
        if (!isLocalConnection(p)) {
            continue;
        }
        localCfg = p;
        foundLocal = true;
        break;
    }
    if (!foundLocal) {
        localCfg.id = QStringLiteral("local");
        localCfg.name = QStringLiteral("Local");
        localCfg.connType = QStringLiteral("LOCAL");
        if (!profile.osType.isEmpty()) {
            localCfg.osType = profile.osType;
        } else {
#ifdef Q_OS_WIN
            localCfg.osType = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
            localCfg.osType = QStringLiteral("macOS");
#elif defined(Q_OS_FREEBSD)
            localCfg.osType = QStringLiteral("FreeBSD");
#else
            localCfg.osType = QStringLiteral("Linux");
#endif
        }
        localCfg.host = QStringLiteral("localhost");
        localCfg.port = 0;
        localCfg.useSudo = true;
    }
    localCfg.machineUid = localUid;
    localCfg.username = m_localSudoUsername;
    localCfg.password = m_localSudoPassword;
    if (!localCfg.useSudo) {
        localCfg.useSudo = true;
    }
    QString storeErr;
    if (!m_store.upsertConnection(localCfg, storeErr)) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("No se pudieron persistir credenciales sudo locales en config.json: %1").arg(oneLine(storeErr)));
    } else {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Credenciales sudo locales persistidas en config.json para machine_uid=%1")
                   .arg(localUid.isEmpty() ? QStringLiteral("-") : localUid));
        bool updated = false;
        for (ConnectionProfile& p : m_profiles) {
            if (!isLocalConnection(p)) {
                continue;
            }
            p.machineUid = localCfg.machineUid;
            p.username = localCfg.username;
            p.password = localCfg.password;
            p.useSudo = localCfg.useSudo;
            updated = true;
            break;
        }
        if (!updated) {
            m_profiles.push_front(localCfg);
        }
    }
    return true;
}

bool MainWindow::hasEquivalentLocalSshConnection() const {
    QString localUid = m_localMachineUuid.trimmed();
    if (localUid.isEmpty()) {
        localUid = currentLocalMachineUidFallback();
    }

    for (const ConnectionProfile& candidate : m_profiles) {
        if (isLocalConnection(candidate)) {
            continue;
        }
        if (candidate.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString candUid = candidate.machineUid.trimmed();
        if (!localUid.isEmpty() && !candUid.isEmpty() && candUid.compare(localUid, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (isLocalHostTokenLocal(candidate.host)) {
            return true;
        }
    }
    return false;
}

void MainWindow::ensureStartupLocalSudoConnection() {
    if (m_startupLocalSudoChecked) {
        return;
    }
    m_startupLocalSudoChecked = true;

    for (int i = 0; i < m_profiles.size(); ++i) {
        ConnectionProfile& localProfile = m_profiles[i];
        if (!isLocalConnection(localProfile) || isWindowsConnection(localProfile) || !localProfile.useSudo) {
            continue;
        }
        if (hasEquivalentLocalSshConnection()) {
            return;
        }
        if (ensureLocalSudoCredentials(localProfile)) {
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Credenciales sudo locales comprobadas al arrancar"));
        }
        return;
    }
}


bool MainWindow::executeDatasetAction(const QString& side,
                                      const QString& actionName,
                                      const DatasetSelectionContext& ctx,
                                      const QString& cmd,
                                      int timeoutMs,
                                      bool allowWindowsScript,
                                      const QByteArray& stdinPayload,
                                      bool invalidatePoolCache,
                                      const std::function<void()>& onSuccessRefresh) {
    if (!ctx.valid) {
        return false;
    }
    if (isPoolSuspended(ctx.connIdx, ctx.poolName)) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("Bloqueada acción %1 en %2::%3 (pool suspended)")
                   .arg(actionName, m_profiles.value(ctx.connIdx).name, ctx.poolName));
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_pool_suspended_block_001"),
                QStringLiteral("El pool está en estado suspended. No se permiten operaciones."),
                QStringLiteral("Pool is suspended. Operations are disabled."),
                QStringLiteral("存储池处于 suspended 状态，已禁用操作。")));
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
                trk(QStringLiteral("t_win_unix_na01"), QStringLiteral("La acción \"%1\" usa shell Unix y no está disponible en conexiones Windows por ahora.")
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
    ConnectionProfile sudoProfile = p;
    if (!ensureLocalSudoCredentials(sudoProfile)) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1 cancelada: faltan credenciales sudo locales").arg(actionName));
        return false;
    }
    const bool daemonMutateApiOk =
        !isWindowsConnection(p)
        && ctx.connIdx >= 0
        && ctx.connIdx < m_states.size()
        && m_states[ctx.connIdx].daemonInstalled
        && m_states[ctx.connIdx].daemonActive
        && m_states[ctx.connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    const bool usesStreamInput = !stdinPayload.isEmpty();
    QString effectiveCmd =
        isWindowsConnection(p) ? cmd : mwhelpers::withUnixSearchPathCommand(cmd);
    bool embeddedStdin = false;
    if (daemonMutateApiOk) {
        const DaemonMutationPlan mutatePlan = daemonMutationPlanForCommand(cmd, stdinPayload);
        if (mutatePlan.matched && !mutatePlan.daemonCmd.trimmed().isEmpty()) {
            effectiveCmd = mwhelpers::withUnixSearchPathCommand(mutatePlan.daemonCmd);
            embeddedStdin = mutatePlan.embedsStdin;
        }
    }
    const bool effectivelyStreamInput = usesStreamInput && !embeddedStdin;
    QString remoteCmd = effectivelyStreamInput ? withSudoStreamInput(sudoProfile, effectiveCmd)
                                               : withSudo(sudoProfile, effectiveCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, remoteCmd));
    if (!confirmActionExecution(actionName, {preview})) {
        return false;
    }
    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 %2::%3").arg(actionName, p.name, ctx.datasetName));
    updateStatus(QStringLiteral("%1 %2::%3").arg(actionName, p.name, ctx.datasetName));
    QString out;
    QString err;
    int rc = -1;
    const bool isBreakdownAction = (actionName == QStringLiteral("Desglosar"));
    const bool isAssembleAction = (actionName == QStringLiteral("Ensamblar"));
    const bool isFromDirAction = (actionName == QStringLiteral("Desde Dir"));
    const bool isToDirAction = (actionName == QStringLiteral("Hacia Dir")
                                || actionName == QStringLiteral("Hasta Dir"));
    const bool isDeleteAllSnapsAction = (actionName == QStringLiteral("Borrar todos los snapshots"));
    bool loggedProgressRealtime = false;
    auto progressLogger = [this, &loggedProgressRealtime](const QString& rawLine) {
        const QString ln = rawLine.trimmed();
        const bool isRsyncProgress =
            ln.contains(QStringLiteral("to-chk="), Qt::CaseInsensitive)
            || ln.contains(QStringLiteral("xfr#"), Qt::CaseInsensitive)
            || (ln.contains(QLatin1Char('%')) && ln.contains(QStringLiteral("/s"), Qt::CaseInsensitive));
        if (ln.contains(QStringLiteral("[BREAKDOWN]"), Qt::CaseInsensitive)
            || ln.contains(QStringLiteral("[ASSEMBLE]"), Qt::CaseInsensitive)
            || ln.contains(QStringLiteral("[DELALLSNAP]"), Qt::CaseInsensitive)
            || ln.contains(QStringLiteral("[FROMDIR]"), Qt::CaseInsensitive)
            || ln.contains(QStringLiteral("[TODIR]"), Qt::CaseInsensitive)
            || isRsyncProgress) {
            loggedProgressRealtime = true;
            appLog(QStringLiteral("NORMAL"), ln);
        }
    };
    const bool withRealtimeProgress =
        (isBreakdownAction || isAssembleAction || isDeleteAllSnapsAction || isFromDirAction || isToDirAction);
    const QByteArray effectiveStdin = embeddedStdin ? QByteArray{} : stdinPayload;
    const bool ok = withRealtimeProgress
                        ? runSsh(p, remoteCmd, timeoutMs, out, err, rc, progressLogger, progressLogger, {}, WindowsCommandMode::Auto, effectiveStdin)
                        : runSsh(p, remoteCmd, timeoutMs, out, err, rc, {}, {}, {}, WindowsCommandMode::Auto, effectiveStdin);
    if (!ok || rc != 0) {
        if (isBreakdownAction || isAssembleAction || isDeleteAllSnapsAction || isFromDirAction || isToDirAction) {
            if (!loggedProgressRealtime) {
                const QStringList progressLines = out.split('\n', Qt::SkipEmptyParts);
                for (const QString& lnRaw : progressLines) {
                    const QString ln = lnRaw.trimmed();
                    const bool isRsyncProgress =
                        ln.contains(QStringLiteral("to-chk="), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("xfr#"), Qt::CaseInsensitive)
                        || (ln.contains(QLatin1Char('%')) && ln.contains(QStringLiteral("/s"), Qt::CaseInsensitive));
                    if (ln.contains(QStringLiteral("[BREAKDOWN]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[ASSEMBLE]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[DELALLSNAP]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[FROMDIR]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[TODIR]"), Qt::CaseInsensitive)
                        || isRsyncProgress) {
                        appLog(QStringLiteral("NORMAL"), ln);
                    }
                }
            }
        }
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
        updateStatus(QStringLiteral("%1 (ERROR) %2::%3").arg(actionName, p.name, ctx.datasetName));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_action_fail001"), QStringLiteral("%1 falló:\n%2"),
                                  QStringLiteral("%1 failed:\n%2"),
                                  QStringLiteral("%1 失败：\n%2"))
                                  .arg(actionName, failureDetail));
        setActionsLocked(false);
        return false;
    }
    if (!out.trimmed().isEmpty()) {
        if (isBreakdownAction || isAssembleAction || isDeleteAllSnapsAction || isFromDirAction || isToDirAction) {
            bool loggedProgress = loggedProgressRealtime;
            if (!loggedProgressRealtime) {
                const QStringList progressLines = out.split('\n', Qt::SkipEmptyParts);
                for (const QString& lnRaw : progressLines) {
                    const QString ln = lnRaw.trimmed();
                    const bool isRsyncProgress =
                        ln.contains(QStringLiteral("to-chk="), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("xfr#"), Qt::CaseInsensitive)
                        || (ln.contains(QLatin1Char('%')) && ln.contains(QStringLiteral("/s"), Qt::CaseInsensitive));
                    if (ln.contains(QStringLiteral("[BREAKDOWN]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[ASSEMBLE]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[DELALLSNAP]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[FROMDIR]"), Qt::CaseInsensitive)
                        || ln.contains(QStringLiteral("[TODIR]"), Qt::CaseInsensitive)
                        || isRsyncProgress) {
                        appLog(QStringLiteral("NORMAL"), ln);
                        loggedProgress = true;
                    }
                }
            }
            if (!loggedProgress) {
                appLog(QStringLiteral("INFO"), oneLine(out));
            }
        } else {
            appLog(QStringLiteral("INFO"), oneLine(out));
        }
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 finalizado").arg(actionName));
    updateStatus(QStringLiteral("%1 finalizado %2::%3").arg(actionName, p.name, ctx.datasetName));
    const bool subtreeTopologyAction = (isBreakdownAction || isAssembleAction);
    if (subtreeTopologyAction) {
        invalidateDatasetSubtreeCacheEntries(ctx.connIdx, ctx.poolName, ctx.datasetName, true);
        invalidatePoolDatasetListingCache(ctx.connIdx, ctx.poolName);
    }
    if (invalidatePoolCache) {
        invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
    }
    if (actionName == QStringLiteral("Borrar") && !ctx.snapshotName.isEmpty()) {
        if (side == QStringLiteral("origin") && m_connActionOrigin.valid
            && m_connActionOrigin.connIdx == ctx.connIdx
            && m_connActionOrigin.poolName == ctx.poolName
            && m_connActionOrigin.datasetName == ctx.datasetName
            && m_connActionOrigin.snapshotName == ctx.snapshotName) {
            m_connActionOrigin.snapshotName.clear();
        } else if (side == QStringLiteral("dest") && m_connActionDest.valid
                   && m_connActionDest.connIdx == ctx.connIdx
                   && m_connActionDest.poolName == ctx.poolName
                   && m_connActionDest.datasetName == ctx.datasetName
                   && m_connActionDest.snapshotName == ctx.snapshotName) {
            m_connActionDest.snapshotName.clear();
        } else if (side == QStringLiteral("conncontent")) {
            const auto clearDeletedSnapshotSelection = [&](QTreeWidget* tree) {
                if (!tree) {
                    return;
                }
                const auto selected = tree->selectedItems();
                if (selected.isEmpty()) {
                    return;
                }
                QTreeWidgetItem* item = selected.first();
                if (item->data(0, Qt::UserRole).toString().trimmed() == ctx.datasetName
                    && item->data(1, Qt::UserRole).toString().trimmed() == ctx.snapshotName) {
                    item->setData(1, Qt::UserRole, QString());
                }
            };
            clearDeletedSnapshotSelection(m_connContentTree);
        }
    }
    const bool needsDeferredRefresh =
        (actionName == QStringLiteral("Montar")
         || actionName == QStringLiteral("Montar con todos los hijos")
         || actionName == QStringLiteral("Desmontar")
         || actionName == QStringLiteral("Desde Dir")
         || actionName == QStringLiteral("Hacia Dir")
         || actionName == QStringLiteral("Desglosar")
         || actionName == QStringLiteral("Ensamblar"));
    if (onSuccessRefresh) {
        onSuccessRefresh();
        setActionsLocked(false);
        return true;
    }
    if (needsDeferredRefresh) {
        if (side == QStringLiteral("conncontent")) {
            reloadConnContentPool(ctx.connIdx, ctx.poolName);
        } else {
            reloadDatasetSide(side);
        }
        if (shouldRefreshSizePropsForCommand(actionName, cmd)) {
            refreshDatasetAndPoolSizeProperties(ctx.connIdx, ctx.poolName, ctx.datasetName);
        }
        setActionsLocked(false);
        return true;
    }
    if (side == QStringLiteral("conncontent")) {
        reloadConnContentPool(ctx.connIdx, ctx.poolName);
    } else {
        reloadDatasetSide(side);
    }
    if (shouldRefreshSizePropsForCommand(actionName, cmd)) {
        refreshDatasetAndPoolSizeProperties(ctx.connIdx, ctx.poolName, ctx.datasetName);
    }
    setActionsLocked(false);
    return true;
}

bool MainWindow::shouldRefreshSizePropsForCommand(const QString& actionLabel, const QString& command) const {
    const QString label = actionLabel.trimmed().toLower();
    const QString cmd = command.trimmed().toLower();
    if (label.isEmpty() && cmd.isEmpty()) {
        return false;
    }

    if (containsAnyToken(label,
                         {QStringLiteral("copiar"),
                          QStringLiteral("copy"),
                          QStringLiteral("clonar"),
                          QStringLiteral("clone"),
                          QStringLiteral("rollback"),
                          QStringLiteral("borrar"),
                          QStringLiteral("destroy"),
                          QStringLiteral("desglosar"),
                          QStringLiteral("ensamblar"),
                          QStringLiteral("desde dir"),
                          QStringLiteral("hacia dir"),
                          QStringLiteral("from dir"),
                          QStringLiteral("to dir"),
                          QStringLiteral("sync"),
                          QStringLiteral("sincron"),
                          QStringLiteral("aplicar propiedades")})) {
        return true;
    }

    if (containsAnyToken(cmd,
                         {QStringLiteral("zfs recv"),
                          QStringLiteral("zfs receive"),
                          QStringLiteral("zfs clone"),
                          QStringLiteral("zfs rollback"),
                          QStringLiteral("zfs destroy"),
                          QStringLiteral("zfs create"),
                          QStringLiteral("rsync "),
                          QStringLiteral("tar --"),
                          QStringLiteral("zfs set quota="),
                          QStringLiteral("zfs set refquota="),
                          QStringLiteral("zfs set reservation="),
                          QStringLiteral("zfs set refreservation="),
                          QStringLiteral("zfs set volsize=")})) {
        return true;
    }
    return false;
}

bool MainWindow::refreshDatasetAndPoolSizeProperties(int connIdx,
                                                     const QString& poolName,
                                                     const QString& datasetName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedDataset = datasetName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty() || trimmedDataset.isEmpty()) {
        return false;
    }

    bool refreshedSomething = false;
    const QStringList dsProps = datasetSizePropertyNames();
    if (ensureDatasetPropertySubsetLoaded(connIdx, trimmedPool, trimmedDataset, dsProps)) {
        const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool);
        const QString key = QStringLiteral("%1|%2").arg(token, trimmedDataset);
        QMap<QString, QString> mergedValues = m_connContentPropValuesByObject.value(key);
        const QMap<QString, QString> sizeValues =
            datasetPropertyValuesForNames(connIdx, trimmedPool, trimmedDataset, dsProps);
        for (auto it = sizeValues.cbegin(); it != sizeValues.cend(); ++it) {
            mergedValues.insert(it.key(), it.value());
        }
        if (!mergedValues.isEmpty()) {
            updateConnContentPropertyValues(token, trimmedDataset, mergedValues);
        }
        if (m_connContentTree && m_topDetailConnIdx == connIdx) {
            syncConnContentPropertyColumnsFor(m_connContentTree, token);
            if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
                syncConnContentPropertyColumnsFor(m_bottomConnContentTree, token);
            }
        }
        refreshedSomething = true;
    }

    const ConnectionProfile profile = m_profiles.at(connIdx);
    QString out;
    QString err;
    int rc = -1;
    const QStringList poolProps = poolSizePropertyNames();
    const QString propsCsv = poolProps.join(QLatin1Char(','));
    const bool poolWin = isWindowsConnection(connIdx);
    const bool daemonReadApiOk =
        !poolWin
        && connIdx >= 0
        && connIdx < m_states.size()
        && m_states[connIdx].daemonInstalled
        && m_states[connIdx].daemonActive
        && m_states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    const QString poolCmdClassic = withSudo(
        profile,
        mwhelpers::withUnixSearchPathCommand(
            poolWin
                ? QStringLiteral("zpool get -H -o property,value,source %1 %2")
                      .arg(propsCsv, shSingleQuote(trimmedPool))
                : QStringLiteral("zpool get -j %1 %2")
                      .arg(propsCsv, shSingleQuote(trimmedPool))));
    const QString poolCmdDaemon = withSudo(
        profile, mwhelpers::withUnixSearchPathCommand(
                     QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zpool-get-all %1")
                         .arg(shSingleQuote(trimmedPool))));
    const QString selectedPoolCmd = daemonReadApiOk ? poolCmdDaemon : poolCmdClassic;
    bool poolPropsOk = runSsh(profile, selectedPoolCmd, 15000, out, err, rc) && rc == 0;
    if (poolPropsOk) {
        const QString cacheKey = poolDetailsCacheKey(connIdx, trimmedPool);
        PoolDetailsCacheEntry entry = m_poolDetailsCache.value(cacheKey);
        QMap<QString, QStringList> rowsByProp;
        for (const QStringList& row : std::as_const(entry.propsRows)) {
            if (row.size() >= 2) {
                rowsByProp.insert(row.value(0).trimmed().toLower(), row);
            }
        }

        if (poolWin) {
            const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QStringList parts = line.split('\t');
                if (parts.size() < 3) {
                    continue;
                }
                const QString prop = parts.value(0).trimmed();
                if (prop.isEmpty()) {
                    continue;
                }
                rowsByProp.insert(prop.toLower(),
                                  QStringList{prop, parts.value(1).trimmed(), parts.value(2).trimmed()});
            }
        } else {
            const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
            const QJsonObject poolObj = doc.object().value(QStringLiteral("pools")).toObject().value(trimmedPool).toObject();
            const QJsonObject properties = poolObj.value(QStringLiteral("properties")).toObject();
            for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
                const QJsonObject propObj = it.value().toObject();
                const QString value = propObj.value(QStringLiteral("value")).toString().trimmed();
                const QString source = propObj.value(QStringLiteral("source")).toObject().value(QStringLiteral("type")).toString().trimmed();
                rowsByProp.insert(it.key().trimmed().toLower(), QStringList{it.key(), value, source});
            }
        }

        QVector<QStringList> mergedRows;
        mergedRows.reserve(rowsByProp.size());
        for (auto it = rowsByProp.cbegin(); it != rowsByProp.cend(); ++it) {
            mergedRows.push_back(it.value());
        }
        entry.loaded = true;
        entry.propsRows = mergedRows;
        m_poolDetailsCache.insert(cacheKey, entry);

        if (PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool)) {
            poolInfo->runtime.detailsState = LoadState::Loaded;
            poolInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
            poolInfo->runtime.zpoolPropertyRows = mergedRows;
            poolInfo->runtime.zpoolProperties.clear();
            for (const QStringList& row : std::as_const(mergedRows)) {
                if (row.size() >= 2) {
                    poolInfo->runtime.zpoolProperties.insert(row.value(0).trimmed(), row.value(1));
                }
            }
        }
        rebuildConnInfoFor(connIdx);
        const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool);
        if (m_connContentTree && m_topDetailConnIdx == connIdx) {
            syncConnContentPoolColumnsFor(m_connContentTree, token);
            if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
                syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
            }
        }
        refreshedSomething = true;
    } else {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("No se pudieron refrescar propiedades de tamaño del pool %1::%2: %3")
                   .arg(m_profiles.at(connIdx).name, trimmedPool, oneLine(err.isEmpty() ? out : err)));
    }

    return refreshedSomething;
}

bool MainWindow::executePendingDatasetRenameDraft(const PendingDatasetRenameDraft& draft,
                                                  bool interactiveErrorDialog,
                                                  QString* failureDetailOut) {
    if (draft.connIdx < 0 || draft.connIdx >= m_profiles.size()
        || draft.poolName.trimmed().isEmpty()
        || draft.sourceName.trimmed().isEmpty()
        || draft.targetName.trimmed().isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid draft");
        }
        return false;
    }
    const ConnectionProfile& p = m_profiles.at(draft.connIdx);
    ConnectionProfile sudoProfile = p;
    if (!ensureLocalSudoCredentials(sudoProfile)) {
        const QString msg = QStringLiteral("faltan credenciales sudo locales");
        appLog(QStringLiteral("INFO"), QStringLiteral("Aplicar renombrado cancelado: %1").arg(msg));
        if (failureDetailOut) {
            *failureDetailOut = msg;
        }
        return false;
    }
    const QString renameRawCmd = pendingDatasetRenameCommand(draft);
    QString renameExecCmd = renameRawCmd;
    if (!isWindowsConnection(p)) {
        if (const QString daemonCmd = daemonizeZfsMutationCommand(draft.connIdx, renameRawCmd);
            !daemonCmd.isEmpty()) {
            renameExecCmd = daemonCmd;
        }
        renameExecCmd = mwhelpers::withUnixSearchPathCommand(renameExecCmd);
    }
    const QString remoteCmd = withSudo(sudoProfile, renameExecCmd);
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Aplicar renombrado %1::%2")
               .arg(p.name, draft.sourceName.trimmed()));
    updateStatus(QStringLiteral("Aplicar renombrado %1::%2").arg(p.name, draft.sourceName.trimmed()));
    QString failureDetail;
    if (!executeConnectionCommand(draft.connIdx,
                                  QStringLiteral("Aplicar renombrado"),
                                  remoteCmd,
                                  60000,
                                  &failureDetail)) {
        if (failureDetail.trimmed().isEmpty()) {
            failureDetail = QStringLiteral("exit 1");
        }
        if (failureDetailOut) {
            *failureDetailOut = failureDetail;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error en Aplicar renombrado: %1")
                   .arg(oneLine(failureDetail)));
        updateStatus(QStringLiteral("Aplicar renombrado (ERROR) %1::%2").arg(p.name, draft.sourceName.trimmed()));
        if (interactiveErrorDialog) {
            QMessageBox::critical(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_action_fail001"),
                    QStringLiteral("%1 falló:\n%2"),
                    QStringLiteral("%1 failed:\n%2"),
                    QStringLiteral("%1 失败：\n%2"))
                    .arg(QStringLiteral("Aplicar renombrado"), failureDetail));
        }
        return false;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Aplicar renombrado finalizado"));
    invalidatePoolDatasetListingCache(draft.connIdx, draft.poolName.trimmed());
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
        return trk(QStringLiteral("t_diag_mp_fail01"), QStringLiteral("No se pudo resolver el mountpoint para diagnóstico."),
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

    const QString effectiveDiagCmd =
        isWindowsConnection(p) ? diagCmd : mwhelpers::withUnixSearchPathCommand(diagCmd);
    if (!runSsh(p, withSudo(p, effectiveDiagCmd), 15000, out, err, rc)) {
        return trk(QStringLiteral("t_diag_run_fail1"), QStringLiteral("No se pudo ejecutar el diagnóstico remoto."),
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

void MainWindow::invalidateDatasetCacheEntry(int connIdx,
                                             const QString& poolName,
                                             const QString& objectName,
                                             bool invalidatePermissions) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return;
    }

    removeDatasetPropertyEntry(connIdx, trimmedPool, trimmedObject);
    if (invalidatePermissions) {
        removeDatasetPermissionsEntry(connIdx, trimmedPool, trimmedObject);
    }

    const QString uiKey = QStringLiteral("%1::%2|%3")
                              .arg(QString::number(connIdx),
                                   trimmedPool,
                                   trimmedObject);
    m_connContentPropValuesByObject.remove(uiKey);

    if (DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        dsInfo->runtime.errorText.clear();
    }
}

void MainWindow::invalidateDatasetSubtreeCacheEntries(int connIdx,
                                                      const QString& poolName,
                                                      const QString& datasetName,
                                                      bool invalidatePermissions) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedDataset = datasetName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedDataset.isEmpty()) {
        return;
    }

    QStringList objectsToInvalidate{trimmedDataset};
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool)) {
        const QString childPrefix = trimmedDataset + QStringLiteral("/");
        const QString snapshotPrefix = trimmedDataset + QStringLiteral("@");
        for (auto itDs = poolInfo->objectsByFullName.cbegin();
             itDs != poolInfo->objectsByFullName.cend();
             ++itDs) {
            const QString objectName = itDs.key().trimmed();
            if (objectName == trimmedDataset
                || objectName.startsWith(childPrefix)
                || objectName.startsWith(snapshotPrefix)) {
                objectsToInvalidate.push_back(objectName);
            }
        }
    }

    objectsToInvalidate.removeDuplicates();
    for (const QString& objectName : std::as_const(objectsToInvalidate)) {
        invalidateDatasetCacheEntry(connIdx, trimmedPool, objectName, invalidatePermissions);
    }
}

void MainWindow::invalidatePoolDatasetListingCache(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty()) {
        return;
    }
    m_poolDatasetCache.remove(datasetCacheKey(connIdx, trimmedPool));
    removeDatasetPropertyEntriesForPool(connIdx, trimmedPool);
    removeDatasetPermissionsEntriesForPool(connIdx, trimmedPool);
    const QString uiPrefix =
        QStringLiteral("%1::%2|").arg(QString::number(connIdx), trimmedPool);
    auto vit = m_connContentPropValuesByObject.begin();
    while (vit != m_connContentPropValuesByObject.end()) {
        if (vit.key().startsWith(uiPrefix)) {
            vit = m_connContentPropValuesByObject.erase(vit);
        } else {
            ++vit;
        }
    }
    rebuildConnInfoFor(connIdx);
}

void MainWindow::invalidateDatasetCacheForPool(int connIdx, const QString& poolName) {
    m_poolDatasetCache.remove(datasetCacheKey(connIdx, poolName));
    m_poolDetailsCache.remove(poolDetailsCacheKey(connIdx, poolName));
    invalidateDatasetPermissionsCacheForPool(connIdx, poolName);
    removeDatasetPropertyEntriesForPool(connIdx, poolName);
    const QString uiPrefix =
        QStringLiteral("%1::%2|").arg(QString::number(connIdx), poolName.trimmed());
    auto vit = m_connContentPropValuesByObject.begin();
    while (vit != m_connContentPropValuesByObject.end()) {
        if (vit.key().startsWith(uiPrefix)) {
            vit = m_connContentPropValuesByObject.erase(vit);
        } else {
            ++vit;
        }
    }
    rebuildConnInfoFor(connIdx);
}

void MainWindow::scheduleReloadFlush() {
    if (m_reloadFlushScheduled) {
        return;
    }
    m_reloadFlushScheduled = true;
    QTimer::singleShot(40, this, [this]() {
        flushPendingReloads();
    });
}

void MainWindow::flushPendingReloads() {
    m_reloadFlushScheduled = false;
    if (m_pendingConnContentPoolReloadKeys.isEmpty() && m_pendingConnectionRefreshIndices.isEmpty()) {
        return;
    }

    const QSet<QString> poolKeys = m_pendingConnContentPoolReloadKeys;
    const QSet<int> refreshIdxs = m_pendingConnectionRefreshIndices;
    m_pendingConnContentPoolReloadKeys.clear();
    m_pendingConnectionRefreshIndices.clear();

    for (const QString& key : poolKeys) {
        const int sep = key.indexOf(QStringLiteral("::"));
        if (sep <= 0) {
            continue;
        }
        bool ok = false;
        const int connIdx = key.left(sep).toInt(&ok);
        const QString poolName = key.mid(sep + 2).trimmed();
        if (!ok || poolName.isEmpty()) {
            continue;
        }
        reloadConnContentPoolNow(connIdx, poolName);
    }

    for (int connIdx : refreshIdxs) {
        if (connIdx < 0 || connIdx >= m_profiles.size()) {
            continue;
        }
        refreshConnectionByIndex(connIdx);
    }
}

void MainWindow::reloadConnContentPoolNow(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return;
    }

    constexpr int kConnIdxRoleLocal = Qt::UserRole + 10;
    constexpr int kPoolNameRoleLocal = Qt::UserRole + 11;
    constexpr int kIsPoolRootRoleLocal = Qt::UserRole + 12;
    const QString targetToken = QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool);

    auto tokenMatchesTarget = [&](QTreeWidget* tree) -> bool {
        if (!tree) {
            return false;
        }
        const QString token = connContentTokenForTree(tree).trimmed();
        if (token.isEmpty()) {
            return false;
        }
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep <= 0) {
            return false;
        }
        bool okConn = false;
        const int tokenConnIdx = token.left(sep).toInt(&okConn);
        const QString tokenPool = token.mid(sep + 2).trimmed();
        return okConn && tokenConnIdx == connIdx
               && tokenPool.compare(trimmedPool, Qt::CaseInsensitive) == 0;
    };

    auto treeHasTargetPool = [&](QTreeWidget* tree) -> bool {
        if (!tree) {
            return false;
        }
        std::function<bool(QTreeWidgetItem*)> recHasPool = [&](QTreeWidgetItem* node) -> bool {
            if (!node) {
                return false;
            }
            if (node->data(0, kIsPoolRootRoleLocal).toBool()
                && node->data(0, kConnIdxRoleLocal).toInt() == connIdx
                && node->data(0, kPoolNameRoleLocal).toString().trimmed().compare(
                       trimmedPool, Qt::CaseInsensitive) == 0) {
                return true;
            }
            for (int i = 0; i < node->childCount(); ++i) {
                if (recHasPool(node->child(i))) {
                    return true;
                }
            }
            return false;
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (recHasPool(tree->topLevelItem(i))) {
                return true;
            }
        }
        return false;
    };

    auto removeTargetPoolRoots = [&](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        std::function<void(QTreeWidgetItem*)> recPurge = [&](QTreeWidgetItem* parent) {
            if (!parent) {
                return;
            }
            for (int i = parent->childCount() - 1; i >= 0; --i) {
                QTreeWidgetItem* child = parent->child(i);
                if (!child) {
                    continue;
                }
                if (child->data(0, kIsPoolRootRoleLocal).toBool()
                    && child->data(0, kConnIdxRoleLocal).toInt() == connIdx
                    && child->data(0, kPoolNameRoleLocal).toString().trimmed().compare(
                           trimmedPool, Qt::CaseInsensitive) == 0) {
                    delete parent->takeChild(i);
                    continue;
                }
                recPurge(child);
            }
        };
        for (int i = tree->topLevelItemCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* top = tree->topLevelItem(i);
            if (!top) {
                continue;
            }
            if (top->data(0, kIsPoolRootRoleLocal).toBool()
                && top->data(0, kConnIdxRoleLocal).toInt() == connIdx
                && top->data(0, kPoolNameRoleLocal).toString().trimmed().compare(
                       trimmedPool, Qt::CaseInsensitive) == 0) {
                delete tree->takeTopLevelItem(i);
                continue;
            }
            recPurge(top);
        }
    };

    auto refreshTargetPoolInTree = [&](QTreeWidget* tree) -> bool {
        if (!tree || !(tokenMatchesTarget(tree) || treeHasTargetPool(tree))) {
            return false;
        }
        const QString stateToken = connContentTokenForTree(tree).trimmed();
        if (!stateToken.isEmpty()) {
            saveConnContentTreeStateFor(tree, stateToken);
        }
        removeTargetPoolRoots(tree);
        const bool groupedByConnectionRoots =
            tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool();
        const DatasetTreeContext ctx = groupedByConnectionRoots
                                           ? DatasetTreeContext::ConnectionContentMulti
                                           : DatasetTreeContext::ConnectionContent;
        const DatasetTreeRenderOptions opts = datasetTreeRenderOptionsForTree(tree, ctx);
        appendDatasetTreeForPool(tree, connIdx, trimmedPool, ctx, opts, true);
        if (groupedByConnectionRoots) {
            tree->expandToDepth(0);
        }
        syncConnContentPoolColumnsFor(tree, targetToken);
        if (!stateToken.isEmpty()) {
            restoreConnContentTreeStateFor(tree, stateToken);
        }
        return true;
    };

    beginUiBusy();
    bool refreshed = false;
    refreshed = refreshTargetPoolInTree(m_connContentTree) || refreshed;
    if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
        refreshed = refreshTargetPoolInTree(m_bottomConnContentTree) || refreshed;
    }
    for (const SplitTreeEntry& entry : std::as_const(m_splitTrees)) {
        QTreeWidget* tree = entry.treeWidget ? entry.treeWidget->tree() : nullptr;
        if (!tree || tree == m_connContentTree || tree == m_bottomConnContentTree) {
            continue;
        }
        refreshed = refreshTargetPoolInTree(tree) || refreshed;
    }
    if (!refreshed) {
        refreshConnectionByIndex(connIdx);
    }
    endUiBusy();
}

void MainWindow::reloadConnContentPool(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    m_pendingConnContentPoolReloadKeys.insert(QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool));
    scheduleReloadFlush();
}

void MainWindow::reloadDatasetSide(const QString& side) {
    if (side == QStringLiteral("origin") || side == QStringLiteral("dest")) {
        const DatasetSelectionContext ctx = currentDatasetSelection(side);
        if (ctx.valid && ctx.connIdx >= 0 && ctx.connIdx < m_profiles.size()) {
            m_pendingConnectionRefreshIndices.insert(ctx.connIdx);
            scheduleReloadFlush();
        }
        return;
    } else if (side == QStringLiteral("conncontent")) {
        auto reloadTree = [this](QTreeWidget* tree) -> bool {
            const QString token = connContentTokenForTree(tree);
            if (token.isEmpty()) {
                return false;
            }
            const int sep = token.indexOf(QStringLiteral("::"));
            if (sep <= 0) {
                return false;
            }
            bool ok = false;
            const int connIdx = token.left(sep).toInt(&ok);
            const QString poolName = token.mid(sep + 2).trimmed();
            if (!ok || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
                return false;
            }
            m_pendingConnContentPoolReloadKeys.insert(QStringLiteral("%1::%2").arg(connIdx).arg(poolName));
            scheduleReloadFlush();
            return true;
        };
        const bool reloadedTop = reloadTree(m_connContentTree);
        if (reloadedTop) {
            return;
        }
    }
}

bool MainWindow::mountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return false;
    }
    auto mountRefresh = [this, side, ctx]() {
        invalidateDatasetSubtreeCacheEntries(ctx.connIdx, ctx.poolName, ctx.datasetName, false);
        if (side == QStringLiteral("conncontent")) {
            reloadConnContentPool(ctx.connIdx, ctx.poolName);
        } else {
            reloadDatasetSide(side);
        }
    };
    QString keyLocation;
    getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("keylocation"), keyLocation);
    keyLocation = keyLocation.trimmed().toLower();
    QString keyStatus;
    getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("keystatus"), keyStatus);
    keyStatus = keyStatus.trimmed().toLower();
    QString encryptionRoot;
    getDatasetProperty(ctx.connIdx, ctx.datasetName, QStringLiteral("encryptionroot"), encryptionRoot);
    encryptionRoot = encryptionRoot.trimmed();
    const bool needsPromptLoadKey = keyLocation == QStringLiteral("prompt")
                                    && keyStatus != QStringLiteral("available");
    if (needsPromptLoadKey) {
        bool ok = false;
        const QString passphrase = QInputDialog::getText(
            this,
            QStringLiteral("Load key"),
            QStringLiteral("Clave"),
            QLineEdit::Password,
            QString(),
            &ok);
        if (!ok || passphrase.isEmpty()) {
            return false;
        }
        const QString keyTarget =
            (!encryptionRoot.isEmpty() && encryptionRoot != QStringLiteral("-"))
                ? encryptionRoot
                : ctx.datasetName;
        const QString cmd = QStringLiteral("zfs load-key %1 && %2")
                                .arg(shSingleQuote(keyTarget),
                                     mwhelpers::buildSingleMountCommand(ctx.datasetName));
        const QByteArray stdinPayload = (passphrase + QStringLiteral("\n")).toUtf8();
        return executeDatasetAction(side,
                                    QStringLiteral("Montar"),
                                    ctx,
                                    cmd,
                                    90000,
                                    false,
                                    stdinPayload,
                                    false,
                                    mountRefresh);
    }
    if (!ensureParentMountedBeforeMount(ctx)) {
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
        const QString precheckCmd = mwhelpers::buildWindowsMountPrecheckCommand(ctx.datasetName, effectiveMp);
        QString preOut;
        QString preErr;
        int preRc = -1;
        if (!runSsh(p, withSudo(p, precheckCmd), 15000, preOut, preErr, preRc) || preRc != 0) {
            const QString reason = oneLine((preErr.isEmpty() ? preOut : preErr));
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_mount_pre_fail1"), QStringLiteral("No se puede montar %1.\n%2").arg(ctx.datasetName, reason),
                    QStringLiteral("Cannot mount %1.\n%2").arg(ctx.datasetName, reason),
                    QStringLiteral("无法挂载 %1。\n%2").arg(ctx.datasetName, reason)));
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Precheck montar falló %1::%2 -> %3")
                       .arg(p.name, ctx.datasetName, reason));
            return false;
        }
    }
    const QString cmd = mwhelpers::buildSingleMountCommand(ctx.datasetName);
    return executeDatasetAction(side,
                                QStringLiteral("Montar"),
                                ctx,
                                cmd,
                                45000,
                                false,
                                {},
                                false,
                                mountRefresh);
}

bool MainWindow::ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx) {
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        return false;
    }
    int resolvedConnIdx = ctx.connIdx;
    auto datasetExistsInConn = [&](int idx) -> bool {
        if (idx < 0 || idx >= m_profiles.size() || ctx.poolName.isEmpty()) {
            return false;
        }
        if (!ensureDatasetsLoaded(idx, ctx.poolName)) {
            return false;
        }
        return datasetExistsInModel(idx, ctx.poolName, ctx.datasetName);
    };
    if (!datasetExistsInConn(resolvedConnIdx)) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (i == resolvedConnIdx) {
                continue;
            }
            if (datasetExistsInConn(i)) {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("Contexto de conexión ajustado para montar %1: %2 -> %3")
                           .arg(ctx.datasetName)
                           .arg((ctx.connIdx >= 0 && ctx.connIdx < m_profiles.size()) ? m_profiles[ctx.connIdx].name : QStringLiteral("<?>"))
                           .arg(m_profiles[i].name));
                resolvedConnIdx = i;
                break;
            }
        }
    }

    const QString parent = parentDatasetName(ctx.datasetName);
    if (parent.isEmpty()) {
        return true;
    }

    QString parentMountpoint;
    if (!getDatasetProperty(resolvedConnIdx, parent, QStringLiteral("mountpoint"), parentMountpoint)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_parent_mp_fail1"), QStringLiteral("No se pudo comprobar mountpoint del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mountpoint %1").arg(parent),
                                 QStringLiteral("无法检查父数据集 mountpoint：%1").arg(parent)));
        return false;
    }
    QString parentCanmount;
    if (!getDatasetProperty(resolvedConnIdx, parent, QStringLiteral("canmount"), parentCanmount)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_parent_cm_fail1"), QStringLiteral("No se pudo comprobar canmount del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent canmount %1").arg(parent),
                                 QStringLiteral("无法检查父数据集 canmount：%1").arg(parent)));
        return false;
    }
    QString parentMounted;
    if (!getDatasetProperty(resolvedConnIdx, parent, QStringLiteral("mounted"), parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_parent_mntfail1"), QStringLiteral("No se pudo comprobar estado mounted del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mounted state %1").arg(parent),
                                 QStringLiteral("无法检查父数据集挂载状态：%1").arg(parent)));
        return false;
    }
    if (!mwhelpers::parentAllowsChildMount(parentMountpoint, parentCanmount, parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_parent_notmnt1"), QStringLiteral("El dataset padre %1 no está montado, móntelo antes por favor").arg(parent),
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
                             trk(QStringLiteral("t_mp_conf_chk01"), QStringLiteral("No se pudo comprobar conflictos de mountpoint."),
                                 QStringLiteral("Could not validate mountpoint conflicts."),
                                 QStringLiteral("无法检查挂载点冲突。")));
        return false;
    }
    const QString key = datasetCacheKey(ctx.connIdx, ctx.poolName);
    const auto cacheIt = m_poolDatasetCache.constFind(key);
    if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_mp_conf_chk01"), QStringLiteral("No se pudo comprobar conflictos de mountpoint."),
                                 QStringLiteral("Could not validate mountpoint conflicts."),
                                 QStringLiteral("无法检查挂载点冲突。")));
        return false;
    }
    const PoolDatasetCache& cache = cacheIt.value();

    QMap<QString, QString> targetMpByDs;
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
    }

    const QMap<QString, QStringList> duplicateMps = mwhelpers::duplicateMountpoints(targetMpByDs);
    for (auto it = duplicateMps.constBegin(); it != duplicateMps.constEnd(); ++it) {
        const QStringList dsList = it.value();
        if (!dsList.isEmpty()) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_mp_conf_int001"), QStringLiteral("Conflicto de mountpoint dentro de la selección.\nMountpoint: %1\nDatasets:\n%2")
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
    const bool isWinConn = isWindowsConnection(p);
    const bool daemonReadApiOk =
        !isWinConn
        && ctx.connIdx >= 0
        && ctx.connIdx < m_states.size()
        && m_states[ctx.connIdx].daemonInstalled
        && m_states[ctx.connIdx].daemonActive
        && m_states[ctx.connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    const QString mountedCmdClassic = withSudo(
        p,
        mwhelpers::withUnixSearchPathCommand(
            isWinConn ? QStringLiteral("zfs mount")
                      : QStringLiteral("zfs mount -j")));
    const QString mountedCmdDaemon = withSudo(
        p, mwhelpers::withUnixSearchPathCommand(
               QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-mount")));
    QVector<QPair<QString, QString>> mountedRows;
    const QString selectedMountCmd = daemonReadApiOk ? mountedCmdDaemon : mountedCmdClassic;
    bool mountListOk = runSsh(p, selectedMountCmd, 20000, mountedOut, mountedErr, mountedRc) && mountedRc == 0;
    if (mountListOk) {
        mountedRows = isWinConn
            ? mwhelpers::parseZfsMountOutput(mountedOut)
            : mwhelpers::parseZfsMountJsonOutput(mountedOut);
    }
    if (!isWinConn && mountedRows.isEmpty() && !daemonReadApiOk) {
        QString fallbackOut;
        QString fallbackErr;
        int fallbackRc = -1;
        const QString fallbackCmd = withSudo(p, mwhelpers::withUnixSearchPathCommand(QStringLiteral("zfs mount")));
        if (runSsh(p, fallbackCmd, 20000, fallbackOut, fallbackErr, fallbackRc) && fallbackRc == 0) {
            mountedRows = mwhelpers::parseZfsMountOutput(fallbackOut);
        }
    }
    if (mountedRows.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_mounted_rd_err1"), QStringLiteral("No se pudo leer datasets montados."),
                                 QStringLiteral("Could not read mounted datasets."),
                                 QStringLiteral("无法读取已挂载数据集。")));
        return false;
    }

    QMap<QString, QStringList> mountedByMp;
    for (const auto& row : mountedRows) {
        const QString ds = row.first;
        const QString mp = row.second;
        if (ds.isEmpty() || mp.isEmpty()) {
            continue;
        }
        mountedByMp[mp].push_back(ds);
    }

    const QVector<mwhelpers::MountpointConflict> conflicts =
        mwhelpers::externalMountpointConflicts(targetMpByDs, mountedByMp);
    if (!conflicts.isEmpty()) {
        const mwhelpers::MountpointConflict& c = conflicts.front();
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_mount_single01"), QStringLiteral("No se permite montar más de un dataset en el mismo directorio.\nMountpoint: %1\nMontado: %2\nSolicitado: %3")
                    .arg(c.mountpoint, c.mountedDataset, c.requestedDataset),
                QStringLiteral("Only one mounted dataset per directory is allowed.\nMountpoint: %1\nMounted: %2\nRequested: %3")
                    .arg(c.mountpoint, c.mountedDataset, c.requestedDataset),
                QStringLiteral("同一目录不允许挂载多个数据集。\n挂载点：%1\n已挂载：%2\n请求：%3")
                    .arg(c.mountpoint, c.mountedDataset, c.requestedDataset)));
        return false;
    }
    return true;
}

bool MainWindow::umountDataset(const QString& side, const DatasetSelectionContext& ctx) {
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return false;
    }
    auto umountRefresh = [this, side, ctx]() {
        invalidateDatasetSubtreeCacheEntries(ctx.connIdx, ctx.poolName, ctx.datasetName, false);
        if (side == QStringLiteral("conncontent")) {
            reloadConnContentPool(ctx.connIdx, ctx.poolName);
        } else {
            reloadDatasetSide(side);
        }
    };
    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString hasChildrenCmd = mwhelpers::buildHasMountedChildrenCommand(isWin, ctx.datasetName);

    QString out;
    QString err;
    int rc = -1;
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString checkCmd = withSudo(p, isWin ? hasChildrenCmd : mwhelpers::withUnixSearchPathCommand(hasChildrenCmd));
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
        cmd = mwhelpers::buildRecursiveUmountCommand(isWin, ctx.datasetName);
    } else {
        cmd = mwhelpers::buildSingleUmountCommand(isWin, ctx.datasetName);
    }
    return executeDatasetAction(side,
                                QStringLiteral("Desmontar"),
                                ctx,
                                cmd,
                                90000,
                                isWin,
                                {},
                                false,
                                umountRefresh);
}
