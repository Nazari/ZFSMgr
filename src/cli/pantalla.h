#pragma once

#include <cstddef>
#include <string>
#include <vector>

// La PANTALLA del intérprete con terminal: todo lo que se pinta pasa por aquí.
//
// Tres cosas que con `printf` sueltos no se pueden hacer:
//   - una línea de órdenes que se parte en varias filas, como en bash, en vez de desplazarse
//     de lado con «<»;
//   - repintar lo que ya se ha escrito al cambiar el tamaño de la ventana —las tablas al
//     ancho nuevo, no cortadas al viejo—;
//   - una zona fija abajo, debajo de la línea de órdenes, con el estado.
//
// **Cómo, sin tocar los miles de `printf` que hay repartidos:** al arrancar, la salida
// estándar y la de error se desvían a dos tuberías que lee esta pieza. Lo que llega se pinta
// POR ENCIMA de la zona fija —se borra la zona, se escribe, se vuelve a pintar— y se guarda
// en un historial, que es lo que se repinta al cambiar el tamaño: se borra la pantalla y su
// historial de desplazamiento y se vuelve a escribir todo al ancho nuevo.
//
// Solo con terminal. En un guion, una tubería o la batería no se arranca y todo sigue igual
// que siempre; `--no-tui` o `TERM=dumb` la apagan también con terminal.
//
// La zona fija va ABAJO y no arriba a propósito: un margen superior fijo hace que, en la
// mayoría de terminales, lo que sale por arriba ya no pase a su historial, y se perdería la
// rueda del ratón para ver lo anterior.
namespace zfsmgr::cli::pantalla {

// Arranca si se puede. Devuelve false —y no cambia nada— sin terminal, con TERM=dumb o si la
// consola de Windows no admite secuencias de escape.
bool arranca();
// Deja los descriptores como estaban. Se llama al salir; también desde `atexit`.
void para();
bool activa();

// El ancho y el alto del terminal de verdad, esté o no activa (con la salida desviada,
// preguntárselo a stdout o stderr ya no sirve: son tuberías).
std::size_t anchoTerminal();
std::size_t altoTerminal();

// La línea que se está escribiendo. `cursor` en bytes dentro de `linea`.
void ponEdicion(const std::string& indicador, const std::string& linea, std::size_t cursor);
// Intro: la línea pasa al historial tal cual se veía, y la zona se queda solo con el estado.
void cierraEdicion();

// Las líneas de estado, debajo de la línea de órdenes. Vacío para no enseñar ninguna.
void ponEstado(const std::vector<std::string>& lineas);

// Una pregunta por el terminal (s/N, una contraseña). `pausa` escribe la pregunta DIRECTAMENTE
// —tras vaciar lo que quedara en las tuberías, para que salga en su orden— y deja de repintar
// la zona, que se comería lo que se teclea. `reanuda` guarda pregunta y respuesta en el
// historial; `secreto` dice que no hubo eco, así que el salto de línea lo pone ella.
void pausa(const std::string& pregunta);
void reanuda(const std::string& eco, bool secreto);

// La última línea que salió por la salida de ERROR desde `marcaOrden`: lo que la zona de
// estado enseña como «último error» cuando una orden falla.
void marcaOrden();
std::string ultimaLineaDeError();
// Espera a que lo que quedaba en las tuberías esté pintado (y su último error, anotado).
void sincroniza();

// `cls`: borra la pantalla y el historial guardado.
void limpia();

// Una TABLA, que se guarda como tal y se pinta con bordes al ancho de cada momento: al
// cambiar el tamaño se vuelve a componer, no se parte la que se pintó al ancho viejo.
//
// No se pinta desde aquí: se escribe en la salida estándar como una marca interna (una
// secuencia APC con los datos en JSON) para que llegue EN SU ORDEN con el resto de lo
// impreso, que viaja por la tubería. La pantalla la reconoce al leer.
// `derecha`: columnas que se alinean a la derecha (números, tamaños).
void tabla(const std::vector<std::string>& cabeceras,
           const std::vector<std::vector<std::string>>& filas, const std::vector<bool>& derecha);

// Compone una tabla con bordes para `ancho` columnas. Aparte para poder probarla sin
// terminal. Las celdas que no caben se parten en varias líneas dentro de su columna,
// estrechando antes las columnas más anchas. Una fila con menos celdas que cabeceras
// extiende la última hasta el final (los alias de `devices`).
std::vector<std::string> componTabla(const std::vector<std::string>& cabeceras,
                                     const std::vector<std::vector<std::string>>& filas,
                                     const std::vector<bool>& derecha, std::size_t ancho,
                                     bool unicode);

}  // namespace zfsmgr::cli::pantalla
