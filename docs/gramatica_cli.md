# Gramática del intérprete

## Qué problema resuelve

Hoy cada orden decide por su cuenta cómo interpretar sus argumentos. Eso ha producido, ya
en producción, un fallo que se repite: **la orden acepta un argumento y no le hace caso**.

```text
zfsm://local> install-daemon oldlau
   → reinstala el daemon en LOCAL, sin decir nada
```

No es un caso aislado. Barriendo las 50 órdenes contra el código:

| Cómo resuelve el destino | Cuántas | Qué hace con un argumento suelto |
|---|---|---|
| a mano, cada una a su manera | **27** | depende de cuál |
| `destinoDePool()` / `adoptaPoolSuelto()` | 10 | lo usa si nombra un pool |
| `destinoDe()` y nada más | **5** | **lo ignora en silencio** |
| `destinoSuelto()` | 4 | lo usa, y protesta si sobra |

Las cinco mudas son `info`, `install-daemon`, `jobs`, `load-key` y `unload-key`. Las
cuatro que se portan bien son `edit`, `mount`, `promote` y `unmount`. Y las 27 restantes
—`ls`, `cd`, `create`, `copy`, `destroy`…— llevan cada una su propio troceo escrito a
mano, así que ni siquiera son un grupo: son 27 criterios.

Cuatro convenciones para la misma idea, una de ellas muda. Un quinto grupo —`status`,
`history`— tenía el mismo fallo y se corrigió a mano hace un rato, que es justamente la
señal de que corregirlo a mano no escala.

La causa no es que falte un análisis sintáctico: la línea se trocea bien. Es que **no hay
ningún sitio que declare qué argumentos admite cada orden**, así que no hay nada que pueda
comprobar que se han consumido todos. La ayuda sí lo dice —`ayuda.cpp` tiene la línea de
uso y los parámetros como DATOS— pero es documentación: nadie la contrasta con lo que el
código hace. De ahí que la ayuda haya prometido cosas que no existían (`#contenido`) y
callado otras que sí (`--on`).

**La regla que mata la clase entera de fallos es una sola: todo componente de la línea
tiene que ser consumido por una ranura declarada; lo que sobre es un error.**

## Los dos niveles

La gramática tiene dos partes, y conviene no mezclarlas porque se implementan distinto:

1. **Nivel léxico y sintáctico** — cómo se parte una línea en verbo, opciones y valores, y
   cómo se escribe una URL. Es pequeño, regular y no cambia al añadir órdenes. Esto es lo
   que un `lex`/`yacc` resuelve bien.
2. **Nivel de firma** — qué ranuras admite CADA verbo, de qué tipo es cada una y sobre qué
   nodo actúa. Esto es lo que arregla el fallo, y no cabe en la gramática sin escribir 50
   producciones a mano: va en una tabla.

## 1. Léxico

```lex
%%
[ \t]+                       /* separador, se descarta */
"#"[A-Za-z][A-Za-z0-9_-]*    return SECCION;      /* #content, #properties */
"@"[^ \t/@#]+                return INSTANTANEA;  /* @ayer */
"--"[a-z][a-z0-9-]*          return OPCION_LARGA; /* --delete, --on */
"-"[A-Za-z]+                 return OPCION_CORTA; /* -r, -rf */
"zfsm://"                    return ESQUEMA;
[0-9]+                       return NUMERO;
[^ \t]+                      return PALABRA;
\"([^"\\]|\\.)*\"            return PALABRA;      /* entrecomillada, se desescapa */
'([^'\\]|\\.)*'              return PALABRA;
%%
```

Tres notas que no son obvias:

- **El `@` y el `#` son del léxico, no de la semántica.** Es lo que permite que
  `create @ayer` sea una instantánea y `create hijo` un dataset sin que `create` tenga que
  mirar el primer carácter a mano, que es como está hoy.
- **`-rf` es una sola opción corta con dos letras**, no dos componentes. Es como se
  comporta hoy `trocea()`.
- **Las comillas se desescapan en el léxico.** Un punto de montaje con espacios es un
  argumento, no dos.

## 2. Sintaxis

```yacc
linea       : /* vacía */
            | orden
            ;

orden       : VERBO lista_componentes
            ;

lista_componentes
            : /* vacía */
            | lista_componentes componente
            ;

componente  : valor
            | OPCION_LARGA                 /* bandera:  --delete        */
            | OPCION_LARGA valor           /* con valor: --on <url>     */
            | OPCION_CORTA                 /* -r, -rf                   */
            ;

valor       : url
            | PALABRA
            | NUMERO
            | asignacion                   /* compression=lz4           */
            ;

asignacion  : PALABRA '=' PALABRA
            ;

url         : ESQUEMA cuerpo
            | cuerpo
            ;

cuerpo      : ruta sufijo_opt
            | INSTANTANEA sufijo_opt       /* @ayer: sobre el sitio actual */
            | SECCION detalle_opt          /* #content: sobre el sitio actual */
            ;

ruta        : '/' segmentos                /* absoluta: primer tramo = CONEXIÓN */
            | segmentos                    /* relativa al sitio actual */
            | '-'                          /* el sitio anterior */
            ;

segmentos   : segmento
            | segmentos '/' segmento
            ;

segmento    : PALABRA | '.' | '..'
            ;

sufijo_opt  : /* nada */
            | INSTANTANEA detalle_opt
            | SECCION detalle_opt
            ;

detalle_opt : /* nada */
            | '/' segmentos
            ;
```

**Ambigüedad que la gramática NO resuelve, y es correcto que no lo haga**: si un
`PALABRA` en posición de valor es una URL relativa o una palabra corriente. `scrub stop`
y `flush tank` tienen la misma forma. Eso no es sintaxis, es tipo de ranura: lo decide el
nivel 3.

## 3. Firmas

Cada verbo declara sus ranuras. Es la parte que no existe hoy y la que arregla el fallo.

```
firma := verbo objetivo? ranura* opcion*

objetivo := "@" tipo_nodo          # sobre qué actúa; sin él, la orden no toma destino
tipo_nodo := raiz | conexion | pool | dataset | instantanea | dataset_o_instantanea

ranura := nombre ":" tipo cardinalidad
tipo   := url<tipo_nodo>   # se resuelve y se COMPRUEBA que es de ese tipo
        | palabra_de{...}  # de un conjunto cerrado: stop, pause, start
        | vdev             # ruta de dispositivo
        | propiedad        # nombre=valor
        | ruta             # ruta del sistema de ficheros de la máquina
        | texto            # cualquier cosa
cardinalidad := "!" (exacta) | "?" (opcional) | "*" (cero o más) | "+" (una o más)

opcion := "--" nombre [ ":" tipo ] [ "=" valor_por_omision ]
```

Con eso, las órdenes del ejemplo quedan así:

```text
install-daemon  @conexion
flush           @pool
scrub           @pool  fase:palabra_de{start,stop,pause}?
trim            @pool  fase:palabra_de{start,stop,pause}?  disco:vdev?
clear           @pool  disco:vdev?
create          @dataset  nombre:texto!  props:propiedad*
create          @dataset  instantanea:url<instantanea>!  --recursive
rsync           @dataset  destino:url<dataset>!  --delete --check --wait
copy            @instantanea  destino:url<dataset>!  --base:url<instantanea>? --wait
import          @conexion  pool:texto!  --rename:texto?
```

**Cómo se llena el objetivo**, y esta es la regla única que sustituye a las tres de hoy:

1. Si hay `--on <url>` (o `--from`), esa es.
2. Si no, y la primera ranura sin llenar es una `url<tipo_nodo>` compatible con el
   objetivo, el primer componente suelto va ahí.
3. Si no, el sitio actual.
4. En los tres casos se COMPRUEBA que el nodo es del tipo declarado. Si no lo es, error.
5. **Lo que quede sin consumir es un error**, siempre. Ahí muere
   `install-daemon oldlau` silencioso.

## Cómo se conecta con los verbos del daemon

La firma no describe solo la línea: describe también **qué se le pide al agente**. Con eso,
añadir una orden deja de ser escribir una función y pasa a ser declarar una entrada.

```
verbo flush {
  objetivo  @pool
  agente    --zpool-generic  ["sync", $objetivo.pool]
  capacidad JobsSupport?     # no: es inmediata
}

verbo rsync {
  objetivo  @dataset
  ranura    destino:url<dataset>!  misma_maquina_que($objetivo)
  opcion    --delete --check --wait
  agente    --mutate-rsync-local  b64(json([
              $--delete, $--check, "", "", montaje($objetivo), montaje($destino)]))
  trabajo   salvo si $--check          # las que mueven datos van como trabajo
  confirma  si $--delete: "%1 va a quedar IDÉNTICO a %2, borrando lo que sobre"
}
```

Tres cosas salen gratis en cuanto la firma es un dato:

- **La ayuda se genera de ella**, y deja de poder mentir. Los tres desajustes que hemos
  encontrado esta semana —`#contenido` que no existía, `--on` que sí y no se documentaba,
  `snapshot` retirado— eran divergencias entre dos copias de la misma información.
- **El completado con el tabulador sabe qué toca**: en una ranura `url<pool>` ofrece
  pools; en `palabra_de{...}`, esas palabras; en `vdev`, la salida de `devices`.
- **La tabla de capacidades entra aquí**: un verbo que necesita `--zfs-send-to-peer` se
  deshabilita con motivo en una conexión Windows, en vez de fallar al intentarlo.

## Decisiones abiertas

**1. Qué significa una barra inicial.** Hoy `/x` es absoluta y su primer tramo es una
CONEXIÓN, así que `flush /pruebacli` busca una conexión llamada `pruebacli`. Comprobado
estando en `zfsm://local`:

```text
history /fc16  →  «/fc16» no nombra un pool de esta máquina
history fc16   →  el historial de fc16
```
Para que esa forma signifique «el pool pruebacli de la conexión actual» hay dos caminos:

- **(a)** Dejarlo como está y escribir `flush local/pruebacli` o `flush pruebacli`.
- **(b)** Que la barra inicial signifique «desde la raíz de lo que estoy mirando»: en una
  conexión, sus pools. Es más cómodo al teclear, pero rompe que una ruta absoluta
  signifique lo mismo se escriba donde se escriba, que es lo que hace que una URL se pueda
  copiar y pegar. Y `zfsm://` completa seguiría siendo la forma no ambigua.

Recomiendo **(a)**, y que la ranura `url<pool>` acepte el nombre suelto —`flush pruebacli`—
comprobándolo contra los pools de la máquina, que es lo que ya hace hoy.

**2. Si el nombre suelto se comprueba preguntando a la máquina.** Es lo implementado y
resuelve `scrub stop` frente a `flush tank` sin vocabulario nuevo, pero mete una ida y
vuelta. Con firmas, la alternativa limpia es que el ORDEN de las ranuras decida:
`scrub [<pool>] [fase]` con `fase` de un conjunto cerrado; `stop` no está en los pools ni
en las URLs, así que cae en `fase` sin preguntar nada.

**3. Sensibilidad a mayúsculas.** Hoy los identificadores de conexión se comparan en
minúsculas y los nombres ZFS no. Conviene escribirlo, porque no está en ningún sitio.

## Estado de la migración

El mecanismo está puesto: `Objetivo` y `Ranura` en `ayuda.h`, y `prepara()` en `shell.cpp`
como preámbulo único. Se mide con:

```sh
python3 scripts/revisa_firmas_cli.py     # sale con error si queda alguna orden muda
```

Tandas cerradas:

1. Las **cinco mudas** —`info`, `install-daemon`, `jobs`, `load-key`, `unload-key`—, que
   eran justo las que ignoraban argumentos en silencio.
2. Las **diez de pool** —`flush`, `scrub`, `trim`, `initialize`, `clear`, `upgrade`,
   `reguid`, `export`, `status`, `history`—, que ya compartían resolución. Aquí entraron
   las primeras ranuras de verdad: `fase:palabra_de{start,stop,pause…}` y `disco:vdev`.

3. Las **trece de dataset y conexión** —`mount`, `unmount`, `promote`, `rollback`, `holds`,
   `hold`, `release`, `get`, `set`, `refresh`, `connect`, `disconnect`, `edit`—. Aquí
   aparecieron las ranuras de texto y de propiedad, y con ellas la regla de precedencia.

Van **28 de 46**, con **0 mudas**. Las 18 restantes resuelven su destino a mano; ninguna es
muda, pero cada una lleva su criterio.

### Quién se queda el primer argumento suelto

Tres reglas, en este orden. Salieron de verlo fallar en las dos direcciones:

1. Si la orden actúa sobre un **pool** y el suelto NOMBRA un pool de la máquina, es el
   destino. Se pregunta qué pools hay en vez de mirar la forma: eso deja `clear tank sda1`
   —tank el pool, sda1 el disco— y `clear sda1` —el disco, sobre el pool actual—.
2. Si no, y la **primera ranura** declarada lo acepta, es de la ranura. Sin esto,
   `get compression` tomaba «compression» por destino y preguntaba por el dataset
   `tank/datos/compression`: un nombre suelto SIEMPRE resuelve a un dataset, porque la
   existencia no se comprueba, así que el destino se lo tragaba todo.
3. Si ninguna ranura lo quiere, se prueba como destino.

Consecuencia que conviene saber: en una orden cuya primera ranura acepta texto libre
—`get`, `hold`—, el destino va por `--on`. Es el precio de que `get compression` signifique
lo natural.

La decisión de la barra inicial está tomada: **se queda como está**. `/x` es absoluta y su
primer tramo es una conexión, así que la forma completa de un pool es `/conexion/pool`.

Dos reglas que salieron al implementarlo y no estaban en el diseño:

- **Un argumento EXPLÍCITO tiene que ser exactamente lo que se pide**; el sitio ACTUAL se
  sube hasta lo que haga falta. Estando en `local/tank/datos`, `install-daemon` habla de la
  MÁQUINA —no obliga a subir a mano—, pero `install-daemon local/sobra1` se rechaza en vez
  de tomar «sobra1» por una máquina.
- **`Objetivo::Conexion` significa la conexión y nada más.** Con la comprobación laxa
  —«que la URL tenga conexión»— cualquier dataset pasaba por máquina.

## La gramática, ya construida con bison y flex

Se construyó, y la valoración de más abajo —escrita antes— se quedó corta en lo esencial.
Se conserva porque el argumento que la corrige es lo que importa:

**Lo que se descartó por «poco»: los conflictos.** Las reglas de precedencia escritas a mano
se descubrían EJECUTANDO órdenes y viendo salidas raras —`get compression` preguntando por
el dataset `tank/datos/compression`, `trim <pool> <disco>` mandando el disco donde iba el
pool—. Con bison, una ambigüedad es un error de construcción con el caso concreto delante.
La primera pasada dio **cinco conflictos**, todos la misma pregunta que la versión a mano
respondía en silencio: *si `clear /dev/sda1` lleva una URL, ¿es el pool o el disco?*

Ficheros:

- `src/cli/gramatica.l` — el léxico. **La decisión que hace posible todo lo demás**: una URL
  y una palabra son componentes distintos, y se distinguen por su FORMA. Por eso
  `scrub stop` y `scrub /local/tank` dejan de ser la misma frase, y desaparece la consulta
  al daemon para saber qué pools existen.
- `src/cli/gramatica.y` — la gramática, con `%expect 0`: si alguien introduce una
  ambigüedad, el build falla.
- `src/cli/gramatica_cli.cpp` — la frontera con C++: convierte el resultado en estructuras
  del proyecto y traduce «syntax error» a algo útil usando la firma del catálogo.

**Una producción por ORDEN, no por «forma» compartida.** La primera versión clasificaba los
verbos por forma —diez formas para 46 órdenes— y derivaba la clase de la firma declarada.
Se descartó: la gramática se lee peor y, sobre todo, **cambiar la sintaxis de una orden
obligaba a averiguar antes qué otras usaban su misma forma** y decidir si había que
partirla. Con una regla por orden, tocar `scrub` es editar la línea de `scrub`. Es más
largo y compensa.
- `src/cli/generado/` — lo generado, en el repositorio: el agente y la interfaz se cruzan
  dentro de un contenedor y exigir bison/flex allí sería una dependencia a cambio de nada.
  Se regenera con `scripts/genera_gramatica.sh`.

### Lo que la gramática cambió del lenguaje

- **El destino posicional es una URL**, salvo en las órdenes de conexión, donde una máquina
  se nombra por su identificador. Es lo que ya se había decidido con `flush /conn/pool`.
- **Las órdenes cuya ranura también acepta una URL no llevan destino posicional**: `clear`,
  `trim`, `initialize`, `rsync`, `copy`, `diff`, `todir`, `fromdir`, `create`. Su destino va
  por `--on`. No es una limitación arbitraria: es la ambigüedad que bison señaló.
- **`get /local/tank/x compression` ahora se puede escribir**, y antes no: con las reglas a
  mano la ranura de texto se tragaba el destino y había que usar `--on`.

### Conectado

El despacho consume `analizaLinea()`. **`trocea()` y `Opts` ya no existen**, y con ellos se
fueron `destinoDe`, `destinoSuelto`, `destinoDePool`, `adoptaPoolSuelto`, `cabeEn` y
`todasLlenas` —las cuatro maneras distintas de repartir argumentos que convivían—.

Lo que queda de `prepara()` es lo único que no es sintaxis: convertir el texto del destino
en una URL y comprobar que el nodo es del tipo que la orden pide. Treinta líneas de reglas
de precedencia se fueron con la consulta al daemon que necesitaban.

Las 46 órdenes pasan por ahí: `scripts/revisa_firmas_cli.py` lo mide.

**Los mensajes de error salen ahora del catálogo.** Antes cada orden llevaba su propio
texto de «uso:» escrito a mano, y por eso unas lo tenían y otras no. Ahora un fallo de
análisis responde con la línea de sintaxis de esa orden y, si falta una ranura obligatoria,
cuál:

```text
uso: clone <nuevo> [--from <@instantánea>]  (falta <texto>)
uso: hold <etiqueta> [-r]  (falta <etiqueta>)
```

**Lo que se rompió al migrar, y cómo se caza en adelante.** Cinco órdenes —`copy`, `rsync`,
`diff`, `todir`, `fromdir`— leían la ranura por el nombre viejo: la gramática llama
«destino» a la URL de `copy` y ellas la buscaban como «texto», así que el argumento se
perdía. `zfsmgr_gramatica_cli_test` fija ahora el contrato de nombres orden por orden, que
es donde tenía que estar.

## Valoración: ¿yacc/lex de verdad? (escrita ANTES de construirlo)

Con franqueza: **la gramática vale la pena escribirla; generar el analizador con yacc/lex
probablemente no.**

Lo que arregla los fallos que hemos visto es el nivel 3 —las firmas y la regla de que no
puede sobrar nada—, y ese nivel no es una gramática libre de contexto: es una tabla con
comprobaciones semánticas (¿este nodo es un pool?, ¿está en la misma máquina?). Yacc no lo
comprueba; llamaría a código nuestro que lo haga.

El nivel 1-2 sí es agramaticable, pero es tan pequeño que el `trocea()` actual ya lo cubre
casi entero: le faltan las comillas y la asignación `p=v` como componente. Meter dos
herramientas de construcción, dos ficheros generados y una dependencia nueva para eso
saldría caro comparado con lo que resuelve.

Lo que sí propongo tomar de la idea, que es lo valioso:

- **Escribir la gramática** —este documento— como contrato, y que la ayuda se genere de él.
- **Convertir `ayuda.cpp` de documentación en la fuente de verdad**: ya tiene la línea de
  uso y los parámetros como datos; solo le falta el tipo de cada ranura y el nodo objetivo.
- **Un único preámbulo** que resuelva objetivo y ranuras contra la firma, y falle si sobra
  algo. Las 50 funciones dejan de trocear a mano y reciben la petición ya validada.

Eso da el mismo resultado —se acaban los argumentos ignorados, las órdenes se amplían
declarando, y la ayuda no puede mentir— sin generador de analizadores. Si más adelante la
sintaxis crece (expresiones, tuberías entre órdenes, condicionales), entonces sí: la
gramática ya estaría escrita y pasar a yacc sería mecánico.
