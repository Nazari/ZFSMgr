#pragma once

#include "i18n.h"

#include <string>

// Traducir un texto del CLI.
//
// El castellano va ESCRITO EN EL CÓDIGO y la clave lo acompaña: `T("t_x_ab12", "texto")`.
// Así el fuente se lee sin ir a buscar a un fichero qué dice cada mensaje, y si el
// catálogo falta, está incompleto o está roto, la herramienta sigue saliendo en castellano
// en vez de escupir claves.
//
// Las claves se generan con la MISMA regla que las de la interfaz —«t_» + trozo del texto
// + hash— y viven en los MISMOS ficheros `i18n/*.json`: dos sistemas de traducción en un
// programa acaban discrepando.
//
// Lo que NO pasa por aquí, y no es un olvido: los verbos, los nombres de campo de tsv y
// json, y los literales de las URL. Son interfaz para programas y van en inglés siempre.
// Traducirlos rompería cualquier guion en cuanto alguien cambiara de idioma.
#define T(clave, castellano) (::zfsmgr::base::i18n::tr((clave), (castellano)))

// La misma, para cuando hace falta un `const char*` —`printf` y compañía—.
#define TC(clave, castellano) (T(clave, castellano).c_str())
