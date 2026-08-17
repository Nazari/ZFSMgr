#pragma once

#include <string>

// Traducción sin Qt, leyendo los MISMOS ficheros que usa la interfaz.
//
// `i18n/<idioma>.json` con un objeto `translations` de clave a texto. Se reutiliza el
// catálogo que ya existe en vez de inventar uno para el CLI: son los mismos idiomas, los
// mismos ficheros, y ya viajan dentro del instalador y del AppImage. Dos sistemas de
// traducción en un programa acaban discrepando.
//
// **Si no hay traducción, devuelve el texto de partida.** El castellano está escrito en el
// código, así que un catálogo ausente, incompleto o corrupto degrada a «sale en castellano»
// y nunca a «sale una clave» ni a «sale vacío», que es lo que hace ilegible una herramienta.
namespace zfsmgr::base::i18n {

// Qué idioma se usa. Vacío o desconocido = «es». Se acepta «es-ES», «en_US» y demás: se
// queda con las dos primeras letras.
void setLanguage(const std::string& idioma);
const std::string& language();

// Dónde buscar los catálogos, además de los sitios de siempre. Lo pone quien sepa dónde
// está instalado el programa.
void addSearchPath(const std::string& dir);

// El texto traducido, o `porOmision` si no hay nada.
const std::string& tr(const std::string& clave, const std::string& porOmision);

}  // namespace zfsmgr::base::i18n
