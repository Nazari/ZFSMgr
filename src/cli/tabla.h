#pragma once

#include "tr.h"

#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

// Cómo se saca lo que se lista, y una tabla que se sabe imprimir de las tres maneras.
//
// Vive aparte porque lo usan las dos caras del CLI: las órdenes sueltas y el modo
// interactivo. Tenerlo en un solo sitio es lo que evita que `connections list` y `ls`
// acaben imprimiendo distinto.
namespace zfsmgr::cli {

namespace B = zfsmgr::base;

//
// `Texto` es para leer: columnas alineadas y encabezados en el idioma de la aplicación.
// `Tsv` es para guiones: **sin encabezado**, separado por tabuladores, columnas fijas y
// en inglés. Es la misma convención que `zfs list -H`, que quien use esta herramienta ya
// conoce, y por eso no lleva encabezado: una línea de más que hay que saltarse.
//
// Los campos vacíos salen como «-», también como en `zfs`: así el número de columnas no
// cambia y `cut -f4` sigue apuntando a lo mismo.
//
// `Json` es para programas, y aporta lo ÚNICO que tsv no puede dar: **tipos**. Un puerto
// sale como número y no como «22», `sudo` como booleano y no como «true», y lo que no
// aplica sale como `null` en vez de «-». Si se emitiera todo como cadenas, JSON no
// añadiría nada sobre tsv salvo comillas y una dependencia más de quien lo lea.
enum class Formato { Texto, Tsv, Json };

// De qué tipo es una columna. Solo lo usa JSON: texto y tsv lo sacan todo como texto,
// que es lo que son.
enum class Tipo { Cadena, Booleano, Entero, Bytes };

// Una tabla que se sabe imprimir de las tres maneras. Existe para que añadir una orden de
// listado no obligue a escribir tres veces la misma salida y que se separen con el tiempo.
//
// **Las celdas se guardan SIEMPRE en forma canónica**, no en la de salida: un booleano se
// guarda como «true», y es el impresor de texto quien lo traduce a «sí». Antes lo decidía
// quien rellenaba la fila mirando el formato, lo que obligaba a cada orden de listado a
// conocer los tres y a acertar en todos.
struct Tabla {
    std::string nombreJson;                   // la clave de la colección: "connections"
    std::vector<std::string> cabecerasTexto;  // en el idioma de la aplicación
    std::vector<std::string> campos;          // estables, en inglés; los usan tsv y json
    std::vector<Tipo> tipos;                  // por columna; solo lo mira json
    std::vector<std::vector<std::string>> filas;

    Tipo tipoDe(std::size_t i) const { return i < tipos.size() ? tipos[i] : Tipo::Cadena; }

    // Una celda tal y como la ve una PERSONA. Los booleanos van en el idioma de la
    // aplicación; en tsv y en json no, porque «sí» depende del idioma y un guion no
    // debería tener que saberlo.
    std::string celdaTexto(std::size_t col, const std::string& v) const {
        if (tipoDe(col) == Tipo::Booleano) {
            return v == "true" ? T("t_si", "sí") : (v == "false" ? T("t_no", "no") : v);
        }
        if (tipoDe(col) == Tipo::Bytes) {
            return tamanoLegible(v);
        }
        return v;
    }

    // Un tamaño para LEER. En texto y nada más: en tsv y en json el valor va en bytes
    // exactos, porque un guion que compare o sume no puede hacerlo con «1,2 G».
    //
    // Se usan múltiplos de 1024 y las mismas unidades que `zfs list`, para que lo que se ve
    // aquí y lo que se ve allí coincidan.
    static std::string tamanoLegible(const std::string& v) {
        char* fin = nullptr;
        const double n = std::strtod(v.c_str(), &fin);
        if (!fin || *fin != '\0' || v.empty()) {
            return v;  // no es un número: se deja como vino
        }
        static const char* kUnidades[] = {"B", "K", "M", "G", "T", "P", "E"};
        double x = n;
        int u = 0;
        while (x >= 1024.0 && u < 6) {
            x /= 1024.0;
            ++u;
        }
        char buf[64];
        // Una cifra decimal por debajo de 10, ninguna por encima: es lo que hace `zfs`.
        std::snprintf(buf, sizeof(buf), (u == 0 || x >= 10.0) ? "%.0f%s" : "%.1f%s", x,
                      kUnidades[u]);
        return buf;
    }

    void imprimeJson() const {
        // Array VACÍO, no un Value nulo: sin filas se quedaría en `null`, y
        // `jq '.connections[]'` sobre null no da cero elementos, da un error. Es una lista
        // siempre, tenga cero o mil.
        B::json::Value filasJson{B::json::Array{}};
        for (const auto& fila : filas) {
            B::json::Value o;
            for (std::size_t i = 0; i < fila.size() && i < campos.size(); ++i) {
                // Vacío es `null`, NO «-»: el guion es una convención de columnas, que en
                // JSON no hace falta. Un programa que leyera «-» en `port` tendría que
                // saberlo, y es justo lo que este formato viene a evitar.
                if (fila[i].empty()) {
                    o.set(campos[i], B::json::Value());
                    continue;
                }
                switch (tipoDe(i)) {
                    case Tipo::Booleano:
                        o.set(campos[i], B::json::Value(fila[i] == "true"));
                        break;
                    case Tipo::Entero:
                    case Tipo::Bytes:
                        o.set(campos[i], B::json::Value(std::atoll(fila[i].c_str())));
                        break;
                    case Tipo::Cadena:
                        o.set(campos[i], B::json::Value(fila[i]));
                        break;
                }
            }
            filasJson.push(std::move(o));
        }
        // Un objeto en la raíz y no un array suelto: deja sitio para añadir después algo
        // al lado —avisos, la versión— sin que a quien ya lo lea se le rompa el guion.
        B::json::Value raiz;
        raiz.set(nombreJson.empty() ? std::string("items") : nombreJson, std::move(filasJson));
        std::printf("%s", B::json::toIndented(raiz).c_str());
    }

    void imprime(Formato f) const {
        if (f == Formato::Json) {
            imprimeJson();
            return;
        }
        if (f == Formato::Tsv) {
            for (const auto& fila : filas) {
                for (std::size_t i = 0; i < fila.size(); ++i) {
                    std::printf("%s%s", i ? "\t" : "", fila[i].empty() ? "-" : fila[i].c_str());
                }
                std::printf("\n");
            }
            return;
        }
        // Anchos por columna, contando CARACTERES y no bytes: «sí» ocupa tres bytes y
        // dos columnas, y con printf("%-*s") las últimas columnas salían desplazadas.
        const auto anchoVisible = [](const std::string& s) {
            std::size_t n = 0;
            for (const char c : s) {
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                    ++n;  // los bytes de continuación de UTF-8 no ocupan columna
                }
            }
            return n;
        };
        std::vector<std::size_t> ancho(cabecerasTexto.size(), 0);
        for (std::size_t i = 0; i < cabecerasTexto.size(); ++i) {
            ancho[i] = anchoVisible(cabecerasTexto[i]);
        }
        for (const auto& fila : filas) {
            for (std::size_t i = 0; i < fila.size() && i < ancho.size(); ++i) {
                const std::string c = celdaTexto(i, fila[i]);
                ancho[i] = std::max(ancho[i], anchoVisible(c.empty() ? "-" : c));
            }
        }
        auto linea = [&](const std::vector<std::string>& celdas, bool esCabecera) {
            for (std::size_t i = 0; i < celdas.size(); ++i) {
                const bool ultima = (i + 1 == celdas.size());
                const std::string bruta = esCabecera ? celdas[i] : celdaTexto(i, celdas[i]);
                const std::string celda = bruta.empty() ? std::string("-") : bruta;
                std::printf("%s", celda.c_str());
                if (!ultima) {
                    std::printf("%*s", static_cast<int>(ancho[i] - anchoVisible(celda) + 2), "");
                }
            }
            std::printf("\n");
        };
        linea(cabecerasTexto, true);
        for (const auto& fila : filas) {
            linea(fila, false);
        }
    }
};


}  // namespace zfsmgr::cli
