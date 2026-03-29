# Menús contextuales

ZFSMgr usa menús contextuales sobre el árbol unificado.

## Sobre una conexión

![Menú contextual de conexiones](qrc:/help/img/auto/connection-context-menu.png)

- El menú que antes colgaba de la tabla de conexiones ahora cuelga del nodo raíz de conexión.
- Incluye acciones como:
  - `Nueva conexión`
  - `Refrescar`
  - `Editar`
  - `Borrar`
  - `Nuevo pool`
  - acciones GSA y refresco

## Sobre el nodo raíz del pool fusionado

![Menú contextual de pool importado](qrc:/help/img/auto/pool-context-menu-imported.png)

- El primer submenú es `Pool`.
- Dentro de `Pool` aparecen las acciones de pool:
  - `Actualizar`
  - `Importar`
  - `Importar renombrando`
  - `Exportar`
  - `Historial`
  - `Gestión`
  - `Mostrar Información del Pool`
  - `Mostrar Datasets programados`
- Después del submenú `Pool` siguen las acciones normales de dataset sobre ese mismo nodo dual.

## Sobre datasets y snapshots

- Acciones típicas:
  - `Gestionar visualización de propiedades`
  - `Mostrar propiedades en línea`
  - `Mostrar Permisos en línea`
  - `Crear dataset/snapshot/vol`
  - `Renombrar`
  - `Borrar`
  - `Encriptación`
  - `Seleccionar snapshot`
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
- `Seleccionar como origen` y `Seleccionar como destino` rellenan la caja `Selected datasets`.
- `Gestionar visualización de propiedades` aplica a `Dataset properties` y `Pool Information`.
