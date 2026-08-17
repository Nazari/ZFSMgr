#pragma once

#include <string>
#include <vector>

// El catálogo de órdenes, como DATOS y no como un bloque de texto.
//
// Esto empezó siendo un `fprintf` de sesenta líneas con la alineación puesta a mano, que
// se descuadraba en cuanto una orden crecía. Pasarlo a datos arregla eso y además da dos
// cosas gratis que con texto no se podían: `help <orden>` y el completado con el
// tabulador, que necesitan saber qué órdenes hay y qué acepta cada una.
namespace zfsmgr::cli {

// Un texto traducible: la clave y el castellano, JUNTOS.
//
// Van en una estructura y no en una lista plana de cadenas alternas a propósito: con la
// lista, olvidar una clave no daba error de compilación, solo hacía que un párrafo
// desapareciera de la ayuda. Pasó, y lo cazó el contraste de la salida en castellano — que
// es justo el fallo que no debe depender de que alguien mire.
struct Texto {
    const char* clave;
    const char* es;
};

struct Parametro {
    // «--from <@instantánea>»: va ENTERA por el catálogo. Mezcla el nombre de la opción
    // —que no cambia nunca— con un marcador que sí se lee; quien traduzca deja el nombre de
    // la opción tal cual y traduce el marcador. Partirla en dos campos obligaría a
    // recomponerla al imprimir sin ganar nada.
    Texto forma;
    Texto que;  // qué hace
};

struct Orden {
    const char* nombre;  // el VERBO: no se traduce, es lo que se teclea
    Texto grupo;
    Texto uso;  // la línea de invocación: ver Parametro::forma
    Texto resumen;
    std::vector<Parametro> params;
    // Lo que solo sale con `help <orden>`: el porqué, las trampas, los ejemplos. En la
    // lista general estorbaría; buscándola a propósito es justo lo que hace falta.
    std::vector<Texto> detalle;
};

// Todas, en el orden en que se enseñan.
const std::vector<Orden>& ordenes();

// La orden con ese nombre, o nullptr.
const Orden* ordenPorNombre(const std::string& nombre);

// La ayuda general, agrupada. `ancho` es el del terminal, para partir las descripciones.
void imprimeAyuda(int ancho);

// La de una sola orden, con su detalle. Devuelve false si no existe.
bool imprimeAyudaDe(const std::string& nombre, int ancho);

// Los nombres que empiezan por ese prefijo. Lo usa el completado.
std::vector<std::string> nombresQueEmpiezanPor(const std::string& prefijo);

// Las opciones de una orden que empiezan por ese prefijo, con el guion delante.
std::vector<std::string> opcionesQueEmpiezanPor(const std::string& orden, const std::string& prefijo);

}  // namespace zfsmgr::cli
