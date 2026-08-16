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

## Expresiones regulares: `std::regex`, sin dependencias

Decisión del usuario: no meter bibliotecas. Qt usa PCRE y `std::regex` usa ECMAScript,
así que hubo que resolver dos diferencias, las dos comprobadas:

- **El modificador en línea `(?i)` no existe** en `std::regex`. Va como bandera
  `std::regex::icase`. Si se hubiera copiado tal cual, el patrón habría dejado de casar
  y `PASSWORD: hunter2` habría acabado en el registro con el valor a la vista.
- **El reemplazo se escribe `$1`, no `\1`.**

Lo que sí existe en ECMAScript, y era la duda de fondo, es la **anticipación** `(?=...)`,
de la que dependen dos de los siete patrones de `maskCommandSecrets`. La retrospección
`(?<=...)` no existe, pero no se usaba.

### Cómo se comprobó, que aquí importa más que de costumbre

`maskCommandSecrets` es lo que impide que una contraseña acabe escrita en el registro,
así que una referencia dorada de 2 KB no bastaba. Se comparó la implementación **vieja
de Qt** —recuperada de git— contra la nueva sobre **200.000 entradas generadas al azar**
a partir de fragmentos elegidos para picar los bordes de cada patrón: comillas sueltas,
`'"'"'`, escapes octales a medias, `%` sin nada detrás, saltos de línea.

    maskCommandSecrets:      200.000 casos, 0 diferencias
    parseOpenZfsVersionText: 200.000 casos, 0 diferencias

Es la técnica a repetir cada vez que se porte algo con expresiones regulares: la
referencia dorada cubre los casos que uno imagina, y el sorteo cubre los que no.

## JSON propio, sin dependencias

El formato **no cambia**: `config.json` y `trust-store.json` los escribió `QJsonDocument`
y se siguen leyendo y escribiendo igual. Lo que cambia es quién los analiza.

Se escribió `src/base/json.{h,cpp}` en vez de traer una biblioteca, y el tamaño lo
justifica: el `config.json` real tiene profundidad 3 y solo cinco tipos —objeto, lista,
cadena, booleano y entero—. Ni decimales ni nulos.

La serialización imita a `QJsonDocument::Indented` hasta en sus rarezas, y hay dos que
NO son adorno:

- **Un array vacío se escribe `[\n<sangría>]`, no `[]`.** Es lo que hace Qt.
- **Los enteros se escriben sin decimales.** Qt guarda los números como `double` pero
  imprime `47653`, no `47653.0`. El puerto TLS es uno de ellos.

Si cualquiera de las dos se hiciera «bien» en vez de «como Qt», el primer guardado
reescribiría el fichero entero. No rompería nada, pero ensuciaría las copias
(`trust-store.json.bak.*`) y confundiría a cualquiera que mire un `diff`.

### Cómo se verificó

1. **Ida y vuelta sobre los ficheros reales del usuario**: `config.json` (32.341 B) y
   `trust-store.json` (6.693 B). Analizados y reescritos por la capa base, el resultado
   es **idéntico byte a byte** al que produce Qt y al fichero que ya estaba en disco.
2. **Sorteo de 20.000 documentos** generados con Qt —con comillas, barras, caracteres de
   control, acentos, emoji fuera del plano básico, enteros grandes y decimales—,
   analizados y reescritos por la capa base: **0 fallos de análisis, 0 diferencias**.

El sorteo encontró un fallo que la referencia dorada no podía encontrar, porque la
configuración real no tiene ningún decimal: **Qt escribe la representación más corta que
reconstruye el mismo `double`**, y un `%.17g` fijo daba `867811.87520846119` donde Qt
pone `867811.8752084612`. Ahora se prueban las precisiones 15, 16 y 17 y se toma la
primera que vuelve a dar el valor exacto.

3. **Regresión sobre la aplicación real**: se cargaron las conexiones del usuario con
   el `ConnectionStore` ya sin migración. Las dos aparecen, con sus campos, y el fichero
   queda intacto.

## La migración de los `.ini` se ha eliminado

Decisión del usuario. Los `.ini` por conexión son anteriores a abril de 2026, cuando la
configuración pasó a JSON (`d345d85`). Con ellos se van `migrateLegacyConnectionsToPerFile`,
`connectionIniPaths` y las cuatro funciones de carga y guardado por grupos de `QSettings`,
más siete puntos de llamada: 229 líneas.

`QSettings` sigue incluido en `connectionstore.cpp`, pero **solo para leer el
`MachineGuid` del registro de Windows**, que no es un formato de fichero.

**Consecuencia que hay que anunciar en las notas de la versión:** quien actualice desde
una versión anterior a abril de 2026 no verá convertidas sus conexiones guardadas, y no
habrá aviso. También desaparece el alias `iniPath()`, que devolvía la ruta del JSON y
solo confundía; sus dos usos pasan a `configPath()`.

## El cifrado de los secretos

El formato es `encv1$<sal>$<token>`: **Fernet** —versión 0x80, marca de tiempo, IV,
AES-128-CBC y HMAC-SHA256— con la clave derivada por PBKDF2-HMAC-SHA256, 390.000
iteraciones y **sal propia de cada valor**. Se porta tal cual: usa OpenSSL directamente,
y Qt solo aparecía en las cadenas y en la fecha.

Se revisó antes de tocarlo y **está bien**: verifica el HMAC ANTES de mirar la versión y
de descifrar, compara las firmas en tiempo constante, y parte los 32 bytes derivados en
firma y cifrado como manda la especificación (AES-128 ahí no es un recorte, es el
diseño). Se fue a buscar dos huecos concretos y no están: la rotación de contraseña
maestra cubre también `trust-store.json`, y `validateMasterPassword` valida los dos
ficheros antes de dejar entrar.

Ese último punto importa más de lo que parece. **Cuando el descifrado falla, el campo
conserva el texto cifrado.** Si se pudiera llegar a la ventana principal en ese estado,
la aplicación mandaría `encv1$…` como contraseña de sudo y, a los tres intentos,
`pam_faillock` bloquearía la cuenta diez minutos. No es alcanzable por el camino normal
porque el arranque está cerrado, pero conviene no abrir esa puerta al reorganizar nada.

### El coste, medido

**40 ms por derivación**, y con sal por valor eso se paga en cada campo cifrado —hasta
cinco por conexión— tanto al cargar como al guardar: ~0,3 s con dos conexiones, ~2 s con
diez, y el doble al rotar la contraseña maestra. **Se deja como está** (decisión del
usuario): una sal única por fichero sería más rápida pero menos conservadora, y cambiarla
obligaría a migrar.

### Lo que sí se mejoró de paso

- **Los secretos se borran de memoria** (`OPENSSL_cleanse`) antes de soltarla. No se
  hacía.
- Se quitó una fragilidad: la llamada a PBKDF2 al descifrar usaba `masterPassword.toUtf8()`
  **dos veces**, tomando el puntero de un temporal y el tamaño de otro. Era C++ válido,
  pero estaba a un refactor de convertirse en un fallo de memoria.

### Permisos de los ficheros de configuración

**Nadie los fijaba.** Que `trust-store.json` saliera 0600 y `config.json` 0664 era
casualidad del umask del momento en que se crearon. Ahora los dos se fijan
explícitamente a **0600**, y **antes** de escribir el contenido: al revés quedaría un
instante con el fichero ya lleno de secretos y los permisos que tocaran.

### Verificación

- **Compatibilidad cruzada** con la implementación de Qt, en los DOS sentidos y sobre un
  cruce de claves y textos —vacíos, con acentos, con comillas, un PEM entero y 5.000
  caracteres—: lo que cifra una lo descifra la otra. Es la prueba que aplica aquí, porque
  el cifrado lleva sal e IV aleatorios y los criptogramas nunca son iguales.
- Clave equivocada: **falla y no deja salida**. Token con un byte cambiado: rechazado por
  firma.
- Carga real de las conexiones del usuario con la capa base, y guardado forzado sobre una
  copia: contenido **byte a byte idéntico** y permisos 0666 -> 0600.

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

- **Las tres de expresiones regulares**, con `std::regex`.

`mainwindow_helpers.cpp` queda en **542 líneas** de las 1.224 iniciales, y **56 de sus
58 funciones** viven ya en la capa base. Solo quedan dos:

| | cuántas | por qué |
|---|---|---|
| Ya portadas | **38** | órdenes, predicados, SSH, `scp`, `sudo` y agente |
| Movibles, aún sin portar | 15 | sobre todo por los contenedores (`QMap`, `QVector`) |
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

1. **`connectionstore.cpp`**. El JSON, el cifrado y la migración ya están resueltos;
   queda portar la clase en sí, que a estas alturas es sobre todo pegamento.
2. **`mainwindow_refresh.cpp`** (1.174, solo 2 métodos de `MainWindow`).
3. A partir de ahí toca desacoplar de `MainWindow`, que es otro tipo de trabajo.
