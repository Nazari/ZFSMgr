#pragma once

#include <cstdint>
#include <string>
#include <utility>
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
    // El origen, ESCRITO COMO LO ESCRIBE `zfs get -H -o source`: «local», «default»,
    // «inherited from fc16», «received», «-».
    //
    // El JSON no lo da así: trae `{"type":"DEFAULT","data":"-"}`, y quedarse con `data`
    // —que es lo que se hacía— deja en «-» todo lo que viene por omisión. Ese «-» significa
    // otra cosa: es la marca de una propiedad CALCULADA, como `used` o `creation`. Con las
    // dos cosas escritas igual no había forma de distinguir «se puede cambiar y nadie la ha
    // cambiado» de «esto no se cambia», y el servidor web dejaba de ofrecer la edición de
    // `atime`, `quota` y `recordsize` — todo lo que estuviera por omisión.
    std::string origen;
};

// De `zfs get -j all`. Ordenadas por nombre, que es como se leen.
bool propiedades(const std::string& salida, std::vector<Propiedad>& out, std::string& error);

// Una entrada del contenido de un directorio, de `--dump-dir-list`.
//
// El daemon recorre el directorio él mismo y contesta JSON, y solo si la ruta cae dentro de
// un punto de montaje de ZFS. Antes cada cliente lo listaba por shell —y con DOS formatos
// distintos: `ls -lA` en Unix y `Get-ChildItem` con tabuladores en Windows—, así que la
// misma orden enseñaba columnas distintas según la máquina.
struct EntradaDeDirectorio {
    std::string nombre;
    std::uint64_t tamano{0};
    bool directorio{false};
};

// Las entradas, ordenadas por nombre. Un JSON ilegible SÍ es un error; una lista vacía no:
// un directorio vacío es una respuesta legítima.
bool contenidoDeDirectorio(const std::string& salida, std::vector<EntradaDeDirectorio>& out,
                           std::string& error);

// Un dispositivo de bloque, de `--dump-block-devices`.
//
// `alias` distingue las entradas que son un NOMBRE ALTERNATIVO —los `by-id`— de las que son
// el dispositivo: las primeras solo traen ruta y a qué apuntan. No es un adorno: un pool
// creado con `/dev/sdb` se rompe si mañana el kernel llama `sdc` a ese disco, y con el alias
// no.
struct Dispositivo {
    std::string ruta;
    std::string resuelta;   // a qué apunta un alias; vacío si no lo es
    std::string tipo;       // «disk» o «part»
    std::string fs;
    std::string montaje;
    std::string padre;
    std::uint64_t tamano{0};
    bool enUso{false};
    bool alias{false};
};

bool dispositivos(const std::string& salidaJson, std::vector<Dispositivo>& out,
                  std::string& error);

// Los datasets MONTADOS, de `--dump-zfs-mount`. La clave es el nombre y el valor su punto de
// montaje real —el de verdad, no la propiedad `mountpoint`—.
bool montados(const std::string& salidaJson, std::vector<std::pair<std::string, std::string>>& out,
              std::string& error);

// ¿Hay algún DESCENDIENTE de este dataset montado?
//
// Se contesta con la lista de montajes que ya trae `--dump-zfs-mount`, sin preguntar nada
// más. Antes esto era un guion —uno para Unix y otro para Windows— que se ejecutaba por SSH
// solo para contestar sí o no.
//
// El propio dataset NO cuenta: la pregunta es si desmontarlo va a arrastrar a otros.
bool tieneDescendientesMontados(const std::string& salidaJson, const std::string& dataset);

// De `zpool get -j all`. Es el MISMO formato con otra sección: `zfs` cuelga sus objetos de
// «datasets» y `zpool` de «pools». Se separan en dos funciones y no en un parámetro porque
// quien llama sabe cuál pidió, y un booleano en la llamada no se lee.
bool propiedadesDePool(const std::string& salida, std::vector<Propiedad>& out,
                       std::string& error);

}  // namespace zfsmgr::base::listados
