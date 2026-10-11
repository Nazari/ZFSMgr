#include "agentversion.h"

#include "zfsmgr/agentversion_generated.h"

#include "strutil.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace zfsmgr::base::agentversion {
namespace {

// La clave de ordenación: mayor, menor, parche, candidato y sufijo de esquema.
//
// El candidato ausente vale 999999 para que «0.93.0» gane a «0.93.0rc1», que es el orden
// de verdad: un rc va ANTES que su final. Vacío = la cadena no tiene la forma esperada.
std::vector<int> clave(const std::string& version) {
    const std::string v = trim(version);
    std::vector<int> out;
    std::size_t i = 0;
    const auto numero = [&](int& destino) {
        const std::size_t ini = i;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            ++i;
        }
        if (i == ini) {
            return false;
        }
        destino = std::atoi(v.substr(ini, i - ini).c_str());
        return true;
    };
    int mayor = 0, menor = 0, parche = 0;
    if (!numero(mayor) || i >= v.size() || v[i] != '.') return {};
    ++i;
    if (!numero(menor) || i >= v.size() || v[i] != '.') return {};
    ++i;
    if (!numero(parche)) return {};
    int rc = 999999;
    if (i + 2 < v.size() && (v[i] == 'r' || v[i] == 'R') && (v[i + 1] == 'c' || v[i + 1] == 'C')) {
        i += 2;
        if (!numero(rc)) return {};
    }
    int sufijo = 0;
    if (i < v.size() && (v[i] == '.' || v[i] == '-')) {
        ++i;
        if (!numero(sufijo)) return {};
    }
    if (i != v.size()) {
        return {};   // sobra algo: no es una versión, y adivinar sería peor
    }
    out = {mayor, menor, parche, rc, sufijo};
    return out;
}

bool esSeparador(unsigned char c) { return !(std::isdigit(c) || c == '.'); }

// ¿Este tramo tiene forma de versión de AGENTE?
//
// Cuatro números y el último de NUEVE dígitos exactos, que es lo que produce el marcador
// de esquema —`string(SUBSTRING "${SHA}" 0 9 ...)` en el CMakeLists—. Sin esa exigencia,
// rebuscar en un binario encuentra cosas como «127.0.0.1», que también son cuatro números
// separados por puntos y no son ninguna versión.
bool pareceVersionDeAgente(const std::string& tramo) {
    const std::size_t ultimo = tramo.find_last_of('.');
    if (ultimo == std::string::npos || tramo.size() - ultimo - 1 != 9) {
        return false;
    }
    const std::vector<int> k = clave(tramo);
    return !k.empty() && k[4] != 0;
}

// La huella ENTERA y SEGUIDA dentro del binario, con un prefijo que se pueda buscar.
//
// Quien mira un binario desde fuera —el empaquetador, `versionInBinary`— busca la huella como
// texto. `return ZFSMGR_AGENT_VERSION_STRING;` no basta: el compilador conoce la cadena y
// puede construir el `std::string` a trozos, con instrucciones inmediatas, y entonces no
// aparece seguida en ninguna parte del fichero. MinGW lo hace: en el CLI de Windows `strings`
// daba «0.99.3.7I» y «0.99.3.7H», y el empaquetador no podía decir de qué compilación era.
//
// Se lee a través de un puntero que el compilador no puede seguir (`asm` vacío), así que la
// cadena tiene que existir de verdad, entera, en los datos del programa.
//
// MSVC —el del CI de Windows— no conoce `__attribute__` ni el `asm` en línea de gcc, y la
// línea dejó de compilar allí. Con él se lee el puntero como `volatile`: tampoco puede dar
// por sabido su valor, así que la cadena tiene que estar entera en los datos.
#if defined(_MSC_VER)
const char kMarcaHuella[] = "ZFSMGR_HUELLA=" ZFSMGR_AGENT_VERSION_STRING;
#else
__attribute__((used)) const char kMarcaHuella[] = "ZFSMGR_HUELLA=" ZFSMGR_AGENT_VERSION_STRING;
#endif

}  // namespace

std::string expected() {
#if defined(_MSC_VER)
    const char* volatile opaco = kMarcaHuella;
    const char* p = opaco;
#else
    const char* p = kMarcaHuella;
    asm volatile("" : "+r"(p));
#endif
    return std::string(p + sizeof("ZFSMGR_HUELLA=") - 1);
}

std::string expectedApi() { return "3"; }

int compare(const std::string& a, const std::string& b) {
    const std::vector<int> ka = clave(a);
    const std::vector<int> kb = clave(b);
    if (ka.empty() || kb.empty()) {
        const std::string sa = toLowerAscii(trim(a));
        const std::string sb = toLowerAscii(trim(b));
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    }
    for (std::size_t i = 0; i < ka.size(); ++i) {
        if (ka[i] != kb[i]) {
            return ka[i] < kb[i] ? -1 : 1;
        }
    }
    return 0;
}

Verdict verdictFor(const std::string& instalada, const std::string& esperada) {
    if (trim(instalada) == trim(esperada)) {
        return Verdict::UpToDate;
    }
    // La parte de publicación: todo menos el último tramo, si hay cuatro.
    const auto publicacion = [](const std::string& v) {
        const std::string t = trim(v);
        std::size_t puntos = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '.' && ++puntos == 3) {
                return t.substr(0, i);
            }
        }
        return t;
    };
    return compare(publicacion(instalada), publicacion(esperada)) > 0 ? Verdict::Ahead
                                                                     : Verdict::Update;
}

std::string versionInBinary(const std::string& ruta) {
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string blob = ss.str();

    // Se recorren TODOS los tramos de dígitos y puntos delimitados, y se queda con el
    // primero que tenga forma de versión de agente —cuatro números—. La de esta
    // compilación gana si aparece, que es el caso corriente y el más fiable.
    const std::string esperada = expected();
    std::string primera;
    std::size_t i = 0;
    while (i < blob.size()) {
        if (esSeparador(static_cast<unsigned char>(blob[i]))) {
            ++i;
            continue;
        }
        const std::size_t ini = i;
        while (i < blob.size() && !esSeparador(static_cast<unsigned char>(blob[i]))) {
            ++i;
        }
        const std::string tramo = blob.substr(ini, i - ini);
        // Solo la forma completa: «may.men.par.sufijo». Tres números son la versión de la
        // aplicación, no la del agente, y confundirlas daría un aviso falso.
        if (tramo == esperada) {
            return tramo;
        }
        if (primera.empty() && pareceVersionDeAgente(tramo)) {
            primera = tramo;
        }
    }
    return primera;
}

}  // namespace zfsmgr::base::agentversion
