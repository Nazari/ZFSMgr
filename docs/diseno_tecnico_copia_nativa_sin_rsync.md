# Copiar árboles sin rsync: una implementación en el agente

## Por qué

`rsync` no existe en Windows, y de ahí cuelgan cuatro de las cinco funciones que
siguen apagadas allí: `Sincronizar`, `Hacia Dir`, `Desglosar` y `Ensamblar`.

La salida evidente sería instalar un `rsync` para Windows —cwRsync—, y **se descartó
tras medir**. Los motivos, por orden de peso:

1. **Sería una segunda capa Unix.** Todo el rediseño de Windows consistió en quitar
   MSYS2 para dejar de depender de una. Cygwin la reintroduce, y a partir de ahí hay
   que mantenerla para siempre por una función.
2. **Lo que peor traduce es justo lo que importa.** `-A`/`-X` sobre Cygwin mapean a su
   emulación de ACL POSIX encima de NTFS. Esos flags son *la razón* de usar rsync; si
   van a producir permisos aproximados, el remedio es peor que la carencia.
3. **Rutas.** Cygwin espera `/cygdrive/c/...`, no `C:\`. Traducir cada ruta es
   exactamente la clase de suposición Unix que ha costado repetidas rondas de fallos.
4. `rsync` es GPLv3 y el cwRsync actual es comercial; la edición libre está anticuada.

Y hay un motivo que apareció al mirar el código, no antes: **`rsync` no se usa solo en
local**. `Sincronizar` entre dos máquinas lo usa como transporte de red
(`mainwindow_transfer.cpp:2032`):

```
rsync $RSYNC_OPTS $RSYNC_PROGRESS -e <ssh> "$SRC_MP"/ <host>:"$DST_MP"/
```

Con un extremo Windows, ese puente no se puede tender de ninguna manera.

## Qué se le pide hoy a rsync, exactamente

Son **dos cosas**, y conviene no confundirlas porque tienen costes muy distintos.

### 1. Copiar un árbol, fielmente

`daemon_main.cpp:590`, `runRsyncCopyMoveCapture`:

```
rsync -aHWS [-x] [-A] [-X] --exclude=/<nombre>/ <src>/ <dst>/
```

| Flag | Qué preserva | Notas |
|---|---|---|
| `-a` | recursivo, permisos, marcas de tiempo, enlaces simbólicos, propietario y grupo | |
| `-H` | **enlaces duros** | El punto conflictivo; ver abajo |
| `-W` | copia entera, sin deltas | Es local: el delta no aporta |
| `-S` | ficheros dispersos | |
| `-x` | no cruzar a otro sistema de ficheros | Crítico en `Ensamblar` |
| `-A` `-X` | ACL y atributos extendidos | Se sondean antes; no siempre están |
| `--exclude=/x/` | anclado en la raíz de la copia | Sin el `/` inicial se comería cualquier `x` del subárbol |

Llamantes: `Desglosar` (`:985`), `Ensamblar` (`:1344`, `:1355`, `:1458`) y
`Hacia Dir` (`:2927`, `:2936`).

### 2. Verificar antes de borrar el origen

`daemon_main.cpp:687`, `countPendingRsyncTransfers`:

```
rsync -rni --ignore-existing [-x] --exclude=/<nombre>/ <src>/ <dst>/
```

Un simulacro que lista lo que **aún faltaría** por transferir; se cuentan las líneas
que empiezan por `>f`. Cero pendientes = la copia está completa.

Y no es decorativo. `Ensamblar` lo usa así (`:1344-1360`): copia, verifica, y **hasta
cuatro reintentos** mientras queden pendientes, antes de un `zfs destroy -r` que no
admite vuelta atrás. El comentario del código lo dice sin rodeos: *si la copia quedó
incompleta, los datos no están en ningún sitio*.

> **Esta parte no puede ser «ya la haremos».** Es la única red que impide perder datos
> en tres operaciones que borran el origen.

## Lo que se midió en Windows

Contra OldLau, con OpenZFS on Windows. Dos hallazgos que condicionan el diseño.

**`robocopy` cubre casi todo lo de rsync**, y con el modelo de permisos correcto en vez
de una emulación:

| Necesidad | `robocopy` |
|---|---|
| datos, atributos, marcas, **ACL de NTFS**, propietario | `/COPYALL` |
| ficheros dispersos | `/SPARSE` |
| enlaces simbólicos como enlaces | `/SL` |
| no cruzar límites de dataset | `/XJ` |
| simulacro para verificar | `/L` |
| **enlaces duros** | **NO los conserva** |

**Los datasets son junctions colgados de la raíz de la unidad, y NO se anidan.** Con
`winpool/padre` y `winpool/padre/hijo`, ambos con `mountpoint` anidado:

```
Directory of Z:\
  <JUNCTION>  padre  [\??\Volume{afdd0a3a-...}\]
  <JUNCTION>  hijo   [\??\Volume{afdd0a41-...}\]
```

`Z:\padre` está **vacío**: el hijo vive en `Z:\hijo`. Medido con `robocopy`:

- `robocopy Z:\padre` → solo `p.txt` y `sub\s.txt`. **Nada del hijo**, y sin usar `/XJ`.
- `robocopy Z:\` sin `/XJ` → baja a todos los datasets.
- `robocopy Z:\` con `/XJ` → ningún dataset; solo directorios del sistema.

O sea: el problema que `-x` resuelve en Unix **aquí no se plantea** al copiar un dataset
concreto, y `/XJ` lo resuelve al copiar desde la raíz.

Efecto lateral del aplanamiento, de OpenZFS y no nuestro: dos datasets con el mismo
último componente —`winpool/a/datos` y `winpool/b/datos`— querrían el mismo `Z:\datos`.
No se ha probado, pero la colisión está en el diseño.

## La decisión de fondo: una implementación o dos

Es la pregunta que gobierna todo lo demás.

- **Una sola implementación nativa, sin rsync en ninguna plataforma.** Un solo
  comportamiento; los mixtos Windows↔Unix dejan de ser un caso especial. **Precio:** en
  `Sincronizar` entre máquinas Unix se pierde el algoritmo de deltas. Un sync de 2 GB con
  tres ficheros cambiados pasa de mover 30 MB a mover 2 GB. Es una regresión real sobre
  lo que hoy funciona.
- **Nativo cuando algún extremo sea Windows, rsync cuando ambos sean Unix.** No se pierde
  rendimiento donde hoy lo hay, pero se mantienen dos caminos —que es exactamente lo que
  ha costado caro en las transferencias—.

**Recomendación: separar por operación, no por plataforma.**

|  | Copia LOCAL (`Desglosar`, `Ensamblar`, `Hacia Dir`, sync misma máquina) | `Sincronizar` ENTRE máquinas |
|---|---|---|
| Qué aporta rsync | poco: `-W` ya desactiva el delta | mucho: el delta ES el motivo |
| Sustituirlo cuesta | asumible | reimplementar rsync |
| Propuesta | **nativo, en las dos plataformas** | decidir después, con datos |

Sustituir la copia y el algoritmo de deltas a la vez es cambiar dos cosas grandes de
golpe en operaciones que borran el origen. Las fases de abajo hacen lo primero y dejan
lo segundo explícitamente fuera.

## Los enlaces duros

Es el único punto donde no hay solución gratis, y hay que decidirlo antes de escribir.

`rsync -H` los conserva; `robocopy` no. Copiados como ficheros independientes, el espacio
se multiplica en silencio y se pierde la identidad del enlace.

**Una implementación nativa sí puede conservarlos**, y no es difícil: se lleva un mapa de
identidad de fichero → primera ruta copiada, y al repetirse se crea un enlace en vez de
copiar. La identidad es el inodo en Unix (`st_dev` + `st_ino`) y en Windows el par
`dwVolumeSerialNumber` + `nFileIndexHigh/Low` de `GetFileInformationByHandle`.

El coste es memoria proporcional al número de ficheros con más de un enlace, no al total:
solo entran en el mapa los que tienen `st_nlink > 1`.

**Decisión propuesta: conservarlos.** Es la diferencia principal entre hacerlo nativo y
usar `robocopy`, y si no se conservan, la implementación nativa pierde buena parte de su
razón de ser frente a llamar a `robocopy` y acabar antes.

## Fases

Cada fase deja el árbol compilando, `ctest` en verde, y se verifica contra máquinas
reales —Linux y Windows— antes de pasar a la siguiente.

### Fase 1 — El verbo de copia, y su verificación

`--mutate-copy-tree <src> <dst> [--one-file-system] [--exclude=<n>]...` y
`--dump-copy-tree-pending <src> <dst> [mismas opciones]`.

Los dos juntos y desde el principio: la verificación no es un extra, es la red que
sostiene las tres operaciones que borran. El verbo de conteo replica la semántica de
`rsync -rni --ignore-existing`: recorre el origen y cuenta lo que **no existe** en el
destino, con las mismas exclusiones que la copia —si no, lo excluido a propósito contaría
como pendiente y la verificación fallaría siempre—.

Recorrido con `std::filesystem`, y por plataforma solo lo que de verdad difiere:

| | Unix | Windows |
|---|---|---|
| permisos | `chmod`/`chown` | ACL con `GetNamedSecurityInfo`/`SetNamedSecurityInfo` |
| marcas de tiempo | `utimensat` | `SetFileTime` |
| enlace duro | `link()` | `CreateHardLinkW` |
| enlace simbólico | `symlink()` | `CreateSymbolicLinkW` |
| disperso | detectar por `st_blocks` | `FSCTL_SET_SPARSE` |
| identidad | `st_dev`+`st_ino` | volumen + índice de fichero |
| no cruzar | comparar `st_dev` | no descender en puntos de reparseo |

**Verificación**: contra un árbol preparado a mano con enlaces duros, un enlace
simbólico, un fichero disperso y un subdirectorio excluido. Comparar recuento, tamaños
aparentes y ocupación real, y que los enlaces duros del destino compartan identidad
entre sí. En Windows, además, que las ACL lleguen.

### Fase 2 — Enganchar las tres operaciones locales

Sustituir `runRsyncCopyMoveCapture` y `countPendingRsyncTransfers` por los verbos nuevos
en `Desglosar` (`:985`), `Ensamblar` (`:1344`, `:1355`, `:1458`) y `Hacia Dir` (`:2927`,
`:2936`). El bucle de copiar-verificar-reintentar de `Ensamblar` se conserva **tal cual**:
cambia quién copia, no la disciplina de comprobar antes de destruir.

**Verificación**: las tres en Linux contra un dataset con contenido real, comparando el
resultado con el que da rsync hoy. Es la única fase donde existe una referencia contra la
que contrastar, y hay que aprovecharla antes de que desaparezca.

### Fase 3 — Encender las tres en Windows

Quitar `DirBreakdown`, `DirAssemble` y `DirToDir` de la lista de pendientes de
`connectioncapabilities.cpp` y de la guarda de `featureRequiredTool` —que hoy las
deshabilita por faltar `rsync`—, y declararlas en `agentCapabilityList()`.

**Ojo con dos cosas que ya han mordido antes:**

- `makeTempDir()` devolvía cadena vacía en Windows y hacía fallar `Desglosar`/`Ensamblar`
  con `rc=125`. Hay una **contradicción sin resolver**: el agente ya las declara en
  `agentCapabilityList()` diciendo que eso está arreglado, mientras la tabla de
  capacidades las sigue dando por pendientes por ese mismo motivo. Uno de los dos miente
  y hay que averiguar cuál **antes** de encender nada.
- Lo que declara el agente **manda** sobre la tabla estática del cliente. Encender solo la
  tabla no sirve de nada.

**Verificación**: las tres desde la interfaz en Windows, con la conexión **Local** como
destino —no solo entre remotas—.

### Fase 4 — `Sincronizar` local

La misma máquina, dos puntos de montaje: es una copia con `--delete` opcional. Reutiliza
todo lo de la fase 1 más el borrado de lo que sobra en el destino.

`buildRsyncLocalPlan` rechaza hoy cualquier ruta que no empiece por `/`
(`daemon_main.cpp:3431`), lo que descarta `C:\...` de entrada. Esa validación se sustituye
por una comprobación de ruta absoluta por plataforma.

### Fase 5 — `Sincronizar` entre máquinas (DECISIÓN PENDIENTE)

Fuera de alcance hasta que las anteriores estén funcionando y haya datos de cuánto pesa
el sync en uso real.

Cuando toque, el transporte **ya existe**: el socket con testigo, doble pila y
reanudación de las transferencias (`--zfs-recv-listen` / `--zfs-send-to-peer`) sirve
igual para un flujo de ficheros. Lo que falta es el protocolo: intercambiar un manifiesto
—ruta, tamaño, marca de tiempo, y hash solo si hace falta desempatar— y mandar enteros
los que difieran. Correcto, y más lento que rsync en árboles grandes con pocos cambios.

## Lo que NO cambia

- **`Copiar` y `Nivelar` snapshot** no usan rsync: van por `zfs send`/`recv` y ya
  funcionan en las dos plataformas. Este trabajo no las toca.
- El respaldo por TAR entre máquinas con un extremo Windows **ya se retiró**: era shell
  POSIX imposible de ejecutar allí.

## Riesgos

- **Estas operaciones borran el origen.** Cualquier duda sobre la verificación se resuelve
  a favor de no borrar. El bucle de reintentos de `Ensamblar` es un buen precedente:
  reintentar es barato, destruir sin comprobar no.
- **Las ACL de NTFS no tienen equivalente POSIX.** Copiar dentro de la misma plataforma es
  correcto; lo que no se puede es *traducir* entre modelos. Si el origen y el destino son
  plataformas distintas, hay que decidir qué se pierde y **decirlo**, no inventar una
  correspondencia.
- **Reimplementar una copia es asumir una responsabilidad que hoy tiene un programa muy
  probado.** El recorrido es fácil; los casos límite —enlaces, dispersos, nombres
  imposibles, rutas largas de Windows (`\\?\`), permisos que impiden leer— no lo son. Ahí
  es donde debe caer el esfuerzo de pruebas, no en el camino feliz.
