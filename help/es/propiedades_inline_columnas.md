# Propiedades inline y columnas

ZFSMgr puede mostrar propiedades de dataset y pool directamente dentro de los árboles de detalle.

## Dónde aparecen

- En un dataset, las propiedades se muestran inline bajo el nodo `Propiedades`, que arranca colapsado.
- En un pool, las propiedades se muestran inline bajo el nodo `Información`.
- En un snapshot, las propiedades se muestran inline bajo el nodo `Propiedades`.

## Visualización

- El número de columnas visibles se cambia desde el menú contextual del encabezado del treeview, en `Columnas de propiedades`.
- La primera columna mantiene siempre el prefijo `Origen:` en el árbol superior y `Destino:` en el inferior.
- Cada árbol usa sus propios anchos de columna.
- Los anchos de columna se guardan en configuración y se restauran al abrir la aplicación.
- El scroll vertical de ambos treeviews es por píxel para que el desplazamiento sea más suave.
- Puede ajustar una columna o todas desde el menú contextual del encabezado del treeview.
  El efecto es el mismo que hacer doble clic en el separador de columnas.
- Desde ese mismo menú contextual también puede elegir entre `5` y `10` columnas de propiedades.

## Gestión de propiedades visibles

Con clic derecho sobre:

- `Propiedades` de un dataset, o
- `Información` de un pool, o
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

## Edición inline

- Las propiedades editables pueden modificarse directamente en el árbol.
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
- Seleccionar un snapshot en un dataset `filesystem` no genera cambios pendientes ni ejecuta comandos ZFS; solo cambia el contexto visual del árbol.
