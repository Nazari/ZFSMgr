#pragma once

#include "connectionprofile.h"
#include "procesos.h"
#include "transportreason.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Lo que se mantiene ABIERTO mientras se habla con las máquinas remotas: los túneles del
// RPC y la memoria de los intentos que fallaron.
//
// Existe por dos motivos. Uno: es lo que un CLI necesita para hablar con el agente, y
// mientras fueran campos sueltos de la ventana no se podía usar desde otro sitio. Dos, y
// más importante: **el cerrojo y lo que protege estaban separados**, y solo un comentario
// decía cuáles iban juntos. Ahora viven en la misma estructura.
//
// Las claves NO son índices de conexión: salen de las coordenadas (usuario, host, puerto,
// ruta de clave), así que sobreviven a que se reordene la lista. Ver
// docs/diseno_tecnico_capa_base_sin_qt.md, sección de las cachés por posición, para lo
// que pasa cuando no es así.
namespace zfsmgr::base {

struct RemoteRpcTunnelState {
    // El proceso VIVE aquí dentro. Antes era un `QProcess` colgado de la ventana, y esa
    // era la última atadura del transporte a un objeto con bucle de eventos. Como
    // `ChildProcess` no se copia, esta estructura tampoco: se mueve.
    ChildProcess process;
    std::uint16_t localPort{0};
    std::uint16_t remotePort{0};
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point lastUsed;

    RemoteRpcTunnelState() = default;
    RemoteRpcTunnelState(RemoteRpcTunnelState&&) = default;
    RemoteRpcTunnelState& operator=(RemoteRpcTunnelState&&) = default;
};

struct TransportSession {
    // --- A dónde va lo que el transporte cuenta mientras trabaja.
    //
    // Se consideró que cada llamada DEVOLVIERA la lista de lo ocurrido y que quien llama
    // decidiera qué hacer con ella. Es más limpio sobre el papel, pero **habría sido una
    // regresión**: el registro de la aplicación escribe al momento, así que hoy se llena
    // MIENTRAS la operación ocurre. Acumular y devolver al final dejaría treinta segundos
    // de silencio y luego un volcado de golpe.
    //
    // Así que se emite sobre la marcha, pero **a algo que se recibe**, no a algo que el
    // transporte busca. La interfaz pone un destino que escribe en su pestaña; un CLI
    // pondría uno que escriba por la salida de error.
    enum class Nivel { Normal, Info, Warn, Error, Debug };

    // `connId` vacío significa «al registro general»; con valor, además al de esa
    // conexión. Sin destino puesto, no se pierde nada importante: solo no se cuenta.
    std::function<void(Nivel, const std::string& connId, const std::string& msg)> sink;

    void log(Nivel n, const std::string& msg) const {
        if (sink) {
            sink(n, std::string(), msg);
        }
    }
    // Al registro general Y al de la conexión, que es la pareja que se repetía a mano en
    // treinta sitios.
    void logConn(Nivel n, const std::string& connId, const std::string& msg) const {
        if (sink) {
            sink(n, connId, msg);
        }
    }

    // --- Los avisos, que son PROSA y por tanto no los escribe esta capa.
    //
    // `sink` sigue siendo para las TRAZAS: la orden que se ejecuta, los `[daemon-rpc:...]`,
    // las direcciones resueltas. Eso es rastro técnico y va tal cual. Lo que acaba delante
    // del usuario en forma de frase entra por aquí tipificado, y lo redacta quien sabe el
    // idioma. Sin esta separación, una sesión con `--lang en` salía salpicada de castellano.
    std::function<void(Nivel, const std::string& connId, const transport::NotaDeAviso&)> avisoSink;

    void aviso(Nivel n, const std::string& connId, const transport::NotaDeAviso& a) const {
        if (avisoSink) {
            avisoSink(n, connId, a);
            return;
        }
        // Sin traductor puesto se cae a la etiqueta estable. Es fea, pero perder un aviso
        // en silencio porque nadie ha conectado el traductor sería peor.
        if (sink) {
            sink(n, connId,
                 std::string(transport::etiquetaDe(a.aviso))
                     + (a.detalle.empty() ? std::string() : ": " + a.detalle));
        }
    }

    // --- Dejar respirar a quien nos llamó mientras esperamos.
    //
    // Sustituye al `QCoreApplication::processEvents` que había repartido por el
    // transporte. Es lo mismo que ya hacía `StreamCallbacks::onTick`: un solo enganche
    // para las tres cosas que hacía el bucle de Qt —repintar, contar lo que queda y mirar
    // si el usuario canceló—.
    //
    // **Devolver false CANCELA** la espera en curso. Quien no tenga interfaz no lo pone, y
    // entonces la espera simplemente duerme.
    //
    // **El parámetro NO es un detalle.** Distingue los dos contextos que la versión con Qt
    // trataba distinto a propósito:
    //
    // - Mientras se ESPERA a que un túnel acepte conexiones: `false`. Bombear eventos
    //   reentra, y dejando pasar acciones del usuario se colaba por ahí una recarga de
    //   conexiones que dejaba colgando las referencias que sostenía quien había llamado.
    // - Mientras CORRE una orden larga: `true`. Es lo que permite pulsar Cancelar durante
    //   una transferencia; sin ello la ventana se pinta pero no responde.
    //
    // Unificarlos en el estricto haría que Cancelar dejara de funcionar en las
    // transferencias, y en el permisivo reabriría la reentrancia. Son dos cosas distintas.
    std::function<bool(bool permitirEntradaDeUsuario)> pump;

    bool respira(bool permitirEntradaDeUsuario = true) const {
        return pump ? pump(permitirEntradaDeUsuario) : true;
    }

    // --- ¿Se pueden montar túneles desde aquí?
    //
    // Sin ponerlo, sí: una herramienta de un solo hilo no compite con nadie.
    //
    // **El motivo original de esta restricción YA NO EXISTE.** Estaba porque los túneles
    // eran `QProcess` colgados de la ventana, y crearlos desde un hilo de refresco daba un
    // aviso de afinidad o una caída; ahora son `ChildProcess`, que no cuelgan de nadie. Se
    // conserva para NO cambiar el comportamiento en el mismo paso en que se cambia de
    // motor: quitarlo permitiría montar túneles desde los hilos de refresco, que es un
    // cambio de concurrencia real y merece medirse aparte.
    std::function<bool()> tunnelsAllowedHere;

    bool puedeMontarTuneles() const { return tunnelsAllowedHere ? tunnelsAllowedHere() : true; }

    // Ejecuta la tarea DONDE sí se pueden montar túneles, y espera a que termine. La
    // interfaz lo resuelve con una llamada bloqueante al hilo de la ventana.
    //
    // Sin ponerlo, se ejecuta en línea. Es lo correcto para quien no tenga otro hilo: no
    // hacer nada dejaría la operación sin ocurrir, que es peor que hacerla aquí.
    std::function<void(const std::function<void()>&)> runWhereTunnelsAllowed;

    void enElHiloDeTuneles(const std::function<void()>& tarea) const {
        if (!puedeMontarTuneles() && runWhereTunnelsAllowed) {
            runWhereTunnelsAllowed(tarea);
            return;
        }
        tarea();
    }

    // --- Transporte de mentira, para los tests.
    //
    // Vive aquí y no en la ventana porque es una propiedad DEL TRANSPORTE: mientras está
    // puesto no se abre ninguna conexión, las órdenes por argv van a esa función, y las
    // que salgan como cadena de shell se anotan y fracasan —para que un test pueda
    // afirmar que algo NO se fue por ese camino—.
    struct AgentCallForTest {
        std::vector<std::string> argv;  // vacío si la orden salió como cadena de shell
        std::string shellCommand;       // no vacío solo en ese caso
        std::string stdinPayload;
    };
    using AgentTransportForTest = std::function<bool(const std::vector<std::string>& argv,
                                                     std::string& out, std::string& err, int& rc)>;

    AgentTransportForTest transportForTest;
    std::vector<AgentCallForTest> callsForTest;

    // --- Cómo se piden credenciales cuando hacen falta.
    //
    // Es la segunda cosa que el transporte necesita del exterior, junto al destino del
    // registro: **a dónde contar** y **cómo preguntar**. Las dos se reciben, ninguna se
    // busca — y por eso aquí dentro no hay ni un widget.
    //
    // Devuelve false si no se pudo obtener —el usuario canceló, o no había descriptor en
    // un contexto no interactivo—. Sin proveedor puesto devuelve false, que es lo
    // prudente: mejor no hacer nada que intentarlo sin credenciales.
    using CredentialProvider =
        std::function<bool(const std::string& motivo, std::string& usuario, std::string& clave)>;
    CredentialProvider credentialProvider;

    bool askCredentials(const std::string& motivo, std::string& usuario, std::string& clave) const {
        return credentialProvider ? credentialProvider(motivo, usuario, clave) : false;
    }

    // --- Las dos cosas que el transporte necesita del REGISTRO de conexiones.
    //
    // No se le pasa el registro entero a propósito: lo que necesita no son los perfiles,
    // son dos decisiones que dependen de ellos. Pasarle el registro le daría acceso a
    // todo —incluidas las contraseñas de todas las máquinas— para hacer dos cosas
    // concretas.
    //
    // Sin ponerlas, el transporte sigue funcionando: no resuelve credenciales locales y no
    // guarda el material TLS que negocie. Un CLI de solo lectura puede vivir así.

    using LocalSudoResolver = std::function<bool(ConnectionProfile& perfil)>;
    LocalSudoResolver localSudoResolver;

    using TlsPersister = std::function<bool(const ConnectionProfile& p,
                                            const std::string& serverCertPem,
                                            const std::string& clientCertPem,
                                            const std::string& clientKeyPem,
                                            std::uint16_t daemonPort,
                                            std::string* errorOut)>;
    TlsPersister tlsPersister;

    bool resolveLocalSudo(ConnectionProfile& perfil) const {
        return localSudoResolver ? localSudoResolver(perfil) : false;
    }
    bool persistTls(const ConnectionProfile& p, const std::string& serverCertPem,
                    const std::string& clientCertPem, const std::string& clientKeyPem,
                    std::uint16_t daemonPort, std::string* errorOut) const {
        if (!tlsPersister) {
            if (errorOut) {
                *errorOut = "no hay dónde guardar el material TLS";
            }
            return false;
        }
        return tlsPersister(p, serverCertPem, clientCertPem, clientKeyPem, daemonPort, errorOut);
    }

    // TODO lo de abajo va bajo este cerrojo. El refresco de conexiones corre en hilos y
    // estos mapas se tocan desde varios a la vez.
    mutable std::mutex mutex;

    // Túneles `ssh -L` vivos, por clave de conexión.
    std::map<std::string, RemoteRpcTunnelState> tunnelsByConnKey;

    // Claves cuyo túnel se está montando AHORA MISMO. Protege de la reentrancia que
    // provoca el bombeo de eventos de la espera: sin esto se montaban túneles duplicados
    // que quedaban huérfanos fuera del mapa.
    std::set<std::string> tunnelsBeingCreated;

    // Hasta cuándo no se reintenta el RPC de una conexión, y por qué. Sin esto, una
    // conexión con el daemon caído se lleva una ida y vuelta por SSH en cada operación.
    std::map<std::string, std::chrono::steady_clock::time_point> retryAfterByConnKey;
    std::map<std::string, transport::MotivoFallo> retryReasonByConnKey;

    // Conexiones a las que se ha renunciado al multiplexado de SSH, y aquellas cuya
    // resolución de nombre ya se anotó en el registro: las dos existen para no repetir el
    // mismo mensaje en cada operación.
    std::set<std::string> disableMultiplexKeys;
    std::set<std::string> loggedResolutionKeys;
};

}  // namespace zfsmgr::base
