#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "connectionprofile.h"
#include "helpers.h"
#include "transportsession.h"

// Installing or updating the daemon on a machine.
//
// **This is the only operation that can NOT go through the daemon**, and that is not an
// oversight: it is the chicken-and-egg problem. It goes in over SSH and `scp` because if
// there already were a daemon answering, there would be nothing to install. That is why it
// still sends a shell script when everything else stopped doing so — see
// docs/plan_shell_rpc.md.
//
// This used to live inside `src/cli/shell.cpp`, tangled up with the terminal confirmation
// and the `fprintf`s. None of it was shell-specific: it is 200 lines of deciding which
// script fits the system on the other end. Nothing here asks or prints — the caller decides
// how to confirm and how to report, which is what lets more than one client use it without
// duplicating the script.
namespace zfsmgr::base::daemoninstall {

// Why it could not be done, TYPED. A `bool` forced the caller to guess between «there is no
// binary for that platform» —which is fixed by building one— and «the other machine refused
// it», which is not.
enum class Failure {
    None_,
    UnreadableBinary,     // the path does not exist, or the file is empty
    UploadFailed,         // the scp, or the local copy, failed
    InstallFailed,        // the script ran and returned something other than 0
    // La otra máquina no tiene una versión de OpenZFS con la que esto funcione. Se mira
    // ANTES de subir nada: instalar un agente donde no hay ZFS da una máquina que contesta
    // STATUS=OK y no ve un solo pool, que es la peor forma de no funcionar.
    UnsupportedOpenZfs,
};

std::string labelOf(Failure f);

struct Result {
    Failure fallo{Failure::None_};
    int rc{0};
    std::string detail;              // what the other end said, verbatim
    std::string version;             // the one written into agent.conf
    bool versionBehind{false};       // the bundled agent lags behind this client
    bool isMac{false};               // the caller decides whether to warn about «Full Disk Access»

    bool ok() const { return fallo == Failure::None_; }
};

// «linux», «macos», «freebsd» or «windows», spelled the way whoever looks for the bundled
// binary expects. It comes from the profile's `osType`, not from asking the machine.
std::string platformOf(const ConnectionProfile& p);

// The architecture of the OTHER side, by asking it. On Windows it is not asked: it is
// x86_64 and there is no agent for anything else. Empty when the machine does not answer.
std::string remoteArchitecture(TransportSession& ses, const ConnectionProfile& p, bool verbose);

// Qué OpenZFS tiene la otra máquina, preguntándoselo. Se pregunta por SSH y no al daemon,
// porque en una máquina nueva todavía no hay daemon a quien preguntar — que es justo el
// caso en el que esta comprobación importa.
helpers::OpenZfsLectura remoteOpenZfs(TransportSession& ses, const ConnectionProfile& p,
                                      bool verbose);

// La orden que pregunta qué OpenZFS hay, y cómo leer lo que contesta. Públicas porque la
// usan dos caminos —por SSH contra otra máquina y en local— y tienen que ser la misma.
std::string openZfsProbeCommand();
helpers::OpenZfsLectura readOpenZfsProbe(const std::string& salida);

// The script that gets sent, per platform. **Public on purpose**: it is the piece that
// decides where the binary goes, which service manager is used and what gets checked
// afterwards, so a test can examine it without a machine of each system at hand.
std::string installScript(const std::string& platform, const std::string& version,
                               const std::string& apiVersion);

// Installs or updates, and starts it. It asks nothing: confirming is the caller's job,
// because this replaces a binary and restarts a service on the other end.
//
// `trace` receives every line the installation emits, so it can be shown as it happens.
// Instala o actualiza el daemon de ESTA máquina, sin SSH ni configuración: para el
// instalador del paquete, que corre como root en una máquina recién llegada donde todavía
// no hay conexiones ni contraseña maestra.
//
// Ejecuta el MISMO `installScript` que las instalaciones remotas, con el binario por la
// entrada estándar. Comprueba antes OpenZFS con la misma sonda. Con `simular`, no ejecuta
// nada: pasa el guion entero por `traza`. En Windows no hace nada y lo dice: allí el agente
// llega por `install-daemon` desde otra máquina.
Result installLocal(const std::string& platform, const std::string& binaryPath, bool simulate,
                    const std::function<void(const std::string&)>& trace);

// Lo contrario: para el servicio y quita lo que `installScript` puso. Junto a él para que
// las rutas no puedan divergir. La configuración del CLIENTE no se toca.
std::string uninstallScript(const std::string& platform);
Result uninstallLocal(const std::string& platform, bool simulate,
                      const std::function<void(const std::string&)>& trace);

Result install(TransportSession& ses, const ConnectionProfile& profile,
                  const std::string& binaryPath,
                  const std::function<void(const std::string&)>& trace = {},
                  bool verbose = false);

// --- El CLI de la otra máquina.
//
// `install-daemon` pone al día también el `zfsmgr-cli` de la otra punta, si ya tiene uno:
// un daemon nuevo con un CLI viejo al lado es una máquina a medio actualizar. Va por el
// mismo camino que el agente —SSH con sudo, el contenido por la entrada estándar— y por la
// misma razón: es parte del despliegue, no una operación del día a día.

// Lo que hay en la otra máquina. Sin `found`, no tiene CLI y NO se le instala: ponerlo donde
// nadie lo pidió no es actualizar.
struct CliProbe {
    bool found{false};
    bool isLink{false};        // un enlace —el del .app en macOS—: es de otro, no se toca
    std::string path;          // dónde está
    std::string fullVersion;   // «0.99.3.829775496», o vacío si es anterior a decirla
};
CliProbe probeRemoteCli(TransportSession& ses, const ConnectionProfile& profile, bool verbose);
// La sonda y su lectura, públicas para poder probarlas sin máquina.
std::string cliProbeCommand();
CliProbe readCliProbe(const std::string& output);

// Un tar (ustar) en memoria con esos ficheros: (nombre dentro del tar, ruta local). Vacío y
// `error` relleno si alguno no se puede leer o su nombre no cabe.
std::string tarArchive(const std::vector<std::pair<std::string, std::string>>& entries,
                       std::string& error);

// El guion que coloca el kit —bin/zfsmgr-cli, i18n/, agents/— donde ya estaba el CLI:
// `binPath` y, a su lado, `<prefijo>/share/zfsmgr`, que es donde el CLI busca sus catálogos y
// agentes. El binario se sustituye por renombrado, así que uno en marcha no se rompe.
std::string cliInstallScript(const std::string& binPath);

// Sube el kit y lo coloca. `cliBinary` es el de la plataforma de la otra punta; `i18nDir` y
// `agentsDir`, los de este equipo.
//
// `allClis` son los CLI de TODAS las plataformas (clave «linux-x86_64», ruta): se dejan en
// share/zfsmgr/cli/ para que esa máquina quede igual que una instalada con el paquete, y no
// reparta CLI viejos si un día se lanza `install-daemon` desde ella.
Result installCli(TransportSession& ses, const ConnectionProfile& profile,
                  const std::string& cliBinary, const std::string& i18nDir,
                  const std::string& agentsDir,
                  const std::vector<std::pair<std::string, std::string>>& allClis,
                  const std::string& binPath,
                  const std::function<void(const std::string&)>& trace = {},
                  bool verbose = false);

}  // namespace zfsmgr::base::daemoninstall
