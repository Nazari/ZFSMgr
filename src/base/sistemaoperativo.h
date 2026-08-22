#pragma once

#include <string>

// Qué sistema operativo corre en un extremo, a partir de lo que ese extremo responde.
//
// **Aquí solo se INTERPRETA texto.** Quién lanza el proceso y cómo llega su salida es cosa
// de quien llame: el daemon lo lee de su propio disco, la interfaz lo trae por SSH del otro
// lado. Esa separación es justo lo que faltaba, y por eso había tres versiones distintas de
// lo mismo:
//
//   - `daemon_main.cpp:detectOsLine()` abría `/etc/os-release` y lo parseaba en C++.
//   - `connectiondialog.cpp` y `mainwindow_refresh.cpp` mandaban un
//     `sh -lc '. /etc/os-release; printf "%s %s" "$NAME" "$VERSION_ID"'` cada una.
//
// Las dos de Qt delegaban en el intérprete un trabajo que la del daemon ya hacía sin él, y
// además discrepaban en los bordes: el `printf` de shell deja un espacio suelto cuando
// `VERSION_ID` no está —Arch y Gentoo no la traen—, y no quita las comillas del valor, que
// `/etc/os-release` sí lleva. La del daemon sí hacía ambas cosas.
//
// Con el parseo aquí, lo que viaja por SSH pasa a ser `cat /etc/os-release`: un mandato sin
// nada que interpretar, en vez de un guion.
namespace zfsmgr::base::sistemaoperativo {

// El contenido de `/etc/os-release` → «Fedora Linux 42».
//
// Devuelve vacío si el fichero no dice nada útil, para que quien llame ponga su respaldo
// («Linux» a secas) y no una cadena a medias.
//
// Quita las comillas de los valores: el formato las admite —`NAME="Fedora Linux"`— y
// dejarlas puestas se veía en la ficha de la conexión.
std::string deOsRelease(const std::string& contenido);

// La salida de `system_profiler SPSoftwareDataType` → «macOS 15.5 (24F74)».
//
// Se busca la línea «System Version:» y se devuelve lo que va detrás. Antes lo hacía un
// `sed -n "s/^ *System Version: //p" | head -1` dentro del guion remoto.
std::string deSystemProfiler(const std::string& salida);

}  // namespace zfsmgr::base::sistemaoperativo
