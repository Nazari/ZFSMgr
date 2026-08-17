#pragma once

#include <string>

// Cómo entra un secreto en una herramienta sin ventana.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md, «Cómo entran los secretos sin ventana».
// Resumen: por descriptor de fichero, con el terminal como alternativa interactiva.
// **Nunca por variable de entorno ni por argumento**, que salen en `ps` para cualquier
// usuario de la máquina.
namespace zfsmgr::cli {

// Lee un secreto del descriptor indicado, hasta el primer salto de línea o el final.
//
// El salto final se descarta, pero **nada más**: una contraseña puede llevar espacios al
// principio o al final y recortarlos cambiaría el secreto. Devuelve false si el
// descriptor no se puede leer.
bool leerSecretoDeDescriptor(int fd, std::string& out, std::string& error);

// Pregunta por el terminal con el eco apagado. Devuelve false si no hay terminal —en un
// `cron`, por ejemplo—, que es justamente el caso en el que hay que usar el descriptor.
bool preguntarSecretoPorTerminal(const std::string& aviso, std::string& out, std::string& error);

// Pregunta por el terminal CON eco, para lo que no es secreto —un nombre de usuario—.
//
// Vive aquí, junto a su gemela, porque el manejo del terminal es el mismo y la única
// diferencia es si se apaga el eco. Separarlas invitaría a que una de las dos se olvidara
// de comprobar que hay terminal.
bool preguntarPorTerminal(const std::string& aviso, std::string& out, std::string& error);

// ¿Hay un terminal al otro lado? Sirve para decidir si tiene sentido preguntar.
bool hayTerminal();

}  // namespace zfsmgr::cli
