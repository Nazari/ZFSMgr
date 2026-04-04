# Propiedades inline y columnas

ZFSMgr muestra propiedades de dataset y pool directamente dentro del árbol unificado.

## Dónde aparecen

- En datasets, bajo `Dataset properties`.
- En snapshots, bajo `Snapshot properties`.
- En datasets no snapshot, también puede aparecer `Permisos`.
- En el nodo dual pool/dataset raíz, puede aparecer `Pool Information`.
- En datasets filesystem, los snapshots cuelgan del nodo `@`.
- En pools con datasets GSA activos, puede aparecer `Datasets programados`.
- En conexiones, bajo `Propiedades de conexión`.

## Visualización

- El número de columnas visibles se ajusta desde el menú contextual del encabezado del árbol.
- El rango actual de `Columnas de propiedades` es:
  - `4, 6, 8, 10, 12, 14, 16`
- Los anchos de columna se guardan y se restauran.
- El scroll vertical del árbol es suave.

## Gestión de propiedades visibles

Con clic derecho sobre:

- `Dataset properties`
- `Snapshot properties`
- `Pool Information`
- `Propiedades de conexión`

puede abrir `Gestionar visualización de propiedades`.

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
- conexión

## Edición inline

- Las propiedades editables se modifican directamente en el árbol.
- Las propiedades heredables muestran `Inh.` cuando aplica.
- Los permisos ZFS también se editan inline, pero en modo borrador.
- Las propiedades `org.fc16.gsa:*` no usan control de herencia visual.
- Al pulsar una línea en `Pending changes`, ZFSMgr intenta enfocar el objeto y la sección correspondiente.
