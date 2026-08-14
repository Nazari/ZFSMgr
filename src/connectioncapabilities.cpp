#include "connectioncapabilities.h"

namespace zfsmgr::caps {
namespace {

// Funciones que en Windows no están porque el agente todavía no las implementa.
// Comprobado por RPC contra un Windows 11 real: el daemon responde
// "unknown command" a --job-submit, --repair-alt-mountpoints y
// --mutate-advanced-todir; --dump-tool-availability tampoco existe allí.
bool windowsAgentPending(Feature f) {
    switch (f) {
    // BackgroundJobs ya NO está aquí. El comentario que tenía —«depende de fork/waitpid»—
    // era falso: los trabajos siempre se ejecutaron con std::thread. Lo que de verdad los
    // ataba a Unix era el relé del emisor, /dev/urandom para el identificador y la ruta
    // del fichero de estado, y las tres cosas están resueltas.
    case Feature::RepairAltMountpoints:
    case Feature::DirToDir:
    case Feature::ToolAvailability:
        return true;
    // DirBreakdown y DirAssemble ya NO están aquí: funcionan en Windows y está
    // comprobado ejecutándolas contra una máquina real, no leyendo el código.
    //
    // El comentario que había aquí decía que fallaban porque makeTempDir devolvía cadena
    // vacía. Era falso: makeTempDir estaba portado desde hacía tiempo. Lo que de verdad
    // las rompía era otra cosa —la ruta se deducía de la propiedad `mountpoint`, que en
    // Windows vale «/pool/ds» y allí no existe— y encima fallaba en silencio, saltándose
    // todos los directorios y diciendo que había terminado bien.
    // rsync no viaja con el agente y las rutas de Windows ni siquiera pasan su
    // validación, que exige que empiecen por '/'.
    case Feature::RsyncSync:
        return true;
    // SendRecvStreaming ya NO está aquí: el agente de Windows recibe y emite desde las
    // fases 1 y 2. Bombea entre el socket y una tubería propia, en vez de entregarle el
    // descriptor del socket a `zfs`, que es lo que no funciona allí.
    default:
        return false;
    }
}

// Funciones que en Windows no son "trabajo pendiente" sino que no aplican.
bool windowsNotApplicable(Feature f) {
    switch (f) {
    // No hay gestor de paquetes que ofrecer: la aplicación trabaja allí solo con el
    // agente nativo, y no instala herramientas Unix en el host.
    case Feature::HelperCommandInstall:
        return true;
    // Guardar y restaurar el mountpoint para montar en un directorio temporal no
    // significa lo mismo contra letras de unidad.
    case Feature::AlternateMount:
        return true;
    // El planificador de instantáneas se apoya en el bucle del daemon y en los
    // eventos de ZED, y OpenZFS on Windows no trae zed.
    case Feature::AutoSnapshotsGsa:
        return true;
    default:
        return false;
    }
}

// Funciones cuya implementación sigue siendo un script de shell Unix, sin camino por
// el agente ni por PowerShell.
bool windowsNeedsUnixShell(Feature f) {
    return f == Feature::ShellActions;
}

bool requiresDaemon(Feature f) {
    switch (f) {
    case Feature::DatasetPermissions:
    case Feature::BackgroundJobs:
    case Feature::DirBreakdown:
    case Feature::DirAssemble:
    case Feature::DirToDir:
    case Feature::RepairAltMountpoints:
    case Feature::ToolAvailability:
        return true;
    default:
        return false;
    }
}

} // namespace

QString featureAgentVerb(Feature f) {
    switch (f) {
    case Feature::DatasetPermissions:   return QStringLiteral("--dump-zfs-allow");
    case Feature::BackgroundJobs:       return QStringLiteral("--job-submit");
    case Feature::DirBreakdown:         return QStringLiteral("--mutate-advanced-breakdown");
    case Feature::DirAssemble:          return QStringLiteral("--mutate-advanced-assemble");
    case Feature::DirToDir:             return QStringLiteral("--mutate-advanced-todir");
    case Feature::RepairAltMountpoints: return QStringLiteral("--repair-alt-mountpoints");
    case Feature::ToolAvailability:     return QStringLiteral("--dump-tool-availability");
    case Feature::RsyncSync:            return QStringLiteral("--mutate-rsync-local");
    case Feature::SendRecvStreaming:    return QStringLiteral("--zfs-send-to-peer");
    default:                            return QString();
    }
}

QString featureRequiredTool(Feature f) {
    switch (f) {
    // Solo Sincronizar sigue necesitando rsync. Desglosar, Ensamblar y Hacia Dir ya no:
    // copian y verifican con la implementación propia del agente, que además es la que
    // permite hacerlo en Windows, donde rsync no existe. Ver
    // docs/diseno_tecnico_copia_nativa_sin_rsync.md.
    case Feature::RsyncSync:
        return QStringLiteral("rsync");
    // Las instantáneas programadas con destino remoto salen por ssh desde el agente.
    case Feature::AutoSnapshotsGsa:
        return QStringLiteral("ssh");
    case Feature::ShellActions:
        return QStringLiteral("sh");
    default:
        return QString();
    }
}

Availability featureAvailability(Feature f, const Platform& plat) {
    // Una herramienta ausente gana a todo lo demás: da igual que el verbo exista si el
    // agente no puede ejecutar el programa que necesita. Antes esto no se comprobaba y
    // la acción fallaba a mitad, que en Desglosar significa a mitad de mover datos.
    const QString tool = featureRequiredTool(f);
    if (!tool.isEmpty() && plat.missingTools.contains(tool)) {
        return {false, Reason::MissingTool};
    }

    // Lo que declare el propio agente manda sobre cualquier tabla escrita aquí: la
    // tabla es una suposición, su respuesta es un hecho.
    const QString verb = featureAgentVerb(f);
    const bool capsKnown = !plat.daemonCaps.isEmpty();
    if (capsKnown && !verb.isEmpty()) {
        if (plat.daemonCaps.contains(verb)) {
            if (requiresDaemon(f) && !plat.daemonActive) {
                return {false, Reason::DaemonNotReady};
            }
            return {true, Reason::Available};
        }
        return {false, plat.isWindows ? Reason::WindowsAgentPending : Reason::DaemonNotReady};
    }

    if (plat.isWindows) {
        if (windowsNotApplicable(f)) {
            return {false, Reason::WindowsNotApplicable};
        }
        if (windowsNeedsUnixShell(f)) {
            return {false, Reason::WindowsNeedsUnixShell};
        }
        if (windowsAgentPending(f)) {
            return {false, Reason::WindowsAgentPending};
        }
    }

    if (requiresDaemon(f)) {
        if (!plat.daemonActive) {
            return {false, Reason::DaemonNotReady};
        }
        if (!plat.daemonApiOk) {
            return {false, Reason::DaemonApiMismatch};
        }
    }
    return {true, Reason::Available};
}

} // namespace zfsmgr::caps
