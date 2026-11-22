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

// Ctrl-C in the interactive interpreter. Without this, Ctrl-C outside the line editor was a
// plain SIGINT and took the WHOLE interpreter down —in the middle of a command, or while a
// question was waiting for its answer—. With it installed:
//   - while a command runs, the child processes it launched get the SIGINT from the terminal
//     and end; the interpreter only notes it (`interrumpido()`), and long waits check it;
//   - in a question (preguntarPorTerminal, preguntarSecretoPorTerminal) Ctrl-C ends the
//     answer and counts as «no» / not given.
// Installed only where there is a terminal and a person; --serve and pipes keep the default.
void instalaInterrupcion();
bool interrumpido();
void limpiaInterrupcion();

// Quien pinta el terminal (la pantalla del intérprete) se entera de cada pregunta: `antes`
// escribe el aviso él mismo y deja de repintar; `despues` recibe lo respondido (vacío en un
// secreto) y vuelve a pintar. Sin ganchos, el aviso va por la salida de error como siempre.
// Va por ganchos y no llamando a la pantalla porque este fichero entra también en otros
// ejecutables que no la tienen.
using AntesDePreguntar = void (*)(const std::string& aviso);
using DespuesDePreguntar = void (*)(const std::string& eco, bool secreto);
void ponGanchosDePregunta(AntesDePreguntar antes, DespuesDePreguntar despues);

}  // namespace zfsmgr::cli
