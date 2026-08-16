#pragma once

#include <QByteArray>
#include <QString>

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

// NOTA: ensureLocalDaemonTlsMaterial NO está aquí, y no por olvido: llama a runSsh y a
// la resolución de credenciales de sudo, así que entra cuando entre esa cadena.

}  // namespace transport
