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

// La familia de sistema de la máquina donde vive el dataset. Importa porque hay
// propiedades que solo existen en una: `jailed` es de FreeBSD, `zoned` de Linux.
enum class Plataforma {
    Linux,
    MacOs,
    FreeBsd,
    Windows,
    Otra,
};

// De lo que se sabe de la máquina —el tipo declarado en el perfil y la línea de `uname`—
// a una familia. Mira las dos juntas: el perfil puede venir sin rellenar.
Plataforma plataformaDe(const std::string& osType, const std::string& osLine);

// Las propiedades del usuario llevan «:» en el nombre. Siempre se pueden escribir: ZFS no
// las interpreta, y este programa guarda ahí su programación (`org.fc16.gsa:*`).
bool esPropiedadDeUsuario(const std::string& prop);

// ¿Existe esa propiedad en esa plataforma? Ofrecer `jailed` en Linux es ofrecer un error.
bool soportadaEn(const std::string& prop, Plataforma p);

// ¿Se puede cambiar el valor de esa propiedad ESCRIBIÉNDOLO encima?
//
// Hace falta más que el nombre: `origen` distingue una propiedad de verdad de una
// calculada —el «-» las marca—, `readonly` es lo que dice el propio ZFS, y el tipo separa
// un sistema de ficheros de un volumen; a una instantánea no se le cambia nada.
//
// **Estaba duplicada LETRA POR LETRA en `mainwindow_dataset_props.cpp` y en
// `mainwindow_dataset_tree.cpp`**, las dos con Qt dentro. No es una regla de interfaz: es
// lo que ZFS deja hacer, y el servidor web necesita exactamente la misma para saber qué
// celda pinta con una caja de edición y cuál no.
bool editableEnLinea(const std::string& prop, const std::string& tipoDataset,
                     const std::string& origen, const std::string& readonly, Plataforma p);

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
