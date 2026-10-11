#include "compresion.h"

#include "i18n.h"
#include "strutil.h"

#include <algorithm>
#include <cctype>

// La parte de la compresión que NO necesita las bibliotecas: leer `--comp` y reconocer un
// formato por sus primeros bytes. Va en `zfsmgr_commands` para que el CLI pueda validar la
// opción antes de preguntar nada, sin enlazar zlib, lz4 y zstd, que solo usa el agente.
namespace zfsmgr::compresion {
namespace i18n = zfsmgr::base::i18n;
using zfsmgr::base::format;

namespace {

std::string enMinusculas(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

const char* nombreDe(Formato f) {
    switch (f) {
        case Formato::Gzip: return "gzip";
        case Formato::Lz4: return "lz4";
        case Formato::Zstd: return "zstd";
        case Formato::Ninguno: break;
    }
    return "none";
}

std::string textoDe(const Eleccion& e) {
    if (e.formato == Formato::Ninguno) {
        return "none";
    }
    return std::string(nombreDe(e.formato)) + ":" + std::to_string(e.nivel);
}

bool leeEleccion(const std::string& texto, Eleccion& out, std::string& error) {
    out = Eleccion{};
    const std::string t = enMinusculas(texto);
    const std::size_t dos = t.find(':');
    const std::string nombre = t.substr(0, dos);
    int minimo = 0;
    int maximo = 0;
    if (nombre == "none") {
        if (dos != std::string::npos) {
            error = i18n::tr("t_comp_none_nivel", "«none» no lleva nivel");
            return false;
        }
        return true;
    } else if (nombre == "gzip") {
        out = {Formato::Gzip, 6};
        minimo = 1;
        maximo = 9;
    } else if (nombre == "lz4") {
        out = {Formato::Lz4, 0};
        minimo = 0;
        maximo = 12;
    } else if (nombre == "zstd") {
        out = {Formato::Zstd, 3};
        minimo = 1;
        maximo = 19;
    } else {
        error = format(i18n::tr("t_comp_desconocida",
                                "compresión desconocida «%1»: es gzip, lz4 o zstd"),
                       {texto});
        out = Eleccion{};
        return false;
    }
    if (dos != std::string::npos) {
        const std::string nivel = t.substr(dos + 1);
        const bool soloCifras = !nivel.empty() && nivel.size() <= 2
                                && std::all_of(nivel.begin(), nivel.end(), [](unsigned char c) {
                                       return std::isdigit(c) != 0;
                                   });
        const int n = soloCifras ? std::stoi(nivel) : -1;
        if (n < minimo || n > maximo) {
            error = format(i18n::tr("t_comp_nivel", "el nivel de %1 va de %2 a %3: «%4»"),
                           {nombre, std::to_string(minimo), std::to_string(maximo), nivel});
            out = Eleccion{};
            return false;
        }
        out.nivel = n;
    }
    return true;
}

Formato detecta(const unsigned char* d, std::size_t n) {
    if (n >= 2 && d[0] == 0x1f && d[1] == 0x8b) {
        return Formato::Gzip;
    }
    if (n >= 4 && d[0] == 0x04 && d[1] == 0x22 && d[2] == 0x4d && d[3] == 0x18) {
        return Formato::Lz4;
    }
    if (n >= 4 && d[0] == 0x28 && d[1] == 0xb5 && d[2] == 0x2f && d[3] == 0xfd) {
        return Formato::Zstd;
    }
    return Formato::Ninguno;
}

}  // namespace zfsmgr::compresion
