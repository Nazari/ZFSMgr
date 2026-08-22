#pragma once

#include "session.h"
#include "tabla.h"

#include <string>

// El modo interactivo: `zfsmgr-cli` sin verbo se comporta como un intérprete de órdenes.
//
//     zfsm://> cd oldlau/winpool/sa
//     zfsm://OldLau/winpool/sa> ls
//     zfsm://OldLau/winpool/sa> snapshot @antes
//
// **La idea entera es que la posición sea una URL.** No hay «conexión seleccionada» ni
// «dataset actual» por separado: hay un punto del árbol, escrito como `zfsm://`, y todas
// las órdenes actúan sobre él salvo que se diga otra cosa. Eso hace que cualquier orden se
// pueda copiar del historial y ejecutar suelta, y que lo que se aprende navegando sirva
// para escribir guiones.
//
// Ver docs/diseno_tecnico_url_zfsm.md.
namespace zfsmgr::cli {

// Arranca el intérprete. Devuelve el código de salida del proceso.
//
// `urlInicial` vacía empieza en la RAÍZ (`zfsm://`), donde `ls` lista las conexiones. No se
// arranca ya en `zfsm://Local`: hacía creer que esa máquina era el punto de partida
// obligado, y este cliente gobierna varias.
int ejecutarShell(Sesion& ses, Formato formato, const std::string& urlInicial = {},
                  bool asumirSi = false);

}  // namespace zfsmgr::cli
