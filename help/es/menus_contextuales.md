# Menús y acciones

Gestionar conexiones está en la **barra de menús**. Lo que se hace SOBRE algo marcado está
en el **menú contextual** del árbol y de las pestañas del detalle.

## El menú «Conexiones»

![Menú Conexiones](qrc:/help/img/auto/connections-menu.png)

Actúa sobre la conexión del panel que se haya tocado el último. Como esa elección no se ve,
el nombre va **dentro** del rótulo: `Editar «unibody»`, no `Editar`.

- `Nueva Conexión`
- `Editar` · `Borrar`
- `Conectar` · `Desconectar` · `Refrescar` · `Refrescar todas`
- `Nuevo Pool`
- `Instalar comandos auxiliares`
- `Reinstalar/Actualizar daemon`
- `Reparar mountpoints temporales`
- `Exportar trust-store a esta conexión`
- `Autorizar clave SSH en…` (submenú con las demás conexiones SSH conectadas)
- `Entregar credenciales de las demás máquinas…`
- `Cambiar credenciales sudo local…` (solo en la conexión Local)

Condiciones de habilitación:

- `Conectar`: conexión marcada como desconectada y sin acción en curso.
- `Desconectar` y `Refrescar`: conexión conectada y sin acción en curso.
- `Editar` y `Borrar`: no disponibles en la conexión Local ni en conexiones redirigidas a
  Local.
- `Nuevo Pool`: conexión conectada.
- `Instalar comandos auxiliares`: solo si el refresco detectó un gestor de paquetes y un
  plan de instalación soportado para los comandos que faltan. No aplica a conexiones
  Windows, que trabajan solo con el agente nativo.
- `Reinstalar/Actualizar daemon`: cualquier conexión, Windows y Local incluidas, y **también
  desconectada** — que es justo cuando suele hacer falta.
- `Reparar mountpoints temporales`: cualquier conexión no Windows, incluida Local (un
  dataset local también puede quedarse en un mountpoint temporal).
- `Exportar trust-store a esta conexión`: cualquier conexión remota; no aplica a Local, que
  ya usa el trust-store local.
- `Autorizar clave SSH en…`: solo en conexiones SSH no Windows, y solo se puebla con otras
  conexiones SSH conectadas.
- `Cambiar credenciales sudo local…`: solo en Local, que es la única conexión que no se
  puede editar. Se ofrece incluso desconectada: una contraseña equivocada es lo que puede
  haberla dejado así.

`Reparar mountpoints temporales` hace primero una pasada de solo lectura, muestra los
datasets que quedaron con el mountpoint relocalizado por una sincronización interrumpida y
pide confirmación antes de restaurarlos (los desmonta antes de hacerlo). Los que fallen
conservan su marca y se pueden reintentar.

## Actualización automática del daemon

Al terminar un refresco, ZFSMgr reinstala el daemon automáticamente y sin diálogos **solo**
cuando el motivo de atención es una desalineación de versión o de API.

Un backoff TLS de daemon-rpc marca la conexión para atención pero **no** dispara la
reinstalación automática: reinstalar regeneraría el material TLS y perpetuaría el bucle
fallo → reinstalación → fallo. En ese caso use `Reinstalar/Actualizar daemon` o
`Exportar trust-store a esta conexión` manualmente.

## Sobre el nodo raíz del pool

![Menú contextual de pool importado](qrc:/help/img/auto/pool-context-menu-imported.png)

El nodo del pool y su dataset raíz son el mismo item, así que el menú lleva las dos cosas:
primero un submenú `Pool` y después las acciones normales de dataset.

Dentro de `Pool`:

- `Actualizar estado`
- `Importar` · `Importar renombrando` · `Exportar`
- `Historial`
- `Gestión`: `Sync`, `Scrub`, `Upgrade`, `Reguid`, `Trim`, `Initialize`, `Clear`, `Destroy`
  — acciones inmediatas, con diálogo de parámetros cuando aplica.

## Sobre datasets

- `Dataset`:
  - `Crear` · `Renombrar` · `Borrar`
  - `Montar`: solo si el dataset tiene `canmount` distinto de `off`, un `mountpoint` válido
    y **no** está ya montado.
  - `Desmontar`: solo si **está** montado. No se le exige `canmount`: un dataset puede estar
    montado y tener después `canmount=off`, y es justo entonces cuando hace falta.
  - `Clave de Encriptación`: `Cargar Clave`, `Descargar Clave`, `Cambiar Clave`
  - `Programar snapshots`
  - `Permisos`: `Nuevo Set`, `Nueva Delegación`
- `Acciones` (operaciones sobre los DATOS, no sobre el estado del dataset):
  `Desglosar`, `Ensamblar`, `Desde Dir`, `Hacia Dir`
- `Con el origen …` — las seis acciones de dos extremos; ver abajo.

## Sobre snapshots

En la pestaña `Snapshots` del detalle, con el mismo menú que tenían dentro del árbol:

- `Borrar snapshot` · `Rollback` · `Nuevo Hold`
- `Con el origen …`

Y en la pestaña `Holds`, sobre una fila: `Liberar`.

## Las seis acciones de origen y destino

`Enviar`, `Mover`, `Clonar`, `Sincronizar`, `Nivelar` y `Diff` necesitan **dos** extremos, y
cada panel aporta el suyo: el izquierdo es el origen y el derecho el destino.

1. Marque el dataset o snapshot de partida en el panel **izquierdo**.
2. Clic derecho sobre el nodo del panel **derecho**: ese es el destino, y el submenú
   `Con el origen <nombre>` ofrece las seis, nombrando el origen en cada una:

```
Con el origen datos@lunes ▸
   Enviar aquí desde datos@lunes
   Mover aquí desde datos@lunes
   Clonar aquí desde datos@lunes
   Sincronizar aquí desde datos@lunes
   Nivelar con datos@lunes
   Comparar con datos@lunes
```

Marcar un snapshot en la pestaña `Snapshots` cuenta igual que marcarlo en el árbol.

Lo que no aplica sale **en gris, con el motivo en el tooltip**: que el origen no es un
snapshot, que los pools no coinciden, que `Diff` compara dos puntos del mismo dataset, o que
las versiones de OpenZFS no son compatibles para transferir.

**`Mover` no copia nada.** Es un `zfs rename`: el dataset cambia de sitio en el árbol y los
datos se quedan donde están, así que es instantáneo y no queda un original que borrar. Por
eso solo funciona **dentro del mismo pool y la misma máquina**, y con datasets a los dos
lados —nunca snapshots—. Para llevar algo a otro pool o a otra máquina es `Enviar`. Lo que
sí cambia es la ruta de montaje de ese dataset y la de todo lo que cuelgue de él.

## Reglas

- Las acciones destructivas piden confirmación.
- Las **propiedades**, los **permisos** y los **renombrados** se editan como borradores y se
  aplican con `Aplicar cambios`. Las acciones no: se ejecutan al pulsarlas.
- La caja `Cambios sin aplicar` enumera exactamente lo que van a hacer esos botones.
- En pools suspendidos, la mayoría de las acciones del menú contextual aparecen
  deshabilitadas.
