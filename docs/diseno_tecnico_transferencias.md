# Las transferencias, fuera de la interfaz

## Por qué, y por qué ahora

Las cuatro acciones que mueven DATOS entre dos extremos —**Copiar**, **Mover**,
**Sincronizar** y **Nivelar**— son lo único grande que la interfaz de Qt sabe hacer y el
servidor web no. No es que falten verbos en el daemon: **los nueve que hacen falta ya
existen** y están servidos por RPC.

    --zfs-send-to-peer        --zfs-recv-listen        --job-submit
    --mutate-copy-tree        --dump-copy-tree-pending --mutate-rsync-local
    --mutate-sync-temp-tar-source  --mutate-sync-temp-tar-dest  --dump-zfs-diff

Lo que falta es la ORQUESTACIÓN: quién envía, quién recibe, por qué camino, qué se hace
cuando ese camino no se puede montar, y cómo se reanuda lo que se cortó. Eso vive hoy dentro
de `src/mainwindow_transfer.cpp`, que enlaza Qt, así que el servidor web no puede llamarlo.

**Este documento existe para decidir tres cosas antes de escribir código**, porque las tres
son irreversibles en la práctica: qué se baja y qué se queda, quién decide el camino, y
quién sostiene una transferencia que dura horas.

## Lo medido, no lo estimado

`src/mainwindow_transfer.cpp` son **2.554 líneas**. Repartidas así:

| Función | Líneas | Tocan un tipo de Qt | Diálogos y registro |
|---|---|---|---|
| `actionSyncDatasets` | 676 | 271 (40%) | 28 |
| `actionLevelSnapshot` | 402 | 165 (41%) | 23 |
| `actionCopySnapshot` | 400 | 128 (32%) | 24 |
| `launchDaemonJobTransfer` | 122 | | |
| `transferResumeTokenFor` | 57 | | |
| el resto (trabajos, huérfanos, lista) | ~900 | | |

**El dato que decide el diseño está en la última columna.** De las 400 líneas de Copiar,
solo **24** son diálogos y registro; las otras 376 son decisión. Lo que parece «código de
interfaz» no lo es: es lógica escrita con `QString` porque estaba donde estaba. Y de
dependencias de la ventana solo usa **una**, `m_conns`, que es la lista de conexiones y ya
tiene equivalente sin Qt.

Es decir: **esto no es un rescate difícil, es uno grande.** No hay que desenredar la lógica
de la interfaz —están casi separadas ya— sino traducir mucho `QString` a `std::string` sin
cambiar ninguna decisión por el camino.

## Decisión 1: se baja la ORQUESTACIÓN, no las acciones

Lo que baja a `src/base/transferencia.{h,cpp}` es la máquina de decidir:

- qué camino se puede usar entre estos dos extremos y en qué orden se prueban;
- qué argv le toca a cada lado;
- qué significa lo que contesta cada uno;
- desde dónde se reanuda.

Lo que NO baja, y se queda en cada cliente:

- **preguntar**. La confirmación con sus casillas es de Qt; en la web es un formulario; en
  el intérprete es un «¿seguro?». Son tres cosas distintas y deben serlo.
- **contar lo que pasa**. Una barra de progreso, una línea de registro y una tabla HTML no
  se parecen en nada.
- **las credenciales**. Ya está resuelto: `TransportSession` las lleva.

La frontera se dibuja donde ya está: las 24 líneas de diálogo por función se quedan arriba.

## Decisión 2: los caminos, y el orden importa

> **Corregido al implementar la fase 0.** Aquí decía «tres caminos» con el respaldo por tar
> dentro, y leyendo el código resultó que no es así: Copiar **no tiene respaldo por tar**
> —cuando no hay tubería que montar, se para y lo dice— y el tar es cosa de Sincronizar,
> que mueve ficheros. También faltaba un camino entero. Se deja escrito el error porque es
> justo para lo que sirve escribir el diseño antes: el que venga detrás no tiene por qué
> volver a leer 2.554 líneas para descubrir lo mismo.

Los caminos de Copiar y Nivelar son estos, en orden de preferencia:

**1. Trabajo asíncrono** (`--job-submit`). Lo lanza el cliente y lo **sostiene el daemon**:
sobrevive a que se cierre la ventana. Necesita daemon en los dos extremos y que los dos
declaren `JOBS_SUPPORT=1`.

**2. Daemon a daemon** (`--zfs-recv-listen` + `--zfs-send-to-peer`). El receptor abre un
puerto y el emisor se conecta. Sin shell y sin que los bytes pasen por el cliente, pero lo
sostiene quien lo lanzó. Necesita daemon en los dos. Dos detalles que ya costaron sangre y
que hay que llevarse enteros:

- **El receptor escucha en IPv6 con `V6ONLY` desactivado**, que acepta también IPv4. Con
  `AF_INET` a secas, dos máquinas que se hablan por IPv6 fallaban con «cannot connect to
  peer» — el emisor recibía una dirección `fe80::…%enp1s0f0` y allí no escuchaba nadie.
- **La dirección de vuelta se le PREGUNTA al origen**, no se deduce. `$SSH_CLIENT` la trae
  puesta. Una máquina puede tener varias interfaces, estar tras NAT o llegar por VPN, y solo
  el otro extremo sabe por dónde entró.

**3. Tubería por SSH**: `ssh origen 'zfs send' | ssh destino 'zfs recv'`, en tres variantes
—misma conexión, remoto a remoto directo, o pasando por el cliente—. **No necesita daemon en
ningún extremo**, y ese es el dato que se me había escapado: una copia entre dos máquinas sin
agente sigue siendo posible. Por eso este camino siempre está en la lista.

**Y con un extremo Windows no hay ninguno.** No es que se caiga a otro: los dos primeros
necesitan transmitir por una tubería y el agente de Windows no lo hace, y el tercero es un
guion de shell POSIX que allí no puede ejecutarse desde que se retiró MSYS2. Encolarlo
igualmente hacía que PowerShell devolviera su objeto de error en XML, y el usuario veía un
`<Objs Version="1.1.0.1">…` sin relación aparente con la copia que había pedido.

**Sincronizar no es de esta familia.** Usa `rsync` (48 menciones) y `tar`, no `zfs send`.
Copia FICHEROS, no el dataset; no reanuda con testigo, no necesita instantánea común, y sus
modos de fallo son otros. Módulo aparte: meterlo aquí obligaría a que cada decisión
preguntara «¿y si es rsync?».

## Decisión 3: la reanudación se lleva tal cual, con su comentario

`transferResumeTokenFor` son 57 líneas y una de ellas vale el módulo entero:

> Las copias van con `-R`, o sea toda la jerarquía en un solo flujo. Al cortarse, ZFS deja
> el testigo en el dataset que estaba recibiendo en ese momento, que **casi nunca es la
> raíz**: medido cortando una copia de 3,4 GB, el padre quedó completo y el testigo apareció
> en el hijo. Mirar solo la raíz daba «no hay nada que reanudar» teniendo 247 MB ya
> transferidos.

Ese párrafo se copia con el código. Es la clase de detalle que se pierde al reescribir y
que cuesta otra tarde volver a descubrir.

## Decisión 4: quién sostiene una transferencia de horas

Aquí está la diferencia real entre los clientes, y es lo que más hay que pensar.

- **La interfaz** la sostiene ella: la ventana está abierta y hay una barra de progreso.
- **El servidor web NO puede.** Atiende de una en una y una petición HTTP no puede durar
  cuatro horas: el navegador se cansa, y mientras tanto el servidor no atiende a nadie más.

Por eso la web **solo puede usar el camino asíncrono**: `--job-submit`, que deja el trabajo
corriendo en el daemon y devuelve un identificador. La pestaña «Transferencias» que ya
existe es justo la que lo enseña. `launchDaemonJobTransfer` (122 líneas) ya hace eso en la
interfaz.

**Consecuencia que hay que aceptar de entrada**: la web ofrecerá copiar y nivelar **solo
cuando los dos extremos tengan un daemon que admita trabajos**. Donde no, saldrá en gris con
el motivo, como ya salen ahora las cuatro. Eso es mejor que fingir que se puede y colgar el
servidor la primera vez.

Lo declara `JOBS_SUPPORT=1` en el `--health`, y **está comprobado que sale por las dos
puertas** —el bucle de RPC y la línea de órdenes—. Merecía la comprobación: hubo un tiempo
en que solo lo emitía la rama de línea de órdenes, así que el cliente lo leía siempre como
falso y la función quedaba apagada sin que se viera por qué. Si esta fase se retrasa, hay
que volver a comprobarlo antes de fiarse.

## Orden de trabajo

| Fase | Qué | Por qué en este sitio |
|---|---|---|
| 0 | **HECHA** — `base/transferencia`: los tipos, el plan de caminos y el testigo de reanudación, con 40 aserciones | Es lo que decide todo lo demás, y se puede probar sin mover un byte. De hecho corrigió este documento |
| 1 | **HECHA** — bajadas `transferResumeTokenFor` y `sourceViewOfThisHost`; la interfaz las llama. Cambio de comportamiento: **ninguno**, a propósito | Da el módulo real con poco riesgo |
| 2a | **HECHA** — las REGLAS que Copiar necesita: la versión mínima de OpenZFS y las banderas de `zfs send`. Sin comportamiento nuevo | Al empezar la 2 apareció que arrastra ~110 líneas de ayudantes que este documento no había contado; partirla deja los dos trozos comprobables |
| 2b | **HECHA** — bajada la composición, y cerrado el intercambio en vivo: el CLI tenía una copia entera del protocolo de tres pasos y ya había divergido. Ahora los tres clientes llaman a `lanzaTrabajo`. Queda el respaldo SÍNCRONO de la interfaz, que es shell y va con el endurecimiento RPC, no con esta fase | Es la más corta de las tres y la que más se usa |
| 2c | **HECHA** — bajado el camino ASÍNCRONO (`lanzaTrabajo`): los tres pasos que la web necesita. 86 líneas de la ventana pasan a 24 | Es lo que desbloquea la fase 4, y no dependía de la 3 |
| 3 | **HECHA** — y no compartía tanto: la web hacía un envío COMPLETO al sitio equivocado. Regla `planeaNivelar` (base común por GUID + tres negativas) en `base/transferencia`, y la web manda el incremental | Sale barata detrás de la 2, pero solo la mitad se comparte con Copiar |
| 4 | **HECHA** — la web ofrece Copiar y Nivelar, solo por trabajos, con el motivo cuando no se pueda. Comprobado copiando de verdad desde el navegador | Primer valor visible |
| 5 | **HECHA** — Mover NO era lo que decía esta tabla: es un `zfs rename` dentro del pool. Verbo `--mutate-zfs-rename`, regla real en `dosextremos` y confirmación en la web | Resultó ser mucho más barata que la 2, y sin nada que destruir |
| 6 | **HECHA en la misma máquina** — `base/sincronizacion` con la regla y la carga tipada; la web la ofrece con pasada EN SECO antes de confirmar. Entre máquinas, en gris con el motivo | La más grande y la que menos comparte: no comparte NADA, no usa `zfs send` |

Cada fase deja el árbol compilando, `ctest` en verde y **la interfaz haciendo exactamente lo
mismo que antes** — que es la única forma de saber que no se ha perdido una decisión.

## Cómo se comprueba que no se ha roto nada

Lo de siempre en este proyecto, y aquí hace más falta que nunca porque no hay forma de
mirar una transferencia y saber si está bien:

- **Un pool de pruebas sobre fichero en cada extremo**, creado y destruido en cada pasada.
- **Suma SHA-256 de los datos en los dos lados.** Que la orden devuelva 0 no dice nada: ya
  hemos visto que `zfs allow` devuelve 0 sin hacer nada.
- **La reanudación se prueba CORTANDO**: se mata la transferencia a mitad, se relanza y se
  comprueba que sigue desde donde iba y que el resultado coincide. Es la única forma de
  saber que el testigo se busca donde hay que buscarlo.
- **Un control negativo por camino**: forzar que el daemon-a-daemon no se pueda montar y ver
  que cae al tar; forzar un extremo Windows y ver que se para con el motivo.

## Lo que no sé todavía

**Una consulta recursiva de una propiedad.** `buscaTestigo` hace N+1 llamadas —una por
descendiente— porque es lo que hacía la interfaz y la fase 1 no cambia comportamiento. Con un
`--dump-zfs-get-prop-recursive` sería una sola. Merece la pena el día que se toque el daemon
por otra cosa; solo no, porque obliga a un respaldo para los agentes viejos y la degradación
mala sería «no hay testigo» —o sea, volver a mandarlo todo—.

**Cuánto de las 2.554 líneas sobrevive.** Buena parte es `QString::arg` componiendo mensajes
y trozos de argv; con `std::string` y `format` eso encoge. No me atrevo a dar un número
hasta tener hecha la fase 2.

**Si Mover debe existir en la web.** Es Copiar y luego destruir el origen, y el destruir
ocurre cuando ya no hay nadie mirando. En la interfaz uno ve el resultado antes de que se
borre nada; en la web el trabajo termina solo. Puede que lo correcto sea que la web copie y
deje el destruir como un segundo paso explícito.

**Cómo se le habla al agente NO lo decide la capa base.** Una conexión Local no se alcanza
igual que una remota —el RPC por túnel rechaza de entrada todo lo que no sea SSH— y meter esa
distinción abajo obligaría a subir el descubrimiento del TLS local, que es de otro sitio. Así
que `lanzaTrabajo` recibe la llamada como parámetro. Se perdió al extraer y lo cazó la
primera prueba de verdad: el trabajo no arrancaba porque el destino era «Local».

**Qué pasa si el cliente muere a media transferencia por trabajos.** El daemon sigue: para
eso es asíncrona. Pero nadie recoge el resultado. `scanOrphanedJobsForConnection` existe en
la interfaz y habrá que mirar si basta.

## El cuelgue que retenía el pool: herencia de descriptores

Síntoma: lanzar dos veces la misma copia dejaba un `zfs send` vivo para siempre. No
corrompía nada, pero **retenía el pool de origen** —ni exportar ni destruir— hasta
matarlo a mano. Nadie veía un error: el trabajo se quedaba «en marcha».

La causa no estaba donde parecía. No era el duplicado, ni el receptor rechazando; era
una **carrera de herencia de descriptores**, y por eso aparecía y desaparecía. El daemon
es multihilo: mientras un hilo atiende la recepción, otro lanza el `zfs send`. Todo
descriptor abierto en ese instante lo hereda el hijo. Así que el `zfs send` acababa con
el socket **aceptado por el receptor** entre sus descriptores.

A partir de ahí el desenlace es forzoso: cuando el `zfs recv` muere, el daemon cierra su
copia del socket y la conexión **no** se cierra, porque la sigue sujetando el `zfs send`.
El emisor llena el búfer del receptor y se queda dentro de `send()`. Sin error, sin fin
de fichero, sin nada que mirar.

Medido con `ss`, que es lo que lo destapó:

```
emisor:   59466 → 39239   Send-Q 2600248   users:(("zfsmgr-agent",pid=127820,fd=5))
receptor: 39239 ← 59466   Recv-Q  129336   users:(("zfs",pid=128516,fd=7))
```

`pid=128516` es el `zfs send`. El fd 7 es el socket del **otro** extremo.

Se arregla en cuatro sitios, y los cuatro hacen falta:

1. **Los sockets se marcan sin herencia AL CREARLOS**, no después: `SOCK_CLOEXEC` en
   `socket()` y `accept4()`. Entre un `socket()` y un `fcntl()` posterior cabe justo el
   fork del otro hilo, que es la carrera que se quiere cerrar. Donde no hay forma
   atómica (macOS no tiene `accept4`) queda el `fcntl`, que estrecha la ventana.
2. **Plazo de envío** (`SO_SNDTIMEO`). Sin él no hay salida del bucle cuando el receptor
   deja de leer: ni se detecta el fallo, ni se atiende la cancelación —que antes NO
   llegaba nunca, porque solo se consultaba tras escribir con éxito—, ni se llega a
   matar al hijo. Al vencer no se abandona: se comprueba la cancelación y se reintenta.
3. **Se mata al `zfs send`** cuando el relé falla o se cancela, antes de esperarlo.
   Cerrar la tubería no basta: `zfs send` pasa ratos largos dentro del núcleo sin
   escribir nada, así que el SIGPIPE no llega y el `waitpid` no vuelve.
4. **El motivo del `zfs recv` se registra.** Antes iba a la salida de error del daemon
   —o sea, a ninguna parte cuando corre como servicio— y al emisor solo le llegaba que
   la conexión se cortó. Si tampoco quedaba aquí, no había dónde mirar.

Y el orden de los motivos importa: al matar al hijo su código de salida pasa a ser 143
(SIGTERM), provocado por nosotros. Mirarlo primero apuntaba «zfs send failed (exit 143)»
y mandaba a investigar el emisor cuando el problema estaba en el receptor. La causa que
se conoce gana a la consecuencia.

**El mismo descuido estaba en el socket de RPC** (`src/base/tlsserver.cpp`), que es el
camino más transitado: el daemon lanza un hijo por casi cada operación, así que cada uno
se llevaba la conexión del cliente. Corregido igual.

## Mover no era lo que ponía aquí

Esta tabla decía «Mover = Copiar + destruir el origen, con la confirmación en cada
cliente». **Es falso.** Se vio al leer `executeConnectionTransferAction`
(`mainwindow_state_ui.cpp:458`): la interfaz de Qt exige que origen y destino estén en la
**misma conexión y el mismo pool** y que ninguno sea una instantánea, y entonces encola un
`zfs rename`. No hay copia, no hay transferencia, no hay nada que destruir después.

Se deja escrito el error porque la versión equivocada es la plausible —«mover es copiar y
borrar» suena razonable— y volverá a proponerse. Es el segundo fallo de este mismo
documento: el primero fue la lista de caminos de transferencia.

Lo que cambia como consecuencia:

- **No cuesta una transferencia**: es instantáneo y los datos no se tocan. Por eso no
  necesita trabajos, ni daemon en los dos extremos, ni versión mínima de OpenZFS.
- **Las condiciones son otras**: mismo pool (`zfs rename` no cruza pools; para eso está
  copiar), los dos extremos datasets, y el destino no puede colgar del origen. Están en
  `dosextremos::compruebo` con motivo propio cada una, y fijadas con aserciones.
- **La confirmación sigue haciendo falta**, aunque no destruya: cambia la ruta de montaje
  del dataset y de todo lo que cuelgue de él.

Comprobado en vivo desde el navegador: `wm/origen` con un hijo dentro pasa a
`wm/carpeta/origen` con su hijo, el dato de 8 MB intacto en la ruta nueva, y los cuatro
controles negativos —otro pool, dentro de sí mismo, origen instantánea, otra máquina— con
su motivo escrito en vez de un error de ZFS.

## Nivelar no era «Copiar con otra etiqueta»

La fase 4 dio por hecho que Nivelar compartía camino con Copiar, y la web acabó con las
dos en la MISMA rama del manejador. Eran distintas en dos puntos, y los dos importan:

1. **El flujo.** Copiar manda un stream completo; nivelar manda `zfs send -I <base>
   <objetivo>`. La web no mandaba base ninguna, así que «Nivelar» hacía un envío completo
   que llega al otro extremo con `zfs recv -Fus`.
2. **Dónde se recibe.** Copiar recibe en «<destino>/<hoja del origen>»
   (`mainwindow_transfer.cpp:378`); nivelar recibe en el **dataset destino en sí**
   (`:1352`). La web usaba la ruta de copiar para las dos, así que nivelar creaba un hijo
   en vez de poner al día lo que se había pulsado.

Y faltaban las **tres negativas** que la interfaz de Qt tiene desde el principio: que la
última del destino no exista en el origen, que el destino tenga algo más moderno que lo que
se quiere enviar, y que ya esté nivelado. Sin ellas, las dos últimas situaciones pasaban por
encima del destino en silencio.

**La base común se busca por GUID, no por nombre.** Dos instantáneas pueden llamarse igual
en las dos máquinas sin tener nada que ver —con nombres automáticos eso es lo normal, no lo
raro— y un incremental contra una base falsa es un incremental contra otra historia. El GUID
ya viene en `--dump-zfs-list-all`, así que la comprobación no cuesta ninguna consulta extra.

Comprobado en vivo: origen con `@uno/@dos/@tres`, destino sembrado hasta `@dos` con los
mismos GUID. Nivelar trae `@tres` al propio `wo/d`, y el fichero que solo existía después de
`@dos` llega con SHA-256 idéntico. Repetirlo responde «el destino ya está nivelado»; pedir
`@dos` con el destino en `@tres` responde «el destino tiene una instantánea más moderna».
Las dos son 400, no un envío silencioso.

## Sincronizar no comparte nada con las otras cinco

El plan la puso en esta tabla junto a copiar y nivelar. Está en el sitio equivocado:
**Sincronizar no usa `zfs send`.** Trabaja a nivel de FICHEROS, con `rsync` sobre los puntos
de montaje de los dos extremos. De ahí salen tres diferencias que mandan sobre el diseño:

- **Necesita que los dos estén montados.** Un dataset con `canmount=off`, o montado en
  `legacy`, no se sincroniza aunque exista y esté sanísimo.
- **Puede BORRAR en el destino.** Es la única de las seis que destruye trabajo ajeno: con
  `--delete`, lo que esté en el destino y no en el origen desaparece.
- **No hay base común ni instantáneas.** No hay nada que negociar entre las dos historias
  de ZFS, porque no interviene ZFS.

`actionSyncDatasets` tiene **cinco** caminos —agente nativo en Windows, tar/ssh entre
Windows, rsync en Unix, tar sobre montaje temporal cuando la raíz no está montada, y un
recorrido por subdatasets cuando `canmount=off`—. La web implementa **uno**: rsync en la
misma máquina, por el verbo tipado `--mutate-rsync-local`, que ya existía y ya estaba en la
lista de lanzables como trabajo. Los demás salen en gris con su motivo.

**Entre máquinas se ha dejado fuera a propósito, y no por trabajo.** El rsync remoto se pide
con `-e <orden ssh>` y `usuario@host:`, y esa orden ssh, con una conexión por contraseña,
lleva la contraseña dentro. Es exactamente la fuga que se acaba de cerrar en
`docs/diseno_tecnico_conectividad.md`; meterla de vuelta por otra puerta no compensa.

### La pasada en seco manda

Preguntar «¿seguro?» no basta cuando la respuesta puede borrar ficheros. La página de
confirmación **ejecuta antes `rsync -n`** y enseña su salida literal. Sigue siendo un GET
honrado: la pasada en seco no toca nada, que es justo lo que esa ruta declara.

Y **lo que se ve es lo que se ejecuta**: el interruptor de borrado NO es una casilla del
formulario, es un enlace que recarga la página con la vista previa ya hecha con ese ajuste.
Sin JavaScript no hay forma de rehacer la vista previa al marcar una casilla, y una vista
previa que no corresponde a lo que se va a ejecutar es peor que no tener ninguna.

La de verdad va por `--job-submit`: puede tardar horas y una petición HTTP colgada durante
horas no es una forma de esperar.

Comprobado en vivo: origen con `a.txt` (nuevo) y `b.txt`; destino con `a.txt` (viejo) y
`c.txt`. Sin borrado deja `a b c` con `a.txt` actualizado; con borrado deja `a b`. Las dos
pasadas en seco dicen lo que corresponde —`*deleting c.txt` solo aparece en la de borrado—.
Negativos: destino desmontado, origen en otra máquina y origen instantánea, cada uno con su
motivo.

## El intercambio en vivo: el problema no era bajarlo, era que ya estaba duplicado

Esta fase quedó con «falta el intercambio en vivo del camino daemon-a-daemon». Al ir a
bajarlo aparecieron **tres** copias del protocolo de tres pasos, no una:

| Quién | Cómo | Estado |
|---|---|---|
| Interfaz, camino asíncrono | `lanzaTrabajo` | ya usaba la capa base |
| **CLI** (`shell.cpp`) | copia entera propia | **duplicada y divergida** |
| Interfaz, respaldo síncrono | cadena de shell propia | sigue aparte |

**La copia del CLI ya había divergido, y de forma comprobable.** Resolvía la dirección del
destino así:

```cpp
const std::string peer = mismaMaquina ? "127.0.0.1" : B::trim(pDestino->host);
```

Cuando el destino es la conexión **Local**, `host` vale `localhost`, y desde una máquina
remota eso apunta **al propio origen**. Copiar de una máquina remota a Local dejaba al
emisor conectándose consigo mismo, contra un puerto donde no escuchaba nadie. La capa base
detecta ese caso y le pregunta al origen con qué dirección nos ve.

Es exactamente el fallo que las fases existen para evitar: dos copias de la misma
orquestación, una arreglada y la otra no. Ahora el CLI llama a `lanzaTrabajo`.

Comprobado en vivo, y el caso roto entre ellos: copia en la misma máquina (SHA-256
idéntico) y copia **unibody → local** con un pool temporal sobre fichero en la máquina
remota, también con SHA-256 idéntico. El pool temporal se destruyó al terminar.

De paso desapareció `MainWindow::sourceViewOfThisHost`: la bajó la fase 1, la fase 2c la
dejó sin sentido —`lanzaTrabajo` resuelve la dirección por dentro— y desde entonces no la
llamaba nadie.

### Y los respaldos por shell, retirados

El respaldo **síncrono** de la interfaz ya no está, ni él ni los otros dos que venían
detrás. Copiar y Nivelar tenían cada uno tres:

1. el «daemon a daemon» síncrono, que pese al nombre se ejecutaba como una cadena de shell
   POSIX lanzada desde el equipo local con sudo en el origen, y resolvía la dirección del
   destino con el atajo del `host` del perfil;
2. remoto a remoto directo, `zfs send | ssh destino zfs recv`;
3. por el cliente, con los bytes dando un rodeo por esta máquina.

Los seis fuera: **entre máquinas, la copia va por los dos daemons o no va**, y si ese
camino no se puede montar se dice con un mensaje entendible en vez de salir por un camino
peor sin avisar. Dentro de una misma máquina queda el verbo tipado `--zfs-pipe-local`, que
el daemon ejecuta con `execvp` sin shell de por medio; si tampoco está, tampoco se cae a
shell. Son 276 líneas menos y ninguna construcción de órdenes de shell en la ruta de copia.

Con ellos se fueron `Montaje` y `montajeDe` de la capa base: decidían cuál de las tres
formas de encadenar `ssh` y tuberías tocaba, y sin tuberías que encadenar la regla no se ha
perdido, ha dejado de existir. Se quedaba sin ningún llamante en producción y solo la
sostenían sus propias aserciones, que es la peor forma de código muerto: la que parece
viva.

## Sincronizar entre máquinas, y cómo entra Windows

La pregunta era cómo meter a Windows para que pueda emitir o recibir rsync como el resto.
La respuesta, después de mirar el código: **no metiéndolo en rsync.**

### Por qué rsync entre máquinas es el eje equivocado

`rsync -e "<ssh>" origen usuario@destino:/ruta/` tiene tres requisitos que no se sostienen
aquí:

1. **rsync en las dos máquinas.** En Windows no está, y ponerlo significa volver a una
   dependencia de la familia MSYS2 —justo lo que el plan de endurecimiento acaba de
   retirar—, con la traducción de rutas `C:\` ↔ `/cygdrive/c` encima.
2. **Que el ORIGEN pueda hacer SSH al DESTINO por su cuenta.** rsync no lo lanza el
   cliente: lo lanza el daemon del origen. El `-i <clave>` que tiene el cliente es una ruta
   de *su* disco, no del origen. El código viejo ya lo sabía y por eso construía la orden
   «sin -i, para que la máquina de origen use su propia clave» — es decir, dependía de que
   el usuario hubiera preparado esa confianza a mano.
3. **La contraseña, si la conexión usa contraseña.** Se puede resolver como todo lo demás
   —por descriptor, con `sshpass -d<n>` dentro del `-e`— pero es plomería nueva para
   sostener un camino que ya falla por los dos puntos anteriores.

### Lo que el proyecto YA tiene

Las dos mitades de un mecanismo mejor están escritas y en producción:

- **`zfsmgr::copytree`** (`src/copytree.cpp`, 645 líneas): salta lo que ya está igual
  comparando tamaño y fecha, `--delete`, `--dry-run`, exclusiones ancladas al primer nivel,
  `--one-file-system` que en Windows entiende los puntos de reparseo. Es un rsync propio, y
  **es lo que Windows ya usa** para sincronizar dentro de una misma máquina.
- **El socket daemon-a-daemon**: `--zfs-recv-listen` / `--zfs-send-to-peer`, con testigo de
  un solo uso, portado a Windows en las fases 1 y 2, y endurecido después (descriptores sin
  herencia, plazo de envío, y matar al hijo cuando el relé falla).

Lo único que falta es el protocolo que lleve un ÁRBOL por ese socket en vez de un flujo de
`zfs send`.

### La propuesta

Un par de verbos nuevos, calcados de los que ya funcionan:

- destino: `--tree-recv-listen <dir> [--delete]` → devuelve `PORT=` y `TOKEN=`
- origen: `--tree-send-to-peer <dir> <host> <puerto> <testigo> [--dry-run]`

El origen manda un manifiesto —ruta relativa, tamaño, fecha, modo—; el destino contesta qué
quiere y qué borraría; el origen envía esos ficheros. En seco se para tras el manifiesto, y
eso es exactamente la vista previa que la web ya pinta.

**Por qué este eje y no el otro:**

- Windows funciona en **las dos direcciones**, con borrado y con pasada en seco. Hoy no
  puede: su único camino entre máquinas es el respaldo por tar, que no tiene ni una cosa ni
  la otra —copia, no sincroniza—.
- No hace falta rsync en ninguna máquina.
- **El problema de la contraseña desaparece**, no se resuelve: no hay SSH del origen al
  destino. A cada daemon se le habla por su propio canal mTLS, y el socket de datos se
  autentica con un testigo de un solo uso.
- Reutiliza el relé que ya está endurecido.
- **Quita** los respaldos por tar en vez de añadirse a ellos.

### Lo que costaría, y lo que puede morder

El recorrido y la comparación ya están escritos; lo nuevo es el formato de cable y el
streaming. Del orden de 400–600 líneas en el agente más sus aserciones, y cambia el esquema
del agente, así que obliga a reconstruir y redesplegar los cinco.

Tres cosas que hay que resolver a propósito, no de rebote:

- **La granularidad de las fechas** no es la misma en NTFS (100 ns), ext4 (ns) y HFS+ (1 s).
  `copytree` ya lo trata en local; la misma regla tiene que viajar por el cable, o cada
  pasada creerá que todo cambió.
- **Separadores y mayúsculas**: el manifiesto tiene que viajar normalizado, y el destino
  Windows compara sin distinguir mayúsculas.
- **Enlaces simbólicos y puntos de reparseo**: decidir si se copian como tales o se siguen,
  y decirlo, porque las dos respuestas son defendibles y la callada produce sorpresas.

### HECHO: `--tree-recv-listen` y `--tree-send-to-peer`

Implementado tal cual se propuso. `src/arbolremoto.{h,cpp}` tiene todo lo comprobable sin
red —recorrer, el manifiesto, la comparación, el plan y el formato de cable—; el daemon pone
los sockets, reutilizando el puerto de escucha del receptor de ZFS, que se extrajo a
`abrePuertoDeTransferencia` para no tener dos veces la doble pila IPv6.

**El protocolo, en una vuelta de red**: el destino manda su manifiesto entero —qué tiene, con
tamaño y fecha—, el origen compara contra el suyo y manda solo las operaciones que faltan.
No se pregunta fichero a fichero.

Cinco operaciones: crear directorio, copiar fichero, enlace simbólico, enlace duro y borrar.
El borrado va al final y de más hondo a menos hondo, porque un directorio no se borra hasta
que está vacío.

**Dos fallos que aparecieron al probarlo, y que no se habrían visto leyendo:**

1. **`fs::relative` sigue los enlaces simbólicos.** Canonicaliza, y canonicalizar resuelve
   el enlace: un enlace llamado «enlace» que apunta a «a.txt» salía del recorrido con la
   ruta «a.txt» —el nombre de su destino—, pisaba al fichero real en la lista y dejaba el
   fichero marcado como enlace duro de sí mismo. Sincronizar así habría destrozado cualquier
   árbol con enlaces dentro. Va con `lexically_relative`.
2. **Cuál de los dos nombres de un enlace duro es «el fichero» dependía del orden del
   directorio**, que no es el mismo en dos máquinas: los dos extremos habrían descrito el
   mismo árbol de forma distinta y la comparación habría mandado rehacer enlaces que ya
   estaban bien. Ahora se resuelve sobre la lista ya ordenada, así que el original es
   siempre el primero alfabéticamente.

Y una decisión que no es pereza: **la fecha viaja en segundos enteros**, leída del sistema
—`lstat` / `GetFileAttributesEx`— y no de `std::filesystem`, cuyo epoch no está
especificado. NTFS guarda 100 ns, ext4 nanosegundos y HFS+ un segundo: comparar la marca
exacta entre dos de ellos da «distinto» siempre y cada pasada volvería a traer el árbol
entero. Es la granularidad de `rsync --modify-window=1`, con su misma consecuencia: un
cambio dentro del mismo segundo que además conserve el tamaño no se detecta.

**La ruta que llega por el cable se comprueba antes de tocar el disco.** Viene del otro
extremo, y sin esa comprobación un «../..» en el manifiesto escribiría fuera del árbol.

Comprobado de punta a punta, y lo importante es la segunda pasada:

- En una máquina: se copia lo que falta, se rehacen enlace duro y simbólico, se crea el
  subdirectorio y se borra lo que sobra. Segunda pasada: `PENDIENTES=0 IGUALES=5`.
- **Entre máquinas** (local → unibody, con el binario nuevo puesto a mano allí): mismo
  resultado, el enlace duro llega como enlace duro —`stat` dice 2 nombres—, y la segunda
  pasada también da `PENDIENTES=0`. Eso es lo que prueba que la fecha sobrevive al viaje;
  sin ello sincronizaría bien una vez y copiaría el árbol entero para siempre.

La web ya ofrece Sincronizar entre máquinas por este camino, con su pasada en seco y su
casilla de borrado, y la ejecución de verdad va como trabajo del daemon.

### Dos cosas que solo se vieron probando contra Windows de verdad

**1. En Windows, la propiedad `mountpoint` NO es una ruta.** Un dataset que `zfs list`
declara en «/winpool/sa» está de verdad en «Z:/sa/», y esa «/winpool/sa» **no existe** para
el sistema: `Test-Path` la da por falsa. La ruta buena solo la sabe `zfs mount`, que el
daemon ya expone como `--dump-zfs-mount`. Sin esto, sincronizar contra Windows habría
escrito en un sitio inventado.

Es la misma trampa que la interfaz de Qt resuelve con `effectiveMountPath`, y por eso
`rutaUsable` pasa a ser consciente de la plataforma: en Unix una ruta absoluta, en Windows
una con letra de unidad.

**2. La papelera del volumen no es del árbol.** La primera pasada en seco contra una unidad
Windows real proponía borrar esto:

```
*deleting   $RECYCLE.BIN/S-1-5-21-…/desktop.ini
*deleting   $RECYCLE.BIN
```

`$RECYCLE.BIN` y `System Volume Information` los pone Windows en la raíz de cada unidad, y
son del VOLUMEN, no de quien lo usa. Sincronizar la raíz de un volumen con borrado se los
llevaba por delante. Ahora se excluyen —junto con `.fseventsd`, `.Spotlight-V100` y
`.Trashes`, que son lo mismo en macOS—, y **anclados al primer nivel**: un directorio del
usuario que se llame igual más abajo sí es suyo y no se toca.

**3. El receptor se callaba los fallos.** Los códigos de error de crear directorio, enlazar
y borrar se ignoraban. La primera sincronización Linux → Windows real dejó el enlace
simbólico sin crear —allí hace falta `SeCreateSymbolicLinkPrivilege` o el modo de
desarrollador— y **el trabajo se apuntó como terminado**. La pasada siguiente lo volvía a
proponer, y así para siempre.

Ahora cada operación se comprueba, las que fallan se cuentan y el receptor contesta
`PARCIAL <hechas> <fallidas> <primer motivo>`, que el emisor devuelve como fallo. No corta
el resto: un enlace que no se puede crear no es motivo para dejar diez mil ficheros sin
copiar.

Y un fichero que no se puede escribir **se lee igual y se tira**. No es limpieza: el emisor
ya ha puesto sus bytes en el cable detrás de la cabecera, así que saltárselo sin consumirlos
descolocaría el flujo y la siguiente cabecera se leería a media altura de esos datos. Lo que
sí corta la sincronización es que se rompa la conexión, porque entonces ya no hay nada que
leer.

### Lo que NO lleva todavía

- **ACL ni atributos extendidos.** `copytree` los preserva en local; por el cable no van.
- **Ficheros dispersos**: se copian correctamente, pero se rellenan.
- **Enlaces duros en Windows**: no se detectan, van como ficheros sueltos.
- **Enlaces simbólicos hacia Windows**: no se crean. Medido contra una máquina real, y el
  motivo no es el privilegio que uno esperaría sino que la operación devuelve directamente
  «Function not implemented». No se calla: cada pasada informa
  `PARCIAL … crear el enlace simbólico «enlace»: Function not implemented`, así que un árbol
  con enlaces sincroniza sus ficheros y avisa de lo que no pudo. Queda pendiente decidir si
  merece la pena decirlo UNA vez por sincronización en lugar de por enlace.

## Delta: mandar solo lo que cambió dentro de un fichero

Hecho, con el algoritmo de rsync. **No con xdelta**: xdelta calcula la diferencia entre dos
ficheros que están los dos en la misma máquina, y aquí ninguna de las dos las tiene — que es
el problema entero. Para usarlo habría que mandar antes una versión al otro lado, o sea
justo lo que se quiere evitar.

Cómo va, en una vuelta de red más:

1. Tras el manifiesto, el origen pide firmas de los ficheros que **el destino ya tiene** y
   que pasan de 1 MiB. De los que no tiene no hay nada contra qué comparar; de los pequeños,
   las firmas y la vuelta cuestan más que mandarlos enteros. rsync aplica un umbral por lo
   mismo.
2. El destino parte cada uno en bloques y devuelve, por bloque, una suma débil rodante y un
   SHA-256 recortado.
3. El origen desliza una ventana **byte a byte**. La suma se actualiza en O(1) por byte —ese
   es el truco—, y cuando coincide con alguna conocida se confirma con el hash fuerte. Emite
   «copia N bloques tuyos desde el i» o «aquí van estos bytes».

Byte a byte y no bloque a bloque a propósito: si alguien inserta un byte al principio, con
bloques alineados no se reconocería ni uno y con la ventana se reconocen todos desplazados.
Hay una aserción para ese caso justamente.

**Entre varios bloques que valgan se prefiere el siguiente al último copiado.** En un fichero
con trozos repetidos —relleno, un disco virtual medio vacío— todos comparten firma, y coger
siempre el primero generaba una instrucción por bloque: 300 KB de un solo byte daban 37
instrucciones. Con esto, una.

### La verificación no es adorno: encontró el fallo

El receptor reconstruye sobre un fichero **temporal** y solo lo pone en su sitio si el
SHA-256 del resultado entero coincide con el que manda el origen. Si no coincide, el
temporal se borra y **el original no se toca**: vale más un fichero viejo que uno a medias.

Esa comprobación cazó un fallo que las aserciones no veían. El bucle de búsqueda era
`while (ini < fin)` con el relleno del búfer DENTRO. Cuando el consumo caía justo en el final
del búfer quedaba `ini == fin` y el bucle salía sin volver a leer: el delta describía solo el
primer búfer y **el resto del fichero desaparecía en silencio**. Con bloques de 8 KiB el
búfer es de 1 MiB, así que cualquier fichero mayor se truncaba. No salió en las aserciones
porque allí el delta se comprobaba en memoria, sobre ficheros pequeños. Ahora el relleno va
antes de comprobar si queda algo, y hay una aserción con 3 MB que cruza ese límite varias
veces.

### Medido

Fichero de 50 MB con **8 bytes** cambiados en medio:

| | |
|---|---|
| enviado | 65.536 bytes (un bloque) |
| ahorrado | 49.934.464 bytes |
| SHA-256 destino | idéntico al del origen |

Igual entre máquinas (local → unibody): mismos 65.536 bytes, mismo hash. El informe lo dice
en la salida —`AHORRADOS=` y `ENVIADOS=`—, que es el único número que responde a si el delta
sirvió de algo en esa pasada.
