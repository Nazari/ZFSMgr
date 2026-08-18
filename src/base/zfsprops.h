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

// Una bandera de `zfs send`, tal y como la escribe el usuario.
struct BanderaSend {
    const char* forma;   // "-w"
    bool valor{false};   // ¿lleva un valor detrás? («-X <dataset>»)
    const char* clave{""};  // la clave de traducción de `que`
    const char* que{""};    // qué hace, en una línea, para la ayuda
};

// Las banderas de `zfs send` que se dejan llegar hasta el mandato.
//
// La lista vive AQUÍ y no en el intérprete porque el que tiene que hacerla cumplir es el
// daemon: es él quien construye el argv de `zfs send` y lo ejecuta con privilegios. El
// intérprete es un cliente más, y validar solo en el cliente no valida nada.
//
// Fuera a propósito: `-i` e `-I`, que las pone el programa a partir de `--base`, y `-t`,
// que es el testigo de reanudación. Si el usuario pudiera escribirlas, podría además
// nombrar OTRO dataset en su valor y sacar por el socket algo que nunca pidió.
const std::vector<BanderaSend>& banderasDeSend();

// Comprueba una cadena entera de banderas —«-w -L»— contra esa lista. Si algo no está,
// devuelve false y deja en `mala` el componente culpable.
bool banderasDeSendValidas(const std::string& cadena, std::string& mala);

}  // namespace zfsmgr::base::zfsprops
