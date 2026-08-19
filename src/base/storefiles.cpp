#include "storefiles.h"

#include "secretcipher.h"

#include <filesystem>
#include <functional>
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

namespace {

// Los campos que van cifrados con la maestra, tal y como se llaman en el JSON. Están aquí
// y no repartidos porque la rotación tiene que tocarlos TODOS: olvidar uno deja la
// configuración medio cifrada con la clave vieja, que es la forma de romperlo sin que se
// note hasta el arranque siguiente.
const char* const kCamposSecretos[] = {
    "password",
    "daemon_tls_server_cert_pem",
    "daemon_tls_client_cert_pem",
    "daemon_tls_client_key_pem",
};

std::string nombreDe(const json::Value& conexion) {
    const std::string n = conexion["name"].isString() ? conexion["name"].toString() : std::string();
    if (!n.empty()) {
        return n;
    }
    return conexion["id"].isString() ? conexion["id"].toString() : std::string("(sin nombre)");
}

// Un campo: se abre con la vieja —si estaba cifrado— y se cierra con la nueva. Un campo en
// claro se CIFRA, que es lo que hace que una configuración a medias quede entera después.
bool rotaCampo(json::Value& conexion, const char* campo, const std::string& vieja,
               const std::string& nueva, Aviso& aviso) {
    if (!conexion[campo].isString()) {
        return true;
    }
    const std::string valor = conexion[campo].toString();
    if (valor.empty()) {
        return true;
    }
    std::string claro = valor;
    if (SecretCipher::isEncrypted(valor)) {
        std::string err;
        if (!SecretCipher::decryptEncv1(valor, vieja, claro, err)) {
            aviso = Aviso{Motivo::NoSeDescifra, nombreDe(conexion), campo, err};
            return false;
        }
    }
    std::string cifrado;
    std::string err;
    if (!SecretCipher::encryptEncv1(claro, nueva, cifrado, err)) {
        aviso = Aviso{Motivo::NoSeCifra, nombreDe(conexion), campo, err};
        return false;
    }
    conexion.set(campo, json::Value(cifrado));
    return true;
}

bool rotaConexiones(json::Value& raiz, const std::string& vieja, const std::string& nueva,
                    Aviso& aviso) {
    if (!raiz["connections"].isArray()) {
        return true;
    }
    json::Array salida;
    for (const json::Value& original : raiz["connections"].toArray()) {
        json::Value conexion = original;
        for (const char* campo : kCamposSecretos) {
            if (!rotaCampo(conexion, campo, vieja, nueva, aviso)) {
                return false;
            }
        }
        salida.push_back(conexion);
    }
    raiz.set("connections", json::Value(salida));
    return true;
}

}  // namespace

namespace {

// Recorre los campos secretos de los dos ficheros. `porCada` decide si se sigue.
bool recorreSecretos(const std::string& dirConfig,
                     const std::function<bool(const json::Value&, const char*, const std::string&)>& porCada,
                     Aviso& aviso) {
    for (int cual = 0; cual < 2; ++cual) {
        Aviso propio;
        const json::Value raiz = (cual == 0) ? leerConfig(dirConfig, propio)
                                             : leerTrustStore(dirConfig, propio);
        if (!propio.vacio()) {
            aviso = propio;
            return false;
        }
        for (const json::Value& conexion : raiz["connections"].toArray()) {
            for (const char* campo : kCamposSecretos) {
                if (!conexion[campo].isString()) {
                    continue;
                }
                const std::string valor = conexion[campo].toString();
                if (valor.empty() || !SecretCipher::isEncrypted(valor)) {
                    continue;
                }
                if (!porCada(conexion, campo, valor)) {
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

bool hayAlgoCifrado(const std::string& dirConfig) {
    bool alguno = false;
    Aviso aviso;
    recorreSecretos(dirConfig,
                    [&alguno](const json::Value&, const char*, const std::string&) {
                        alguno = true;
                        return false;   // con uno basta
                    },
                    aviso);
    return alguno;
}

bool maestraAbreTodo(const std::string& dirConfig, const std::string& maestra, Aviso& aviso) {
    aviso = Aviso{};
    bool ok = true;
    Aviso avisoLectura;
    const bool leido = recorreSecretos(
        dirConfig,
        [&](const json::Value& conexion, const char* campo, const std::string& valor) {
            if (maestra.empty()) {
                aviso = Aviso{Motivo::ClaveMaestraRequerida, nombreDe(conexion), campo, {}};
                ok = false;
                return false;
            }
            std::string claro;
            std::string err;
            if (!SecretCipher::decryptEncv1(valor, maestra, claro, err)) {
                aviso = Aviso{Motivo::NoSeDescifra, nombreDe(conexion), campo, err};
                ok = false;
                return false;
            }
            return true;
        },
        avisoLectura);
    if (!leido && ok) {
        aviso = avisoLectura;
        return false;
    }
    return ok;
}

bool rotaClaveMaestra(const std::string& dirConfig, const std::string& vieja,
                      const std::string& nueva, std::string& copiaSufijo, Aviso& aviso) {
    aviso = Aviso{};
    copiaSufijo.clear();
    if (nueva.empty()) {
        aviso = Aviso{Motivo::NuevaClaveMaestraVacia, {}, {}, {}};
        return false;
    }
    json::Value config = leerConfig(dirConfig, aviso);
    if (!aviso.vacio()) {
        return false;
    }
    Aviso avisoTrust;
    json::Value trust = leerTrustStore(dirConfig, avisoTrust);
    if (!avisoTrust.vacio()) {
        aviso = avisoTrust;
        return false;
    }

    // La copia va ANTES de tocar nada, y de los dos ficheros: si la rotación se parte por
    // la mitad, lo que queda en disco no sirve ni con la clave vieja ni con la nueva.
    copiaSufijo = ".antes-de-rotar";
    std::error_code ec;
    for (const std::string& ruta : {rutaConfig(dirConfig), rutaTrustStore(dirConfig)}) {
        if (!fs::exists(ruta, ec)) {
            continue;
        }
        fs::copy_file(ruta, ruta + copiaSufijo, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            aviso = Aviso{Motivo::ConfigNoSeEscribe, {}, ruta + copiaSufijo, ec.message()};
            copiaSufijo.clear();
            return false;
        }
    }

    if (!rotaConexiones(config, vieja, nueva, aviso)) {
        return false;
    }
    if (!rotaConexiones(trust, vieja, nueva, aviso)) {
        return false;
    }
    if (!escribirConfig(dirConfig, config, aviso)) {
        return false;
    }
    if (trust["connections"].isArray() && !trust["connections"].toArray().empty()) {
        trust.set("schema", json::Value(1));
        trust.set("created_by", json::Value(std::string("ZFSMgr")));
        if (!escribirTrustStore(dirConfig, trust, aviso)) {
            return false;
        }
    }
    return true;
}

}  // namespace zfsmgr::base::store
