# Menús contextuales

ZFSMgr usa menús contextuales sobre el árbol unificado.

## Sobre una conexión

![Menú contextual de conexiones](qrc:/help/img/auto/connection-context-menu.png)

- El menú que antes colgaba de la tabla de conexiones ahora cuelga del nodo raíz de conexión.
- Orden actual en conexiones:
  - `Nueva conexión`
  - separador
  - resto de acciones en el orden existente (`Refrescar`, `Editar`, `Borrar`, `Nuevo pool`, `GSA`, etc.)

## Sobre el nodo raíz del pool fusionado

![Menú contextual de pool importado](qrc:/help/img/auto/pool-context-menu-imported.png)

- El primer submenú es `Pool`.
- Dentro de `Pool` aparecen las acciones de pool:
  - `Refresh status`
  - `Importar`
  - `Importar renombrando`
  - `Exportar`
  - `Historial`
  - `Gestión`
- `Gestión` ejecuta acciones inmediatas (`sync`, `scrub`, `upgrade`, `reguid`, `trim`, `initialize`, `clear`, `destroy`) con diálogo de parámetros cuando aplica.
- Después del submenú `Pool` siguen las acciones normales de dataset sobre ese mismo nodo dual.

## Sobre datasets y snapshots

- Acciones típicas:
  - `Gestionar visualización de propiedades`
  - `Crear dataset/snapshot/vol`
  - `Renombrar`
  - `Borrar`
  - `Encriptación`
  - `Programar snapshots automáticos` (solo dataset filesystem sin GSA activo en ancestros)
  - `Rollback`
  - `Nuevo Hold`
  - `Release`
  - `Desglosar`
  - `Ensamblar`
  - `Desde Dir`
  - `Hacia Dir`
  - `Seleccionar como origen`
  - `Seleccionar como destino`

## Reglas

- Las acciones destructivas piden confirmación.
- Varias acciones trabajan en modo diferido y se acumulan en `Pending changes`.
- `Seleccionar como origen` y `Seleccionar como destino` actualizan la línea `Source/Target` de la caja `Acciones`.
- `Gestionar visualización de propiedades` aplica a nodos de propiedades (`Dataset properties`, `Snapshot properties`, `Pool Information`).
