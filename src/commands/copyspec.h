#pragma once

#include <string>
#include <vector>

#include "zfsmurl.h"

// A DÓNDE copiar y DESDE DÓNDE, resuelto a una sola forma.
//
// `cp` admite cuatro maneras de nombrar un extremo y las cuatro tienen que acabar siendo lo
// mismo: una máquina y una ruta. Esto es la regla que las convierte, y vive aquí —capa base,
// con pruebas— porque de ella depende DÓNDE se escribe. Una regla así enterrada en el
// intérprete es la clase de cosa que este proyecto ya ha pagado cara.
//
//     zfsm://maq/pool/ds                 un dataset entero
//     zfsm://maq/pool/ds#content/sub     un subárbol suyo
//     zfsm://maq#path/var/tmp/x          una ruta suelta de esa máquina
//     /var/tmp/x                         atajo de la máquina local, como ya hace `todir`
//
// Lo que NO se admite se rechaza en vez de interpretarse: una sección que no sea `content`
// ni `path`, un `#path` colgando de un dataset —ahí la ruta ya la da `#content`, y admitir
// las dos sería tener dos formas de decir lo mismo con distinto significado—, y cualquier
// `..` en la subruta.
namespace zfsmgr::base::copyspec {

enum class EndKind {
    Dataset,      // un dataset; la ruta sale de su punto de montaje
    PlainPath,    // una ruta absoluta de la máquina, sin dataset de por medio
};

struct End {
    EndKind kind{EndKind::Dataset};
    std::string connection;   // identificador de la máquina
    std::string zfsName;      // el dataset, vacío si es una ruta suelta
    std::string relative;     // subruta DENTRO del dataset, puede estar vacía
    std::string absolute;     // la ruta suelta, vacía si es un dataset
};

enum class Failure {
    None_,
    NoConnection,        // no se dijo de qué máquina
    BadSection,          // una sección que no nombra ficheros
    PathOnDataset,       // `#path` colgando de un dataset: ambiguo
    NotAbsolute,         // una ruta suelta que no empieza por «/»
    EscapesTree,         // un «..» en la subruta
    Empty,               // no se dijo nada
};

std::string labelOf(Failure f);

struct Reading {
    Failure failure{Failure::None_};
    End end;
    bool ok() const { return failure == Failure::None_; }
};

// `texto` es lo que escribió el usuario; `resuelta` es lo que salió de resolverlo como URL
// —o nada, si el texto era una ruta absoluta suelta—. `conexionLocal` es el identificador
// de la máquina de aquí, para el atajo de la ruta sin URL.
Reading readEnd(const std::string& texto, const ZfsmUrl& resuelta,
                const std::string& conexionLocal);

// --- `#path`: una ruta suelta de la máquina, para TODAS las órdenes, no solo `cp`.
//
// La ruta que nombra una URL `zfsm://maq#path/…`, en la sintaxis de la máquina:
//   #path                 «/»        (en Windows, la lista de unidades)
//   #path/etc/hosts       «/etc/hosts»
//   #path/C:/Users        «C:/Users» (el primer tramo es una unidad de Windows)
// Falla con PathOnDataset si cuelga de un dataset, EscapesTree con un «..», y
// NoConnection sin máquina. NO comprueba que la sección sea «path»: eso es de quien llama.
struct LoosePath {
    Failure failure{Failure::None_};
    std::string connection;
    std::string absolute;
    bool ok() const { return failure == Failure::None_; }
};
LoosePath loosePathOf(const ZfsmUrl& url);

// Al revés: la URL `#path` de una ruta absoluta de la máquina. «C:\x» y «C:/x» dan
// {"C:","x"}; «/» da `#path` sin detalle.
ZfsmUrl urlOfLoosePath(const std::string& connection, const std::string& absolute);

// ¿Es «C:» —una letra y dos puntos—, la unidad de Windows con la que empieza una ruta?
bool isDriveSegment(const std::string& tramo);

// El nombre con el que algo aterriza en el destino: la última pieza del origen, o lo que
// diga `--as`. Vacío cuando no se puede averiguar.
std::string landingName(const End& origen, const std::string& comoSeLlama);

}  // namespace zfsmgr::base::copyspec
