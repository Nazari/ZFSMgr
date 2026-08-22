# Propiedades inline y columnas

ZFSMgr muestra propiedades de dataset y pool directamente dentro del árbol unificado.

## Dónde aparecen

- En datasets, bajo `Dataset properties`.
- En snapshots, bajo `Snapshot properties`.
- En datasets no snapshot, también puede aparecer `Permisos`.
- En el nodo dual pool/dataset raíz, puede aparecer `Pool Information`.
  - Dentro puede aparecer `Dispositivos` (árbol de vdev/discos del pool).
- En datasets filesystem, los snapshots cuelgan del nodo `@`.
- En pools con datasets GSA activos, puede aparecer `Datasets programados`.
- En conexiones, bajo `Properties`.
- En conexiones, `Info` agrupa:
  - `General`
  - `Daemon`
  - `Commands`

## Visualización

- El número de columnas visibles se ajusta desde el menú contextual del encabezado del árbol.
- El rango actual de `Columnas de propiedades` es:
  - `4, 6, 8, 10, 12, 14, 16`
- Los anchos de columna se conservan al cambiar de conexión o de panel dentro de la misma sesión; no se mantienen entre arranques de la aplicación.
- El scroll vertical del árbol es suave.

## Gestión de propiedades visibles

Con clic derecho sobre:

- `Dataset properties`
- `Snapshot properties`
- `Pool Information`

puede abrir `Gestionar visualización de propiedades`.

El nodo `Properties` de la conexión no tiene menú contextual: sus campos se editan directamente en línea.

Ese diálogo permite:

- elegir propiedades visibles
- reordenarlas por arrastrar y soltar
- crear grupos
- renombrarlos
- borrar grupos

Los grupos son independientes por:

- pool
- dataset
- snapshot

## Edición inline

- Las propiedades editables se modifican directamente en el árbol.
- Las propiedades heredables muestran `Inh.` cuando aplica.
- Los permisos ZFS también se editan inline, pero en modo borrador.
- Las propiedades de usuario (las que llevan `:` en el nombre, como `org.fc16.gsa:*`) son editables y también muestran el control de herencia.
- No muestran control de herencia las propiedades de solo lectura, las que no aplican a la plataforma, y `canmount`.
- La lista de cambios pendientes ya no existe, así que tampoco el salto al objeto desde ella.
Los borradores de propiedades se ven en la propia rejilla, resaltados.
