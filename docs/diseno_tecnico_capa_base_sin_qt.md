# Capa base sin Qt

## Por qué

Los artefactos de la 0.92.0 pesan 258 MB de interfaz frente a 39 MB de agentes, la
imagen de toolchain ocupa 18,6 GB y una release completa necesita cuatro pasadas entre
contenedor y anfitrión porque macOS no se puede cruzar dentro de la imagen. El agente,
que no usa Qt, **se compila entero desde cero en 12,57 segundos y ocupa 0,8 MB**.

Casi todo ese coste es Qt: 29,3 MB de bibliotecas dentro del AppImage, 1,6 GB de la
imagen, el aprovisionamiento con `aqt`, y que `QT_HOST_PATH` tenga que ser distinto por
objetivo (Windows 6.8.3, FreeBSD 6.11.1 porque es la del sysroot).

Esta capa es el primer paso para poder prescindir de él. **No decide todavía qué
interfaz habrá después** —web servida, otro toolkit, o ninguna—, y ese es justamente su
valor: si el proyecto se detiene aquí, lo que queda es una aplicación Qt mejor
factorizada, no medio puente sin terminar.

## Qué se puede mover, medido

De 53.339 líneas útiles de cliente (sin blancos ni comentarios), clasificando por
proximidad a APIs de widget y calibrando la ventana contra tramos leídos a mano:

| | líneas | |
|---|---|---|
| Presentación, se borra | ~17.000 | banda real 30-40% según la ventana |
| Lógica, se porta | ~36.000 | |
| Infraestructura (`QProcess`, `QSettings`, `QSsl*`) | ~150 | pero 75 puntos de llamada |

La lógica casi pura se concentra en ocho ficheros, y ese es el orden natural de trabajo:

| fichero | portable | pintar |
|---|---|---|
| `mainwindow_remote.cpp` | 2.956 | 0% |
| `connectionstore.cpp` | 1.463 | 0% |
| `mainwindow_refresh.cpp` | 1.174 | 0% |
| `mainwindow_helpers.cpp` | 1.037 | 0% |
| `daemonpayload.cpp` | 708 | 0% |
| `mainwindow.cpp` | 2.358 | 8% |
| `mainwindow_dataset_props.cpp` | 2.811 | 17% |
| `mainwindow_dataset_actions.cpp` | 1.519 | 17% |

Aviso sobre esas cifras: «0% de pintar» **no** quiere decir «separable». `mainwindow_remote.cpp`
no pinta nada pero son 38 métodos de `MainWindow`, y desacoplarlos de la clase es el
trabajo de verdad. Los que no tienen ningún método —`mainwindow_helpers`, `daemonpayload`,
`connectionstore`— son los que se pueden mover ya.

## La regla

**`zfsmgr_base` no enlaza contra Qt.** Ni `Qt6::Core`. Es toda la garantía que hay, y
es suficiente: si alguien mete un `QString` ahí, el build falla en el commit que lo
introduce en vez de descubrirse con media biblioteca ya contaminada.

`tests/base_test.cpp` refuerza lo mismo desde el otro lado: se enlaza **solo** contra
`zfsmgr_base`, sin Qt, así que trae su propio arnés de asserts en lugar de QTest.

## Cómo se mueve una pieza sin romper nada

El patrón, aplicado ya a `daemonpayload`:

1. La lógica se porta a `src/base/`, con `std::string` en lugar de `QString`.
2. El fichero original se queda como **adaptador delgado**: misma firma con `QString`,
   convirtiendo en la frontera. Los puntos de llamada no se tocan.
3. Cuando esos puntos migren a `std::string`, el adaptador desaparece sin dejar rastro.

`daemonpayload` tenía 47 puntos de llamada. Con el adaptador, el commit de extracción
toca **cero**. Esa es la diferencia entre un cambio revisable y uno que hay que aceptar
a ciegas.

## Cómo se verifica que el puerto no cambió nada

No basta con que compile y los tests pasen: hay que demostrar que produce **los mismos
bytes**. El procedimiento, que conviene repetir en cada pieza:

1. Antes de tocar nada, un programa Qt de usar y tirar vuelca la salida de todas las
   funciones públicas a un fichero, incluyendo casos con comillas y con `%`.
2. Se hace el puerto.
3. El mismo programa se enlaza ahora contra la capa base y se vuelca otra vez.
4. `diff` entre los dos.

En `daemonpayload` el resultado fueron **43.837 bytes idénticos**.

Ese paso encontró una trampa real. `QString::arg()` con varios argumentos sustituye
**en una sola pasada**: un argumento que contenga `%2` se queda literal. Una
implementación que sustituyera en cadena rompería cualquier cadena con un porcentaje
dentro —contraseñas, rutas— y el fallo sería dificilísimo de ver. Está comprobado y
tiene caso propio en el test: `format("A=%1 B=%2", {"%2", "z"})` da `A=%2 B=z`.

Y una segunda: con trece argumentos conviven `%1` y `%10`, así que hay que leer el
número más largo o queda un `0` suelto pegado detrás.

## Los bytes no son caracteres, y eso ya habría sido un fallo

Al comparar `strutil` contra Qt con la cadena `áÉ` salieron cuatro diferencias. Dos eran
un fallo de verdad: **`QString::left(3)` cuenta caracteres y `std::string::substr(0,3)`
cuenta bytes**, así que recortar por bytes parte un carácter UTF-8 por la mitad y deja
bytes inválidos. Eso es justo lo que hace `oneLine(v, 220)` con cada línea del registro.
`left`, `mid` y `byteOfChar` van por caracteres, y coinciden con Qt en todo el plano
básico.

Las otras dos son una **divergencia buscada**: `toLowerAscii`/`toUpperAscii` no cambian
la caja de las letras acentuadas y Qt sí. Llevan «Ascii» en el nombre para que no se
use ninguna por descuido: sirven para comparar valores de propiedad, GUID y nombres de
verbo, y una conversión con reglas de idioma mete sorpresas —la I turca— justo donde se
decide algo. **Consecuencia a vigilar al portar:** cualquier función que baje a
minúsculas TEXTO LIBRE (`looksLikeSudoAuthFailure` mira mensajes de error) cambia de
comportamiento con entrada acentuada. Hay que mirarla una por una, no a bulto.

Este proyecto ya se llevó un disgusto con Unicode —la descomposición NFD de macOS—, así
que la comparación contra Qt no es una formalidad.

## La caja de las letras acentuadas SÍ decidía cosas

Al capturar la referencia de las últimas 15 con mensajes reales de `sudo` salió algo
que la banda ASCII habría dejado pasar:

    looksLikeSudoAuthFailure("SUDO: 1 INTENTO DE CONTRASEÑA INCORRECTO")
      Qt          -> true
      toLowerAscii -> false

Es decir: un rechazo de contraseña se habría clasificado como «no se pudo comprobar» y
el usuario no habría recibido el aviso. **Ese error exacto ya ocurrió una vez** por otro
motivo, y está documentado en los comentarios de esa misma función.

Así que `strutil` tiene ahora `toLowerUtf8`, `toUpperUtf8` e `isLetterAt`, que cubren
ASCII, el suplemento Latin-1 y Latin Extended-A. **Contrastadas contra Qt en todo el
rango U+0000..U+017F**: `isLetter` coincide al 100%, y de la caja solo divergen seis
puntos de código, todos declarados fuera de alcance —la I turca (U+0130/U+0131), `ß→SS`,
`ŉ→ʼN`, `ſ→S` y el signo micro—, que son los que cambian de longitud o pertenecen a
otro alfabeto.

La regla práctica al portar: **`toLowerAscii` para comparar valores de propiedad, GUID y
nombres de verbo; `toLowerUtf8` en cuanto el texto venga de una persona o de un programa
traducido.**

## Estado

Hecho y verificado:

- **`strutil`**: `trim`, `replaceAll`, `format`, `shSingleQuote`, `simplify`,
  `toLowerAscii`, `toUpperAscii`, `contains`, `startsWith`, `endsWith`, `indexOf`,
  `lastIndexOf`, `left`, `mid`, `byteOfChar`, `split`, `join`. Contrastadas una a una
  contra Qt sobre el mismo corpus.
- **`daemonpayload`** entero, con 43.837 bytes idénticos.

- **23 funciones de `mainwindow_helpers`**: construcción de órdenes de montaje y de
  transferencia, predicados sobre valores de ZFS, y `oneLine`/`stripToJson`. Los 631.950
  bytes de la referencia dorada salieron idénticos. `mainwindow_helpers.cpp` baja de
  1.224 a 1.075 líneas y las 23 quedan como adaptadores de una línea.

- **`ConnectionProfile`** y las **15 funciones** que se apoyaban en él: invocación por
  SSH, `scp`, `sudo` y el agente. Referencia dorada de **1.598.953 bytes sobre 648
  perfiles** —cruce de sistema operativo, sudo, contraseña, puerto, familia de
  direcciones y ruta de clave—: idénticas.

  El struct se copia **entero, los 16 campos**, aunque la capa base no use hoy los PEM
  de TLS. Un espejo parcial invita a que alguien lea más adelante un campo que llega
  silenciosamente vacío, y eso es peor que copiar unas cadenas de más al construir una
  orden que va a lanzar un proceso.

- **Las 15 últimas sin bloqueo**: secretos, letras de unidad, particiones de Windows,
  troceo POSIX, estado de los botones de transferencia y conflictos de punto de montaje.

`mainwindow_helpers.cpp` queda en **669 líneas** de las 1.224 iniciales, y **53 de sus
58 funciones** viven ya en la capa base. De las cinco que quedan:

| | cuántas | por qué |
|---|---|---|
| Ya portadas | **38** | órdenes, predicados, SSH, `scp`, `sudo` y agente |
| Movibles, aún sin portar | 15 | sobre todo por los contenedores (`QMap`, `QVector`) |
| Usan `QRegularExpression` | 3 | `maskCommandSecrets`, `parseOpenZfsVersionText`, `parseZpoolImportOutput` — **la decisión pendiente de más peso**: `std::regex` o una biblioteca |
| Sistema de ficheros | 1 | `findLocalExecutable` |
| JSON | 1 | `parseZfsMountJsonOutput` |
| `QProcess` | 1 | `checkLocalSudoPassword` |

### Traducir `.arg()` automáticamente: dos trampas, las dos reales

`.arg()` es postfijo, así que convertirlo a `format(plantilla, {args})` obliga a
localizar dónde empieza la plantilla emparejando paréntesis hacia atrás. Al hacerlo
saltaron dos cosas, y ninguna es teórica:

1. **Los literales llevan paréntesis sueltos.** `"case \"$mounted\" in yes|on|true|1) : ;; *) zfs mount ..."`
   descuadra cualquier emparejado. Hay que **enmascarar los literales antes** de contar
   paréntesis.
2. **`.Trim()` y `.ToLower()` aparecen dentro de literales de PowerShell.** Una
   traducción de métodos de `QString` sin enmascarar los rompe en silencio, y el fallo
   solo aparecería contra una máquina Windows.

Y una precaución de método: el bucle de conversión lleva una guarda que exige que el
número de `.arg(` **baje en cada vuelta**. Sin ella, un caso mal emparejado se convierte
en un bucle que reescribe el fichero hasta corromperlo — que es exactamente lo que pasó
antes de ponerla.

Después de `mainwindow_helpers`:

1. **`connectionstore.cpp`** (1.463). Clase propia, pero usa `QSettings` y cifrado; hay
   que sustituir el almacenamiento.
2. **`mainwindow_refresh.cpp`** (1.174, solo 2 métodos de `MainWindow`).
3. A partir de ahí toca desacoplar de `MainWindow`, que es otro tipo de trabajo.
