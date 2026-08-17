#include "gramatica_cli.h"

#include "ayuda.h"
#include "gramatica_ast.h"
#include "i18n.h"

namespace zfsmgr::cli {
namespace {

// La primera palabra de la línea, para poder hablar del verbo cuando el análisis falló
// antes de reconocerlo.
std::string primeraPalabra(const std::string& linea) {
    std::size_t i = linea.find_first_not_of(" \t");
    if (i == std::string::npos) {
        return {};
    }
    const std::size_t j = linea.find_first_of(" \t", i);
    return linea.substr(i, j == std::string::npos ? std::string::npos : j - i);
}

}  // namespace

LineaAnalizada analizaLinea(const std::string& linea) {
    LineaAnalizada out;
    AnalisisCli a;
    zfsmCliAnaliza(linea.c_str(), &a);
    if (a.error) {
        // «syntax error» no le dice nada a nadie. Casi siempre lo que falta es una ranura
        // obligatoria, y la firma ya sabe cuál y cómo se escribe la orden: se dice eso.
        // «syntax error» no le dice nada a nadie. Si el verbo se reconoce, lo útil es su
        // línea de sintaxis —que ya está en el catálogo— y, si lo que falta es una ranura
        // obligatoria, cuál es. Antes cada orden llevaba su propio texto de «uso:» escrito
        // a mano, y por eso unas lo tenían y otras no.
        const Orden* o = ordenPorNombre(primeraPalabra(linea));
        out.error = a.error;
        if (o) {
            for (const Ranura& r : o->ranuras) {
                if (r.cuantas == Ranura::Cuantas::Una || r.cuantas == Ranura::Cuantas::UnaOMas) {
                    out.faltaRanura = r.nombre;
                    break;
                }
            }
            out.error = std::string("uso: ") + o->nombre + " "
                        + zfsmgr::base::i18n::tr(o->uso.clave, o->uso.es);
            if (!out.faltaRanura.empty()) {
                out.error += "  (falta <" + out.faltaRanura + ">)";
            }
        }
    }
    out.vacia = a.vacia != 0;
    out.verboDesconocido = a.verboDesconocido != 0;
    if (a.verbo) {
        out.verbo = a.verbo;
    }
    if (a.objetivo) {
        out.objetivo = a.objetivo;
    }
    for (int i = 0; i < a.nRanuras; ++i) {
        out.ranuras.emplace(a.ranuras[i].nombre, a.ranuras[i].valor ? a.ranuras[i].valor : "");
    }
    for (int i = 0; i < a.nOpciones; ++i) {
        out.opciones[a.opciones[i].nombre] = a.opciones[i].valor ? a.opciones[i].valor : "";
    }
    for (int i = 0; i < a.nBanderas; ++i) {
        out.banderas.push_back(a.banderas[i]);
    }
    for (int i = 0; i < a.nRepetidas; ++i) {
        out.repetidas.emplace_back(a.repetidas[i].nombre, a.repetidas[i].valor);
    }
    astLibera(&a);
    return out;
}

}  // namespace zfsmgr::cli
