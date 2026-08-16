#pragma once

#include <string>
#include <vector>

// Utilidades de cadena para la capa sin Qt.
//
// Existen para poder sacar lógica del cliente sin arrastrar Qt con ella. Son las
// operaciones concretas que usaba el código portado —ni una más—: recortar, sustituir
// literales, formatear con marcadores posicionales y citar para el shell.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

// Quita espacios en blanco por los dos extremos, como QString::trimmed().
std::string trim(const std::string& s);

// Sustituye TODAS las apariciones de `from` en `s`. No hace nada si `from` está vacío,
// que si no sería un bucle infinito.
void replaceAll(std::string& s, const std::string& from, const std::string& to);

// Formatea sustituyendo %1..%99 por los argumentos, en UNA sola pasada.
//
// La pasada única no es un detalle de implementación, es la semántica: QString::arg()
// con varios argumentos NO vuelve a mirar dentro de lo que acaba de insertar, así que
// un argumento que contenga «%2» se queda literal. Sustituir en cadena rompería
// cualquier cadena con un porcentaje dentro —contraseñas, rutas de Windows— y sería un
// fallo difícil de ver. Comprobado contra Qt: `API='%2%1'`.
//
// Un marcador cuyo número exceda los argumentos dados se deja tal cual, también como Qt.
std::string format(const std::string& tmpl, const std::vector<std::string>& args);

// Cita para pasar como UN argumento a un shell POSIX. La comilla simple se cierra, se
// escapa entrecomillada y se reabre: '"'"' — es la única forma de meterla dentro.
std::string shSingleQuote(const std::string& s);

}  // namespace zfsmgr::base
