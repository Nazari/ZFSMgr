# Diseño técnico: refactorización de treeviews de conexión

## Objetivo

Reducir la duplicación entre el treeview superior y el inferior mediante un componente común reutilizable, manteniendo en una primera fase la lógica de datos, cachés y acciones en `MainWindow`.

El objetivo no es rehacer toda la arquitectura del módulo de conexiones, sino extraer una pieza reutilizable que elimine divergencias visuales y de interacción entre ambos árboles.

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

## Componente propuesto

### Nombre

`ConnectionDatasetTreePane`

### Naturaleza

`QWidget` compuesto que encapsula un `QTreeWidget` y su configuración visual.

### Instancias previstas

- una instancia para el panel superior
- una instancia para el panel inferior

## Responsabilidades del nuevo componente

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

## Interfaz pública sugerida

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

`MainWindow` usará el widget como vista:

- `m_topDatasetPane`
- `m_bottomDatasetPane`

En esta fase, el código de render seguirá recibiendo un `QTreeWidget*`, pero ese puntero se obtendrá desde el pane.

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

### Fase 3: estado visual unificado del pane

#### Alcance

Mover la captura/restauración de estado visual a un helper del pane o a una estructura común de navegación.

#### Beneficio

- menos dependencia de `m_connContentTree` como puntero mutable global

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

Crear `ConnectionDatasetTreePane` y sustituir en `MainWindow`:

- `m_connContentTree`
- `m_bottomConnContentTree`

por:

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
