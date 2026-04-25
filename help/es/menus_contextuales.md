# Menús contextuales

ZFSMgr usa menús contextuales sobre el árbol unificado.

## Sobre una conexión

![Menú contextual de conexiones](qrc:/help/img/auto/connection-context-menu.png)

- El menú que antes colgaba de la tabla de conexiones ahora cuelga del nodo raíz de conexión.
- Orden actual en conexiones:
  - `Conectar`
  - `Desconectar`
  - `Refrescar`
  - separador
  - `Nueva conexión`
  - `Editar`
  - `Borrar`
  - separador
  - `Daemon`
  - separador
  - `Nuevo pool`
  - separador
  - `Split and root` (submenú: `Derecha`, `Izquierda`, `Abajo`, `Arriba`)
  - separador
  - `Instalar MSYS2`
  - `Instalar comandos auxiliares`

`Daemon` incluye:
- `Instalar/actualizar daemon` (o `Daemon actualizado y funcionando` si ya está al día)
- `Desinstalar daemon`

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

- En dataset filesystem (y en nodo pool fusionado):
  - `Gestionar propiedades`
  - `Dataset`
  - `Acciones`
  - `Split and root` (submenú: `Derecha`, `Izquierda`, `Abajo`, `Arriba`)
  - `Seleccionar como origen`
  - `Seleccionar como destino`
- Submenú `Dataset`:
  - `Crear`
  - `Renombrar`
  - `Borrar`
  - `Clave de Encriptación` (`Cargar Clave`, `Descargar Clave`, `Cambiar Clave`)
  - `Programar snapshots`
  - `Permisos` (`Nuevo Set`, `Nueva Delegación`)
- Submenú `Acciones`:
  - `Desglosar`
  - `Ensamblar`
  - `Desde Dir`
  - `Hasta Dir`
- En snapshots:
  - `Gestionar propiedades`
  - `Borrar snapshot`
  - `Rollback`
  - `Nuevo Hold`
  - `Seleccionar como origen`
- En holds:
  - `Release`

## Sobre el nodo raíz de un panel dividido

- Si el nodo es la raíz de un panel dividido (split), aparece la opción:
  - `Close`: cierra ese panel y libera el espacio en el divisor.

## Reglas

- Las acciones destructivas piden confirmación.
- Varias acciones trabajan en modo diferido y se acumulan en `Pending changes`.
- `Seleccionar como origen` y `Seleccionar como destino` actualizan la línea `Source/Target` de la caja `Acciones`.
- No hay menú contextual en nodos `Dataset properties`, `Snapshot properties` ni en el nodo `@`.
- En pools suspendidos, la mayoría de las acciones del menú contextual aparecen deshabilitadas.
