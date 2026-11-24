#include "osinfo.h"

#include "processes.h"
#include "strutil.h"

#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

#include <sstream>

#include "strutil.h"

namespace zfsmgr::base::osinfo {

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

std::string fromOsRelease(const std::string& contenido) {
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

std::string fromSystemProfiler(const std::string& salida) {
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

namespace {

std::string firstLine(const char* path) {
    std::ifstream f(path);
    std::string line;
    if (f && std::getline(f, line)) {
        return trim(line);
    }
    return {};
}

}  // namespace

std::string machineUuid() {
#ifdef _WIN32
    char buf[128];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &sz)
        == ERROR_SUCCESS) {
        return trim(std::string(buf));
    }
    return {};
#else
    std::string id = firstLine("/etc/machine-id");
    if (!id.empty()) {
        return id;
    }
    id = firstLine("/var/lib/dbus/machine-id");
    if (!id.empty()) {
        return id;
    }
    const ExecResult io = runExecCapture("ioreg", {"-rd1", "-c", "IOPlatformExpertDevice"});
    for (const std::string& line : split(io.out + "\n" + io.err, "\n", true)) {
        const auto pos = line.find("IOPlatformUUID");
        if (pos == std::string::npos) {
            continue;
        }
        const auto eq = line.find('=', pos);
        if (eq == std::string::npos || eq + 1 >= line.size()) {
            continue;
        }
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        val = trim(val);
        if (!val.empty()) {
            return val;
        }
    }
    return {};
#endif
}

}  // namespace zfsmgr::base::osinfo
