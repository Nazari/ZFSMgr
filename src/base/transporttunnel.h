#pragma once

#include "connectionprofile.h"
#include "transportsession.h"

#include <cstdint>
#include <string>
#include <vector>

// El RPC por el túnel `ssh -L`, que es el camino normal cuando hay daemon.
//
// Tercera tanda de la mudanza, y la delicada: por aquí pasan las mutaciones. Todo lo que
// hay aquí existe para responder a una sola pregunta con precisión —**¿pudo la orden haber
// llegado al otro lado?**—, porque de ella depende si se puede reintentar. Reenviar un
// `--dump-*` no cuesta nada; reenviar un `--job-submit` lanza la misma transferencia dos
// veces sobre los mismos datos.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

// Trae el material TLS del daemon REMOTO: primero de la caché en memoria, luego del perfil
// guardado, y solo si no hay, por SSH.
//
// `forceRefresh` salta las dos primeras, que es lo que hay que hacer cuando el material
// guardado ha dejado de valer.
struct RemoteTlsMaterial {
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
    std::uint16_t daemonPort{47653};
    // Si vino de la máquina remota —y por tanto conviene guardarlo— y si la clave privada
    // venía dentro. Lo segundo importa porque el daemon deja de entregarla una vez
    // aprovisionado.
    bool fetchedFromRemote{false};
    bool clientKeyFetchedFromRemote{false};
};
bool fetchRemoteDaemonTlsMaterial(const ConnectionProfile& p,
                                  bool forceRefresh,
                                  RemoteTlsMaterial& out,
                                  std::string* failureReason = nullptr);

// Vacía la caché en memoria del material TLS remoto. Hace falta cuando se reaprovisiona
// una conexión: si no, se seguiría hablando con el certificado viejo hasta cinco minutos.
void clearRemoteDaemonTlsCache();
// Solo la de una conexión, que es lo que hace falta al reaprovisionarla: vaciar la de
// todas obligaría a las demás máquinas a una ida y vuelta por SSH sin motivo.
void clearRemoteDaemonTlsCacheForConnection(const ConnectionProfile& p);

// Intenta levantar el servicio del daemon en la otra máquina. Devuelve si la orden llegó a
// ejecutarse, NO si el daemon revivió: eso se sabe reintentando.
bool tryReviveRemoteDaemonService(const ConnectionProfile& p);

// Cierra todos los túneles vivos de la sesión.
void closeAllTunnels(TransportSession& ses);
// Cierra el de una conexión concreta, si lo hay.
void closeTunnelForConnection(TransportSession& ses, const ConnectionProfile& p);

// El RPC por el túnel.
//
// `commandMayHaveRunOut` distingue «no se pudo enviar» de «se envió y no hubo respuesta».
// Es la diferencia que impide reenviar una mutación destructiva dos veces, y se marca
// ANTES de escribir el primer byte: una escritura parcial también llega.
bool tryRunRemoteAgentRpcViaTunnel(TransportSession& ses,
                                   const ConnectionProfile& p,
                                   const std::vector<std::string>& agentArgs,
                                   int timeoutMs,
                                   std::string& out,
                                   std::string& err,
                                   int& rc,
                                   std::string* failureReason = nullptr,
                                   bool* commandMayHaveRunOut = nullptr);

}  // namespace zfsmgr::base::transport
