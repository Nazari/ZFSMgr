#include "mainwindow.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include "mainwindow_helpers.h"
#include "daemonpayload.h"
#include "mainwindow_ui_logic.h"
#include "agentversion.h"

#include <algorithm>
#include <QMessageBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>

#include <QtConcurrent/QtConcurrent>

namespace {
bool zfsmgrTestModeEnabled() {
    const QByteArray value = qgetenv("ZFSMGR_TEST_MODE");
    return !value.isEmpty() && value != "0" && value.compare("false", Qt::CaseInsensitive) != 0;
}

QString canonicalDatasetParentName(const QString& fullName) {
    const QString trimmed = fullName.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const int snapPos = trimmed.indexOf('@');
    if (snapPos > 0) {
        return trimmed.left(snapPos);
    }
    const int slashPos = trimmed.lastIndexOf('/');
    if (slashPos > 0) {
        return trimmed.left(slashPos);
    }
    return QString();
}

QStringList gsaPropertyKeysForModel() {
    return {
        QStringLiteral("org.fc16.gsa:activado"),
        QStringLiteral("org.fc16.gsa:recursivo"),
        QStringLiteral("org.fc16.gsa:horario"),
        QStringLiteral("org.fc16.gsa:diario"),
        QStringLiteral("org.fc16.gsa:semanal"),
        QStringLiteral("org.fc16.gsa:mensual"),
        QStringLiteral("org.fc16.gsa:anual"),
        QStringLiteral("org.fc16.gsa:nivelar"),
        QStringLiteral("org.fc16.gsa:destino"),
    };
}

QString gsaComparableValue(const QString& propName, const QString& rawValue) {
    const QString prop = propName.trimmed();
    const QString value = rawValue.trimmed();
    if (!prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
        return rawValue;
    }
    if (prop.compare(QStringLiteral("org.fc16.gsa:destino"), Qt::CaseInsensitive) == 0) {
        return (value == QStringLiteral("-")) ? QString() : rawValue;
    }
    if (value.isEmpty() || value == QStringLiteral("-")) {
        if (prop.compare(QStringLiteral("org.fc16.gsa:horario"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:diario"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:semanal"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:mensual"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:anual"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("0");
        }
        return QStringLiteral("off");
    }
    return rawValue;
}

QString normalizedPropKey(const QString& propName) {
    return propName.trimmed().toLower();
}

bool mountedStateFromAnyText(const QString& value, bool* mountedOut) {
    const QString s = value.trimmed().toLower();
    if (s == QStringLiteral("montado")
        || s == QStringLiteral("mounted")
        || s == QStringLiteral("已挂载")
        || s == QStringLiteral("on")
        || s == QStringLiteral("yes")
        || s == QStringLiteral("true")
        || s == QStringLiteral("1")) {
        if (mountedOut) {
            *mountedOut = true;
        }
        return true;
    }
    if (s == QStringLiteral("desmontado")
        || s == QStringLiteral("unmounted")
        || s == QStringLiteral("未挂载")
        || s == QStringLiteral("off")
        || s == QStringLiteral("no")
        || s == QStringLiteral("false")
        || s == QStringLiteral("0")) {
        if (mountedOut) {
            *mountedOut = false;
        }
        return true;
    }
    return false;
}

struct PoolAutoSnapshotLoadResult {
    int connIdx{-1};
    QString poolName;
    bool ok{false};
    QString errorText;
    QMap<QString, QMap<QString, QString>> loaded;
};

bool isAllowedGenericZpoolMutationOpClient(const QString& opRaw) {
    const QString op = opRaw.trimmed().toLower();
    static const QSet<QString> allowed = {
        QStringLiteral("create"),
        QStringLiteral("destroy"),
        QStringLiteral("add"),
        QStringLiteral("remove"),
        QStringLiteral("attach"),
        QStringLiteral("detach"),
        QStringLiteral("replace"),
        QStringLiteral("offline"),
        QStringLiteral("online"),
        QStringLiteral("clear"),
        QStringLiteral("export"),
        QStringLiteral("import"),
        QStringLiteral("scrub"),
        QStringLiteral("trim"),
        QStringLiteral("initialize"),
        QStringLiteral("sync"),
        QStringLiteral("upgrade"),
        QStringLiteral("reguid"),
        QStringLiteral("split"),
        QStringLiteral("checkpoint"),
    };
    return allowed.contains(op);
}

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
}

MainWindow::MainWindow(const QString& masterPassword, const QString& language, QWidget* parent)
    : QMainWindow(parent)
    , m_conns(QStringLiteral("ZFSMgr")) {
    QElapsedTimer startupTimer;
    startupTimer.start();
    setObjectName(QStringLiteral("mainWindow"));
    m_language = language.trimmed().toLower();
    if (m_language.isEmpty()) {
        m_language = QStringLiteral("es");
    }
    loadUiSettings();
    if (!language.trimmed().isEmpty()) {
        m_language = language.trimmed().toLower();
        saveUiSettings();
    }
    // El transporte no sabe nada del registro de la aplicación: se le dice a dónde
    // escribir. En un CLI este destino iría a la salida de error.
    // Los avisos —los que son PROSA— llegan tipificados y se redactan aquí, que es donde
    // se sabe el idioma. El transporte solo dice cuál es; ver transportNoticeText.
    m_transport.avisoSink = [this](TransportSession::Nivel n, const std::string& connId,
                                   const zfsmgr::base::transport::NotaDeAviso& a) {
        m_transport.logConn(n, connId, transportNoticeText(a).toStdString());
    };
    m_transport.sink = [this](TransportSession::Nivel n, const std::string& connId,
                              const std::string& msg) {
        static const QMap<TransportSession::Nivel, QString> kNiveles = {
            {TransportSession::Nivel::Normal, QStringLiteral("NORMAL")},
            {TransportSession::Nivel::Info, QStringLiteral("INFO")},
            {TransportSession::Nivel::Warn, QStringLiteral("WARN")},
            {TransportSession::Nivel::Error, QStringLiteral("ERROR")},
            {TransportSession::Nivel::Debug, QStringLiteral("DEBUG")},
        };
        const QString texto = QString::fromStdString(msg);
        appLog(kNiveles.value(n, QStringLiteral("INFO")), texto);
        const QString id = QString::fromStdString(connId).trimmed();
        if (!id.isEmpty()) {
            appendConnectionLog(id, texto);
        }
    };
    // Cómo se piden credenciales cuando no las hay. En la interfaz, un diálogo; un CLI
    // pondría aquí uno que lee del descriptor o pregunta por terminal.
    m_transport.credentialProvider =
        [this](const std::string& motivo, std::string& usuario, std::string& clave) -> bool {
        QDialog dlg(this);
        dlg.setWindowTitle(QString::fromStdString(motivo));
        dlg.setModal(true);
        auto* form = new QFormLayout(&dlg);
        auto* userEdit = new QLineEdit(&dlg);
        auto* passEdit = new QLineEdit(&dlg);
        passEdit->setEchoMode(QLineEdit::Password);
        const QString envUser = qEnvironmentVariable("USER").trimmed();
        const QString envUserWin = qEnvironmentVariable("USERNAME").trimmed();
        userEdit->setText(!envUser.isEmpty() ? envUser : envUserWin);
        form->addRow(trk(QStringLiteral("t_usuario_d31f58"),
                         QStringLiteral("Usuario"), QStringLiteral("User"),
                         QStringLiteral("用户")),
                     userEdit);
        form->addRow(trk(QStringLiteral("t_password_8be3c9"),
                         QStringLiteral("Password"), QStringLiteral("Password"),
                         QStringLiteral("密码")),
                     passEdit);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        form->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        usuario = userEdit->text().toStdString();
        clave = passEdit->text().toStdString();
        return true;
    };
    // Las dos decisiones que el transporte necesita del registro. Se le dan como
    // políticas y no como acceso al registro entero: lo que necesita son estas dos cosas,
    // no las contraseñas de todas las máquinas.
    // Dejar respirar a la ventana mientras el transporte espera. Sustituye al
    // processEvents que el transporte hacía por su cuenta, y con él se va la última razón
    // por la que la sesión necesitaba un puntero a la ventana.
    //
    // ExcludeUserInputEvents a propósito: deja pasar los repintados pero NO las acciones
    // del usuario, así que no puede colarse por aquí nada que recargue las conexiones y
    // deje colgando las referencias que sostiene quien llamó.
    //
    // Y SOLO en el hilo de la ventana: bombear el bucle de eventos desde un hilo de
    // refresco no refresca nada y toca lo que no debe.
    m_transport.pump = [this](bool permitirEntradaDeUsuario) -> bool {
        // Solo en el hilo de la ventana: bombear desde un hilo de refresco no refresca nada
        // y toca lo que no debe.
        if (QThread::currentThread() != this->thread()) {
            return true;
        }
        // Los dos contextos que la versión anterior ya distinguía. Ver
        // TransportSession::pump: unificarlos rompe una cosa u otra.
        QCoreApplication::processEvents(
            permitirEntradaDeUsuario ? QEventLoop::AllEvents : QEventLoop::ExcludeUserInputEvents,
            permitirEntradaDeUsuario ? 20 : 40);
        return true;
    };
    // Dónde se pueden montar túneles, y cómo llegar hasta ahí. Los dos enganches
    // sustituyen a lo que antes era `TransportSession::owner`.
    //
    // **Se conservan aunque su motivo original haya desaparecido.** Estaban porque los
    // túneles eran QProcess colgados de la ventana y crearlos desde un hilo de refresco
    // daba un aviso de afinidad o una caída; ahora son ChildProcess y no cuelgan de nadie.
    // Quitarlos permitiría montar túneles desde los hilos de refresco, que es un cambio de
    // concurrencia real —y es exactamente el arranque serializado que hay anotado aparte—,
    // así que no se hace en el mismo paso en que se cambia de motor.
    m_transport.tunnelsAllowedHere = [this]() {
        return QThread::currentThread() == this->thread();
    };
    m_transport.runWhereTunnelsAllowed = [this](const std::function<void()>& tarea) {
        QMetaObject::invokeMethod(this, tarea, Qt::BlockingQueuedConnection);
    };
    m_transport.localSudoResolver = [this](zfsmgr::base::ConnectionProfile& perfil) {
        ConnectionProfile q = fromBaseProfile(perfil);
        const bool ok = ensureLocalSudoCredentials(q);
        perfil = toBaseProfile(q);
        return ok;
    };
    m_transport.tlsPersister = [this](const zfsmgr::base::ConnectionProfile& p,
                                      const std::string& srv, const std::string& cli,
                                      const std::string& key, std::uint16_t puerto,
                                      std::string* errorOut) {
        QString e;
        const bool ok = persistDaemonTlsMaterialForConnection(
            fromBaseProfile(p), QByteArray::fromStdString(srv), QByteArray::fromStdString(cli),
            QByteArray::fromStdString(key), puerto, &e);
        if (errorOut) {
            *errorOut = e.toStdString();
        }
        return ok;
    };
    m_conns.store.setLanguage(m_language);
    m_conns.store.setMasterPassword(masterPassword);
    initLogPersistence();
    appLog(QStringLiteral("INFO"), QStringLiteral("[startup] initLogPersistence: %1 ms").arg(startupTimer.elapsed()));
    buildUi();
    appLog(QStringLiteral("INFO"), QStringLiteral("[startup] buildUi: %1 ms").arg(startupTimer.elapsed()));
    loadUserExpandedState();
    if (!zfsmgrTestModeEnabled()) {
        loadConnections();
        appLog(QStringLiteral("INFO"), QStringLiteral("[startup] loadConnections: %1 ms").arg(startupTimer.elapsed()));
        // Después de las conexiones, no antes: cada acción guardada apunta a la suya por
        // identificador, y su orden necesita la contraseña del perfil para reponer lo que
        // no se escribió en disco.
        loadPendingActions();
        restoreSplitTreeLayoutFromState(m_splitTreeLayoutState);
        for (const ConnectionProfile& p : std::as_const(m_conns.profiles)) {
            if (!isLocalConnection(p) || p.username.trimmed().isEmpty() || p.password.isEmpty()) {
                continue;
            }
            m_localSudoUsername = p.username.trimmed();
            m_localSudoPassword = p.password;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Credenciales sudo locales inyectadas en memoria al arrancar (connLocal)"));
            break;
        }
        ensureStartupLocalSudoConnection();
        appLog(QStringLiteral("INFO"), QStringLiteral("[startup] ensureLocalSudo: %1 ms").arg(startupTimer.elapsed()));
        updateStatus(trk(QStringLiteral("t_startup_refresh_001"),
                         QStringLiteral("Cargando conexiones..."),
                         QStringLiteral("Loading connections..."),
                         QStringLiteral("正在加载连接...")));
        // Diferido, no aquí: esto se ejecuta en el CONSTRUCTOR, así que el `w.show()`
        // de main.cpp no llega hasta que termina. El preámbulo del refresco monta un
        // túnel SSH por conexión, y con una máquina apagada o un enlace lento eso son
        // decenas de segundos —medido: 56 s con cuatro conexiones, una de ellas
        // inalcanzable— con la pantalla en blanco y sin ninguna pista de qué pasa.
        //
        // Con singleShot(0) la ventana se pinta primero y el refresco corre después: ya
        // sabe actualizar la interfaz por su cuenta a medida que llegan los resultados,
        // y la barra de estado dice "Cargando conexiones...".
        QTimer::singleShot(0, this, [this]() {
            if (!m_closing) {
                refreshAllConnections();
            }
        });
    }
    rebuildConnInfoModel();
    appLog(QStringLiteral("INFO"), QStringLiteral("[startup] constructor done (window not shown yet): %1 ms").arg(startupTimer.elapsed()));
}

void MainWindow::configureSingleConnectionUiTestState(const ConnectionProfile& profile,
                                                      const QStringList& importedPools,
                                                      const QStringList& importablePools) {
    ConnectionRuntimeState state;
    state.status = QStringLiteral("OK");
    state.detail = QStringLiteral("test");
    state.connectionMethod = profile.connType.trimmed();
    for (const QString& poolName : importedPools) {
        const QString trimmed = poolName.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        state.importedPools.push_back(PoolImported{profile.name, trimmed, QStringLiteral("Exportar")});
    }
    for (const QString& poolName : importablePools) {
        const QString trimmed = poolName.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        state.importablePools.push_back(
            PoolImportable{profile.name, trimmed, QString(), QStringLiteral("ONLINE"), QString(), QStringLiteral("Importar")});
    }
    // El estado se construye ANTES de tocar el registro, y entra con el perfil de una
    // vez: así los dos vectores nunca se ven de distinto tamaño.
    m_conns.clear();
    m_conns.append(profile, state);
    rebuildConnInfoModel();

    rebuildConnectionsTable();
    m_topDetailConnIdx = 0;
    setCurrentConnectionInUi(0);
}

void MainWindow::rebuildConnectionDetailsForTest() {
    rebuildConnectionEntityTabs();
}

void MainWindow::configurePoolDatasetsForTest(int connIdx,
                                              const QString& poolName,
                                              const QVector<UiTestDatasetSeed>& datasets) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || poolName.trimmed().isEmpty()) {
        return;
    }
    PoolDatasetCache cache;
    cache.loaded = true;
    for (const UiTestDatasetSeed& seed : datasets) {
        DatasetRecord record;
        record.name = seed.name.trimmed();
        record.guid.clear();
        record.mountpoint = seed.mountpoint.trimmed();
        record.canmount = seed.canmount.trimmed();
        record.mounted = seed.mounted.trimmed();
        if (record.name.isEmpty()) {
            continue;
        }
        cache.datasets.push_back(record);
        cache.recordByName.insert(record.name, record);
        cache.objectGuidByName.insert(record.name, QString());
        cache.snapshotsByDataset.insert(record.name, seed.snapshots);
    }
    m_conns.poolDatasetCache.insert(datasetCacheKey(connIdx, poolName), cache);
    rebuildConnInfoFor(connIdx);
}

// Este gancho ya no conmuta nada, y hay que decirlo aquí: el interruptor que escribía
// —m_showAutomaticGsaSnapshots— no lo leía nadie, porque showAutomaticSnapshots() devuelve
// `true` fijo desde el refactor del árbol unificado. O sea que llamarlo con `false` nunca
// ocultó una instantánea. La única prueba que dependía de ello está en QSKIP desde
// entonces. Se conserva la firma para no tocar las llamadas mientras se decide si la
// función vuelve o se retira entera.
void MainWindow::setShowAutomaticSnapshotsForTest(bool /*visible*/) {
    rebuildConnectionDetailsForTest();
}

void MainWindow::setConnectionDaemonStateForTest(int connIdx, bool installed, bool active) {
    if (connIdx < 0 || connIdx >= m_conns.states.size()) {
        return;
    }
    ConnectionRuntimeState& st = m_conns.states[connIdx];
    st.daemonInstalled = installed;
    st.daemonActive = active;
    st.daemonNativeBinary = installed;
    st.daemonApiVersion = installed ? agentversion::expectedApiVersion() : QString();
}

void MainWindow::setConnectionGsaStateForTest(int /*connIdx*/, bool /*installed*/, bool /*active*/, const QString& /*version*/) {
}

void MainWindow::configureDatasetPropertiesForTest(int connIdx,
                                                   const QString& objectName,
                                                   const QString& datasetType,
                                                   const QVector<UiTestPropertySeed>& rows) {
    const QString trimmedObject = objectName.trimmed();
    const QString poolName = trimmedObject.section('/', 0, 0).trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedObject.isEmpty()) {
        return;
    }
    QVector<DatasetPropCacheRow> cacheRows;
    for (const UiTestPropertySeed& seed : rows) {
        const QString prop = seed.prop.trimmed();
        if (prop.isEmpty()) {
            continue;
        }
        DatasetPropCacheRow row;
        row.prop = prop;
        row.value = seed.value;
        row.source = seed.source.trimmed().isEmpty() ? QStringLiteral("local") : seed.source.trimmed();
        row.readonly = seed.readonly.trimmed().isEmpty() ? QStringLiteral("no") : seed.readonly.trimmed();
        cacheRows.push_back(row);
    }
    storeDatasetPropertyRows(connIdx, poolName, trimmedObject, datasetType.trimmed(), cacheRows);
}

QString MainWindow::connStableIdForIndex(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return QStringLiteral("conn-%1").arg(connIdx);
    }
    const ConnectionProfile profile = m_conns.profiles[connIdx];
    const QString id = profile.id.trimmed();
    if (!id.isEmpty()) {
        return id;
    }
    const QString machineUid = profile.machineUid.trimmed();
    if (!machineUid.isEmpty()) {
        return QStringLiteral("%1#%2").arg(machineUid, QString::number(connIdx));
    }
    return QStringLiteral("conn-%1").arg(connIdx);
}

QString MainWindow::poolStableId(const PoolKey& key) const {
    if (!key.poolGuid.trimmed().isEmpty()) {
        return key.poolGuid.trimmed();
    }
    return key.poolName.trimmed();
}


DSKind MainWindow::dsKindFromNames(const QString& fullName, const QString& datasetType) {
    const QString trimmedType = datasetType.trimmed().toLower();
    if (trimmedType == QStringLiteral("filesystem")) {
        return DSKind::Filesystem;
    }
    if (trimmedType == QStringLiteral("volume")) {
        return DSKind::Volume;
    }
    if (trimmedType == QStringLiteral("snapshot")) {
        return DSKind::Snapshot;
    }
    if (fullName.contains('@')) {
        return DSKind::Snapshot;
    }
    return DSKind::Unknown;
}

void MainWindow::rebuildPoolInfoFromCache(PoolInfo& poolInfo,
                                          int connIdx,
                                          const QString& poolName,
                                          const PoolInfo* previousPoolInfo) {
    const QString datasetKey = datasetCacheKey(connIdx, poolName);
    const auto datasetIt = m_conns.poolDatasetCache.constFind(datasetKey);
    if (datasetIt == m_conns.poolDatasetCache.cend() || !datasetIt->loaded) {
        return;
    }

    const PoolDatasetCache& cache = *datasetIt;
    QSet<QString> rootObjects;
    for (const DatasetRecord& record : cache.datasets) {
        const QString fullName = record.name.trimmed();
        if (fullName.isEmpty()) {
            continue;
        }
        DSInfo& dsInfo = poolInfo.objectsByFullName[fullName];
        dsInfo.key = DSKey{poolInfo.key, fullName};
        dsInfo.kind = dsKindFromNames(fullName, QStringLiteral("filesystem"));
        dsInfo.parentFullName = canonicalDatasetParentName(fullName);
        dsInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
        dsInfo.runtime.propertiesState = LoadState::Loaded;
        dsInfo.runtime.datasetType = QStringLiteral("filesystem");
        dsInfo.runtime.properties.insert(QStringLiteral("used"), record.used);
        dsInfo.runtime.properties.insert(QStringLiteral("compressratio"), record.compressRatio);
        dsInfo.runtime.properties.insert(QStringLiteral("encryption"), record.encryption);
        dsInfo.runtime.properties.insert(QStringLiteral("creation"), record.creation);
        dsInfo.runtime.properties.insert(QStringLiteral("referenced"), record.referenced);
        dsInfo.runtime.properties.insert(QStringLiteral("mounted"), record.mounted);
        dsInfo.runtime.properties.insert(QStringLiteral("mountpoint"), record.mountpoint);
        dsInfo.runtime.properties.insert(QStringLiteral("canmount"), record.canmount);
        if (!record.guid.trimmed().isEmpty()) {
            dsInfo.runtime.properties.insert(QStringLiteral("guid"), record.guid.trimmed());
        } else {
            const QString guidFromCache = cache.objectGuidByName.value(fullName).trimmed();
            if (!guidFromCache.isEmpty()) {
                dsInfo.runtime.properties.insert(QStringLiteral("guid"), guidFromCache);
            }
        }
        dsInfo.capabilities.canDestroy = true;
        dsInfo.capabilities.canRename = true;
        dsInfo.capabilities.canManagePermissions = true;
        dsInfo.capabilities.canManageSchedules = true;
        dsInfo.capabilities.canMount = true;
        dsInfo.capabilities.canUnmount = true;
        dsInfo.editSession.target = dsInfo.key;
        if (dsInfo.parentFullName.isEmpty()) {
            rootObjects.insert(fullName);
        } else {
            rootObjects.remove(fullName);
            DSInfo& parentInfo = poolInfo.objectsByFullName[dsInfo.parentFullName];
            if (!parentInfo.childFullNames.contains(fullName)) {
                parentInfo.childFullNames.push_back(fullName);
            }
        }
    }

    for (auto it = cache.snapshotsByDataset.cbegin(); it != cache.snapshotsByDataset.cend(); ++it) {
        const QString datasetName = it.key().trimmed();
        if (datasetName.isEmpty()) {
            continue;
        }
        DSInfo& datasetInfo = poolInfo.objectsByFullName[datasetName];
        if (datasetInfo.key.fullName.isEmpty()) {
            datasetInfo.key = DSKey{poolInfo.key, datasetName};
            datasetInfo.kind = DSKind::Filesystem;
            datasetInfo.parentFullName = canonicalDatasetParentName(datasetName);
            datasetInfo.runtime.datasetType = QStringLiteral("filesystem");
            const QString datasetGuidFromCache = cache.objectGuidByName.value(datasetName).trimmed();
            if (!datasetGuidFromCache.isEmpty() && datasetGuidFromCache != QStringLiteral("-")) {
                datasetInfo.runtime.properties.insert(QStringLiteral("guid"), datasetGuidFromCache);
            }
            datasetInfo.editSession.target = datasetInfo.key;
            if (datasetInfo.parentFullName.isEmpty()) {
                rootObjects.insert(datasetName);
            }
        }
        datasetInfo.runtime.directSnapshots = it.value();
        for (const QString& snapNameOnly : it.value()) {
            const QString snapTrimmed = snapNameOnly.trimmed();
            if (snapTrimmed.isEmpty()) {
                continue;
            }
            const QString fullSnapshotName =
                snapTrimmed.startsWith(datasetName + QLatin1Char('@'))
                    ? snapTrimmed
                    : QStringLiteral("%1@%2").arg(datasetName, snapTrimmed);
            DSInfo& snapInfo = poolInfo.objectsByFullName[fullSnapshotName];
            snapInfo.key = DSKey{poolInfo.key, fullSnapshotName};
            snapInfo.kind = DSKind::Snapshot;
            snapInfo.parentFullName = datasetName;
            snapInfo.runtime.datasetType = QStringLiteral("snapshot");
            snapInfo.runtime.propertiesState = LoadState::Loaded;
            snapInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
            const QString snapGuid = cache.objectGuidByName.value(fullSnapshotName).trimmed();
            if (!snapGuid.isEmpty()) {
                snapInfo.runtime.properties.insert(QStringLiteral("guid"), snapGuid);
            }
            snapInfo.editSession.target = snapInfo.key;
            if (!datasetInfo.childFullNames.contains(fullSnapshotName)) {
                datasetInfo.childFullNames.push_back(fullSnapshotName);
            }
        }
    }

    for (auto it = poolInfo.objectsByFullName.begin(); it != poolInfo.objectsByFullName.end(); ++it) {
        auto& children = it->childFullNames;
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
    }
    poolInfo.rootObjectNames = rootObjects.values();
    std::sort(poolInfo.rootObjectNames.begin(), poolInfo.rootObjectNames.end());

    for (auto it = poolInfo.objectsByFullName.begin(); it != poolInfo.objectsByFullName.end(); ++it) {
        if (previousPoolInfo) {
            const auto prevDsIt = previousPoolInfo->objectsByFullName.constFind(it.key());
            if (prevDsIt != previousPoolInfo->objectsByFullName.cend()
                && prevDsIt->runtime.propertiesState == LoadState::Loaded
                && !prevDsIt->runtime.propertyRows.isEmpty()) {
                it->runtime.propertiesState = LoadState::Loaded;
                it->runtime.datasetType =
                    prevDsIt->runtime.datasetType.trimmed().isEmpty()
                        ? it->runtime.datasetType
                        : prevDsIt->runtime.datasetType.trimmed();
                it->runtime.propertyRows = prevDsIt->runtime.propertyRows;
                it->runtime.properties = prevDsIt->runtime.properties;
                it->runtime.loadedPropertyNames = prevDsIt->runtime.loadedPropertyNames;
                it->runtime.allPropertiesLoaded = prevDsIt->runtime.allPropertiesLoaded;
                it->runtime.holdsState = prevDsIt->runtime.holdsState;
                it->runtime.snapshotHolds = prevDsIt->runtime.snapshotHolds;
                it->kind = dsKindFromNames(it.key(), it->runtime.datasetType);
            }
        }

        const QString permsKey = datasetPermissionsCacheKey(connIdx, poolName, it.key());
        const auto permsIt = m_conns.datasetPermissionsCache.constFind(permsKey);
        if (permsIt != m_conns.datasetPermissionsCache.cend() && permsIt->loaded) {
            it->runtime.permissionsState = LoadState::Loaded;
            it->permissionsCache = permsIt.value();
        }
    }

    if (cache.autoSnapshotPropsLoaded) {
        poolInfo.runtime.autoSnapshotPropsByDataset = cache.autoSnapshotPropsByDataset;
        poolInfo.runtime.schedulesState = LoadState::Loaded;
    } else if (previousPoolInfo) {
        poolInfo.runtime.autoSnapshotPropsByDataset = previousPoolInfo->runtime.autoSnapshotPropsByDataset;
        poolInfo.runtime.schedulesState = previousPoolInfo->runtime.schedulesState;
    }
}

void MainWindow::rebuildConnInfoFor(int connIdx) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }

    const ConnInfo* oldConnInfo = findConnInfo(connIdx);

    ConnInfo connInfo;
    connInfo.key.connectionId = connStableIdForIndex(connIdx);
    connInfo.connIdx = connIdx;
    connInfo.profile = m_conns.profiles[connIdx];
    if (connIdx < m_conns.states.size()) {
        connInfo.runtime.state = LoadState::Loaded;
        connInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
        connInfo.runtime.snapshot = m_conns.states[connIdx];
    }

    const ConnectionRuntimeState state = (connIdx < m_conns.states.size()) ? m_conns.states[connIdx] : ConnectionRuntimeState{};
    auto ensurePool = [&](const QString& poolName, const QString& guid) -> PoolInfo& {
        PoolKey key{connInfo.key, guid.trimmed(), poolName.trimmed()};
        const QString stableId = poolStableId(key);
        PoolInfo& poolInfo = connInfo.poolsByStableId[stableId];
        poolInfo.key = key;
        return poolInfo;
    };

    for (const PoolImported& pool : state.importedPools) {
        const QString poolGuid = state.poolGuidByName.value(pool.pool.trimmed()).trimmed();
        PoolInfo& poolInfo = ensurePool(pool.pool, poolGuid);
        poolInfo.runtime.imported = true;
        poolInfo.runtime.importAction = pool.action;
        poolInfo.runtime.poolStatusText = state.poolStatusByName.value(pool.pool.trimmed());
        if (!poolInfo.runtime.poolStatusText.trimmed().isEmpty()) {
            poolInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
        }
    }

    for (const PoolImportable& pool : state.importablePools) {
        PoolInfo& poolInfo = ensurePool(pool.pool, pool.guid);
        poolInfo.runtime.importable = true;
        poolInfo.runtime.importState = pool.state;
        poolInfo.runtime.importReason = pool.reason;
        poolInfo.runtime.importAction = pool.action;
    }

    for (auto it = connInfo.poolsByStableId.begin(); it != connInfo.poolsByStableId.end(); ++it) {
        PoolInfo& poolInfo = it.value();
        const QString cacheKey = poolDetailsCacheKey(connIdx, poolInfo.key.poolName);
        const auto poolDetailsIt = m_conns.poolDetailsCache.constFind(cacheKey);
        if (poolDetailsIt != m_conns.poolDetailsCache.cend() && poolDetailsIt->loaded) {
            poolInfo.runtime.detailsState =
                (!poolDetailsIt->propsRows.isEmpty() || !poolDetailsIt->statusText.trimmed().isEmpty())
                    ? LoadState::Loaded
                    : LoadState::NotLoaded;
            poolInfo.runtime.loadedAt = QDateTime::currentDateTimeUtc();
            poolInfo.runtime.poolStatusText = poolDetailsIt->statusText;
            poolInfo.runtime.zpoolPropertyRows = poolDetailsIt->propsRows;
            for (const QStringList& row : poolDetailsIt->propsRows) {
                if (row.size() >= 2) {
                    poolInfo.runtime.zpoolProperties.insert(row.value(0).trimmed(), row.value(1));
                }
            }
        }
        const PoolInfo* previousPoolInfo = nullptr;
        if (oldConnInfo) {
            const auto prevPoolIt = oldConnInfo->poolsByStableId.constFind(it.key());
            if (prevPoolIt != oldConnInfo->poolsByStableId.cend()) {
                previousPoolInfo = &prevPoolIt.value();
                poolInfo.runtime.schedulesState = previousPoolInfo->runtime.schedulesState;
                poolInfo.runtime.autoSnapshotPropsByDataset = previousPoolInfo->runtime.autoSnapshotPropsByDataset;
            }
        }
        rebuildPoolInfoFromCache(poolInfo, connIdx, poolInfo.key.poolName, previousPoolInfo);
    }

    m_conns.connInfoById.insert(connInfo.key.connectionId, connInfo);
}

void MainWindow::rebuildConnInfoModel() {
    m_conns.connInfoById.clear();
    for (int i = 0; i < m_conns.profiles.size(); ++i) {
        rebuildConnInfoFor(i);
    }
}

const ConnInfo* MainWindow::findConnInfo(int connIdx) const {
    const QString stableId = connStableIdForIndex(connIdx);
    const auto it = m_conns.connInfoById.constFind(stableId);
    return (it == m_conns.connInfoById.cend()) ? nullptr : &it.value();
}

ConnInfo* MainWindow::findConnInfo(int connIdx) {
    const QString stableId = connStableIdForIndex(connIdx);
    const auto it = m_conns.connInfoById.find(stableId);
    return (it == m_conns.connInfoById.end()) ? nullptr : &it.value();
}

const PoolInfo* MainWindow::findPoolInfo(int connIdx, const QString& poolName) const {
    const ConnInfo* connInfo = findConnInfo(connIdx);
    if (!connInfo) {
        return nullptr;
    }
    const QString trimmedPool = poolName.trimmed();
    for (auto it = connInfo->poolsByStableId.cbegin(); it != connInfo->poolsByStableId.cend(); ++it) {
        if (it->key.poolName.trimmed() == trimmedPool) {
            return &it.value();
        }
    }
    return nullptr;
}

PoolInfo* MainWindow::findPoolInfo(int connIdx, const QString& poolName) {
    ConnInfo* connInfo = findConnInfo(connIdx);
    if (!connInfo) {
        return nullptr;
    }
    const QString trimmedPool = poolName.trimmed();
    for (auto it = connInfo->poolsByStableId.begin(); it != connInfo->poolsByStableId.end(); ++it) {
        if (it->key.poolName.trimmed() == trimmedPool) {
            return &it.value();
        }
    }
    return nullptr;
}

const DSInfo* MainWindow::findDsInfo(int connIdx, const QString& poolName, const QString& fullName) const {
    const PoolInfo* poolInfo = findPoolInfo(connIdx, poolName);
    if (!poolInfo) {
        return nullptr;
    }
    const auto it = poolInfo->objectsByFullName.constFind(fullName.trimmed());
    return (it == poolInfo->objectsByFullName.cend()) ? nullptr : &it.value();
}

DSInfo* MainWindow::findDsInfo(int connIdx, const QString& poolName, const QString& fullName) {
    PoolInfo* poolInfo = findPoolInfo(connIdx, poolName);
    if (!poolInfo) {
        return nullptr;
    }
    const auto it = poolInfo->objectsByFullName.find(fullName.trimmed());
    return (it == poolInfo->objectsByFullName.end()) ? nullptr : &it.value();
}

QStringList MainWindow::datasetSnapshotsFromModel(int connIdx, const QString& poolName, const QString& datasetName) const {
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return {};
    }
    return dsInfo->runtime.directSnapshots;
}

bool MainWindow::datasetMountedFromModel(int connIdx, const QString& poolName, const QString& datasetName, QString* mountedValueOut) const {
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return false;
    }
    const QString mountedValue = dsInfo->runtime.properties.value(QStringLiteral("mounted")).trimmed();
    if (mountedValueOut) {
        *mountedValueOut = mountedValue;
    }
    return !mountedValue.isEmpty();
}

bool MainWindow::datasetExistsInModel(int connIdx, const QString& poolName, const QString& datasetName) const {
    return findDsInfo(connIdx, poolName, datasetName) != nullptr;
}

QVector<DatasetPropCacheRow> MainWindow::datasetPropertyRowsFromModelOrCache(int connIdx,
                                                                                         const QString& poolName,
                                                                                         const QString& objectName) const {
    if (const DSInfo* objectInfo = findDsInfo(connIdx, poolName, objectName);
        objectInfo && objectInfo->runtime.propertiesState == LoadState::Loaded
        && !objectInfo->runtime.propertyRows.isEmpty()) {
        return objectInfo->runtime.propertyRows;
    }
    return {};
}

QVector<DatasetPropCacheRow> MainWindow::datasetPropertyRowsForNames(int connIdx,
                                                                                 const QString& poolName,
                                                                                 const QString& objectName,
                                                                                 const QStringList& propNames) const {
    const QVector<DatasetPropCacheRow> rows = datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
    if (propNames.isEmpty() || rows.isEmpty()) {
        return rows;
    }
    QSet<QString> wanted;
    for (const QString& propName : propNames) {
        const QString key = normalizedPropKey(propName);
        if (!key.isEmpty()) {
            wanted.insert(key);
        }
    }
    QVector<DatasetPropCacheRow> filtered;
    for (const DatasetPropCacheRow& row : rows) {
        if (wanted.contains(normalizedPropKey(row.prop))) {
            filtered.push_back(row);
        }
    }
    return filtered;
}

QMap<QString, QString> MainWindow::datasetPropertyValuesForNames(int connIdx,
                                                                 const QString& poolName,
                                                                 const QString& objectName,
                                                                 const QStringList& propNames) const {
    QMap<QString, QString> values;
    for (const DatasetPropCacheRow& row : datasetPropertyRowsForNames(connIdx, poolName, objectName, propNames)) {
        values.insert(row.prop, row.value);
    }
    return values;
}


bool MainWindow::ensureDatasetAllPropertiesLoaded(int connIdx,
                                                  const QString& poolName,
                                                  const QString& objectName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return false;
    }
    DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (dsInfo && dsInfo->runtime.propertiesState == LoadState::Loaded && dsInfo->runtime.allPropertiesLoaded) {
        return true;
    }

    // Copy, not a reference: m_conns.profiles is reassigned wholesale by loadConnections(),
    // which a queued event can trigger while runSsh() pumps the event loop.
    const ConnectionProfile p = m_conns.profiles[connIdx];
    const bool daemonReadApiOk =
        connIdx >= 0
        && connIdx < m_conns.states.size()
        && m_conns.states[connIdx].daemonInstalled
        && m_conns.states[connIdx].daemonActive
        && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    QString datasetType = trimmedObject.contains(QLatin1Char('@')) ? QStringLiteral("snapshot") : QStringLiteral("filesystem");
    if (dsInfo && !dsInfo->runtime.datasetType.trimmed().isEmpty()) {
        datasetType = dsInfo->runtime.datasetType.trimmed();
    }

    if (datasetType.trimmed().isEmpty()) {
        QString tOut, tErr;
        int tRc = -1;
        const bool typeOk =
            daemonReadApiOk
            && runAgentCommand(p, {QStringLiteral("--dump-zfs-get-prop"), QStringLiteral("type"), trimmedObject},
                               12000, tOut, tErr, tRc)
            && tRc == 0;
        if (typeOk) {
            const QString t = tOut.trimmed().toLower();
            if (!t.isEmpty()) {
                datasetType = t;
            }
        }
    }

    QString out;
    QString err;
    int rc = -1;
    // Solo por argv al agente: el respaldo por shell se retiró y la orden que se
    // construía aquí no la usaba nadie. Era, además, la que justificaba la rama TSV de
    // más abajo, que se comía las propiedades en Windows.
    const QStringList propsCmdDaemonArgv = {QStringLiteral("--dump-zfs-get-all"), trimmedObject};
    bool propsOk = daemonReadApiOk
          && runAgentCommand(p, propsCmdDaemonArgv, 20000, out, err, rc)
          && rc == 0;
    if (!propsOk) {
        // Re-look up after the yield: rebuildConnInfoFor() replaces the whole ConnInfo,
        // destroying the nested maps that dsInfo points into.
        if (DSInfo* freshDsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
            freshDsInfo->runtime.propertiesState = LoadState::Error;
            freshDsInfo->runtime.errorText = err.trimmed();
        }
        return false;
    }

    QVector<DatasetPropCacheRow> rows;
    rows.push_back(DatasetPropCacheRow{QStringLiteral("dataset"), trimmedObject, QString(), QStringLiteral("true")});
    // JSON SIEMPRE, también en Windows.
    //
    // El agente responde a --dump-zfs-get-all con `zfs get -j all`, o sea JSON, en todas
    // las plataformas. Aquí había una rama que en Windows lo parseaba como TSV, resto de
    // cuando esa consulta iba por shell con `-o property,value,source`. Al retirarse el
    // respaldo por shell quedó dándole JSON a un parser de tabulaciones: de 83
    // propiedades no salvaba ninguna, y el dataset se quedaba con 2 filas.
    //
    // Efectos vistos en un Windows real: `canmount` no constaba, así que Montar salía
    // deshabilitado para siempre, y las propiedades en línea mostraban una etiqueta y un
    // valor sin sentido, que era lo poco que el parser conseguía sacar.
    {
        const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
        const QJsonObject datasets = doc.object().value(QStringLiteral("datasets")).toObject();
        const QJsonObject dsObj = datasets.value(trimmedObject).toObject();
        const QJsonObject properties = dsObj.value(QStringLiteral("properties")).toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            const QJsonObject propObj = it.value().toObject();
            const QString val = propObj.value(QStringLiteral("value")).toString().trimmed();
            const QJsonObject sourceObj = propObj.value(QStringLiteral("source")).toObject();
            const QString source = sourceObj.value(QStringLiteral("type")).toString().trimmed();
            rows.push_back(DatasetPropCacheRow{it.key(), val, source, QString()});
        }
    }

    storeDatasetPropertyRows(connIdx, trimmedPool, trimmedObject, datasetType, rows);
    if (DSInfo* refreshed = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        refreshed->runtime.propertiesState = LoadState::Loaded;
        refreshed->runtime.loadedAt = QDateTime::currentDateTimeUtc();
        refreshed->runtime.errorText.clear();
        refreshed->runtime.allPropertiesLoaded = true;
        refreshed->runtime.loadedPropertyNames.clear();
        for (const DatasetPropCacheRow& row : refreshed->runtime.propertyRows) {
            refreshed->runtime.loadedPropertyNames.insert(normalizedPropKey(row.prop));
        }
    }
    return true;
}

bool MainWindow::ensureDatasetPropertySubsetLoaded(int connIdx,
                                                   const QString& poolName,
                                                   const QString& objectName,
                                                   const QStringList& propNames) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return false;
    }
    if (propNames.isEmpty()) {
        return ensureDatasetAllPropertiesLoaded(connIdx, trimmedPool, trimmedObject);
    }
    DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (!dsInfo) {
        return false;
    }
    if (dsInfo->runtime.allPropertiesLoaded) {
        return true;
    }

    QStringList wantedProps;
    QSet<QString> missingKeys;
    for (const QString& propName : propNames) {
        const QString key = normalizedPropKey(propName);
        if (key.isEmpty()) {
            continue;
        }
        wantedProps.push_back(propName.trimmed());
        if (!dsInfo->runtime.loadedPropertyNames.contains(key)) {
            missingKeys.insert(key);
        }
    }
    if (missingKeys.isEmpty()) {
        return true;
    }

    // Copy, not a reference: m_conns.profiles is reassigned wholesale by loadConnections(),
    // which a queued event can trigger while runSsh() pumps the event loop.
    const ConnectionProfile p = m_conns.profiles[connIdx];
    const bool daemonReadApiOk =
        connIdx >= 0
        && connIdx < m_conns.states.size()
        && m_conns.states[connIdx].daemonInstalled
        && m_conns.states[connIdx].daemonActive
        && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    QString datasetType = dsInfo->runtime.datasetType.trimmed();
    if (datasetType.isEmpty()) {
        datasetType = trimmedObject.contains(QLatin1Char('@')) ? QStringLiteral("snapshot")
                                                               : QStringLiteral("filesystem");
    }
    QString out;
    QString err;
    int rc = -1;
    QStringList quotedProps;
    for (const QString& propName : wantedProps) {
        quotedProps.push_back(mwhelpers::shSingleQuote(propName.trimmed()));
    }
    // Solo por argv al agente: el respaldo por shell se retiró.
    if (!daemonReadApiOk) {
        requireDaemonForRead(connIdx, QStringLiteral("leer las propiedades de un dataset"));
    }
    const bool propsOk =
        daemonReadApiOk
        && runAgentCommand(p, {QStringLiteral("--dump-zfs-get-json"),
                               wantedProps.join(QLatin1Char(',')), trimmedObject},
                           20000, out, err, rc)
        && rc == 0;
    // Re-look up after the yield: rebuildConnInfoFor() replaces the whole ConnInfo,
    // destroying the nested maps dsInfo points into. Every use below is after this.
    dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (!dsInfo) {
        return false;
    }
    if (!propsOk) {
        dsInfo->runtime.propertiesState = LoadState::Error;
        dsInfo->runtime.errorText = err.trimmed();
        return false;
    }

    QMap<QString, DatasetPropCacheRow> mergedByKey;
    for (const DatasetPropCacheRow& row : dsInfo->runtime.propertyRows) {
        mergedByKey.insert(normalizedPropKey(row.prop), row);
    }
    // JSON siempre: --dump-zfs-get-json responde `zfs get -j` en todas las plataformas.
    // La rama TSV para Windows era el mismo resto del respaldo por shell que dejaba sin
    // propiedades a los datasets y a los pools en esa plataforma.
    {
        const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
        const QJsonObject datasets = doc.object().value(QStringLiteral("datasets")).toObject();
        const QJsonObject dsObj = datasets.value(trimmedObject).toObject();
        const QJsonObject properties = dsObj.value(QStringLiteral("properties")).toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            const QJsonObject propObj = it.value().toObject();
            QString val = propObj.value(QStringLiteral("value")).toString().trimmed();
            const QJsonObject sourceObj = propObj.value(QStringLiteral("source")).toObject();
            QString source = sourceObj.value(QStringLiteral("type")).toString().trimmed();
            val = gsaComparableValue(it.key(), val);
            if (source == QStringLiteral("-")) { source.clear(); }
            DatasetPropCacheRow newRow{it.key(), val, source, QString()};
            mergedByKey.insert(normalizedPropKey(it.key()), newRow);
            dsInfo->runtime.properties.insert(it.key(), val);
            dsInfo->runtime.loadedPropertyNames.insert(normalizedPropKey(it.key()));
        }
    }

    QVector<DatasetPropCacheRow> mergedRows;
    mergedRows.reserve(mergedByKey.size());
    for (auto it = mergedByKey.cbegin(); it != mergedByKey.cend(); ++it) {
        mergedRows.push_back(it.value());
    }
    dsInfo->runtime.propertyRows = mergedRows;
    dsInfo->runtime.propertiesState = LoadState::Loaded;
    dsInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
    dsInfo->runtime.errorText.clear();
    dsInfo->runtime.datasetType = datasetType;
    return true;
}

void MainWindow::storeDatasetPropertyRows(int connIdx,
                                          const QString& poolName,
                                          const QString& objectName,
                                          const QString& datasetType,
                                          const QVector<DatasetPropCacheRow>& rows) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return;
    }

    if (DSInfo* objectInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        objectInfo->runtime.propertiesState = LoadState::Loaded;
        objectInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
        objectInfo->runtime.errorText.clear();
        objectInfo->runtime.datasetType = datasetType.trimmed();
        objectInfo->runtime.propertyRows = rows;
        objectInfo->runtime.properties.clear();
        objectInfo->runtime.loadedPropertyNames.clear();
        for (const DatasetPropCacheRow& row : rows) {
            objectInfo->runtime.properties.insert(row.prop.trimmed(), row.value);
            objectInfo->runtime.loadedPropertyNames.insert(normalizedPropKey(row.prop));
        }
        objectInfo->runtime.allPropertiesLoaded = true;
        objectInfo->kind = dsKindFromNames(trimmedObject, datasetType.trimmed());
    } else {
        rebuildConnInfoFor(connIdx);
        if (DSInfo* rebuilt = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
            rebuilt->runtime.propertiesState = LoadState::Loaded;
            rebuilt->runtime.loadedAt = QDateTime::currentDateTimeUtc();
            rebuilt->runtime.errorText.clear();
            rebuilt->runtime.datasetType = datasetType.trimmed();
            rebuilt->runtime.propertyRows = rows;
            rebuilt->runtime.properties.clear();
            rebuilt->runtime.loadedPropertyNames.clear();
            for (const DatasetPropCacheRow& row : rows) {
                rebuilt->runtime.properties.insert(row.prop.trimmed(), row.value);
                rebuilt->runtime.loadedPropertyNames.insert(normalizedPropKey(row.prop));
            }
            rebuilt->runtime.allPropertiesLoaded = true;
            rebuilt->kind = dsKindFromNames(trimmedObject, datasetType.trimmed());
        }
    }
}

void MainWindow::removeDatasetPropertyEntry(int connIdx, const QString& poolName, const QString& objectName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return;
    }
    if (DSInfo* objectInfo = findDsInfo(connIdx, trimmedPool, trimmedObject)) {
        objectInfo->runtime.propertiesState = LoadState::NotLoaded;
        objectInfo->runtime.propertyRows.clear();
        objectInfo->runtime.properties.clear();
        objectInfo->runtime.loadedPropertyNames.clear();
        objectInfo->runtime.allPropertiesLoaded = false;
    }
}

void MainWindow::removeDatasetPropertyEntriesForPool(int connIdx, const QString& poolName) {
    if (PoolInfo* poolInfo = findPoolInfo(connIdx, poolName)) {
        for (auto itDs = poolInfo->objectsByFullName.begin(); itDs != poolInfo->objectsByFullName.end(); ++itDs) {
            itDs->runtime.propertiesState = LoadState::NotLoaded;
            itDs->runtime.propertyRows.clear();
            itDs->runtime.properties.clear();
            itDs->runtime.loadedPropertyNames.clear();
            itDs->runtime.allPropertiesLoaded = false;
        }
    }
}

MainWindow::DatasetPropsDraft MainWindow::propertyDraftForObject(const QString& side,
                                                                const QString& token,
                                                                const QString& objectName) const {
    int connIdx = -1;
    QString poolName;
    if (!splitConnToken(token, connIdx, poolName)) {
        return {};
    }
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, objectName);
    if (!dsInfo) {
        return {};
    }

    DatasetPropsDraft draft;
    for (auto itProp = dsInfo->editSession.propertyEdits.byName.cbegin();
         itProp != dsInfo->editSession.propertyEdits.byName.cend();
         ++itProp) {
        if (!itProp->dirty()) {
            continue;
        }
        if (itProp->valueDirty) {
            draft.valuesByProp.insert(itProp.key(), itProp->value);
        }
        if (itProp->inheritDirty) {
            draft.inheritByProp.insert(itProp.key(), itProp->inherit);
        }
    }
    draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();
    return draft;
}

void MainWindow::storePropertyDraftForObject(const QString& side,
                                            const QString& token,
                                            const QString& objectName,
                                            const DatasetPropsDraft& draftIn) {
    const QString normSide = side.trimmed().toLower();
    const QString normToken = token.trimmed();
    const QString normObject = objectName.trimmed();
    if (normSide.isEmpty() || normToken.isEmpty() || normObject.isEmpty()) {
        return;
    }

    DatasetPropsDraft draft = draftIn;
    int connIdx = -1;
    QString poolName;
    if (!splitConnToken(normToken, connIdx, poolName)) {
        return;
    }
    DSInfo* dsInfo = findDsInfo(connIdx, poolName, normObject);
    if (!dsInfo) {
        return;
    }

    const QVector<DatasetPropCacheRow> originalRows = datasetPropertyRowsFromModelOrCache(connIdx, poolName, normObject);
    QMap<QString, QString> originalValues;
    QMap<QString, bool> originalInherit;
    for (const DatasetPropCacheRow& row : originalRows) {
        originalValues.insert(row.prop, gsaComparableValue(row.prop, row.value));
        originalInherit.insert(row.prop, row.source.trimmed().toLower().startsWith(QStringLiteral("inherited")));
    }
    const auto runtimeProps = dsInfo->runtime.properties;
    for (auto it = runtimeProps.cbegin(); it != runtimeProps.cend(); ++it) {
        const QString prop = it.key().trimmed();
        if (prop.isEmpty() || originalValues.contains(prop)) {
            continue;
        }
        originalValues.insert(prop, gsaComparableValue(prop, it.value()));
    }
    auto valueIt = draft.valuesByProp.begin();
    while (valueIt != draft.valuesByProp.end()) {
        const QString originalValue = gsaComparableValue(valueIt.key(), originalValues.value(valueIt.key()));
        const QString currentValue = gsaComparableValue(valueIt.key(), valueIt.value());
        bool erase = false;
        if (valueIt.key().compare(QStringLiteral("mounted"), Qt::CaseInsensitive) == 0) {
            bool currentMounted = false;
            bool originalMounted = false;
            if (mountedStateFromAnyText(currentValue, &currentMounted)
                && mountedStateFromAnyText(originalValue, &originalMounted)
                && currentMounted == originalMounted
                && !draft.inheritByProp.contains(valueIt.key())) {
                erase = true;
            }
        } else if (currentValue == originalValue && !draft.inheritByProp.contains(valueIt.key())) {
            erase = true;
        }
        if (erase) {
            valueIt = draft.valuesByProp.erase(valueIt);
        } else {
            ++valueIt;
        }
    }
    auto inheritIt = draft.inheritByProp.begin();
    while (inheritIt != draft.inheritByProp.end()) {
        const bool originalInherited = originalInherit.value(inheritIt.key(), false);
        if (inheritIt.value() == originalInherited && !draft.valuesByProp.contains(inheritIt.key())) {
            inheritIt = draft.inheritByProp.erase(inheritIt);
        } else {
            ++inheritIt;
        }
    }
    draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();

    dsInfo->editSession.target = dsInfo->key;
    dsInfo->editSession.propertyEdits.clear();
    for (auto itProp = draft.valuesByProp.cbegin(); itProp != draft.valuesByProp.cend(); ++itProp) {
        DSPropertyEditValue value;
        value.value = itProp.value();
        value.valueDirty = true;
        if (draft.inheritByProp.contains(itProp.key())) {
            value.inherit = draft.inheritByProp.value(itProp.key(), false);
            value.inheritDirty = true;
        }
        dsInfo->editSession.propertyEdits.byName.insert(itProp.key(), value);
    }
    for (auto itInh = draft.inheritByProp.cbegin(); itInh != draft.inheritByProp.cend(); ++itInh) {
        auto existing = dsInfo->editSession.propertyEdits.byName.find(itInh.key());
        if (existing == dsInfo->editSession.propertyEdits.byName.end()) {
            DSPropertyEditValue value;
            value.inherit = itInh.value();
            value.inheritDirty = true;
            dsInfo->editSession.propertyEdits.byName.insert(itInh.key(), value);
        } else {
            existing->inherit = itInh.value();
            existing->inheritDirty = true;
        }
    }
}

QVector<MainWindow::PendingPropertyDraftEntry> MainWindow::pendingConnContentPropertyDraftsFromModel() const {
    QVector<PendingPropertyDraftEntry> drafts;
    for (auto itConn = m_conns.connInfoById.cbegin(); itConn != m_conns.connInfoById.cend(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.cbegin(); itPool != itConn->poolsByStableId.cend(); ++itPool) {
            const QString poolName = itPool->key.poolName.trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            const QString token = QStringLiteral("%1::%2").arg(connToken(itConn->connIdx), poolName);
            for (auto itDs = itPool->objectsByFullName.cbegin(); itDs != itPool->objectsByFullName.cend(); ++itDs) {
                const DatasetPropsDraft draft =
                    propertyDraftForObject(QStringLiteral("conncontent"), token, itDs.key());
                if (!draft.dirty) {
                    continue;
                }
                PendingPropertyDraftEntry entry;
                entry.connIdx = itConn->connIdx;
                entry.poolName = poolName;
                entry.token = token;
                entry.objectName = itDs.key();
                entry.draft = draft;
                drafts.push_back(entry);
            }
        }
    }
    return drafts;
}

const DatasetPermissionsCacheEntry* MainWindow::datasetPermissionsEntry(int connIdx,
                                                                                   const QString& poolName,
                                                                                   const QString& datasetName) const {
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (dsInfo && dsInfo->runtime.permissionsState == LoadState::Loaded && dsInfo->permissionsCache.loaded) {
        return &dsInfo->permissionsCache;
    }
    const QString key = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
    const auto it = m_conns.datasetPermissionsCache.constFind(key);
    return (it == m_conns.datasetPermissionsCache.cend()) ? nullptr : &it.value();
}

const DatasetPermissionsCacheEntry* MainWindow::ensureDatasetPermissionsEntryLoaded(int connIdx,
                                                                                               const QString& poolName,
                                                                                               const QString& datasetName) {
    if (!ensureDatasetPermissionsLoaded(connIdx, poolName, datasetName)) {
        return nullptr;
    }
    return datasetPermissionsEntry(connIdx, poolName, datasetName);
}

const PoolDetailsCacheEntry* MainWindow::poolDetailsEntry(int connIdx, const QString& poolName) const {
    const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
    const auto it = m_conns.poolDetailsCache.constFind(cacheKey);
    return (it == m_conns.poolDetailsCache.cend()) ? nullptr : &it.value();
}

bool MainWindow::ensurePoolDetailsLoaded(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return false;
    }
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
        poolInfo && poolInfo->runtime.detailsState == LoadState::Loaded
        && (!poolInfo->runtime.zpoolPropertyRows.isEmpty() || !poolInfo->runtime.poolStatusText.trimmed().isEmpty())) {
        return true;
    }
    const PoolDetailsCacheEntry* cached = poolDetailsEntry(connIdx, trimmedPool);
    if (cached && cached->loaded
        && (!cached->propsRows.isEmpty() || !cached->statusText.trimmed().isEmpty())) {
        return true;
    }
    schedulePoolDetailsLoad(connIdx, trimmedPool);
    return false;
}

void MainWindow::schedulePoolDetailsLoad(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    const QString key = poolDetailsCacheKey(connIdx, trimmedPool);
    if (m_poolDetailsLoadsInFlight.contains(key)) {
        return;
    }
    m_poolDetailsLoadsInFlight.insert(key);
    const ConnectionProfile profile = m_conns.profiles[connIdx];
    const bool daemonReadApiOk =
        connIdx >= 0
        && connIdx < m_conns.states.size()
        && m_conns.states[connIdx].daemonInstalled
        && m_conns.states[connIdx].daemonActive
        && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    (void)QtConcurrent::run([this, profile, connIdx, trimmedPool, daemonReadApiOk]() {
        PoolDetailsCacheEntry fresh;
        QString errorText;
        {
            QString out;
            QString err;
            int rc = -1;
            // Solo por argv al agente, igual que en las propiedades de dataset.
            // Por argv cuando hay daemon: la orden no pasa por ninguna cadena de shell.
            const QStringList propsCmdDaemonArgv = {QStringLiteral("--dump-zpool-get-all"), trimmedPool};
            bool propsOk = daemonReadApiOk
                  && runAgentCommand(profile, propsCmdDaemonArgv, 20000, out, err, rc)
                  && rc == 0;
            if (propsOk) {
                // JSON siempre, igual que en las propiedades de dataset: el agente
                // responde a --dump-zpool-get-all con `zpool get -j all` en todas las
                // plataformas, y la rama TSV para Windows era resto del respaldo por
                // shell ya retirado. Dejaba los pools de Windows sin propiedades.
                {
                    const QJsonDocument doc = QJsonDocument::fromJson(mwhelpers::stripToJson(out).toUtf8());
                    const QJsonObject pools = doc.object().value(QStringLiteral("pools")).toObject();
                    const QJsonObject poolObj = pools.value(trimmedPool).toObject();
                    const QJsonObject properties = poolObj.value(QStringLiteral("properties")).toObject();
                    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
                        const QJsonObject propObj = it.value().toObject();
                        const QString value = propObj.value(QStringLiteral("value")).toString().trimmed();
                        const QJsonObject sourceObj = propObj.value(QStringLiteral("source")).toObject();
                        const QString source = sourceObj.value(QStringLiteral("type")).toString().trimmed();
                        fresh.propsRows.push_back(QStringList{it.key(), value, source});
                    }
                }
            } else {
                errorText = err.trimmed();
            }

            out.clear();
            err.clear();
            rc = -1;
            // Por argv cuando hay daemon: la orden no pasa por ninguna cadena de shell.
            const QStringList stCmdDaemonArgv = {QStringLiteral("--dump-zpool-status"), trimmedPool};
            bool statusOk = daemonReadApiOk
                  && runAgentCommand(profile, stCmdDaemonArgv, 20000, out, err, rc)
                  && rc == 0;
            if (statusOk) {
                fresh.statusText = out.trimmed();
            } else {
                const QString statusErr = err.trimmed();
                fresh.statusText = statusErr;
                if (errorText.isEmpty()) {
                    errorText = statusErr;
                }
            }

            out.clear();
            err.clear();
            rc = -1;
            // Por argv cuando hay daemon: la orden no pasa por ninguna cadena de shell.
            const QStringList stPCmdDaemonArgv = {QStringLiteral("--dump-zpool-status-p"), trimmedPool};
            bool statusPOk = daemonReadApiOk
                  && runAgentCommand(profile, stPCmdDaemonArgv, 20000, out, err, rc)
                  && rc == 0;
            if (statusPOk) {
                fresh.statusPText = out.trimmed();
            } else {
                fresh.statusPText.clear();
            }
        }
        fresh.loaded = true;
        const bool ok = errorText.isEmpty() || !fresh.propsRows.isEmpty() || !fresh.statusText.trimmed().isEmpty();
        QMetaObject::invokeMethod(this, [this, connIdx, trimmedPool, ok, fresh, errorText]() {
            applyPoolDetailsLoadResult(connIdx, trimmedPool, ok, fresh, errorText);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::applyPoolDetailsLoadResult(int connIdx,
                                            const QString& poolName,
                                            bool ok,
                                            const PoolDetailsCacheEntry& fresh,
                                            const QString& errorText) {
    const QString trimmedPool = poolName.trimmed();
    const QString key = poolDetailsCacheKey(connIdx, trimmedPool);
    m_poolDetailsLoadsInFlight.remove(key);
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    Q_UNUSED(ok);
    Q_UNUSED(errorText);
    m_conns.poolDetailsCache.insert(key, fresh);
    rebuildConnInfoFor(connIdx);
    applyPoolRootTooltipToVisibleTrees(connIdx, trimmedPool, fresh.statusText);
    const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool);
    if (m_connContentTree) {
        syncConnContentPoolColumnsFor(m_connContentTree, token);
    }
    if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
        syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
    }
    const int row = selectedPoolRowFromTabs();
    if (row >= 0 && row < m_conns.poolListEntries.size()) {
        const auto& pe = m_conns.poolListEntries[row];
        if (findConnectionIndexByName(pe.connection) == connIdx
            && pe.pool.trimmed().compare(trimmedPool, Qt::CaseInsensitive) == 0) {
            refreshSelectedPoolDetails(false, false);
        }
    }
}

bool MainWindow::ensurePoolAutoSnapshotInfoLoaded(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return false;
    }
    PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
    if (poolInfo && poolInfo->runtime.schedulesState == LoadState::Loaded) {
        return true;
    }
    if (poolInfo && poolInfo->runtime.schedulesState == LoadState::Error) {
        return false;
    }
    if (!featureAvailable(connIdx, zfsmgr::caps::Feature::AutoSnapshotsGsa)) {
        return false;
    }
    if (isPoolSuspended(connIdx, trimmedPool)) {
        if (poolInfo) {
            poolInfo->runtime.schedulesState = LoadState::Error;
            poolInfo->runtime.errorText = QStringLiteral("pool suspended");
        }
        return false;
    }
    if (poolInfo && poolInfo->runtime.schedulesState == LoadState::Loading) {
        return false;
    }
    schedulePoolAutoSnapshotInfoLoad(connIdx, trimmedPool);
    return false;
}

void MainWindow::invalidatePoolAutoSnapshotInfoForConnection(int connIdx) {
    if (ConnInfo* connInfo = findConnInfo(connIdx)) {
        for (auto itPool = connInfo->poolsByStableId.begin(); itPool != connInfo->poolsByStableId.end(); ++itPool) {
            itPool->runtime.schedulesState = LoadState::NotLoaded;
            itPool->runtime.autoSnapshotPropsByDataset.clear();
        }
    }
    const QString prefix = QStringLiteral("%1::").arg(connToken(connIdx));
    for (auto it = m_poolAutoSnapshotLoadsInFlight.begin(); it != m_poolAutoSnapshotLoadsInFlight.end();) {
        if (it->startsWith(prefix)) {
            it = m_poolAutoSnapshotLoadsInFlight.erase(it);
        } else {
            ++it;
        }
    }
    m_poolAutoSnapshotPendingLoadsByConn.remove(connIdx);
    m_poolAutoSnapshotDirtyPoolsByConn.remove(connIdx);
    m_poolAutoSnapshotUiDeferByConn.remove(connIdx);
}

void MainWindow::preloadPoolAutoSnapshotInfoForConnection(int connIdx) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || !featureAvailable(connIdx, zfsmgr::caps::Feature::AutoSnapshotsGsa)) {
        return;
    }
    const ConnectionRuntimeState state =
        (connIdx < m_conns.states.size()) ? m_conns.states[connIdx] : ConnectionRuntimeState{};
    int startedLoads = 0;
    for (const PoolImported& pool : state.importedPools) {
        const QString trimmedPool = pool.pool.trimmed();
        if (trimmedPool.isEmpty()) {
            continue;
        }
        if (schedulePoolAutoSnapshotInfoLoad(connIdx, trimmedPool)) {
            ++startedLoads;
        }
    }
    if (startedLoads > 0) {
        m_poolAutoSnapshotUiDeferByConn.insert(connIdx);
    }
}

bool MainWindow::schedulePoolAutoSnapshotInfoLoad(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()
        || !featureAvailable(connIdx, zfsmgr::caps::Feature::AutoSnapshotsGsa)) {
        return false;
    }
    if (isPoolSuspended(connIdx, trimmedPool)) {
        if (PoolInfo* p = findPoolInfo(connIdx, trimmedPool)) {
            p->runtime.schedulesState = LoadState::Error;
            p->runtime.errorText = QStringLiteral("pool suspended");
        }
        return false;
    }
    PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
    if (poolInfo && (poolInfo->runtime.schedulesState == LoadState::Loaded
                     || poolInfo->runtime.schedulesState == LoadState::Loading)) {
        return false;
    }
    const QString key = QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool);
    if (m_poolAutoSnapshotLoadsInFlight.contains(key)) {
        return false;
    }
    if (!poolInfo) {
        rebuildConnInfoFor(connIdx);
        poolInfo = findPoolInfo(connIdx, trimmedPool);
    }
    if (poolInfo) {
        poolInfo->runtime.schedulesState = LoadState::Loading;
        poolInfo->runtime.errorText.clear();
    }
    m_poolAutoSnapshotLoadsInFlight.insert(key);
    m_poolAutoSnapshotPendingLoadsByConn[connIdx] =
        m_poolAutoSnapshotPendingLoadsByConn.value(connIdx, 0) + 1;
    const ConnectionProfile profile = m_conns.profiles[connIdx];
    const bool daemonReadApiOk =
        connIdx >= 0
        && connIdx < m_conns.states.size()
        && m_conns.states[connIdx].daemonInstalled
        && m_conns.states[connIdx].daemonActive
        && m_conns.states[connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    (void)QtConcurrent::run([this, profile, connIdx, trimmedPool, daemonReadApiOk]() {
        PoolAutoSnapshotLoadResult result;
        result.connIdx = connIdx;
        result.poolName = trimmedPool;
        const QStringList gsaProps = gsaPropertyKeysForModel();
        QStringList propArgs;
        for (const QString& prop : gsaProps) {
            propArgs << mwhelpers::shSingleQuote(prop);
        }
        const QString cmdDaemon =
            mwhelpers::agentShellCommand(profile, {QStringLiteral("--dump-zfs-get-gsa-raw-recursive"), trimmedPool});
        QString out;
        QString err;
        int rc = -1;
        const bool scanOk =
            daemonReadApiOk
            && runAgentCommand(profile, {QStringLiteral("--dump-zfs-get-gsa-raw-recursive"), trimmedPool},
                               20000, out, err, rc)
            && rc == 0;
        if (!scanOk) {
            result.errorText = err.trimmed();
        } else {
            auto isLocallyConfiguredGsaSource = [](const QString& source) {
                const QString src = source.trimmed().toLower();
                if (src.isEmpty() || src == QStringLiteral("-")) {
                    return false;
                }
                if (src.startsWith(QStringLiteral("inherited")) || src == QStringLiteral("default")) {
                    return false;
                }
                return true;
            };
            for (const QString& line : out.split(QLatin1Char('\n'))) {
                const QString trimmed = line.trimmed();
                if (trimmed.isEmpty()) {
                    continue;
                }
                const QStringList cols = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                                       Qt::SkipEmptyParts);
                if (cols.size() < 4) {
                    continue;
                }
                const QString dsName = cols.at(0).trimmed();
                const QString prop = cols.at(1).trimmed();
                const QString value = cols.at(2).trimmed();
                const QString source = cols.mid(3).join(QStringLiteral(" ")).trimmed();
                if (dsName.isEmpty() || dsName.contains(QLatin1Char('@'))) {
                    continue;
                }
                if (prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)
                    && isLocallyConfiguredGsaSource(source)) {
                    result.loaded[dsName].insert(prop, value);
                }
            }
            result.ok = true;
        }
        QMetaObject::invokeMethod(this, [this, result]() {
            applyPoolAutoSnapshotInfoLoadResult(result.connIdx,
                                                result.poolName,
                                                result.ok,
                                                result.errorText,
                                                result.loaded);
        }, Qt::QueuedConnection);
    });
    return true;
}

void MainWindow::applyPoolAutoSnapshotInfoLoadResult(
    int connIdx,
    const QString& poolName,
    bool ok,
    const QString& errorText,
    const QMap<QString, QMap<QString, QString>>& loaded) {
    const QString trimmedPool = poolName.trimmed();
    const QString key = QStringLiteral("%1::%2").arg(connToken(connIdx), trimmedPool);
    m_poolAutoSnapshotLoadsInFlight.remove(key);
    int pendingLoads = m_poolAutoSnapshotPendingLoadsByConn.value(connIdx, 0);
    if (pendingLoads > 0) {
        --pendingLoads;
    }
    if (pendingLoads <= 0) {
        m_poolAutoSnapshotPendingLoadsByConn.remove(connIdx);
        pendingLoads = 0;
    } else {
        m_poolAutoSnapshotPendingLoadsByConn[connIdx] = pendingLoads;
    }
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty()) {
        return;
    }
    PoolInfo* poolInfo = findPoolInfo(connIdx, trimmedPool);
    if (!poolInfo) {
        rebuildConnInfoFor(connIdx);
        poolInfo = findPoolInfo(connIdx, trimmedPool);
    }
    if (!poolInfo) {
        return;
    }
    if (ok) {
        poolInfo->runtime.autoSnapshotPropsByDataset = loaded;
        poolInfo->runtime.schedulesState = LoadState::Loaded;
        poolInfo->runtime.errorText.clear();
    } else {
        poolInfo->runtime.schedulesState = LoadState::Error;
        poolInfo->runtime.errorText = errorText.trimmed();
    }
    const bool deferUi = m_poolAutoSnapshotUiDeferByConn.contains(connIdx);
    if (deferUi) {
        m_poolAutoSnapshotDirtyPoolsByConn[connIdx].insert(trimmedPool);
        if (pendingLoads > 0) {
            return;
        }
    }

    QSet<QString> poolsToSync;
    if (deferUi) {
        poolsToSync = m_poolAutoSnapshotDirtyPoolsByConn.value(connIdx);
        m_poolAutoSnapshotDirtyPoolsByConn.remove(connIdx);
        m_poolAutoSnapshotUiDeferByConn.remove(connIdx);
    } else {
        poolsToSync.insert(trimmedPool);
    }
    for (const QString& pool : poolsToSync) {
        const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), pool);
        if (m_connContentTree) {
            syncConnContentPoolColumnsFor(m_connContentTree, token);
        }
        if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
            syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
        }
    }
}

bool MainWindow::executePoolCommand(int connIdx,
                                    const QString& poolName,
                                    const QString& actionName,
                                    const QString& remoteCmd,
                                    int timeoutMs,
                                    QString* failureDetailOut,
                                    bool refreshPoolsTable,
                                    bool refreshSelectedPoolDetailsAfter) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedAction = actionName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty() || trimmedAction.isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid pool command context");
        }
        return false;
    }
    if (isPoolSuspended(connIdx, trimmedPool)) {
        const QString detail = QStringLiteral("pool is suspended");
        if (failureDetailOut) {
            *failureDetailOut = detail;
        }
        appLog(QStringLiteral("WARN"),
               QStringLiteral("Bloqueado %1 sobre %2::%3 (%4)")
                   .arg(trimmedAction, m_conns.profiles[connIdx].name, trimmedPool, detail));
        return false;
    }

    const ConnectionProfile p = m_conns.profiles[connIdx];
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Inicio %1 %2::%3").arg(trimmedAction.toLower(), p.name, trimmedPool));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, timeoutMs, out, err, rc) && rc == 0;
    setActionsLocked(false);
    if (!ok) {
        const QString detail = mwhelpers::oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err);
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error %1 %2::%3 -> %4")
                   .arg(trimmedAction.toLower(), p.name, trimmedPool, detail));
        if (failureDetailOut) {
            *failureDetailOut = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
        }
        return false;
    }

    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Fin %1 %2::%3").arg(trimmedAction.toLower(), p.name, trimmedPool));
    refreshConnectionByIndex(connIdx);
    if (refreshPoolsTable) {
        populateAllPoolsTables();
    }
    if (refreshSelectedPoolDetailsAfter) {
        refreshSelectedPoolDetails(true, true);
    }
    return true;
}

// QProcess::splitCommand only handles double-quoted strings; single-quoted arguments
// (produced by shSingleQuote) would be passed to execvp with literal quote characters.
static QStringList splitShellCommand(const QString& cmd) {
    QStringList result;
    QString current;
    bool inSingle = false;
    bool inDouble = false;
    for (const QChar c : cmd) {
        if (inSingle) {
            if (c == QLatin1Char('\'')) inSingle = false;
            else current += c;
        } else if (inDouble) {
            if (c == QLatin1Char('"')) inDouble = false;
            else current += c;
        } else if (c == QLatin1Char('\'')) {
            inSingle = true;
        } else if (c == QLatin1Char('"')) {
            inDouble = true;
        } else if (c.isSpace()) {
            if (!current.isEmpty()) { result << current; current.clear(); }
        } else {
            current += c;
        }
    }
    if (!current.isEmpty()) result << current;
    return result;
}

QStringList MainWindow::daemonizeZpoolMutationArgs(int connIdx, const QString& rawCmd) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return {};
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    if (connIdx < 0 || connIdx >= m_conns.states.size()) {
        return {};
    }
    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    if (!st.daemonInstalled || !st.daemonActive) {
        return {};
    }
    if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        return {};
    }
    const QStringList parts = splitShellCommand(rawCmd.trimmed());
    if (parts.size() < 2 || parts.first().trimmed() != QStringLiteral("zpool")) {
        return {};
    }
    const QString op = parts.at(1).trimmed();
    if (!isAllowedGenericZpoolMutationOpClient(op)) {
        return {};
    }
    if (op.compare(QStringLiteral("import"), Qt::CaseInsensitive) == 0
        && !st.daemonZpoolImportUsable) {
        return {};
    }
    QJsonArray arr;
    for (int i = 1; i < parts.size(); ++i) {
        arr.push_back(parts.at(i));
    }
    if (arr.isEmpty()) {
        return {};
    }
    const QString payloadB64 =
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact).toBase64());
    return {QStringLiteral("--mutate-zpool-generic"), payloadB64};
}

QStringList MainWindow::daemonizeZfsMutationArgs(int connIdx, const QString& rawCmd) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return {};
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    if (connIdx < 0 || connIdx >= m_conns.states.size()) {
        return {};
    }
    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    if (!st.daemonInstalled || !st.daemonActive) {
        return {};
    }
    if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        return {};
    }
    const QStringList parts = splitShellCommand(rawCmd.trimmed());
    if (parts.size() < 2 || parts.first().trimmed() != QStringLiteral("zfs")) {
        return {};
    }
    const QString op = parts.at(1).trimmed();
    if (!isAllowedGenericZfsMutationOpClient(op)) {
        return {};
    }
    QJsonArray arr;
    for (int i = 1; i < parts.size(); ++i) {
        arr.push_back(parts.at(i));
    }
    if (arr.isEmpty()) {
        return {};
    }
    const QString payloadB64 =
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact).toBase64());
    return {QStringLiteral("--mutate-zfs-generic"), payloadB64};
}

QStringList MainWindow::daemonizeLocalSendRecvArgs(int connIdx,
                                                  const QString& sendRawCmd,
                                                  const QString& recvRawCmd) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || connIdx >= m_conns.states.size()) {
        return {};
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    // El agente de Windows no sirve --zfs-pipe-local (responde "unknown command",
    // comprobado por RPC), así que la mutación se queda en el camino clásico.
    if (isWindowsConnection(p)) {
        return {};
    }
    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    if (!st.daemonInstalled || !st.daemonActive) {
        return {};
    }
    if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        return {};
    }

    // Both sides must be plain `zfs send ...` / `zfs recv ...` with no shell
    // operators; anything else keeps the shell path so its semantics are honored.
    auto toArgv = [](const QString& rawCmd, const QString& expectOp) -> QJsonArray {
        const QString trimmed = rawCmd.trimmed();
        if (trimmed.contains(QLatin1Char('&')) || trimmed.contains(QLatin1Char('|'))
            || trimmed.contains(QLatin1Char('`')) || trimmed.contains(QLatin1Char('>'))
            || trimmed.contains(QLatin1Char('<')) || trimmed.contains(QStringLiteral("$("))) {
            return QJsonArray();
        }
        const QStringList parts = splitShellCommand(trimmed);
        if (parts.size() < 2 || parts.first().trimmed() != QStringLiteral("zfs")) {
            return QJsonArray();
        }
        if (parts.at(1).trimmed().toLower() != expectOp) {
            return QJsonArray();
        }
        QJsonArray argv;
        for (int i = 1; i < parts.size(); ++i) {
            argv.push_back(parts.at(i));
        }
        return argv;
    };

    const QJsonArray sendArgv = toArgv(sendRawCmd, QStringLiteral("send"));
    const QJsonArray recvArgv = toArgv(recvRawCmd, QStringLiteral("recv"));
    if (sendArgv.isEmpty() || recvArgv.isEmpty()) {
        return {};
    }

    QJsonArray outer;
    outer.push_back(QString::fromLatin1(
        QJsonDocument(sendArgv).toJson(QJsonDocument::Compact).toBase64()));
    outer.push_back(QString::fromLatin1(
        QJsonDocument(recvArgv).toJson(QJsonDocument::Compact).toBase64()));
    const QString payloadB64 = QString::fromLatin1(
        QJsonDocument(outer).toJson(QJsonDocument::Compact).toBase64());
    return {QStringLiteral("--zfs-pipe-local"), payloadB64};
}

QStringList MainWindow::daemonizeRsyncSyncArgs(int connIdx,
                                              const QList<QPair<QString, QString>>& pathPairs,
                                              bool useDelete,
                                              bool dryRun,
                                              const QString& rsh,
                                              const QString& dstHost) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || connIdx >= m_conns.states.size()) {
        return {};
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    if (isWindowsConnection(p)) {
        return {};
    }
    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    if (!st.daemonInstalled || !st.daemonActive) {
        return {};
    }
    if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        return {};
    }
    if (pathPairs.isEmpty()) {
        return {};
    }

    QJsonArray fields;
    fields.push_back(useDelete ? QStringLiteral("1") : QStringLiteral("0"));
    fields.push_back(dryRun ? QStringLiteral("1") : QStringLiteral("0"));
    fields.push_back(rsh);
    fields.push_back(dstHost);
    for (const QPair<QString, QString>& pair : pathPairs) {
        const QString src = pair.first.trimmed();
        const QString dst = pair.second.trimmed();
        if (src.isEmpty() || dst.isEmpty()
            || !src.startsWith(QLatin1Char('/')) || !dst.startsWith(QLatin1Char('/'))) {
            return {};
        }
        fields.push_back(src);
        fields.push_back(dst);
    }
    const QString payloadB64 = QString::fromLatin1(
        QJsonDocument(fields).toJson(QJsonDocument::Compact).toBase64());
    return {QStringLiteral("--mutate-rsync-local"), payloadB64};
}

// Sincronizar dos directorios de la MISMA máquina con la copia propia del agente, sin
// rsync. Es lo que hace posible Sincronizar en Windows, donde rsync no existe y el
// respaldo por tar/ssh no puede funcionar: el stdio de un comando remoto por SSH se
// corta a 132 KiB, y además ese respaldo nunca borraba lo que sobraba, así que no
// sincronizaba, copiaba.
//
// Solo para la misma máquina. Entre máquinas distintas rsync manda solo las
// diferencias por la red, y sustituirlo por esto sería un retroceso.
QStringList MainWindow::daemonizeCopyTreeSyncArgs(int connIdx,
                                                  const QString& srcPath,
                                                  const QString& dstPath,
                                                  bool useDelete,
                                                  bool dryRun) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || connIdx >= m_conns.states.size()) {
        return {};
    }
    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    if (!st.daemonInstalled || !st.daemonActive) {
        return {};
    }
    if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        return {};
    }
    const QString src = srcPath.trimmed();
    const QString dst = dstPath.trimmed();
    if (src.isEmpty() || dst.isEmpty()) {
        return {};
    }
    // Misma comprobación que isUsableMountPath: en Windows un punto de montaje lleva
    // letra de unidad y no empieza por '/'.
    const bool win = isWindowsConnection(m_conns.profiles[connIdx]);
    const auto absolute = [win](const QString& v) {
        return win ? v.contains(QLatin1Char(':')) : v.startsWith(QLatin1Char('/'));
    };
    if (!absolute(src) || !absolute(dst)) {
        return {};
    }
    QStringList args{QStringLiteral("--mutate-copy-tree"), src, dst};
    if (useDelete) {
        args << QStringLiteral("--delete");
    }
    if (dryRun) {
        args << QStringLiteral("--dry-run");
    }
    return args;
}

QStringList MainWindow::daemonizeShellMutationArgs(int connIdx, const QString& rawShell) const {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return {};
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    // El agente de Windows no sirve --mutate-shell-generic (responde "unknown command",
    // comprobado por RPC), así que la mutación se queda en el camino clásico.
    if (isWindowsConnection(p)) {
        return {};
    }
    if (connIdx < 0 || connIdx >= m_conns.states.size()) {
        return {};
    }
    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    if (!st.daemonInstalled || !st.daemonActive) {
        return {};
    }
    if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        return {};
    }
    const QByteArray utf8 = rawShell.trimmed().toUtf8();
    if (utf8.isEmpty()) {
        return {};
    }
    const QString payloadB64 = QString::fromLatin1(utf8.toBase64());
    return {QStringLiteral("--mutate-shell-generic"), payloadB64};
}

bool MainWindow::fetchPoolCommandOutput(int connIdx,
                                        const QString& poolName,
                                        const QString& actionName,
                                        const QString& remoteCmd,
                                        QString* outputOut,
                                        QString* failureDetailOut,
                                        int timeoutMs) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedAction = actionName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty() || trimmedAction.isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid pool query context");
        }
        return false;
    }

    const ConnectionProfile p = m_conns.profiles[connIdx];
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Consulta %1 %2::%3").arg(trimmedAction.toLower(), p.name, trimmedPool));
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, remoteCmd, timeoutMs, out, err, rc) || rc != 0) {
        const QString detail = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error %1 %2::%3 -> %4")
                   .arg(trimmedAction.toLower(), p.name, trimmedPool, mwhelpers::oneLine(detail)));
        if (failureDetailOut) {
            *failureDetailOut = detail;
        }
        return false;
    }
    if (outputOut) {
        *outputOut = out;
    }
    return true;
}

bool MainWindow::executeConnectionCommand(int connIdx,
                                          const QString& actionName,
                                          const QString& remoteCmd,
                                          int timeoutMs,
                                          QString* failureDetailOut,
                                          const QByteArray& stdinPayload) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || actionName.trimmed().isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid connection command context");
        }
        return false;
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, timeoutMs, out, err, rc, {}, {}, {}, stdinPayload) && rc == 0;
    if (!ok) {
        QStringList parts;
        if (!err.trimmed().isEmpty()) {
            parts << err.trimmed();
        }
        if (!out.trimmed().isEmpty()) {
            parts << out.trimmed();
        }
        const QString detail = parts.isEmpty() ? QStringLiteral("exit %1").arg(rc) : parts.join(QStringLiteral("\n\n"));
        appLog(QStringLiteral("WARN"),
               QStringLiteral("Fallo comando conexión [%1] en \"%2\" (rc=%3): %4")
                   .arg(actionName.trimmed().isEmpty() ? QStringLiteral("?") : actionName.trimmed(),
                        p.name,
                        QString::number(rc),
                        mwhelpers::oneLine(detail)));
        if (failureDetailOut) {
            *failureDetailOut = detail;
        }
        if (isLocalConnection(connIdx) && mwhelpers::looksLikeSudoAuthFailure(detail)) {
            offerLocalSudoCredentialFix();
        }
    }
    return ok;
}

bool MainWindow::fetchConnectionCommandOutput(int connIdx,
                                              const QString& actionName,
                                              const QString& remoteCmd,
                                              QString* outputOut,
                                              QString* failureDetailOut,
                                              int timeoutMs) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || actionName.trimmed().isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid connection query context");
        }
        return false;
    }
    const ConnectionProfile p = m_conns.profiles[connIdx];
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, timeoutMs, out, err, rc) && rc == 0;
    if (!ok) {
        if (failureDetailOut) {
            QStringList parts;
            if (!err.trimmed().isEmpty()) {
                parts << err.trimmed();
            }
            if (!out.trimmed().isEmpty()) {
                parts << out.trimmed();
            }
            *failureDetailOut = parts.isEmpty() ? QStringLiteral("exit %1").arg(rc) : parts.join(QStringLiteral("\n\n"));
        }
        return false;
    }
    if (outputOut) {
        *outputOut = out;
    }
    return true;
}

bool MainWindow::fetchConnectionProbeOutput(int sourceConnIdx,
                                            const QString& actionName,
                                            const QString& remoteCmd,
                                            QString* mergedOutputOut,
                                            QString* failureDetailOut,
                                            int timeoutMs) {
    if (sourceConnIdx < 0 || sourceConnIdx >= m_conns.profiles.size() || actionName.trimmed().isEmpty()) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("invalid probe context");
        }
        return false;
    }
    const ConnectionProfile src = m_conns.profiles[sourceConnIdx];
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(src, remoteCmd, timeoutMs, out, err, rc);
    const QString merged = (out + QStringLiteral("\n") + err).trimmed();
    if (mergedOutputOut) {
        *mergedOutputOut = merged;
    }
    if (!ok || rc != 0) {
        if (failureDetailOut) {
            *failureDetailOut = merged.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : merged;
        }
        return false;
    }
    if (failureDetailOut) {
        failureDetailOut->clear();
    }
    return true;
}

QMap<QString, QMap<QString, QString>> MainWindow::poolAutoSnapshotPropsByDataset(int connIdx, const QString& poolName) const {
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, poolName)) {
        return poolInfo->runtime.autoSnapshotPropsByDataset;
    }
    return {};
}

bool MainWindow::ensureDatasetSnapshotHoldsLoaded(int connIdx, const QString& poolName, const QString& objectName) {
    const QString trimmedPool = poolName.trimmed();
    const QString trimmedObject = objectName.trimmed();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || trimmedPool.isEmpty() || trimmedObject.isEmpty()) {
        return false;
    }
    DSInfo* dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (dsInfo && dsInfo->runtime.holdsState == LoadState::Loaded) {
        return true;
    }
    // Copy, not a reference: m_conns.profiles is reassigned wholesale by loadConnections(),
    // which a queued event can trigger while runSsh() pumps the event loop.
    const ConnectionProfile p = m_conns.profiles[connIdx];
    QString out;
    QString err;
    int rc = -1;
    const QString cmd = withSudo(
        p,
        QStringLiteral("zfs holds -H %1").arg(mwhelpers::shSingleQuote(trimmedObject)));
    const bool holdsOk = runSsh(p, cmd, 20000, out, err, rc) && rc == 0;
    // Re-look up after the yield: rebuildConnInfoFor() replaces the whole ConnInfo,
    // destroying the nested maps dsInfo points into. Every use below is after this.
    dsInfo = findDsInfo(connIdx, trimmedPool, trimmedObject);
    if (!holdsOk) {
        if (dsInfo) {
            dsInfo->runtime.holdsState = LoadState::Error;
            dsInfo->runtime.errorText = err.trimmed();
        }
        return false;
    }

    QVector<QPair<QString, QString>> holds;
    QSet<QString> seen;
    for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = line.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString tag = parts.value(1).trimmed();
        const QString timestamp = parts.value(2).trimmed();
        const QString key = tag.toLower();
        if (tag.isEmpty() || seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        holds.push_back(qMakePair(tag, timestamp));
    }
    if (dsInfo) {
        dsInfo->runtime.snapshotHolds = holds;
        dsInfo->runtime.holdsState = LoadState::Loaded;
        dsInfo->runtime.loadedAt = QDateTime::currentDateTimeUtc();
        dsInfo->runtime.errorText.clear();
    }
    return true;
}

QVector<QPair<QString, QString>> MainWindow::datasetSnapshotHolds(int connIdx, const QString& poolName, const QString& objectName) const {
    if (const DSInfo* dsInfo = findDsInfo(connIdx, poolName, objectName)) {
        return dsInfo->runtime.snapshotHolds;
    }
    return {};
}

DatasetPermissionsCacheEntry* MainWindow::datasetPermissionsEntryMutable(int connIdx,
                                                                                    const QString& poolName,
                                                                                    const QString& datasetName) {
    const QString key = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
    auto it = m_conns.datasetPermissionsCache.find(key);
    if (it != m_conns.datasetPermissionsCache.end()) {
        return &it.value();
    }
    DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (dsInfo && dsInfo->runtime.permissionsState == LoadState::Loaded && dsInfo->permissionsCache.loaded) {
        m_conns.datasetPermissionsCache.insert(key, dsInfo->permissionsCache);
        auto inserted = m_conns.datasetPermissionsCache.find(key);
        return (inserted == m_conns.datasetPermissionsCache.end()) ? nullptr : &inserted.value();
    }
    return nullptr;
}

void MainWindow::mirrorDatasetPermissionsEntryToModel(int connIdx, const QString& poolName, const QString& datasetName) {
    const QString key = datasetPermissionsCacheKey(connIdx, poolName, datasetName);
    const auto it = m_conns.datasetPermissionsCache.constFind(key);
    DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return;
    }
    if (it == m_conns.datasetPermissionsCache.cend()) {
        dsInfo->permissionsCache = DatasetPermissionsCacheEntry{};
        dsInfo->runtime.permissionsState = LoadState::NotLoaded;
        return;
    }
    dsInfo->permissionsCache = it.value();
    dsInfo->runtime.permissionsState = it->loaded ? LoadState::Loaded : LoadState::NotLoaded;
}

QVector<MainWindow::PendingPermissionDraftEntry> MainWindow::dirtyDatasetPermissionsEntriesFromModel() const {
    QVector<PendingPermissionDraftEntry> entries;
    for (auto itConn = m_conns.connInfoById.cbegin(); itConn != m_conns.connInfoById.cend(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.cbegin(); itPool != itConn->poolsByStableId.cend(); ++itPool) {
            const QString poolName = itPool->key.poolName.trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            for (auto itDs = itPool->objectsByFullName.cbegin(); itDs != itPool->objectsByFullName.cend(); ++itDs) {
                if (!itDs->permissionsCache.loaded || !itDs->permissionsCache.dirty) {
                    continue;
                }
                PendingPermissionDraftEntry entry;
                entry.connIdx = itConn->connIdx;
                entry.poolName = poolName;
                entry.datasetName = itDs.key();
                entry.entry = itDs->permissionsCache;
                entries.push_back(entry);
            }
        }
    }
    return entries;
}

void MainWindow::removeDatasetPermissionsEntry(int connIdx, const QString& poolName, const QString& datasetName) {
    m_conns.datasetPermissionsCache.remove(datasetPermissionsCacheKey(connIdx, poolName, datasetName));
    mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
}

void MainWindow::removeDatasetPermissionsEntriesForPool(int connIdx, const QString& poolName) {
    const QString prefix = QStringLiteral("%1::%2::").arg(connToken(connIdx), poolName.trimmed().toLower());
    for (auto it = m_conns.datasetPermissionsCache.begin(); it != m_conns.datasetPermissionsCache.end();) {
        if (it.key().startsWith(prefix)) {
            it = m_conns.datasetPermissionsCache.erase(it);
        } else {
            ++it;
        }
    }
    if (ConnInfo* connInfo = findConnInfo(connIdx)) {
        for (auto itPool = connInfo->poolsByStableId.begin(); itPool != connInfo->poolsByStableId.end(); ++itPool) {
            if (itPool->key.poolName.trimmed() != poolName.trimmed()) {
                continue;
            }
            for (auto itDs = itPool->objectsByFullName.begin(); itDs != itPool->objectsByFullName.end(); ++itDs) {
                itDs->permissionsCache = DatasetPermissionsCacheEntry{};
                itDs->runtime.permissionsState = LoadState::NotLoaded;
            }
            break;
        }
    }
}

void MainWindow::resetAllDatasetPermissionDrafts() {
    for (auto it = m_conns.datasetPermissionsCache.begin(); it != m_conns.datasetPermissionsCache.end(); ++it) {
        if (!it.value().loaded) {
            continue;
        }
        it.value().dirty = false;
        it.value().localGrants = it.value().originalLocalGrants;
        it.value().descendantGrants = it.value().originalDescendantGrants;
        it.value().localDescendantGrants = it.value().originalLocalDescendantGrants;
        it.value().createPermissions = it.value().originalCreatePermissions;
        it.value().permissionSets = it.value().originalPermissionSets;
    }
    for (auto itConn = m_conns.connInfoById.begin(); itConn != m_conns.connInfoById.end(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.begin(); itPool != itConn->poolsByStableId.end(); ++itPool) {
            for (auto itDs = itPool->objectsByFullName.begin(); itDs != itPool->objectsByFullName.end(); ++itDs) {
                if (!itDs->permissionsCache.loaded) {
                    continue;
                }
                itDs->permissionsCache.dirty = false;
                itDs->permissionsCache.localGrants = itDs->permissionsCache.originalLocalGrants;
                itDs->permissionsCache.descendantGrants = itDs->permissionsCache.originalDescendantGrants;
                itDs->permissionsCache.localDescendantGrants = itDs->permissionsCache.originalLocalDescendantGrants;
                itDs->permissionsCache.createPermissions = itDs->permissionsCache.originalCreatePermissions;
                itDs->permissionsCache.permissionSets = itDs->permissionsCache.originalPermissionSets;
            }
        }
    }
}

bool MainWindow::selectDatasetForTest(const QString& datasetName, bool bottom) {
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty()) {
        return false;
    }
    const QString wanted = datasetName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wanted) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !item; ++i) {
        item = rec(tree->topLevelItem(i));
    }
    if (!item) {
        return false;
    }
    const int connIdx = item->data(0, Qt::UserRole + 10).toInt();
    const QString poolName = item->data(0, Qt::UserRole + 11).toString().trimmed();
    const QString token = (connIdx >= 0 && !poolName.isEmpty())
                              ? QStringLiteral("%1::%2").arg(connToken(connIdx), poolName)
                              : QString();
    Q_UNUSED(token);
    tree->setCurrentItem(item);
    refreshConnContentPropertiesFor(tree);
    return true;
}

bool MainWindow::setDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool expanded, bool bottom) {
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty() || childLabel.trimmed().isEmpty()) {
        return false;
    }
    const QString wantedDataset = datasetName.trimmed();
    const QString wantedChild = childLabel.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> recDataset = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wantedDataset) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = recDataset(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* datasetItem = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !datasetItem; ++i) {
        datasetItem = recDataset(tree->topLevelItem(i));
    }
    if (!datasetItem) {
        return false;
    }
    for (int i = 0; i < datasetItem->childCount(); ++i) {
        QTreeWidgetItem* child = datasetItem->child(i);
        if (child && child->text(0).trimmed() == wantedChild) {
            child->setExpanded(expanded);
            return true;
        }
    }
    return false;
}

bool MainWindow::isDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool bottom) const {
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty() || childLabel.trimmed().isEmpty()) {
        return false;
    }
    const QString wantedDataset = datasetName.trimmed();
    const QString wantedChild = childLabel.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> recDataset = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wantedDataset) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = recDataset(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* datasetItem = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !datasetItem; ++i) {
        datasetItem = recDataset(tree->topLevelItem(i));
    }
    if (!datasetItem) {
        return false;
    }
    for (int i = 0; i < datasetItem->childCount(); ++i) {
        QTreeWidgetItem* child = datasetItem->child(i);
        if (child && child->text(0).trimmed() == wantedChild) {
            return child->isExpanded();
        }
    }
    return false;
}

void MainWindow::rebuildConnContentTreeForTest(const QString& datasetToSelect, bool bottom) {
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree) {
        return;
    }
    const QString token = connContentTokenForTree(tree);
    int connIdx = -1;
    QString poolName;
    if (!splitConnToken(token, connIdx, poolName)) {
        return;
    }
    populateDatasetTree(tree, connIdx, poolName, DatasetTreeContext::ConnectionContent, true);
    if (!datasetToSelect.trimmed().isEmpty()) {
        selectDatasetForTest(datasetToSelect, bottom);
    }
}

namespace {
// Mismo valor que kIsPoolRootRole en mainwindow_dataset_tree.cpp, que es una
// constante de unidad de traducción y no está expuesta en la cabecera.
constexpr int kIsPoolRootRoleForTest = Qt::UserRole + 12;
}  // namespace

QStringList MainWindow::topLevelPoolNamesForTest(bool bottom) const {
    QStringList names;
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree) {
        return names;
    }
    // Los pools ya NO son nodos raíz del árbol: tras unificarlo, la raíz es la
    // conexión y los pools cuelgan de ella. Esta función devolvía los raíces sin
    // más, así que contaba conexiones y hacía fallar los tests como si faltaran
    // pools. Se buscan por su rol, esté donde esté el nodo.
    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        if (node->data(0, kIsPoolRootRoleForTest).toBool()) {
            names.push_back(node->text(0).trimmed());
        }
        for (int i = 0; i < node->childCount(); ++i) {
            collect(node->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        collect(tree->topLevelItem(i));
    }
    return names;
}

QStringList MainWindow::childLabelsForDatasetForTest(const QString& datasetName, bool bottom) const {
    QStringList labels;
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty()) {
        return labels;
    }
    const QString wanted = datasetName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wanted) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !item; ++i) {
        item = rec(tree->topLevelItem(i));
    }
    if (!item) {
        return labels;
    }
    for (int i = 0; i < item->childCount(); ++i) {
        if (QTreeWidgetItem* child = item->child(i)) {
            labels.push_back(child->text(0).trimmed());
        }
    }
    return labels;
}

QStringList MainWindow::snapshotNamesForDatasetForTest(const QString& datasetName, bool bottom) const {
    QStringList names;
    Q_UNUSED(bottom);
    QTreeWidget* tree = m_connContentTree;
    if (!tree || datasetName.trimmed().isEmpty()) {
        return names;
    }
    const QString wanted = datasetName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == wanted) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(node->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !item; ++i) {
        item = rec(tree->topLevelItem(i));
    }
    if (!item) {
        return names;
    }
    constexpr int kSnapshotListRole = Qt::UserRole + 1;
    names = item->data(1, kSnapshotListRole).toStringList();
    return names;
}

bool MainWindow::requireDaemonForRead(int connIdx, const QString& what) const {
    if (connIdx < 0 || connIdx >= m_conns.states.size()) {
        return false;
    }
    return requireDaemonForRead(m_conns.profiles.value(connIdx).name, m_conns.states[connIdx], what);
}

bool MainWindow::requireDaemonForRead(const QString& connName,
                                      const ConnectionRuntimeState& st,
                                      const QString& what) const {
    QString reason;
    if (!st.daemonInstalled) {
        reason = QStringLiteral("el agente no está instalado");
    } else if (!st.daemonActive) {
        reason = QStringLiteral("el agente no está en marcha");
    } else if (st.daemonApiVersion.trimmed() != agentversion::expectedApiVersion().trimmed()) {
        reason = QStringLiteral("la versión de API del agente es %1 y se espera %2")
                     .arg(st.daemonApiVersion.trimmed().isEmpty() ? QStringLiteral("desconocida")
                                                                  : st.daemonApiVersion.trimmed(),
                          agentversion::expectedApiVersion());
    } else {
        return true;
    }
    const_cast<MainWindow*>(this)->appLog(
        QStringLiteral("WARN"),
        QStringLiteral("%1: no se puede %2 porque %3. Reinstale el daemon desde el menú de la conexión.")
            .arg(connName, what, reason));
    return false;
}

zfsmgr::caps::Platform MainWindow::capabilityPlatform(int connIdx) const {
    zfsmgr::caps::Platform plat;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return plat;
    }
    plat.isWindows = isWindowsConnection(connIdx);
    if (connIdx < m_conns.states.size()) {
        const ConnectionRuntimeState& st = m_conns.states[connIdx];
        plat.daemonActive = st.daemonInstalled && st.daemonActive;
        plat.daemonApiOk =
            st.daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
        plat.daemonCaps = st.daemonCaps;
        for (const QString& c : st.missingUnixCommands) {
            const QString t = c.trimmed();
            if (!t.isEmpty()) {
                plat.missingTools.insert(t);
            }
        }
    }
    return plat;
}

QString MainWindow::capabilityReasonText(zfsmgr::caps::Reason r) const {
    using R = zfsmgr::caps::Reason;
    switch (r) {
    case R::Available:
        return QString();
    case R::DaemonNotReady:
        return trk(QStringLiteral("t_cap_daemon_down"),
                   QStringLiteral("el agente no está instalado o no está activo en esta conexión"),
                   QStringLiteral("the agent is not installed or not running on this connection"),
                   QStringLiteral("此连接上未安装或未运行代理"));
    case R::DaemonApiMismatch:
        return trk(QStringLiteral("t_cap_api_mismatch"),
                   QStringLiteral("la versión de API del agente no coincide con la que espera esta "
                                  "aplicación; reinstale el daemon"),
                   QStringLiteral("the agent API version does not match the one this application "
                                  "expects; reinstall the daemon"),
                   QStringLiteral("代理 API 版本与本应用程序期望的版本不匹配；请重新安装守护进程"));
    case R::MissingTool:
        return trk(QStringLiteral("t_cap_missing_tool"),
                   QStringLiteral("falta una herramienta que el agente necesita en el equipo remoto"),
                   QStringLiteral("a tool the agent needs is missing on the remote machine"),
                   QStringLiteral("远程计算机上缺少代理所需的工具"));
    case R::WindowsAgentPending:
        return trk(QStringLiteral("t_cap_win_agent_todo"),
                   QStringLiteral("el agente de Windows todavía no lo implementa"),
                   QStringLiteral("the Windows agent does not implement it yet"),
                   QStringLiteral("Windows 代理尚未实现该功能"));
    case R::WindowsNeedsUnixShell:
        return trk(QStringLiteral("t_win_unix_na01"),
                   QStringLiteral("usa shell Unix y no está disponible en conexiones Windows"),
                   QStringLiteral("it uses a Unix shell and is not available on Windows connections"),
                   QStringLiteral("它使用 Unix shell，在 Windows 连接上不可用"));
    case R::WindowsNotApplicable:
        return trk(QStringLiteral("t_cap_win_na"),
                   QStringLiteral("no es aplicable en Windows"),
                   QStringLiteral("it does not apply on Windows"),
                   QStringLiteral("不适用于 Windows"));
    }
    return QString();
}

bool MainWindow::featureAvailable(int connIdx, zfsmgr::caps::Feature f, QString* reasonOut) const {
    const zfsmgr::caps::Availability a =
        zfsmgr::caps::featureAvailability(f, capabilityPlatform(connIdx));
    if (reasonOut) {
        *reasonOut = a.available ? QString() : capabilityReasonText(a.reason);
    }
    return a.available;
}

bool MainWindow::requireFeature(int connIdx, zfsmgr::caps::Feature f) {
    QString reason;
    if (featureAvailable(connIdx, f, &reason)) {
        return true;
    }
    const QString msg = trk(QStringLiteral("t_cap_blocked_msg"),
                            QStringLiteral("Esta acción no está disponible: %1."),
                            QStringLiteral("This action is not available: %1."),
                            QStringLiteral("此操作不可用：%1。"))
                            .arg(reason);
    appLog(QStringLiteral("WARN"), msg);
    QMessageBox::warning(this, QStringLiteral("ZFSMgr"), msg);
    return false;
}

QStringList MainWindow::connectionContextMenuTopLevelLabelsForTest() const {
    return {
        trk(QStringLiteral("t_connect_ctx_001"),
            QStringLiteral("Conectar"),
            QStringLiteral("Connect"),
            QStringLiteral("连接")),
        trk(QStringLiteral("t_disconnect_ctx001"),
            QStringLiteral("Desconectar"),
            QStringLiteral("Disconnect"),
            QStringLiteral("断开连接")),
        trk(QStringLiteral("t_refresh_conn_ctx001"),
            QStringLiteral("Refrescar"),
            QStringLiteral("Refresh"),
            QStringLiteral("刷新")),
        QString(),
        trk(QStringLiteral("t_new_conn_ctx001"),
            QStringLiteral("Nueva Conexión"),
            QStringLiteral("New Connection"),
            QStringLiteral("新建连接")),
        trk(QStringLiteral("t_edit_conn_ctx001"),
            QStringLiteral("Editar"),
            QStringLiteral("Edit"),
            QStringLiteral("编辑")),
        trk(QStringLiteral("t_del_conn_ctx001"),
            QStringLiteral("Borrar"),
            QStringLiteral("Delete"),
            QStringLiteral("删除")),
        QString(),
        trk(QStringLiteral("t_new_pool_ctx_001"),
            QStringLiteral("Nuevo Pool"),
            QStringLiteral("New Pool"),
            QStringLiteral("新建存储池")),
        QString(),
        trk(QStringLiteral("t_install_helpers_ctx001"),
            QStringLiteral("Instalar comandos auxiliares"),
            QStringLiteral("Install helper commands"),
            QStringLiteral("安装辅助命令")),
    };
}

QStringList MainWindow::connectionRefreshMenuLabelsForTest() const {
    return {
        trk(QStringLiteral("t_refresh_this_conn_001"),
            QStringLiteral("Esta conexión"),
            QStringLiteral("This connection"),
            QStringLiteral("此连接")),
        trk(QStringLiteral("t_refresh_all_001"),
            QStringLiteral("Todas las conexiones"),
            QStringLiteral("All connections"),
            QStringLiteral("所有连接")),
    };
}

QStringList MainWindow::poolContextMenuLabelsForTest(const QString& poolName, bool bottom) const {
    Q_UNUSED(bottom);
    const int connIdx = m_topDetailConnIdx;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || connIdx >= m_conns.states.size() || poolName.trimmed().isEmpty()) {
        return {};
    }
    QString poolAction;
    const ConnectionRuntimeState& st = m_conns.states[connIdx];
    for (const PoolImported& pool : st.importedPools) {
        if (pool.pool.trimmed().compare(poolName.trimmed(), Qt::CaseInsensitive) == 0) {
            poolAction = QStringLiteral("Exportar");
            break;
        }
    }
    if (poolAction.isEmpty()) {
        for (const PoolImportable& pool : st.importablePools) {
            if (pool.pool.trimmed().compare(poolName.trimmed(), Qt::CaseInsensitive) == 0) {
                poolAction = pool.action.trimmed();
                break;
            }
        }
    }
    const zfsmgr::uilogic::PoolRootMenuState menuState =
        zfsmgr::uilogic::buildPoolRootMenuState(poolAction, QStringLiteral("ONLINE"), true);
    Q_UNUSED(menuState);
    return {
        trk(QStringLiteral("t_pool_refresh_status001"),
            QStringLiteral("Actualizar estado"),
            QStringLiteral("Refresh status"),
            QStringLiteral("刷新状态")),
        trk(QStringLiteral("t_import_btn001"),
            QStringLiteral("Importar"),
            QStringLiteral("Import"),
            QStringLiteral("导入")),
        QStringLiteral("Importar renombrando"),
        trk(QStringLiteral("t_export_btn001"),
            QStringLiteral("Exportar"),
            QStringLiteral("Export"),
            QStringLiteral("导出")),
        trk(QStringLiteral("t_pool_history_t1"),
            QStringLiteral("Historial")),
        QStringLiteral("Sync"),
        QStringLiteral("Scrub"),
        QStringLiteral("Upgrade"),
        QStringLiteral("Reguid"),
        QStringLiteral("Trim"),
        QStringLiteral("Initialize"),
        QStringLiteral("Clear"),
        QStringLiteral("Destroy"),
        QStringLiteral("Mostrar Datasets programados"),
    };
}
