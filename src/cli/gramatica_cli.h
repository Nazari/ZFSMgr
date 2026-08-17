#pragma once

#include <map>
#include <string>
#include <vector>

// El análisis de una línea, en C++.
//
// La gramática vive en `gramatica.y` y el analizador se genera con bison; esto es la
// frontera: convierte el resultado en estructuras de C++ y clasifica cada verbo por su
// FORMA leyéndola del catálogo de `ayuda.cpp`.
//
// Que la forma se derive de la firma declarada es lo que hace ampliable el conjunto: una
// orden nueva es una entrada en el catálogo, no una producción en la gramática.
namespace zfsmgr::cli {

struct LineaAnalizada {
    bool vacia{false};
    bool verboDesconocido{false};
    std::string verbo;
    std::string objetivo;  // vacío = el sitio actual
    std::multimap<std::string, std::string> ranuras;
    std::map<std::string, std::string> opciones;  // --clave [valor]; valor vacío = bandera
    std::vector<std::string> banderas;            // -r, -rf
    // `-o p=v` repetidas: en lista y no en mapa porque el orden importa y la clave se repite.
    std::vector<std::pair<std::string, std::string>> repetidas;
    std::string error;                            // vacío si fue bien
    std::string faltaRanura;                      // qué ranura obligatoria falta, si esa fue la causa

    std::vector<std::string> lista(const std::string& nombre) const {
        std::vector<std::string> v;
        for (auto it = ranuras.lower_bound(nombre); it != ranuras.upper_bound(nombre); ++it) {
            v.push_back(it->second);
        }
        return v;
    }
    std::string uno(const std::string& nombre) const {
        const auto it = ranuras.find(nombre);
        return it == ranuras.end() ? std::string() : it->second;
    }
    // Se acepta con guiones y sin ellos. El léxico guarda las opciones largas SIN los dos
    // guiones —`--wait` se almacena como «wait»— y las banderas cortas CON el suyo, así que
    // preguntar `tiene("--wait")` devolvía siempre false. Ninguna opción larga funcionaba, y
    // no fallaba nada: la orden seguía adelante como si no se hubiera escrito.
    bool tiene(const std::string& bandera) const {
        std::string limpia = bandera;
        while (!limpia.empty() && limpia.front() == '-') {
            limpia.erase(limpia.begin());
        }
        if (opciones.count(limpia) > 0) {
            return true;
        }
        for (const auto& b : banderas) {
            std::string bl = b;
            while (!bl.empty() && bl.front() == '-') {
                bl.erase(bl.begin());
            }
            if (bl == limpia) {
                return true;
            }
        }
        return false;
    }
};

// Analiza una línea del intérprete. Nunca lanza: los errores van en `error`.
LineaAnalizada analizaLinea(const std::string& linea);

}  // namespace zfsmgr::cli
