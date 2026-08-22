# Accion: Desde Dir

Objetivo: crear un dataset hijo a partir de uno o varios directorios existentes.

Condiciones:

- Dataset seleccionado en el árbol del pool.
- La seleccion debe ser dataset (no snapshot).
- **Requiere el agente `zfsmgr-agent` en el destino.** Ningún cliente opera sobre una máquina
  sin agente: no hay camino alternativo por shell.

Comportamiento:

- Abre una ventana para definir el nuevo dataset (propiedades, creación de padres con `-p` y cifrado opcional) y para marcar los directorios origen.
- Puede marcar **varios directorios, incluso de conexiones distintas**. Si marca un directorio que ya contiene a otro marcado, se descarta el descendiente. La jerarquía relativa de cada directorio se reproduce dentro del dataset destino.
- Crea el dataset, fuerza `canmount=on` y lo monta en su punto de montaje definitivo.
- **Los datos van de máquina a máquina, no por la suya.** Cuando las dos puntas tienen el
  agente, el destino se pone a escuchar y el origen le manda el árbol directamente. Copiar
  100 GB de una máquina a otra ya no mueve 200 GB por el equipo desde el que usted manda.
- Es un **trabajo del daemon**: hay progreso, se puede cancelar, y sigue si cierra la ventana.
- La copia es **incremental**: una segunda pasada solo mueve lo que cambió.
- Si a alguna de las dos puntas le falta el agente, o si marca la casilla de borrar los
  directorios de origen, se usa el camino antiguo: una tubería `tar --acls --xattrs` con las
  dos puntas por SSH, que sí pasa los datos por su equipo. El borrado va por ahí a propósito:
  por el camino nuevo la copia es asíncrona y el borrado se lanzaría sin saber si terminó.
- Si la copia termina correctamente y marcó esa casilla, borra los directorios origen.
- **Se ejecuta al pulsarla.** Antes se añadía a una lista de cambios pendientes y esperaba a
  que usted la aplicara; esa lista ya no existe.
- Registra comandos y resultado en el log combinado.

Advertencias importantes:

- **El dataset queda montado al terminar**, también si decide no borrar los directorios origen.
- No hay verificación posterior de la copia: el borrado del origen depende únicamente del código de salida de la copia.
