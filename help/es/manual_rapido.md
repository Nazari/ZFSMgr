# Manual rápido

ZFSMgr trabaja con **dos paneles**: el de la izquierda es el ORIGEN y el de la derecha el
DESTINO. La posición no es una preferencia de colocación: es lo que decide qué papel juega
cada selección en las acciones de transferencia.

## Vista general

![Ventana principal](qrc:/help/img/auto/main-window.png)

De arriba abajo:

- **Barra de menús**: `Menú`, `Conexiones`, `Ajustes`, `Ayuda`.
- **Estado y Progreso**, en una línea: lo que está pasando y el último mensaje.
- **Los dos paneles**, cada uno en tres filas separadas por divisores:
  - **Desplegables** de conexión y de pool.
  - **Árbol** de pools y datasets.
  - **Detalle** de lo que haya marcado, con sus pestañas.
  - **Log** de la conexión del panel, con `Log` y `Daemon`.
- Abajo, dos cajas: **Cambios sin aplicar** y **Transferencias**.

Los divisores son **uno solo para los dos paneles**: mover la frontera entre el árbol y el
detalle la mueve en los dos lados a la vez, para que las dos columnas se lean a la misma
altura. Entre el panel izquierdo y el derecho no hay divisor: el ancho se reparte a medias.

## Elegir qué se ve

Cada panel tiene dos desplegables:

- **Conexión**: la máquina. Cambiarla cambia el árbol, el detalle y el log de ese panel.
- **Pool**: `(todos los pools)` enseña todos los de esa conexión; eligiendo uno concreto, el
  árbol se enraíza en él.

Si elige un **pool sin importar** —sale marcado `[Importable]`—, se abre el diálogo de
importación con sus opciones. Un pool sin importar no tiene datasets que enseñar, así que
elegirlo es pedir importarlo.

## El árbol

Dentro hay **pools y datasets, y nada más**. Las propiedades, los permisos, el contenido y
los snapshots están en el detalle de abajo.

- Un pool sin importar sale marcado `[Importable]`.
- Un pool suspendido sale marcado `(Suspended)` y tiene bloqueada la mayoría de sus
  operaciones.
- El nodo raíz del pool está fusionado con el dataset raíz del pool: mantiene el icono de
  pool y actúa también como dataset raíz, para no duplicar `pool/pool`.

## El detalle

![Pestañas del detalle](qrc:/help/img/auto/detail-tabs.png)

Debajo de cada árbol, con el camino completo de lo que se está mirando arriba:
`Local / tank1 / user`. Los dos primeros tramos son **enlaces**:

- **La conexión** trae su ficha —los campos del perfil y el diagnóstico: estado, motivo del
  color, sistema operativo, versión de OpenZFS, daemon, gestor de paquetes—.
- **El pool** trae la suya: sus propiedades y su `zpool status`.

Se pulsan sin perder lo que haya marcado en el árbol, y marcar cualquier otra cosa vuelve al
objeto.

Con un **dataset** marcado hay cuatro pestañas:

- `Propiedades` — editables. Las de valores cerrados salen como desplegable, y las que
  admiten herencia llevan casilla `Heredada`.
- `Contenido` — los ficheros del punto de montaje. Cada directorio se pide al abrirlo.
- `Snapshots` — agrupados por su clase: `Horarios`, `Diarios`, `Semanales`, `Mensuales`,
  `Anuales`; los creados a mano van sueltos.
- `Permisos` — las delegaciones, cada una con sus permisos y su casilla.

Con un **snapshot** marcado, `Permisos` se cambia por `Holds`: un snapshot no delega
permisos y un dataset no tiene holds, así que la pestaña que no aplica no se enseña.

## Elegir origen y destino

- El **origen** es lo marcado en el panel izquierdo; el **destino**, lo marcado en el
  derecho.
- Marcar un snapshot en la pestaña `Snapshots` cuenta igual que marcarlo en el árbol.
- Las acciones —`Enviar`, `Clonar`, `Sincronizar`, `Nivelar`, `Diff`— se piden desde el menú
  contextual del destino, igual que al pegar.

## Cambios sin aplicar

Las acciones se ejecutan al pulsarlas. Lo que **sí** se edita en lote son las
**propiedades** y los **permisos**: se acumulan como borradores y se aplican con
`Aplicar cambios`.

La caja de abajo a la izquierda enumera exactamente lo que van a hacer esos botones, uno por
línea. `Descartar cambios` los tira, avisando antes de qué se pierde.

Los borradores **se pierden al cerrar sin aplicarlos**: no hay nada que sobreviva al cierre
de la aplicación.

## Transferencias

La caja de abajo a la derecha enseña los **trabajos en marcha** en los daemons: qué está
corriendo y su progreso. `Refrescar` vuelve a preguntar, y `Cancelar seleccionado` corta el
que esté marcado.

## Logs

Bajo cada panel, el log de **su** conexión, con dos pestañas:

- `Log · <máquina>` — lo que se ha ejecutado en esa máquina.
- `Daemon · <máquina>` — el log del daemon, con un botón `Heartbeat` para preguntarle.

Si la misma conexión está elegida en los dos paneles, los dos enseñan el mismo log sin
duplicarlo.

El log de la aplicación no se enseña en la ventana: se escribe en disco y se copia desde
`Ajustes ▸ Logs`.

## Creación de pools

![Crear pool](qrc:/help/img/crearpool.png)

- `Conexiones ▸ Nuevo Pool` abre el constructor de VDEV y parámetros del pool.
- La estructura del árbol del pool valida combinaciones OpenZFS compatibles.
- Si falla, el diálogo permanece abierto para corregir y reintentar.

## Creación de datasets

![Crear dataset](qrc:/help/img/creardataset.png)

- `Crear dataset` se abre desde el menú contextual del árbol.
- Si el dataset es cifrado con `keylocation=prompt`, ZFSMgr pide passphrase.
- Si falla, el diálogo permanece abierto con los datos introducidos.

## Navegación

- Cada panel recuerda por separado qué tenía desplegado: los dos pueden estar sobre la misma
  conexión sin pisarse.
- La elección de conexión y de pool sobrevive a los refrescos: se busca por identificador,
  no por posición en la lista.
