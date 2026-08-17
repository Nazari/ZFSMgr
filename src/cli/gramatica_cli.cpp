#include "gramatica_cli.h"

#include "ayuda.h"
#include "gramatica_ast.h"
#include "gramatica.tab.h"

namespace zfsmgr::cli {
namespace {

// La FORMA de un verbo, derivada de su firma declarada.
//
// Aquí está el motivo de que la gramática tenga una producción por forma y no una por
// verbo: mientras una orden nueva encaje en una de estas formas, no hay que tocar ni la
// gramática ni este fichero. Y si no encaja, el compilador no dice nada pero el análisis
// falla en la primera prueba, que es donde se quiere que falle.
int formaDe(const Orden& o) {
    const auto tipo = [&o](std::size_t i) {
        return i < o.ranuras.size() ? o.ranuras[i].tipo : Ranura::Tipo::Texto;
    };
    const std::size_t n = o.ranuras.size();
    switch (o.objetivo) {
        case Objetivo::Ninguno:
            return n == 0 ? V_NADA : V_TEXTO;
        case Objetivo::Cualquiera:
            return V_CUALQUIERA;
        case Objetivo::Conexion:
            return n == 0 ? V_CONN : V_CONN_TEXTO;
        case Objetivo::Pool:
            if (n == 0) {
                return V_POOL;
            }
            if (tipo(0) == Ranura::Tipo::Palabra) {
                return n > 1 ? V_POOL_FASE_VDEV : V_POOL_FASE;
            }
            return V_POOL_VDEV;
        case Objetivo::Dataset:
        case Objetivo::DatasetOInstantanea:
            if (n == 0) {
                return V_DS;
            }
            switch (tipo(0)) {
                case Ranura::Tipo::Propiedad: return V_DS_ASIGNA;
                case Ranura::Tipo::Url: return V_DS_URL;
                case Ranura::Tipo::Ruta: return V_DS_RUTA;
                default: break;
            }
            return o.ranuras[0].cuantas == Ranura::Cuantas::Opcional ? V_DS_TEXTO_OPC
                                                                     : V_DS_TEXTO_MAS;
        case Objetivo::Instantanea:
            if (n == 0) {
                return V_SNAP;
            }
            return tipo(0) == Ranura::Tipo::Url ? V_SNAP_URL : V_SNAP_TEXTO;
    }
    return V_NADA;
}

extern "C" int claseDeVerbo(const char* verbo, void* /*ctx*/) {
    const Orden* o = ordenPorNombre(verbo);
    // Un verbo desconocido se analiza como si no tomara nada: así el error que ve el usuario
    // es «orden desconocida» y no un fallo de sintaxis, que no le diría nada.
    return o ? formaDe(*o) : V_NADA;
}

}  // namespace

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

LineaAnalizada analizaLinea(const std::string& linea) {
    LineaAnalizada out;
    AnalisisCli a;
    zfsmCliAnaliza(linea.c_str(), &claseDeVerbo, nullptr, &a);
    if (a.error) {
        // «syntax error» no le dice nada a nadie. Casi siempre lo que falta es una ranura
        // obligatoria, y la firma ya sabe cuál y cómo se escribe la orden: se dice eso.
        const Orden* o = ordenPorNombre(primeraPalabra(linea));
        out.error = a.error;
        if (o) {
            for (const Ranura& r : o->ranuras) {
                if (r.cuantas == Ranura::Cuantas::Una || r.cuantas == Ranura::Cuantas::UnaOMas) {
                    out.error = std::string("falta <") + r.nombre + ">";
                    out.faltaRanura = r.nombre;
                    break;
                }
            }
        }
    }
    out.vacia = a.vacia != 0;
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
    astLibera(&a);
    return out;
}

}  // namespace zfsmgr::cli
