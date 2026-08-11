// Persistencia de la lista de acciones pendientes.
//
// La lista dejó de ser una cola que se vacía y pasó a ser un plan de trabajo: sobrevive
// a la ejecución de cada acción y, con esto, también al cierre de la aplicación.
//
// LO IMPORTANTE DE ESTE FICHERO ES LO QUE **NO** SE GUARDA.
//
// La orden de cada acción lleva la contraseña de sudo dentro. No es un descuido: la
// pone `withSudoCommand`, en la forma
//
//     printf '%b\n' '\0162\0160\0161…' | sudo -k -S -p '' sh -c '…'
//
// que existe porque en macOS Qt descompone el texto no-ASCII al pasar la orden al
// intérprete. Escribir eso tal cual en config.json dejaría la contraseña en claro para
// cualquiera que sepa leer octal —config.json no está cifrado; lo están los campos de
// las conexiones—. Así que al guardar se sustituye por un marcador y al cargar se
// repone desde el perfil, que es donde vive cifrada. Efecto lateral deseable: si el
// usuario cambia la contraseña entre sesiones, la acción restaurada usa la nueva.
//
// Y la frase de cifrado de un dataset nuevo (`rpcSecret`) NO se guarda de ninguna forma:
// esas acciones sencillamente no se persisten. Guardarlas sin el secreto sería peor que
// no guardarlas, porque al aplicarlas crearían el dataset sin cifrar o fallarían a mitad.

#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace {
constexpr auto kPendingRootKey = "pending_actions";
}  // namespace

QString MainWindow::pendingConnKey(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return QString();
    }
    const ConnectionProfile& p = m_profiles.at(connIdx);
    const QString id = p.id.trimmed();
    return id.isEmpty() ? p.name.trimmed().toLower() : id.toLower();
}

int MainWindow::pendingConnIndex(const QString& key) const {
    const QString wanted = key.trimmed().toLower();
    if (wanted.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (pendingConnKey(i) == wanted) {
            return i;
        }
    }
    return -1;
}

QVector<mwhelpers::StorableSecret> MainWindow::pendingStorableSecrets() const {
    QVector<mwhelpers::StorableSecret> secrets;
    secrets.reserve(m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i) {
        const QString key = pendingConnKey(i);
        if (key.isEmpty()) {
            continue;
        }
        secrets.push_back(mwhelpers::StorableSecret{key, m_profiles.at(i).password});
    }
    return secrets;
}

QString MainWindow::redactStoredSecrets(const QString& command, bool* okOut) const {
    return mwhelpers::redactSecretsForStorage(command, pendingStorableSecrets(), okOut);
}

QString MainWindow::restoreStoredSecrets(const QString& stored) const {
    return mwhelpers::restoreSecretsFromStorage(stored, pendingStorableSecrets());
}

QJsonObject MainWindow::pendingCtxToJson(const DatasetSelectionContext& ctx) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("valid"), ctx.valid);
    // Por identificador de conexión, no por índice: los índices se mueven en cuanto se
    // añade o se borra una conexión, y la acción restaurada apuntaría a otra máquina.
    obj.insert(QStringLiteral("conn"), pendingConnKey(ctx.connIdx));
    obj.insert(QStringLiteral("pool"), ctx.poolName);
    obj.insert(QStringLiteral("dataset"), ctx.datasetName);
    obj.insert(QStringLiteral("snapshot"), ctx.snapshotName);
    return obj;
}

MainWindow::DatasetSelectionContext MainWindow::pendingCtxFromJson(const QJsonObject& obj) const {
    DatasetSelectionContext ctx;
    ctx.connIdx = pendingConnIndex(obj.value(QStringLiteral("conn")).toString());
    ctx.poolName = obj.value(QStringLiteral("pool")).toString();
    ctx.datasetName = obj.value(QStringLiteral("dataset")).toString();
    ctx.snapshotName = obj.value(QStringLiteral("snapshot")).toString();
    ctx.valid = obj.value(QStringLiteral("valid")).toBool() && ctx.connIdx >= 0;
    return ctx;
}

QJsonObject MainWindow::pendingShellDraftToJson(const PendingShellActionDraft& draft,
                                                QString* refusalOut) const {
    auto refuse = [refusalOut](const QString& why) {
        if (refusalOut) {
            *refusalOut = why;
        }
        return QJsonObject();
    };
    if (!draft.rpcSecret.isEmpty()) {
        return refuse(QStringLiteral("lleva una frase de cifrado, que no se guarda en disco"));
    }
    bool redactOk = false;
    const QString storedCommand = redactStoredSecrets(draft.command, &redactOk);
    if (!redactOk) {
        return refuse(QStringLiteral("no se pudo separar la contraseña de la orden"));
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("uid"), draft.uid);
    obj.insert(QStringLiteral("user_name"), draft.userName);
    obj.insert(QStringLiteral("active"), draft.active);
    obj.insert(QStringLiteral("scope_label"), draft.scopeLabel);
    obj.insert(QStringLiteral("display_label"), draft.displayLabel);
    obj.insert(QStringLiteral("command"), storedCommand);
    obj.insert(QStringLiteral("timeout_ms"), draft.timeoutMs);
    obj.insert(QStringLiteral("stream_progress"), draft.streamProgress);
    obj.insert(QStringLiteral("refresh_source"), pendingCtxToJson(draft.refreshSource));
    obj.insert(QStringLiteral("refresh_target"), pendingCtxToJson(draft.refreshTarget));
    obj.insert(QStringLiteral("refresh_scope"), static_cast<int>(draft.refreshScope));
    obj.insert(QStringLiteral("rpc_conn"), pendingConnKey(draft.rpcConnIdx));
    obj.insert(QStringLiteral("rpc_argv"), QJsonArray::fromStringList(draft.rpcArgv));
    obj.insert(QStringLiteral("action_side"), draft.datasetActionSide);
    obj.insert(QStringLiteral("action_name"), draft.datasetActionName);
    obj.insert(QStringLiteral("action_ctx"), pendingCtxToJson(draft.datasetActionCtx));
    obj.insert(QStringLiteral("action_argv"), QJsonArray::fromStringList(draft.datasetActionArgv));
    obj.insert(QStringLiteral("action_stdin"),
               QString::fromLatin1(draft.datasetActionStdin.toBase64()));
    obj.insert(QStringLiteral("action_allow_win_script"), draft.datasetActionAllowWindowsScript);
    if (draft.fromDirInput.valid) {
        QJsonObject fd;
        fd.insert(QStringLiteral("dataset_path"), draft.fromDirInput.datasetPath);
        fd.insert(QStringLiteral("blocksize"), draft.fromDirInput.blocksize);
        fd.insert(QStringLiteral("parents"), draft.fromDirInput.parents);
        fd.insert(QStringLiteral("properties"),
                  QJsonArray::fromStringList(draft.fromDirInput.properties));
        fd.insert(QStringLiteral("extra_args"), draft.fromDirInput.extraArgs);
        fd.insert(QStringLiteral("delete_sources"), draft.fromDirInput.deleteSourceDirs);
        QJsonArray sources;
        for (const auto& src : draft.fromDirInput.sources) {
            QJsonObject s;
            s.insert(QStringLiteral("conn"), src.first);
            s.insert(QStringLiteral("path"), src.second);
            sources.append(s);
        }
        fd.insert(QStringLiteral("sources"), sources);
        obj.insert(QStringLiteral("from_dir"), fd);
    }
    return obj;
}

bool MainWindow::pendingShellDraftFromJson(const QJsonObject& obj,
                                           PendingShellActionDraft* out) const {
    if (!out) {
        return false;
    }
    PendingShellActionDraft draft;
    draft.uid = obj.value(QStringLiteral("uid")).toString().trimmed();
    draft.userName = obj.value(QStringLiteral("user_name")).toString();
    draft.active = obj.value(QStringLiteral("active")).toBool(false);
    draft.scopeLabel = obj.value(QStringLiteral("scope_label")).toString();
    draft.displayLabel = obj.value(QStringLiteral("display_label")).toString();
    draft.command = restoreStoredSecrets(obj.value(QStringLiteral("command")).toString());
    draft.timeoutMs = obj.value(QStringLiteral("timeout_ms")).toInt(0);
    draft.streamProgress = obj.value(QStringLiteral("stream_progress")).toBool(true);
    draft.refreshSource = pendingCtxFromJson(obj.value(QStringLiteral("refresh_source")).toObject());
    draft.refreshTarget = pendingCtxFromJson(obj.value(QStringLiteral("refresh_target")).toObject());
    const int scope = obj.value(QStringLiteral("refresh_scope")).toInt(
        static_cast<int>(PendingShellActionDraft::RefreshScope::TargetOnly));
    draft.refreshScope = static_cast<PendingShellActionDraft::RefreshScope>(scope);
    draft.rpcConnIdx = pendingConnIndex(obj.value(QStringLiteral("rpc_conn")).toString());
    for (const QJsonValue& v : obj.value(QStringLiteral("rpc_argv")).toArray()) {
        draft.rpcArgv << v.toString();
    }
    draft.datasetActionSide = obj.value(QStringLiteral("action_side")).toString();
    draft.datasetActionName = obj.value(QStringLiteral("action_name")).toString();
    draft.datasetActionCtx = pendingCtxFromJson(obj.value(QStringLiteral("action_ctx")).toObject());
    for (const QJsonValue& v : obj.value(QStringLiteral("action_argv")).toArray()) {
        draft.datasetActionArgv << v.toString();
    }
    draft.datasetActionStdin = QByteArray::fromBase64(
        obj.value(QStringLiteral("action_stdin")).toString().toLatin1());
    draft.datasetActionAllowWindowsScript =
        obj.value(QStringLiteral("action_allow_win_script")).toBool(false);
    if (obj.contains(QStringLiteral("from_dir"))) {
        const QJsonObject fd = obj.value(QStringLiteral("from_dir")).toObject();
        draft.fromDirInput.valid = true;
        draft.fromDirInput.datasetPath = fd.value(QStringLiteral("dataset_path")).toString();
        draft.fromDirInput.blocksize = fd.value(QStringLiteral("blocksize")).toString();
        draft.fromDirInput.parents = fd.value(QStringLiteral("parents")).toBool(true);
        for (const QJsonValue& v : fd.value(QStringLiteral("properties")).toArray()) {
            draft.fromDirInput.properties << v.toString();
        }
        draft.fromDirInput.extraArgs = fd.value(QStringLiteral("extra_args")).toString();
        draft.fromDirInput.deleteSourceDirs =
            fd.value(QStringLiteral("delete_sources")).toBool(false);
        for (const QJsonValue& v : fd.value(QStringLiteral("sources")).toArray()) {
            const QJsonObject s = v.toObject();
            draft.fromDirInput.sources.push_back(
                qMakePair(s.value(QStringLiteral("conn")).toString(),
                          s.value(QStringLiteral("path")).toString()));
        }
    }
    if (draft.uid.isEmpty() || draft.command.trimmed().isEmpty()) {
        return false;
    }
    // Sin la conexión a la que apuntaba, la acción no se puede ni mostrar con sentido ni
    // ejecutar: se descarta en vez de quedarse como una línea que falla al pulsarla.
    if (draft.datasetActionCtx.connIdx < 0 && draft.refreshTarget.connIdx < 0) {
        return false;
    }
    // Una orden restaurada que todavía tenga marcadores es una orden rota: apuntaba a un
    // perfil que ya no existe, y ejecutarla mandaría el marcador literal al intérprete.
    if (draft.command.contains(mwhelpers::storedSecretMarkerPrefix())) {
        return false;
    }
    *out = draft;
    return true;
}

void MainWindow::savePendingActions() {
    QString jsonErr;
    QJsonObject root = m_store.loadConfigJson(&jsonErr);
    QJsonArray arr;
    int refused = 0;
    for (const PendingChange& change : m_pendingChangesModel) {
        if (change.kind != PendingChange::Kind::ShellAction) {
            continue;
        }
        QString refusal;
        const QJsonObject obj = pendingShellDraftToJson(change.shellDraft, &refusal);
        if (obj.isEmpty()) {
            ++refused;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("[pendientes] «%1» no se guarda en disco: %2")
                       .arg(change.shellDraft.displayLabel.trimmed(), refusal));
            continue;
        }
        arr.append(obj);
    }
    root.insert(QString::fromLatin1(kPendingRootKey), arr);
    if (!m_store.saveConfigJson(root, &jsonErr)) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("[pendientes] no se pudo guardar la lista: %1").arg(jsonErr));
        return;
    }
    if (arr.size() > 0 || refused > 0) {
        appLog(QStringLiteral("INFO"),
               QStringLiteral("[pendientes] lista guardada: %1 acciones (%2 no guardables)")
                   .arg(arr.size())
                   .arg(refused));
    }
}

void MainWindow::loadPendingActions() {
    const QJsonObject root = m_store.loadConfigJson();
    const QJsonArray arr = root.value(QString::fromLatin1(kPendingRootKey)).toArray();
    if (arr.isEmpty()) {
        return;
    }
    int restored = 0;
    int dropped = 0;
    for (const QJsonValue& v : arr) {
        PendingShellActionDraft draft;
        if (!pendingShellDraftFromJson(v.toObject(), &draft)) {
            ++dropped;
            continue;
        }
        PendingChange change;
        change.kind = PendingChange::Kind::ShellAction;
        change.shellDraft = draft;
        change.removableIndividually = true;
        change.executableIndividually = true;
        change.activatable = true;
        change.stableId = QStringLiteral("shell|%1").arg(draft.uid);
        m_pendingChangesModel.push_back(change);
        ++restored;
    }
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("[pendientes] lista restaurada: %1 acciones%2")
               .arg(restored)
               .arg(dropped > 0 ? QStringLiteral(" (%1 descartadas: su conexión ya no existe)")
                                      .arg(dropped)
                                : QString()));
    updateApplyPropsButtonState();
}
