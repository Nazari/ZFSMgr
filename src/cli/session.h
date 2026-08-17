#pragma once

#include "connectionprofile.h"
#include "transportsession.h"

#include <memory>
#include <string>
#include <vector>

// Lo que el CLI necesita para hablar con las máquinas.
//
// La capa base no busca nada por su cuenta: se le DICE a dónde contar lo que hace y cómo
// preguntar lo que no sabe. Este fichero es la versión de esas respuestas para una
// herramienta de terminal, igual que `MainWindow` tiene la suya para la ventana.
//
// Las tres diferencias con la interfaz gráfica, y por qué:
//
// - **El registro va a la salida de ERROR**, no a la estándar. Lo que se pide sale por la
//   estándar y lo que pasa por el camino sale por la de error, que es lo que permite
//   `zfsmgr-cli ... > datos.tsv` sin que se mezclen.
// - **Las credenciales se piden por el terminal**, nunca por argumento ni por variable de
//   entorno: las dos cosas quedan visibles en `ps` para cualquier usuario de la máquina.
// - **No hay nada que dejar respirar**: sin ventana, el enganche de bombeo sobra.
namespace zfsmgr::cli {

// Las conexiones tal y como las ve el CLI: ya descifradas y con el material TLS del
// almacén de confianza puesto.
struct Conexiones {
    std::vector<zfsmgr::base::ConnectionProfile> perfiles;
    std::string aviso;  // vacío si todo fue bien
};

// Carga config.json y trust-store.json de `dirConfig` y funde los dos.
//
// **El material TLS vive en trust-store.json, no en config.json**, que es un fichero
// aparte precisamente para separarlo de las contraseñas de acceso. Leer solo el primero
// hacía que una conexión CON TLS apareciera como si no lo tuviera, y el transporte
// intentara traerlo otra vez por SSH en cada arranque.
Conexiones cargarConexiones(const std::string& dirConfig, const std::string& maestra);

// Busca una conexión por identificador o por nombre, sin distinguir mayúsculas. Devuelve
// nullptr si no está. Se aceptan las dos formas porque una URL escrita a mano lleva lo que
// el usuario recuerde, y en la interfaz lo que se ve es el nombre.
const zfsmgr::base::ConnectionProfile* buscarConexion(const Conexiones& c, const std::string& nombre);

// La sesión de transporte de una herramienta de terminal.
//
// Se devuelve por puntero porque `TransportSession` tiene un mutex y no se puede mover, y
// porque los enganches capturan referencias a lo que vive dentro de esta estructura.
struct Sesion {
    zfsmgr::base::TransportSession transporte;
    std::string dirConfig;
    std::string maestra;
    bool verboso{false};

    // Las credenciales de sudo de ESTA máquina, una vez resueltas.
    //
    // Se recuerdan durante la sesión a propósito: sin esto se pregunta en CADA orden que
    // necesite elevar, y una sesión interactiva se vuelve inusable —o peor, invita a poner
    // la contraseña en un alias—. Es lo mismo que hace la interfaz.
    std::string sudoUsuario;
    std::string sudoClave;
    bool sudoResuelto{false};
};

// Monta la sesión: destino del registro, proveedor de credenciales, resolución del sudo
// local y persistencia del material TLS negociado.
std::unique_ptr<Sesion> crearSesion(const std::string& dirConfig,
                                    const std::string& maestra,
                                    bool verboso);

// --- Cambiar la configuración, no solo leerla.

// Da de alta o actualiza una conexión en config.json.
//
// **La contraseña se guarda CIFRADA con la contraseña maestra**, igual que hace la
// interfaz. Sin contraseña maestra no se guarda en claro: se falla y se dice por qué.
// El resto del fichero —ajustes, acciones pendientes, estado del árbol— se conserva tal
// cual: aquí se toca únicamente la lista de conexiones.
bool guardarConexion(Sesion& s, const zfsmgr::base::ConnectionProfile& p, std::string& error);

// Quita una conexión de la configuración. NO toca nada en la máquina.
bool borrarConexion(Sesion& s, const std::string& id, std::string& error);

// La marca de «desconectada», que vive en `app.disconnected_connections` de config.json y
// es la misma que usa la interfaz.
//
// Significa «no hables con esta máquina»: la interfaz se salta las desconectadas al
// refrescar y al recoger registros, y el intérprete se niega a operar sobre ellas.
bool estaDesconectada(const Sesion& s, const std::string& id);
bool marcarDesconectada(Sesion& s, const std::string& id, bool desconectada, std::string& error);

// Ejecuta un verbo del agente por RPC TIPADO: argv, sin intérprete de por medio.
//
// **No hay respaldo por shell, y es deliberado.** En la interfaz existe por historia; aquí
// se empieza limpio. Si el daemon no está o no responde, se dice — que es infinitamente
// mejor que ejecutar por otro camino una orden que el usuario creía tipada.
//
// Devuelve false si no se pudo hablar con la máquina; `rc` distinto de cero significa que
// se habló y el agente contestó que no.
bool ejecutarAgente(Sesion& s,
                    const zfsmgr::base::ConnectionProfile& p,
                    const std::vector<std::string>& args,
                    std::string& out,
                    std::string& err,
                    int& rc,
                    std::string* motivo = nullptr,
                    int timeoutMs = 60000);

}  // namespace zfsmgr::cli
