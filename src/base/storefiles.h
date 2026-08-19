#pragma once

#include <string>

#include "json.h"
#include "storewarnings.h"

// Lectura y escritura de los dos ficheros del almacén de conexiones, sin Qt.
//
// El directorio se recibe como argumento en vez de calcularse aquí: hoy es
// `~/.config/<app>`, y reimplementar las reglas de cada plataforma para ahorrarse un
// parámetro sería arriesgar que la aplicación deje de encontrar la configuración de la
// gente a cambio de nada.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::store {

std::string rutaConfig(const std::string& dirConfig);
std::string rutaTrustStore(const std::string& dirConfig);

// Que el fichero NO exista no es un aviso: es el primer arranque. Devuelve un objeto
// vacío y `aviso` sin motivo.
json::Value leerConfig(const std::string& dirConfig, Aviso& aviso);
json::Value leerTrustStore(const std::string& dirConfig, Aviso& aviso);

// Escriben con permisos **solo del dueño**, fijados ANTES de volcar el contenido: al
// revés quedaría un instante con el fichero ya lleno de secretos cifrados y los
// permisos que dejara el umask.
bool escribirConfig(const std::string& dirConfig, const json::Value& root, Aviso& aviso);
bool escribirTrustStore(const std::string& dirConfig, const json::Value& root, Aviso& aviso);

// Cambia la CLAVE MAESTRA: descifra con la vieja y vuelve a cifrar con la nueva TODO lo
// que cuelga de ella, en los dos ficheros.
//
// No es «cambiar un valor»: de la maestra cuelgan la contraseña de cada conexión y el
// material TLS del daemon —certificado del servidor, certificado y clave del cliente—, y
// esto último está además en el almacén de confianza. Hacerlo a medias deja campos
// cifrados con la clave vieja, y la sesión siguiente no abre ni las conexiones ni el TLS.
//
// Por eso se escribe primero una COPIA de los dos ficheros —con el sufijo que se devuelve
// en `copiaSufijo`— y solo después se tocan. Si algo falla a mitad, el aviso dice qué
// campo fue y las copias siguen ahí.
//
// Vive en la capa base, y no en la interfaz, porque el intérprete la necesita igual: era
// la última cosa que solo se podía hacer con una ventana delante.
bool rotaClaveMaestra(const std::string& dirConfig, const std::string& vieja,
                      const std::string& nueva, std::string& copiaSufijo, Aviso& aviso);

}  // namespace zfsmgr::base::store
