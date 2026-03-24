# Diseño técnico de Conectividad

## Objetivo

La matriz de `Conectividad` muestra si una conexión origen puede alcanzar directamente a otra conexión destino para operaciones remotas que dependan de:

- `SSH`
- `rsync`

Su función principal es validar rutas reales entre máquinas antes de:

- transferencias remotas directas,
- nivelaciones GSA,
- operaciones que dependen de `rsync` extremo a extremo.

## Modelo

- Cada fila representa la conexión origen.
- Cada columna representa la conexión destino.
- Cada celda muestra dos estados:
  - `SSH`
  - `rsync`

Formato visible:

```text
SSH:✓
rsync:✓
```

o, en caso de fallo:

```text
SSH:✗
rsync:-
```

## Reglas de cálculo

### Estados previos

La sonda solo se ejecuta si:

- la conexión origen está en `OK`
- la conexión destino está en `OK`

Si no, la celda queda en estado no verificable.

### Misma máquina

Si origen y destino:

- son la misma conexión, o
- se consideran la misma máquina por `machineUid`,

entonces la celda se marca como:

- `SSH:✓`
- `rsync:✓`

sin prueba remota adicional.

### Conexiones `Local`

Cuando el destino es `Local`, ZFSMgr intenta resolver una conexión `SSH` equivalente hacia esa misma máquina.

Si no existe, la celda queda no verificable y el tooltip explica:

- que `Local` no tiene una conexión `SSH` equivalente para la sonda remota.

### Tipos soportados

La matriz comprueba conectividad saliente solo hacia destinos:

- `SSH`
- `Local` con `SSH` equivalente

No comprueba:

- destinos `PSRP`
- salidas desde conexiones `PSRP`

## Sonda SSH

La verificación base es una ejecución remota del tipo:

- `echo ZFSMGR_CONNECT_OK`

La celda `SSH` queda en verde solo si:

- el comando se ejecuta correctamente
- el código de salida es `0`
- la salida contiene `ZFSMGR_CONNECT_OK`

## Sonda rsync

Si `SSH` es correcto, la matriz hace una segunda comprobación:

- `command -v rsync`

en origen y destino.

La celda `rsync` queda en verde solo si ambos extremos pueden usar `rsync`.

## PATH ampliado en Unix/macOS

Las sondas se ejecutan en shells remotas no interactivas. Por eso ZFSMgr fuerza un `PATH` ampliado antes de comprobar herramientas auxiliares.

Rutas añadidas:

- `/opt/homebrew/bin`
- `/opt/homebrew/sbin`
- `/usr/local/bin`
- `/usr/local/sbin`
- `/usr/local/zfs/bin`
- rutas estándar del sistema

Esto evita falsos negativos cuando herramientas como `sshpass` o `rsync` existen, pero no están en el `PATH` por defecto de la shell no interactiva.

## Password y sshpass

Si la conexión destino necesita autenticación por contraseña, la sonda remota usa `sshpass`.

Consecuencia:

- si la conexión origen no encuentra `sshpass`, la prueba `SSH` falla aunque el destino exista y sea accesible manualmente

Ese caso aparece como celda roja con tooltip explicativo.

## Tooltips de motivo

Toda celda roja debe mostrar el motivo en tooltip.

Motivos normalizados:

- falta `sshpass`
- falta `rsync`
- fallo de autenticación SSH
- error de resolución DNS
- conexión rechazada
- timeout
- mensaje bruto del sistema si no encaja en una categoría conocida

## Relación con GSA

Antes de instalar o actualizar GSA, ZFSMgr usa esta misma lógica para detectar rutas de nivelación remota que no podrán ejecutarse directamente.

Si una ruta requerida no tiene `SSH:✓`:

- ZFSMgr avisa
- la instalación puede continuar
- pero la nivelación seguirá fallando hasta que la conectividad sea correcta

## Limitaciones

- La matriz valida ejecutabilidad real de la ruta, no solo resolución teórica.
- Puede marcar rojo por falta de herramienta auxiliar en origen, aunque SSH manual funcione.
- `rsync:✓` depende de disponibilidad en ambos extremos.
- No sustituye una prueba funcional completa de `zfs send | zfs recv`, pero reduce errores previos evidentes.
