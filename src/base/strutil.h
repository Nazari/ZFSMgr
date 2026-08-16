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

// --- Operaciones que en Qt son métodos de QString.
//
// Van como funciones libres a propósito: envolver std::string en una clase con la API
// de QString haría el puerto más cómodo hoy y dejaría al proyecto con un clon casero de
// QString, que es justo lo contrario del objetivo.

// Colapsa cada tira de espacios en uno solo y recorta, como QString::simplified().
std::string simplify(const std::string& s);

// SOLO ASCII, y el nombre lo dice a propósito. Qt cambia también la caja de las letras
// acentuadas; aquí no, y es lo que se quiere: se usan para comparar valores de
// propiedad («yes», «on»), GUID y nombres de verbo, todos ASCII. Una conversión con
// reglas de idioma introduce sorpresas —la I turca es el ejemplo clásico— justo donde
// se está tomando una decisión.
std::string toLowerAscii(const std::string& s);
std::string toUpperAscii(const std::string& s);

bool contains(const std::string& s, const std::string& sub);
bool startsWith(const std::string& s, const std::string& pre);
bool endsWith(const std::string& s, const std::string& suf);

// Devuelven -1 cuando no hay coincidencia, como QString::indexOf().
long long indexOf(const std::string& s, const std::string& sub);
long long lastIndexOf(const std::string& s, const std::string& sub);

// Recortes por CARACTERES, no por bytes, y tolerantes con posiciones fuera de rango.
//
// Contar bytes aquí sería un fallo real, no una imprecisión: `left(s, 220)` es lo que
// recorta las líneas del registro, y cortar a mitad de un carácter UTF-8 deja bytes
// inválidos. Se detectó comparando contra Qt con «áÉ». Coincide con Qt en todo el plano
// básico; solo diverge en caracteres fuera de él, donde Qt cuenta unidades UTF-16.
std::string left(const std::string& s, std::size_t nChars);
std::string mid(const std::string& s, std::size_t posChars);
std::string mid(const std::string& s, std::size_t posChars, std::size_t nChars);

// Índice del byte donde empieza el carácter número `nChars`, o el tamaño si se pasa.
std::size_t byteOfChar(const std::string& s, std::size_t nChars);

// Caja consciente de UTF-8, para cuando el texto NO es ASCII y la decisión depende de
// ello. El caso que lo obligó: `looksLikeSudoAuthFailure` compara contra frases con
// acento, y con «SUDO: 1 INTENTO DE CONTRASEÑA INCORRECTO» la versión ASCII devolvía
// que no había fallo de contraseña. Un rechazo se habría clasificado como «no se pudo
// comprobar», que es el fallo que los comentarios de esa función dicen haber sufrido ya.
//
// Cubre ASCII, el suplemento Latin-1 y Latin Extended-A: español, francés, alemán,
// portugués y buena parte del este de Europa. NO cubre griego, cirílico, la I turca ni
// la expansión ß->SS; ahí devuelve el carácter sin tocar. Contrastado contra Qt en todo
// el rango U+0000..U+017F.
std::string toLowerUtf8(const std::string& s);
std::string toUpperUtf8(const std::string& s);

// ¿Es letra el carácter que empieza en el byte `pos`? ASCII más los mismos rangos
// latinos de arriba.
bool isLetterAt(const std::string& s, std::size_t pos);

// Base64 estándar (RFC 4648) con relleno. `base64Decode` devuelve false si aparece un
// carácter que no pertenece al alfabeto; los espacios se ignoran y el relleno corta.
std::string base64Encode(const std::string& data);
bool base64Decode(const std::string& text, std::string& out);

// `skipEmpty` imita Qt::SkipEmptyParts, que es como se usa en casi todo el código.
std::vector<std::string> split(const std::string& s, const std::string& sep, bool skipEmpty);
std::string join(const std::vector<std::string>& parts, const std::string& sep);

}  // namespace zfsmgr::base
