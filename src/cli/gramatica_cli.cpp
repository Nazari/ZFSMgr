#include "gramatica_cli.h"

#include "ayuda.h"
#include "gramatica_ast.h"
#include "i18n.h"

namespace zfsmgr::cli {
namespace {

// ¿Lleva valor esta opción larga? Sale del catálogo: la línea de un parámetro es
// «--delete» —suelta— o «--name <n>» / «--password-fd <n>» —con valor—, así que basta con
// mirar si tras el nombre viene algo entre ángulos.
//
// `--on` y `--from` valen en todas y siempre llevan valor.
extern "C" int llevaValorLaOpcion(const char* verbo, const char* opcion, void* /*ctx*/) {
    // Sin guiones, vengan como vengan: el léxico entrega las largas ya peladas —«wait»— y
    // las cortas enteras —«-r»—. Comparar sin normalizar hacía que ninguna corta con valor
    // se reconociera: `trim -r 100M` mandaba «100M» a la ranura del disco y el pool acababa
    // de argumento de `-r`.
    std::string op = opcion ? opcion : "";
    while (!op.empty() && op.front() == '-') {
        op.erase(op.begin());
    }
    if (op == "on" || op == "from") {
        return 1;
    }
    const Orden* o = ordenPorNombre(verbo ? verbo : "");
    if (!o) {
        return 0;
    }
    // Las banderas del mandato original: la lista dice cuáles llevan valor.
    for (const Nativa& n : o->nativas) {
        std::string f = n.forma;
        while (!f.empty() && f.front() == '-') {
            f.erase(f.begin());
        }
        if (f == op) {
            return n.valor ? 1 : 0;
        }
    }
    for (const Parametro& par : o->params) {
        const std::string forma = par.forma.es;
        const std::size_t i = forma.find("--" + op);
        if (i == std::string::npos) {
            continue;
        }
        // La coincidencia tiene que ser la OPCIÓN ENTERA, no un prefijo suyo.
        //
        // Buscar la subcadena hacía que `--password` casara dentro de
        // «--password-fd <n>»: el tramo que se mira era «-fd <n>», con su «<», así que el
        // léxico daba `--password` por opción CON valor. Y como en `edit local --password`
        // no hay nada detrás, la opción se perdía entera: la orden se ejecutaba sin
        // preguntar la contraseña y decía «connection local updated», que es justo lo que
        // uno no quiere de una orden que existe para cambiar la contraseña.
        //
        // El nombre acaba donde deja de ser nombre: fin de cadena, espacio o barra.
        const std::size_t finNombre = i + op.size() + 2;
        if (finNombre < forma.size()) {
            const char sig = forma[finNombre];
            const bool sigueElNombre =
                (sig >= 'a' && sig <= 'z') || (sig >= 'A' && sig <= 'Z')
                || (sig >= '0' && sig <= '9') || sig == '-' || sig == '_';
            if (sigueElNombre) {
                continue;
            }
        }
        // Lo que sigue al nombre HASTA la siguiente opción: si ahí aparece un «<», lleva
        // valor. Se mira el tramo entero y no solo el carácter siguiente porque la forma
        // puede escribirse de varias maneras —«--name <n>», «--set @<nombre>»,
        // «--user <u> / --group <g>»— y mirar solo el carácter de al lado dejaba fuera
        // `--set`, que sí lo lleva: el valor se tragaba como si fuera un argumento.
        const std::size_t tras = i + op.size() + 2;
        const std::size_t siguiente = forma.find(" / ", tras);
        const std::string tramo =
            forma.substr(tras, siguiente == std::string::npos ? std::string::npos : siguiente - tras);
        if (tramo.find('<') != std::string::npos) {
            return 1;
        }
    }
    return 0;
}

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
    zfsmCliAnaliza(linea.c_str(), &llevaValorLaOpcion, nullptr, &a);
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
    // Las banderas cortas AGRUPADAS se reparten aquí: `-wLecR` son cinco.
    //
    // Es como se escriben de verdad —`zfs send -wLecR`—, y getopt las acepta desde
    // siempre. Quien las teclea así no está usando una abreviatura del programa: está
    // usando la sintaxis del mandato que hay debajo, y rechazarla obligaba a escribirlas
    // de una forma distinta a la del manual de OpenZFS.
    //
    // Se reparte solo si TODAS las letras están declaradas para esa orden y NINGUNA lleva
    // valor. Si alguna lo lleva, el grupo se deja tal cual: `-d /dev` dentro de un grupo
    // significa cosas distintas según dónde esté el valor, y adivinarlo sería inventar. El
    // grupo entero llega entonces a la validación y el usuario ve qué escribió.
    const Orden* ordenNativas = ordenPorNombre(out.verbo);
    const auto declaradaSinValor = [&](char letra) {
        if (!ordenNativas) {
            return false;
        }
        for (const Nativa& n : ordenNativas->nativas) {
            const std::string f = n.forma;
            if (f.size() == 2 && f[0] == '-' && f[1] == letra) {
                return !n.valor;
            }
        }
        return false;
    };
    for (int i = 0; i < a.nBanderas; ++i) {
        const std::string b = a.banderas[i];
        bool agrupada = b.size() > 2 && b[0] == '-' && b[1] != '-';
        if (agrupada) {
            for (std::size_t k = 1; k < b.size(); ++k) {
                if (!declaradaSinValor(b[k])) {
                    agrupada = false;
                    break;
                }
            }
        }
        if (agrupada) {
            for (std::size_t k = 1; k < b.size(); ++k) {
                out.banderas.push_back(std::string("-") + b[k]);
            }
        } else {
            out.banderas.push_back(b);
        }
    }
    for (int i = 0; i < a.nRepetidas; ++i) {
        out.repetidas.emplace_back(a.repetidas[i].nombre, a.repetidas[i].valor);
    }
    astLibera(&a);
    return out;
}

}  // namespace zfsmgr::cli
