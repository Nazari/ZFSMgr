#pragma once

#include <map>
#include <string>
#include <vector>

// HTTP/1.1, lo justo: analizar una petición y componer una respuesta.
//
// Escrito a mano y no con una biblioteca porque el proyecto solo enlaza OpenSSL y añadir
// una dependencia por esto sería pagar mucho por poco: aquí no hay ni compresión, ni
// «chunked», ni HTTP/2. Lo que sí hay está comprobado, incluido lo que se RECHAZA.
namespace zfsmgr::web::http {

struct Peticion {
    std::string metodo;   // GET, POST, PROPFIND…
    std::string ruta;     // sin la parte de consulta
    std::string consulta; // lo que va tras «?», sin decodificar
    std::string version;
    std::map<std::string, std::string> cabeceras;  // en minúsculas
    std::string cuerpo;
    bool valida{false};
    std::string porQueNoVale;   // vacío si `valida`

    // Una cabecera, o vacío. El nombre se busca en minúsculas.
    std::string cabecera(const std::string& nombre) const;
    // Una cookie concreta del `Cookie:`.
    std::string cookie(const std::string& nombre) const;
    // Un campo de un cuerpo `application/x-www-form-urlencoded`.
    std::string campo(const std::string& nombre) const;
};

// Analiza. Nunca lanza: una petición malformada devuelve `valida=false` y su motivo.
//
// **Rechaza lo que no entiende en vez de adivinar**: sin línea de petición, sin versión,
// con un `Content-Length` que no es un número o con un cuerpo más corto de lo anunciado.
// Adivinar en un analizador que mira lo que manda un navegador es cómo se cuelan cosas.
Peticion analiza(const std::string& crudo);

// Descodifica `%XX` y `+`. Un `%` mal formado se deja tal cual: perder el dato es peor que
// enseñarlo raro.
std::string desdeUrl(const std::string& s);

// Escapa `& < > " '` para meter texto dentro de HTML.
//
// Todo lo que venga de la configuración o de una máquina pasa por aquí: el nombre de una
// conexión, de un pool o de un dataset lo escribe una persona, y un `<script>` en el nombre
// de un dataset no puede acabar ejecutándose en la página.
std::string escapaHtml(const std::string& s);

struct Respuesta {
    int codigo{200};
    std::string tipo{"text/html; charset=utf-8"};
    std::string cuerpo;
    std::vector<std::string> cabecerasExtra;   // «Nombre: valor», sin el salto
};

// La respuesta entera, lista para el socket.
//
// Lleva siempre las cabeceras que impiden que el navegador haga cosas por su cuenta:
// nada de adivinar el tipo, nada de marcos ajenos, y ningún contenido externo.
std::string componer(const Respuesta& r);

}  // namespace zfsmgr::web::http
