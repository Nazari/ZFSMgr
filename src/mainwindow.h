#pragma once

#include "connectionmodel.h"
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
#include <QFile>

#include <memory>
#include <thread>

class QTimer;
class QComboBox;
class QColor;
class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPoint;
class QPlainTextEdit;
class QProcess;
class QPushButton;
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
    struct AgentCallForTest {
        QStringList argv;        // vacío si la orden salió como cadena de shell
        QString shellCommand;    // no vacío solo en ese caso
        QByteArray stdinPayload;
    };
    using AgentTransportForTest =
        std::function<bool(const QStringList& argv, QString& out, QString& err, int& rc)>;
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
    void clearAgentCallsForTest();
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
    void setShowPoolInfoNodeForTest(bool visible);
    void setShowInlineGsaNodeForTest(bool visible);
    void setShowAutomaticSnapshotsForTest(bool visible);
    void setConnectionGsaStateForTest(int connIdx, bool installed, bool active, const QString& version = QString());
    void configureDatasetPropertiesForTest(int connIdx,
                                           const QString& objectName,
                                           const QString& datasetType,
                                           const QVector<UiTestPropertySeed>& rows);
    void debugTrace(const QString& msg);
    QStringList topLevelPoolNamesForTest(bool bottom = false) const;
    QStringList childLabelsForDatasetForTest(const QString& datasetName, bool bottom = false) const;
    QStringList snapshotNamesForDatasetForTest(const QString& datasetName, bool bottom = false) const;
    bool selectDatasetForTest(const QString& datasetName, bool bottom = false);
    bool setDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool expanded, bool bottom = false);
    bool isDatasetChildExpandedForTest(const QString& datasetName, const QString& childLabel, bool bottom = false) const;
    void rebuildConnContentTreeForTest(const QString& datasetToSelect, bool bottom = false);
    QStringList connectionContextMenuTopLevelLabelsForTest() const;
    QStringList connectionRefreshMenuLabelsForTest() const;
    QStringList poolContextMenuLabelsForTest(const QString& poolName, bool bottom = false) const;

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
        bool dirty{false};
    };

    struct PendingDatasetRenameDraft {
        int connIdx{-1};
        QString poolName;
        QString sourceName;
        QString targetName;
    };

    // Entrada del diálogo de «Desde Dir», para poder reabrirlo tal y como se dejó.
    //
    // Las otras tres acciones re-editables no necesitan esto: su entrada ES la orden
    // tipada que ya se guarda (`datasetActionArgv`). La de Desde Dir no se puede
    // reconstruir de su orden, que es una tubería `tar | ssh | tar` con la selección ya
    // resuelta y las propiedades ya convertidas en argumentos de `zfs create`.
    //
    // Las conexiones van por identificador, no por índice, porque esto se escribe en
    // disco y los índices se mueven al añadir o borrar una conexión.
    //
    // La frase de cifrado NO está aquí a propósito, ni siquiera en memoria: al re-editar
    // se vuelve a pedir. Es un campo de contraseña con repetición; rellenarlo solo para
    // que el usuario lo acepte sin mirar sería peor que dejarlo vacío.
    struct FromDirInput {
        bool valid{false};
        QString datasetPath;
        QString blocksize;
        bool parents{true};
        QStringList properties;   // "nombre=valor", tal cual los recoge el diálogo
        QString extraArgs;
        QVector<QPair<QString, QString>> sources;   // clave de conexión, ruta
        bool deleteSourceDirs{false};
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
        // `rpcSecret` se guarda aparte y NUNCA se copia a `command`, `displayLabel` ni
        // al identificador estable, que son los tres sitios que se muestran o registran.
        int rpcConnIdx{-1};
        QStringList rpcArgv;   // sin el secreto: se añade al ejecutar
        QString rpcSecret;
        // Acción de dataset diferida. Desglosar y Ensamblar se ejecutaban en el acto,
        // a diferencia del resto: no pasaban por la lista de cambios pendientes, así
        // que no se podían revisar antes ni quitar de la cola. Guardando aquí lo que
        // necesita executeDatasetAction, se encolan como las demás y al aplicarlas
        // conservan TODO lo suyo: el envío como trabajo del daemon, el progreso en
        // tiempo real y la regla de no reintentar una mutación por SSH.
        QString datasetActionSide;
        QString datasetActionName;   // no vacío = es una acción de dataset diferida
        DatasetSelectionContext datasetActionCtx;
        QStringList datasetActionArgv;   // vacío en los caminos de respaldo por shell
        QByteArray datasetActionStdin;
        bool datasetActionAllowWindowsScript{false};
        // Identidad propia de la entrada, y lo que el usuario decide sobre ella.
        //
        // Antes la entrada se identificaba por su texto —`displayLabel` + `command`—,
        // que bastaba mientras moría al ejecutarse. Ya no muere: se le puede poner
        // nombre y se puede volver a lanzar, así que necesita una clave que NO cambie
        // cuando cambia lo que se ve.
        QString uid;
        QString userName;   // puesto por el usuario; vacío = se muestra displayLabel
        bool active{true};  // ¿entra en «Aplicar cambios»?
        // Qué binario construyó la orden que hay en `command`.
        //
        // Lo encolado guarda la orden YA CONSTRUIDA, así que arreglar el código no
        // arregla lo que ya está en la lista: sigue ahí la orden de antes, y ahora
        // además sobrevive a los reinicios. Pasó de verdad —un «Montar» encolado antes
        // de que Montar aprendiera a cargar la clave de un dataset cifrado se aplicó
        // días después y falló con «encryption key not loaded», sin que nada indicara
        // que la orden era vieja—.
        //
        // No basta el número de versión: aquel caso tenía la misma a los dos lados
        // (0.90.14 encoló y 0.90.14 ejecutó, con el arreglo entre medias). Por eso la
        // huella lleva además la marca de tiempo del ejecutable, que sí cambia en cada
        // recompilación. Vacío = encolada antes de que esto existiera, o sea vieja.
        QString createdByBuild;
        // Solo en Desde Dir. No se usa para ejecutar —para eso está `command`—, solo
        // para volver a abrir su diálogo.
        FromDirInput fromDirInput;
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
        Entry copy;
        Entry clone;
        Entry move;
        Entry level;
        Entry sync;
        Entry diff;
    };
    TransferActionAvailability transferActionAvailabilityFor(const DatasetSelectionContext& src,
                                                             const DatasetSelectionContext& dst);

    struct PendingChange {
        enum class Kind {
            Property,
            Permission,
            Rename,
            ShellAction,
        };
        Kind kind{Kind::Property};
        QString stableId;
        QString displayLine;
        QString commandLine;
        int connIdx{-1};
        QString poolName;
        QString objectName;
        QString propertyName;
        bool focusPermissionsNode{false};
        bool removableIndividually{false};
        bool executableIndividually{false};
        // Solo las acciones sobreviven a su ejecución, se nombran y se activan. Las
        // propiedades, los permisos y los renombrados son EDICIONES de un estado: tienen
        // un final natural —una vez aplicadas ya no hay nada que reaplicar—, así que
        // siguen desapareciendo de la lista al aplicarse.
        bool activatable{false};
        QString uid;
        QString userName;
        bool active{true};
        bool staleBuild{false};
        PendingDatasetRenameDraft renameDraft;
        PendingShellActionDraft shellDraft;
    };



















    void buildUi();
    void loadConnections();
    void ensureStartupLocalSudoConnection();
    void rebuildConnectionsTable();
    // Reintroduce usuario y contraseña de sudo de la conexión Local, que es la única
    // que no se puede editar. Valida la contraseña antes de guardarla.
    void changeLocalSudoCredentials();
    // Ofrece reintroducirlas cuando una operación en Local falla porque sudo rechaza
    // la contraseña. Se ofrece una sola vez por sesión hasta que se corrija: un fallo
    // de sudo se repite en cada comando y si no, saldrían diálogos en cadena.
    void offerLocalSudoCredentialFix();
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
    bool connectionsReferToSameMachine(int a, int b) const;
    int equivalentSshForLocal(int localIdx) const;
    void removeDuplicateMachineConnections(int keepIdx);
    bool canSshBetweenConnections(int rowIdx, int colIdx, QString* errorOut = nullptr, int* effectiveDstIdxOut = nullptr);
    void refreshAllConnections();
    void refreshSelectedConnection();
    void createConnection();
    void exportTrustStoreToSelectedConnection();
    void installHelperCommandsForSelectedConnection();
    void repairAltMountpointsForSelectedConnection();
    void editConnection();
    void deleteConnection();
    int currentConnectionIndexFromUnifiedTree() const;
    int currentConnectionIndexFromUi() const;
    void setCurrentConnectionInUi(int connIdx);
    QColor connectionStateRowColor(int connIdx) const;
    QString connectionStateColorReason(int connIdx) const;
    QString connectionStateTooltipHtml(int connIdx) const;
    void openConnectivityMatrixDialog();
    void showConnectionContextMenu(int connIdx, const QPoint& globalPos, QTreeWidget* sourceTree = nullptr);
    void onConnectionSelectionChanged();
    void updateSecondaryConnectionDetail();
    void rebuildConnectionEntityTabs();
    struct DatasetTreeRenderOptions {
        bool includePoolRoot{false};
        bool interactiveConnContent{false};
        bool showInlinePropertyNodes{true};
        bool showInlinePermissionsNodes{true};
        bool showInlineGsaNode{true};
        bool showAutomaticSnapshots{true};
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
    void onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, DatasetTreeContext side, const QString& chosen);
    void onDatasetTreeItemChanged(QTreeWidget* tree, QTreeWidgetItem* item, int col, DatasetTreeContext side);
    void clearOtherSnapshotSelections(QTreeWidget* tree, QTreeWidgetItem* keepItem);
    void refreshConnectionNodeDetails();
    void updateConnectionDetailTitlesForCurrentSelection();
    void rebuildConnContentDetailTree(QTreeWidget* tree,
                                      int connIdx,
                                      bool& rebuildingFlag,
                                      int* forceRestoreConnIdx,
                                      const std::function<void(int)>& saveTreeState,
                                      const std::function<void()>& clearPendingState = {});
    void saveTopTreeStateForConnection(int connIdx);
    void saveBottomTreeStateForConnection(int connIdx);
    void restoreTopTreeStateForConnection(int connIdx);
    void restoreBottomTreeStateForConnection(int connIdx);
    void saveConnContentTreeState(QTreeWidget* tree, const QString& token);
    void saveConnContentTreeStateFor(QTreeWidget* tree, const QString& token);
    void saveConnContentTreeState(const QString& token);
    void setConnContentTreeStateWriteLocked(bool locked);
    bool connContentTreeStateWriteLocked() const;
    void applyDebugNodeIdsToTree(QTreeWidget* tree);
    void restoreConnContentTreeState(QTreeWidget* tree, const QString& token);
    void restoreConnContentTreeStateFor(QTreeWidget* tree, const QString& token);
    void restoreConnContentTreeState(const QString& token);
    void rebuildConnContentTreeFor(QTreeWidget* tree,
                                   const QString& token,
                                   int connIdx,
                                   const QString& poolName,
                                   bool restoreState = true);
    QTreeWidgetItem* findConnContentDatasetItemFor(QTreeWidget* tree,
                                                   int connIdx,
                                                   const QString& poolName,
                                                   const QString& datasetName) const;
    QString connectionDisplayModeForIndex(int connIdx) const;
    void syncConnectionDisplaySelectors();
    void applyConnectionDisplayMode(int connIdx, const QString& mode);
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
    bool showAutomaticSnapshots() const;
    bool validatePendingGsaDrafts(QString* errorOut = nullptr);

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
    bool ensureLocalDaemonTlsMaterial(QByteArray& serverCertPem,
                                      QByteArray& clientCertPem,
                                      QByteArray& clientKeyPem,
                                      quint16& daemonPort);

    AgentTransportForTest m_agentTransportForTest;
    QVector<AgentCallForTest> m_agentCallsForTest;

    bool tryAgentRpcOverSsh(const ConnectionProfile& p,
                            const QStringList& agentArgs,
                            int timeoutMs,
                            QString& out,
                            QString& err,
                            int& rc,
                            const std::function<void(const QString&)>& onStdoutLine = {},
                            const std::function<void(const QString&)>& onStderrLine = {},
                            // Mismo motivo que en runSsh: el volcado del log del
                            // daemon llega por aquí, y eran ~27.000 líneas de latidos
                            // eco al registro de la aplicación.
                            bool echoOutputToLog = true);
    // Submits a long-running agent mutation as a background job and polls until it
    // finishes. Removes the RPC read timeout from the equation: the submission is
    // instant and the outcome is asked for afterwards, so a slow operation can no
    // longer look like a failure. Returns false only if the job could not be
    // submitted or tracked; the operation's own exit code comes back in rc.
    // jobSubmittedOut distinguishes "could not submit" (nothing ran, a fallback is
    // safe) from "submitted but lost track of it" (the daemon is working right now,
    // so re-running the command would duplicate it).
    bool runAgentMutationAsJob(const ConnectionProfile& p,
                               const QStringList& agentArgs,
                               QString& out,
                               QString& err,
                               int& rc,
                               const std::function<void(const QString&)>& progressCb = {},
                               bool* jobSubmittedOut = nullptr);
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
                                               const QByteArray& clientKeyPem,
                                               quint16 daemonPort,
                                               QString* errorOut = nullptr);
    bool cacheDaemonTlsMaterialForConnection(const ConnectionProfile& p, QString* errorOut = nullptr);
    bool cleanupRemoteDaemonClientPrivateKey(const ConnectionProfile& p, QString* errorOut = nullptr);
    void closeAllRemoteDaemonRpcTunnels();
    void clearDaemonRpcStateForConnection(const ConnectionProfile& p);
    void clearDaemonRpcBackoffForConnection(const ConnectionProfile& p);
    QString daemonRpcBackoffTextForConnection(const ConnectionProfile& p) const;
    QString daemonRpcBackoffTextForConnection(int connIdx) const;
    void closeAllSshControlMasters();
    QString withSudo(const ConnectionProfile& p, const QString& cmd) const;
    QString withSudoStreamInput(const ConnectionProfile& p, const QString& cmd) const;
    bool isLocalConnection(const ConnectionProfile& p) const;
    bool isLocalConnection(int connIdx) const;
    bool isWindowsConnection(const ConnectionProfile& p) const;
    bool isWindowsConnection(int connIdx) const;
    bool supportsAlternateDatasetMount(int connIdx) const;
    QString wrapRemoteCommand(const ConnectionProfile& p,
                              const QString& remoteCmd) const;
    QString sshExecFromLocal(const ConnectionProfile& p,
                             const QString& remoteCmd) const;
    bool getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut);
    // Testigo de reanudación del destino, o cadena vacía si no hay transferencia a
    // medias. Se lee con --dump-zfs-get-prop, que ya existía: no hace falta un verbo
    // nuevo, y por tanto tampoco cambiar la versión del esquema del agente.
    // Devuelve el testigo de reanudación del destino, o vacío. holderOut recibe el
    // dataset que lo tiene, que NO tiene por qué ser el de destino: en una copia
    // recursiva el testigo queda en el descendiente que se estaba recibiendo.
    QString transferResumeTokenFor(int connIdx, const QString& dataset,
                                   QString* holderOut = nullptr);
    // Dirección con la que el ORIGEN ve a este equipo, para que pueda conectar de vuelta
    // cuando el destino es la conexión Local. Vacío si no se puede averiguar.
    QString sourceViewOfThisHost(int srcConnIdx);
    QString effectiveMountPath(int connIdx, const QString& poolName, const QString& datasetName, const QString& mountpointHint, const QString& mountedValue);
    QString datasetCacheKey(int connIdx, const QString& poolName) const;
    QString datasetPermissionsCacheKey(int connIdx, const QString& poolName, const QString& datasetName) const;
    QString connectionAccountCacheKey(int connIdx) const;
    QString pendingDatasetRenameCommand(const PendingDatasetRenameDraft& draft) const;
    bool queuePendingDatasetRename(const PendingDatasetRenameDraft& draft, QString* errorOut = nullptr);
    QVector<PendingChange> pendingChanges() const;
    bool findPendingChangeByDisplayLine(const QString& line, PendingChange* out) const;
    QStringList pendingConnContentApplyCommands() const;
    QStringList pendingConnContentApplyDisplayLines() const;
    void activatePendingChangeAtCursor();
    bool focusPendingChangeLine(const QString& line);
    void updatePendingChangesList();
    void startPendingApplyAnimation();
    void finishPendingApplyAnimation();
    void splitAndRootConnContent(Qt::Orientation orientation, bool insertBefore, int connIdx,
                                  const QString& poolName, const QString& rootDataset,
                                  QTreeWidget* sourceTree = nullptr);
    void closeSplitTree(QTreeWidget* tree);
    void rebuildAllSplitTrees();
    QString serializeSplitTreeLayoutState() const;
    void restoreSplitTreeLayoutFromState(const QString& state);
    void appendSplitDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName,
                                 const QString& rootDataset, const QString& displayRoot);
    void appendSplitDatasetTreeForConnection(QTreeWidget* tree, int connIdx);
    void installConnContentTreeHeaderContextMenu(QTreeWidget* tree);
    QString poolDetailsCacheKey(int connIdx, const QString& poolName) const;
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
    // Repinta las propiedades de las filas de fichero YA cargadas. Se llama al cambiar el
    // número de columnas: sin esto, ampliarlas solo surte efecto en lo que se abra después.
    void reapplyFileBrowserPropertyCells(QTreeWidget* tree);
    QStringList availableDelegablePermissions(const QString& datasetName,
                                              int connIdx,
                                              const QString& poolName,
                                              const QString& excludeSetName = QString()) const;
    void populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, DatasetTreeContext side, bool allowRemoteLoadIfMissing = true);
    void refreshDatasetProperties(const QString& side);
    void refreshDatasetProperties(const QString& side, QTreeWidget* connContentTree);
    void refreshConnContentPropertiesFor(QTreeWidget* tree);
    void setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName);
    DatasetSelectionContext currentDatasetSelection(const QString& side) const;
    DatasetSelectionContext currentConnContentSelection(const QTreeWidget* tree) const;
    DatasetSelectionContext normalizeDatasetSelectionContext(const DatasetSelectionContext& ctx,
                                                            const QTreeWidget* treeHint = nullptr) const;
    bool executeDatasetAction(const QString& side,
                              const QString& actionName,
                              const DatasetSelectionContext& ctx,
                              const QString& cmd,
                              int timeoutMs = 45000,
                              bool allowWindowsScript = false,
                              const QByteArray& stdinPayload = {},
                              bool invalidatePoolCache = true,
                              const std::function<void()>& onSuccessRefresh = {},
                              // Argumentos del agente cuando el llamante los tiene. Con
                              // ellos la orden viaja por RPC sin pasar por el parseo de
                              // la cadena, y el ciclo de trabajos usa el túnel ya
                              // abierto en vez de un SSH con sudo por segundo.
                              const QStringList& agentArgvIn = {});
    bool executePendingDatasetRenameDraft(const PendingDatasetRenameDraft& draft,
                                          bool interactiveErrorDialog = true,
                                          QString* failureDetailOut = nullptr);
    bool executePoolCommand(int connIdx,
                            const QString& poolName,
                            const QString& actionName,
                            const QString& remoteCmd,
                            int timeoutMs,
                            QString* failureDetailOut = nullptr,
                            bool refreshPoolsTable = false,
                            bool refreshSelectedPoolDetailsAfter = false);
    QStringList daemonizeZfsMutationArgs(int connIdx, const QString& rawCmd) const;
    QStringList daemonizeZpoolMutationArgs(int connIdx, const QString& rawCmd) const;
    QStringList daemonizeShellMutationArgs(int connIdx, const QString& rawShell) const;
    QStringList daemonizeLocalSendRecvArgs(int connIdx,
                                          const QString& sendRawCmd,
                                          const QString& recvRawCmd) const;
    QStringList daemonizeCopyTreeSyncArgs(int connIdx,
                                          const QString& srcPath,
                                          const QString& dstPath,
                                          bool useDelete,
                                          bool dryRun) const;
    QStringList daemonizeRsyncSyncArgs(int connIdx,
                                      const QList<QPair<QString, QString>>& pathPairs,
                                      bool useDelete,
                                      bool dryRun,
                                      const QString& rsh,
                                      const QString& dstHost) const;
    bool fetchPoolCommandOutput(int connIdx,
                                const QString& poolName,
                                const QString& actionName,
                                const QString& remoteCmd,
                                QString* outputOut,
                                QString* failureDetailOut = nullptr,
                                int timeoutMs = 45000);
    bool executeConnectionCommand(int connIdx,
                                  const QString& actionName,
                                  const QString& remoteCmd,
                                  int timeoutMs,
                                  QString* failureDetailOut = nullptr,
                                  const QByteArray& stdinPayload = QByteArray());
    bool fetchConnectionCommandOutput(int connIdx,
                                      const QString& actionName,
                                      const QString& remoteCmd,
                                      QString* outputOut,
                                      QString* failureDetailOut = nullptr,
                                      int timeoutMs = 45000);
    bool fetchConnectionProbeOutput(int sourceConnIdx,
                                    const QString& actionName,
                                    const QString& remoteCmd,
                                    QString* mergedOutputOut,
                                    QString* failureDetailOut = nullptr,
                                    int timeoutMs = 12000);
    bool ensureLocalSudoCredentials(ConnectionProfile& profile);
    bool hasEquivalentLocalSshConnection() const;
    QString diagnoseUmountFailure(const DatasetSelectionContext& ctx);
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
    int pendingShellSingleConnectionIdx(const PendingShellActionDraft& draft) const;
    bool tryExecutePendingShellActionRemotely(const PendingShellActionDraft& draft,
                                              bool* handledOut,
                                              QString* failureDetailOut = nullptr);
    void refreshPendingShellActionDraft(const PendingShellActionDraft& draft);
    void updateConnectionActionsState();
    bool isTransferVersionAllowed(const DatasetSelectionContext& src,
                                  const DatasetSelectionContext& dst,
                                  QString* reasonOut = nullptr) const;
    // Encola una acción de dataset en vez de ejecutarla. Devuelve si se encoló.
    //
    // Las que llevan un secreto por la entrada estándar —montar un dataset cifrado,
    // crear uno cifrado— NO deben pasar por aquí: encolarlas obliga a sostener la frase
    // en memoria mientras el cambio espera en la lista, que puede ser minutos.
    bool queueDatasetAction(const QString& side,
                            const QString& actionName,
                            const QString& displayLabel,
                            const DatasetSelectionContext& ctx,
                            const QString& cmd,
                            bool allowWindowsScript = false,
                            const QStringList& agentArgv = {});
    bool queuePendingShellAction(const PendingShellActionDraft& draft, QString* errorOut = nullptr);
    QString pendingTransferScopeLabel(const DatasetSelectionContext& src, const DatasetSelectionContext& dst) const;
    void executeConnectionTransferAction(const QString& action);
    void setConnectionOriginSelection(const DatasetSelectionContext& ctx);
    void setConnectionDestinationSelection(const DatasetSelectionContext& ctx);
    bool connAdvancedDatasetActionAllowed(const DatasetSelectionContext& ctx) const;
    bool connDirectoryDatasetActionAllowed(const DatasetSelectionContext& ctx) const;
    QString connContentTokenForTree(const QTreeWidget* tree) const;
    void withConnContentContext(QTreeWidget* tree,
                                const QString& token,
                                const std::function<void()>& fn);
    bool runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs = 0, bool forceConfirmDialog = false, bool streamProgress = false);
    void actionCopySnapshot();
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
    bool showInlinePropertyNodesForTree(const QTreeWidget* tree) const;
    bool showInlinePermissionsNodesForTree(const QTreeWidget* tree) const;
    bool showPoolInfoNodeForTree(const QTreeWidget* tree) const;
    bool showInlineGsaNodeForTree(const QTreeWidget* tree) const;
    void setShowInlinePropertyNodesForTree(QTreeWidget* tree, bool visible);
    void setShowInlinePermissionsNodesForTree(QTreeWidget* tree, bool visible);
    void setShowPoolInfoNodeForTree(const QTreeWidget* tree, bool visible);
    void setShowInlineGsaNodeForTree(QTreeWidget* tree, bool visible);
    bool mountDataset(const QString& side, const DatasetSelectionContext& ctx);
    bool umountDataset(const QString& side, const DatasetSelectionContext& ctx);
    void actionCreateChildDataset(const QString& side);
    void actionCreateChildDataset(const QString& side, const DatasetSelectionContext& explicitCtx);
    void actionDeleteDatasetOrSnapshot(const QString& side);
    void actionDeleteDatasetOrSnapshot(const QString& side, const DatasetSelectionContext& explicitCtx);
    bool ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx);
    bool ensureNoMountpointConflictsBeforeMount(const DatasetSelectionContext& ctx, bool includeDescendants);
    void onDatasetPropsCellChanged(int row, int col);
    void applyDatasetPropertyChanges();
    void updateApplyPropsButtonState();
    void clearAllPendingChanges();
    bool removePendingQueuedChangeLine(const QString& line);
    // Persistencia de la lista de acciones (mainwindow_pending_store.cpp)
    void updatePendingChangesTabTitle();
    void savePendingActions();
    void loadPendingActions();
    QJsonObject pendingShellDraftToJson(const PendingShellActionDraft& draft,
                                        QString* refusalOut) const;
    bool pendingShellDraftFromJson(const QJsonObject& obj, PendingShellActionDraft* out) const;
    QJsonObject pendingCtxToJson(const DatasetSelectionContext& ctx) const;
    DatasetSelectionContext pendingCtxFromJson(const QJsonObject& obj) const;
    QString pendingConnKey(int connIdx) const;
    int pendingConnIndex(const QString& key) const;
    // Identifica el binario en curso: versión + marca del ejecutable. Ver createdByBuild.
    static QString currentBuildFingerprint();
    static QString buildFingerprintVersionPart(const QString& fingerprint);
    QVector<mwhelpers::StorableSecret> pendingStorableSecrets() const;
    QString redactStoredSecrets(const QString& command, bool* okOut) const;
    QString restoreStoredSecrets(const QString& stored) const;
    bool hasActivePendingModelWork() const;
    void deactivatePendingShellAction(const PendingShellActionDraft& draft);
    bool setPendingShellActionActive(const QString& uid, bool active);
    bool setPendingShellActionUserName(const QString& uid, const QString& name);
    bool executePendingQueuedChangeLine(const QString& line);
    bool executePendingChange(const PendingChange& change);
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
    void refreshConnectionDaemonLogAsync(int idx, bool fullReset = false);
    void runDaemonHeartbeat(const QString& connId);
    void pollDaemonZedAllConnections();
    bool launchDaemonJobTransfer(const QString& srcSnap, const QString& recvTarget,
                                 const QString& fromSnap, const QString& sendFlags,
                                 int srcConnIdx, int dstConnIdx,
                                 // Testigo de reanudación: cuando viene, el emisor
                                 // continúa donde se cortó en vez de mandarlo todo.
                                 const QString& resumeToken = QString());
    void pollDaemonJobs();
    void scanOrphanedJobsForConnection(int connIdx);
    void updateJobsListWidget();
    void onAsyncRefreshResult(int generation, int idx, const QString& connId, const ConnectionRuntimeState& state);
    void onAsyncRefreshDone(int generation);
    QString nodeStablePath(QTreeWidgetItem* item) const;
    QString userExpandedKey(QTreeWidget* tree, QTreeWidgetItem* item) const;
    void applyUserExpandedState(QTreeWidget* tree);
    void loadUserExpandedState();
    void saveUserExpandedState();
    void stopAllDaemonEventWatchers();
    int findConnectionIndexByName(const QString& name) const;
    bool isConnectionRedirectedToLocal(int idx) const;
    QString connectionPersistKey(int idx) const;
    bool isConnectionDisconnected(int idx) const;
    void setConnectionDisconnected(int idx, bool disconnected);
    void refreshConnectionByIndex(int idx);
    bool installOrUpdateDaemonForConnectionInternal(int idx, bool interactive);
    void exportPoolFromRow(int row);
    void importPoolFromRow(int row);
    void importPoolRenamingFromRow(int row);
    void scrubPoolFromRow(int row);
    void upgradePoolFromRow(int row);
    void reguidPoolFromRow(int row);
    void syncPoolFromRow(int row);
    void trimPoolFromRow(int row);
    void initializePoolFromRow(int row);
    void clearPoolFromRow(int row);
    void destroyPoolFromRow(int row);
    void showPoolHistoryFromRow(int row);
    void createPoolForSelectedConnection();
    void refreshSelectedPoolDetails(bool forceRefresh = false, bool allowRemoteLoadIfMissing = true);
    void refreshPoolStatusNow(int connIdx, const QString& poolName);
    int findPoolRow(const QString& connection, const QString& pool) const;
    int selectedPoolRowFromTabs() const;
    int selectedConnectionIndexForPoolManagement() const;
    void updatePoolManagementBoxTitle();
    void updateStatus(const QString& text);
    void beginUiBusy();
    void endUiBusy();
    void beginTransientUiBusy(const QString& statusText);
    void endTransientUiBusy();
    void updateBusyCursor();
    QString defaultStatusTextForCurrentState() const;
    void updateConnectivityMatrixButtonState();
    void setActionsLocked(bool locked);
    bool actionsLocked() const;
    void requestCancelRunningAction();
    void terminateProcessTree(qint64 rootPid);
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
    bool editInlinePropertiesDialog(const QString& title,
                                    const QString& intro,
                                    const QStringList& items,
                                    QStringList& selected,
                                    QVector<InlinePropGroupConfig>& groups,
                                    const QString& initialGroupName = QString());
    bool confirmActionExecution(const QString& actionName, const QStringList& commands, bool forceDialog = false);
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

    // Motivo único de que Copiar/Nivelar no estén disponibles con un extremo Windows: lo
    // usan el menú (para deshabilitar con explicación) y la comprobación de ejecución.
    QString streamingUnavailableReason(const QString& actionLabel) const;
    bool requireNonWindowsStreamingEndpoints(int srcConnIdx,
                                             int dstConnIdx,
                                             const QString& actionLabel);
    QString buildSshPreviewCommand(const ConnectionProfile& p, const QString& remoteCmd) const;
    QString trk(const QString& key,
                const QString& es = QString(),
                const QString& en = QString(),
                const QString& zh = QString()) const;
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
    QString dsStableId(const DSKey& key) const;
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
    bool datasetMountedFromModel(int connIdx, const QString& poolName, const QString& datasetName, QString* mountedValueOut = nullptr) const;
    bool datasetExistsInModel(int connIdx, const QString& poolName, const QString& datasetName) const;
    bool ensureObjectGuidLoaded(int connIdx, const QString& poolName, const QString& objectName, QString* guidOut = nullptr);
    QVector<DatasetPropCacheRow> datasetPropertyRowsFromModelOrCache(int connIdx, const QString& poolName, const QString& objectName) const;
    QVector<DatasetPropCacheRow> datasetPropertyRowsForNames(int connIdx,
                                                             const QString& poolName,
                                                             const QString& objectName,
                                                             const QStringList& propNames) const;
    QMap<QString, QString> datasetPropertyValuesForNames(int connIdx,
                                                         const QString& poolName,
                                                         const QString& objectName,
                                                         const QStringList& propNames) const;
    QMap<QString, QString> datasetGsaPropertyValues(int connIdx,
                                                    const QString& poolName,
                                                    const QString& objectName) const;
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

    ConnectionStore m_store;
    QVector<ConnectionProfile> m_profiles;
    QVector<ConnectionRuntimeState> m_states;
    QMap<QString, ConnInfo> m_connInfoById;

    QTableWidget* m_connectionsTable{nullptr};
    QAction* m_connectivityMatrixAction{nullptr};
    QTabWidget* m_rightTabs{nullptr};

    QWidget* m_jobsTab{nullptr};
    QWidget* m_pendingChangesTab{nullptr};
    QGroupBox* m_poolMgmtBox{nullptr};
    QAction* m_menuExitAction{nullptr};
    QLabel* m_connOriginSelectionLabel{nullptr};
    DatasetSelectionContext m_connActionOrigin;
    DatasetSelectionContext m_connActionDest;
    bool m_transferSelectionOverrideActive{false};
    DatasetSelectionContext m_transferSelectionOverrideOrigin;
    DatasetSelectionContext m_transferSelectionOverrideDest;

    QVector<PoolListEntry> m_poolListEntries;
    QWidget* m_poolDetailTabs{nullptr};
    bool m_updatingConnectionEntityTabs{false};
    QString m_lastConnectionSelectionKey;
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
    MainWindowConnectionDatasetTreeDelegate* m_topConnContentDelegate{nullptr};
    ConnectionDatasetTreeCoordinator* m_topConnContentCoordinator{nullptr};
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
    QString m_splitTreeLayoutState;
    QMap<int, QSet<QString>> m_savedBottomExpandedKeysByConn;
    QMap<int, QString> m_savedBottomSelectedKeyByConn;
    int m_forceRestoreTopStateConnIdx{-1};
    int m_forceRestoreBottomStateConnIdx{-1};
    QMap<int, QSet<QString>> m_pendingBottomExpandedKeysByConn;
    QMap<int, QString> m_pendingBottomSelectedKeyByConn;
    QString m_userSelectedConnectionKey;
    QString m_persistedTopDetailConnectionKey;
    QString m_persistedBottomDetailConnectionKey;
    int m_topDetailConnIdx{-1};
    int m_bottomDetailConnIdx{-1};
    bool m_connSelectorDefaultsInitialized{false};
    bool m_syncConnSelectorChecks{false};
    bool m_rebuildingTopConnContentTree{false};
    bool m_rebuildingBottomConnContentTree{false};
    ConnectionDatasetTreeWidget* m_bottomDatasetTreeWidget{nullptr};
    ConnectionDatasetTreePane* m_bottomDatasetPane{nullptr};
    MainWindowConnectionDatasetTreeDelegate* m_bottomConnContentDelegate{nullptr};
    ConnectionDatasetTreeCoordinator* m_bottomConnContentCoordinator{nullptr};
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
    QPushButton* m_btnAdvancedBreakdown{nullptr};
    QPushButton* m_btnAdvancedAssemble{nullptr};
    QPushButton* m_btnAdvancedFromDir{nullptr};
    QPushButton* m_btnAdvancedToDir{nullptr};

    QTextEdit* m_statusText{nullptr};
    QTextEdit* m_lastDetailText{nullptr};
    QTabWidget* m_logsTabs{nullptr};
    QPlainTextEdit* m_logView{nullptr};
    QListWidget* m_pendingChangesList{nullptr};
    QMap<QString, PendingItemStatus> m_pendingItemStatus;
    QStringList m_pendingOrderedDisplayLines;
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
    QListWidget* m_jobsListWidget{nullptr};
    int m_pendingSpinnerFrame{0};
    bool m_pendingApplyInProgress{false};
    bool m_pendingApplyFinishSuppressed{false};
    struct SplitTreeEntry {
        int connIdx{-1};
        QString poolName;
        QString rootDataset;
        QString displayRoot;
        ConnectionDatasetTreeWidget* treeWidget{nullptr};
        MainWindowConnectionDatasetTreeDelegate* delegate{nullptr};
    };
    QList<SplitTreeEntry> m_splitTrees;
    QSplitter* m_connContentTreeSplitter{nullptr};
    QMap<QString, QPointer<QPlainTextEdit>> m_connectionLogViews;
    QMap<QString, QPointer<QPlainTextEdit>> m_connectionGsaLogViews;
    QMap<QString, QPointer<QWidget>> m_connectionLogTabs;
    struct ConnCompactState {
        bool valid{false};
        QString date, time, conn, level;
    };
    QMap<QString, ConnCompactState> m_connCompactState;
    QMap<QString, ConnCompactState> m_connGsaCompactState;
    QMap<QString, qint64> m_connectionDaemonLogOffset;
    QSet<QString> m_sshDisableMultiplexKeys;
    QSet<QString> m_loggedSshResolutionKeys;
    QSet<QString> m_daemonBootstrapPromptedConnIds;
    QMap<QString, QDateTime> m_daemonRpcRetryAfterByConnKey;
    QMap<QString, QString> m_daemonRpcRetryReasonByConnKey;
    struct RemoteRpcTunnelState {
        QPointer<QProcess> process;
        quint16 localPort{0};
        quint16 remotePort{0};
        QDateTime startedAtUtc;
        QDateTime lastUsedUtc;
    };
    QMap<QString, RemoteRpcTunnelState> m_remoteDaemonRpcTunnelsByConnKey;
    // Claves cuyo túnel se está montando ahora mismo. Protege de la reentrancia que
    // provoca el processEvents de la espera: sin esto se montaban túneles duplicados que
    // quedaban huérfanos fuera del mapa. Bajo m_sshRuntimeSetsMutex, como el mapa.
    QSet<QString> m_remoteRpcTunnelsBeingCreated;
    mutable QMutex m_sshRuntimeSetsMutex;
    QMap<QString, PoolDatasetCache> m_poolDatasetCache;
    QMap<QString, DatasetPermissionsCacheEntry> m_datasetPermissionsCache;
    QMap<QString, PoolDetailsCacheEntry> m_poolDetailsCache;
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
    QVector<PendingChange> m_pendingChangesModel;
    mutable QMap<QString, int> m_pendingChangeOrderByStableId;
    mutable int m_nextPendingChangeOrder{0};
    bool m_loadingPropsTable{false};
    bool m_loadingDatasetTrees{false};
    QString m_language{QStringLiteral("es")};
    bool m_actionConfirmEnabled{true};
    int m_logMaxSizeMb{10};
    QString m_logLevelSetting{QStringLiteral("normal")};
    int m_logMaxLinesSetting{500};
    bool m_showInlineDatasetProps{true};
    bool m_showInlinePropertyNodesTop{true};
    bool m_showInlinePropertyNodesBottom{true};
    bool m_showInlinePermissionsNodesTop{true};
    bool m_showInlinePermissionsNodesBottom{true};
    bool m_showInlineGsaNodeTop{true};
    bool m_showInlineGsaNodeBottom{true};
    bool m_showPoolInfoNodeTop{true};
    bool m_showPoolInfoNodeBottom{true};
    bool m_showAutomaticGsaSnapshots{true};
    int m_connPropColumnsSetting{7};
    bool m_pendingChangeActivationInProgress{false};
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
    bool m_connectivityMatrixInProgress{false};
    bool m_closing{false};
    QStringList m_transientStatusStack;
    bool m_cancelActionRequested{false};
    // La última acción terminó porque el usuario la detuvo, no por un fallo. Sin esto,
    // el bucle que aplica los cambios pendientes no distingue una cosa de la otra y
    // saca "Error ejecutando cambio pendiente" tras una cancelación pedida a propósito.
    bool m_lastActionWasCancelled{false};
    // Semilla para re-editar un cambio ya encolado: se quita de la lista, se guarda aquí
    // lo que se le pidió, y se vuelve a abrir su diálogo con eso ya puesto. Las tres
    // acciones que la usan guardan su entrada completa en datasetActionArgv, así que no
    // hace falta almacenar nada más; "Desde Dir" no, porque su orden es una tubería de
    // shell y su diálogo tiene mucho más estado del que cabe en la orden.
    struct PendingEditSeed {
        bool active{false};
        QStringList argv;
        FromDirInput fromDir;   // solo para Desde Dir, que no se reconstruye de argv
    };
    PendingEditSeed m_pendingEditSeed;
    QProcess* m_activeLocalProcess{nullptr};
    qint64 m_activeLocalPid{-1};
    bool m_busyOnImportRefresh{false};
    QPushButton* m_activeConnActionBtn{nullptr};
    QAction* m_confirmActionsMenuAction{nullptr};
    QString m_activeConnActionName;
    bool m_syncingConnContentColumns{false};
    QSet<QString> m_poolDetailsLoadsInFlight;
    QSet<QString> m_poolAutoSnapshotLoadsInFlight;
    QMap<int, int> m_poolAutoSnapshotPendingLoadsByConn;
    QMap<int, QSet<QString>> m_poolAutoSnapshotDirtyPoolsByConn;
    QSet<int> m_poolAutoSnapshotUiDeferByConn;
    QSet<QString> m_pendingConnContentPoolReloadKeys;
    QSet<int> m_pendingConnectionRefreshIndices;
    bool m_reloadFlushScheduled{false};
};
