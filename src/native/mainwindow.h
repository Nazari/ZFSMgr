#pragma once

#include "commands/gsa.h"
#include "cliservice.h"
#include "clireader.h"
#include "connectionmodel.h"
#include "connectionregistry.h"
#include "transportsession.h"
#include "connectiondialog.h"
#include "mainwindow_helpers.h"
#include "connectioncapabilities.h"
#include "connectiondatasettreepane.h"
#include "connectiondatasettreecoordinator.h"
#include "connectiondatasettreewidget.h"
#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.10.0rc1"
#endif

#include <QMainWindow>
#include <QDateTime>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <atomic>
#include <functional>
#include <optional>
#include <QFile>

#include <memory>
#include <thread>

class QVBoxLayout;
class QTimer;
class QComboBox;
class QColor;
class QGroupBox;
class QLabel;
class QLineEdit;
class QCheckBox;
class QSpinBox;
class QDialog;
class QListWidget;
class QListWidgetItem;
class QPoint;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QMenu;
class QAction;
class QCloseEvent;
class QTableWidget;
class QTabBar;
class QTabWidget;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;
class QTextEdit;
class QByteArray;
class MainWindowConnectionDatasetTreeDelegate;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    // Público porque lo necesitan el delegado del árbol y el ayudante que calcula el
    // testigo de estado: el testigo tiene que salir IGUAL desde los tres sitios.
    QString connToken(int connIdx) const;
    int connIdxFromToken(const QString& token) const;
    bool splitConnToken(const QString& token, int& connIdx, QString& poolName) const;


    enum class DatasetTreeContext {
        Origin,
        Destination,
        ConnectionContent,
        ConnectionContentMulti,
    };
    enum class PendingItemStatus { Pending, Running, Success, Failed };
    struct InlinePropGroupConfig {
        QString name;
        QStringList props;
    };
    struct UiTestDatasetSeed {
        QString name;
        QString mountpoint;
        QString canmount{QStringLiteral("on")};
        QString mounted{QStringLiteral("yes")};
        QStringList snapshots;
    };
    struct UiTestPropertySeed {
        QString prop;
        QString value;
        QString source{QStringLiteral("local")};
        QString readonly{QStringLiteral("no")};
    };

    explicit MainWindow(const QString& masterPassword, const QString& language, QWidget* parent = nullptr);
    ~MainWindow() override;
    void configureSingleConnectionUiTestState(const ConnectionProfile& profile,
                                              const QStringList& importedPools,
                                              const QStringList& importablePools);
    void configurePoolDatasetsForTest(int connIdx,
                                      const QString& poolName,
                                      const QVector<UiTestDatasetSeed>& datasets);
    // Expuesta para fijar el corte por separador: es donde un directorio con '&' en el
    // nombre truncaba la orden.
    static QStringList extractAgentArgsForTest(const QString& remoteCmd);

    // Transporte de mentira, para poder comprobar QUÉ se le pide al agente sin
    // necesitar una máquina remota.
    //
    // Existe porque los tests no ejercitaban nada fuera de este equipo, y por ahí se
    // colaron tres fallos reales: una orden que llegaba con la ruta destrozada, otra
    // que dejó de usar el daemon y se iba por shell, y un bloque de datos que dejó de
    // rellenarse. Los tres compilaban y pasaban todos los tests.
    //
    // Mientras está puesto NO se abre ninguna conexión: las órdenes por argv van a la
    // función que se le pase, y las que salgan como cadena de shell se registran y
    // fracasan, para que un test pueda afirmar que algo NO se fue por ese camino.
    // Alias de los de TransportSession: los tests ya los nombran así y no hay motivo
    // para hacerles cambiar por una reorganización interna.
    using AgentCallForTest = TransportSession::AgentCallForTest;
    using AgentTransportForTest = TransportSession::AgentTransportForTest;
    // Deja la conexión con un daemon sano, que es la precondición de casi todo desde
    // que no hay respaldo por shell.
    void setConnectionDaemonStateForTest(int connIdx, bool installed, bool active);
    void setAgentTransportForTest(AgentTransportForTest fn);
    // getDatasetProperty es privada; esto la expone para poder ejercitar la lectura
    // más usada de la aplicación contra el transporte de mentira.
    bool getDatasetPropertyForTest(int connIdx, const QString& dataset, const QString& prop,
                                   QString& valueOut);
    // Clasificación de órdenes mutantes. Es la que decide si, tras una respuesta
    // ambigua del daemon, se REENVÍA la orden. Clasificar mal una destructiva la
    // ejecuta dos veces solapadas, así que conviene fijarla con tests.
    static bool isMutatingAgentCommandForTest(const QStringList& agentArgs);
    QVector<AgentCallForTest> agentCallsForTest() const;
    // Cuántas veces se ha ejecutado DE VERDAD cada reconstrucción de la interfaz.
    // Existe para poder afirmar que un lote de operaciones sobre varias conexiones
    // repinta el árbol una sola vez, que es lo que se rompió: la actualización
    // automática de daemons lo repintaba una vez por conexión.
    struct UiRebuildCountsForTest {
        int table{0};
        int pools{0};
        int nodeDetails{0};
    };
    UiRebuildCountsForTest uiRebuildCountsForTest() const;
    void runWithDeferredUiRebuildForTest(const std::function<void()>& body);
    // Lo mismo que pide refreshConnectionByIndex() al terminar de refrescar una
    // conexión: tabla, tablas de pools y árbol.
    void requestConnectionsUiRebuildForTest();
    void rebuildConnectionDetailsForTest();
    void setConnectionGsaStateForTest(int connIdx, bool installed, bool active, const QString& version = QString());
    void configureDatasetPropertiesForTest(int connIdx,
                                           const QString& objectName,
                                           const QString& datasetType,
                                           const QVector<UiTestPropertySeed>& rows);
    void debugTrace(const QString& msg);
    QStringList topLevelPoolNamesForTest(bool bottom = false) const;
    bool selectDatasetForTest(const QString& datasetName, bool bottom = false);
    // La selección de transferencia tal y como la ven las seis acciones de dos extremos.
    struct TransferSelectionForTest {
        bool valid{false};
        int connIdx{-1};
        QString poolName;
        QString datasetName;
        QString snapshotName;
    };
    TransferSelectionForTest transferSelectionForTest(const QString& side) const;
    // Un trabajo del daemon en marcha, y el sondeo que lo consulta. Sirven para fijar que
    // una conexión Local TAMBIÉN se sondea: por el túnel no se puede, y saltárselo no da
    // error, solo deja el panel de Transferencias sin progreso.
    void addActiveDaemonJobForTest(int srcConnIdx, int dstConnIdx, const QString& jobId);
    void pollDaemonJobsForTest();
    void refreshAllJobsForTest() { refreshAllJobsAsync(); }
    void setCliFakeForTest(CliService::Fake fake) {
        m_cli->setFakeForTest(std::move(fake));
        m_refreshAfterCli = false;
    }
    // What the shell answers to the window's READS (`ls … --format json`, `jobs`…).
    void setCliReadFakeForTest(CliService::Fake fake) {
        m_cliReader->setFakeForTest(std::move(fake));
    }
    QStringList cliReadLinesForTest() const { return m_cliReader->linesForTest(); }
    bool loadPoolDatasetsForTest(int connIdx, const QString& pool) {
        m_conns.poolDatasetCache.remove(datasetCacheKey(connIdx, pool));
        return ensureDatasetsLoaded(connIdx, pool, true);
    }
    QStringList cachedSnapshotsForTest(int connIdx, const QString& pool, const QString& ds) const {
        return m_conns.poolDatasetCache.value(datasetCacheKey(connIdx, pool))
            .snapshotsByDataset.value(ds);
    }
    QString cachedGuidForTest(int connIdx, const QString& pool, const QString& obj) const {
        return m_conns.poolDatasetCache.value(datasetCacheKey(connIdx, pool))
            .objectGuidByName.value(obj);
    }
    QStringList cliLinesForTest() const { return m_cli->linesForTest(); }
    bool mountDatasetForTest(int connIdx, const QString& ds);
    bool cliPoolForTest(int connIdx, const QString& pool, const QString& verb,
                        const QString& rest) {
        return cliPool(connIdx, pool, verb, rest);
    }
    bool cliImportPoolForTest(int connIdx, const QString& pool, const QString& as, bool force) {
        return cliImportPool(connIdx, pool, as, force);
    }
    void destroyForTest(int connIdx, const QString& ds, const QString& snap, const QString& flags);
    bool renameForTest(int connIdx, const QString& src, const QString& target);
    void releaseHoldForTest(int connIdx, const QString& ds, const QString& snap,
                            const QString& tag);
    void transferForTest(int srcConn, const QString& srcDs, const QString& srcSnap, int dstConn,
                         const QString& dstDs, const QString& dstSnap, const QString& verb);
    static QString cliConnectionOptionsForTest(const ConnectionProfile& p, bool editing) {
        return cliConnectionOptions(p, editing);
    }
    QString zfsmUrlForTest(int connIdx, const QString& ds, const QString& snap) const {
        return zfsmUrl(connIdx, ds, snap);
    }
    void fillPaneSchedulesForTest(int pane, int connIdx, const QString& pool, const QString& ds) {
        fillPaneSchedules(pane, connIdx, pool, ds);
    }
    void setPaneScheduleDailyForTest(int pane, int daily, bool enabled);
    void applyPaneScheduleForTest(int pane) { applyPaneSchedule(pane); }
    void refreshTransfersListForTest();
    // El estado de validación de una conexión, y cómo queda su fila en el desplegable del
    // panel: «<rótulo>|caida» o «<rótulo>|viva».
    void setConnectionStatusForTest(int connIdx, const QString& status);
    QStringList paneConnectionComboEntriesForTest(bool bottom = false) const;
    // Simula que el daemon de una conexión ACABA de ponerse en marcha, que es el momento en
    // que hay que buscar trabajos que sigan corriendo sin que esta ventana lo sepa.
    void deliverDaemonBecameActiveForTest(int connIdx);
    void removeActiveDaemonJobForTest(const QString& jobId);
    bool selectTransferJobForTest(const QString& jobId);
    QString selectedTransferJobForTest() const;
    quint64 daemonJobBytesForTest(const QString& jobId) const;
    QString daemonJobErrorForTest(const QString& jobId) const;
    bool selectSnapshotInPaneDetailForTest(const QString& datasetName, const QString& snapshotName,
                                           bool bottom = false);
    // Deja el panel como lo deja un refresco: sin selección en el árbol y sin origen/destino.
    void clearTransferSelectionForTest(const QString& side, bool bottom = false);
    TransferSelectionForTest forceTransferSelectionForTest(const QString& side, int connIdx,
                                                           const QString& poolName,
                                                           const QString& datasetName,
                                                           const QString& snapshotName);
    QStringList connectionsMenuLabelsForTest() const;
    QStringList connectionRefreshMenuLabelsForTest() const;
    QStringList poolContextMenuLabelsForTest(const QString& poolName, bool bottom = false) const;
    // El camino ENTERO de una programación preparada: se guarda con el testigo que
    // construye la interfaz y se relee con el que reconstruye el árbol. Si las dos mitades
    // dejan de entenderse —y dejaron de entenderse al llevar el testigo el identificador
    // de la conexión en lugar de su posición—, el borrador se escribe en un sitio que
    // nadie sabe leer y la orden «Programar snapshots» no hace nada visible.
    void stageGsaDraftForTest(int connIdx, const QString& poolName, const QString& datasetName,
                              const QMap<QString, QString>& valuesByProp);
    QStringList scheduledDatasetsForTest(int connIdx, const QString& poolName) const;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class MainWindowConnectionDatasetTreeDelegate;
    struct DatasetSelectionContext;






    struct DatasetSelectionContext {
        bool valid{false};
        int connIdx{-1};
        QString poolName;
        QString datasetName;
        QString snapshotName;
    };
    // A dataset (and snapshot) of a connection, as a selection context.
    static DatasetSelectionContext selectionFor(int connIdx, const QString& dataset,
                                                const QString& snapshot);

    struct ConnContentTreeState {
        QStringList expandedDatasets;
        QStringList expandedNodePaths;
        bool poolRootExpanded{true};
        bool infoExpanded{false};
        QMap<QString, bool> poolRootExpandedByPool;
        QMap<QString, bool> infoExpandedByPool;
        QString selectedDataset;
        QString selectedSnapshot;
        QString selectedNodePath;
        QMap<QString, QString> snapshotByDataset;
        QMap<QString, QStringList> expandedChildPathsByDataset;
        QByteArray headerState;
        int verticalScrollValue{0};
        int horizontalScrollValue{0};
    };

    struct DatasetPropsDraft {
        QMap<QString, QString> valuesByProp;
        QMap<QString, bool> inheritByProp;
        // En qué orden se tocó cada propiedad, la primera vez.
        //
        // Los dos mapas de arriba ordenan por clave y pierden esa información, y el
        // conjunto desde el que se aplicaban ni siquiera tenía orden definido. El orden
        // en que se ejecutan las órdenes tiene que ser el que hizo el USUARIO, de la más
        // antigua a la más nueva: es lo único predecible, y en propiedades de montaje
        // además importa —`canmount=on` monta usando el `mountpoint` que haya puesto en
        // ese momento—.
        QStringList editOrder;
        bool dirty{false};

        // Deja constancia de que se ha tocado `prop`, si no la había ya. Se llama al
        // escribir en cualquiera de los dos mapas.
        void noteEdited(const QString& prop) {
            if (!prop.trimmed().isEmpty() && !editOrder.contains(prop)) {
                editOrder.push_back(prop);
            }
        }
    };

    struct PendingDatasetRenameDraft {
        int connIdx{-1};
        QString poolName;
        QString sourceName;
        QString targetName;
    };

    struct PendingShellActionDraft {
        enum class RefreshScope {
            None,
            TargetOnly,
            SourceAndTarget
        };
        QString scopeLabel;
        QString displayLabel;
        QString command;
        int timeoutMs{0};
        bool streamProgress{true};
        DatasetSelectionContext refreshSource;
        DatasetSelectionContext refreshTarget;
        RefreshScope refreshScope{RefreshScope::SourceAndTarget};
        // Paso previo TIPADO, ejecutado por RPC antes de la orden de shell.
        //
        // Existe por un motivo concreto: la frase de cifrado de un dataset nuevo no
        // puede viajar dentro de `command`. Ahí acabaría en la línea de órdenes de ssh
        // —visible en `ps` de las dos máquinas—, en la vista previa de confirmación y en
        // el registro. Por el RPC va dentro de la carga, cifrada por mTLS, y el daemon
        // se la pasa a `zfs` por una tubería.
        //
        // `rpcSecret` se guarda aparte y NUNCA se copia a `command` ni a `displayLabel`,
        // que son los dos sitios que se muestran o se registran.
        int rpcConnIdx{-1};
        QStringList rpcArgv;   // sin el secreto: se añade al ejecutar
        QString rpcSecret;
        // Lo que necesita `executeDatasetAction` para Desglosar y Ensamblar, que no se
        // describen con una orden de shell. Yendo aquí conservan TODO lo suyo: el envío
        // como trabajo del daemon, el progreso en tiempo real y la regla de no
        // reintentar una mutación por SSH.
        QString datasetActionSide;
        QString datasetActionName;   // no vacío = es una acción de dataset diferida
        DatasetSelectionContext datasetActionCtx;
        QStringList datasetActionArgv;   // vacío en los caminos de respaldo por shell
        QByteArray datasetActionStdin;
        bool datasetActionAllowWindowsScript{false};
    };
    struct PendingPropertyDraftEntry {
        int connIdx{-1};
        QString poolName;
        QString token;
        QString objectName;
        DatasetPropsDraft draft;
    };

    struct PendingPermissionDraftEntry {
        int connIdx{-1};
        QString poolName;
        QString datasetName;
        DatasetPermissionsCacheEntry entry;
    };
    struct RefreshRuntimeCacheEntry {
        QDateTime loadedAt;
        QMap<QString, QString> poolStatusByName;
        QMap<QString, QString> poolGuidByName;
        QMap<QString, QMap<QString, QString>> gsaPropsByDataset;
        bool commandsProbeLoaded{false};
        QStringList detectedUnixCommands;
        QStringList missingUnixCommands;
        QMap<QString, bool> packageManagerAvailabilityById;
    };
    // Disponibilidad de las seis acciones que necesitan origen Y destino.
    //
    // Existe para poder preguntar «¿estaría permitido esto SI el destino fuera este
    // nodo?» sin duplicar las reglas. El menú contextual necesita justo eso: se invoca
    // sobre un nodo cualquiera y tiene que decidir, ahí mismo, si la acción cabe y qué
    // decir cuando no. Las reglas viven en un solo sitio o se desincronizan —ya pasó con
    // los dos validadores del diálogo de selección—.
    struct TransferActionAvailability {
        struct Entry {
            bool enabled{false};
            QString reason;   // por qué no, cuando enabled es false
        };
        Entry send;
        Entry clone;
        Entry move;
        Entry level;
        Entry sync;
        Entry diff;
    };
    TransferActionAvailability transferActionAvailabilityFor(const DatasetSelectionContext& src,
                                                             const DatasetSelectionContext& dst);




















    void buildUi();
    void loadConnections();
    void ensureStartupLocalSudoConnection();
    void rebuildConnectionsTable();
    // Reintroduce usuario y contraseña de sudo de la conexión Local, que es la única
    // que no se puede editar. Valida la contraseña antes de guardarla.
    void changeLocalSudoCredentials();
    bool m_localSudoFixOffered{false};
    // Invalida el material TLS local cacheado: se leyó elevando con la contraseña de
    // sudo anterior, así que al cambiarla hay que volver a pedirlo.
    void clearLocalDaemonTlsCache();
    // Agrupa reconstrucciones de la interfaz. Mientras haya un guardián vivo, las
    // llamadas a rebuildConnectionsTable/populateAllPoolsTables/
    // refreshConnectionNodeDetails se anotan en vez de ejecutarse, y al soltarlo se
    // hace UNA sola pasada. Existe porque un lote de operaciones sobre varias
    // conexiones —la actualización automática de daemons al arrancar— repintaba el
    // árbol entero una vez por conexión: el usuario veía el árbol rehacerse cuatro
    // veces seguidas sin saber por qué.
    class DeferredUiRebuild {
    public:
        explicit DeferredUiRebuild(MainWindow* w);
        ~DeferredUiRebuild();
        DeferredUiRebuild(const DeferredUiRebuild&) = delete;
        DeferredUiRebuild& operator=(const DeferredUiRebuild&) = delete;

    private:
        MainWindow* m_w;
    };
    int connectionIndexByNameOrId(const QString& value) const;
    void removeDuplicateMachineConnections(int keepIdx);
    void refreshAllConnections();
    void refreshSelectedConnection();
    void createConnection();
    void exportTrustStoreToSelectedConnection();
    void pushPeerCredentialsToConnection(int connIdx);
    void repairAltMountpointsForSelectedConnection();
    void editConnection();
    void deleteConnection();
    int currentConnectionIndexFromUnifiedTree() const;
    int currentConnectionIndexFromUi() const;
    void setCurrentConnectionInUi(int connIdx);
    QColor connectionStateRowColor(int connIdx) const;
    QString connectionStateColorReason(int connIdx) const;
    QString connectionStateTooltipHtml(int connIdx) const;
    void fillConnectionsMenu(QMenu* menu);
    void rebuildConnectionEntityTabs();
    struct DatasetTreeRenderOptions {
        bool includePoolRoot{false};
        bool interactiveConnContent{false};
    };
    DatasetTreeRenderOptions datasetTreeRenderOptionsForTree(const QTreeWidget* tree,
                                                             DatasetTreeContext side) const;
    void appendDatasetTreeForPool(QTreeWidget* tree,
                                  int connIdx,
                                  const QString& poolName,
                                  DatasetTreeContext side,
                                  const DatasetTreeRenderOptions& options,
                                  bool allowRemoteLoadIfMissing = true);
    void ensureConnectionRootAuxNodes(QTreeWidget* tree, QTreeWidgetItem* connRoot, int connIdx);
    bool applyConnectionInlineFieldValue(int connIdx,
                                         const QString& fieldKey,
                                         const QString& rawValue,
                                         QString* normalizedOut = nullptr,
                                         QString* errorOut = nullptr);
    void attachDatasetTreeSnapshotCombos(QTreeWidget* tree, DatasetTreeContext side);
    void populateConnectionPoolsIntoTree(QTreeWidget* tree,
                                         int connIdx,
                                         const ConnectionRuntimeState& st);
    void onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, DatasetTreeContext side);
    void refreshConnectionNodeDetails();
    void updateConnectionDetailTitlesForCurrentSelection();
    void saveConnContentTreeState(QTreeWidget* tree, const QString& token);
    void saveConnContentTreeStateFor(QTreeWidget* tree, const QString& token);
    void saveConnContentTreeState(const QString& token);
    void applyDebugNodeIdsToTree(QTreeWidget* tree);
    void restoreConnContentTreeState(QTreeWidget* tree, const QString& token);
    void restoreConnContentTreeStateFor(QTreeWidget* tree, const QString& token);
    void restoreConnContentTreeState(const QString& token);
    void rebuildConnContentTreeFor(QTreeWidget* tree,
                                   const QString& token,
                                   int connIdx,
                                   const QString& poolName,
                                   bool restoreState = true);
    void resizeTreeColumnsToVisibleContent(QTreeWidget* tree);
    int propColumnCountForTree(const QTreeWidget* tree) const;
    void syncConnContentPropertyColumns(QTreeWidget* tree);
    void syncConnContentPropertyColumnsFor(QTreeWidget* tree, const QString& token);
    void syncConnContentPropertyColumns();
    void syncConnContentPoolColumns(QTreeWidget* tree, const QString& token);
    void syncConnContentPoolColumnsFor(QTreeWidget* tree, const QString& token);
    void syncConnContentPoolColumns();
    void updateConnContentPropertyValues(const QString& token,
                                         const QString& objectName,
                                         const QMap<QString, QString>& valuesByProp);
    void updateConnContentDraftValue(const QString& token,
                                     const QString& objectName,
                                     const QString& prop,
                                     const QString& value);
    void updateConnContentDraftInherit(const QString& token,
                                       const QString& objectName,
                                       const QString& prop,
                                       bool inherit);
    void authorizePublicKeyOnConnection(int srcIdx, int dstIdx);
    bool validatePendingGsaDrafts(QString* errorOut = nullptr);
    // Redacta el motivo tipado que devuelve `zfsmgr::base::gsa`, en el idioma en curso.
    QString gsaMensajeDeMotivo(const zfsmgr::base::gsa::Reason& m, const QString& dataset) const;

    ConnectionRuntimeState refreshConnection(const ConnectionProfile& p);
    bool runSsh(const ConnectionProfile& p,
                const QString& remoteCmd,
                int timeoutMs,
                QString& out,
                QString& err,
                int& rc,
                const std::function<void(const QString&)>& onStdoutLine = {},
                const std::function<void(const QString&)>& onStderrLine = {},
                const std::function<void(int)>& onIdleTimeoutRemaining = {},
                const QByteArray& stdinPayload = {},
                // Lo pone a false runAgentCommand, que ya ha intentado el RPC con los
                // argumentos correctos. Sin esto, la cadena renderizada se volvería a
                // parsear aquí y un segundo intento podría salir con argumentos
                // truncados, que es justo el defecto que la migración elimina.
                bool allowAgentRpc = true,
                // Silencia el eco línea a línea de la salida al registro. Existe por el
                // volcado del log del daemon: se pide desde el desplazamiento 0 en cada
                // arranque, y con un daemon llevando días en marcha son ~13.000 líneas de
                // latidos que acababan en el registro de la aplicación, cada una con su
                // escritura a fichero y su llamada al registro de eventos de Windows. Su
                // destino es una vista propia, no el diagnóstico de la aplicación.
                bool echoOutputToLog = true);

    // Puerta única para pedirle algo al agente. Los sitios de llamada pasan argv y no
    // construyen cadenas de shell.
    bool runAgentCommand(const ConnectionProfile& p,
                         const QStringList& agentArgs,
                         int timeoutMs,
                         QString& out,
                         QString& err,
                         int& rc,
                         const QByteArray& stdinPayload = {});

    // Material TLS del daemon LOCAL. Vive bajo /etc/zfsmgr con permisos de root, así
    // que hay que leerlo elevando, igual que el de las conexiones remotas se trae por
    // SSH con sudo. Se cachea en memoria para no pedir credenciales en cada orden.
    bool runAgentOnConnection(const ConnectionProfile& p, const QStringList& agentArgs,
                              int timeoutMs, QString& out, QString& err, int& rc);
    bool ensureLocalDaemonTlsMaterial(QByteArray& serverCertPem,
                                      QByteArray& clientCertPem,
                                      QByteArray& clientKeyPem,
                                      quint16& daemonPort);

    bool tryAgentRpcOverSsh(const ConnectionProfile& p, const QStringList& agentArgs, int timeoutMs,
                            QString& out, QString& err, int& rc,
                            const std::function<void(const QString&)>& onStdoutLine = {},
                            const std::function<void(const QString&)>& onStderrLine = {},
                            // Mismo motivo que en runSsh: el volcado del log del
                            // daemon llega por aquí, y eran ~27.000 líneas de latidos
                            // eco al registro de la aplicación.
                            bool echoOutputToLog = true);
    // commandMayHaveRunOut, when non-null, is set to true as soon as the request has
    // been written to the daemon. From that point a failure is ambiguous: the daemon
    // may be executing the command right now, so retrying or falling back to SSH would
    // run it a second time. Closing the tunnel does NOT abort the remote work.
    bool tryRunRemoteAgentRpcViaTunnel(const ConnectionProfile& p,
                                       const QStringList& agentArgs,
                                       int timeoutMs,
                                       QString& out,
                                       QString& err,
                                       int& rc,
                                       QString* failureReason = nullptr,
                                       bool* commandMayHaveRunOut = nullptr);
    bool persistDaemonTlsMaterialForConnection(const ConnectionProfile& p,
                                               const QByteArray& serverCertPem,
                                               const QByteArray& clientCertPem,
                                               const QByteArray& clientKeyPem, quint16 daemonPort,
                                               QString* errorOut = nullptr);
    void closeAllRemoteDaemonRpcTunnels();
    QString daemonRpcBackoffTextForConnection(const ConnectionProfile& p) const;
    QString daemonRpcBackoffTextForConnection(int connIdx) const;
    // El texto de un motivo de fallo del transporte. La capa base lo devuelve TIPIFICADO
    // para poder decidir con él —reintentar, castigar, revivir el daemon— sin que esas
    // decisiones dependan de una frase que se puede reescribir o traducir; aquí, que es
    // donde hay idioma, se le pone texto. El CLI hace lo mismo por su cuenta.
    QString transportFailureText(const zfsmgr::base::transport::FailureReason& m) const;
    // Y el de un aviso del transporte, por lo mismo.
    QString transportNoticeText(const zfsmgr::base::transport::WarningNote& a) const;
    void closeAllSshControlMasters();
    QString withSudo(const ConnectionProfile& p, const QString& cmd) const;
    QString withSudoStreamInput(const ConnectionProfile& p, const QString& cmd) const;
    bool isLocalConnection(const ConnectionProfile& p) const;
    bool isLocalConnection(int connIdx) const;
    bool isWindowsConnection(const ConnectionProfile& p) const;
    bool isWindowsConnection(int connIdx) const;
    bool supportsAlternateDatasetMount(int connIdx) const;
    QString wrapRemoteCommand(const ConnectionProfile& p, const QString& remoteCmd) const;
    bool getDatasetProperty(int connIdx, const QString& dataset, const QString& prop,
                            QString& valueOut);
    QString effectiveMountPath(int connIdx, const QString& poolName, const QString& datasetName, const QString& mountpointHint, const QString& mountedValue);
    QString datasetCacheKey(int connIdx, const QString& poolName) const;
    QString datasetPermissionsCacheKey(int connIdx, const QString& poolName, const QString& datasetName) const;
    QString connectionAccountCacheKey(int connIdx) const;
    QString pendingDatasetRenameCommand(const PendingDatasetRenameDraft& draft) const;
    bool runDatasetRenameNow(const PendingDatasetRenameDraft& draft, QString* errorOut = nullptr);
    void updatePendingChangesList();
    void startPendingApplyAnimation();
    void finishPendingApplyAnimation();
    void buildDatasetPane(int paneIdx);
    // A connection's log or its daemon's, in a window of its own (Connections menu).
    void openConnectionLogWindow(int connIdx, bool daemon);
    // Before a connection's log owner is destroyed: its windows let go of its document.
    void releaseLogWindowsOf(const QString& connId);
    void refillDatasetPaneCombos();
    void onPaneConnectionChosen(int paneIdx);
    void onPanePoolChosen(int paneIdx);
    bool isPoolImportableForConnection(int connIdx, const QString& poolName) const;
    void importPoolByName(int connIdx, const QString& poolName);
    void rebuildDatasetPane(int paneIdx);
    void rebuildDatasetPanes();
    void populatePaneTree(QTreeWidget* tree, int connIdx, const QString& poolName);
    QWidget* buildPaneDetail(int paneIdx, QWidget* parent);
    void updatePaneDetail(int paneIdx);
    QString paneDetailPathHtml(int connIdx, const QString& poolName, const QString& datasetName,
                               const QString& snapshotName) const;
    void showPaneConnectionDetail(int paneIdx, int connIdx);
    void showPanePoolDetail(int paneIdx, int connIdx, const QString& poolName, bool allowLoad);
    void releaseSnapshotHoldNamed(int connIdx, const QString& poolName,
                                  const QString& datasetName, const QString& snapshotName,
                                  const QString& holdName);
    QWidget* buildPaneSchedulesTab(int paneIdx, QWidget* parent);
    void fillPaneSchedules(int paneIdx, int connIdx, const QString& poolName,
                           const QString& dataset);
    void loadPaneScheduleStatusAsync(int paneIdx);
    void applyPaneSchedule(int paneIdx);
    void clearPaneSchedule(int paneIdx);
    void runPaneScheduleNow(int paneIdx);
    void fillPanePermissions(int paneIdx, int connIdx, const QString& poolName,
                             const QString& datasetName);
    void commitPanePermissionGrant(int paneIdx, QTreeWidgetItem* grantNode);
    void fillPaneHolds(int paneIdx, int connIdx, const QString& poolName,
                       const QString& datasetName, const QString& snapshotName);
    void fillPaneContent(int paneIdx, int connIdx, const QString& poolName,
                         const QString& datasetName, const QString& snapshotName);
    void fillPaneSnapshots(int paneIdx, int connIdx, const QString& poolName,
                           const QString& datasetName, const QString& currentSnapshot);
    QVector<QPair<QString, QString>> connectionProfileRows(int connIdx) const;
    QVector<QPair<QString, QString>> connectionInfoRows(int connIdx) const;
    void appendPaneTreeRootedAtPool(QTreeWidget* tree, int connIdx, const QString& poolName,
                                    const QString& rootDataset, const QString& displayRoot);
    void installConnContentTreeHeaderContextMenu(QTreeWidget* tree);
    QString poolDetailsCacheKey(int connIdx, const QString& poolName) const;
    void readPoolGuids(int connIdx);
    bool ensureDatasetsLoaded(int connIdx, const QString& poolName, bool allowRemoteLoadIfMissing = true);
    bool ensureDatasetPermissionsLoaded(int connIdx, const QString& poolName, const QString& datasetName);
    bool ensureDatasetPermissionsLoadedBatch(int connIdx, const QString& poolName, const QStringList& datasetNames);
    bool ensurePoolDetailsLoaded(int connIdx, const QString& poolName);
    const PoolDetailsCacheEntry* poolDetailsEntry(int connIdx, const QString& poolName) const;
    void schedulePoolDetailsLoad(int connIdx, const QString& poolName);
    void applyPoolDetailsLoadResult(int connIdx,
                                    const QString& poolName,
                                    bool ok,
                                    const PoolDetailsCacheEntry& fresh,
                                    const QString& errorText);
    bool ensurePoolAutoSnapshotInfoLoaded(int connIdx, const QString& poolName);
    QMap<QString, QMap<QString, QString>> poolAutoSnapshotPropsByDataset(int connIdx, const QString& poolName) const;
    void invalidatePoolAutoSnapshotInfoForConnection(int connIdx);
    void preloadPoolAutoSnapshotInfoForConnection(int connIdx);
    bool schedulePoolAutoSnapshotInfoLoad(int connIdx, const QString& poolName);
    void applyPoolAutoSnapshotInfoLoadResult(int connIdx,
                                             const QString& poolName,
                                             bool ok,
                                             const QString& errorText,
                                             const QMap<QString, QMap<QString, QString>>& loaded);
    bool ensureDatasetSnapshotHoldsLoaded(int connIdx, const QString& poolName, const QString& objectName);
    QVector<QPair<QString, QString>> datasetSnapshotHolds(int connIdx, const QString& poolName, const QString& objectName) const;
    void invalidateDatasetPermissionsCacheForPool(int connIdx, const QString& poolName);
    void populateDatasetPermissionsNode(QTreeWidget* tree, QTreeWidgetItem* datasetItem, bool forceReload = false);
    void populateFileBrowserNode(QTreeWidget* tree, QTreeWidgetItem* browserNode);
    // Escribe las propiedades de una entrada de fichero en las columnas 4 en adelante, y
    // las deja guardadas en la propia fila para poder repintarlas si cambian las columnas.
    void writeFileBrowserPropCells(QTreeWidget* tree, QTreeWidgetItem* item,
                                   const QStringList& values);
    QStringList availableDelegablePermissions(const QString& datasetName,
                                              int connIdx,
                                              const QString& poolName,
                                              const QString& excludeSetName = QString()) const;
    void populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, DatasetTreeContext side, bool allowRemoteLoadIfMissing = true);
    void refreshDatasetProperties(const QString& side);
    void refreshDatasetProperties(const QString& side, QTreeWidget* connContentTree);
    void refreshConnContentPropertiesFor(QTreeWidget* tree);
    // `connIdxHint`/`poolHint`: la conexión y el pool a los que pertenece de verdad lo que se
    // marca. Quien llama desde una lista que YA los lleva (la pestaña Snapshots del detalle)
    // debe pasarlos: sin ellos la selección se daba por buena con `connIdx = -1` y la primera
    // acción de transferencia indexaba `m_conns.profiles[-1]`.
    void setSelectedDataset(const QString& side, const QString& datasetName,
                            const QString& snapshotName, int connIdxHint = -1,
                            const QString& poolHint = QString());
    DatasetSelectionContext currentDatasetSelection(const QString& side) const;
    DatasetSelectionContext currentConnContentSelection(const QTreeWidget* tree) const;
    DatasetSelectionContext normalizeDatasetSelectionContext(
        const DatasetSelectionContext& ctx, const QTreeWidget* treeHint = nullptr) const;
    QStringList daemonizeZfsMutationArgs(int connIdx, const QStringList& argv) const;
    QStringList daemonizeZpoolMutationArgs(int connIdx, const QStringList& argv) const;
    bool fetchConnectionCommandOutput(int connIdx, const QString& actionName,
                                      const QString& remoteCmd, QString* outputOut,
                                      QString* failureDetailOut = nullptr, int timeoutMs = 45000);
    bool ensureLocalSudoCredentials(ConnectionProfile& profile);
    bool hasEquivalentLocalSshConnection() const;
    void invalidateDatasetCacheEntry(int connIdx, const QString& poolName, const QString& objectName, bool invalidatePermissions = true);
    void invalidateDatasetSubtreeCacheEntries(int connIdx,
                                              const QString& poolName,
                                              const QString& datasetName,
                                              bool invalidatePermissions = true);
    void invalidatePoolDatasetListingCache(int connIdx, const QString& poolName);
    void invalidateDatasetCacheForPool(int connIdx, const QString& poolName);
    void invalidatePoolDetailsCacheForConnection(int connIdx);
    bool shouldRefreshSizePropsForCommand(const QString& actionLabel, const QString& command) const;
    bool refreshDatasetAndPoolSizeProperties(int connIdx,
                                             const QString& poolName,
                                             const QString& datasetName);
    void scheduleReloadFlush();
    void flushPendingReloads();
    void reloadConnContentPoolNow(int connIdx, const QString& poolName);
    void reloadConnContentPool(int connIdx, const QString& poolName);
    void reloadDatasetSide(const QString& side);
    void refreshPendingShellActionDraft(const PendingShellActionDraft& draft);
    void updateConnectionActionsState();
    bool isTransferVersionAllowed(const DatasetSelectionContext& src,
                                  const DatasetSelectionContext& dst,
                                  QString* reasonOut = nullptr) const;
    void executeConnectionTransferAction(const QString& action);
    void runTransferVerb(const QString& action);
    void setConnectionOriginSelection(const DatasetSelectionContext& ctx);
    void setConnectionDestinationSelection(const DatasetSelectionContext& ctx);
    bool connAdvancedDatasetActionAllowed(const DatasetSelectionContext& ctx) const;
    bool connDirectoryDatasetActionAllowed(const DatasetSelectionContext& ctx) const;
    QString connContentTokenForTree(const QTreeWidget* tree) const;
    void actionSendSnapshot();
    // `send` with extra words: "-R", "-w", "--resume", "--discard-partial".
    void cliSend(const QString& extra);
    // `rsync` with extra words: "--delete".
    void cliSync(const QString& extra);
    void actionCloneSnapshot();
    void actionDiffSnapshot();
    void actionLevelSnapshot();
    void actionSyncDatasets();
    void actionAdvancedBreakdown();
    void actionAdvancedBreakdown(const DatasetSelectionContext& explicitCtx);
    void actionAdvancedAssemble();
    void actionAdvancedAssemble(const DatasetSelectionContext& explicitCtx);
    void actionAdvancedCreateFromDir();
    void actionAdvancedCreateFromDir(const DatasetSelectionContext& explicitCtx);
    void actionAdvancedToDir();
    void actionAdvancedToDir(const DatasetSelectionContext& explicitCtx);
    bool mountDataset(const QString& side, const DatasetSelectionContext& ctx);
    bool umountDataset(const QString& side, const DatasetSelectionContext& ctx);
    void actionCreateChildDataset(const QString& side);
    void actionCreateChildDataset(const QString& side, const DatasetSelectionContext& explicitCtx);
    void actionDeleteDatasetOrSnapshot(const QString& side);
    void actionDeleteDatasetOrSnapshot(const QString& side,
                                       const DatasetSelectionContext& explicitCtx);
    void onDatasetPropsCellChanged(int row, int col);
    void applyDatasetPropertyChanges();
    void updateApplyPropsButtonState();
    void discardAllDraftEdits();
    QString connectionKeyByIndex(int connIdx) const;
    int connectionIndexByKey(const QString& key) const;
    void initLogPersistence();
    void rotateLogIfNeeded();
    void flushAppLogFile();
    void appendLogToFile(const QString& line);
    void appendLogToNative(const QString& level, const QString& line);
    void clearAppLog();
    void copyAppLogToClipboard();
    int maxLogLines() const;
    void trimLogWidget(QPlainTextEdit* widget);
    void syncConnectionLogTabs();
    void appendConnectionLog(const QString& connId, const QString& line);
    void refreshConnectionDaemonLogAsync(int idx, bool fullReset = false, bool userAsked = false);
    void runDaemonHeartbeat(const QString& connId);
    void pollDaemonZedAllConnections();
    void pollDaemonJobs();
    void scanOrphanedJobsForConnection(int connIdx);
    void updateJobsListWidget();
    // The jobs of every connection with a daemon, asked in the background.
    void refreshAllJobsAsync();
    void cancelSelectedJob();
    void onAsyncRefreshResult(int generation, int idx, const QString& connId, const ConnectionRuntimeState& state);
    void onAsyncRefreshDone(int generation);
    QString userExpandedKey(QTreeWidget* tree, QTreeWidgetItem* item) const;
    void applyUserExpandedState(QTreeWidget* tree);
    void loadUserExpandedState();
    void saveUserExpandedState();
    void stopAllDaemonEventWatchers();
    int findConnectionIndexByName(const QString& name) const;
    bool isConnectionRedirectedToLocal(int idx) const;
    QString connectionPersistKey(int idx) const;
    bool isConnectionDisconnected(int idx) const;
    bool isConnectionUnusable(int idx) const;
    void setConnectionDisconnected(int idx, bool disconnected);
    void refreshConnectionByIndex(int idx);
    bool installOrUpdateDaemonForConnectionInternal(int idx, bool interactive);
    void exportPoolFromRow(int row);
    void importPoolFromRow(int row);
    void scrubPoolFromRow(int row);
    void destroyPoolFromRow(int row);
    void createPoolForSelectedConnection();
    void refreshSelectedPoolDetails(bool forceRefresh = false, bool allowRemoteLoadIfMissing = true);
    void refreshPoolStatusNow(int connIdx, const QString& poolName);
    int findPoolRow(const QString& connection, const QString& pool) const;
    int selectedPoolRowFromTabs() const;
    int selectedConnectionIndexForPoolManagement() const;
    void updatePoolManagementBoxTitle();
    void updateStatus(const QString& text);
    // The shell service every action goes through (mainwindow_cli.cpp).
    void initCliService();
    void runCli(const QString& line, std::function<void(const CliService::Result&)> done = {},
                const QString& format = QString());
    // The zfsm:// URL of what is selected: a connection, a dataset or a snapshot.
    QString zfsmUrl(int connIdx, const QString& dataset = QString(),
                    const QString& snapshot = QString()) const;
    // A value as the shell's line takes it: as is, or in double quotes when it has spaces or
    // characters the line would split on.
    static QString cliQuote(const QString& value);
    // After a line that changed something under `ctx`: what the tree shows is read again.
    void refreshAfterCli(const DatasetSelectionContext& ctx);
    // The same, waiting for the answer (mainwindow_cli_actions.cpp).
    CliService::Result runCliAndWait(const QString& line, const QString& format = QString());
    // `<verb> <rest> --on <url of ctx>`; refreshes on success.
    bool cliRunOn(const DatasetSelectionContext& ctx, const QString& verb,
                  const QString& rest = QString());
    void cliDestroy(const DatasetSelectionContext& ctx, const QString& flags);
    void cliHold(const DatasetSelectionContext& ctx);
    // `<verb> <rest> --on zfsm://<conn>/<pool>`; the connection is read again on success.
    bool cliPool(int connIdx, const QString& pool, const QString& verb,
                 const QString& rest = QString());
    // `import <pool> [--as <new>] [-f] --on zfsm://<conn>`.
    bool cliImportPool(int connIdx, const QString& pool, const QString& newName, bool force);
    // A command whose answer is text to read (history, status): shown in a window.
    void cliShowOutput(const QString& line, const QString& title);
    void showCliText(const QString& title, const QString& text);
    // How the shell names a connection: its id, or its name when it has none.
    QString cliConnName(int connIdx) const;
    // A read of the shell, as JSON, on the process of that connection. Callable from any
    // thread. False (and `error`) when the line failed or did not answer JSON.
    bool cliReadJson(int connIdx, const QString& line, QJsonObject& out, QString* error = nullptr,
                     int timeoutMs = 120000);
    // The schedules of a connection as the shell's `schedule status <conn>` reports them.
    bool readScheduleStatus(int connIdx, zfsmgr::base::gsa::ConnectionState& out,
                            QString* error = nullptr);
    // The options of `create <id>` / `edit <id>` for what the connection dialog holds.
    static QString cliConnectionOptions(const ConnectionProfile& p, bool editing);
    void buildStatusBar();
    void composeStatusBar();
    void refreshStatusBarSchedulesAsync();
    void beginUiBusy();
    void endUiBusy();
    void beginTransientUiBusy(const QString& statusText);
    void endTransientUiBusy();
    void updateBusyCursor();
    QString defaultStatusTextForCurrentState() const;
    QString defaultIdleStatusText() const;
    bool actionsLocked() const;
    void loadUiSettings();
    void saveUiSettings() const;
    void applyLanguageLive();
    void openHelpTopic(const QString& topicId, const QString& titleOverride = QString());
    QString loadHelpTopicMarkdown(const QString& topicId) const;
    bool selectItemsDialog(const QString& title,
                           const QString& intro,
                           const QStringList& items,
                           QStringList& selected,
                           const QString& detail = QString(),
                           const QMap<QString, QString>& invalidItems = {});
    // Segunda columna con el nombre del dataset de cada fila: el que ya tiene
    // (Ensamblar) o el que va a tener (Desglosar, editable).
    //
    // Antes había aquí un `ancestorsRequired` que marcaba los ascendientes al marcar un
    // nodo, porque para crear <ds>/Disks/Bootables hace falta que exista <ds>/Disks. La
    // dependencia es real pero es entre NOMBRES, no entre directorios: `zfs create`
    // exige que exista el dataset padre, no que el directorio de encima sea un dataset.
    // Dando el nombre explícitamente —`Disks:Bootables`, hijo directo de <ds>— se puede
    // convertir un directorio sin convertir ninguno de los de encima, y la obligación
    // desaparece.
    struct TreeNameColumn {
        QString header;
        bool editable{false};
        // Nombre propuesto para `path` sabiendo qué rutas están marcadas. Se recalcula
        // en cada cambio de marca, salvo en las filas que el usuario haya editado.
        std::function<QString(const QString& path, const QSet<QString>& checked)> propose;
        QSet<QString> takenNames;      // nombres ya ocupados, para avisar del choque
        // Nombres con los que llega el diálogo, al re-editar un cambio encolado: se
        // respetan tal cual y no se recalculan, porque pueden haberse escrito a mano.
        QMap<QString, QString> initialNames;
        QMap<QString, QString> chosen; // salida: ruta -> nombre elegido
    };
    bool selectTreeItemsDialog(const QString& title,
                               const QString& intro,
                               const QStringList& items,
                               QStringList& selected,
                               const QString& detail = QString(),
                               const QMap<QString, QString>& invalidItems = {},
                               TreeNameColumn* nameColumn = nullptr);
    bool editInlinePropertiesDialog(const QString& title, const QString& intro,
                                    const QStringList& items, QStringList& selected,
                                    QVector<InlinePropGroupConfig>& groups,
                                    const QString& initialGroupName = QString());
    // Consulta única de "¿puede el usuario hacer esto en esta conexión?". Sustituye a
    // las comprobaciones sueltas de isWindowsConnection repartidas por 20 ficheros,
    // que es lo que hacía que las funciones se apagaran de una en una y que el usuario
    // se encontrara acciones que fallaban al pulsarlas.
    // El daemon es obligatorio: sin él no hay camino alternativo. Registra por qué no
    // está disponible y devuelve false, para que quien llama corte sin inventarse un
    // respaldo por shell. El respaldo ocultaba fallos reales del daemon durante meses.
    bool requireDaemonForRead(int connIdx, const QString& what) const;
    // Variante para el refresco, que trabaja con el estado que está construyendo y
    // todavía no tiene índice de conexión.
    bool requireDaemonForRead(const QString& connName,
                              const ConnectionRuntimeState& st,
                              const QString& what) const;

    zfsmgr::caps::Platform capabilityPlatform(int connIdx) const;
    bool featureAvailable(int connIdx, zfsmgr::caps::Feature f, QString* reasonOut = nullptr) const;
    // Igual que la anterior, pero además explica al usuario por qué no. Para usar como
    // primera línea de un slot: if (!requireFeature(idx, F::X)) return;
    bool requireFeature(int connIdx, zfsmgr::caps::Feature f);
    QString capabilityReasonText(zfsmgr::caps::Reason r) const;

    // Motivo único de que Send/Nivelar no estén disponibles con un extremo Windows: lo
    // usan el menú (para deshabilitar con explicación) y la comprobación de ejecución.
    QString streamingUnavailableReason(const QString& actionLabel) const;
    QString trk(const QString& key,
                const QString& es = QString(),
                const QString& en = QString()) const;
    QString maskSecrets(const QString& text) const;
    void logUiAction(const QString& action);
    void appLog(const QString& level, const QString& msg);
    void appendAppLogLineToView(const QString& fullLine);
    void loadPersistedAppLogToView();
    void populateAllPoolsTables();
    void enableSortableHeader(QTableWidget* table);
    void setTablePopulationMode(QTableWidget* table, bool populating);
    QString formatPoolStatusTooltipHtml(const QString& statusText) const;
    QString cachedPoolStatusTooltipHtml(int connIdx, const QString& poolName) const;
    bool isPoolSuspendedByStatusText(const QString& statusText) const;
    bool isPoolSuspended(int connIdx, const QString& poolName) const;
    void applyPoolRootTooltipForTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& statusText) const;
    void applyPoolRootTooltipToVisibleTrees(int connIdx, const QString& poolName, const QString& statusText) const;
    void cachePoolStatusTextsForConnection(int connIdx, const ConnectionRuntimeState& state);
    QString connStableIdForIndex(int connIdx) const;
    QString poolStableId(const PoolKey& key) const;
    void rebuildConnInfoModel();
    void rebuildConnInfoFor(int connIdx);
    void rebuildPoolInfoFromCache(PoolInfo& poolInfo, int connIdx, const QString& poolName, const PoolInfo* previousPoolInfo = nullptr);
    static DSKind dsKindFromNames(const QString& fullName, const QString& datasetType);
    const ConnInfo* findConnInfo(int connIdx) const;
    ConnInfo* findConnInfo(int connIdx);
    const PoolInfo* findPoolInfo(int connIdx, const QString& poolName) const;
    PoolInfo* findPoolInfo(int connIdx, const QString& poolName);
    const DSInfo* findDsInfo(int connIdx, const QString& poolName, const QString& fullName) const;
    DSInfo* findDsInfo(int connIdx, const QString& poolName, const QString& fullName);
    QStringList datasetSnapshotsFromModel(int connIdx, const QString& poolName, const QString& datasetName) const;
    bool datasetMountedFromModel(int connIdx, const QString& poolName, const QString& datasetName,
                                 QString* mountedValueOut = nullptr) const;
    QVector<DatasetPropCacheRow> datasetPropertyRowsFromModelOrCache(int connIdx, const QString& poolName, const QString& objectName) const;
    QVector<DatasetPropCacheRow> datasetPropertyRowsForNames(int connIdx,
                                                             const QString& poolName,
                                                             const QString& objectName,
                                                             const QStringList& propNames) const;
    QMap<QString, QString> datasetPropertyValuesForNames(int connIdx,
                                                         const QString& poolName,
                                                         const QString& objectName,
                                                         const QStringList& propNames) const;
    bool readObjectProperties(int connIdx, const QString& object,
                              QVector<DatasetPropCacheRow>& rows, QString* error = nullptr);
    bool ensureDatasetAllPropertiesLoaded(int connIdx, const QString& poolName, const QString& objectName);
    bool ensureDatasetPropertySubsetLoaded(int connIdx,
                                           const QString& poolName,
                                           const QString& objectName,
                                           const QStringList& propNames);
    void storeDatasetPropertyRows(int connIdx, const QString& poolName, const QString& objectName, const QString& datasetType, const QVector<DatasetPropCacheRow>& rows);
    void removeDatasetPropertyEntry(int connIdx, const QString& poolName, const QString& objectName);
    void removeDatasetPropertyEntriesForPool(int connIdx, const QString& poolName);
    DatasetPropsDraft propertyDraftForObject(const QString& side, const QString& token, const QString& objectName) const;
    void storePropertyDraftForObject(const QString& side, const QString& token, const QString& objectName, const DatasetPropsDraft& draft);
    QVector<PendingPropertyDraftEntry> pendingConnContentPropertyDraftsFromModel() const;
    QStringList scheduledDatasetsForPool(int connIdx,
                                         const QString& poolName,
                                         QMap<QString, QMap<QString, QString>>* propsOut) const;
    const DatasetPermissionsCacheEntry* datasetPermissionsEntry(int connIdx, const QString& poolName, const QString& datasetName) const;
    const DatasetPermissionsCacheEntry* ensureDatasetPermissionsEntryLoaded(int connIdx,
                                                                            const QString& poolName,
                                                                            const QString& datasetName);
    DatasetPermissionsCacheEntry* datasetPermissionsEntryMutable(int connIdx, const QString& poolName, const QString& datasetName);
    void mirrorDatasetPermissionsEntryToModel(int connIdx, const QString& poolName, const QString& datasetName);
    QVector<PendingPermissionDraftEntry> dirtyDatasetPermissionsEntriesFromModel() const;
    void removeDatasetPermissionsEntry(int connIdx, const QString& poolName, const QString& datasetName);
    void removeDatasetPermissionsEntriesForPool(int connIdx, const QString& poolName);
    void resetAllDatasetPermissionDrafts();

    // Perfiles y estados van juntos: son vectores paralelos indexados por connIdx y
    // el invariante vive en el propio registro. Ver connectionregistry.h.
    ConnectionRegistry m_conns;

    QTabWidget* m_rightTabs{nullptr};
    QWidget* m_pendingChangesTab{nullptr};
    QGroupBox* m_poolMgmtBox{nullptr};
    QAction* m_menuExitAction{nullptr};
    QLabel* m_connOriginSelectionLabel{nullptr};
    DatasetSelectionContext m_connActionOrigin;
    DatasetSelectionContext m_connActionDest;
    bool m_transferSelectionOverrideActive{false};
    DatasetSelectionContext m_transferSelectionOverrideOrigin;
    DatasetSelectionContext m_transferSelectionOverrideDest;

    QWidget* m_poolDetailTabs{nullptr};
    QWidget* m_connPropsGroup{nullptr};
    QSplitter* m_topMainSplit{nullptr};
    QSplitter* m_rightMainSplit{nullptr};
    QSplitter* m_verticalMainSplit{nullptr};
    QTableWidget* m_poolPropsTable{nullptr};
    QStackedWidget* m_connPropsStack{nullptr};
    QWidget* m_connPoolPropsPage{nullptr};
    QWidget* m_connContentPage{nullptr};
    ConnectionDatasetTreeWidget* m_topDatasetTreeWidget{nullptr};
    ConnectionDatasetTreePane* m_topDatasetPane{nullptr};
    QTreeWidget* m_connContentTree{nullptr};
    QTableWidget* m_connContentPropsTable{nullptr};
    QString m_connContentToken;
    QMap<QString, QMap<QString, QString>> m_connContentPropValuesByObject;
    QMap<QString, ConnContentTreeState> m_connContentTreeStateByToken;
    bool m_connContentTreeStateWriteLocked{false};
    QSet<QString> m_disconnectedConnectionKeys;
    QByteArray m_mainWindowGeometryState;
    QByteArray m_topMainSplitState;
    QByteArray m_rightMainSplitState;
    QByteArray m_verticalMainSplitState;
    QString m_persistedTopDetailConnectionKey;
    int m_topDetailConnIdx{-1};
    bool m_connSelectorDefaultsInitialized{false};
    bool m_rebuildingTopConnContentTree{false};
    ConnectionDatasetTreeWidget* m_bottomDatasetTreeWidget{nullptr};
    QTreeWidget* m_bottomConnContentTree{nullptr};
    QPlainTextEdit* m_poolStatusText{nullptr};
    QPushButton* m_poolStatusRefreshBtn{nullptr};
    QPushButton* m_poolStatusImportBtn{nullptr};
    QPushButton* m_poolStatusExportBtn{nullptr};
    QPushButton* m_poolStatusScrubBtn{nullptr};
    QPushButton* m_poolStatusDestroyBtn{nullptr};
    QStackedWidget* m_rightStack{nullptr};
    QPushButton* m_btnApplyConnContentProps{nullptr};
    QPushButton* m_btnDiscardPendingChanges{nullptr};

    // The STATUS BAR, the same line as the shell's status area: what is being done now, the
    // next scheduled snapshot, jobs, outdated daemons and the last error.
    struct LogWindow {
        QPointer<QDialog> dialog;
        QPointer<QPlainTextEdit> view;
        QString connId;
        bool daemon{false};
    };
    QList<LogWindow> m_logWindows;
    CliService* m_cli{nullptr};
    CliReader* m_cliReader{nullptr};
    bool m_cliRefused{false};  // the user answered «no» to the last confirmation
    // The answer to the secrets the shell asks for while one line runs, when a dialog already
    // took it (the password of the connection dialog). The caller resets it afterwards.
    std::optional<QString> m_cliPresetSecret;
    bool m_refreshAfterCli{true};  // off only in tests, where no machine is behind
    QLabel* m_statusBarLabel{nullptr};
    QString m_statusActivity;
    QString m_statusLastError;
    std::vector<zfsmgr::base::gsa::ConnectionState> m_statusSchedules;
    int m_statusJobsRunning{-1};  // -1 while not asked yet
    QTimer* m_statusScheduleTimer{nullptr};
    QGroupBox* m_transfersBox{nullptr};
    QGroupBox* m_pendingBox{nullptr};
    QPlainTextEdit* m_logView{nullptr};
    QListWidget* m_pendingChangesList{nullptr};
    // La columna de botones del panel de abajo. Se guarda para poder colgar allí los de
    // los trabajos, que se construyen más tarde y en otro sitio del mismo fichero.
    QVBoxLayout* m_pendingButtonsCol{nullptr};
    QTimer* m_pendingSpinnerTimer{nullptr};
    QTimer* m_autoRefreshTimer{nullptr};
    QTimer* m_jobPollTimer{nullptr};

    struct ActiveDaemonJob {
        int     srcConnIdx{-1};
        int     dstConnIdx{-1};
        QString jobId;
        QString displayLabel;
        QString state;    // "running"|"done"|"failed"|"cancelled"
        quint64 bytesTransferred{0};
        double  rateMiBs{0.0};
        long    elapsedSecs{0};
        QString lastProgressLine;
        QString startedAt;
        QString errorText;
    };
    QList<ActiveDaemonJob> m_activeDaemonJobs;
    QTreeWidget* m_jobsListWidget{nullptr};
    // A row of the Jobs box: what `jobs -a` shows for one job of one connection.
    struct DaemonJobRow {
        int connIdx{-1};
        QString connName;
        QString id;
        QString state;
        QString type;
        QString snap;
        qulonglong bytes{0};
        double rate{0.0};
        long long elapsed{0};
        QString error;
    };
    QVector<DaemonJobRow> m_allJobs;
    QTimer* m_allJobsTimer{nullptr};
    int m_pendingSpinnerFrame{0};
    bool m_pendingApplyInProgress{false};
    bool m_pendingApplyFinishSuppressed{false};
    // Los dos árboles fijos: el 0 a la izquierda es el ORIGEN y el 1 a la derecha el
    // DESTINO. La posición no es una preferencia de colocación: es lo que decide qué papel
    // juega cada selección en las seis acciones de transferencia. Antes había un solo árbol
    // con todas las conexiones dentro, el origen se marcaba a mano con «Marcar como origen»
    // y el destino era aquello sobre lo que se pulsaba; los dos papeles vivían en el mismo
    // sitio y no se distinguían mirando.
    struct DatasetPane {
        ConnectionDatasetTreeWidget* treeWidget{nullptr};
        MainWindowConnectionDatasetTreeDelegate* delegate{nullptr};
        QComboBox* connCombo{nullptr};
        QComboBox* poolCombo{nullptr};
        int connIdx{-1};
        // Vacío quiere decir «todos los pools de esa conexión», que es lo que muestra la
        // primera entrada del desplegable.
        QString poolName;
        // Mientras se rellenan los desplegables, sus señales no deben rehacer el árbol:
        // rellenar dispara currentIndexChanged aunque la elección del usuario no cambie.
        bool refilling{false};
        // El panel es una COLUMNA repartida en tres filas —árbol, detalle y log—, pero
        // las tres no viven juntas: cada una va en su fila del partidor compartido, que
        // es lo que hace que mover el divisor mueva los dos paneles a la vez.
        QWidget* treeBox{nullptr};
        QWidget* detailBox{nullptr};
        // El detalle de lo seleccionado, debajo del árbol.
        // Enseña una de tres cosas según qué haya marcado: la conexión del panel cuando
        // no hay nada marcado, el pool, o el dataset.
        QLabel* detailTitle{nullptr};
        QStackedWidget* detailStack{nullptr};
        QTableWidget* connDetailTable{nullptr};
        QTableWidget* poolDetailTable{nullptr};
        QPlainTextEdit* poolDetailStatus{nullptr};
        // El detalle de un dataset son dos cosas, y en pestañas: sus PROPIEDADES y sus
        // PERMISOS. Las dos se editaban dentro del árbol, tumbadas en las columnas
        // C1...C10; aquí cada una tiene su forma natural —tabla y árbol con casillas—.
        QTabWidget* datasetTabs{nullptr};
        QTableWidget* datasetDetailTable{nullptr};
        QTreeWidget* datasetPermsTree{nullptr};
        QTreeWidget* datasetContentTree{nullptr};
        QTreeWidget* datasetSnapsTree{nullptr};
        // El snapshot marcado en la pestaña «Snapshots», si hay alguno. El árbol ya no
        // los lleva dentro, así que sin esto el detalle no tendría forma de saber que lo
        // que se está mirando es una instantánea y no su dataset.
        QString snapshotSel;
        // Qué tramo del rótulo se ha pulsado: vacío, «conn» o «pool». Trae la ficha de
        // ese tramo sin tocar lo que haya marcado en el árbol, y se deshace al marcar
        // cualquier otra cosa.
        QString detailForced;
        QTableWidget* datasetHoldsTable{nullptr};
        // Schedules tab: what `schedule status` says and what `schedule` sets.
        QWidget* schedulesTab{nullptr};
        QTreeWidget* schedStatus{nullptr};
        QCheckBox* schedEnabled{nullptr};
        QCheckBox* schedRecursive{nullptr};
        QCheckBox* schedLevel{nullptr};
        QSpinBox* schedRetention[5]{};  // hourly, daily, weekly, monthly, yearly
        QLineEdit* schedTarget{nullptr};
        int schedConnIdx{-1};
        QString schedPool;
        QString schedDataset;
    };
    DatasetPane m_datasetPanes[2];
    // UN solo partidor para los dos paneles: tres filas —árboles, detalles, logs— y
    // dentro de cada fila las dos columnas al 50%, sin divisor entre ellas. Antes cada
    // panel tenía el suyo, así que el reparto vertical de la izquierda y el de la derecha
    // se descuadraban en cuanto se tocaba uno.
    QSplitter* m_paneRowsSplit{nullptr};
    QMap<QString, QPointer<QPlainTextEdit>> m_connectionLogViews;
    QMap<QString, QPointer<QPlainTextEdit>> m_connectionGsaLogViews;
    struct ConnCompactState {
        bool valid{false};
        QString date, time, conn, level;
    };
    QMap<QString, ConnCompactState> m_connCompactState;
    QMap<QString, ConnCompactState> m_connGsaCompactState;
    QMap<QString, qint64> m_connectionDaemonLogOffset;
    QSet<QString> m_daemonBootstrapPromptedConnIds;
    // Túneles, reintentos y el cerrojo que los protege, todo junto. Antes el cerrojo y
    // lo que protegía estaban separados y solo un comentario decía cuáles iban juntos.
    TransportSession m_transport;
    QMap<QString, RefreshRuntimeCacheEntry> m_refreshRuntimeCacheByConnId;
    QMap<QString, QStringList> m_connSystemUsersCacheByKey;
    QMap<QString, QStringList> m_connSystemGroupsCacheByKey;
    QSet<QString> m_connSystemUsersLoadedKeys;
    QSet<QString> m_connSystemGroupsLoadedKeys;
    QString m_propsSide;
    QString m_propsDataset;
    QString m_propsToken;
    QMap<QString, QString> m_propsOriginalValues;
    QMap<QString, bool> m_propsOriginalInherit;
    bool m_propsDirty{false};
    bool m_loadingPropsTable{false};
    bool m_loadingDatasetTrees{false};
    QString m_language{QStringLiteral("es")};
    bool m_actionConfirmEnabled{true};
    int m_logMaxSizeMb{10};
    QString m_logLevelSetting{QStringLiteral("normal")};
    int m_logMaxLinesSetting{500};
    bool m_showInlineDatasetProps{true};
    int m_connPropColumnsSetting{7};
    QStringList m_datasetInlinePropsOrder;
    QVector<InlinePropGroupConfig> m_datasetInlinePropGroups;
    QStringList m_poolInlinePropsOrder;
    QVector<InlinePropGroupConfig> m_poolInlinePropGroups;
    QStringList m_snapshotInlineVisibleProps;
    QVector<InlinePropGroupConfig> m_snapshotInlinePropGroups;
    QString m_appLogPath;
    // Descriptor persistente del registro. Antes cada línea abría el fichero, escribía,
    // vaciaba, cerraba, y además hacía un stat para decidir si rotar. En Linux el caché
    // de página lo disimula; en Windows son 0,76 ms por línea (medido en una VM), y un
    // refresco emite decenas de miles: ~31 s de reloj sin contar los demás registros.
    // Se mantiene abierto y el tamaño se lleva contando bytes, sin volver a preguntar
    // al sistema de ficheros.
    std::unique_ptr<QFile> m_appLogFile;
    qint64 m_appLogBytes{0};
    bool m_compactPrevValid{false};
    QString m_compactPrevDate;
    QString m_compactPrevTime;
    QString m_compactPrevConn;
    QString m_compactPrevLevel;
    int m_refreshGeneration{0};
    int m_refreshPending{0};
    int m_refreshTotal{0};
    bool m_refreshInProgress{false};
    // Contador de guardianes DeferredUiRebuild vivos, y qué quedó pendiente.
    int m_uiRebuildDeferred{0};
    bool m_uiRebuildPendingTable{false};
    bool m_uiRebuildPendingPools{false};
    bool m_uiRebuildPendingNodeDetails{false};
    UiRebuildCountsForTest m_uiRebuildCounts;
    int m_zedPollPending{0};
    QMap<QString, bool> m_userNodeExpanded;
    QTimer* m_userExpandedSaveTimer{nullptr};
    QSet<int> m_refreshingByIndex;
    bool m_initialRefreshCompleted{false};
    QString m_localSudoUsername;
    QString m_localSudoPassword;
    QString m_localMachineUuid;
    bool m_startupLocalSudoChecked{false};
    bool m_actionsLocked{false};
    bool m_waitCursorActive{false};
    int m_uiBusyDepth{0};
    bool m_closing{false};
    // Set by the destructor only. Pool tasks read it from their threads, so it is atomic; once
    // set, nothing more is sent to the window's thread (see runWhereTunnelsAllowed).
    std::atomic<bool> m_destroying{false};
    QStringList m_transientStatusStack;
    // La última acción terminó porque el usuario la detuvo, no por un fallo. Sin esto,
    // el bucle que aplica los cambios pendientes no distingue una cosa de la otra y
    // saca "Error ejecutando cambio pendiente" tras una cancelación pedida a propósito.
    bool m_lastActionWasCancelled{false};
    // Semilla para re-editar un cambio ya encolado: se quita de la lista, se guarda aquí
    // lo que se le pidió, y se vuelve a abrir su diálogo con eso ya puesto. Las tres
    // acciones que la usan guardan su entrada completa en datasetActionArgv, así que no
    // hace falta almacenar nada más; "Desde Dir" no, porque su orden es una tubería de
    // shell y su diálogo tiene mucho más estado del que cabe en la orden.
    QProcess* m_activeLocalProcess{nullptr};
    qint64 m_activeLocalPid{-1};
    bool m_busyOnImportRefresh{false};
    QAction* m_confirmActionsMenuAction{nullptr};
    QSet<QString> m_poolDetailsLoadsInFlight;
    QSet<QString> m_poolAutoSnapshotLoadsInFlight;
    QMap<int, int> m_poolAutoSnapshotPendingLoadsByConn;
    QMap<int, QSet<QString>> m_poolAutoSnapshotDirtyPoolsByConn;
    QSet<int> m_poolAutoSnapshotUiDeferByConn;
    QSet<QString> m_pendingConnContentPoolReloadKeys;
    QSet<int> m_pendingConnectionRefreshIndices;
    bool m_reloadFlushScheduled{false};
};
