#pragma once

#include <QString>
#include <QStringList>

namespace zfsmgr::uilogic {

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
    bool canDestroy{false};
};

struct ConnectionContextMenuState {
    bool canConnect{false};
    bool canDisconnect{false};
    bool canInstallMsys{false};
    bool canManageGsa{false};
    bool canUninstallGsa{false};
    bool gsaSubmenuEnabled{false};
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
                                                           bool isWindowsConnection,
                                                           bool hasWindowsUnixLayerReady,
                                                           bool canManageGsa,
                                                           bool canUninstallGsa);

bool isValidPoolRenameCandidate(const QString& name, QString* errorOut = nullptr);
bool isPoolNameInUse(const QStringList& importedPools,
                     const QStringList& importablePools,
                     const QString& candidate,
                     const QString& originalPoolName = QString());

} // namespace zfsmgr::uilogic
