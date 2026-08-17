#include "i18n.h"

#include "json.h"
#include "strutil.h"

#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace zfsmgr::base::i18n {
namespace {

std::mutex g_mutex;
std::string g_idioma = "es";
std::vector<std::string> g_rutas;
// Catálogo por idioma, cargado la primera vez que se pide algo de él.
std::map<std::string, std::map<std::string, std::string>> g_catalogos;

std::string normaliza(const std::string& idioma) {
    const std::string t = toLowerAscii(trim(idioma));
    if (t.size() >= 2) {
        const std::string dos = t.substr(0, 2);
        if (dos == "es" || dos == "en" || dos == "zh") {
            return dos;
        }
    }
    return "es";
}

std::string lee(const std::string& ruta) {
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::map<std::string, std::string> cargaCatalogo(const std::string& idioma) {
    std::map<std::string, std::string> out;
    // Los sitios de siempre, en orden: primero lo que diga quien nos llama, luego el
    // directorio de trabajo. El primero que exista gana.
    std::vector<std::string> candidatos = g_rutas;
    candidatos.push_back("i18n");
    for (const std::string& dir : candidatos) {
        const std::string texto = lee(dir + "/" + idioma + ".json");
        if (texto.empty()) {
            continue;
        }
        json::Value raiz;
        std::string err;
        if (!json::parse(texto, raiz, &err)) {
            continue;  // un catálogo roto no debe tumbar el programa; se sigue sin él
        }
        for (const auto& kv : raiz["translations"].toObject()) {
            const std::string valor = kv.second.toString();
            if (!valor.empty()) {
                out[kv.first] = valor;
            }
        }
        if (!out.empty()) {
            return out;
        }
    }
    return out;
}

}  // namespace

void setLanguage(const std::string& idioma) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_idioma = normaliza(idioma);
}

const std::string& language() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_idioma;
}

void addSearchPath(const std::string& dir) {
    if (trim(dir).empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rutas.push_back(dir);
    g_catalogos.clear();  // una ruta nueva puede traer un catálogo mejor
}

const std::string& tr(const std::string& clave, const std::string& porOmision) {
    // En castellano no hay nada que buscar: es el idioma en el que está escrito el código.
    // Ahorra cargar un catálogo entero para devolver lo que ya se tiene.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_idioma == "es") {
            return porOmision;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto itCat = g_catalogos.find(g_idioma);
    if (itCat == g_catalogos.end()) {
        itCat = g_catalogos.emplace(g_idioma, cargaCatalogo(g_idioma)).first;
    }
    const auto it = itCat->second.find(clave);
    return it == itCat->second.end() ? porOmision : it->second;
}

}  // namespace zfsmgr::base::i18n
