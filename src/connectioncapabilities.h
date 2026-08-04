#pragma once

#include <QSet>
#include <QString>

// Qué puede hacer una conexión, y por qué no puede hacer lo demás.
//
// Existe porque durante mucho tiempo la respuesta estuvo repartida en ~170
// comprobaciones sueltas de isWindowsConnection por 20 ficheros. Esa dispersión tuvo
// dos consecuencias medibles: las funciones se fueron apagando en Windows de una en
// una según se descubrían, y el usuario se encontraba acciones que fallaban al
// pulsarlas en vez de salir deshabilitadas con una explicación.
//
// La función de consulta es pura para poder probarla sin levantar la interfaz ni
// tocar la red. Devuelve un motivo tipificado, no un texto: la traducción es cosa de
// la capa de interfaz, que ya tiene el mecanismo.
namespace zfsmgr::caps {

enum class Feature {
    DatasetPermissions,    // zfs allow / unallow
    AutoSnapshotsGsa,      // instantáneas automáticas programadas
    BackgroundJobs,        // --job-submit y el ciclo de sondeo
    AlternateMount,        // montar un dataset en un punto temporal
    RepairAltMountpoints,  // reparar puntos de montaje temporales
    DirBreakdown,          // Desglosar: directorios -> datasets hijos
    DirAssemble,           // Ensamblar: datasets hijos -> directorios
    DirToDir,              // Hacia Dir
    SendRecvStreaming,     // copiar/nivelar snapshot entre máquinas
    RsyncSync,             // sincronizar con rsync
    ShellActions,          // acciones que necesitan un shell Unix
    HelperCommandInstall,  // instalar herramientas auxiliares en el host
    ToolAvailability,      // sondeo de herramientas disponibles
};

// Motivo tipificado. La interfaz lo traduce; aquí no hay texto de usuario.
enum class Reason {
    MissingTool,           // falta una herramienta que el agente necesita ejecutar
    Available,
    DaemonNotReady,        // el agente no está instalado o activo
    DaemonApiMismatch,     // versión de API distinta de la esperada
    WindowsAgentPending,   // el agente de Windows aún no implementa el verbo
    WindowsNeedsUnixShell, // necesita un shell Unix, que Windows ya no usa
    WindowsNotApplicable,  // no tiene sentido en Windows (no es falta de trabajo)
};

struct Platform {
    bool isWindows{false};
    bool daemonActive{false};
    bool daemonApiOk{false};
    // Herramientas que el agente NO encuentra en su PATH. Las pregunta el refresco con
    // --dump-tool-availability, así que responde quien las va a ejecutar.
    QSet<QString> missingTools;
    // Capacidades declaradas por el propio agente. Hoy llega vacío: el daemon aún no
    // las publica en --health. Cuando lo haga, manda sobre la tabla estática de abajo,
    // que si no se desincroniza en cuanto se porte cualquier verbo.
    QSet<QString> daemonCaps;
};

struct Availability {
    bool available{false};
    Reason reason{Reason::Available};
};

Availability featureAvailability(Feature f, const Platform& plat);

// Identificador estable del verbo del agente asociado a la función, o cadena vacía si
// no depende de uno. Es la clave con la que se contrastará daemonCaps.
QString featureAgentVerb(Feature f);

// Herramienta externa sin la cual esa función no puede funcionar, o cadena vacía si no
// depende de ninguna. El agente las invoca con execvp: si no están, la operación falla
// a mitad en vez de no ofrecerse.
QString featureRequiredTool(Feature f);

} // namespace zfsmgr::caps
