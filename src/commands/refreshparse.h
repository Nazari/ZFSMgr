#pragma once

#include <map>
#include <string>
#include <vector>

// Análisis de las respuestas que da el agente al refrescar una conexión, sin Qt.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::refresh {

// Las herramientas que la aplicación comprueba en el host remoto.
//
// La lista es corta a propósito. Antes se sondeaban también awk, grep, sort, find,
// mktemp, printf, cat, gzip, pv y sudo: las necesitaban las tuberías de shell que la
// aplicación enviaba, y de las que el daemon no usa ninguna. Eran residuo del modelo
// anterior, y sondearlas solo servía para mostrar una lista de «faltantes» que no
// afectaba a nada.
std::vector<std::string> zfsmgrUnixCommandSet();

// Minúsculas y sin las llaves que pone el registro de Windows.
std::string normalizeMachineUuid(std::string s);

// Saca un identificador de máquina de una salida en texto libre: primero con guiones,
// luego los 32 dígitos seguidos, y si no hay ninguno se queda con la primera línea.
std::string extractMachineUuid(const std::string& text);

// «CLAVE=valor» por línea. La clave se pasa a MAYÚSCULAS; el valor se conserva tal cual
// tras recortarlo, porque puede llevar cualquier cosa —incluido un '='—.
std::map<std::string, std::string> parseKeyValueOutput(const std::string& text);

struct PoolGuidStatus {
    std::string guid;
    std::string status;
};

// Trocea la respuesta por lotes del estado de los pools, delimitada por marcadores
// `__ZFSMGR_*__`. Las líneas de estado se conservan SIN recortar cada una: la sangría de
// `zpool status` es parte de lo que se muestra.
std::map<std::string, PoolGuidStatus> parsePoolGuidStatusBatch(const std::string& text);

}  // namespace zfsmgr::base::refresh
