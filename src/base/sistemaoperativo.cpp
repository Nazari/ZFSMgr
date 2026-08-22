#include "sistemaoperativo.h"

#include <sstream>

#include "strutil.h"

namespace zfsmgr::base::sistemaoperativo {

namespace {

std::string sinComillas(std::string x) {
    x = trim(x);
    if (x.size() >= 2) {
        const char a = x.front();
        const char b = x.back();
        if ((a == '\'' && b == '\'') || (a == '"' && b == '"')) {
            x = x.substr(1, x.size() - 2);
        }
    }
    return trim(x);
}

}  // namespace

std::string deOsRelease(const std::string& contenido) {
    std::istringstream iss(contenido);
    std::string linea;
    std::string nombre;
    std::string version;
    while (std::getline(iss, linea)) {
        // Los ficheros que vienen de Windows traen «\r» al final, y sin quitarlo la versión
        // salía con un retorno de carro dentro que la interfaz pintaba como un cuadrito.
        if (!linea.empty() && linea.back() == '\r') {
            linea.pop_back();
        }
        if (startsWith(linea, "NAME=")) {
            nombre = linea.substr(5);
        } else if (startsWith(linea, "VERSION_ID=")) {
            version = linea.substr(11);
        }
    }
    nombre = sinComillas(nombre);
    version = sinComillas(version);
    // `trim` del conjunto y no concatenación a secas: sin versión —Arch, Gentoo— quedaba un
    // espacio suelto al final, que es lo que hacía el `printf` del guion que esto sustituye.
    return trim(trim(nombre) + " " + version);
}

std::string deSystemProfiler(const std::string& salida) {
    std::istringstream iss(salida);
    std::string linea;
    const std::string marca = "System Version:";
    while (std::getline(iss, linea)) {
        if (!linea.empty() && linea.back() == '\r') {
            linea.pop_back();
        }
        const std::size_t pos = linea.find(marca);
        if (pos == std::string::npos) {
            continue;
        }
        // La primera que aparezca, como hacía el `head -1`.
        return trim(linea.substr(pos + marca.size()));
    }
    return {};
}

}  // namespace zfsmgr::base::sistemaoperativo
