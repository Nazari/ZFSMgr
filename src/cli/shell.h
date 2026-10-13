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
//
// `compruebaProgramaciones`: con terminal, comprobar en segundo plano el estado de las
// programaciones de todas las conexiones y avisar en una línea (`--no-schedule-check` lo quita).
//
// `pantallaCompleta`: con terminal, la pantalla de `pantalla.h` —línea multilínea, repintado
// al cambiar el tamaño, zona de estado—; `--no-tui` la quita.
// The shell as a SERVICE for another program (the graphical interface): one JSON request per
// line on standard input, `{"id":N,"line":"<a shell command>"}`, and on standard output one
// JSON event per line: `{"type":"ready"}` once, `{"id":N,"type":"confirm","text":…}` when the
// command asks (answered with `{"id":N,"answer":true|false}`), and
// `{"id":N,"type":"result","rc":…,"stdout":…,"stderr":…}`. A secret the command needs (a
// passphrase) is asked with `{"id":N,"type":"secret","text":…}` and answered with
// `{"id":N,"secret":"…"}` (or `{"id":N,"secret":null}` to refuse). A command that fails does not end
// the service. Output is JSON (`--format json` tables), or what a request's optional
// `"format"` (text, tsv, json) says for that request only.
int servirOrdenes(Sesion& ses);

int ejecutarShell(Sesion& ses, Formato formato, const std::string& urlInicial = {},
                  bool asumirSi = false, bool compruebaProgramaciones = true,
                  bool pantallaCompleta = true);

}  // namespace zfsmgr::cli
