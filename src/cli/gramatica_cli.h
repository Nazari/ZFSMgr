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
    std::string verbo;
    std::string objetivo;  // vacío = el sitio actual
    std::multimap<std::string, std::string> ranuras;
    std::map<std::string, std::string> opciones;  // --clave [valor]; valor vacío = bandera
    std::vector<std::string> banderas;            // -r, -rf
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
    bool tiene(const std::string& bandera) const {
        for (const auto& b : banderas) {
            if (b == bandera) {
                return true;
            }
        }
        return opciones.count(bandera) > 0;
    }
};

// Analiza una línea del intérprete. Nunca lanza: los errores van en `error`.
LineaAnalizada analizaLinea(const std::string& linea);

}  // namespace zfsmgr::cli
