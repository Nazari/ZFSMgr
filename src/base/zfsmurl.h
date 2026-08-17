#pragma once

#include <string>
#include <vector>

// `zfsm://` — nombrar cualquier elemento del árbol.
//
//     zfsm://<conexión>/<pool>/<dataset>[@snapshot][#<sección>[/<detalle>]]
//
// **La regla, en una frase:** antes de `#` está el objeto ZFS; después de `#`, la ruta
// DENTRO de ese objeto, con los mismos nombres que se ven en el árbol.
//
// No hay casi nada inventado, y es a propósito: un nombre ZFS ya es una ruta separada por
// `/`, un snapshot ya usa `@`, y un fragmento ya significa «una parte de esto» en
// cualquier URL. Lo único que se decide aquí es que **la conexión es la autoridad** —el
// «dónde», que es para lo que existe— y que el primer tramo de la ruta es el pool.
//
//     zfsm://unibody                                    la conexión
//     zfsm://unibody#daemon                             su pestaña Daemon
//     zfsm://unibody/sback                             el pool, que TAMBIÉN es un dataset
//     zfsm://unibody/sback@antes                       su snapshot
//     zfsm://unibody/sback/user                        un dataset
//     zfsm://unibody/sback/user@ayer                   un snapshot
//     zfsm://unibody/sback/user#properties/compression una propiedad
//     zfsm://unibody/sback/user#content/docs/a.pdf     un fichero dentro
//
// **Por ahora solo NOMBRA.** Resolver —ir a buscar lo que nombra, abrirlo en el árbol,
// aceptarlo desde la línea de órdenes del sistema— vendrá después y se construye encima
// sin cambiar nada de esto: por eso el resultado del análisis es una estructura y no una
// cadena troceada a medias.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

// Qué nombra la URL.
//
// **No hay una clase «pool» aparte, y no es un olvido:** en ZFS el pool ES un dataset
// —`zfs list sback` lo devuelve, y `zfs snapshot sback@antes` funciona—. Tenerlo como
// clase distinta era una mentira que además impedía nombrar el snapshot de un pool. Para
// saber si un dataset es la raíz de su pool está `esRaizDePool()`.
enum class ZfsmKind {
    Invalida,
    Conexion,   // zfsm://unibody
    Dataset,    // zfsm://unibody/sback  y  zfsm://unibody/sback/user
    Snapshot,   // zfsm://unibody/sback@antes  y  zfsm://unibody/sback/user@ayer
};

// Nombres de sección que la aplicación conoce hoy.
//
// **En inglés, aunque el árbol se vea en español o en chino.** Una URL es un
// identificador, no texto para leer: si el literal dependiera del idioma de quien la
// escribió, la misma cosa tendría tres nombres y ninguno serviría para guardarla o
// compararla.
//
// Se aceptan otros nombres: una sección desconocida es válida y quien la resuelva
// decidirá qué hacer. Rechazarlas obligaría a tocar este fichero cada vez que el árbol
// gane una pestaña.
namespace zfsmSection {
constexpr const char* kContent = "content";
constexpr const char* kProperties = "properties";
constexpr const char* kPermissions = "permissions";
constexpr const char* kInfo = "info";
constexpr const char* kDaemon = "daemon";
}  // namespace zfsmSection

struct ZfsmUrl {
    ZfsmKind kind{ZfsmKind::Invalida};

    // La conexión, tal cual se escribió. Es su identificador o su nombre; quién lo
    // resuelve no es asunto de esta capa.
    std::string conexion;

    // El pool: el primer tramo de la ruta. Vacío si la URL solo nombra la conexión.
    std::string pool;

    // El nombre ZFS COMPLETO, con el pool delante: «sback/user/docs». Es lo que se le
    // pasa a `zfs`, así que se guarda ya montado y no en trozos.
    std::string dataset;

    // Sin el `@`. Vacío si no es un snapshot.
    std::string snapshot;

    // En minúsculas, sin el `#`. Vacío si no hay fragmento.
    std::string seccion;

    // Lo que sigue a la sección, ya descodificado y por tramos:
    // `#contenido/docs/a.pdf` -> {"docs", "a.pdf"}; `#propiedades/compression` ->
    // {"compression"}.
    std::vector<std::string> detalle;

    bool valida() const { return kind != ZfsmKind::Invalida; }

    // ¿El dataset nombrado es la raíz de su pool? Es lo que sustituye a la antigua clase
    // «pool», sin fingir que sea otra cosa que un dataset.
    bool esRaizDePool() const { return !dataset.empty() && dataset == pool; }

    // `dataset@snapshot`, que es como lo escribe ZFS. Sin snapshot, solo el dataset.
    std::string nombreZfs() const;
};

// Analiza. Devuelve false y explica en `error` qué falta o sobra.
//
// Es estricto con lo que puede ocultar un fallo —esquema equivocado, conexión vacía, dos
// `@`— y tolerante con lo que no —una sección que no conoce, una barra final—.
bool parseZfsmUrl(const std::string& texto, ZfsmUrl& out, std::string& error);

// La vuelta: reconstruye el texto, codificando lo que haga falta. `parse(format(x)) == x`
// para toda URL válida, que es lo que impide que la ida y la vuelta se separen.
std::string formatZfsmUrl(const ZfsmUrl& u);

// Codificación por-ciento de un tramo, según RFC 3986. Se expone porque quien construya
// una URL a mano la necesita: ZFS admite espacios en los nombres.
std::string percentEncodeSegment(const std::string& s);
bool percentDecode(const std::string& s, std::string& out);

}  // namespace zfsmgr::base
