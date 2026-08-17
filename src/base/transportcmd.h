#pragma once

#include "connectionprofile.h"

#include <cstdint>
#include <string>
#include <vector>

// La parte del transporte que **decide y analiza texto**, sin tocar red ni procesos.
//
// Es la primera tanda de la mudanza del transporte a la capa base. Se separa así a
// propósito: todo lo que hay aquí es una función pura —entra texto o un perfil, sale una
// decisión o más texto—, de modo que se puede contrastar BYTE A BYTE contra la versión
// con Qt sin levantar una máquina ni abrir un socket. Lo que queda en `src/transport.h`
// es lo que sí abre sockets, lanza procesos y mantiene túneles, y va en tandas
// posteriores porque necesita piezas nuevas en la base.
//
// No se llama `transport.h` porque `src/base` está en la ruta de inclusión junto a
// `src/`, y mientras exista el adaptador `src/transport.h` dos ficheros con el mismo
// nombre harían ambiguo el `#include`. Cuando el adaptador desaparezca, esto se renombra.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

// --- Qué clase de máquina hay al otro lado.

// «LOCAL» como tipo de conexión: no hay SSH de por medio.
bool isLocalConnection(const ConnectionProfile& p);
bool isWindowsConnection(const ConnectionProfile& p);

// --- La clave con la que se recuerda una conexión.
//
// Sale de las COORDENADAS (usuario, host, puerto, ruta de clave), no de su posición en la
// lista. Es la misma regla que arregló las cachés indexadas por índice: si se borra una
// conexión, la siguiente no debe heredar nada suyo.
std::string remoteDaemonTlsCacheKey(const ConnectionProfile& p);

// --- La orden que se manda al otro extremo.

// Envuelve la orden para el intérprete del otro lado: PowerShell en Windows, y tal cual
// en el resto. En Windows se manda codificada en base64 UTF-16LE (`-EncodedCommand`),
// que es lo único que sobrevive a atravesar un `ssh` y un `cmd.exe`.
std::string wrapRemoteCommand(const ConnectionProfile& p, const std::string& remoteCmd);

// --- Si una orden cambia el estado del otro lado.
//
// **Esta es la función delicada de todo el fichero.** De ella depende que una mutación que
// pudo haber llegado NO se reenvíe: reenviar un `--dump-*` no cuesta nada, pero reenviar
// un `--job-submit` lanza la misma transferencia dos veces sobre los mismos datos.
bool isMutatingAgentCommand(const std::vector<std::string>& agentArgs);

// --- Camino HEREDADO: recuperar los argumentos de una cadena de shell.
//
// Existe solo para los sitios que todavía construyen la orden como cadena. **No añadir
// sitios nuevos por aquí**: el corte por separador, la lista blanca por prefijo y el
// deshacer del doble entrecomillado son suposiciones sobre cómo se construyó la cadena, y
// cada una ha fallado al menos una vez.
bool extractLocalAgentArgs(const std::string& remoteCmd, std::vector<std::string>& argsOut);

// --- El material TLS que el daemon remoto manda por SSH.

struct RemoteTlsBundle {
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
    std::uint16_t port{47653};
    bool clientKeyIncluded{false};
};

// Analiza el volcado delimitado por `__ZFSMGR_TLS_BEGIN__:` / `__ZFSMGR_TLS_END__:`.
// Devuelve false si falta el certificado del servidor o el del cliente, que son los dos
// imprescindibles: sin ellos no hay conversación posible.
bool parseRemoteDaemonTlsBundle(const std::string& text, RemoteTlsBundle& out);

// --- La configuración del daemon de ESTA máquina.

struct LocalAgentConfig {
    std::string bindAddress{"127.0.0.1"};
    std::uint16_t port{47653};
    std::string tlsCertPath;
    std::string tlsClientCertPath;
    std::string tlsClientKeyPath;
};

// Las rutas por omisión de esta plataforma. Tienen que coincidir con kDefaultTlsDir y
// kDefaultAgentConfigPath de daemon_main.cpp; estaban fijas a las POSIX, y por eso en
// Windows la conexión Local nunca encontraba su propio material TLS.
const char* defaultAgentConfigPath();
const char* defaultAgentTlsCertPath();
const char* defaultAgentTlsClientCertPath();
const char* defaultAgentTlsClientKeyPath();

// Analiza el contenido de agent.conf. Lo que no aparezca conserva su valor por omisión:
// un fichero a medias no debe dejar la configuración inservible.
LocalAgentConfig parseLocalAgentConfig(const std::string& text);
// Como la anterior, leyendo el fichero. Si no se puede abrir, devuelve los valores por
// omisión —que es el caso normal cuando no hay daemon instalado, no un error—.
LocalAgentConfig loadLocalAgentConfig(const std::string& path = defaultAgentConfigPath());

// --- Limpieza de lo que contesta el otro lado.

// PowerShell escupe un preámbulo CLIXML por la salida de error cuando hay flujos de aviso
// o información. No es un error: es ruido con forma de XML, y se quita.
std::string sanitizeWindowsCliXml(const std::string& raw);

// ¿Merece la pena reintentar SSH sin multiplexado? Solo ante los fallos que delatan que el
// socket de control no sirve; ante cualquier otro, reintentar sería esconder el problema.
bool shouldRetrySshWithoutMultiplexing(const std::string& stderrText);

}  // namespace zfsmgr::base::transport
