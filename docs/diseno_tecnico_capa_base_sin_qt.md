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

## Estado

Hecho: `strutil` (`trim`, `replaceAll`, `format`, `shSingleQuote`) y `daemonpayload`.

Siguiente por coste creciente:

1. **`mainwindow_helpers.cpp`** (1.037 líneas, sin métodos de `MainWindow`, ya en el
   espacio de nombres `mwhelpers`). Necesita antes `QRegularExpression` —el enmascarado
   de secretos para el log— y decidir qué hacer con sus cinco usos de `QProcess`.
2. **`connectionstore.cpp`** (1.463). Es una clase propia, no de `MainWindow`, pero usa
   `QSettings` y cifrado; hay que sustituir el almacenamiento.
3. **`mainwindow_refresh.cpp`** (1.174, solo 2 métodos de `MainWindow`).
4. A partir de ahí ya toca desacoplar de `MainWindow`, que es otro tipo de trabajo.
