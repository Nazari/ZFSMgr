# Transferencias en Windows: Copiar y Nivelar

## Por qué

`Copiar` y `Nivelar` snapshot están apagadas en Windows. El motivo declarado —y el que
lee el usuario en el menú— es que necesitan encadenar `zfs send | zfs recv` por una
tubería, y el agente de Windows no lo implementa.

La premisa que se arrastraba era más pesimista de lo que correspondía: se daba por hecho
que **Windows no podía con las tuberías**, y que por tanto había que esperar a que el
daemon escuchara por un socket propio antes de poder ofrecer nada. Se midió contra una
máquina real y esa premisa es falsa. Lo que falla es mucho más estrecho, y saber
exactamente qué falla es lo que hace que este trabajo sea abordable.

## Lo que se midió

Todo contra OldLau (`zfswin-2.4.1rc14`), con pool desechable, el 2026-08-13. Cada fila
se comprobó con suma MD5 de punta a punta, no por el código de salida.

| Camino | Resultado |
|---|---|
| Unix→Win: `zfs send \| ssh win "zfs recv"` | falla a los **135.168 B** |
| Win→Unix: `ssh win "zfs send"` > fichero | truncado a **134.784 B**, rc=1 |
| Win, tubería local: `type f \| zfs recv` | **OK**, MD5 idéntico |
| Win, tubería local: `zfs send \| zfs recv` | **OK**, MD5 idéntico |
| Win: `zfs recv < f` y `zfs send > f` (cmd) | **OK** en ambos sentidos |
| SFTP, subida y bajada de 8 MB | **OK** |
| Túnel `ssh -L`, 8 MB | **OK, 0,6 s**, MD5 idéntico |
| Fragmentos con `recv -s` (20 MB en 4) | **OK**, MD5 idéntico |

Los dos únicos fallos se cortan en el mismo sitio, **132 KiB exactos**, en direcciones
opuestas. No es cosa de `zfs.exe`, ni del flujo, ni del transporte SSH.

## La regla, y la consecuencia que no es obvia

> En Windows, el **stdio de un comando lanzado por `ssh host "orden"`** no sirve para
> datos masivos. Se corta a 132 KiB. Todo lo demás funciona.

De ahí se sigue algo que conviene tener presente **antes** de escribir código, porque de
otro modo se descubre a mitad de la implementación:

**No hay que entregarle el socket a `zfs.exe` como entrada estándar.** La versión POSIX
de `--zfs-recv-listen` hace exactamente eso: acepta la conexión y llama a
`dup2(clientFd, STDIN_FILENO)` antes del `execvp` (daemon_main.cpp, sobre la línea 5285).
Es elegante y en Unix es gratis. Pero el descriptor que `sshd` entrega en Windows —el que
sí sabemos que `zfs.exe` no digiere— es justamente de esa naturaleza, así que hay motivo
fundado para esperar el mismo fallo.

La versión de Windows debe **bombear**: el daemon lee del socket y escribe en una tubería
anónima (`CreatePipe`) que sí alimenta a `zfs recv`. Cuesta un hilo y un búfer, y es lo
que la medición dice que funcionará.

## Lo que ya existe y no hay que inventar

Esto es lo que cambia el tamaño del trabajo:

- **`--zfs-recv-listen`** (daemon_main.cpp:5285) y **`--zfs-send-to-peer`** (:4640) ya
  están implementados y en uso entre daemons Unix. `socket()`, `fork()`, `dup2()`,
  `execvp()`. No son verbos por diseñar: son verbos por portar.
- **El cliente ya los orquesta**: `tryBuildDaemonToDaemonCopyPipeline`
  (mainwindow_transfer.cpp) negocia el testigo, arranca el receptor y lanza el emisor.
  Hoy se corta con un `if (isWindowsConnection(...)) return {};`.
- **La reanudación ya está contemplada**: el camino existente recibe con `zfs recv -Fus`,
  con la `-s` puesta. La misma que hace posible reanudar por fragmentos.
- **La interfaz ya sabe explicarse**: la tabla de capacidades
  (`src/connectioncapabilities.{h,cpp}`) modela el motivo `WindowsAgentPending`, y desde
  la 0.90.17 el menú deshabilita con el motivo en el tooltip en vez de dejar pulsar y
  rechazar después.

## La pieza que falta

Una, y bien delimitada: **el agente no tiene ningún camino de entrada estándar en
Windows**. `runExecCaptureWithStdin` (daemon_main.cpp:1931) es POSIX puro —`pipe()`,
`fork()`, `execvp()`— y sus llamadas están dentro de `#ifndef _WIN32` (líneas 5185, 5199
y 6920).

Escribir esa variante con `CreateProcess` no solo desbloquea las transferencias: también
desbloquea `--mutate-zfs-load-key` y `--mutate-zfs-change-key`, que hoy están apagados
por el mismo motivo y no tienen nada que ver con transferir.

## Dos diseños, y cuál conviene

**A — Fragmentos por fichero.** El emisor genera trozos acotados, viajan por SFTP, el
receptor los consume con `zfs recv -s` y devuelve el testigo de reanudación. Validado de
punta a punta (20 MB en cuatro fragmentos). Pico en disco: un fragmento.

**B — Portar los dos verbos.** `--zfs-recv-listen` y `--zfs-send-to-peer` en Windows, con
`CreateProcess` + `CreatePipe` + bombeo entre socket y tubería. Sin fichero intermedio.

La intuición dice que A es «lo pequeño» y B «lo ambicioso». **Es al revés**, y conviene
verlo antes de empezar:

- **A necesita maquinaria nueva en el cliente** que hoy no existe en ninguna parte: bucle
  de fragmentación, cliente SFTP, sondeo del testigo, y una máquina de estados que
  sobreviva a que el usuario cierre la aplicación a mitad.
- **B reutiliza todo lo del cliente** y se limita a dos funciones del daemon, con la
  forma ya escrita al lado en su versión POSIX.

Además el túnel mueve 8 MB en 0,6 s, así que la vía B no tiene un problema de
rendimiento que justifique el rodeo.

**Recomendación: B.** A queda como plan de respaldo si el bombeo se atasca contra algo
imprevisto, y como opción para entornos sin túnel disponible. Esto **corrige** la
recomendación que di antes de mirar el código existente, cuando aún creía que los verbos
había que escribirlos desde cero.

## Fases

Cada fase deja el árbol compilando, `ctest` en verde, y se verifica contra la máquina
Windows antes de pasar a la siguiente.

### Fase 0 — Ejecutar con entrada y salida redirigidas en Windows

Variante Windows de `runExecCaptureWithStdin` con `CreateProcess`, tuberías anónimas
heredables y lectura concurrente de salida y error (leer una después de otra se bloquea
en cuanto una llena su búfer).

**Verificación**: `--mutate-zfs-load-key` sobre un dataset cifrado en Windows. Es la
prueba más barata del primitivo y además arregla una función real de paso.

### Fase 1 — `--zfs-recv-listen` en Windows

Socket de escucha con Winsock (el daemon ya hace `WSAStartup`), y al aceptar: crear la
tubería, lanzar `zfs recv -Fus <dataset>` con el extremo de lectura como entrada, y
bombear del socket a la tubería hasta el fin de datos. **Nada de pasarle el socket al
proceso hijo.**

**Verificación**: recibir en Windows un stream de 8 MB desde Linux, con MD5 idéntico. Ya
existe el procedimiento manual con el que contrastar.

### Fase 2 — `--zfs-send-to-peer` en Windows

Simétrica: lanzar `zfs send` con la salida a una tubería, leer de ella y escribir al
socket. Esta cara es más directa porque la versión POSIX ya bombea (usa `pipe()` y relé,
no `dup2` sobre el socket), así que la estructura se traslada casi tal cual.

**Verificación**: enviar desde Windows a Linux, MD5 idéntico. Ya está comprobado que
`zfs send` escribe bien en una tubería local en Windows.

### Fase 3 — Encender la función

Quitar el `if (isWindowsConnection(...)) return {};` de
`tryBuildDaemonToDaemonCopyPipeline` y retirar `SendRecvStreaming` de la lista de
pendientes de Windows en `connectioncapabilities.cpp`.

Mejor aún: que **el daemon declare sus capacidades** en `--health` (`CAPS=`) y que la
tabla estática solo actúe como respaldo. Es lo que ya prevé el comentario de
`Platform::daemonCaps`, y evita que cliente y agente se desincronicen en cuanto se porte
cualquier otro verbo.

**Verificación**: con el agente nuevo, `Copiar aquí desde...` aparece **habilitada** en el
menú contextual sin tocar el cliente. Con un agente viejo, sigue en gris con su motivo.

### Fase 4 — Reanudar de verdad

El camino ya recibe con `-s`, así que ante un corte queda un testigo. Falta la parte
visible: detectarlo, decirlo en lugar de un error crudo, y ofrecer continuar.

**Verificación**: cortar la red a mitad de una transferencia grande y reanudarla.

### Fase 5 — Trabajos en segundo plano (decisión pendiente)

`Copiar` se ejecuta hoy **como trabajo**, y `BackgroundJobs` no está en Windows
(`fork`/`waitpid`). Hay dos salidas y conviene elegirla antes de la fase 3, no después:

1. Ejecutar `Copiar` de forma síncrona en Windows. Más simple; bloquea la interfaz en
   transferencias largas.
2. Portar el modelo de trabajos con `CreateProcess` y seguimiento por handle. Más caro,
   pero es la pieza que también necesitan otras funciones.

## Lo que sigue fuera, y por qué

Conviene que quede escrito para no reabrirlo cada vez:

| Función | Por qué no la desbloquea este trabajo |
|---|---|
| **Sincronizar (rsync)** | No hay `rsync` en Windows y `buildRsyncLocalPlan` rechaza rutas que no empiecen por `/`. Es reimplementar la copia, no transportar un flujo. |
| **Hacia Dir** | Igual, y además rsync es quien **verifica antes de borrar el origen**. Esa verificación es lo que lo hace seguro. |
| **Desglosar / Ensamblar** | Dos bloqueos: `makeTempDir()` devuelve cadena vacía en Windows (de ahí el `rc=125`), que se arregla con `GetTempPath`; pero después siguen necesitando rsync. |
| **Instantáneas automáticas** | Se apoyan en ZED, y OpenZFS on Windows no lo trae. No depende de nosotros. |
| **Montaje alternativo / Reparar mountpoints** | Declaradas «no aplica»: contra letras de unidad no significan lo mismo. No es trabajo pendiente. |

## Riesgos y cabos sueltos

- **El bombeo es la apuesta central.** Si `zfs.exe` también se atragantara con una tubería
  anónima alimentada por el daemon —cosa que la medición de `type f | zfs recv`
  desaconseja, pero que no es exactamente el mismo montaje—, habría que caer al diseño A.
  Es la primera cosa que debe probar la fase 1, con un stream pequeño, antes de escribir
  el resto.
- **Streams largos sin medir.** Todo lo comprobado son transferencias de segundos. Un
  envío de horas puede sacar problemas de tiempo de espera, de memoria o de reconexión que
  aquí no aparecen.
- **`zpool create` sigue roto en Windows**, también sobre fichero
  (`no such device in \\?\`). No afecta a esto, pero limita las pruebas: los pools de
  prueba hay que crearlos fuera e importarlos.
