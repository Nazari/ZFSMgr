#pragma once

#include <map>
#include <string>
#include <vector>

// El catálogo de propiedades de ZFS que este programa conoce.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md: aquí no hay Qt, así que lo usan por igual la
// interfaz y el intérprete.
namespace zfsmgr::base::zfsprops {

// Las propiedades cuyo valor sale de una lista CERRADA, y esa lista. Vacío para las que no
// la tienen —`quota`, `mountpoint`—, que no es lo mismo que «no existe la propiedad».
const std::map<std::string, std::vector<std::string>>& propiedadesConValores();

// Los valores posibles de una propiedad, o vacío si no tiene lista cerrada.
const std::vector<std::string>& valoresDe(const std::string& propiedad);

}  // namespace zfsmgr::base::zfsprops
