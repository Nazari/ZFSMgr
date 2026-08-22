#pragma once

#include <string>
#include <vector>

#include "connectionprofile.h"

// Con qué OTRAS máquinas puede hablar el daemon de una máquina.
//
// El daemon de un extremo necesita a veces llamar al de otro por su cuenta, sin cliente de
// por medio: la nivelación GSA contra otra máquina es el caso. Para eso guarda en
// `/etc/zfsmgr/peers.json` las credenciales mTLS de esos pares, y **quién es él mismo**.
//
// **La clave `self` es la que se olvida y la que más duele.** Sin ella, el daemon no
// reconoce como propio un destino que apunta a su propia máquina: se va por el camino
// remoto, no encuentra credenciales para sí mismo y registra «no hay credenciales del par»,
// que es un mensaje desconcertante cuando el par es uno mismo. La rama de nivelación local
// no llega a ejecutarse nunca. Visto en vivo el 2026-08-21 sobre un `peers.json` escrito por
// una versión anterior del cliente, que no la ponía.
//
// Esto vivía dentro de `cli/shell.cpp` y por eso ni la interfaz de Qt ni el servidor web
// podían entregar credenciales ni decir qué tenía puesto una máquina. Aquí está lo que se
// puede hacer sin tocar la red: componer la carga y leer lo que el daemon responde.
namespace zfsmgr::base::peers {

// Una línea de `--dump-peers`.
struct Par {
    std::string id;
    std::string host;
    int puerto{0};
};

// Lo que el daemon sabe de sí mismo y de los demás.
struct Vista {
    std::string self;          // con quién se identifica; vacío es el fallo silencioso de arriba
    std::vector<Par> pares;
};

// Interpreta la salida de `--dump-peers`: una línea `SELF\t<id>` y luego
// `<id>\t<host>\t<puerto>` por par. Tolera que no venga la de SELF, porque un daemon
// anterior a este cambio no la emite.
Vista analiza(const std::string& salida);

enum class Fallo {
    Ninguno,
    SinOtrasConexiones,   // no hay ninguna otra que entregar
    SinMaterialTls,       // las hay, pero ninguna tiene certificados
};

struct Entrega {
    std::string cargaB64;              // para `--mutate-set-peers`
    std::vector<std::string> nombres;  // qué se entrega, para poder preguntarlo antes
    Fallo fallo{Fallo::Ninguno};
    bool sePuede() const { return fallo == Fallo::Ninguno; }
};

// Compone lo que hay que entregarle a `destino`: TODAS las demás conexiones con material
// TLS, más `self` = el nombre con el que el cliente llama a esa máquina.
//
// La propia conexión de destino se excluye a propósito: sería decirle cómo hablar consigo
// misma, y para eso está `self`.
//
// **`self` lo sabe el cliente y solo el cliente.** La máquina de destino no puede
// deducirlo: no hay forma de que sepa con qué nombre la tiene apuntada quien le habla, y ese
// nombre es justo el que aparecerá en el destino de una nivelación.
Entrega componeEntrega(const std::vector<ConnectionProfile>& perfiles,
                       const std::string& destino);

std::string etiquetaDe(Fallo f);

// Las tres direcciones que el daemon admite para escuchar.
//
// No es una lista arbitraria: el cliente llega por un túnel contra 127.0.0.1, así que una
// dirección suelta le cortaría el acceso. El daemon rechaza el resto, y tener aquí la misma
// lista permite ofrecer solo lo válido en vez de dejar fallar la llamada.
bool direccionDeEscuchaValida(const std::string& dir);
std::vector<std::string> direccionesDeEscucha();

}  // namespace zfsmgr::base::peers
