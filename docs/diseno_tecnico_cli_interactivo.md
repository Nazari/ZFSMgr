# El modo interactivo del CLI

`zfsmgr-cli` sin ninguna orden se comporta como un intérprete: hay una **posición**, que es
una URL `zfsm://`, y todas las órdenes actúan sobre ella salvo que se diga otra cosa.

```
zfsm://local> cd fc16/work
zfsm://local/fc16/work> ls
zfsm://local/fc16/work> create @antes
zfsm://local/fc16/work> ls --on /unibody/sback
```

## Por qué la posición es una URL y no «una conexión y un dataset»

Porque así lo que se aprende navegando sirve para escribir guiones: cualquier orden del
historial se puede copiar, ponerle `--on <url>` y ejecutarla suelta. Si la posición fuera
un estado interno con dos campos, la forma de nombrar las cosas dentro del intérprete y
fuera de él serían distintas, y habría que aprender las dos.

## El árbol, y las cuatro formas de moverse por él

El modelo es el de un sistema de ficheros, con la lista de conexiones como raíz:

```
/                          las conexiones
/OldLau                    una conexión
/OldLau/winpool            el pool, que ES un dataset
/OldLau/winpool/sa         un dataset
/OldLau/winpool/sa@ayer    una instantánea
```

`cd` acepta la URL entera, la ruta absoluta, la relativa, y `..` / `.` / `-`.

**La raíz no es una URL válida** —`zfsm://` sin conexión no nombra nada—, así que existe
solo dentro del intérprete, donde sí hace falta: es el sitio al que subir desde una
conexión. Se enseña como `zfsm:/`.

### Las dos reglas que quitan ambigüedad a una ruta relativa

Las dos salieron de escribir órdenes y ver qué hacía el intérprete con ellas:

1. **Si el primer tramo nombra una conexión, la ruta es absoluta.** `cd oldlau/winpool/sa`
   estando en `zfsm://local` va a OldLau. Sin esto habría que anteponer una barra que
   nadie recuerda al saltar de máquina.
2. **Si el primer tramo es el nombre del pool en el que estamos, es el nombre ZFS
   completo.** `destroy zfsmgrtest/clonado` desde `zfsm://local/zfsmgrtest/origen` apuntaba
   a `zfsmgrtest/origen/zfsmgrtest/clonado`: un nombre válido que no existe, o sea un error
   confuso donde uno había escrito exactamente lo que quería.

### `cd` comprueba que el destino existe

Como el de cualquier intérprete. Cuesta una ida y vuelta, pero por el túnel ya montado son
un par de milisegundos, y sin ello uno se queda apuntando a algo que no está: la orden
siguiente falla por un motivo que no parece tener nada que ver con el `cd` que la precedió.

## `--on` y `--from` son sinónimas

«from» es la palabra natural en las órdenes que además tienen un destino y «on» en las que
actúan sobre un solo sitio. Obligar a recordar cuál lleva cada una sería una regla que no
aporta nada.

**La excepción es `fromdir`**, donde sí significan cosas distintas: `--on` es el dataset de
destino y `--from` la máquina de la que sale el directorio.

## Los formatos, y para quién es cada uno

`format text|tsv|json` cambia la salida de los listados en caliente. Los tamaños salen
legibles —`464G`— en texto y en **bytes exactos** en tsv y json, porque un guion que compare
o sume no puede hacerlo con «1,2 G».

## Lo destructivo pregunta, y la pregunta dice qué

`destroy`, `rollback`, `breakdown`, `assemble`, `todir` y `fromdir` piden confirmación
nombrando lo que va a pasar y con qué alcance. «¿Seguro?» a secas es lo que hace que se
conteste que sí sin leer. `-y` en la línea de órdenes, o `yes on` dentro, lo desactivan.

## Lo que NO tiene respaldo por shell, a propósito

Las órdenes van por **RPC tipado**: argv, sin intérprete de por medio. Si el daemon no está
o no responde, se dice. En la interfaz existe un camino alternativo por shell por historia;
aquí se empieza limpio, porque ejecutar por otro camino una orden que el usuario creía
tipada es peor que fallar.

Las dos excepciones, y las dos son del dominio, no del diseño:

- **`ls #content`** lista con `ls -lA`. El agente no tiene verbo para leer un directorio.
- **`fromdir`** es una tubería `tar` entre dos máquinas. El verbo del agente lee el tar por
  la entrada estándar y **el canal RPC no tiene stdin**, cosa que el propio daemon deja
  dicha en su código.

## `fromdir` NO es la inversa de `todir`

Aunque el nombre lo sugiera. Es «Desde Dir»: crea el contenido de un dataset A PARTIR de un
directorio, que puede estar en otra máquina. La inversa de `todir` es `assemble`.

## Modo guion

Con la entrada redirigida no hay indicador ni confirmaciones interactivas, y **un fallo
detiene la sesión**: seguir ejecutando órdenes sobre un estado que no es el previsto es
cómo se destruye lo que no se quería tocar. El código de salida es el de la orden que falló.

```sh
printf 'cd fc16/work\nsnapshot @nocturna -r\n' | zfsmgr-cli --password-fd 3 -y 3< <(pass show zfsmgr)
```

## Cómo se probó

Contra el daemon real de la máquina, y sobre un **pool de pruebas montado en un fichero**
—creado y destruido para la ocasión, sin tocar nada existente—: navegación completa,
listados en los tres formatos, `#content` y `#properties`, `get`/`set`, `create`
—de dataset, de pool y de instantánea—, `clone`, `destroy`, `rollback`, `breakdown`, `assemble`, `todir` y `fromdir`.

Se condujo por un pseudoterminal, porque el material TLS del daemon local vive en
`/etc/zfsmgr` con permisos de root y hace falta poder contestar a la petición de sudo.

**Dos fallos que encontró esa prueba y que no se habrían visto de otro modo:**

1. `assemble` recibía los hijos con nombre relativo. El agente los comprueba con
   `zfs list <hijo>`, así que no existían para él y la operación se saldaba con
   «ya absorbido» y **rc=0**: parecía haber funcionado sin hacer nada.
2. `-y` se parseaba y no se pasaba al intérprete, así que seguía preguntando.

## Una sola lista de conexiones

`connections list` y el `ls` de la raíz dan **exactamente la misma salida**: la tabla se
construye en un solo sitio. Eran dos, con columnas distintas, y la misma pregunta se
contestaba de dos maneras según por dónde se preguntara — justo lo que se quería evitar al
sacar `Tabla` a su propio fichero.

Al unificarlas hubo que conservar dos cosas que se pierden si uno copia sin mirar:

- **Si una conexión tiene TLS se anota mirando el valor CRUDO**, antes de descifrar. Un
  campo que no se puede abrir queda vacío, así que con `--no-secrets` una conexión con TLS
  aparecería como si no lo tuviera.
- **Un usuario que no se ha podido descifrar sale como `<cifrado>`, nunca vacío.** Vacío se
  leería como «no tiene usuario», que es otra cosa. Sale igual en los tres formatos porque
  es un dato y no una decoración: quien lo lea tiene que saber que ahí falta la contraseña
  maestra.

Los conjuntos que llevan esa información van por **identificador**, nunca por posición.

## Las conexiones: crear, apartar, refrescar

**`create` en la RAÍZ crea una conexión**, por la misma regla que hace que `create` en un
dataset cree un hijo: se crea un nodo donde uno está. Y `destroy` estando en una conexión
la quita de la configuración —la pregunta dice explícitamente que no se toca nada en la
máquina—.

**La contraseña nunca por argumento**: se teclea o entra por `--password-fd`. Y se guarda
CIFRADA con la contraseña maestra, como hace la interfaz; sin maestra no se guarda en
claro. Eso destapó un pez que se muerde la cola: la maestra solo se pedía si YA había algo
cifrado, así que para dar de alta la primera conexión con contraseña no había ninguna. Se
arregló leyendo el descriptor siempre que se dé, y pidiéndola en el momento si hay
terminal. Y se comprueba ANTES de pedir la contraseña de la conexión: hacerla teclear para
tirarla después es la peor forma de contarlo.

**`disconnect` es la misma marca que usa la interfaz** —`app.disconnected_connections` en
config.json, con la clave que calcula `MainWindow::connectionPersistKey`—. Significa «no
hables con esta máquina»: al apartarla se cierra su túnel, y el intérprete se niega a
tocarla. Navegar hasta ella sí se permite, porque hay que poder llegar para reconectarla.

**`refresh` no es un listado.** Suelta el túnel, vacía la caché del material TLS, quita el
castigo por fallos recientes, relee la configuración —por si la interfaz la ha cambiado
mientras tanto— y vuelve a sondear. Es lo que uno espera cuando algo se ha quedado colgado.

Al probarlo salió que el motivo real de un fallo solo se veía con `-v`: el mensaje decía
«no respondió por RPC» y no si faltaba el material TLS, si la máquina estaba apagada o si
el daemon no estaba instalado —tres cosas con arreglos distintos—. Ahora se lee del mapa de
castigos, donde el transporte ya lo anota, y sale en el error.

## Windows no se parece a lo demás en dos sitios

**El directorio de configuración es `<home>/.config/ZFSMgr` también en Windows**, no
`%APPDATA%`. Es lo que hace `ConnectionStore::configDir()`, y el CLI tiene que mirar
exactamente ahí: cuando prefería `%APPDATA%` no veía NINGUNA conexión y arrancaba en la
raíz aunque la interfaz tuviera media docena configuradas. El orden para averiguar el
«home» imita al de `QDir::homePath()` —HOME, USERPROFILE, HOMEDRIVE+HOMEPATH— para que
las dos mitades del programa no puedan discrepar.

**El `mountpoint` de un dataset NO es una ruta que exista.** OpenZFS en Windows dice
`/winpool/sa`, y esa ruta no está: el pool se monta en una letra de unidad y los
descendientes heredan la del POOL, no la suya. `winpool/sa` vive en `Z:\sa`. Comprobado
contra una máquina real: `Test-Path /winpool/sa` da False y `Test-Path Z:\sa` da True.
Hay que preguntar `driveletter` **al pool**. Las instantáneas sí están donde se espera,
bajo `.zfs\snapshot\`.

Y la consola de Windows interpreta lo que le llega con la página de códigos OEM, así que
el programa pone la suya en UTF-8 al arrancar: sin eso, «— «help»» salía como
«ÔÇö ┬½help┬½».

## Pools, permisos, trabajos, daemon y transferencias

**`create` y `destroy` son el mismo verbo en los tres niveles**, porque es la misma idea:
en la RAÍZ una conexión, en una CONEXIÓN un pool, en un DATASET un hijo. Y `destroy` en un
pool es `zpool destroy` —`zfs destroy` sobre el dataset raíz de un pool no funciona, así
que es la única lectura posible—.

**Y en un dataset, `create @nombre` crea una instantánea.** Hubo un verbo aparte,
`snapshot`, heredado de que en la interfaz hay un botón distinto; se retiró. El modelo del
intérprete no es el de la interfaz: aquí una instantánea es otro nodo que se crea donde uno
está, el `@` ya es el marcador que la distingue en la URL, y `ls` dentro de un dataset ya
lista hijos e instantáneas como hermanos. Con el verbo aparte, `create @x` construía
`tank/datos/@x` y ZFS respondía «snapshot delimiter '@' is not expected here»: el programa
tenía delante todo lo necesario para saber qué se le pedía y contestaba sobre
delimitadores. Se retiró sin dejar alias a propósito —dos formas de pedir lo mismo son dos
formas de que la ayuda y la costumbre discrepen—, y se pudo hacer porque el intérprete
todavía no está publicado.

**Crear un pool es la orden más destructiva de todas**: escribe en los dispositivos que se
le den. La confirmación los ENUMERA uno a uno, porque «¿seguro?» sobre una lista que no se
ve es cómo se formatea el disco equivocado.

**`zfs allow` no tiene salida tabulada**: escribe un bloque para leer, con secciones y
entradas indentadas. Se analiza para poder darlo en los tres formatos, que es lo que
permite comprobar quién tiene qué sin leer prosa. Sin argumentos, `allow` LISTA, que es lo
que hace `zfs allow` a secas.

**Los trabajos existen porque una transferencia grande no cabe en una espera.** El daemon
ejecuta breakdown, assemble, todir y rsync sin que nadie aguarde al otro lado; se piden con
`--job` y se siguen con `jobs` y `job <id>`.

**La transferencia entre máquinas tiene DOS EXTREMOS**, no es una tubería:

1. En el DESTINO, `--zfs-recv-listen <dataset> 1` abre un puerto y devuelve `PORT=` y
   `TOKEN=`.
2. En el ORIGEN, `--zfs-send-to-peer-async <snap> <host> <puerto> <testigo> …` conecta con
   ese puerto y devuelve un `JOB_ID=`.
3. El avance se consulta en la máquina de ORIGEN.

`copy` con `--base` es incremental —lo que la interfaz llama «Nivelar»—: es la misma
operación con o sin instantánea de partida, así que es una sola orden y no dos.

**Ninguno de los dos extremos puede ser Windows**: el flujo por socket no está portado
allí. Se dice ANTES de empezar, porque un fallo a mitad de transferencia no se entiende.

**Instalar el daemon no tiene respaldo por guion, a propósito.** Si falta el binario nativo
de esa plataforma no se instala nada: un agente de guion no habla TLS, y dejarlo puesto da
una máquina que PARECE atendida y no lo está. El binario viaja por la entrada estándar en
Unix; en Windows por scp, porque PowerShell no vuelve de `ReadToEnd()` con megabytes.

## Dos fallos del TRANSPORTE que salieron al probar, y que afectaban también a la interfaz

Los dos son de antes de sacar el transporte de Qt; se portaron tal cual y nadie los había
pisado porque todas las máquinas en uso entraban por clave SSH.

**En OpenSSH gana el PRIMER valor de cada opción.** El transporte ponía `-o BatchMode=yes`
y, más abajo, `-o BatchMode=no` cuando había contraseña. El segundo no hacía nada: BatchMode
se quedaba en «yes», que DESACTIVA la autenticación por contraseña. Resultado: **ninguna
conexión con contraseña guardada ha funcionado nunca**, tampoco desde la interfaz, y el
mensaje era un «Permission denied» que parecía de credenciales incorrectas. Comprobado
contra una máquina real: con las dos opciones en ese orden deniega; con solo `BatchMode=no`
entra. Ahora se emite una sola vez, con el valor que toca.

**`scp` ni siquiera intentaba usar la contraseña.** `scpUploadArgs` fijaba `BatchMode=yes` y
el punto de llamada lanzaba `scp` a secas, así que desplegar el daemon a una máquina con
contraseña moría con «Connection closed». Ahora el helper devuelve **el programa y los
argumentos juntos** (`scpUpload`): iban separados y eso obligaba a acordarse en cada punto
de llamada de que a veces hay que lanzar `sshpass` en vez de `scp` — y no se hacía.

## Un fallo del daemon que salió al probar esto

`--job-list` devolvía errores con las barras multiplicadas: `\\\\n` donde debería haber un
salto. La causa está en la persistencia de trabajos de `daemon_main.cpp`: al guardar se
aplica `jsonEscape()`, pero al releer el `getStr` toma la subcadena entre comillas **sin
des-escapar**. Cada reescritura del fichero dobla las barras, sin límite.

Es del daemon y no del CLI, y arreglarlo es cambiar el formato de un fichero que ya está
en las máquinas, así que se deja anotado y no se toca de paso.

## La ayuda es un CATÁLOGO, no un bloque de texto

Empezó siendo un `fprintf` de sesenta líneas con la alineación puesta a mano, que se
descuadraba en cuanto una orden crecía. Pasarla a datos —nombre, uso, resumen, parámetros y
detalle— arregla la alineación y da gratis dos cosas que con texto no se podían: `help
<orden>` y el completado con el tabulador, que necesitan saber qué órdenes hay y qué acepta
cada una.

Los parámetros van **debajo y tabulados**, uno por línea: metidos en la misma línea que la
orden, una con cinco opciones ocupaba tres renglones sin que se viera cuál es cuál. Y el
ancho se mide en **caracteres**, no en bytes, o «instantánea» descuadra la columna.

## El tabulador, sin `readline`

El motivo para no usar `readline` **no es la licencia** —ZFSMgr es GPL v3, así que
enlazarla no plantea ningún problema ahí—: es que **readline no existe en Windows**, y
libedit tampoco. Sería una dependencia que resuelve el problema en tres plataformas y deja
fuera justo aquella donde este editor es más flojo, obligando a mantener los dos caminos de
todas formas. El editor de línea está escrito a mano: modo crudo, historial con las
flechas, inicio y fin, borrado, y el tabulador. **El modo del terminal se restaura siempre**,
también al salir por error: dejarlo sin eco deja al usuario escribiendo a ciegas en su
propia shell.

**El repintado no usa NI UNA secuencia de escape**, solo retorno de carro y espacios. La
consola de Windows no interpreta ANSI si no se le activa el modo de terminal virtual, y sin
eso `\033[K` se imprime literal: la pantalla se llenaba de «[K». Se podría activar ese modo,
pero repintar con `\r` funciona en cualquier terminal y en cualquier versión sin preguntar
nada, y una cosa que funciona siempre vale más que dos caminos según la plataforma.

**En Windows la entrada se pone en BINARIO mientras se edita.** En modo texto la biblioteca
de C traduce CRLF a LF, y para saber si un `\r` va seguido de `\n` tiene que mirar el
carácter siguiente: al pulsar Intro la consola entrega solo `\r`, así que `fgetc` se quedaba
esperando otra tecla y había que **pulsar Intro dos veces**. Se cambia solo mientras dura la
edición y se restaura al salir: en binario, una contraseña leída de una tubería se quedaría
con un `\r` pegado, y eso es otra contraseña.

El completado tiene tres casos, y el orden importa: la PRIMERA palabra es una orden; una
que empieza por guion es una opción DE ESA orden; y cualquier otra cosa se trata como una
URL, preguntando a la máquina por los hijos del sitio.

**Los fallos del completado se tragan a propósito.** Pulsar el tabulador no es pedir una
operación: llenar la pantalla de errores porque una máquina está apagada convertiría una
comodidad en un estorbo. Sin respuesta, simplemente no completa.

## Con --no-secrets NO se escribe la configuración

Un perfil cargado con `--no-secrets` trae los campos cifrados **vacíos**, porque no se han
podido abrir. Guardarlo así los deja vacíos EN EL FICHERO: se pierde la contraseña, sin
aviso y sin vuelta atrás.

No es hipotético: un `edit` con `--no-secrets` se llevó por delante la contraseña de sudo
de una conexión real. Ahora `guardarConexion` se niega y lo dice.

De la misma tanda: **una orden nunca ignora un argumento en silencio**. `edit fc16` se
limitaba a descartar el «fc16» y editaba la conexión donde uno estaba —diciendo
«actualizada la conexión local»—, así que uno creía haber editado otra cosa. Las órdenes
sin argumentos propios aceptan el destino suelto, y sobra uno de más se dice.

Y un esquema equivocado se nombra: `zfsmgr://fc16` decía algo sobre un tramo llamado
«zfsmgr:», que no ayuda a ver que lo único que sobran son tres letras.

## El idioma

`--lang es|en|zh`, y sin él **el mismo que use la interfaz gráfica** —`app.language` de
`config.json`—: dos sitios donde elegir idioma para el mismo programa serían dos sitios
donde discrepar.

Se reutiliza el catálogo que ya existe: los mismos ficheros `i18n/*.json` y la misma regla
de claves. El cargador está en `base/i18n.cpp`, sin Qt, y viaja al lado del ejecutable.

**El castellano va escrito en el código**, con la clave al lado: `T("t_x_ab12", "texto")`.
Así el fuente se lee sin ir a buscar qué dice cada mensaje, y si el catálogo falta, está
incompleto o está roto, la herramienta **sale en castellano** en vez de escupir claves.

Lo que NO pasa por el catálogo, y no es un olvido: los verbos, los nombres de campo de tsv
y json, y los literales de las URL. Son interfaz para programas y van en inglés siempre;
traducirlos rompería cualquier guion en cuanto alguien cambiara de idioma.

### Cómo se comprobó que no se rompía nada

Con una **referencia dorada**: 545 líneas con toda la salida que el CLI produce sin tocar
ninguna máquina —ayudas, errores de uso, rechazos, validaciones—, capturadas ANTES de
empezar. Tras cada tanda, la salida en castellano tiene que salir idéntica.

Cazó una regresión de verdad: al pasar el detalle de la ayuda a pares {clave, texto}, las
entradas con un solo párrafo se quedaron sin clave y **el párrafo desaparecía**. El arreglo
no fue poner la clave que faltaba sino cambiar el tipo, para que **lo exija el compilador**:
una lista plana de cadenas alternas no se queja si falta una; una lista de `Texto`, sí.

### Lo que queda en castellano

Los marcadores de la línea de sintaxis —`<destino>`, `<@instantánea>`— siguen en
castellano, porque van mezclados con los nombres de opción, que no cambian nunca.
Separarlos es trabajo aparte; dejarlo a medias sería peor que dejarlo entero.

Y los mensajes que se construyen concatenando trozos (≈220) tampoco: envolver cada trozo
suelto es mala práctica —un traductor no puede reordenarlos—, así que piden pasar a
`format()` con marcadores. Es la continuación natural.

## Lo que falta

- La resolución de rutas es lógica pura y está probada solo por las pruebas en vivo.
  Sacarla a la capa base la haría contrastable como el resto.
- El caso `fromdir --from <otra-máquina>` no se ha ejercitado: el material TLS de las
  conexiones remotas está cifrado con la contraseña maestra.
- No hay historial ni completado por tabulador: eso pide `readline`, y meter una
  dependencia por comodidad va en contra de lo que se ha hecho hasta ahora.
