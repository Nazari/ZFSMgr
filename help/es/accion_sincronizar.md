# Accion: Sincronizar

> **Cómo se pide.** Marque el origen con el botón derecho (`Marcar como origen`) y luego abra el menú contextual **sobre el nodo destino**: el submenú `Con el origen …` ofrece esta acción. Ya no hay botón. Si sale en gris, el motivo está en su tooltip. Ver `Menús contextuales`.

Objetivo: sincronizar contenido de dataset origen hacia dataset destino.

Condiciones:

- Origen: dataset seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben ser distintos y ambos montados. También se admite `canmount=off` si los subdatasets equivalentes están montados en ambos extremos.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- En conexiones Unix usa `rsync` a través del daemon (`--mutate-rsync-local`): es el propio daemon quien sondea las capacidades de rsync (`-A` para ACLs, `-X` para atributos extendidos, `--info=progress2`) y ejecuta el comando, sin construir órdenes de shell. Sin agente en las dos puntas **no se sincroniza**: se dice el motivo antes de abrir el
diálogo de opciones, en vez de preguntarle qué quiere para luego no poder hacerlo.
- En Windows, **entre dos datasets de la misma máquina**, usa la copia propia del agente (`--mutate-copy-tree`), que no necesita `rsync`. Sincroniza de verdad: salta lo que ya está igual, admite `--delete` y sabe simular, así que el `Check` funciona igual que en Unix.
- En Windows **entre máquinas distintas** se sigue usando `tar` sobre SSH (con `zstd` o `gzip` si están disponibles en ambos extremos) y **sin `--delete`**: ahí no sincroniza, copia.
- **En Windows, Sincronizar no borra en el destino.** Esa tubería `tar` añade y sobrescribe,
  pero no quita lo que sobre: la casilla de borrado no se aplica allí. Comprobado de una
  máquina Unix a una Windows. En Unix sí borra, porque va por `rsync` a través del agente.
- En Linux, macOS y FreeBSD, un dataset no montado puede sincronizarse mediante un montaje temporal alternativo: el agente relocaliza su punto de montaje, transfiere y lo restaura al terminar.
- Antes de encolar se abre el diálogo *Opciones de sincronización*, donde puede activar o desactivar `--delete` (no disponible en el modo tar entre máquinas) y lanzar un `Check` (dry-run) cuya salida se muestra en el propio diálogo.
- **Se ejecuta al pulsarla.** Antes se añadía a una lista de cambios pendientes y esperaba
  a que usted la aplicara; esa lista ya no existe.
- La salida de rsync se vuelca al log al terminar la operación. En la ruta a través del daemon **no hay líneas de progreso en tiempo real**.
- El `Check` (dry-run) puede cancelarse desde el diálogo. La ejecución real, una vez aplicada, no dispone de cancelación.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.

Nota sobre interrupciones:

- Si una sincronización con montaje temporal se interrumpe de forma abrupta, el dataset puede quedar montado en un directorio temporal. Use `Reparar mountpoints temporales` en el menú contextual de la conexión para restaurarlo.
