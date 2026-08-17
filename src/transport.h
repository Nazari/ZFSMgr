#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

#include "connectionstore.h"
#include "transportsession.h"

// Hablar con una máquina remota, sin depender de la ventana.
//
// Son funciones LIBRES que reciben la sesión, no métodos de `MainWindow`. Ese es el
// punto: lo que hay aquí es lo que un CLI necesita, y mientras fueran miembros no se
// podía llamar desde ningún otro sitio.
//
// Aquí dentro no se nombra `MainWindow`, ni `appLog`, ni un widget. Lo que el transporte
// cuenta mientras trabaja sale por el destino de la sesión, y lo que necesita preguntar
// va por su proveedor de credenciales.
//
// `MainWindow` conserva métodos con el mismo nombre que delegan aquí, para no tener que
// tocar los puntos de llamada en el mismo cambio. Ver
// docs/diseno_tecnico_capa_base_sin_qt.md.
namespace transport {

// «LOCAL» como tipo de conexión: no hay SSH de por medio.
bool isLocalConnection(const ConnectionProfile& p);
bool isWindowsConnection(const ConnectionProfile& p);

// Envuelve la orden para el intérprete del otro extremo: PowerShell en Windows, shell
// POSIX en el resto.
QString wrapRemoteCommand(const ConnectionProfile& p, const QString& remoteCmd);

// Material TLS del daemon LOCAL. Vive bajo /etc/zfsmgr con permisos de root, así que hay
// que leerlo elevando; se cachea para no pedir credenciales en cada orden.
bool ensureLocalDaemonTlsMaterial(TransportSession& ses,
                                  QByteArray& serverCertPem,
                                  QByteArray& clientCertPem,
                                  QByteArray& clientKeyPem,
                                  quint16& daemonPort);

// Ejecuta una orden en la máquina. Si `allowAgentRpc`, intenta primero el RPC tipado del
// agente y solo cae a SSH en crudo si no se puede.
bool runSsh(TransportSession& ses,
            const ConnectionProfile& p,
            const QString& remoteCmd,
            int timeoutMs,
            QString& out,
            QString& err,
            int& rc,
            const std::function<void(const QString&)>& onStdoutLine = {},
            const std::function<void(const QString&)>& onStderrLine = {},
            const std::function<void(int)>& onIdleTimeoutRemaining = {},
            const QByteArray& stdinPayload = {},
            bool allowAgentRpc = true,
            bool echoOutputToLog = true);

bool tryAgentRpcOverSsh(TransportSession& ses,
                        const ConnectionProfile& p,
                        const QStringList& agentArgs,
                        int timeoutMs,
                        QString& out,
                        QString& err,
                        int& rc,
                        const std::function<void(const QString&)>& onStdoutLine = {},
                        const std::function<void(const QString&)>& onStderrLine = {},
                        bool echoOutputToLog = true);

// El RPC por el túnel `ssh -L`, que es el camino normal cuando hay daemon.
// `commandMayHaveRunOut` distingue «no se pudo enviar» de «se envió y no hubo
// respuesta». Es la diferencia que impide reenviar una mutación destructiva dos veces.
bool tryRunRemoteAgentRpcViaTunnel(TransportSession& ses,
                                   const ConnectionProfile& p,
                                   const QStringList& agentArgs,
                                   int timeoutMs,
                                   QString& out,
                                   QString& err,
                                   int& rc,
                                   QString* failureReason = nullptr,
                                   bool* commandMayHaveRunOut = nullptr);

}  // namespace transport
