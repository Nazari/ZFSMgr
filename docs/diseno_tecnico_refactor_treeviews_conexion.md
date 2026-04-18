# Diseño técnico: refactorización de treeviews de conexión

## Objetivo

Reducir la duplicación entre árboles de conexión y converger hacia una sola superficie de navegación reusable.

## Estado actual implementado

La fase de árbol global único ya está aplicada en la UI actual.

Ahora mismo la interfaz usa:

- un único árbol global visible
- conexiones como nodos raíz
- pools bajo cada conexión
- datasets y snapshots bajo cada pool
- selección lógica `Origen/Destino` desde el menú contextual del árbol

Ya no forman parte de la UI activa:

- la tabla de conexiones
- el treeview superior separado
- el treeview inferior separado
- el botón flotante `Connectivity`

La funcionalidad de conectividad vive ahora en el menú principal como:

- `Comprobar conectividad`

## Objetivo funcional del rediseño aplicado

El rediseño global persigue y ya materializa:

- dejar un solo árbol visible en la zona central de navegación
- usar conexiones como nodos raíz del árbol
- hacer desaparecer la tabla de conexiones como control independiente
- seleccionar `Origen` y `Destino` desde el propio menú contextual del árbol
- integrar esa lógica en el componente reusable del árbol

## Estructura objetivo del árbol

El árbol único debe quedar estructurado así:

- raíz de nivel 1: conexiones
- bajo cada conexión: pools
- bajo cada pool: datasets, snapshots y nodos inline

Consecuencias de esta decisión:

- los pools ya no necesitan mostrarse con el prefijo `Conexion::Pool`
- el nombre visible del pool puede ser solo el `poolName`
- la identidad completa sigue existiendo en datos internos (`connIdx`, `poolName`, `DSKey`, etc.), pero no en el texto visible del nodo

## Sustitución de la tabla de conexiones

La tabla de conexiones ya ha desaparecido de la UI activa.

Su funcionalidad se redistribuye así:

- la navegación por conexión pasa al árbol único
- el menú contextual que hoy se abre sobre filas de la tabla debe abrirse sobre los nodos raíz de conexión del nuevo árbol
- los colores visuales de la tabla actual deben trasladarse a las filas raíz de conexión en el árbol
- la marca `(*)` asociada al nombre de conexión debe seguir existiendo, pero aplicada al texto del nodo raíz de conexión

## Selección de origen y destino

La selección de `Origen` y `Destino` ya no depende de checks en ninguna tabla.

La implementación actual usa:

- en el menú contextual de dataset dos acciones:
  - `Seleccionar como origen`
  - `Seleccionar como destino`
- esas acciones rellenan las labels de la caja `Selected datasets`
- la caja `Selected datasets` permanece en la UI
- la semántica de `Origen` y `Destino` sigue existiendo, pero su selección se hace desde el árbol único

Esto implica que el árbol reusable debe poder emitir intención de selección lógica, no solo selección visual del item actual.

## Reubicación del árbol en la UI

El árbol único ocupa el espacio central principal de navegación.

La disposición actual relevante es:

- columna izquierda:
  - `Selected datasets`
  - `Status and progress`
- columna derecha:
  - árbol único
  - `Pending changes` debajo del árbol
- zona inferior:
  - logs

## Reglas de comportamiento

### Desconexión

Si una conexión se marca como desconectada:

- la conexión sigue visible como nodo raíz
- desaparecen del árbol sus pools
- y por tanto también sus datasets y snapshots

### Menús contextuales

Debe haber al menos dos familias claras de menús contextuales:

- menú de conexión sobre nodo raíz de conexión
- menú de pool/dataset/snapshot sobre los nodos ya existentes del árbol reusable

La funcionalidad que hoy cuelga de la tabla de conexiones debe migrar al nodo de conexión.

### Selección lógica frente a selección visual

El árbol único debe distinguir entre:

- item actualmente seleccionado en la UI
- dataset marcado como `Origen`
- dataset marcado como `Destino`

No deben confundirse esos tres conceptos.

## Integración requerida en el componente reusable

La propuesta exige que el cambio no se resuelva solo a base de lógica extra en `MainWindow`.

Debe quedar integrado en el componente reusable del árbol, al menos en estos planos:

- configuración del árbol con conexiones como raíz
- representación visual de estados de conexión
- soporte de acciones contextuales de conexión
- soporte de selección lógica `Origen/Destino`
- preservación del estado visual en un árbol mixto conexión/pool/dataset

Eso implica evolucionar el `Config` y el `DomainAdapter` actuales.

## Cambios previstos sobre `ConnectionDatasetTreeWidget`

El wrapper reusable actual:

```cpp
ConnectionDatasetTreeWidget(Config, DomainAdapter, parent)
```

deberá evolucionar para soportar explícitamente:

- modo de raíz por conexión
- acciones contextuales de conexión
- resaltado/estilo de fila de conexión
- selección lógica `Origen/Destino`
- árbol único en vez de dos instancias acopladas

No se fija todavía la forma exacta del API nueva, pero sí el sentido del cambio:

- la selección lógica no debe seguir siendo un detalle externo a la vista
- el árbol reusable debe ser la pieza central de navegación del módulo

## Migración funcional prevista

La migración propuesta debe seguir este orden lógico:

1. permitir que el árbol reusable renderice conexiones como raíz
2. mover el menú contextual de conexión desde la tabla a esos nodos raíz
3. introducir `Seleccionar como origen` y `Seleccionar como destino`
4. trasladar color y marca `(*)` de la tabla a filas de conexión
5. eliminar la tabla de conexiones
6. eliminar el árbol duplicado superior/inferior y dejar una sola instancia

## Conectividad

El botón flotante `Connectivity` asociado a la tabla de conexiones debe eliminarse.

Su funcionalidad se traslada al menú principal de la aplicación:

- debajo de `Logs`
- con el nombre `Comprobar conectividad`

Esto deja de depender de la existencia física de la tabla de conexiones.

## Estado actual

La refactorización descrita en este documento ya no está solo en fase de propuesta. Ya existe una cadena reusable real:

- `ConnectionDatasetTreeWidget`
- `ConnectionDatasetTreePane`
- `ConnectionDatasetTreeController`
- `ConnectionDatasetTreeCoordinator`
- un adaptador de dominio actual: `MainWindowConnectionDatasetTreeDelegate`

Estado implementado a fecha de este documento:

- el árbol visible se crea mediante `ConnectionDatasetTreeWidget(Config, DomainAdapter, parent)`
- `ConnectionDatasetTreePane` encapsula el `QTreeWidget`
- `ConnectionDatasetTreeController` centraliza el cableado de señales del pane
- `ConnectionDatasetTreeCoordinator` coordina el flujo de interacción del árbol
- el `DomainAdapter` actual está implementado por `MainWindowConnectionDatasetTreeDelegate`
- el árbol está en modo unificado, con conexiones como raíz
- las opciones `Mostrar en línea` se aplican al árbol único visible

Lo que sigue pendiente:

- reducir todavía más la dependencia funcional de `MainWindow`
- seguir desacoplando más lógica de refresco/render/selección del `MainWindow`

## Problema actual

Actualmente existen dos controles y dos rutas de interacción muy parecidas para:

- árbol superior (`Origen`)
- árbol inferior (`Destino`)

Ambos comparten gran parte de:

- configuración visual del `QTreeWidget`
- restauración de estado visual
- menús contextuales muy similares
- opciones `Mostrar en línea`
- comportamiento de expansión/colapso
- render de pools, datasets, snapshots y nodos inline

Esto está generando:

- comportamiento distinto entre ambos árboles
- textos divergentes
- bugs asimétricos
- más coste de mantenimiento

## Principio de diseño

La refactorización debe ser incremental.

En la primera fase:

- se extrae la capa de presentación e interacción local del árbol
- se conserva en `MainWindow` la lógica de dominio y de ejecución
- no se intenta mover todavía la lógica ZFS/SSH ni las cachés

## Componente objetivo

### Nombre

`ConnectionDatasetTreeWidget`

### Naturaleza

`QWidget` reusable construido a partir de:

- `Config`
- `DomainAdapter`
- `parent`

Internamente compone:

- `ConnectionDatasetTreePane`
- `ConnectionDatasetTreeCoordinator`
- `ConnectionDatasetTreeController`

### Instancias actuales

- una instancia para el panel superior
- una instancia para el panel inferior
- el diseño ya permite una tercera instancia sin volver a cablear `pane + controller + coordinator` manualmente

## Responsabilidades del nuevo componente

`ConnectionDatasetTreeWidget` debe encargarse de:

- crear el pane interno
- aplicarle un `Config`
- conectarlo a un `DomainAdapter`
- exponer el `QTreeWidget` y el `Coordinator` para compatibilidad incremental

`ConnectionDatasetTreePane` debe encargarse de:

- crear y poseer el `QTreeWidget`
- aplicar la configuración visual común del árbol
- gestionar el menú contextual del header
- almacenar su estado visual local
- almacenar sus flags visuales propios
- emitir señales con intención de interacción

No debe encargarse de:

- ejecutar comandos ZFS
- acceder a SSH
- leer o invalidar cachés globales
- aplicar reglas de negocio complejas
- decidir operaciones sobre `Origen` o `Destino`

## Estado propio del widget

Cada instancia de `ConnectionDatasetTreeWidget` mantiene:

- `Config`
- `ConnectionDatasetTreePane`
- `ConnectionDatasetTreeCoordinator`

## Estado propio del pane

Cada instancia debe mantener al menos:

- rol del pane: `Top` o `Bottom`
- flags `Mostrar en línea` propios:
  - propiedades
  - permisos
  - programar snapshots
- referencia al `QTreeWidget`
- configuración del encabezado
- snapshot de estado visual:
  - expansión
  - scroll vertical
  - scroll horizontal
  - selección

## Interfaz pública aplicada

La interfaz efectiva del wrapper reusable es:

```cpp
class ConnectionDatasetTreeWidget final : public QWidget {
    Q_OBJECT
public:
    using DomainAdapter = ConnectionDatasetTreeDomainAdapter;

    struct Config {
        QString treeName;
        QString primaryColumnTitle;
        ConnectionDatasetTreePane::Role role;
        ConnectionDatasetTreePane::VisualOptions visualOptions;
    };

    explicit ConnectionDatasetTreeWidget(const Config& config,
                                         DomainAdapter* adapter,
                                         QWidget* parent = nullptr);

    const Config& config() const;
    QTreeWidget* tree() const;
    ConnectionDatasetTreePane* pane() const;
    ConnectionDatasetTreeCoordinator* coordinator() const;
};
```

El pane interno mantiene una interfaz muy parecida a la prevista originalmente:

```cpp
class ConnectionDatasetTreePane final : public QWidget {
    Q_OBJECT
public:
    enum class Role {
        Top,
        Bottom
    };

    struct VisualOptions {
        bool showInlineProperties{true};
        bool showInlinePermissions{true};
        bool showInlineGsa{true};
    };

    struct VisualState {
        QByteArray headerState;
        int verticalScroll{0};
        int horizontalScroll{0};
        QString currentNodeKey;
        QSet<QString> expandedNodeKeys;
    };

    explicit ConnectionDatasetTreePane(Role role, QWidget* parent = nullptr);

    Role role() const;
    QTreeWidget* tree() const;

    void setPrimaryColumnTitle(const QString& text);
    void setVisualOptions(const VisualOptions& options);
    VisualOptions visualOptions() const;

    VisualState captureVisualState() const;
    void restoreVisualState(const VisualState& state);

signals:
    void itemClicked(QTreeWidgetItem* item, int column);
    void itemChanged(QTreeWidgetItem* item, int column);
    void itemExpanded(QTreeWidgetItem* item);
    void itemCollapsed(QTreeWidgetItem* item);
    void selectionChanged();
    void contextMenuRequested(const QPoint& pos, QTreeWidgetItem* item);
    void headerContextMenuRequested(const QPoint& pos, int logicalColumn);
};
```

## Relación con `MainWindow`

`MainWindow` seguirá siendo propietario de:

- caches de pools y datasets
- caches de propiedades
- caches de permisos
- borradores (`drafts`)
- selección lógica `Origen/Destino`
- acciones remotas/locales
- refrescos de conexión
- reglas de negocio

`MainWindow` usa hoy el componente reusable así:

- `m_topDatasetTreeWidget`
- `m_bottomDatasetTreeWidget`

Y mantiene por compatibilidad referencias derivadas:

- `m_topDatasetPane`
- `m_bottomDatasetPane`
- `m_connContentTree`
- `m_bottomConnContentTree`
- `m_topConnContentCoordinator`
- `m_bottomConnContentCoordinator`

El acoplamiento pendiente está aquí:

- el `DomainAdapter` actual es `MainWindowConnectionDatasetTreeDelegate`
- ese adapter sigue delegando mucha lógica en `MainWindow`
- varias rutas comunes de refresco/render siguen dependiendo de helpers globales de `MainWindow`

Es decir:

- la construcción del componente ya está encapsulada
- la lógica de dominio todavía no está totalmente desacoplada

## Cambios previstos por fases

### Fase 1: extracción del widget contenedor

#### Alcance

Mover a `ConnectionDatasetTreePane`:

- construcción del `QTreeWidget`
- configuración base del árbol:
  - columnas
  - resize mode
  - `setRootIsDecorated(true)`
  - `setItemsExpandable(true)`
  - scroll suave
  - delegate común
  - fuente
  - objectName
- almacenamiento de flags visuales por pane
- menú contextual del header

#### Lo que no cambia

- `populateDatasetTree(...)`
- menús contextuales de items
- acciones ZFS/SSH
- cachés
- `refreshConnectionByIndex(...)`

#### Beneficio

- una sola implementación del árbol base
- opciones visuales realmente independientes por pane
- menos duplicación en `mainwindow_ui.cpp`

Estado:

- completada

### Fase 2: extracción del menú contextual de items

#### Alcance

Crear un constructor común de menú contextual para items de árbol, parametrizado por:

- pane origen (`Top/Bottom`)
- árbol
- item pulsado
- contexto lógico

`MainWindow` seguiría ejecutando las acciones.

#### Beneficio

- evita divergencias entre menús superior e inferior

Estado:

- completada en gran parte mediante `ConnectionDatasetTreeCoordinator` y `MainWindowConnectionDatasetTreeDelegate`

### Fase 3: estado visual unificado del pane

#### Alcance

Mover la captura/restauración de estado visual a un helper del pane o a una estructura común de navegación.

#### Beneficio

- menos dependencia de `m_connContentTree` como puntero mutable global

Estado:

- parcialmente completada
- todavía quedan rutas de estado visual y render que dependen del árbol implícito de `MainWindow`

### Fase 4: presenter opcional

#### Alcance

Introducir un presenter o renderer reutilizable que construya el árbol a partir de:

- caches
- drafts
- opciones del pane

#### Beneficio

- eliminar gran parte de la duplicación de render entre superior e inferior

## Estrategia de migración

### Paso 1

Crear `ConnectionDatasetTreeWidget` como única forma de instanciación del árbol.

Estado:

- ya aplicado para árbol superior e inferior

### Paso 2

Mantener punteros derivados para migración incremental:

- `pane()`
- `tree()`
- `coordinator()`

Estado:

- ya aplicado

### Paso 3

Reducir dependencia de `MainWindow` en el adapter de dominio.

Objetivo siguiente:

- introducir un adapter de dominio menos acoplado
- hacer que el wrapper reusable no necesite conocer `MainWindow`

### Paso 4

Eliminar el uso de `m_connContentTree` como árbol implícito en rutas comunes.

Objetivo siguiente:

- `save/restore`
- selección
- refresco de propiedades
- sincronización de columnas

- `m_topDatasetPane`
- `m_bottomDatasetPane`

manteniendo accesores temporales:

```cpp
QTreeWidget* MainWindow::topConnTree() const;
QTreeWidget* MainWindow::bottomConnTree() const;
```

### Paso 2

Redirigir conexiones de señales desde `MainWindow` a los panes.

### Paso 3

Sustituir el almacenamiento global de flags visuales por estado por pane.

### Paso 4

Actualizar persistencia en `config.ini`:

- `show_inline_property_nodes_top`
- `show_inline_property_nodes_bottom`
- `show_inline_permissions_nodes_top`
- `show_inline_permissions_nodes_bottom`
- `show_inline_gsa_node_top`
- `show_inline_gsa_node_bottom`

Esto ya está alineado con la dirección actual del código.

## Riesgos

### Riesgo 1

Hay mucha lógica que usa `m_connContentTree` como puntero mutable compartido.

#### Mitigación

No eliminar ese patrón en Fase 1. Solo encapsular el control visual.

### Riesgo 2

Refactorizar menús contextuales y render al mismo tiempo sería demasiado agresivo.

#### Mitigación

Separar Fase 1 y Fase 2.

### Riesgo 3

Los tests GUI existentes pueden depender de nombres concretos o de acceso directo al árbol.

#### Mitigación

Mantener accesores de compatibilidad durante la transición.

## Criterios de éxito de la Fase 1

La Fase 1 se considera correcta si:

- ambos paneles usan el mismo widget base
- ambos conservan el comportamiento actual
- los flags `Mostrar en línea` son realmente independientes
- no aumenta la complejidad del código de acciones
- el layout y los tests siguen funcionando

## Recomendación

La Fase 1 es viable y recomendable.

Es una refactorización con una relación coste/beneficio razonable porque:

- reduce duplicación visible
- ataca una fuente recurrente de bugs asimétricos
- no exige mover todavía la lógica de negocio fuera de `MainWindow`

El siguiente paso lógico, si se decide implementarla, es:

1. crear `ConnectionDatasetTreePane`
2. mover la construcción/configuración del `QTreeWidget`
3. mantener temporalmente toda la lógica de render y acciones en `MainWindow`
