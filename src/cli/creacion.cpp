#include "creacion.h"

#include "strutil.h"

#include <vector>

namespace zfsmgr::cli::creacion {

Decision queSeCrea(Nivel donde, const std::string& texto) {
    Decision d;
    std::string t = zfsmgr::base::trim(texto);
    // An ABSOLUTE name —`/conn/pool` or `zfsm://conn/pool`— starts at the root wherever one
    // is: from inside `unib`, `create /local/tank sda` is a pool on local, not the dataset
    // `local/tank` on unib. It used to be read relative to the current place, and the URL
    // was split at its slashes: «there is no connection named zfsm:».
    bool absoluta = false;
    if (t.rfind("zfsm://", 0) == 0) {
        t = t.substr(7);
        absoluta = true;
    } else if (!t.empty() && t.front() == '/') {
        absoluta = true;
    }
    d.nombre = t;

    // El marcador `@` gana en cualquier nivel: es lo que distingue una instantánea en la
    // URL, así que no hay una regla nueva que recordar.
    if (!t.empty() && t.front() == '@') {
        d.que = Objeto::Snapshot;
        return d;
    }

    const std::vector<std::string> tramos = zfsmgr::base::split(t, "/", true);

    if (donde == Nivel::Raiz || absoluta) {
        // Sin barra es un identificador de conexión. Con barra, el primer tramo nombra la
        // MÁQUINA y lo demás es lo que se crea en ella: un pool si queda un tramo, un
        // dataset si quedan más.
        if (tramos.size() <= 1) {
            d.que = Objeto::Conexion;
            if (absoluta) {
                d.nombre = tramos.empty() ? std::string() : tramos.front();
            }
            return d;
        }
        d.ruta = absoluta ? "/" + tramos.front() : tramos.front();
        d.nombre = zfsmgr::base::join(std::vector<std::string>(tramos.begin() + 1, tramos.end()), "/");
        d.que = tramos.size() == 2 ? Objeto::Pool : Objeto::Dataset;
        return d;
    }

    if (donde == Nivel::Conexion) {
        // Un tramo es el nombre de un pool nuevo; más de uno ya cuelga de un pool.
        d.que = tramos.size() <= 1 ? Objeto::Pool : Objeto::Dataset;
        return d;
    }

    // En un dataset siempre se crea un hijo. Que el nombre lleve barra o no cambia si se
    // toma como relativo al sitio o como nombre ZFS completo, pero eso lo decide quien
    // llama: aquí sigue siendo un dataset.
    d.que = Objeto::Dataset;
    return d;
}

}  // namespace zfsmgr::cli::creacion
