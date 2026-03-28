# Diseño técnico: modelo `ConnInfo` / `PoolInfo` / `DSInfo` y estado de treeviews

## Objetivo

Rediseñar la gestión de:

- datos remotos de conexiones, pools y datasets
- cachés en memoria
- estado de edición
- cambios pendientes
- estado visual de los treeviews

La intención es pasar de una arquitectura basada en mapas paralelos y claves string repartidas por `MainWindow` a un modelo explícito, con propietarios claros y refresco controlado.

## Problema actual

Hoy la información está fragmentada entre varias estructuras independientes:

- `m_states`
- `m_poolDatasetCache`
- `m_poolDetailsCache`
- `m_datasetPropsCache`
- `m_datasetPermissionsCache`
- `m_connContentPropValuesByObject`
- estados visuales del árbol
- borradores de edición separados
- cambios pendientes generados desde varias rutas distintas

Consecuencias:

- no hay una fuente de verdad única por conexión/pool/dataset
- la invalidación depende de prefijos string frágiles
- un mismo dataset puede estar representado en varias cachés con distinta frescura
- el treeview conserva demasiado estado de negocio
- los drafts de edición están demasiado acoplados a la UI actual
- refrescar un árbol no garantiza refrescar todas las vistas del mismo objeto

## Principios de diseño

El rediseño debe separar cinco planos:

1. identidad de dominio
2. estado remoto cargado
3. capacidades del objeto
4. estado de edición y cambios pendientes
5. estado visual de cada treeview

Regla básica:

- el treeview no es la fuente de verdad del dominio
- el treeview solo proyecta dominio + edición + estado visual

## Estado actual del componente de treeview

Además del modelo `ConnInfo / PoolInfo / DSInfo`, el desarrollo ya ha cristalizado en un componente reusable para los treeviews de conexión.

Ese componente actual es:

- `ConnectionDatasetTreeWidget(Config, DomainAdapter, parent)`

Internamente encapsula:

- `ConnectionDatasetTreePane`
- `ConnectionDatasetTreeController`
- `ConnectionDatasetTreeCoordinator`

Y hoy sus dos instancias reales son:

- treeview superior (`Origen`)
- treeview inferior (`Destino`)

La parte importante para este documento es:

- el estado visual ya no debería pensarse como algo incrustado en `MainWindow`
- el tree reusable es una pieza separada, aunque todavía usa un adaptador de dominio acoplado a `MainWindow`

Conclusión de coherencia con el estado real:

- el modelo de dominio y el componente de treeview han avanzado en paralelo
- el componente reusable existe ya
- la independencia total de `MainWindow` todavía no está completada

## Cambios aceptados sobre el diseño inicial

Quedan aceptados estos ajustes respecto a la primera propuesta:

- `PoolInfoUI` se sustituye por un `TreeViewState` por vista completa
- las capacidades del objeto pasan a un bloque explícito (`DSCapabilities`)
- todo bloque remoto cargable usa ciclo de vida común (`LoadState`)
- los cambios pendientes se modelan explícitamente (`PendingChange`)
- el estado remoto de `DSInfo` y el estado editable se separan mediante `DSEditSession`

Con esto se evita que el dominio quede mezclado con los widgets y con los borradores de edición.

## Modelo objetivo

## 1. Identidad tipada

Antes de hablar de clases, conviene fijar identidades explícitas.

No se recomienda seguir propagando `QString` libres como clave lógica principal.

### `ConnKey`

Identifica una conexión.

```cpp
struct ConnKey {
    QString connectionId;
};
```

### `PoolKey`

Identifica un pool dentro de una conexión.

```cpp
struct PoolKey {
    ConnKey conn;
    QString poolGuid;
    QString poolName;
};
```

Regla:

- si existe `guid`, es la identidad fuerte
- `poolName` se usa para visualización y fallback

### `DSKey`

Identifica un objeto ZFS concreto.

```cpp
struct DSKey {
    PoolKey pool;
    QString fullName;
};
```

Ejemplos:

- dataset: `t7pool/users`
- snapshot: `t7pool/users@autosnap-001`

Regla importante:

- no normalizar a minúsculas
- el nombre completo es case-sensitive

## 2. `ConnInfo`

Representa una conexión concreta y es el nodo raíz del modelo de dominio.

Responsabilidades:

- identidad y perfil de conexión
- estado runtime de conectividad
- catálogo de pools de esa conexión
- coordinación de refresco por conexión

Relación:

- una `ConnInfo` posee muchas `PoolInfo`
- una `PoolInfo` pertenece a una sola `ConnInfo`

Interfaz conceptual:

```cpp
class ConnInfo final {
public:
    const ConnKey& key() const;
    const ConnectionProfile& profile() const;

    ConnectionRuntimeInfo& runtime();
    const ConnectionRuntimeInfo& runtime() const;

    PoolInfo* findPool(const PoolKey& key);
    PoolInfo* findPoolByName(const QString& poolName);
    PoolInfo* findPoolByGuid(const QString& poolGuid);
    QList<PoolInfo*> pools() const;

    PoolInfo& ensurePool(const PoolKey& key);
    void clearPools();
};
```

## 3. `PoolInfo`

Representa un pool concreto dentro de una conexión.

Responsabilidades:

- identidad del pool
- estado de importación
- runtime de pool
- datasets del pool
- lookup rápido de datasets

Relación:

- pertenece a una `ConnInfo`
- posee muchos `DSInfo`
- cada `DSInfo` pertenece a un solo `PoolInfo`

Interfaz conceptual:

```cpp
class PoolInfo final {
public:
    const PoolKey& key() const;
    ConnInfo& ownerConnection();

    PoolRuntimeInfo& runtime();
    const PoolRuntimeInfo& runtime() const;

    DSInfo* findObject(const DSKey& key);
    DSInfo* findObjectByFullName(const QString& fullName);
    QList<DSInfo*> rootObjects() const;
    QList<DSInfo*> allObjects() const;

    DSInfo& ensureObject(const DSKey& key, DSKind kind);
    void rebuildHierarchy();
    void clearObjects();
};
```

## 4. `DSInfo`

Representa un objeto ZFS individual:

- filesystem
- volume
- snapshot

Recomendación:

- snapshots también deben ser `DSInfo`

Motivo:

- unifica identidad
- simplifica navegación y refresco
- evita rutas especiales para selección y cambios pendientes

Responsabilidades:

- identidad del objeto
- tipo y jerarquía
- estado remoto cargado
- capacidades del objeto
- acceso a sesiones de edición ligadas a ese objeto

Interfaz conceptual:

```cpp
class DSInfo final {
public:
    const DSKey& key() const;
    PoolInfo& ownerPool();
    DSInfo* parent() const;
    QList<DSInfo*> children() const;

    DSKind kind() const;
    QString fullName() const;
    QString datasetName() const;
    QString snapshotName() const;
    QString leafName() const;

    DSRuntimeInfo& runtime();
    const DSRuntimeInfo& runtime() const;

    DSCapabilities& capabilities();
    const DSCapabilities& capabilities() const;
};
```

## Estado remoto cargado

## 1. Estado de carga común

Todo bloque cargable debe llevar un pequeño ciclo de vida explícito.

```cpp
enum class LoadState {
    NotLoaded,
    Loading,
    Loaded,
    Stale,
    Error
};
```

Cada bloque de datos cargables debe incluir:

- `LoadState state`
- `QString errorText`
- `QDateTime loadedAt`

Esto aplica a:

- runtime de conexión
- runtime de pool
- propiedades de dataset
- permisos
- programación de snapshots

## 2. `ConnectionRuntimeInfo`

Agrupa todo lo que hoy vive en `ConnectionRuntimeState`.

Debe incluir:

- `status`
- `detail`
- `osLine`
- `machineUuid`
- `connectionMethod`
- `zfsVersion`
- `zfsVersionFull`
- comandos detectados y faltantes
- datos de helper install
- datos GSA
- pools importados e importables

## 3. `PoolRuntimeInfo`

Debe agrupar:

- estado importado/importable
- `zpool status -v`
- propiedades de `zpool get`
- `reason`, `state`, `action`

```cpp
struct PoolRuntimeInfo {
    LoadState detailsState{LoadState::NotLoaded};
    QString errorText;
    QDateTime loadedAt;

    QString poolStatusText;
    QMap<QString, QString> zpoolProperties;

    bool imported{false};
    bool importable{false};
    QString importState;
    QString importReason;
    QString importAction;
};
```

## 4. `DSRuntimeInfo`

Contiene solo datos remotos del dataset.

Ejemplos:

- `used`
- `referenced`
- `compressRatio`
- `mountpoint`
- `mounted`
- `canmount`
- `driveletter`
- `encryption`
- `keystatus`
- holds
- hijos directos

## Capacidades

El modelo debe separar claramente los datos cargados de lo que puede hacerse sobre ellos.

No conviene que las reglas de edición sigan dispersas por la UI.

```cpp
struct DSPropertyCapability {
    bool visible{true};
    bool editableInline{false};
    bool editableBySet{false};
    bool editableBySpecialAction{false};
    QString specialActionId;
    bool inheritable{false};
};

struct DSCapabilities {
    bool canMount{false};
    bool canUnmount{false};
    bool canDestroy{false};
    bool canRename{false};
    bool canClone{false};
    bool canManagePermissions{false};
    bool canManageSchedules{false};

    QMap<QString, DSPropertyCapability> propertyCaps;
};
```

Ejemplos:

- `compression`: `editableInline=true`, `editableBySet=true`
- `encryption`: `editableInline=false`, `editableBySet=false`
- `keyformat`: `editableInline=false`, `editableBySpecialAction=true`

## Estado de edición

## 1. `DSEditSession`

No se recomienda mezclar directamente el estado remoto de `DSInfo` con los borradores.

Mejor separar:

- `DSInfo`: dominio cargado
- `DSEditSession`: edición pendiente del objeto

Una `DSEditSession` pertenece a un único `DSInfo`.

```cpp
class DSEditSession final {
public:
    const DSKey& target() const;
    bool dirty() const;
    void clear();

    DSPropertyEditState& propertyEdits();
    DSPermissionsEditState& permissionsEdits();
    DSScheduleEditState& scheduleEdits();
};
```

## 2. Edición de propiedades

```cpp
struct DSPropertyEditValue {
    QString value;
    bool inherit{false};
    bool dirty{false};
};

struct DSPropertyEditState {
    QMap<QString, DSPropertyEditValue> byName;
};
```

## 3. Edición de permisos

```cpp
struct DSPermissionsEditState {
    bool dirty{false};
    QList<DSPermissionGrant> grants;
    QSet<QString> createPermissions;
    QMap<QString, DSPermissionSet> sets;
};
```

## 4. Edición de schedules

```cpp
struct DSScheduleEditState {
    bool dirty{false};
    QList<DSSnapshotScheduleRule> rules;
};
```

## Cambios pendientes

Los cambios pendientes no deben salir solo de `DSEditSession`.

Hay acciones que no son "editar propiedades del dataset":

- mover
- renombrar
- copiar
- nivelar
- sincronizar
- acciones shell
- import/export de pool

Por eso se propone un modelo específico de cambios pendientes.

## 1. `PendingChange`

```cpp
enum class PendingChangeKind {
    DatasetProperties,
    DatasetPermissions,
    DatasetSchedule,
    DatasetRename,
    DatasetMove,
    PoolImport,
    PoolExport,
    ShellAction,
    TransferAction
};

struct PendingChangeTarget {
    std::optional<ConnKey> conn;
    std::optional<PoolKey> pool;
    std::optional<DSKey> object;
};

struct PendingChange {
    QString id;
    PendingChangeKind kind;
    PendingChangeTarget target;
    QString displayText;
    QString previewText;
    QString executionKey;
};
```

## 2. `PendingChangeBuilder`

Debe construir `PendingChange` desde:

- `DSEditSession`
- acciones directas del usuario

## 3. `PendingChangeExecutor`

Debe ejecutar los cambios pendientes y devolver:

- éxito/error
- objetos afectados
- necesidad de invalidación de cachés

## Estado visual del treeview

## 1. Sustituir `PoolInfoUI` por `TreeViewState`

En vez de crear muchos objetos `PoolInfoUI`, es preferible un estado visual por treeview completo.

Ventajas:

- menos objetos
- relación más simple con el widget
- captura/restauración más natural
- fácil soportar varios pools por vista

Cada treeview tiene un `viewId`:

- `origin`
- `dest`
- `conncontent-top`
- `conncontent-bottom`

## 2. `TreeViewState`

```cpp
class TreeViewState final {
public:
    QString viewId() const;

    TreeVisualOptions& options();
    const TreeVisualOptions& options() const;

    TreeSelectionState& selection();
    const TreeSelectionState& selection() const;

    QMap<QString, PoolVisualState>& pools();
    const QMap<QString, PoolVisualState>& pools() const;
};
```

## 3. `TreeVisualOptions`

```cpp
struct TreeVisualOptions {
    bool showPoolInfo{true};
    bool showInlineProperties{true};
    bool showInlinePermissions{true};
    bool showInlineSchedules{true};
    bool showAutomaticSnapshots{true};
    int propertyColumns{4};
};
```

## 4. `TreeSelectionState`

```cpp
struct TreeSelectionState {
    QString selectedObjectKey;
    QByteArray headerState;
    int verticalScroll{0};
    int horizontalScroll{0};
};
```

## 5. `PoolVisualState`

```cpp
struct PoolVisualState {
    bool poolRootExpanded{true};
    bool poolInfoExpanded{false};
    QSet<QString> expandedObjectKeys;
    QMap<QString, QString> selectedSnapshotByDataset;
    QMap<QString, QStringList> expandedChildNodesByObject;
};
```

Regla:

- el estado visual vive en `TreeViewState`
- el dominio vive en `ConnInfo` / `PoolInfo` / `DSInfo`
- la UI solo sincroniza ambos planos

## Proyección del árbol

## `TreeProjectionBuilder`

Debe construir una proyección renderizable a partir de:

- `ConnInfo`
- `PoolInfo`
- `DSInfo`
- `DSEditSession`
- `TreeViewState`

No debe:

- ejecutar SSH
- mutar estado remoto
- decidir reglas de negocio remotas

Sí debe:

- decidir qué nodos mostrar
- decidir qué valor pintar
- pintar drafts en lugar de valores remotos cuando proceda
- aplicar expansión/selección/combos desde `TreeViewState`

## Servicios recomendados

## 1. `ConnectionRefreshService`

Responsable de refrescar `ConnInfo`.

Debe:

- validar conexión
- detectar SO
- detectar OpenZFS
- detectar comandos auxiliares
- cargar pools importados/importables

## 2. `PoolRefreshService`

Responsable de refrescar `PoolInfo`.

Debe:

- cargar `zpool status -v`
- cargar `zpool get`
- recargar datasets del pool

## 3. `DatasetRefreshService`

Responsable de refrescar `DSInfo`.

Debe:

- cargar `zfs get`
- cargar permisos
- cargar programación de snapshots
- cargar holds y metadatos asociados

## 4. `EditSessionService`

Responsable de:

- crear/recuperar `DSEditSession`
- limpiar sesiones inválidas
- aplicar drafts sobre el modelo proyectado

## 5. `PendingChangeBuilder`

Responsable de transformar sesiones y acciones en cola en `PendingChange`.

## 6. `PendingChangeExecutor`

Responsable de:

- ejecutar cambios
- invalidar `ConnInfo`/`PoolInfo`/`DSInfo` afectados
- marcar como `stale` lo que deba recargarse

## Reglas de actualización

## 1. Cambio remoto

Si una acción modifica un dataset:

- se invalida `DSInfo` o `PoolInfo`
- se marca `LoadState::Stale`
- se refrescan todas las vistas que muestran ese objeto

## 2. Cambio visual

Si el usuario expande, colapsa o selecciona:

- solo cambia `TreeViewState`

## 3. Cambio en borrador

Si el usuario edita una propiedad inline:

- cambia `DSEditSession`
- no cambia `DSInfo::runtime`
- todas las vistas que muestren ese dataset deben poder reflejar el draft

## Estrategia de migración

La migración debe ser incremental.

## Fase 1. Claves tipadas e identidad

Introducir:

- `ConnKey`
- `PoolKey`
- `DSKey`

Sin tocar aún la UI.

## Fase 2. Introducir `ConnInfo` / `PoolInfo` / `DSInfo`

Primero solo para estado remoto cargado.

No mover todavía drafts ni estado visual.

## Fase 3. Mover datasets y snapshots al modelo

Sustituir progresivamente:

- `m_poolDatasetCache`

por:

- `ConnInfo -> PoolInfo -> DSInfo`

## Fase 4. Mover propiedades, permisos y schedules

Sustituir progresivamente:

- `m_datasetPropsCache`
- `m_datasetPermissionsCache`

por bloques internos de estado cargado en `DSInfo`.

## Fase 5. Introducir `DSEditSession`

Mover borradores de:

- propiedades
- permisos
- programación

a sesiones ligadas a `DSKey`.

## Fase 6. Introducir `PendingChange`

Unificar la lista de cambios pendientes en un modelo común.

## Fase 7. Introducir `TreeViewState`

Sacar definitivamente el estado visual de los widgets y agruparlo por vista.

## Fase 8. Render declarativo

Hacer que `populateDatasetTree()` y las tablas de propiedades lean del modelo nuevo en vez de reconstruir a partir de mapas paralelos.

## Decisiones explícitas adoptadas

- snapshots se modelan como `DSInfo`
- `guid` del pool es identidad fuerte cuando existe
- nombres de datasets no se normalizan a minúsculas
- capacidades se separan de datos cargados
- estado remoto y estado de edición no se mezclan
- cambios pendientes tienen modelo propio
- el estado visual vive en `TreeViewState`, no en `PoolInfoUI`

## Beneficios esperados

Con esta arquitectura:

- cada conexión, pool y dataset tiene un único propietario claro
- el refresco deja de depender de invalidaciones string dispersas
- los drafts de edición dejan de vivir en mapas globales frágiles
- varias vistas pueden mostrar el mismo dataset sin duplicar dominio
- el árbol se vuelve una proyección consistente del modelo
- importes ambiguos por nombre de pool se resuelven naturalmente con `guid`

## Resultado práctico

El orden recomendado para implementar es:

1. identidad tipada
2. dominio cargado
3. datasets y snapshots
4. propiedades/permisos/schedules
5. edición
6. cambios pendientes
7. estado visual
8. renderer declarativo

Ese orden minimiza riesgo y permite introducir el nuevo modelo sin rehacer toda la UI de golpe.
