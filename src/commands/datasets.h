#pragma once

#include <string>
#include <vector>

// Los datasets: crear, renombrar, montar, promover y poner propiedades.
//
// Todo esto viaja por `--mutate-zfs-generic`, así que lo que se compone aquí es el argv de
// `zfs` sin el nombre del programa: `{"rename", viejo, nuevo}`.
namespace zfsmgr::commands::datasets {

// ¿Sirve este nombre para un dataset?
//
// ZFS admite letras, dígitos y `_-.:/ `; la arroba NO —eso haría una instantánea— y el
// espacio tampoco. La comprobación estaba escrita a mano y distinta en cada cliente.
bool nombreValido(const std::string& nombre);

// El nombre completo al que renombrar, a partir de lo que haya escrito quien llama.
//
// **La regla**: un nombre SIN barra se entiende como «cámbiale la hoja, déjalo donde está»,
// así que se le antepone el padre del actual. Con barra se toma tal cual, que es como se
// mueve un dataset de sitio.
//
// Sin esto, teclear «fotos» para renombrar `tank/media/cine` manda `zfs rename
// tank/media/cine fotos`, y ZFS responde «cannot create 'fotos': missing dataset name» —un
// mensaje que no dice lo que hay que hacer—. El intérprete ya aplicaba la regla; el servidor
// web no, así que el mismo producto se comportaba distinto según por dónde se entrara.
std::string nombreDeRenombrado(const std::string& actual, const std::string& nuevo);

// `zfs rename <actual> <nuevo-completo>`
std::vector<std::string> argvRenombrar(const std::string& actual, const std::string& nuevo);

// `zfs create [-p] [-o p=v...] <dataset>`
//
// `padres` añade `-p`: crea los intermedios que falten. Sin él, `zfs create a/b/c` falla si
// `a/b` no existe.
std::vector<std::string> argvCrear(const std::string& dataset,
                                   const std::vector<std::string>& propiedades = {},
                                   bool padres = false);

// El nombre de un hijo: `<padre>/<hoja>`. Si la hoja ya trae barra, se respeta.
std::string nombreDeHijo(const std::string& padre, const std::string& hoja);

// `zfs promote <dataset>`
//
// Solo hace algo si el dataset es un CLON: promoverlo invierte la relación con su origen.
// Sobre uno que no lo es, ZFS responde «not a cloned filesystem» —correcto, pero el cliente
// puede decirlo antes y mejor—.
std::vector<std::string> argvPromover(const std::string& dataset);

// `zfs mount [-f] <dataset>` y `zfs unmount [-f] <dataset>`
std::vector<std::string> argvMontar(const std::string& dataset, bool forzar = false);
std::vector<std::string> argvDesmontar(const std::string& dataset, bool forzar = false);

// `zfs set <prop>=<valor> <dataset>`
//
// Vacío si la propiedad no tiene nombre. El valor SÍ puede ser vacío: hay propiedades que se
// ponen en blanco a propósito.
std::vector<std::string> argvPonerPropiedad(const std::string& dataset, const std::string& propiedad,
                                            const std::string& valor);

// `zfs inherit <prop> <dataset>`: devolver una propiedad a lo que herede del padre.
std::vector<std::string> argvHeredarPropiedad(const std::string& dataset,
                                              const std::string& propiedad);

}  // namespace zfsmgr::commands::datasets
