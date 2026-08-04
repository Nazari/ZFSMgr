# Windows: un solo camino, el agente nativo

## Por qué

El soporte de Windows se construyó cuando la aplicación funcionaba enviando **tuberías
de shell Unix** (`zfs list | awk ...`). Windows no las entiende, así que se añadió
MSYS2: un `bash` instalado en el equipo remoto que las ejecutara. Fue razonable con las
premisas de entonces.

Esas premisas dejaron de sostenerse cuando el trabajo del daemon fue justo en la
dirección contraria: **dejar de enviar shell**. Las órdenes se convierten en llamadas
RPC tipadas que el agente ejecuta con `execvp`, sin intérprete. Y el agente nativo
compila y funciona en Windows.

El síntoma que lo destapó fue pequeño y muy elocuente: el agente **nativo** de Windows
se estaba invocando *a través del bash de MSYS2*, que al quitar comillas se comía las
barras invertidas de su propia ruta.

## Qué se decidió

1. **Windows es solo SSH.** `PSRP` no admite daemon: el RPC viaja por un túnel `ssh -L`
   y sin SSH no hay túnel. Mantenerlo obligaba a conservar indefinidamente el camino de
   shell, además de una segunda implementación del agente —un stub de PowerShell—.
   Windows 10 y 11 traen OpenSSH de serie.
2. **Windows depende solo del agente.** MSYS2 desaparece como capa de ejecución, junto
   con el modo `UnixShell`, la heurística que decidía a qué modo mandar cada orden, y
   la sonda de capa Unix.
3. **Lo no portado se deshabilita indicando el motivo**, en vez de intentarse y fallar
   de forma opaca.

PowerShell queda reducido a lo que ocurre **antes** de que exista el canal: sondear si
el agente está, instalarlo, arrancarlo y leer su material TLS.

## Matriz de capacidades

La decisión de si una función está disponible vive en un único sitio,
`src/connectioncapabilities.{h,cpp}` (`zfsmgr::caps`), con una función pura y su test.
Distingue tres motivos que antes se confundían:

| Motivo | Significado |
|---|---|
| `WindowsAgentPending` | el agente todavía no lo implementa; cabe esperar una versión futura |
| `WindowsNotApplicable` | no tiene sentido en Windows; no es falta de trabajo |
| `DaemonNotReady` / `DaemonApiMismatch` | el agente no está listo, con independencia del sistema |

Esa distinción importa al usuario: le dice si esperar o no esperar.

### La tabla no manda: manda el agente

`--health` publica `CAPS=` con los verbos que el binario sirve de verdad, calculado con
los **mismos `#ifdef`** que deciden qué se compila, así que no puede mentir. El cliente
lo consume y prevalece sobre la tabla estática.

El motivo es concreto: una tabla escrita en la GUI se desincroniza en cuanto alguien
porta un verbo, porque quien lo porta toca `daemon_main.cpp`, no el cliente.

### Estado en 0.90.6

Disponible en Windows: lectura y modificación de datasets y pools, instantáneas,
clonar, **permisos ZFS**, *Desglosar*, *Ensamblar*, registro y latido del agente.

No disponible, y por qué:

| Función | Motivo |
|---|---|
| Trabajos en segundo plano | dependen de `fork`/`waitpid` y de `/dev/urandom` |
| Transferencias entre daemons | dependen de los trabajos |
| Copiar/Nivelar entre máquinas | necesitan transmitir el flujo por tuberías `pipe()` |
| Sincronizar con `rsync` | `rsync` no viaja con el agente |
| *Hacia Dir* | usa el baile de guardar/restaurar `mountpoint`, sin equivalente contra letras de unidad |
| Reparar mountpoints temporales | igual que la anterior |
| Instantáneas automáticas | el planificador se apoya en eventos de ZED, y OpenZFS on Windows no trae `zed` |

## Huecos del daemon que hubo que tapar

Varios verbos **aparentaban** estar disponibles en Windows y fallaban siempre, que es
peor que no tenerlos:

- `detectOsLine()` invocaba `uname`, así que la línea de sistema era literalmente el
  mensaje de error de `cmd`. Ahora se lee del registro.
- `detectMachineUuid()` devolvía vacío, con lo que la deduplicación de conexiones por
  máquina no podía funcionar. Ahora lee `MachineGuid`.
- `makeTempDir()` devolvía cadena vacía y sus llamantes no lo comprobaban: *Desglosar*
  y *Ensamblar* fallaban **siempre** con `rc=125`, a mitad de una operación
  destructiva. Ahora usa `GetTempPath` con un GUID por nombre, porque el daemon atiende
  varias conexiones a la vez.
- `runExecCapture()` usaba `_popen` con `2>&1`: stderr se fusionaba en stdout y `err`
  salía siempre vacío. Se sustituyó por `CreateProcess` con dos tuberías, lo que además
  eliminó el apaño de comillas que hacía falta porque `cmd.exe` se come la primera y la
  última de la línea.
- `kHeartbeatPath` apuntaba a `/tmp`, que no existe: el `ofstream` fallaba y la función
  retornaba en silencio.

## Sin respaldo por shell

Desde 0.90.6 **no hay camino alternativo**: si el agente no está disponible, la
operación no se intenta y se dice por qué. Antes, cada lectura y cada mutación tenían
un `zfs`/`zpool` clásico de reserva, elegido en 67 puntos distintos.

El motivo de quitarlo no es la simplificación —aunque desaparecen unas 60
construcciones dobles de comando—, sino que **el respaldo ocultaba fallos reales del
agente durante meses**:

- En macOS el agente no tenía "Acceso total al disco" y no veía ningún pool
  importable. La aplicación repetía la sonda por shell, que sí los veía, y todo
  parecía correcto. Se descubrió solo al reinstalar el agente, cuando el sistema pidió
  el permiso.
- En Windows, exportar e importar pools se ejecutaban por shell mientras en Unix iban
  por RPC. Nadie lo notó porque el resultado era el mismo.

Donde el agente no ve algo que el shell sí vería, ahora se explica en el registro con
la acción concreta que lo arregla, en vez de compensarlo en silencio.

Lo que **no** es respaldo y sigue existiendo:

- El arranque en frío: detección del sistema, sonda de presencia del agente e
  instalación. Ocurre antes de que exista el agente, así que no puede depender de él.
- El agente en modo línea de comandos sobre SSH, para los cuatro verbos `cli-only`
  que transportan flujos por la entrada y la salida estándar. El canal RPC no lleva
  stdin.

## Pendiente: las credenciales salientes del agente

La matriz de conectividad comprueba si una máquina llega por SSH a otra, y eso sigue
haciendo falta: las transferencias entre máquinas y las instantáneas programadas con
destino remoto las abre el **agente del origen** contra el destino.

Pero la sonda se ejecuta como el usuario de la sesión SSH, y quien abrirá esa conexión
es el agente, que corre como `root` o `SYSTEM` **con otras credenciales**. La tabla
puede salir en verde y la transferencia fallar igual. Desde 0.90.7 el diálogo lo dice,
que es lo barato; lo correcto sería un verbo del daemon —`--probe-peer <host>`— que
sondee con las credenciales que realmente va a usar.

El trabajo no es el verbo, es la pregunta que hay debajo: **qué credenciales usa el
agente para salir**. Hoy no está definido en ninguna parte. Conviene decidirlo aparte y
no de pasada, porque afecta a cómo se instala y se configura el agente en cada máquina.

## Lo que no cubre ningún test

**No hay cobertura automatizada de Windows.** Los cuatro binarios de prueba se ejecutan
en el equipo de desarrollo y no ejercitan ni el agente de Windows ni su transporte.

Lo que sí hay desde 0.90.6 es un **transporte de mentira**
(`MainWindow::setAgentTransportForTest`). Mientras está puesto no se abre ninguna
conexión: las órdenes por argv van a la función que se le pase y las que salgan como
cadena de shell se registran y fracasan. Con eso un test puede afirmar tres cosas que
antes no se podían comprobar sin una máquina remota:

- qué verbo y qué argumentos exactos recibe el agente;
- que un argumento hostil —con `&`, `;` o `|`— llega entero;
- que **no** se intenta nada por shell, ni cuando el agente responde bien ni cuando
  falla.

Ese último es el que impide que reaparezca un respaldo. Está comprobado por mutación:
reintroducir a mano un `runSsh` de reserva en `getDatasetProperty` hace fallar
`failingAgentCallDoesNotFallBackToShell`, y cambiar el verbo hace fallar
`datasetPropertyReadGoesToTheAgentByArgv`.

Sigue sin cubrir el agente real: que el binario de Windows responda lo que se espera
solo lo valida una máquina Windows con OpenZFS y un pool.

Dos consecuencias prácticas:

- Cada cambio en este terreno debe **cruzarse para Windows** además de compilarse para
  Linux. Durante este trabajo el cruce destapó una regresión que el build de Linux no
  podía ver: una referencia dentro de un bloque `#ifdef Q_OS_WIN`.
- Lo que valida de verdad es una máquina Windows real con OpenZFS y un pool importado.
