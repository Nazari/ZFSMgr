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

}  // namespace zfsmgr::base::store
