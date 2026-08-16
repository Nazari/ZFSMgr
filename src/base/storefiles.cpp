#include "storefiles.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace zfsmgr::base::store {
namespace {

namespace fs = std::filesystem;

void soloElDueno(const std::string& ruta) {
#ifdef _WIN32
    // En Windows los permisos POSIX no significan lo mismo; el fichero queda bajo el
    // perfil del usuario, que ya es privado. std::filesystem no puede expresar una ACL.
    (void)ruta;
#else
    (void)::chmod(ruta.c_str(), S_IRUSR | S_IWUSR);
#endif
}

json::Value leerFichero(const std::string& ruta,
                        Motivo motivoAbrir,
                        Motivo motivoInvalido,
                        Aviso& aviso) {
    aviso = Aviso{};
    std::error_code ec;
    if (!fs::exists(ruta, ec) || ec) {
        // Primer arranque: no hay nada que leer y no hay nada que avisar.
        return json::Value(json::Object{});
    }
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        aviso.motivo = motivoAbrir;
        return json::Value(json::Object{});
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f && !f.eof()) {
        aviso.motivo = motivoAbrir;
        return json::Value(json::Object{});
    }
    json::Value v;
    std::string err;
    if (!json::parse(ss.str(), v, &err) || !v.isObject()) {
        aviso.motivo = motivoInvalido;
        aviso.detalle = err;
        return json::Value(json::Object{});
    }
    return v;
}

bool escribirFichero(const std::string& dirConfig,
                     const std::string& ruta,
                     const json::Value& root,
                     Motivo motivoEscribir,
                     Aviso& aviso) {
    aviso = Aviso{};
    std::error_code ec;
    if (!fs::exists(dirConfig, ec) && !fs::create_directories(dirConfig, ec)) {
        // create_directories devuelve false también cuando el directorio ya existía por
        // una carrera; solo es fallo si de verdad no está.
        if (!fs::exists(dirConfig, ec)) {
            aviso.motivo = Motivo::ConfigDirNoSeCrea;
            return false;
        }
    }
    {
        std::ofstream f(ruta, std::ios::binary | std::ios::trunc);
        if (!f) {
            aviso.motivo = motivoEscribir;
            return false;
        }
        // El permiso va con el fichero ya creado pero todavía VACÍO.
        soloElDueno(ruta);
        const std::string texto = json::toIndented(root);
        f.write(texto.data(), static_cast<std::streamsize>(texto.size()));
        if (!f) {
            aviso.motivo = motivoEscribir;
            return false;
        }
    }
    return true;
}

}  // namespace

std::string rutaConfig(const std::string& dirConfig) {
    return dirConfig + "/config.json";
}

std::string rutaTrustStore(const std::string& dirConfig) {
    return dirConfig + "/trust-store.json";
}

json::Value leerConfig(const std::string& dirConfig, Aviso& aviso) {
    return leerFichero(rutaConfig(dirConfig), Motivo::ConfigNoSeAbre, Motivo::ConfigNoValido, aviso);
}

json::Value leerTrustStore(const std::string& dirConfig, Aviso& aviso) {
    return leerFichero(rutaTrustStore(dirConfig), Motivo::TrustNoSeAbre, Motivo::TrustNoValido, aviso);
}

bool escribirConfig(const std::string& dirConfig, const json::Value& root, Aviso& aviso) {
    return escribirFichero(dirConfig, rutaConfig(dirConfig), root, Motivo::ConfigNoSeEscribe, aviso);
}

bool escribirTrustStore(const std::string& dirConfig, const json::Value& root, Aviso& aviso) {
    return escribirFichero(dirConfig, rutaTrustStore(dirConfig), root, Motivo::TrustNoSeEscribe, aviso);
}

}  // namespace zfsmgr::base::store
