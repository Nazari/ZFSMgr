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
| 2b | **HECHA a medias** — bajada la COMPOSICIÓN de la orden: dónde se recibe de verdad, el `zfs send`, el `zfs recv -Fus` y cuál de los tres montajes toca. Falta el intercambio en vivo del camino daemon-a-daemon, que es E/S y no composición | Es la más corta de las tres y la que más se usa |
| 2c | **HECHA** — bajado el camino ASÍNCRONO (`lanzaTrabajo`): los tres pasos que la web necesita. 86 líneas de la ventana pasan a 24 | Es lo que desbloquea la fase 4, y no dependía de la 3 |
| 3 | **Nivelar**, que comparte casi todo con Copiar | Sale barata detrás de la 2 |
| 4 | La web ofrece las dos, **solo por trabajos**, con el motivo cuando no se pueda | Primer valor visible |
| 5 | **Mover** = Copiar + destruir el origen, con la confirmación en cada cliente | Es la 2 con un paso más, y el paso más peligroso |
| 6 | **Sincronizar**, en su propio módulo | La más grande y la que menos comparte |

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
