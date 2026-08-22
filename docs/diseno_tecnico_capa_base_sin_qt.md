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

## La traducción entre `ConnectionProfile` y el JSON

`src/base/connectionjson.{h,cpp}`: lo que decide qué acaba escrito en `config.json` y en
`trust-store.json`. Doce funciones, todas lógica pura salvo una dependencia que hubo que
cortar.

**La dependencia:** `normalizeMachineUidForStorage` llamaba a `currentLocalMachineUid()`,
que lanza `ioreg` en macOS o lee el registro en Windows —400-600 ms, con caché de proceso—.
Eso no puede vivir en la capa base. Ahora el identificador de la máquina local **se pasa
como argumento**, así que la dependencia es explícita en la firma en vez de esconderse
dentro de una estática.

**Un detalle que había que replicar y no inventar:** `decodeHexAsciiIfUuid` usaba
`QByteArray::fromHex`, que **se salta los caracteres no hexadecimales** en lugar de
fallar, y ante un número impar de dígitos actúa como si llevara un `0` delante. Hay
identificadores guardados que dependen de eso, así que la versión de la base hace lo
mismo.

### Verificación

La referencia dorada se capturó **por la API pública**, que es lo que de verdad acaba en
los ficheros del usuario: se dan de alta **3.000 perfiles** —cruce de tipo de conexión,
sistema operativo, puerto, familia de direcciones e identificador de máquina, con
espacios sobrantes por todas partes— y se vuelcan los dos ficheros resultantes más la
relectura de los 3.000. **2.238.510 bytes, idénticos.**

Los secretos van ya cifrados en la muestra a propósito: el cifrado lleva sal e IV
aleatorios, así que con secretos en claro la salida no sería comparable dos veces.

## Motivos tipificados: cómo se sacó el idioma del almacén

`ConnectionStore` devolvía avisos **ya traducidos**, así que llevárselo a la capa base
habría significado llevarse también el sistema de traducción. La salida es la misma que
ya usaba `connectioncapabilities`: **la base devuelve un motivo con sus datos y quien
tiene interfaz decide cómo se dice.**

`src/base/storewarnings.h` define un `Motivo` (20 valores) y un `Aviso` con **campos con
nombre** —`conexion`, `campo`, `detalle`— en vez de una lista de argumentos: así el sitio
que lo construye se lee solo y quien traduce no puede intercambiarlos de orden.

En el lado Qt hay un único `ConnectionStore::traduce()`. Resultado: **cero `trk()` fuera
del traductor**. Los 28 puntos que antes armaban texto a mano ahora dicen, por ejemplo,
`error = aviso(BS::Motivo::HostRequerido);`.

Dos motivos se unificaron por el camino: `t_cstore_auto009`/`auto010` eran las variantes
con «password» escrito a mano de los mismos avisos que ya existían parametrizados. El
texto resultante es idéntico, y la referencia dorada lo confirma.

## Entrada y salida de ficheros

`src/base/storefiles.{h,cpp}`, con `std::filesystem` y `fstream`. Se lleva `QFile`,
`QDir`, `QIODevice`, `QFileDevice` y `QJsonParseError`.

**El directorio de configuración se recibe como argumento.** Hoy es `~/.config/<app>`, y
reimplementar las reglas de cada plataforma para ahorrarse un parámetro sería arriesgar
que la aplicación deje de encontrar la configuración de la gente a cambio de nada. Mismo
criterio que con el identificador de máquina.

Que un fichero **no exista no es un aviso**: es el primer arranque. Solo lo es no poder
abrirlo o que su contenido no sea un objeto JSON.

## Los analizadores del refresco

`src/base/refreshparse.{h,cpp}`: identificador de máquina, pares `CLAVE=valor` y el lote
de estado de los pools. Son los que interpretan lo que responde el agente, así que son
exactamente el tipo de código que gana con tener pruebas propias.

**El aviso de siempre, otra vez:** `mainwindow_refresh.cpp` figuraba con «1.174 líneas
portables y 0% de pintar», pero de esas, **mil son un único método de `MainWindow`**
(`refreshConnection`). Lo que se ha podido mover son las siete funciones libres, ~145
líneas. El fichero se queda casi igual de largo porque los adaptadores ocupan lo que
ocupaban los analizadores; la ganancia no está en las líneas, está en que la lógica ya se
puede probar sin levantar Qt.

**Y dos de esas siete no se movieron: se borraron.** `refreshCompareAppVersions` y
`refreshVersionOrderingKey` no las llamaba nadie. Parecen el resto de una función pensada
—distinguir un agente más viejo de uno más nuevo— que nunca llegó a usarse, porque la
comprobación de versión que hay hoy es de **igualdad exacta**. Mover código muerto a la
capa nueva es exactamente como la capa vieja se llenó; están en el historial si algún día
se quiere esa función.

Verificado comparando contra las implementaciones originales recuperadas de git, sobre
**6.030 muestras** —30 escritas a mano y 6.000 sorteadas a partir de fragmentos elegidos
para picar los bordes de cada marcador—, más la comparación de versiones todas contra
todas: **0 diferencias**.

## Qué queda: la medición de `MainWindow`

Hasta aquí portar era traducir tipos. Lo que queda está atado a `MainWindow`, así que
antes de seguir se midió de qué depende realmente: para cada uno de sus **402 métodos**,
qué campos de la clase toca.

**La clase no es una bola enredada**, que era la sospecha razonable:

| campos que toca un método | métodos |
|---|---|
| ninguno | **114** (3.742 líneas) |
| uno | 105 |
| dos | 78 |
| tres o más | 105 |

Y hay **un solo centro, pequeño**: `m_profiles` lo tocan 167 métodos (41%) y `m_states`
70 (17%). El siguiente ya baja a 39. Sus tipos son triviales —`QVector<ConnectionProfile>`,
`QVector<ConnectionRuntimeState>`, `ConnectionStore`— y el tercero **ya está portado**.

### El efecto de extraer una sola cosa

Métodos que quedarían libres de todo estado de la clase:

| escenario | libres |
|---|---|
| hoy | 3.742 líneas / 114 métodos |
| sacando `m_profiles` | 5.591 / 157 |
| sacando `m_profiles` + `m_states` | 8.565 / 185 |
| **+ `m_store` y las cinco cachés** | **16.051 / 226** |

### Pero «libre de estado» no es «portable»

Cruzando con el análisis de proximidad a widgets —la corrección que este documento ya ha
tenido que hacer tres veces—:

| | lógica | mixto | pintar |
|---|---|---|---|
| hoy, libre de estado | 1.386 | 221 | 1.897 |
| hoy, atado | 9.321 | 11.525 | 7.269 |
| tras extraer el registro, libre | 4.481 | 6.679 | 3.917 |

**Movible de verdad: de 3.575 líneas (9%) a 7.937 (22%).** Cuatro veces más, no cuarenta.

Dos métodos que conviene tener presentes: **`buildUi`** son 2.652 líneas y **71 campos**
—es el nudo, y es presentación pura: si cambia la interfaz **se borra, no se porta**— y
**`applyDatasetPropertyChanges`**, 1.072 líneas y 16 campos, que cae en el peor cuadrante:
grande, atado y medio pintar.

El techo honesto: aun después de la extracción quedan **11.525 líneas en «atado + mixto»**.
Ese cuadrante no se desenreda portando; se resuelve cuando se decida qué interfaz habrá,
o no se resuelve.

## Antes de la interfaz web, un CLI

Decidido el 2026-08-16. **Un CLI es la versión ejecutable de la medición de arriba:** si
puede hacer algo, la lógica está fuera de la interfaz; si no puede, señala dónde sigue
metida. Hoy nada obliga a la capa base a estar completa —solo la usan los tests y el
cliente Qt a través de adaptadores—.

Y es un **subconjunto estricto** del trabajo web: el CLI necesita la capa de lógica; la
web necesita eso **más** servidor HTTP, sesión e interfaz. Si el CLI funciona, la web es
los mismos verbos con otra cara.

El agente ya es un CLI con 45 verbos; lo que falta es el del **cliente**, el que sabe de
varias máquinas y orquesta entre ellas.

### Lo que hace falta antes

El transporte (`mainwindow_remote.cpp`) son **2.528 líneas en 38 métodos y 0% de pintar**,
y su estado se agrupa en **dos** cosas: el registro de conexiones, y una **sesión de
transporte** con los túneles y los reintentos (`m_remoteDaemonRpcTunnelsByConnKey`,
`m_daemonRpcRetryAfterByConnKey`, `m_daemonRpcRetryReasonByConnKey`, el mutex y los
conjuntos de SSH).

### Orden acordado

1. Registro de conexiones (vale aunque no haya CLI ni web: hoy no se puede probar casi
   nada de `MainWindow` sin levantar Qt entero).
2. Sesión de transporte.
3. CLI de **solo lectura**: conexiones, pools, datasets, snapshots, propiedades. Sin
   riesgo, y ejercita toda la pila.
4. Mutaciones, con `--dry-run` y confirmación explícita.

### Dos cosas pendientes de decidir, no de implementar

- ~~La contraseña maestra en el CLI~~ — **decidido, ver abajo**.
- **Lo que no se traduce solo**: la cola de *Cambios pendientes* —revisar y luego
  aplicar— y el marcado de origen/destino son interactivos por diseño. En un CLI pasan a
  ser argumentos explícitos y `--dry-run`. Eso es rediseñar, no portar.

## Paso 1a: el modelo de datos sale de `MainWindow`

Antes de poder extraer el registro de conexiones había un obstáculo que no se veía en la
medición: **`ConnectionRuntimeState` y compañía estaban declaradas DENTRO de la clase**.
Mientras siguieran ahí, ningún registro, ningún CLI y ninguna prueba que no levante la
ventana podía nombrarlas.

`src/connectionmodel.h` recoge las **29 declaraciones** del cierre completo —desde
`ConnectionRuntimeState`, `DatasetRecord` y las cachés hasta `ConnKey`, `DSInfo` y sus
estados de edición—. El cierre se calculó a la fuerza bruta: mover, compilar, añadir lo
que el compilador eche en falta, repetir. Costó cinco rondas.

Sigue usando tipos de Qt, y es a propósito: **este paso desacopla de la CLASE, no todavía
de Qt.** Son dos ejes distintos y mezclarlos habría hecho el cambio irrevisable.

`mainwindow.h` baja de 1.662 a 1.382 líneas. En los `.cpp` solo hubo que quitar el
prefijo `MainWindow::` de 36 referencias; el resto de usos no llevaban cualificador y no
se tocaron.

## Paso 1b: el registro de conexiones (primera tanda)

`src/connectionregistry.h`. Van juntos **`profiles` y `states`**, y no por comodidad: son
vectores **paralelos** indexados los dos por `connIdx`, y separarlos hacía que mantener
ese invariante fuera cosa de acordarse. Ahora el único camino para cambiar el número de
conexiones es `setProfiles()`, que ajusta los dos a la vez.

Eso cerró una ventana que estaba abierta: `loadConnections()` asignaba los perfiles
nuevos y ajustaba los estados **veinte líneas más abajo**, y entre medias corría un
bloque con los dos vectores de distinto tamaño. Se comprobó y hoy nadie la aprovecha —lo
único que corre en medio comprueba límites—, pero es exactamente la clase de hueco que
muerde en cuanto alguien añade una línea.

**Lo que el registro NO resuelve, y está anotado en su cabecera:** guardar una
*referencia* a un elemento sigue siendo peligroso. Un modal bombea el bucle de eventos,
por ahí puede colarse una recarga, y la referencia queda colgando —lo cual importa de
verdad porque alguno de esos valores acaba dentro de una orden con `sudo`—. La regla
sigue siendo copiar por valor antes de abrir nada modal.

**781 referencias renombradas en 23 ficheros**, y compiló a la primera: es un cambio que
el compilador verifica punto por punto, aunque el diff no se pueda leer línea a línea.
Quedan dos tandas: las cinco cachés y `m_store`.

## Paso 1c: las cachés, y el fallo que destaparon

Segunda tanda del registro: `poolDatasetCache`, `poolDetailsCache`,
`datasetPermissionsCache`, `connInfoById` y `poolListEntries` (135 referencias).

Antes de moverlas se comprobó lo que se había anunciado —si su ciclo de vida es el de la
lista de conexiones— y la respuesta destapó **un fallo**:

1. Las tres primeras se indexan por **posición**: `datasetCacheKey()` produce `"0::tank"`.
2. `loadConnections()` **reindexa** los perfiles y **no las tocaba**.
3. Al borrar una conexión, la siguiente hereda su índice — y con él sus datasets
   cacheados. `ensureDatasetsLoaded()` devuelve lo cacheado sin comprobar nada más.

Solo se salvaba si esa conexión se refrescaba antes, porque `refreshConnectionByIndex()`
sí invalida por índice; pero **recargar no refresca**. Y no es un fallo cosmético: desde
ahí el usuario puede actuar —destruir un dataset, por ejemplo— creyendo que está en otra
máquina.

**Trazado leyendo el código, no reproducido en la aplicación.** La cadena es corta y cada
eslabón está verificado, pero conviene decirlo.

La corrección cae con el refactor: `setProfiles()` vacía esas tres. Cuesta una relectura;
no vaciarlas costaba enseñar los datos de una máquina bajo el nombre de otra.

**Se acotó a las tres, y a propósito.** `connInfoById` va por identificador, así que el
reindexado no le afecta; y `poolListEntries` no es una caché sino una lista que
reconstruye entera `populateAllPoolsTables()`, a la que `loadConnections()` ni llama —
vaciarla ahí habría sido una regresión introducida por el propio arreglo.

### La cura de fondo, hecha

Indexar por identificador en vez de por posición. Al ir a hacerlo, la medición corrigió
dos cosas de lo escrito arriba:

- **Eran DOS cachés indexadas por posición, no tres.** `poolDetailsCacheKey` usaba el
  NOMBRE de la conexión, no el índice, y su invalidación también: era coherente y no
  sufría el fallo. Lo dicho en el commit anterior estaba de más en ese punto.
- **Pero había una tercera víctima que no estaba en la lista:**
  `m_connContentPropValuesByObject`, con el mismo prefijo `"<idx>::"` y sin vaciarse
  nunca — las propiedades que se ven en línea en el árbol.

El patrón compartido resultó ser un **testigo** `"<conexión>::<pool>"` construido a mano
en **37 sitios de 11 ficheros**: unos para cachés persistentes (donde era un fallo) y
otros para elementos de menú y de árbol (donde el índice es inofensivo porque se
construyen y consumen en la misma operación).

Cambiar solo los persistentes habría dejado dos formatos conviviendo, y un testigo que no
casa con otro **no da error: simplemente deja de encontrar nada**. Así que se cambiaron
los 37, todos a través de una única función `MainWindow::connToken()`.

Lo que hace verificable el cambio no es el compilador —un testigo es una cadena
cualquiera— sino esta comprobación, que hay que repetir si se toca:

    grep -rnE 'QStringLiteral\("%1::[^"]*"\)[^;]*\.arg\((connIdx|idx|\w*\.connIdx)\)' src/*.cpp

Debe salir **vacío**. Si algún día no lo está, alguien ha reintroducido el fallo.

El vaciado de las cachés en `setProfiles()` se conserva, pero ya no es lo que impide el
fallo: es para no acumular las entradas de conexiones borradas, que con claves estables
no reclamaría nadie nunca.

## Paso 1d: el almacén, y el resultado de las tres tandas

`m_store` entra en el registro (29 referencias). Encaja: `loadConnections()` era ya una
danza entre el almacén y los dos vectores repartida en campos distintos de la ventana.
`ConnectionStore` no tiene constructor por defecto, así que el registro tiene el suyo —lo
cual obliga a decir de qué aplicación se cargan las conexiones, en vez de darlo por
sabido—.

### Lo que dieron las tres tandas, medido igual que antes

| | lógica | mixto | pintar |
|---|---|---|---|
| libre de estado | 1.386 | 221 | 1.897 |
| poco acoplado (≤2 campos) | **5.489** | 7.462 | 4.091 |
| atado | 6.017 | 5.111 | 4.384 |

**Movible: de 3.575 líneas (9%) a 6.875 (19%).**

Y el dato que más importa: **198 métodos dependen ahora de UNA cosa bien definida
(`m_conns`) en lugar de ocho campos dispersos.** El siguiente escalón —que esos métodos
reciban el registro como parámetro en vez de leerlo de la ventana— es lo que los llevaría
a «libre», y ya es un cambio mecánico en vez de una excavación.

Los campos que quedan por detrás son mucho más pequeños: `m_connContentTree` (44
métodos, y es un widget: eso es interfaz, no lógica), `m_topDetailConnIdx` (16),
`m_pendingChangesModel` (14).

## Paso 2: la sesión de transporte

`src/transportsession.h`: los túneles `ssh -L` vivos, las claves cuyo túnel se está
montando, la memoria de los reintentos que fallaron y los conjuntos de SSH. 67
referencias, y casi todas confinadas a `mainwindow_remote.cpp` — es el grupo más limpio
de todos los que se han movido.

**El motivo de hoy pesa más que el del CLI:** el cerrojo y lo que protege **estaban
separados**, y solo un comentario decía cuáles iban juntos. Ahora `mutex` es el primer
campo de la estructura y todo lo demás va debajo. El refresco corre en hilos
(`QtConcurrent`) y estos mapas se tocan desde varios a la vez, así que esa relación no
debería depender de que alguien lea un comentario.

Buena noticia de la revisión: **las claves del transporte nunca sufrieron el fallo de la
posición**. Salen de las coordenadas de conexión —usuario, host, puerto, ruta de clave—,
así que sobreviven a que se reordene la lista. Ese patrón ya era el correcto.

Queda fuera, y a propósito, `s_remoteDaemonTlsCache`: es una caché **estática de fichero**
con su propio cerrojo, compartida por todo el proceso. Ya está autocontenida.

### El registro: por qué NO se hizo «devolver lo que pasó»

La idea era que cada llamada devolviera la lista de lo ocurrido y que quien llama
decidiera si lo escribe en un log o lo pinta. Es más limpio sobre el papel. **Habría sido
una regresión**, y se vio al medirlo: `appLog()` escribe en la interfaz al momento, y
`runSsh()` bombea el bucle de eventos seis veces. Hoy el registro se llena **mientras** la
operación ocurre; acumular y devolver al final dejaría treinta segundos de silencio y
luego un volcado de golpe.

La versión de «limpio» que no regresa: el transporte **emite sobre la marcha, pero a algo
que recibe**. `TransportSession` lleva un destino —una función— y dentro de
`mainwindow_remote.cpp` ya **no se nombra `appLog` ni `appendConnectionLog`**: cero
apariciones, de 78 que había. La interfaz pone un destino que escribe en su pestaña; un
CLI pondría uno que escriba por la salida de error.

De paso desaparece una pareja repetida treinta veces: `appLog(...)` seguido de
`appendConnectionLog(mismo mensaje)` es ahora una sola llamada, `logConn()`.

### Dónde se para la conversión a funciones libres, y por qué

Los ganchos del transporte de mentira (`transportForTest`, `callsForTest`) entran también
en la sesión: son una propiedad DEL TRANSPORTE, no de la ventana. Con eso, `runSsh`
—547 líneas— pasa a depender de **un solo campo**, `m_transport`.

Se intentó convertirla en función libre y **se paró a propósito**, porque la cadena lleva
a un sitio que no es mecánico:

    runSsh -> tryAgentRpcOverSsh -> ensureLocalSudoCredentials -> PREGUNTA LA CONTRASEÑA

El transporte no está atado al *estado* de la ventana —eso ya está resuelto— sino a
**preguntarle cosas al usuario**. Y eso no se convierte renombrando: hay que decidir cómo
se piden credenciales cuando no hay ventana, que es la misma pregunta que ya estaba
anotada para el CLI y que corresponde al usuario, no al código.

La forma que encaja con lo ya hecho es la del destino del registro: **un proveedor de
credenciales que se recibe**, no que se busca. La interfaz pondría uno que abre un
diálogo; un CLI, uno que pregunta por terminal. Pero es una decisión, no un refactor.

Medido: de los métodos del transporte, `tryRunRemoteAgentRpcViaTunnel` (521 líneas)
necesita sesión y registro, `ensureLocalSudoCredentials` (204) necesita además las
credenciales locales y preguntar. El resto de la cadena ya solo depende de la sesión.

Y queda un `QMessageBox` en `runLocalCommand`, que no es un descuido: ese método muestra
un diálogo de progreso, es interfaz por naturaleza y no es algo que un CLI reutilice.

## Cómo entran los secretos sin ventana

Decidido el 2026-08-16, y en la misma dirección que ya apuntaba
`docs/diseno_tecnico_endurecimiento_gsa.md` («Fase 1: config protegida en disco»).

**Por descriptor de fichero, con el terminal como alternativa interactiva. Sin llavero.**

    zfsmgr --password-fd 3   3< /ruta/al/secreto      # fichero 0600
    zfsmgr                                            # sin él, pregunta por terminal

**Nunca por variable de entorno ni por argumento:** los dos quedan visibles en `ps` para
cualquier usuario de la máquina.

### Por qué no el llavero del sistema

Falla justo donde el CLI sirve. Un CLI existe sobre todo para guiones, `cron` e
integración continua, es decir máquinas sin sesión gráfica — y ahí el Secret Service de
Linux necesita D-Bus de sesión y el llavero desbloqueado, el de macOS no está desbloqueado
en una sesión SSH, y el de Windows va por sesión interactiva.

Añade además **tres implementaciones** (`libsecret`, `Security.framework`, `wincred`) y en
Linux una dependencia, para fallar en el escenario que lo justificaba.

Y un matiz que suele darse por bueno sin mirarlo: **un llavero desbloqueado lo lee
cualquier proceso de ese usuario**. No es una frontera más fuerte que un fichero 0600; es
la misma frontera con más maquinaria.

### Por qué no solo el terminal

Es correcto y sin dependencias, pero **es interactivo**, y un `cron` no teclea. Si el CLI
solo sabe preguntar, mata su propio caso de uso principal.

### Lo que gana el descriptor

No sale en `ps`; funciona headless; una implementación, cero dependencias, las cuatro
plataformas. Y es el idioma que el proyecto **ya usa**: `/etc/zfsmgr` a 700, el material
TLS a 600, `config.json` y `trust-store.json` a 0600.

El argumento fuerte: **así no hay que implementar ningún llavero, pero se soportan
todos.**

    zfsmgr --password-fd 3  3< <(pass show zfsmgr)
    zfsmgr --password-fd 3  3< <(secret-tool lookup app zfsmgr)
    zfsmgr --password-fd 3  3< <(security find-generic-password -w -s zfsmgr)

Quien lo use enchufa **su** gestor de secretos sin que la aplicación conozca ninguno. La
«fase 2» que contemplaba el documento de endurecimiento se vuelve innecesaria: la
resuelve la composición.

### Un solo secreto, no dos

Hay dos en juego y conviene no confundirlos:

| | qué abre | cuántas veces |
|---|---|---|
| **contraseña maestra** | descifra `config.json`: contraseñas de conexión y PEM de TLS | una por ejecución |
| **contraseña de sudo** | eleva en cada máquina | por operación |

La de sudo **sale ya cifrada de la configuración** una vez abierta la maestra. Así que en
la práctica el CLI necesita **una sola** entrada de secreto, y quien tiene la maestra
tiene todo lo demás — que es también el motivo de que no deba pasar por `ps`.

### Consecuencia para el transporte, ya implementada

`ensureLocalSudoCredentials` era lo que impedía convertir la cadena de `runSsh` en
funciones libres, porque **pregunta**. Ahora `TransportSession` lleva un
`credentialProvider` con la misma forma que el destino del registro: **se recibe, no se
busca**. El diálogo salió de esa función —cero widgets ahí dentro— y la interfaz lo pone
desde su constructor.

Sin proveedor puesto, `askCredentials()` devuelve **false**, y es lo prudente: intentar
una operación con `sudo` sin credenciales es peor que no intentarla, porque a los tres
fallos `pam_faillock` bloquea la cuenta diez minutos.

Con esto, las dos cosas que el transporte necesita del exterior —**a dónde contar** y
**cómo preguntar**— se reciben, y ninguna se busca.

**Ese camino no tenía ninguna prueba automática.** Ahora tiene dos, en el test de la
ventana: que sin proveedor no se intenta, que se le pasa el motivo y devuelve lo tecleado,
y que cancelar se propaga. Lo mismo para el destino del registro. No cubren el diálogo en
sí —eso sigue sin probarse— pero sí el punto de unión, que es lo que acaba de cambiar.

## Paso 3: `transport::`, las primeras funciones libres

`src/transport.h` abre el espacio de nombres donde vive lo que un CLI llamaría. De
momento tres funciones —`isLocalConnection`, `isWindowsConnection` y `wrapRemoteCommand`—
y `MainWindow` conserva métodos del mismo nombre que delegan, así que ningún punto de
llamada cambió.

Son pocas líneas y es a propósito: lo que fija esta tanda es **el patrón** —espacio de
nombres, cabecera, envoltorios— no el volumen.

**Lo que enseñó el intento:** `ensureLocalDaemonTlsMaterial` parecía una hoja y no lo es.
Llama a `runSsh` y a la resolución de credenciales de `sudo`, así que entra cuando entre
esa cadena. Está anotado en la cabecera para que nadie lo intente otra vez creyendo que
se olvidó.

Queda por convertir la cadena grande —`runSsh` (547 líneas), `tryRunRemoteAgentRpcViaTunnel`
(521) y `tryAgentRpcOverSsh` (135)—, que ya no tiene muros conceptuales: el estado está en
la sesión, el registro sale por su destino y las credenciales por su proveedor. Falta
además llevar `ensureLocalSudoCredentials` a la sesión como política, porque la cadena la
llama.

## Paso 4: el CLI existe

`src/cli/`, objetivo `zfsmgr-cli`. **Enlaza solo contra `zfsmgr_base`**, y eso no es un
detalle de empaquetado: es el contrato. Si algún día deja de compilar por un símbolo de
Qt, es que se ha metido interfaz en la capa de lógica.

Los números, frente a lo que había:

| | |
|---|---|
| CLI | **349 KB**, compila desde cero en **5,14 s** |
| Agente | 814 KB |
| AppImage de la GUI | 53.142 KB |

`zfsmgr-cli connections list` funciona **hoy**, contra la configuración real, sin tocar el
transporte: leer la configuración, unir el almacén de confianza, descifrar los campos y
sacar columnas separadas por tabulador —troceables con `cut` sin adivinar anchos—.

### Los secretos, de punta a punta

Es la primera vez que se ejercita la decisión completa:

    zfsmgr-cli --password-fd 3 connections list  3< <(pass show zfsmgr)   # cualquier gestor
    zfsmgr-cli connections list                                           # pregunta, sin eco
    zfsmgr-cli --no-secrets connections list                              # ni pregunta

Comprobado con una configuración preparada al efecto —porque en la real el usuario NO va
cifrado y la prueba obvia no probaba nada—: con la contraseña correcta descifra; con la
equivocada sale `<no se pudo descifrar>` y **nunca el texto cifrado**; sin terminal ni
descriptor falla con `rc=1` diciendo qué usar, que es el caso `cron`.

Detalle deliberado: **solo se pide la contraseña maestra si hay algo cifrado**.
Preguntarla sin necesidad es la clase de fricción que lleva a la gente a ponerla en un
alias de shell.

### Un fallo que cazó la propia herramienta

La primera versión daba «TLS: no» para una conexión que **sí** lo tiene. El material TLS
no vive en `config.json` sino en `trust-store.json`, que es un fichero aparte justamente
para separarlo de las contraseñas. Lo delató comparar la salida con lo que ya se sabía de
esa conexión — y es exactamente para lo que sirve tener una segunda vía de leer los mismos
datos.

## Paso 5: la cadena del transporte, convertida

`runSsh`, `tryAgentRpcOverSsh`, `tryRunRemoteAgentRpcViaTunnel` y
`ensureLocalDaemonTlsMaterial` son ya funciones libres en `transport::`. **1.384 líneas
sin una referencia a `MainWindow`, `m_conns`, `appLog` ni un widget** —comprobado
recorriendo cada cuerpo—. La ventana conserva métodos del mismo nombre que delegan, así
que ningún punto de llamada cambió.

### Lo que hacía falta del exterior, y cómo se recibe

Cinco cosas, todas en `TransportSession` y ninguna buscada:

| | |
|---|---|
| `sink` | a dónde contar lo que ocurre |
| `credentialProvider` | cómo preguntar |
| `localSudoResolver` | resolver las credenciales de `sudo` local |
| `tlsPersister` | guardar el material TLS negociado |
| `owner` | qué objeto posee el hilo de los túneles |

Las dos del registro se pasan **como políticas y no como el registro entero**: lo que el
transporte necesita no son los perfiles, son dos decisiones que dependen de ellos.
Pasarle el registro le daría acceso a las contraseñas de todas las máquinas para hacer
dos cosas concretas.

### La dependencia que quedó, y qué es en realidad

`tryRunRemoteAgentRpcViaTunnel` **rechaza correr fuera del hilo de la interfaz**, y
`tryAgentRpcOverSsh` ordena la llamada a ese hilo bloqueando. Parecía atadura a
`MainWindow` y no lo es: los `QProcess` de los túneles necesitan **un objeto con bucle de
eventos**, y en la aplicación resulta ser la ventana.

Ahora es `ses.owner`. En una herramienta de un solo hilo se queda nulo, `enHiloDeTuneles()`
devuelve siempre cierto y todo corre en línea sin ordenar nada a nadie. Los dos
`processEvents` de las esperas también pasan por ahí: solo se bombea si hay dueño, porque
sin interfaz no hay nada que refrescar.

De paso queda a la vista lo que ya estaba anotado como problema aparte: **que el dueño sea
la ventana es lo que serializa el montaje de túneles en el hilo de interfaz**. El refactor
no lo arregla, pero ahora se ve de dónde sale y qué habría que cambiar.

## Paso 6: quitar Qt del transporte — la ejecución de procesos

El transporte usa `QProcess` (32 usos), `QSslSocket` (12) y `QThread` (5). Sacarlo de Qt
significa sustituir esas tres piezas.

**Dos de las tres ya existían sin Qt en este repositorio**, y no había que escribirlas:
el agente ejecuta procesos con `fork`/`exec` en POSIX y `CreateProcess` en Windows desde
hace tiempo, y usa `std::thread`. Lo que faltaba era usarlo desde el cliente.

`src/base/process.{h,cpp}`: **432 líneas movidas del agente a la base**, ya probadas
contra Linux, macOS, FreeBSD y Windows. `runExecCapture`, `runExecStreaming`,
`runExecCaptureWithStdin`, `winBuildCommandLine` y `decodeWaitStatus`. El agente pasa a
enlazar `zfsmgr_base` y usarlas desde ahí: **un solo sitio**, de modo que una corrección
ya no puede dejar a la otra mitad tratando mal exactamente los mismos argumentos —rutas
con espacios, datasets con comillas—.

Nunca hay intérprete de por medio: se pasa argv y se ejecuta directamente. Es lo que
impide que un nombre de dataset con `;` se convierta en otra orden.

Verificado con el agente contra el pool real: `--version`, `--dump-zpool-list` y
`--mutate-copy-tree` responden igual que antes.

### El cliente TLS

`src/base/tlsclient.{h,cpp}`, la única de las tres que había que escribir: el agente tenía
el lado servidor con OpenSSL, no el de cliente.

**La validación es por FIJACIÓN de certificado, no por CA**, y se conserva tal cual estaba
porque su motivo está medido: en macOS, SecureTransport nunca validaba la cadena —«The
root CA certificate is not trusted for this purpose»— ni con `subjectAltName`,
`extendedKeyUsage`, `keyUsage` y `basicConstraints` correctos. Como el certificado del
daemon se trae POR SSH y se guarda, comparar contra ESE certificado exacto es más
estricto que confiar en una cadena. La autenticación mutua se mantiene entera.

Dos detalles del puerto: la fijación se comprueba **antes de escribir nada** —no tiene
sentido mandarle una petición a quien no sabemos quién es— y la clave del cliente se lee
con `PEM_read_bio_PrivateKey`, que reconoce RSA y EC sin distinguir, cosa que en la
versión con Qt había que intentar por separado.

Probado **contra el daemon local real**, que responde `STATUS=OK`. Y los cuatro caminos de
fallo, que es lo que importa de una pieza así:

| | |
|---|---|
| certificado esperado distinto (un intermediario) | rechazado: «no es el esperado» |
| sin certificado de cliente válido | el daemon corta, mTLS funciona |
| PEM corrupto | rechazado antes de conectar |
| puerto sin nadie | falla al conectar, no se cuelga |

Con esto, **las tres piezas están**: procesos, hilos (`std::thread`) y TLS. El transporte
ya puede vivir fuera de Qt.

## Paso 7: el RPC local pasa a usarlas

`tryRunLocalAgentRpc` —el camino de la conexión Local— ya no usa `QSslSocket` ni
`QJsonDocument`: usa `base::tlsRequestLine` y `base::json`. De 85 líneas a 60.

Dos cosas cambian de comportamiento, las dos a mejor:

- **Se prueba UN nombre de par, no dos.** El bucle sobre `zfsmgr-agent-server` y
  `zfsmgr-agent` existía para la verificación de nombre de host de Qt; con la fijación
  explícita del certificado el nombre no interviene en la validación, así que probar dos
  era gastar una conexión de más.
- **La clave del cliente no se lee dos veces.** Antes se intentaba como RSA y, si fallaba,
  como EC. `PEM_read_bio_PrivateKey` reconoce las dos.

Verificado que la petición sale **byte a byte idéntica** a la que construía
`QJsonDocument(...).toJson(Compact)`, sobre cinco casos con comillas, barras invertidas,
saltos de línea y argumentos vacíos: **0 diferencias**. Es lo que había que comprobar,
porque una petición con otra forma la rechaza el daemon.

## Paso 8: ejecución con retroalimentación

`base::runExecCapture` no bastaba para `runSsh`. Lo que hace falta para una transferencia
no es ejecutar y esperar: es **enseñar las líneas según llegan, avisar de cuánto queda y
poder cancelar**. Eso lo hacía `QProcess` bombeando el bucle de eventos de Qt, y es
precisamente lo que ataba el transporte a la interfaz.

`runExecStream` lo cubre con **un solo punto de enganche**, `onTick`: se llama cada pocos
milisegundos aunque no llegue nada, y **devolver false cancela**. Con eso, quien tiene
interfaz deja respirar a la ventana, cuenta cuánto queda y mira si el usuario canceló —las
tres cosas del bucle de Qt— y quien no la tiene simplemente no lo pone.

Detalles que no son evidentes y están en el código:

- **El retorno de carro corta línea igual que el salto.** `zfs send` escribe el progreso
  con retornos y sin saltos; sin esto la barra no aparecería hasta el final.
- **Cerrar la entrada estándar** cuando se acaba lo que hay que escribir. Sin eso,
  `zfs recv` espera para siempre.
- **TERM antes que KILL** al cancelar: matar de golpe deja huérfanos los hijos del proceso
  —el `ssh` que a su vez lanzó otra cosa—.
- Todo sin bloqueo y con `poll`: escribir y leer a la vez sobre tuberías bloqueantes es
  cómo se abrazan los dos procesos.

### Probado, no leído

Doce casos, en el test permanente. Tres importaban de verdad:

| | |
|---|---|
| las líneas llegan **mientras** corre | medido: la primera antes de 200 ms, la segunda pasados 300 |
| **4 MB** por la entrada estándar | la tubería se llena y hay que alternar escritura y lectura |
| cancelar **corta de verdad** | un `sleep 30` cancelado a los 300 ms termina en menos de 3 s |

Y los que delatan errores tontos: `stdout` y `stderr` separados, código de salida
propagado, 127 si el programa no existe, 124 por tiempo, 130 por cancelación,
`timeoutMs=0` como «sin límite» y no como cero, la última línea sin salto entregada, y
que un argumento con `;` o `|` llegue **literal** —porque no hay shell de por medio—.

Son POSIX solamente. En Windows harían falta otras rutas, y una prueba que solo corre en
una plataforma es mejor declararla que fingirla.

## Paso 9: `runSsh` sin `QProcess`

Los dos bucles de `QProcess` de `runSsh` —el local y el remoto, con la misma forma—
pasan a `runExecStream`. En la función ya no queda ninguna mención a `QProcess` salvo un
comentario histórico.

**El detalle que había que no romper:** el tope de tiempo es de **INACTIVIDAD**, no total.
El temporizador se reiniciaba con cada trozo que llegaba. Una transferencia de horas no
puede morir por durar; sí debe morir si se queda muda. Como `runExecStream` lleva un tope
total, se le pasa cero —sin límite— y la cuenta de inactividad va en `onTick`, que es
también donde se avisa de cuánto queda y se deja respirar a la interfaz.

De paso desaparece un guardián: el `ProcessGuard` que mataba al hijo en el destructor
existía porque Qt avisaba —«Destroyed while process is still running»— y dejaba sueltos
el `ssh` o el `sshpass` reteniendo su socket de multiplexado. `runExecStream` espera
siempre al proceso, salga por donde salga.

### Probado contra una máquina real

Aquí las referencias doradas no valen: un `ssh` de verdad falla de maneras que ningún
volcado reproduce.

| | |
|---|---|
| orden simple, tres líneas | llegan por retrollamada, `rc=0` |
| orden que falla | `rc=7`, propagado |
| 10.000 líneas de salida | todas, en 18 ms —la tubería no se atasca— |
| 4 MB por la entrada estándar | `wc -c` responde 4194304 |
| callar 8 s con tope de 3 | corta a los 3.054 ms, `err=Timeout`, 4 avisos de cuenta atrás |
| **hablar 6 s con tope de 3** | **sobrevive**: el tope es de inactividad, no total |

El último es el que de verdad valida el diseño: si el tope fuera total, una transferencia
larga moriría a mitad.

**Sigue sin probarse la rama de Windows** de `runExecStream`, escrita con `PeekNamedPipe`
y sin ejecutar.

## Paso 10: el transporte se muda a la base — primera tanda

`transport::` ya eran funciones libres desde el paso 3, pero su implementación seguía
dentro de `mainwindow_remote.cpp` y hablando en `QString`. La mudanza a `src/base/` se
hace por tandas, y **el corte no es por tamaño: es por qué se puede verificar sin una
máquina delante**.

Esta primera tanda es todo lo que **decide y analiza texto**: funciones puras a las que
entra un perfil o una cadena y sale una decisión u otra cadena. Diez, unas 350 líneas, en
`src/base/transportcmd.{h,cpp}`. Lo que abre sockets, lanza procesos y mantiene túneles se
queda para después, porque necesita piezas nuevas en la base.

No se llama `transport.h` porque `src/base` está en la ruta de inclusión junto a `src/`, y
mientras exista el adaptador `src/transport.h` dos ficheros con el mismo nombre harían
ambiguo el `#include`.

### Cómo se verificó: un contraste con control negativo

Se extrajeron las diez implementaciones **literalmente** del fichero original —no de
memoria— a un programa que enlaza Qt y la base a la vez y las ejecuta en paralelo sobre el
mismo corpus: perfiles con acentos y mayúsculas mezcladas, órdenes reales, volcados TLS a
medias, `agent.conf` con basura, y 4.000 cadenas al azar hechas con los caracteres que han
roto esto alguna vez. **9.279 casos, ninguna diferencia.**

Pero «ninguna diferencia» no significa nada si el contraste no sabe fallar, así que se
metieron averías a propósito. Y ahí apareció lo interesante: **la avería del byte alto de
UTF-16 no se detectó.** El corpus escrito a mano tenía `ñ á é € 💾`, y resulta que todos
ellos tienen el byte alto PAR, de modo que la conversión a UTF-16LE —la pieza más
intrincada de la tanda, la que decide si PowerShell entiende la orden o recibe basura—
estaba efectivamente sin comprobar.

Con el corpus ampliado a un barrido de puntos de código de los tres tamaños, incluidos los
de byte alto impar y los que necesitan pareja suplente, las tres averías se detectan: 120
diferencias la del byte alto, 12 la de la pareja suplente, 41 la del corte por `&`.

La lección no es sobre UTF-16. Es que **un corpus escrito a mano tiende a parecerse a lo
que ya se tenía en la cabeza**, y sin control negativo no hay forma de saberlo.

Lo verificado se fijó luego como aserciones permanentes en `tests/base_test.cpp`, porque
el arnés de contraste se tira en cuanto la mudanza termina.

### De paso: el conversor de perfil, en un solo sitio

Había **dos copias idénticas** de la traducción entre `ConnectionProfile` y su espejo sin
Qt —una en `connectionstore.cpp`, otra en `mainwindow_helpers.cpp`— y esta tanda estaba a
punto de añadir la tercera. Es un espejo de 16 campos: si una copia se queda atrás al
añadir un campo, ese campo llega **vacío** al otro lado sin que nada falle, que es
exactamente el fallo del que avisa la cabecera de `base/connectionprofile.h`. Ahora es una
función `inline` en `connectionstore.h`.

### Lo que queda pendiente de esta zona

`fetchRemoteDaemonTlsMaterial` repite a mano la codificación UTF-16LE + base64 que ahora
vive en `wrapRemoteCommand`. No se ha unificado en esta tanda porque esa función hace E/S
por SSH y entra en la siguiente.

## Paso 11: el transporte entero, tandas 2 y 3

La segunda tanda mueve lo que **ejecuta**: `runSshRaw` —el último `QProcess` del camino de
órdenes— y el RPC local por TLS. Va en un fichero aparte de `transportcmd` porque aquí sí
hay E/S, y eso cambia cómo se verifica: lo de la primera tanda se contrasta byte a byte
contra Qt, y esto hay que probarlo contra una máquina.

La tercera es la delicada: el túnel `ssh -L`, `tryAgentRpcOverSsh` y `runSsh`. Por ahí
pasan las mutaciones, y casi todo lo que hay existe para responder con precisión a una
sola pregunta —**¿pudo la orden haber llegado al otro lado?**—, porque de ella depende si
se puede reintentar.

### Lo que hubo que construir antes

El túnel no se podía mudar sin tres piezas que la base no tenía:

- **`ChildProcess`**: un proceso que sigue VIVO entre llamadas. Todo lo que había en
  `process.h` lanza algo, espera y recoge; un túnel se levanta, se usa muchas veces y se
  cierra. **El destructor lo mata**, e `isRunning()` recoge al hijo si acaba de morir para
  que no queden zombis.
- **`reserveFreeLocalPort` y `canConnectLocal`**: el puerto del reenvío y la pregunta «¿ya
  acepta?». Conectarse antes de tiempo da ECONNREFUSED, que el llamante contaba como fallo
  del saludo TLS y le valía a la conexión un castigo que no arregla nada.
- **Enganches en el cliente TLS**: `onBeforeWrite`, que marca el punto a partir del cual la
  orden puede haber llegado, y `keepWaiting`, que permite abandonar en cuanto el túnel
  muere. Con `keepWaiting` la espera se trocea, y por eso un `ioTimeout` de cero —«sin
  límite», que es lo que piden las operaciones que copian datos— ya no bloquea para
  siempre. También un `TlsFailure` que dice EN QUÉ PUNTO falló: no llegar a conectar y que
  el saludo TLS falle apuntan a causas opuestas, y decidirlo buscando subcadenas en un
  mensaje era frágil.

### Lo que desapareció, y lo que se conservó a propósito

**El bombeo de eventos tiene dos contextos, y unificarlos rompía algo.** Al sacarlos a un
solo `pump` dejé el estricto para ambos, y eso habría hecho que **Cancelar dejara de
funcionar durante una transferencia**: la espera del túnel excluye la entrada del usuario a
propósito —por ahí se colaba una recarga de conexiones que dejaba colgando referencias—,
pero `runSsh` la permite justamente para poder cancelar. El enganche lleva ahora un
parámetro que dice en cuál de los dos está.

Desapareció `TransportSession::owner`, el puntero a un objeto con bucle de eventos. Tenía
tres cometidos: ser padre de los `QProcess` —ya no hay—, decidir dónde se puede montar un
túnel, y saber cuándo bombear eventos. Los dos últimos son ahora enganches con nombre:
`pump` y `tunnelsAllowedHere` / `runWhereTunnelsAllowed`.

**El motivo original de la restricción de hilo ya no existe**, y está dicho en el código:
estaba porque los túneles eran `QProcess` colgados de la ventana. Se conserva igualmente
para no cambiar el comportamiento de concurrencia en el mismo paso en que se cambia de
motor — quitarlo permitiría montar túneles desde los hilos de refresco, que es exactamente
el arranque serializado anotado aparte, y merece medirse solo.

También desapareció el recorrido por dos «nombres de par» en el saludo TLS. Existían para
la verificación de nombre de host de Qt; con la fijación del certificado el nombre no
interviene, y el propio código ya avisaba de que tras escribir la petición no se podía
probar el segundo sin **enviar la misma orden otra vez**.

Y con OpenSSL directo se cae el bloque que registraba qué backend TLS estaba activo: era
para diagnosticar el SecureTransport de macOS, que ya no puede intervenir.

### Cómo se verificó

El camino completo, contra un daemon real: SSH → `ssh -L` → TLS con fijación → JSON por
línea. Se usó **la propia máquina como remoto**, porque su material TLS sí se puede leer;
para el código es una conexión SSH cualquiera. Ocho comprobaciones, todas correctas:

| | |
|---|---|
| RPC por el túnel | 243 ms (montaje + saludo) |
| Segunda llamada | 2 ms — el túnel se reutiliza |
| `runSsh` con orden del agente | se desvía al RPC, 2 ms |
| `runSsh` con orden corriente | va por SSH |
| Líneas según llegan | 260 / 1263 / 2267 ms, con cuenta atrás |
| Muerte por SILENCIO | tope 2 s sobre `sleep 20` → corta a los 2092 ms |
| El tope es de INACTIVIDAD | 6 s de trabajo hablando con tope de 2 s → **no** muere |
| Certificado equivocado | la fijación rechaza |

Al terminar aparecieron cuatro `ssh -L` sueltos. **No eran de esta prueba**: llevaban ahí
desde el día anterior, contra otras máquinas, y su padre era `systemd --user` — o sea,
huérfanos que sobrevivieron a la aplicación que los creó. Es exactamente el fallo que el
destructor de `ChildProcess` impide ahora, encontrado por accidente y sirviendo de prueba.

Todo `src/base` compila además para Windows con mingw.

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

1. **`mainwindow_refresh.cpp`** (1.174 líneas, solo 2 métodos de `MainWindow`).
   `connectionstore` ya no ata a Qt en nada esencial: de 1.565 líneas quedan 1.274, y son
   el envoltorio Qt de la clase más el traductor de motivos.
2. **`mainwindow_refresh.cpp`** (1.174, solo 2 métodos de `MainWindow`).
3. A partir de ahí toca desacoplar de `MainWindow`, que es otro tipo de trabajo.

## Barrido de duplicidades

Se buscaron con `grep`, no de memoria: lo que estaba escrito más de una vez y no tenía por
qué. Cinco cosas, y todas eran **contratos del daemon** copiados en los clientes.

| Qué | Estaba en | Ahora |
|---|---|---|
| La carga de `--mutate-rsync-local` | interfaz, intérprete y web, cada uno armando el JSON | `sincronizacion::cargaRsync` |
| `argv` → JSON → base64 | **catorce sitios**: once en la interfaz, dos en el intérprete, uno en la web | `helpers::argvParaAgente` |
| Resolver y conectar con el otro extremo | dos veces en el daemon: emisor de `zfs send` y emisor del árbol | `conectaConElPar` |
| Leer `PORT=`/`TOKEN=` | `transferencia::leeEscucha` y una copia en la web | solo la de la capa base |
| Leer `JOB_ID=` | dos veces en el mismo fichero de la web | `idDeTrabajoEn` |

El criterio no fue «esto se parece» sino **de quién es la decisión**. El orden de los campos
de una carga, cómo se serializa un argv o qué familias se prueban al conectar son cosas del
DAEMON: ninguno de los tres clientes tiene por qué saberlas, y teniéndolas cada uno se
arreglan por separado. `conectaConElPar` es el ejemplo claro: la doble pila IPv6 costó un
diagnóstico entero, y con dos copias el siguiente arreglo entra en una y se olvida en la
otra.

Lo que **no** se unificó, a propósito: la comparación «ya está igual» de `copytree` usa la
fecha exacta y la del árbol remoto la usa en segundos enteros. Parecen la misma regla y no
lo son —una compara dentro de un sistema de ficheros y la otra entre dos distintos—;
juntarlas rompería una de las dos.

### Una trampa del build que conviene saber

`cmake --build builds/linux --target ZFSMgr` **no compila las fuentes de la interfaz**: eso
lo hace el objetivo `zfsmgr_core`. Durante este barrido, once conversiones que no compilaban
dieron «build limpio» con `--target ZFSMgr` y solo salieron al construir `zfsmgr_core`. Para
comprobar un cambio en la interfaz hay que usar `zfsmgr_core` o construir sin `--target`.

## El reparto de `src/` (2026-08-21)

```
src/
  native/    la interfaz de Qt          62 ficheros
  cli/       el intérprete
  web/       el servidor sin JavaScript
  daemon/    el agente que corre como root
  commands/  las OPERACIONES del dominio
  base/      lo que no sabe qué es ZFS
```

Antes, los 62 ficheros de Qt vivían en la raíz de `src/` junto al daemon —9000 líneas de un
programa distinto, que corre como root y no enlaza Qt—, y las operaciones estaban mezcladas
con las primitivas dentro de `base/`.

**La línea entre `base/` y `commands/`**, que es la única que hay que tener clara para que
esto no vuelva a ser un cajón:

- `base/` no sabe qué es ZFS: cadenas, JSON, procesos, TLS, transporte, almacenamiento,
  i18n, y dónde está instalado el agente. Serviría igual en otro programa.
- `commands/` nombra conceptos de éste: copiar, nivelar, mover, sincronizar, desglosar,
  ensamblar, GSA, permisos, peers, el árbol remoto.

**La dependencia va en un solo sentido y se hace cumplir.** La ruta de inclusión de
`zfsmgr_base` es solo `../src/base`: sin `../src`, un `#include "commands/…"` desde `base/`
no resuelve y el build falla en el sitio.

Costó dos intentos dar con el punto donde eso se puede comprobar, y merece quedar escrito
porque lo intuitivo no funciona:

- **Enlazar no lo detecta.** Una biblioteca estática no resuelve símbolos al crearse, y
  cualquier programa que enlace las dos los encuentra. Se probó metiendo en `base/` una
  llamada a `commands::peers::etiquetaDe`: compiló y enlazó sin una queja.
- **Con `../src` en la ruta tampoco.** El include resolvía por `../src/commands/…`.

Lo único que lo hace cumplir es restringir lo que `base/` alcanza a ver.

**Un hallazgo del propio reparto**: al separar las bibliotecas, el build señaló que
`base/helpers.cpp` y `base/transportcmd.cpp` incluían `daemonpayload.h`. Al mirarlo, lo
único que usaban eran `unixBinPath()` y `windowsBinPath()` —dónde está instalado el
agente—, no la composición de cargas. La conclusión no fue tapar la violación sino que
`daemonpayload` **no es una orden**: es disposición de la instalación. Se devolvió a
`base/`. La regla sirvió para lo que se puso: decir dónde estaba de verdad la línea.

## Fase 2, primera entrega: Desglosar, Ensamblar y Hacia Dir

`commands/avanzadas.{h,cpp}` compone el argv de las tres y guarda sus reglas. Los tres
clientes lo llaman; ninguno arma ya esas órdenes por su cuenta.

**Lo que se ganó no es el argv, son las reglas**, que estaban repartidas de tres maneras
distintas para lo mismo:

| | antes | ahora |
|---|---|---|
| Nombre completo del hijo (`assemble`) | comentario en `cli/shell.cpp`, la misma expresión copiada en `web/main.cpp`, y en Qt por un mapa del árbol | `hijoConNombreCompleto`, con pruebas |
| Par a medias en `breakdown` | el web saltaba filas vacías; el CLI y Qt no comprobaban nada | se descarta el par ENTERO |
| Ruta absoluta en `todir` | en ninguna parte | `rutaDeDestinoValida`, y los tres rechazan una relativa |

De las tres, la primera es la que enseña por qué esto importa: el agente comprueba cada hijo
con `zfs list <hijo>`, así que un nombre relativo no existe para él y la operación se saldaba
con «ya absorbido» y **rc=0** —decía que sí sin hacer nada—. Se descubrió ejecutando, no
leyendo, y acabó escrita en dos comentarios y resuelta de una tercera forma.

Que en Qt llegaran ya completos por un mapa del árbol no era que la regla se aplicara: era
que se cumplía por casualidad. Si ese mapa devolviera alguna vez un relativo, la interfaz
habría tenido el fallo sin que nada lo dijera.

**Comprobado ejecutando** sobre un pool de pruebas: desglosar dos directorios por el CLI,
ensamblar por el CLI y por el web **con nombre relativo** en los dos casos, y las tres
guardas nuevas rechazando —hacia dir con ruta relativa, ensamblar sin hijos utilizables—.

**Un residuo que apareció de paso**: `m_pendingEditSeed`, el mecanismo para reabrir el
diálogo de una acción ENCOLADA y re-editarla. Nadie lo activaba ya —murió con la lista de
cambios pendientes— pero seguían ahí sus lecturas. Retirado.

## Fase 2, segunda entrega: pools, instantáneas y datasets

Tres módulos más en `commands/`, y en los tres el valor está en las reglas, no en el argv.

### `commands/pools`

Diez operaciones de mantenimiento. La regla que justifica el módulo:

> **`parar` y `pausar` NO son la misma letra en todas.** En `scrub` son `-s` y `-p`; en
> `trim` e `initialize` son `-c` y `-s`. O sea que **`-s` significa «parar» en scrub y
> «suspender» en initialize**.

Eso no es teoría: el servidor web tenía un botón «Parar initialize» que mandaba `-s`, o sea
que suspendía. El botón decía una cosa y hacía otra. Corregido, y fijado por tests en las
tres operaciones. De paso se añadió «Parar trim», que no se ofrecía.

También guarda el orden —banderas, pool, discos— con sus dos historias de fallo (discos
delante → «invalid character '/' in pool name»; banderas detrás → ignoradas EN SILENCIO), y
qué operaciones se confirman, que no es solo «las que destruyen»: `clear` no borra datos
pero borra la cuenta de errores, y se teclea queriendo limpiar el terminal.

### `commands/instantaneas`

Crear, destruir, rollback, clonar y retener. Dos reglas que se pierden fácil:

- **La etiqueta de una retención va PRIMERO**, al revés de como se dice hablando. Invertirlos
  no da error: `zfs hold` acepta dos cadenas cualesquiera y falla luego diciendo que no
  encuentra la instantánea «micopia».
- **El verbo tipado `--mutate-zfs-hold` NO admite `-r`**: lee exactamente dos parámetros. El
  recursivo tiene que ir por el verbo genérico. El módulo expone las dos formas en vez de
  aceptar un parámetro que se ignoraría.

El alcance de `destroy`/`rollback` pasa de ser las letras sueltas «R», «r» y vacío a un
enumerado con nombres: en una llamada, esas tres letras no se distinguen de un vistazo y son
la diferencia entre borrar una cosa o un subárbol con sus clones.

### `commands/datasets`

Crear, renombrar, montar, promover y propiedades. La regla:

> Un nombre de renombrado **sin barra** significa «cámbiale la hoja, déjalo donde está», así
> que se le antepone el padre.

El intérprete la aplicaba; el servidor web no. Quien tecleaba «fotos» para renombrar
`tank/media/cine` recibía de ZFS «cannot create 'fotos': missing dataset name», que no dice
qué hacer. Comprobado en vivo antes y después: ahora el web renombra con la hoja a secas.

### Verificado ejecutando

Sobre un pool de pruebas de 250 MB: retener y soltar por el CLI —con la comprobación directa
de `zfs holds`—, rechazo de una etiqueta con espacios, clonar, rollback (se comprueba que
desaparece lo escrito después), y renombrado desde el web con nombre a secas.

## Fase 2, tercera entrega: Qt empieza a usar `commands/`

La interfaz construía una **cadena de shell** —`zpool scrub 'tank'`— y luego
`daemonizeZpoolMutationArgs` la volvía a trocear para meterla en el verbo tipado. Ese viaje
argv → cadena → argv es el que el plan técnico señala como origen de fallos: el corte no
respeta comillas, así que un nombre con `;`, `&` o `|` dentro trunca los argumentos.

**La cadena sigue haciendo falta**, pero solo para el camino de RESPALDO por SSH cuando no
hay daemon. Así que no se quita: se invierte el sentido. Ahora el argv se construye primero
—desde `commands::pools`, con sus reglas— y la cadena se deriva de él con
`mwhelpers::cadenaDeArgv`, que entrecomilla TODO, incluidas las banderas. Antes se
entrecomillaba solo el nombre del pool, decidiendo caso por caso qué lo necesitaba; decidirlo
era justamente lo que se hacía mal.

Se añadió `daemonizeZpoolMutationArgs(int, const QStringList&)`, que conserva todas las
comprobaciones de la variante de cadena —daemon instalado, activo, versión de API, y que el
`import` solo va por RPC si ese daemon lo sirve— y se salta el troceo.

**Convertidos los 11 sitios.** Ninguno construye ya la cadena primero.

### La decisión sobre los varios pools

Cinco de ellos no encajaban en el módulo, y la razón era un hallazgo: **la interfaz exponía
operaciones sobre VARIOS pools que el módulo no modela**, porque el intérprete y el servidor
web trabajan siempre sobre uno. `zpool export tank1 tank2` es válido para ZFS.

Se decidió **una llamada por pool**, por ser más simple y uniforme con el resto de clientes.

Tiene una consecuencia que hay que aceptar y que se ve en la vista previa —ahora enseña una
orden por pool en vez de una sola—: **dejan de ser atómicas**. Si la segunda falla, la
primera ya se exportó. Por eso el bucle PARA en el primer fallo y dice cuál falló, en vez de
seguir y dejar un resultado a medias que nadie sabría explicar.

Dos casos NO son una lista y siguen siendo una sola orden, porque no nombran pools:

- `zpool export -a` y `zpool sync` sin argumentos significan «todos».
- `zpool upgrade -v` enumera las características que soporta el sistema: es una LECTURA, no
  la mutación de un pool. No pasa por el módulo, que exige uno; forzarlo habría sido
  distorsionarlo para que cupiera algo que no es una operación sobre un pool.

### Un fallo que apareció al convertir

En `import`, las banderas se construían **ya entrecomilladas** con `shSingleQuote` —porque
iban a una cadena de shell—. Al pasar a argv habrían quedado entrecomilladas DOS veces, y al
daemon le habrían llegado con las comillas dentro del argumento: `-c '/ruta/fichero'` como un
solo argumento con apóstrofos incluidos. Trece banderas afectadas. Se ve fácil al escribirlo
y es invisible después, porque el fallo sería «no encuentro el fichero '/ruta/fichero'» con
las comillas dentro del mensaje.

Importar renombrando gana además la validación del nombre nuevo —`nombreDePoolValido`, la
misma del intérprete y el servidor web—, que en la interfaz no existía: se mandaba a ZFS y
se leía su queja.

## Fase 2, cuarta entrega: instantáneas y datasets en Qt

Mismo patrón que en pools: `daemonizeZfsMutationArgs(int, const QStringList&)` recibe el argv
ya construido, y la cadena —que sigue haciendo falta para el respaldo por SSH— se deriva de
él con `cadenaDeArgv`.

Convertidos: **renombrar**, **destruir**, **clonar** y **rollback**.

Y en cada uno apareció algo que no era solo duplicación:

| Sitio | Lo que había |
|---|---|
| **renombrar** | no aplicaba la regla del nombre sin barra; ahora la hereda de `commands::datasets` |
| **destruir** | pasaba la cadena `"none"` como alcance. Funcionaba **por casualidad**: el daemon solo mira si el valor es «r» o «R», así que cualquier otra cosa significa «ninguno». El módulo devuelve la cadena vacía, que es lo que el verbo espera |
| **clonar** | **no validaba nada**: se mandaba a ZFS y se leía su queja, que además habla de otra cosa cuando el origen no es una instantánea |
| **rollback** | entrecomillaba **a mano** con `'%1'`, sin `shSingleQuote`. Un nombre con un apóstrofo dentro rompía la orden |

El módulo de instantáneas gana `argvZfsClonar`, la forma genérica de clonar, por el mismo
motivo que su gemela de las retenciones: el verbo tipado toma exactamente dos argumentos y
hay pantallas que ofrecen `-p`, `-u` y propiedades.

### Lo que NO se ha tocado, y por qué

`daemonMutationPlanForCommand` sigue existiendo: es el parser que convierte una cadena de
shell en verbo tipado. Ya no lo alimentan los sitios convertidos, pero sí el camino de la
orden libre —donde el usuario escribe la orden— y los productores de `snapshot`, que pasan
por el mecanismo de borradores. Eliminarlo del todo exige que el borrador lleve argv en vez
de cadena, y eso es otra pasada.

Los `QStringLiteral("zfs clone")` que quedan en `mainwindow_dataset_actions.cpp` y
`mainwindow_dialogs.cpp` **no construyen nada**: son comparaciones de texto para decidir si
una orden necesita confirmación reforzada.

## Permisos en Qt: el diff que nadie había probado

Hecho. Y lo interesante no es que ahora use `commands/zfsallow`, sino **por qué no se podía
probar antes**.

El cálculo de qué hay que cambiar —dado lo que había delegado y lo que muestra la ficha,
qué se retira y qué se concede— vivía en una lambda dentro de `applyDatasetPropertyChanges`,
una función de 400 líneas que además toca propiedades, renombrados y refrescos. Para
ejecutar esas veinte líneas había que abrir la ventana, seleccionar un dataset, editar
permisos a mano y pulsar Aplicar. Con eso delante, nadie iba a recorrer las combinaciones:

- cuatro estados por entrada —aparece, desaparece, cambia, sigue igual—,
- por tres alcances —local, descendientes, ambos—,
- más «al crear» y los conjuntos con nombre.

Doce combinaciones, y **ninguna se había comprobado nunca**.

Ahora es `uilogic::permissionChangeCommands(entrada, dataset) → QList<QStringList>`. Es una
función pura: entra la entrada de caché —que lleva los originales y los actuales— y sale la
lista de argv. Once casos en `tests/ui_logic_test.cpp`, uno por combinación.

Dos reglas que el diff tiene que respetar y que solo se ven cuando se escriben los casos:

- **El orden de los permisos no es un cambio.** «create,mount» y «mount,create» son lo
  mismo. Si no se normalizan, cada apertura de la ficha produce un unallow+allow que deja
  todo igual.
- **Cambiar una concesión es retirarla y volver a concederla, en ese orden.** `zfs allow`
  SUMA: sin el `unallow` previo, pasar de «create,mount» a «create» dejaría `mount` puesto.

Y el lote ya no viaja como cadena. Antes se unían con `; `, y al otro lado
`daemonMutationPlanForCommand` volvía a trocear por ese separador —con un corte que no
respeta comillas— para armar el `--mutate-zfs-allow-batch`. Esa rama se ha borrado:
`daemonizeZfsAllowBatchArgs` codifica el argv de cada orden por su cuenta y el lote entero
después, así que **en ningún punto del viaje hay una cadena que alguien tenga que partir**.
La cadena se sigue componiendo, pero solo para la vista previa de la confirmación y para el
respaldo por SSH.

Comprobado contra `tpool` en fc16, aplicando un lote por el propio verbo del daemon y
leyendo con `zfs allow`: los cuatro alcances caen donde deben, el cambio reduce en vez de
sumar, y lo que no cambia no genera orden.

Un detalle del daemon que conviene saber: el lote **no para en el primer fallo**, los
ejecuta todos y devuelve el primer código de error. Es lo correcto aquí —parar tampoco
desharía un `unallow` ya hecho— y la ficha se recarga leyendo el estado real, así que
muestra lo que hay, no lo que se pidió.

## Listados en Qt: una llamada al daemon que se tiraba entera

Esto entró en la lista como «Qt parsea el TSV por su cuenta», que suena a limpieza. No lo
era.

`loadPoolDatasets` pedía `--dump-zfs-list-all` al daemon y luego decidía si la respuesta
servía **mirando si era JSON**:

```cpp
const bool pareceJson = jsonPayload.trimmed().startsWith(QLatin1Char('{'));
const QJsonObject datasets = doc.object().value("datasets").toObject();
if (!datasets.isEmpty()) { loadedFromJson = true; ... }
```

Ese verbo es `zfs list -H -p`: TSV de diez columnas, y nunca ha sido JSON. Así que la
condición no se cumplía jamás, la respuesta del daemon se descartaba y treinta líneas más
abajo el listado se volvía a pedir con `zfs list` por SSH y `sudo`.

**Medido en el registro de la aplicación**, no deducido: 4018 «Loading datasets» y 4006
ejecuciones del `zfs list` de respaldo. Es decir, prácticamente **el 100 % de los listados
de datasets iba por SSH**, y cada uno gastaba además un viaje al daemon que se tiraba. El
árbol de datasets —el camino de lectura más usado de la aplicación— era el único que no
pasaba por el daemon. (Los 83 «Invalid JSON from zfsmgr-zfs-list-all» del registro son de
antes de que se añadiera `pareceJson`: se taparon los avisos, no la rama que los producía.)

Un efecto secundario que se ve en el mismo registro: el respaldo usa `sudo -n`, así que en
una sesión sin credenciales el listado fallaba con «sudo: se requiere una contraseña». Por
RPC no hace falta: el daemon ya es root.

Ahora el TSV que llega se reparte con `commands::listados::entradas` —la misma función que
usa el servidor web, ya cubierta en `base_test.cpp`— y el respaldo por SSH queda para lo que
debía ser: Windows y las conexiones sin daemon al día. Una respuesta vacía no se da por
buena: todo pool tiene al menos su dataset raíz.

Lo que **no** se ha movido, y no debe moverse: montar el árbol. Columnas, iconos, orden y
estado de expansión son de Qt. Lo que sale de Qt es solo el reparto de columnas, que era una
regla de formato con tres copias.

## Desde Dir: lo que no puede ser un RPC, y lo que sí

De las cuatro acciones que mueven contenido, **Desde Dir es la única que no puede ser una
llamada al daemon**, y no por descuido: el verbo `--mutate-advanced-fromdir` lee un tar por
la entrada estándar, y el canal RPC no tiene entrada estándar. Así que la tubería se queda:
las dos puntas por SSH y la máquina de quien manda en medio, que es la que tiene las
credenciales de ambas.

Lo que sí sale de los clientes son sus reglas, que estaban en tres sitios distintos:

- El intérprete y la interfaz componían cada uno la orden de recepción pegando cadenas.
  Ahora las dos llaman a `avanzadas::argvDesdeDir`.
- El subdirectorio de destino lo comprobaba **solo el daemon, al otro extremo de la
  tubería**. Para cuando lo miraba, el `tar` del origen ya estaba corriendo: la operación
  moría a mitad con parte del contenido ya fuera de su máquina. Ahora
  `subdirectorioRelativoValido` lo comprueba antes de abrir nada.
- Y la regla de colocación —dónde cae cada origen dentro del dataset— vivía en cuarenta
  líneas de lambdas dentro de una función de la interfaz.

### El fallo de la regla de colocación

La regla era: un origen va a la raíz; varios, cada uno a un subdirectorio con su nombre; si
dos coinciden, se antepone el nombre de su máquina.

Anteponer la máquina desempata cuando las máquinas son **distintas**. Dos directorios
llamados «docs» de la **misma** conexión daban los dos `fc16-docs`, y los dos tar se
extraían en el mismo sitio: los árboles se mezclan y los ficheros con el mismo nombre se
sobrescriben en silencio.

Comprobado en vivo contra `tpool`, con dos directorios `docs` que contenían cada uno su
`informe.txt`:

```
--- con la regla antigua, informe.txt dice:
informe de B
```

El de A había desaparecido. En una operación cuyo trabajo es copiar, eso es perder contenido
sin decir nada.

`subdirectoriosDeDestino` ahora **garantiza que los destinos son únicos** —el segundo lleva
sufijo— y limpia el nombre resultante: un nombre de conexión con una barra dentro habría
creado un nivel de más, y un `..` habría hecho que el receptor rechazara el destino con el
tar ya en marcha. Diez casos de prueba, uno por variante.

Con la misma comprobación en vivo, los dos orígenes caen ahora en `fc16-docs` y
`fc16-docs-2`, cada uno con su contenido.

### Y después: Desde Dir sin tubería

El servidor web hacía Desde Dir por otro camino —copia de árbol **entre daemons**, sin tar y
sin stdin—. Preguntado si ese camino servía también para la ventana y el intérprete, la
respuesta resultó ser sí, con un hueco de por medio.

**El hueco.** `--tree-recv-listen`, el receptor del árbol, exige que el directorio de destino
ya exista:

```cpp
if (!std::filesystem::is_directory(raiz, ec)) { r.rc = 2; r.err = "no es un directorio: " + raiz; }
```

No lo crea, y por eso el servidor web solo sabía volcar en la raíz del dataset —lo decía su
propio comentario: «crear uno pediría un verbo que hoy no hay»—. Pero la regla de colocación
crea subdirectorios, así que tal cual el árbol no le valía a Desde Dir.

**El verbo que faltaba ya estaba medio escrito.** `--mutate-advanced-fromdir` hace cuatro
cosas: montar el dataset, resolver su punto de montaje efectivo, crear el subdirectorio y
leer un tar por la entrada estándar. Solo la cuarta es la que obliga a la tubería. Partido en
dos, la primera mitad es `--mutate-advanced-fromdir-prepare`, que va por RPC y contesta
`DST=<ruta absoluta>`.

Con eso, Desde Dir entero es RPC:

```
destino:  --mutate-advanced-fromdir-prepare <dataset> <rel>   -> DST=/tpool/desdecli/traido
destino:  --tree-recv-listen DST                              -> PORT=…  TOKEN=…
origen:   [--job-submit] --tree-send-to-peer <dir> <dir> <puerto> <testigo>
```

Los tres pasos viven en `transferencia::lanzaTrabajoDeArbol`, al lado de `lanzaTrabajo`, que
es la misma coreografía para una instantánea. El verbo va **fuera** del `#ifndef _WIN32`:
no usa tar ni nada de Unix, y `getDatasetMountpointCapture` ya consulta los montajes reales
—en Windows la propiedad `mountpoint` dice «/pool/ds» y el montaje de verdad es «Z:\ds»—.

**Qué cambia.**

| | Tubería de tar | Árbol entre daemons |
|---|---|---|
| Por dónde van los datos | por la máquina de quien manda | de máquina a máquina |
| Progreso y cancelación | no | sí, es un trabajo |
| Si se cierra la ventana | muere a mitad | sigue |
| Segunda pasada | reenvía todo | solo lo que cambió |
| Shell | `ssh … tar \| ssh … sudo agente` | ninguno |
| Windows en el origen | tar de Windows | igual que Unix |
| Requiere daemon en… | solo el destino | las dos puntas |

Copiar 100 GB de una máquina a otra movía 200 GB por la de en medio. Ya no.

**Lo que NO se ha borrado, y por qué.** La tubería sigue, de respaldo. El árbol pide daemon
en las dos puntas —al tar le basta con el destino—, y además el destino abre un puerto
efímero al que el origen conecta: donde haya un cortafuegos entre las dos máquinas, SSH pasa
y esto no.

**Y con «borrar los directorios de origen» marcado se usa la tubería a propósito.** Por el
árbol la copia es un trabajo asíncrono, así que el borrado se lanzaría sin saber si el envío
terminó. La tubería encadena con `&&`. Entre perder la mejora y borrar un origen que quizá
no llegó a copiarse, no hay duda.

**Cómo se decide, y por qué no por la versión.** La ventana pregunta por la CAPACIDAD:
`caps::Feature::DirFromDirTree`, que se resuelve contra el `CAPS=` que el agente publica en
`--health`. Añadir un verbo no cambia la versión de API, así que un daemon «al día» en
versión puede no conocerlo; preguntando por el verbo, un agente sin actualizar simplemente se
va por la tubería en lugar de fallar. El intérprete no cachea estado de las máquinas, así que
allí se INTENTA y se cae al respaldo diciendo por qué.

**Una trampa que costó una pasada.** `Trabajo::ok()` exige identificador, y sin encolar no
hay ninguno: el envío síncrono terminaba bien y `ok()` decía que no, así que el intérprete se
iba al respaldo después de haber copiado. Ahí se mira `fallo`. `ok()` significa «hay un
trabajo al que seguirle la pista», no «salió bien».

Comprobado en vivo contra `tpool`: por el intérprete, `fromdir --subdir traido` mueve el
árbol sin tar (`APLICADAS=4 BYTES=22`), el contenido llega correcto, y la segunda pasada
mueve `BYTES=0` con `IGUALES=4`.

## Fase 3 — ningún cliente opera sin agente

La regla nueva: **ningún cliente trabaja con una máquina que no tenga daemon**. Con una
excepción acordada: **instalar el daemon no puede ir por el daemon**, así que el SSH
sobrevive para el arranque —poner el agente en marcha, traer su material TLS, instalar la
clave— y para nada más.

El punto de partida medido: de 59 verbos del agente, 34 se construyen a mano en dos o tres
clientes; Qt tiene 46 bloques de guion embebido (~791 líneas); y **el servidor web no
construye ni una sola cadena de shell**, lo que prueba que se puede y sirve de modelo.

### Lo que ya no está

**541 líneas de shell que no ejecutaba nadie.** `daemonpayload::unixStubScript()` era un
agente entero en `sh` —44 verbos, con cargas de Python incrustadas— y su único llamante era
una prueba. El fichero pasó de 781 a 241 líneas.

**El respaldo del listado de datasets.** Ejecutaba `zfs list` por SSH con sudo, y atendía el
100 % de los listados. De paso, Windows entra ahora por el daemon como los demás: estaba
envuelto en `if (!isWin)`, así que allí ni se intentaba.

**Dos lambdas muertas en transferencias** —`buildSourceExecutionCommand` e `isDaemonReady`,
duplicadas en Copiar y en Nivelar— que nadie invocaba desde que se retiraron esos respaldos,
pero que dejaban un `withSudo` a la vista dando a entender que el camino sabía caer a shell.

**Seis respaldos de mutación.** Crear y soltar un hold, rollback, promover, y el listado de
descendientes de Desglosar y Ensamblar. Todos tenían la misma forma: si el daemon está, verbo
tipado; si no, la orden `zfs` en crudo por SSH. Ahora exigen agente.

### Dos defectos que aparecieron al hacerlo

**Borrar un DATASET no pasaba nunca por verbo tipado.** La condición era
`daemonMutateApiOk && target.contains('@')`, y `'@'` solo lo tienen las instantáneas. O sea
que todo borrado de dataset —incluido `-R`, que arrastra clones y descendientes— salía por
shell. Ahora va por `--mutate-zfs-generic destroy`, comprobado en vivo con un dataset con
hijo.

**`--dump-dir-list` y `--dump-file` estaban muertos en Windows.** Su comprobación de
seguridad —que la ruta caiga dentro de un punto de montaje de ZFS— comparaba contra la
propiedad `mountpoint`, que en Windows dice «/winpool/sa», mientras el montaje real es
«Z:/sa/». Verificado contra OldLau: la propiedad y `zfs mount` dan cosas distintas. Así que
contestaba «la ruta no está dentro de ningún punto de montaje» a cualquier ruta, incluida la
buena — y eso afectaba al navegador de ficheros del servidor web. Es el mismo error de
familia que ya se había corregido en Desglosar y en el árbol de la interfaz: **deducir la
ruta de una propiedad heredable en vez de consultar los montajes de verdad**.

### El intérprete, un guion menos

`ls #content` listaba con `ls -lA` por SSH en Unix y con `Get-ChildItem` en Windows, y su
comentario decía que el agente no tenía verbo para esto. Sí lo tiene —`--dump-dir-list`, el
que usa el servidor web—. Se gana más que quitar dos guiones: los dos formatos eran
**distintos**, así que la misma orden enseñaba unas columnas en Unix y otras en Windows. Y el
reparto del JSON vive ahora en `listados::contenidoDeDirectorio`, compartido con el web.

Comprobado sobre `tpool` y sobre `winpool` de OldLau: la misma tabla en las dos.

### Lo que queda de la fase 3

- El respaldo genérico de `runAgentCommand` (`mainwindow_remote.cpp`), que hay que conservar
  **solo** para los verbos que no pueden ir por RPC porque leen la entrada estándar.
- Los respaldos de rsync en transferencias, y la rama `zfs diff` por shell.
- Dos sitios que llaman a `--health` como cadena en vez de por `runAgentCommand`.
- Tres caminos que hoy NO tienen verbo y por eso siguen siendo shell: las letras de unidad de
  Windows, el listado de directorios de Desglosar en Windows, y «¿hay hijos montados?».

## Fase 4 — que el nombre de un verbo no se escriba en un cliente

El nombre de un verbo —«--dump-zpool-status»— es un contrato entre el daemon y sus tres
clientes, y se escribía a mano en cada uno. Medido antes de tocar nada: de **59 verbos, 34
aparecían literalmente en dos o tres clientes**; veinte en los tres.

Eso no es feo, es frágil de una manera concreta: con el verbo se reparte también **cuántos
argumentos lleva y en qué orden**, y eso no estaba escrito en ninguna parte. Cuando un verbo
gana un argumento, el cliente que no se entera no falla al compilar. Falla en ejecución,
contra una máquina, y con suerte.

`commands/peticiones` es una función por cosa que se le pide. No decide nada más —ni
transporte, ni sudo, ni formato— porque eso sí es de cada cliente: una conexión Local no se
alcanza igual que una remota.

Las mutaciones con reglas propias **no** están ahí: viven donde vive su regla —`pools`,
`instantaneas`, `datasets`, `avanzadas`, `zfsallow`—, porque componer su argv exige saber qué
significa cada bandera. `peticiones` es para lo que no tiene más regla que su nombre.

### Lo que se lleva por delante

Tres cosas que se veían al escribir las funciones y no al leer las llamadas:

- **Un verbo pelado NO se manda.** Si el argumento obligatorio viene vacío se devuelve la
  lista vacía: el daemon contestaría con su línea de uso y un rc=2, que es un error mucho
  peor de leer que no haber preguntado.
- **`--dump-daemon-log` cuenta BYTES, no líneas**, aunque el nombre no lo diga: el daemon hace
  `seek`. Un cliente que pida «las últimas 200» recibe 200 bytes a media palabra.
- **`--dump-zfs-get-prop` lleva la propiedad ANTES que el objeto**, al revés de como se dice
  en voz alta. La función lo pide en ese mismo orden para que no haya que acordarse.

### La lista de verbos encolables tiene un solo dueño

`--job-submit` solo acepta unas pocas mutaciones —las largas—. Esa lista la necesitan el
daemon (para no fiarse del cliente) y los clientes (para decidir antes de pedir nada), así
que la tentación era escribirla dos veces. No: vive en `peticiones::sePuedeEncolar`, y
`daemon_main.cpp` —que enlaza `zfsmgr_commands`— la llama. `isAsyncSubmittableCommand` es una
línea.

### Y las mutaciones, donde vive su regla

Las mutaciones no fueron todas a `peticiones`. Las que tienen regla propia se componen en su
módulo, que es donde está escrito qué significa cada bandera:

- crear, destruir, rollback y clonar una instantánea → `commands/instantaneas`
- renombrar, montar, promover, propiedades → `commands/datasets`
- las diez operaciones de pool → `commands/pools`
- Desglosar, Ensamblar, Hacia Dir, Desde Dir → `commands/avanzadas`

Al hacerlo apareció un hueco: **no había función para crear una instantánea**. Los tres
clientes pegaban `dataset + "@" + nombre` a mano, cada uno con su propia idea de qué nombre
vale. Ahora es `instantaneas::argvCrearInstantanea`, que compone el nombre completo y lo
valida.

En `peticiones` quedaron solo las que no tienen más regla que su forma —los dos genéricos,
las claves, los pares, la escucha, rsync, el lote de permisos—. Y ahí dos de ellas ganaron
algo: **`cargaClave` y `cambiaClave` codifican sus argumentos aquí dentro**. Una frase de paso
en argv la ve cualquiera con un `ps`; dejar la codificación en manos del llamante era invitar
a que un cliente se olvidara.

### Resultado

Verbos del agente escritos literalmente en un cliente:

| | antes | después |
|---|---|---|
| intérprete | 35 | **0** |
| servidor web | 51 | **0** |
| interfaz Qt | 35 | 21 |

De los 21 de Qt, **ninguno construye una llamada**: nueve son el registro de capacidades
—donde el nombre del verbo ES el dato—, cuatro son comprobaciones de texto en
`mainwindow_pools.cpp` para saber si una orden ya fue por RPC, y seis viven dentro de
`daemonMutationPlanForCommand`, el parseo de cadena a argv que desaparece cuando los
borradores lleven argv.

### Una coreografía menos

El servidor web tenía su propia versión de los tres pasos del árbol entre daemons —escuchar,
averiguar la dirección de vuelta, enviar— dentro de `sincroniza`. Era la **tercera copia**, y
la única que además sabía pedir `--delete`. Ahora las tres llaman a
`transferencia::lanzaTrabajoDeArbol`, y el borrado es un parámetro con un comentario que dice
por qué «Desde Dir» lo deja en falso: traer un directorio es AÑADIR, y con el borrado puesto
se llevaría por delante lo que ya hubiera en el destino.

### Verificación

Buena parte de la conversión se hizo con expresiones regulares, y eso se comprueba
ejecutando, no leyendo. Contra `tpool` y las cuatro máquinas: `ls` en los cuatro equipos, las
secciones `#properties`, `#permissions` y `#content`, y el ciclo completo de una instantánea
—crear, clonar, destruir el clon, destruir la instantánea— por el intérprete.

## Fase 5 — los verbos que faltaban para poder borrar el shell de Qt

Tres caminos seguían siendo guion porque «no había verbo». Al mirarlos de cerca, **dos de los
tres ya lo tenían**.

### «¿Hay hijos montados?» — no hacía falta ningún verbo

Antes de desmontar, la interfaz preguntaba si había descendientes montados con un guion
—uno de `sh` con `awk` y otro de PowerShell— ejecutado por SSH **solo para contestar sí o
no**. La respuesta ya estaba en `--dump-zfs-mount`, que el agente sirve en las dos
plataformas: ahora se calcula con `listados::tieneDescendientesMontados`.

Y al escribir su prueba apareció la regla que el guion sí respetaba y que era fácil perder:
el prefijo lleva barra. **«tank/datos2» empieza por «tank/datos» y no está debajo de él**;
sin la barra, desmontar un dataset preguntaría por un hermano con nombre parecido.

### El listado de Desglosar en Windows — el verbo ya existía

`--dump-advanced-breakdown-list` resuelve el punto de montaje con los montajes REALES y
recorre el árbol con `std::filesystem`, o sea que funciona igual en las dos plataformas. Qt
lo usaba solo en Unix y en Windows tenía 55 líneas de PowerShell que hacían lo mismo:
resolver la letra de unidad subiendo por los padres y recorrer con `Get-ChildItem`.

Comprobado contra OldLau antes de cambiar nada: el verbo contesta `__MP__=Z:/sa` y la lista
de directorios. Las 55 líneas se han ido.

### Las letras de unidad — este sí era nuevo

`--dump-zfs-driveletters <pool>`, que era el último camino de shell del refresco. Lleva el
ORIGEN de la propiedad, y eso no es un detalle: en Windows los descendientes heredan la letra
del pool y se montan planos bajo esa unidad, así que dos datasets con la misma letra heredada
es el funcionamiento normal. Sin el origen, cualquier pool con más de un dataset parecía tener
letras duplicadas y ese aviso llenaba el registro en cada refresco.

Comprobado contra OldLau:

```
winpool     Z:  local
winpool/sa  z:  temporary
winpool/sb  z:  temporary
```

Existe también fuera de Windows: allí `zfs` contesta «invalid property 'driveletter'»
—comprobado en macOS— con código distinto de cero, y el cliente lo lee como «no hay letras».
Un verbo que solo existiera en una plataforma obligaría a quien llama a saber en cuál está.

**Y volvió a morder la asimetría de despachos**: el verbo se añadió a la rama RPC y no a la
de terminal, así que el agente contestaba con su línea de uso cuando se le llamaba por el
respaldo. Es la tercera vez en este proyecto. Un verbo nuevo son SIEMPRE dos sitios en
`daemon_main.cpp` más la tabla de capacidades.

### Lo que queda, y qué necesita

Guion embebido en Qt: de ~791 líneas a ~576. Los dos bloques grandes que quedan necesitan
trabajo de verdad, no cableado:

- **Hacia Dir en Windows** (~153 líneas de PowerShell). Ver la sección de abajo: no es
  cableado, hace falta otro algoritmo.
- **Crear pool** (~166 líneas). Ver la sección de abajo: el grueso no era crear, era
  descubrir discos.

### Hacia Dir en Windows: por qué no es un cableado

La suposición era que `--mutate-advanced-todir` vivía bajo `#ifndef _WIN32` porque copiaba con
`rsync`. **No es así**: `runRsyncCopyMoveCapture` conserva el nombre pero por dentro ya usa
`copytree`, la copia propia del agente, que funciona en las dos plataformas.

Lo que de verdad lo ata a Unix es otra cosa. El caso principal de Hacia Dir es convertir un
dataset en un directorio **en su mismo sitio**, y para eso hay que quitar el dataset de en
medio antes de escribir ahí: `AltMountGuard` lo reubica a un montaje temporal con
`zfs set mountpoint=/tmp/...`.

Y eso en Windows no se puede. Comprobado contra OldLau con un dataset de usar y tirar:

```
SET rc=1  cannot set property for 'winpool/prueba': 'mountpoint' must be
          a drive letter, 'none', or 'legacy'
DESPUES: /winpool/prueba
```

**Un dataset de Windows no se puede montar en un directorio.** Solo acepta letra de unidad,
`none` o `legacy`. Así que la coreografía de Unix —aparta el dataset, copia a su antiguo
sitio, destrúyelo— no tiene traducción directa.

Sí la tiene otra, y además ya está escrita en el propio agente: es lo que hace **Ensamblar**.
Copiar primero a un temporal DENTRO del dataset padre —mismo pool, misma unidad, así el paso
final es un renombrado y no una segunda copia—, desmontar el hijo, renombrar el temporal a su
sitio y destruir el dataset. `makeTempDirIn` ya está portado a Windows.

Es trabajo de diseño sobre una operación que **destruye el dataset de origen**, así que se
deja apuntado en vez de improvisado. Mientras tanto la tabla de capacidades sigue rechazando
la acción en Windows con su motivo, que es el comportamiento correcto: no ofrecer lo que no
se puede hacer.

### Crear pool: el grueso no era crear

De las ~166 líneas de guion del diálogo de crear pool, **crear el pool son dos**: `zpool
create` ya se puede mandar por `--mutate-zpool-generic`, que tiene `create` en su lista
blanca. El resto es DESCUBRIR DISCOS, con tres guiones —`lsblk` en Linux, `diskutil` en
macOS, `Get-Partition` en Windows— que producen un formato común que Qt vuelve a analizar.

Y para eso ya había verbo: `--dump-block-devices`. Solo que **estaba portado únicamente a
Linux**; en las otras dos contestaba «no está portado a esta plataforma todavía».

**macOS, portado y comprobado en mmela.** `diskutil list` da los identificadores y
`diskutil info` los datos de cada uno en líneas «clave: valor». Se piden los bytes EXACTOS
—«(494384795648 Bytes)»— y no el «494.4 GB» redondeado: esa cifra no sirve para decidir si un
disco cabe en un pool, y reconvertirla es inventarse precisión que no había.

Al comprobarlo aparecieron **dos formas de marcar como libre un disco que no lo está**, y las
dos son peligrosas porque lo que se ofrece es borrarlo:

1. **El disco de arranque.** La partición que aloja un contenedor APFS no tiene punto de
   montaje ni sistema de ficheros propios —`diskutil` contesta «Not applicable»—, así que con
   la regla de «montado o con fs» salía libre, y su disco entero también por herencia. Lo que
   la delata es `Partition Type: Apple_APFS`. Verificado: `disk0s2` en mmela.
2. **Un disco que respalda un pool importado.** Puede no tener nada de eso: `diskutil` solo
   dice que es un disco. El que respalda `mpool` salía LIBRE.

Lo segundo no se arregla con heurísticas de contenido, así que **se le pregunta a ZFS**:
`zpool status -P` y se marca todo lo que aparezca como vdev. Es la respuesta autoritativa y
vale en las dos plataformas. Con eso, mmela pasó de «25 dispositivos, 2 libres» a «25, 0
libres», que es la verdad: ahí no hay ningún disco libre.

**Windows, portado y comprobado en OldLau.** Allí la herramienta de consulta es PowerShell:
se le llama con `execvp` y un guion CONSTANTE —no se interpola nada de quien pide—, igual que
en macOS se llama a `diskutil`. La regla que importa sigue en pie: quien elige la ruta no
elige además lo que se ejecuta.

Dos cosas que solo se ven ejecutándolo:

- **El agente no encontraba PowerShell.** Corre como tarea programada en la sesión de
  servicios, y allí no está en el PATH: la primera versión contestó «cannot start
  powershell». Va con la ruta absoluta y una lista de candidatos.
- **El cruce con ZFS necesita traducir.** En Windows los vdev no se llaman como los
  dispositivos: `zpool status -P` de un pool sobre el disco 2 dice «/dev/Harddisk2Partition0»
  y el dispositivo es «\\.\PhysicalDrive2». Buscar la ruta tal cual —que es lo que hace la
  rama de macOS— no habría encontrado nada, y el disco que respalda `winpool` habría salido
  libre. Se extrae el número de «HarddiskN» y se marca ese disco.

### Los alias `by-id`, que casi se pierden por el camino

El diálogo ofrece los nombres `by-id` además de los `/dev/sdX`, y **no es un adorno**: un pool
creado con `/dev/sdb` se rompe si mañana el kernel llama `sdc` a ese disco, y con el alias no.
El verbo no los daba, así que cambiarlo por el verbo habría sido una regresión callada.

Ahora los da —Linux y macOS, cada uno donde su sistema los pone— como entradas con `alias` y
`resolved`. Y con eso desaparece además un SEGUNDO guion que el diálogo ejecutaba por SSH para
lo mismo: un bucle de shell con `perl -MCwd=realpath` dentro.

### El diálogo, cableado

Los tres guiones se sustituyen por una llamada al verbo, y su respuesta se traduce a las
mismas columnas que el análisis de abajo ya consumía: alias, rutas resueltas y deduplicado no
se tocan. Los tamaños llegan en bytes y se formatean con la regla que ya usaba el intérprete,
que por eso se ha movido de `cli/tabla.h` a `base/strutil`.

`mainwindow_pool_create.cpp` pasa de ~166 líneas de guion a ~25. El total de guion embebido en
Qt, de ~576 a ~355.

Comprobado de extremo a extremo en fc16: crear un pool sobre fichero por
`--mutate-zpool-generic create` funciona, y **el verbo pasa a marcar ese disco como ocupado en
cuanto el pool existe** —de «1 libre» con el fichero suelto a no ofrecerlo—.

### Desglosar y Ensamblar en Windows: uno sí, el otro no

Los dos tenían su guion de PowerShell en la interfaz mientras el agente ya servía los verbos
en las dos plataformas. Antes de cablearlos se comprobaron contra OldLau con un dataset de
usar y tirar, y salieron dos respuestas distintas.

**Ensamblar funciona.** El verbo devuelve los datasets a directorios en su sitio:
`Z:\des\fotos\a.txt` donde tenía que estar. Cableado, y con eso desaparece también la
condición por plataforma: es el mismo camino en todas.

**Desglosar decía «ok» y movía los datos de sitio.** Esto es lo importante:

```
antes:   Z:\des\fotos\a.txt   Z:\des\musica\b.txt
después: [BREAKDOWN] ok fotos -> winpool/des/fotos
         [BREAKDOWN] ok musica -> winpool/des/musica
         Z:\des\  ...vacío
         Z:\fotos\a.txt      Z:\musica\b.txt      ← en la RAÍZ de la unidad
```

Los ficheros no se pierden —siguen ahí, con sus 8 y 11 bytes— pero ya no están donde el
usuario los tenía: aparecen en el nivel superior del disco. La causa es de la plataforma:
**Windows OpenZFS monta los descendientes planos bajo la letra del pool**, y no admite
fijarles un punto de montaje —`zfs set mountpoint` allí solo acepta letra, `none` o
`legacy`—. No hay forma de que `winpool/des/fotos` se monte en `Z:\des\fotos`.

Y esto llevaba tiempo funcionando así: el comentario de la tabla de capacidades decía que
Desglosar estaba «comprobada contra una máquina» en Windows. Lo estaba a nivel de «el verbo
devuelve cero», no a nivel de «los datos quedan donde el usuario cree».

Mover datos de sitio sin decirlo es peor que no hacer nada, así que:

- el verbo **se niega en Windows** con el motivo escrito;
- el agente **deja de anunciarlo** en `CAPS` allí, así que la tabla de capacidades deshabilita
  la acción antes de que nadie la pulse;
- y la interfaz, si se llega igualmente, lo explica en lugar de intentarlo.

Verificado tras desplegar: `CAPS` en OldLau lista `--mutate-advanced-assemble` y no
`--mutate-advanced-breakdown`, y pedirlo a mano contesta el motivo.

Guion embebido en Qt: de ~355 a ~281 líneas.

### Dos restos más

**La sonda de códecs.** Antes de transferir hay que elegir entre `zstd` y `gzip`, y para eso
se preguntaba a las dos máquinas si los tienen: cuatro sondas por SSH —`command -v` o
`Get-Command`— **cada vez que se abría el diálogo**. El refresco ya pregunta al agente qué
herramientas le faltan y lo guarda; solo faltaba que la lista incluyera esas dos. Ahora la
sonda es mirar el estado que ya está en memoria.

La prueba que guardaba esa lista comprobaba su TAMAÑO —«es corta a propósito»—. Un número
solo dice que alguien la tocó, no si lo que metió tenía sentido, así que ahora comprueba el
contenido y deja escrito cuáles son necesarias y cuáles son para elegir.

**El navegador de ficheros se queda como está**, y a propósito: enseña permisos, dueño,
grupo y fecha, y `--dump-dir-list` solo da nombre, tipo y tamaño. Convertirlo sin ampliar el
verbo perdería columnas —y el marcador de enlace simbólico—. Ampliarlo es trabajo aparte, y
son nueve líneas.

## Fase 6 — el argv por acción



Mover ficheros aclara el árbol pero **no concentra la lógica todavía**. Los tres clientes
siguen armando el mismo argv por su cuenta:

```
cli/shell.cpp:2789          {"--mutate-advanced-assemble", destino.dataset}
web/main.cpp:4755           {"--mutate-advanced-assemble", objeto}
native/mainwindow_advanced.cpp:875   {"--mutate-advanced-assemble", ds}
```

Pequeño por acción, pero son ~20 acciones por 3 clientes, y con el argv se reparten también
las reglas —como que a `assemble` los nombres se le dan COMPLETOS y no relativos, que no
está escrita en ninguna parte y costó una sesión descubrirla—. Eso es lo que debe acabar en
`commands/`, una función por acción, y con tests.
