#pragma once

#include "connectionprofile.h"
#include "transportreason.h"
#include "transportsession.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// La parte del transporte que **ejecuta**: una orden por SSH y el RPC contra el daemon de
// esta máquina.
//
// Segunda tanda de la mudanza. Se separa de `transportcmd.h` porque aquí sí hay E/S —se
// lanza un proceso, se abre un socket—, y eso cambia cómo se verifica: lo de allí se
// contrasta byte a byte contra la versión con Qt, y esto hay que probarlo contra una
// máquina.
//
// Lo que NO está aquí, y va en la tercera tanda, es el túnel `ssh -L`: mantiene procesos
// vivos entre llamadas, necesita un puerto libre y depende de que alguien deje respirar a
// la interfaz mientras se monta.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

// Ejecuta una orden por SSH y devuelve lo que salió. **No registra nada**: es el camino
// que usan los hilos de vigilancia en segundo plano, donde escribir en el registro desde
// fuera del hilo de la interfaz era el problema, no la solución.
//
// `timeoutMs <= 0` toma 15 s, que es el valor que tenía la versión con Qt.
bool runSshRaw(const ConnectionProfile& p,
               const std::string& remoteCmd,
               int timeoutMs,
               std::string& out,
               std::string& err,
               int& rc);

// A qué dirección hay que conectarse dado lo que dice `AGENT_BIND` en agent.conf.
//
// Existe porque la dirección de ESCUCHA no sirve como dirección de CONEXIÓN: el daemon
// puede escuchar en `0.0.0.0` o en `::`, que significan «en todas», y a eso no se conecta
// nadie. Lo mismo si el valor no es una dirección válida. En los tres casos se va a
// `127.0.0.1`, que es donde el daemon local está de todas formas.
std::string bindAddressToConnectHost(const std::string& bindAddress);

// Lo que costó la llamada y por qué falló, para que quien llame pueda registrarlo. Se
// devuelve en vez de escribirlo aquí: esta capa no sabe dónde está el registro.
struct LocalRpcDiag {
    long long elapsedMs{0};
    MotivoFallo failure;  // vacío si fue bien
};

// El RPC contra el daemon de ESTA máquina: una línea JSON de ida, una de vuelta, por TLS
// con autenticación mutua y **validando por fijación del certificado**, no por CA.
//
// El material TLS llega por parámetro y no se lee del disco: vive bajo /etc/zfsmgr con
// permisos de root, así que quien lo tiene ya tuvo que elevarse para leerlo.
bool runLocalAgentRpc(const std::vector<std::string>& agentArgs,
                      const std::string& serverCertPem,
                      const std::string& clientCertPem,
                      const std::string& clientKeyPem,
                      std::uint16_t daemonPort,
                      int timeoutMs,
                      std::string& out,
                      std::string& err,
                      int& rc,
                      LocalRpcDiag* diag = nullptr);

// --- Resolución de nombres, solo para poder CONTARLO.
//
// No se usa para conectar —de eso se encarga `ssh`—, sino para dejar dicho en el registro
// a qué se resolvió un nombre. Existe porque los `*.local` van por mDNS y los fallos de
// ahí son de los que se diagnostican mal: parecen «la máquina no responde».
struct HostResolution {
    bool ok{false};
    std::string error;
    // «IPv4:192.168.1.33», tal y como se escribe en el registro.
    std::vector<std::string> addresses;
};
HostResolution resolveHostAddresses(const std::string& host);

// --- El material TLS del daemon de ESTA máquina.
//
// Vive bajo /etc/zfsmgr con permisos de root, así que puede hacer falta elevar para
// leerlo; se cachea cinco minutos para no pedir credenciales en cada orden.
bool ensureLocalDaemonTlsMaterial(TransportSession& ses,
                                  std::string& serverCertPem,
                                  std::string& clientCertPem,
                                  std::string& clientKeyPem,
                                  std::uint16_t& daemonPort);
void clearLocalDaemonTlsCache();

// --- Los dos caminos de alto nivel.

// Intenta el RPC tipado del agente contra una conexión SSH. Devuelve false si hay que caer
// al camino de siempre.
//
// **Devuelve TRUE con rc=124 en un caso muy concreto**: cuando una MUTACIÓN llegó al daemon
// y no hubo respuesta. Es «true» porque no hay que reintentar por SSH —sería ejecutar la
// misma orden destructiva por segunda vez—, y el mensaje se lo dice al usuario.
bool tryAgentRpcOverSsh(TransportSession& ses,
                        const ConnectionProfile& p,
                        const std::vector<std::string>& agentArgs,
                        int timeoutMs,
                        std::string& out,
                        std::string& err,
                        int& rc,
                        const std::function<void(const std::string&)>& onStdoutLine = {},
                        const std::function<void(const std::string&)>& onStderrLine = {},
                        bool echoOutputToLog = true);

// Ejecuta una orden en la máquina. Si `allowAgentRpc`, intenta primero el RPC tipado del
// agente y solo cae a SSH en crudo si no se puede.
//
// **El plazo es de INACTIVIDAD, no total**: se reinicia con cada trozo que llega. Una
// transferencia de horas no puede morir por durar; sí debe morir si se queda muda.
bool runSsh(TransportSession& ses,
            const ConnectionProfile& p,
            const std::string& remoteCmd,
            int timeoutMs,
            std::string& out,
            std::string& err,
            int& rc,
            const std::function<void(const std::string&)>& onStdoutLine = {},
            const std::function<void(const std::string&)>& onStderrLine = {},
            const std::function<void(int)>& onIdleTimeoutRemaining = {},
            const std::string& stdinPayload = {},
            bool allowAgentRpc = true,
            bool echoOutputToLog = true);

}  // namespace zfsmgr::base::transport
