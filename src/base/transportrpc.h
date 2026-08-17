#pragma once

#include "connectionprofile.h"

#include <cstdint>
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
    std::string failure;  // vacío si fue bien
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

}  // namespace zfsmgr::base::transport
