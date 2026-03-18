# Propiedades inline y columnas

ZFSMgr puede mostrar propiedades de dataset y pool directamente dentro de los árboles de detalle.

## Dónde aparecen

- En un dataset, las propiedades se muestran inline bajo el nodo `Propiedades`, que arranca colapsado.
- En un dataset no snapshot también puede aparecer el nodo `Permisos`, independiente de `Propiedades`.
- En un pool, las propiedades se muestran inline bajo el nodo `Información del pool`.
- En un snapshot, las propiedades se muestran inline bajo el nodo `Propiedades`.
- Los datasets del pool cuelgan directamente del pool.
- Los subdatasets cuelgan directamente de su dataset padre.

## Permisos

- El nodo `Permisos` muestra delegaciones ZFS definidas con `zfs allow`.
- Se organiza en:
  - `Deleg.`
  - `Nuevos DS`
  - `Conjuntos`
- En `Delegaciones`, cada hijo representa una delegación concreta:
  - `Usuario <nombre> Ámbito Local`
  - `Grupo <nombre> Ámbito Desc.`
  - `Everyone Ámbito Local y Desc.`
- Al expandir una delegación o un set, los permisos se muestran inline en rejilla, igual que las propiedades del dataset:
  - fila superior con nombres
  - fila inferior con checks
- `Nuevos DS` indica qué permisos recibirá automáticamente quien cree nuevos descendientes bajo ese dataset.
- Los checks de `Permisos` no ejecutan comandos inmediatamente.
  Modifican un borrador local y el resultado se aplica con `Aplicar cambios`.
- La pestaña `Cambios pendientes` lista también los cambios pendientes de permisos, con prefijo `conexión::pool`.
- La misma pestaña muestra también los cambios pendientes generados por `Mover`, `Renombrar` y otras acciones diferidas, pero enseñando una descripción legible en vez del comando real.

## Visualización

- El número de columnas visibles se cambia desde el menú contextual del encabezado del treeview, en `Columnas de propiedades`.
- La primera columna mantiene siempre el prefijo `Origen:` en el árbol superior y `Destino:` en el inferior.
- Cada árbol usa sus propios anchos de columna.
- Los anchos de columna se guardan en configuración y se restauran al abrir la aplicación.
- El scroll vertical de ambos treeviews es por píxel para que el desplazamiento sea más suave.
- Puede ajustar una columna o todas desde el menú contextual del encabezado del treeview.
  El efecto es el mismo que hacer doble clic en el separador de columnas.
- Desde ese mismo menú contextual también puede elegir entre `5` y `12` columnas de propiedades.

## Gestión de propiedades visibles

Con clic derecho sobre:

- `Propiedades` de un dataset, o
- `Información del pool` de un pool, o
- `Propiedades` de un snapshot

puede abrir `Gestionar visualización de propiedades`.

Ese diálogo permite:

- marcar qué propiedades se muestran,
- reordenarlas por arrastrar y soltar,
- mantener la misma cantidad de columnas que el árbol,
- crear o borrar grupos de visualización,
- guardar la selección y el orden en configuración.

Los grupos son independientes por tipo:

- pool
- dataset
- snapshot

En snapshots:

- la propiedad `snapshot` queda fija en la primera posición del grupo principal,
- los grupos extra no reutilizan los de dataset ni los de pool.

Las propiedades no marcadas no se muestran en el árbol, aunque sigan estando disponibles en la lista de gestión.

## Visibilidad de nodos inline

En el menú contextual del árbol también puede activar o desactivar:

- `Mostrar propiedades en línea`
- `Mostrar Permisos en línea`
- `Mostrar Información del pool`

Efectos:

- si desactiva `Mostrar propiedades en línea`, desaparece el nodo `Propiedades` del árbol;
- si desactiva `Mostrar Permisos en línea`, desaparece el nodo `Permisos`;
- si desactiva `Mostrar Información del pool`, desaparece el nodo `Información del pool`;
- los datasets y subdatasets no usan nodos intermedios `Contenido` ni `Subdatasets`.

## Edición inline

- Las propiedades editables pueden modificarse directamente en el árbol.
- Los permisos ZFS de `Delegaciones`, `Permisos para nuevos subdatasets` y `Sets de permisos` también se modifican directamente en el árbol, pero en modo borrador.
- Algunas propiedades dependen del sistema operativo de la conexión.
  Si una propiedad no está soportada en ese sistema, se muestra atenuada (`greyed-out`) y no permite edición.
  Ejemplos actuales:
  - `sharesmb` desactivada en macOS
  - `jailed` solo editable en FreeBSD
  - `zoned` y `nbmand` solo editables en Linux
  - `vscan` desactivada
- Si una propiedad es heredable, aparece la marca `Inh.` en el nombre.
- En ese caso la celda de valor puede incluir un control adicional `off/on` para aplicar `zfs inherit`.
- El estado inicial de ese control refleja si la propiedad ya está heredada o no.
- Aunque una propiedad heredable esté actualmente heredada, su editor inline de valor sigue disponible.
  Al cambiar el valor, el borrador pasa automáticamente a modo local (`inherit=off`).
- Al hacer clic en una línea de `Cambios pendientes`, ZFSMgr intenta abrir el dataset afectado y situarse sobre la propiedad o sección correspondiente.
- Si la línea corresponde a un `zfs rename`, el foco se intenta situar sobre el objeto origen del renombrado.
- `Cambios pendientes` es la primera pestaña visible por defecto en la zona inferior.
- Seleccionar un snapshot en un dataset `filesystem` no genera cambios pendientes ni ejecuta comandos ZFS; solo cambia el contexto visual del árbol.
