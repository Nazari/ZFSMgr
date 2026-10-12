# Accion: Sincronizar

> **Cómo se pide.** Marque el origen en el panel **izquierdo** y abra el menú contextual **sobre el nodo del panel derecho**: el submenú `Con el origen …` ofrece esta acción. Si sale en gris, el motivo está en su tooltip. Ver `Menús y acciones`.

Objetivo: sincronizar contenido de dataset origen hacia dataset destino.

Condiciones:

- Origen: dataset seleccionado.
- Destino: dataset seleccionado.
- Origen y destino deben ser distintos y ambos montados. También se admite `canmount=off` si los subdatasets equivalentes están montados en ambos extremos.
- Origen y destino deben usar OpenZFS `2.3.3` o superior.

Comportamiento:

- **Entre dos datasets de la misma máquina** usa la copia propia del agente (`--mutate-copy-local`), en todas las plataformas: no hace falta `rsync`. Sincroniza de verdad: salta lo que ya está igual, conserva dueño, modo, fechas, enlaces, ficheros especiales, huecos, ACL y atributos extendidos, admite `--delete` y sabe simular. Sin agente **no se sincroniza**: se dice el motivo antes de abrir el diálogo de opciones.
- **Entre dos máquinas Unix** la interfaz no sincroniza: lo dice y remite a la orden `rsync` de `zfsmgr-cli`, que lo hace con el árbol entre daemons. Volverá a la interfaz en la 2.0.
- En Windows **entre máquinas distintas** se sigue usando `tar` sobre SSH (con `zstd` o `gzip` si están disponibles en ambos extremos) y **sin `--delete`**: ahí no sincroniza, copia.
- **En Windows, Sincronizar no borra en el destino.** Esa tubería `tar` añade y sobrescribe,
  pero no quita lo que sobre: la casilla de borrado no se aplica allí. Comprobado de una
  máquina Unix a una Windows. Dentro de una misma máquina sí borra.
- En Linux, macOS y FreeBSD, un dataset no montado puede sincronizarse mediante un montaje temporal alternativo: el agente relocaliza su punto de montaje, transfiere y lo restaura al terminar.
- Antes de encolar se abre el diálogo *Opciones de sincronización*, donde puede activar o desactivar `--delete` (no disponible en el modo tar entre máquinas) y lanzar un `Check` (dry-run) cuya salida se muestra en el propio diálogo.
- **Se ejecuta al pulsarla.** Antes se añadía a una lista de cambios pendientes y esperaba
  a que usted la aplicara; esa lista ya no existe.
- El resumen de la copia se vuelca al log al terminar la operación.
- El `Check` (dry-run) puede cancelarse desde el diálogo. La ejecución real, una vez aplicada, no dispone de cancelación.
- Si alguna conexión está por debajo de `2.3.3`, la acción se bloquea.

Nota sobre interrupciones:

- Si una sincronización con montaje temporal se interrumpe de forma abrupta, el dataset puede quedar montado en un directorio temporal. Use `Reparar mountpoints temporales` en el menú contextual de la conexión para restaurarlo.
