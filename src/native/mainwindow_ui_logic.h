#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "connectionmodel.h"

namespace zfsmgr::uilogic {

// Qué órdenes hacen falta para llevar los permisos de un dataset de como estaban a como
// están ahora en la interfaz.
//
// **Es una función pura**, y por eso vive aquí: depende solo de la entrada de caché —que
// lleva los grants originales y los actuales— y del nombre del dataset. No toca la ventana,
// ni la red, ni el estado de ninguna conexión.
//
// Estaba dentro de una lambda enterrada en `applyDatasetPropertyChanges`, una función de 400
// líneas, y eso era lo único que la hacía imposible de probar: el diff tiene CUATRO estados
// por entrada —no estaba y se añade, estaba y se quita, estaba y cambia, no cambia— por TRES
// tipos de alcance, más «al crear» y los conjuntos. Doce combinaciones que nadie había
// comprobado nunca porque no había forma de llamarlas sin abrir la interfaz.
//
// Devuelve una lista de argv de `zfs`, sin el nombre del programa: `{"allow", "-l", "-u",
// "juan", "create,mount", "tank/datos"}`. No devuelve cadenas a propósito —ver
// `commands/zfsallow`, que es quien las compone—: una cadena habría que volver a partirla, y
// ese viaje de ida y vuelta es lo que se está quitando.
QList<QStringList> permissionChangeCommands(const DatasetPermissionsCacheEntry& entry,
                                            const QString& datasetName);

struct PoolRootMenuState {
    bool canRefresh{false};
    bool canImport{false};
    bool canExport{false};
    bool canHistory{false};
    bool canSync{false};
    bool canScrub{false};
    bool canUpgrade{false};
    bool canReguid{false};
    bool canTrim{false};
    bool canInitialize{false};
    bool canClear{false};
    bool canDestroy{false};
};

struct ConnectionContextMenuState {
    bool canConnect{false};
    bool canDisconnect{false};
    bool canRefreshThis{false};
    bool canRefreshAll{false};
    bool canEditDelete{false};
    bool canNewConnection{false};
    bool canNewPool{false};
};

PoolRootMenuState buildPoolRootMenuState(const QString& poolAction,
                                         const QString& poolState,
                                         bool hasPoolRow);

ConnectionContextMenuState buildConnectionContextMenuState(bool hasConn,
                                                           bool isDisconnected,
                                                           bool actionsLocked,
                                                           bool isLocalConnection,
                                                           bool isRedirectedToLocal,
                                                           bool isWindowsConnection);

bool isValidPoolRenameCandidate(const QString& name, QString* errorOut = nullptr);
bool isPoolNameInUse(const QStringList& importedPools,
                     const QStringList& importablePools,
                     const QString& candidate,
                     const QString& originalPoolName = QString());

} // namespace zfsmgr::uilogic
