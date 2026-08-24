# Propiedades, permisos y contenido

Todo esto está en el **detalle** de cada panel, debajo de su árbol. Antes vivía dentro del
árbol, con los valores tumbados en unas columnas `C1`…`C10` sin rótulo; con más de cuatro
propiedades había que leerlas en zigzag, y el árbol dejaba de poder recorrerse de un vistazo.

![Pestañas del detalle](qrc:/help/img/auto/detail-tabs.png)

## El camino

Arriba del detalle, lo que se está mirando: `Local / tank1 / user`. Los dos primeros tramos
son enlaces y traen su ficha sin perder lo marcado en el árbol:

- **La conexión** — los campos del perfil (nombre, tipo, host, puerto, usuario, clave, sudo)
  y el diagnóstico: estado, motivo del color, sistema operativo, versión de OpenZFS, estado
  del daemon, gestor de paquetes, comandos instalables.
- **El pool** — sus propiedades y su `zpool status`.

Estas dos fichas enseñan sus datos a **dos pares por fila** —«Propiedad · Valor · Propiedad
· Valor»—, con una raya más marcada entre los dos grupos.

## Propiedades del dataset

La primera pestaña, y la única **editable**.

- Las de valores cerrados salen como desplegable.
- Las que admiten herencia llevan casilla en la columna `Heredada`.
- Las propiedades de usuario —las que llevan `:` en el nombre, como `org.fc16.gsa:*`— son
  editables y también llevan control de herencia.
- No llevan control de herencia las de solo lectura, las que no aplican a la plataforma, y
  `canmount`.
- Las primeras filas —nombre, punto de montaje, `canmount`, tamaño— van fijas arriba.

Lo editado **no se aplica al momento**: se acumula como borrador y sale enumerado en la caja
`Cambios sin aplicar` de abajo. Se aplica con `Aplicar cambios` o se tira con
`Descartar cambios`.

## Contenido

Los ficheros del punto de montaje del dataset, con columnas propias: nombre, permisos,
propietario, grupo, tamaño y fecha de modificación.

Cada directorio se pide **al abrirlo**, no antes: un dataset puede tener miles de ficheros.
Si el dataset no está montado, la pestaña lo dice en vez de quedarse vacía.

En un snapshot se navega por su `.zfs/snapshot`, que es donde ZFS lo deja ver.

## Snapshots

Agrupados por su clase, igual que estaban en el árbol: `Horarios`, `Diarios`, `Semanales`,
`Mensuales`, `Anuales`. Los creados a mano van sueltos, sin grupo.

Marcar uno aquí es marcarlo **como origen o destino** de ese panel, igual que marcarlo en el
árbol: de eso dependen `Enviar`, `Clonar`, `Sincronizar`, `Nivelar` y `Diff`. Y el detalle
pasa a hablar de ese snapshot: sus propiedades, su contenido y sus holds.

## Permisos

Cada delegación es una fila —a quién y con qué ámbito— y debajo sus permisos con casilla.

Se leen **al abrir la pestaña**, no al marcar el dataset: leerlos con la selección sería una
llamada remota por cada movimiento del cursor. Mientras no se hayan leído, la pestaña lo
dice.

Marcar o desmarcar un permiso reescribe la delegación entera, porque `zfs allow` recibe la
lista completa de quien delega. Es un borrador, como las propiedades: sale en
`Cambios sin aplicar` y espera a `Aplicar cambios`.

Crear una delegación nueva está en el menú contextual del dataset, en `Dataset ▸ Permisos`.

## Holds

Solo en snapshots: cada hold con su nombre y su fecha. Liberar uno sale del menú contextual
de su fila. Crear un hold está en el menú contextual del snapshot.

## Qué pestaña se ve

La que no aplica **no se enseña**: un snapshot no delega permisos y un dataset no tiene
holds. Sobre un dataset salen `Propiedades`, `Contenido`, `Snapshots` y `Permisos`; sobre un
snapshot, `Propiedades`, `Contenido`, `Snapshots` y `Holds`.
