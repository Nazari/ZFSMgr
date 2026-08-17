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

struct Parametro {
    const char* forma;  // «--from <@instantánea>»
    const char* que;    // qué hace
};

struct Orden {
    const char* nombre;
    const char* grupo;
    const char* uso;      // la línea de invocación, sin el nombre repetido
    const char* resumen;  // una línea
    std::vector<Parametro> params;
    // Lo que solo sale con `help <orden>`: el porqué, las trampas, los ejemplos. En la
    // lista general estorbaría; buscándola a propósito es justo lo que hace falta.
    std::vector<const char*> detalle;
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
