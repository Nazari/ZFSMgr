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
  - `Nueva Conexión`
  - `Editar`
  - `Borrar`
  - separador
  - `Nuevo Pool`
  - separador
  - `Instalar MSYS2`
  - `Instalar comandos auxiliares`
  - `Reinstalar/Actualizar daemon`
  - `Reparar mountpoints temporales`
  - `Exportar trust-store a esta conexión`
  - `Autorizar clave SSH en...` (submenú con las demás conexiones SSH conectadas)
  - separador
  - `Split and root` (submenú: `Derecha`, `Izquierda`, `Abajo`, `Arriba`), o `Close` si el menú se abre sobre la raíz de un panel dividido

Condiciones de habilitación:

- `Conectar`: conexión marcada como desconectada y sin acción en curso.
- `Desconectar` y `Refrescar`: conexión conectada y sin acción en curso.
- `Editar` y `Borrar`: no disponibles en la conexión Local ni en conexiones redirigidas a Local.
- `Nuevo Pool`: conexión conectada.
- `Instalar MSYS2`: solo en conexiones Windows sin una capa Unix (MSYS2/MinGW) completa.
- `Instalar comandos auxiliares`: solo si el refresco detectó un gestor de paquetes y un plan de instalación soportado para los comandos que faltan.
- `Reinstalar/Actualizar daemon`: conexiones remotas no Windows.
- `Reparar mountpoints temporales`: cualquier conexión no Windows, incluida Local (un dataset local también puede quedarse en un mountpoint temporal).
- `Exportar trust-store a esta conexión`: cualquier conexión remota; no aplica a Local, que ya usa el trust-store local.
- `Autorizar clave SSH en...`: solo en conexiones SSH no Windows, y solo se puebla con otras conexiones SSH conectadas.

`Reparar mountpoints temporales` hace primero una pasada de solo lectura, muestra los datasets que quedaron con el mountpoint relocalizado por una sincronización interrumpida y pide confirmación antes de restaurarlos (los desmonta antes de hacerlo). Los que fallen conservan su marca y se pueden reintentar.

## Actualización automática del daemon

Al terminar un refresco, ZFSMgr reinstala el daemon automáticamente y sin diálogos **solo** cuando el motivo de atención es una desalineación de versión o de API.

Un backoff TLS de daemon-rpc marca la conexión para atención pero **no** dispara la reinstalación automática: reinstalar regeneraría el material TLS y perpetuaría el bucle fallo → reinstalación → fallo. En ese caso use `Reinstalar/Actualizar daemon` o `Exportar trust-store a esta conexión` manualmente.

## Sobre el nodo raíz del pool fusionado

![Menú contextual de pool importado](qrc:/help/img/auto/pool-context-menu-imported.png)

- El primer submenú es `Pool`.
- Dentro de `Pool` aparecen las acciones de pool:
  - `Actualizar estado`
  - `Importar`
  - `Importar renombrando`
  - `Exportar`
  - `Historial`
  - `Gestión`
- `Gestión` ejecuta acciones inmediatas (`Sync`, `Scrub`, `Upgrade`, `Reguid`, `Trim`, `Initialize`, `Clear`, `Destroy`) con diálogo de parámetros cuando aplica.
- Justo después del submenú `Pool` aparece `Split and root`, y a continuación las acciones normales de dataset sobre ese mismo nodo dual.

## Sobre datasets y snapshots

- En dataset filesystem (y en nodo pool fusionado):
  - `Gestionar visualización de propiedades`
  - `Dataset`
  - `Acciones`
  - `Seleccionar como origen`
  - `Seleccionar como destino`
  - `Split and root` (submenú: `Derecha`, `Izquierda`, `Abajo`, `Arriba`)
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
  - `Mount`: solo se habilita si el dataset tiene `canmount` distinto de `off`, un `mountpoint` válido y no está ya montado.
- En snapshots:
  - `Gestionar visualización de propiedades`
  - `Borrar snapshot`
  - `Rollback`
  - `Nuevo Hold`
  - `Seleccionar como origen`
- En holds:
  - `Release`

En el nodo pool fusionado, `Split and root` aparece justo después del submenú `Pool`, no al final.

## Sobre el nodo raíz de un panel dividido

- Si el nodo es la raíz de un panel dividido (split), aparece la opción:
  - `Close`: cierra ese panel y libera el espacio en el divisor.

## Reglas

- Las acciones destructivas piden confirmación.
- Varias acciones trabajan en modo diferido y se acumulan en `Pending changes`.
- `Seleccionar como origen` y `Seleccionar como destino` actualizan la línea `Source/Target` de la caja `Acciones`.
- El nodo `@` (agrupador de snapshots) no tiene menú contextual.
- En `Dataset properties` y `Snapshot properties` el menú contextual contiene únicamente `Gestionar visualización de propiedades`.
- Los hijos de la raíz de conexión que no son raíz de pool (`Properties`, `Info`) no tienen menú contextual.
- En pools suspendidos, la mayoría de las acciones del menú contextual aparecen deshabilitadas.
