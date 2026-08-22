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

> **Nota (0.90.6).** `PSRP` se retiró como tipo de conexión: no admite el daemon,
> porque el RPC viaja por un túnel `ssh -L` y sin SSH no hay túnel. Los perfiles
> guardados con `PSRP` se convierten a `SSH` al cargarlos, reponiendo el puerto 22.

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

## Los secretos no viajan por la línea de órdenes

**La regla**: una contraseña llega a un proceso hijo por **descriptor** o por **terminal**.
Nunca por argumento ni por variable de entorno. El argv de cualquier proceso lo lee todo
el mundo con `ps`; el entorno se lee en `/proc/<pid>/environ`.

Estaba incumplida en seis sitios, todos con la misma forma `sshpass -p <contraseña>`:
`base/helpers.cpp` (scp), `base/transportrpc.cpp` (dos), `base/transporttunnel.cpp`, y
`connectiondialog.cpp` (dos). `sshpass` borra su propio argv nada más arrancar —por eso en
`ps` se ven espacios donde estaba la contraseña— pero entre el `exec` y ese borrado hay
una ventana real, y basta con mirar en el momento justo.

La pieza es `helpers::SecretoPorDescriptor`: monta una tubería, escribe el secreto, cierra
el extremo de escritura y deja el de lectura para que lo herede el hijo, que lo recibe como
`sshpass -d<N>`. Dos detalles que no son opcionales:

- **Una tubería se lee UNA vez.** `runRemoteCommand` reintenta sin multiplexado, así que el
  descriptor se crea DENTRO del intento, no fuera. Con uno solo creado fuera, el segundo
  intento leería una tubería vacía y la autenticación fallaría sin explicar por qué.
- **El descriptor se deja sin CLOEXEC a propósito**, justo al revés que los sockets: aquí
  la herencia es el mecanismo, no el fallo.

Con QProcess no se puede elegir el número del descriptor desde el padre, y el argumento
`-d<N>` lo lleva dentro. Se resuelve con `setChildProcessModifier`, que corre ya en el hijo
entre el `fork` y el `exec`: ahí se duplica el descriptor real sobre el 3 fijo.

Si la tubería no se puede montar, **no se cae al método viejo**: se lanza `ssh` a secas y
que falle por credenciales. Fallar con un mensaje entendible es preferible a publicar la
contraseña en la tabla de procesos.

Comprobado en vivo: 1200 muestreos de `ps` durante conexiones nuevas a dos máquinas con
contraseña; `sshpass -d6 ssh -o BatchMode=no …` y **cero** apariciones de la contraseña.

### Y los cuatro que faltaban, también

Los cuatro sitios que metían la contraseña del destino dentro de una orden de shell ya no
existen:

- `mainwindow_transfer.cpp:481` y `:1387` **desaparecieron con su código**: eran los
  respaldos por shell de Copiar y Nivelar, retirados al cerrar el plan de transferencias.
- `mainwindow_connections.cpp:724` y `:747` —las dos pruebas de la matriz de
  conectividad— pasan de `SSHPASS=<contraseña> sshpass -e` a **`sshpass -d0`**, con la
  contraseña como primera línea de la entrada estándar del ssh exterior.

Para lo segundo, `fetchConnectionProbeOutput` recibe ahora una carga de entrada estándar y
se la pasa a `runSsh`, que ya la admitía. Quien la aporta es el llamante, que es quien tiene
el perfil del destino.

Comprobado a mano contra máquinas reales: `sshpass -d0` leyendo de la entrada estándar
autentica y devuelve `ZFSMGR_CONNECT_OK`. (En una máquina sin `sshpass` la orden cae en su
rama de siempre y lo dice.)

### Lo que queda (no está arreglado)

Nada de lo de arriba. Queda pendiente el **rsync entre máquinas**, que necesita pasarle a
rsync una orden ssh por `-e`; con una conexión por contraseña, esa orden tendría el mismo
problema. Se trata en `docs/diseno_tecnico_transferencias.md`.

## Guiones de shell: cuántos quedan y por qué

Distinto problema que el de las contraseñas, aunque se toque el mismo código. Aquí no se
trata de qué se ve en `ps`, sino de **cuánta lógica del programa vive escrita en un
lenguaje que no compila nadie**: un guion de `sh -lc` no lo revisa el compilador, no lo
cubre ningún test, y se comporta distinto según el shell que haya en el otro extremo.

Tres se han retirado y están medidos.

### Qué sistema corre el otro extremo

Había **tres** implementaciones de la misma pregunta, y no coincidían:

| Dónde | Cómo |
|---|---|
| `daemon_main.cpp:detectOsLine()` | abría `/etc/os-release` y lo parseaba en C++ |
| `connectiondialog.cpp` | `sh -lc '. /etc/os-release; printf "%s %s" "$NAME" "$VERSION_ID"'` |
| `mainwindow_refresh.cpp` | el mismo guion, copiado |

Las dos de Qt delegaban en el intérprete remoto un trabajo que la del daemon ya hacía sin
él. Y discrepaban en los bordes: aquel `printf` no quitaba las comillas que el formato
admite —`NAME="Fedora Linux"`— y dejaba un **espacio de cola** cuando no hay `VERSION_ID`.

No es teórico. Contra `unib.local`, que es Arch:

```
guion viejo:  [Arch Linux ]
ahora:        [Arch Linux]
```

Arch y Gentoo no traen `VERSION_ID`, así que ese espacio llegaba a la ficha de la conexión
en cualquier máquina de esa familia.

El parseo pasa a `base/sistemaoperativo.{h,cpp}` —`deOsRelease` y `deSystemProfiler`— y lo
usan los tres. Por el cable viaja ahora `cat /etc/os-release`: un mandato, no un guion. La
parte de macOS se comprobó contra `mmela.local` (macOS 26.5.2), donde el resultado es
idéntico al del guion que sustituye; ahí la ganancia es solo tener una copia en vez de dos.

### Matar el árbol de procesos al cancelar

`MainWindow::terminateProcessTree` era un guion que llamaba a `pgrep -P` una vez por
proceso y por cada uno de **ocho** niveles, y remataba con `sleep 0.3` y dos bucles de
`kill`.

El tope de ocho no era una precaución: era una pérdida. Medido sobre una cadena real de
procesos anidados:

```
guion viejo, tope de 8 niveles: alcanza 8
recorrido nuevo, sin tope:      alcanza 13
sobrevivirían al viejo:          5
```

Cinco procesos vivos tras cancelar es exactamente el fallo que el comentario original
describía —el `tar` que sigue escribiendo y deja el punto de montaje ocupado—, solo que la
corrección de entonces se quedó corta.

Ahora es `base::mataDescendencia`: **una** lectura de `ps -eo pid=,ppid=` y `kill()`
directo, que es una llamada al sistema y no un proceso. El recorrido —`descendientesDe`,
que ordena de hojas a raíz— está separado de la ejecución para poder probarlo con una
salida de `ps` escrita a mano, que es lo único de esto comprobable sin matar procesos.

### La tubería de la nivelación GSA

`gsaLevelSnapshot`, en la rama de misma máquina, montaba `zfs send … | sh -lc 'sudo -n sh
-lc "zfs recv …"'` con `std::system()`. Su propio comentario decía que el intérprete era lo
que quedaba por quitar allí.

No hacía falta ninguno: el daemon ya monta esa tubería con `pipe()` y dos `execvp` para
servir `--zfs-pipe-local`. Se extrajo `ejecutaTuberiaZfsLocal(sendArgv, recvArgv)` y ahora
la usan los dos. El `sudo` desaparece sin echarse en falta: ese código **ya corre como
root** dentro del daemon, así que envolverse en sudo era pedir un permiso que ya se tiene.

Con esto **no queda ninguna llamada a `std::system` en el daemon**.

Comprobado en vivo sobre un pool de pruebas, los tres caminos: envío completo, incremental
—que es donde `-i @base` pasa a ser dos elementos de argv en vez de un trozo de cadena— y
recursivo, con `-wLecR` y `-u -s`.

### Un hallazgo de paso: la nivelación local no se podía disparar

Para probar lo anterior hubo que descubrir que `gsaYoMismo()` lee la clave `self` de
`/etc/zfsmgr/peers.json`, y **el fichero de esta máquina no la tenía**. Sin ella
`selfConn` sale vacío, ningún destino se reconoce como propio, y la rama de misma máquina
no se ejecuta jamás: se cae a la remota y se registra «no hay credenciales del par», que
es un mensaje desconcertante cuando el par es uno mismo.

La escribe `cli/shell.cpp` al entregar credenciales con `--mutate-set-peers`, pero es
añadida reciente: los `peers.json` ya desplegados no la llevan. Se arregla volviendo a
entregar credenciales desde el CLI. Queda dicho aquí porque el síntoma no señala la causa.

### Lo que sigue habiendo (10 sitios), y por qué

- `daemon_main.cpp:3541` — `--mutate-shell-generic`, que ejecuta shell arbitrario como
  root. **No se puede borrar sin más**: respalda la orden libre de la lista de pendientes,
  que el usuario escribe a mano, y el plan B de las transferencias. Es el final del plan
  por fases, no un barrido.
- `daemon_main.cpp:7869` — el `popen` de `zpool events -f`. La cadena es **constante**: no
  entra ningún dato de nadie, así que no hay superficie de inyección. Lo que compra el
  shell es `stdbuf -oL` con respaldo y la redirección de errores.
- `base/process.cpp:105` — solo Windows, donde `std::system` es `cmd.exe /c`. El argv se
  entrecomilla antes; el fichero ya tiene la maquinaria de `CreateProcess` que lo
  sustituiría.
- `base/transporttunnel.cpp` (×2), `base/transportrpc.cpp:469`, `cli/shell.cpp` (×2) —
  guiones de aprovisionamiento e instalación que corren en el otro extremo antes de que
  haya daemon con quien hablar. Son justamente los que no pueden ir por RPC.
- `mainwindow_filebrowser.cpp:182` — el explorador de ficheros remoto.
- `base/daemonpayload.cpp:237` — dentro de la carga de instalación.
