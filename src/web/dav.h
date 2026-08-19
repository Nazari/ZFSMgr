#pragma once

#include <string>
#include <vector>

// WebDAV, lo justo para que un explorador de archivos monte esto y lo recorra.
//
// **Por qué WebDAV y no un plugin por plataforma.** Finder, Explorer y Dolphin lo montan
// de fábrica; un KIO worker, un backend GVfs y una extensión de espacio de nombres COM son
// tres cosas distintas, y la de Finder además exige entitlements de Apple que este proyecto
// no paga. Ver docs/diseno_tecnico_servidor_web.md.
//
// Aquí solo hay lectura: OPTIONS, PROPFIND, GET y HEAD. Ni LOCK ni PUT ni DELETE — montar
// esto en modo escritura sería otra conversación, y de las serias.
namespace zfsmgr::web::dav {

struct Recurso {
    std::string href;        // la ruta, ya con «/dav/» delante y con barra final si es colección
    std::string nombre;      // lo que enseña el explorador
    bool coleccion{true};
    long long tamano{0};     // solo si no es colección
};

// El XML de una respuesta 207. `profundidad` es la cabecera `Depth`: con «0» solo va el
// primer recurso, con «1» van él y sus hijos.
//
// Se compone a mano y no con una biblioteca de XML: son cuatro etiquetas y una dependencia
// más costaría más que esto. Lo que NO se hace a mano es el escapado.
std::string multiestado(const std::vector<Recurso>& recursos, const std::string& profundidad);

// Escapa para XML. Es el mismo juego que en HTML, pero se nombra aparte porque un día
// podrían divergir y porque quien lo lee tiene que ver que aquí también se escapa.
std::string escapaXml(const std::string& s);

}  // namespace zfsmgr::web::dav
