#pragma once

#include <string>
#include <vector>

#include "json.h"

// Lo que el agente contesta, convertido en estructuras.
//
// Los tres formatos que hay que leer para enseñar un árbol de ZFS:
//
//   `--dump-zpool-list`     JSON de `zpool list -j`
//   `--dump-zfs-list-all`   TSV de diez columnas
//   `--dump-zfs-get-all`    JSON de `zfs get -j all`
//
// Estaban dentro del intérprete, mezclados con la tabla de texto que los pinta. Son
// REGLAS —qué campo es cuál y qué hacer cuando falta—, y el servidor web necesita los
// mismos datos con otra cara. Aquí no se decide nada de presentación: ni orden de
// columnas, ni unidades, ni traducciones.
namespace zfsmgr::base::listados {

struct Pool {
    std::string nombre;
    std::string estado;      // ONLINE, DEGRADED…
    std::string salud;
    std::string tamano;
    std::string libre;
    std::string uso;         // «27%», tal cual lo da zpool
    std::string guid;
};

// De `zpool list -j`.
//
// **Una salida vacía NO es un error**: sin pools, Linux imprime un objeto con «pools»
// vacío y el OpenZFS de macOS —2.4.1— no imprime nada y sale con 0. Tratarlo como JSON
// ilegible decía «respuesta ilegible» en una máquina donde lo único que pasa es que aún no
// hay ningún pool.
bool pools(const std::string& salida, std::vector<Pool>& out, std::string& error);

struct Entrada {
    std::string nombre;
    std::string guid;
    std::string usado;
    std::string compresion;
    std::string cifrado;
    std::string creacion;
    std::string referenciado;
    std::string montado;      // «yes», «no», «-»
    std::string puntoMontaje;
    std::string canmount;

    bool esInstantanea() const { return nombre.find('@') != std::string::npos; }
};

// De `--dump-zfs-list-all`: TSV con diez columnas en este orden —name, guid, used,
// compressratio, encryption, creation, referenced, mounted, mountpoint, canmount—.
//
// Una línea con menos de diez columnas se SALTA en vez de rellenar con vacíos: un punto de
// montaje con un tabulador dentro rompería el reparto, y preferimos perder la fila a
// enseñar los campos corridos.
std::vector<Entrada> entradas(const std::string& salidaTsv);

struct Propiedad {
    std::string nombre;
    std::string valor;
    std::string origen;   // local, default, inherited from…, -
};

// De `zfs get -j all`. Ordenadas por nombre, que es como se leen.
bool propiedades(const std::string& salida, std::vector<Propiedad>& out, std::string& error);

}  // namespace zfsmgr::base::listados
