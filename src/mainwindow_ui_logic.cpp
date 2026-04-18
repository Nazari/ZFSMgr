#include "mainwindow_ui_logic.h"

#include <QRegularExpression>

namespace zfsmgr::uilogic {

PoolRootMenuState buildPoolRootMenuState(const QString& poolAction,
                                         const QString& poolState,
                                         bool hasPoolRow) {
    PoolRootMenuState state;
    state.canRefresh = hasPoolRow;

    const QString normalizedAction = poolAction.trimmed();
    const QString normalizedState = poolState.trimmed().toUpper();

    state.canImport = (normalizedAction.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0
                       && normalizedState == QStringLiteral("ONLINE"));
    state.canExport = (normalizedAction.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0);
    state.canHistory = state.canExport;
    state.canSync = state.canExport;
    state.canScrub = state.canExport;
    state.canUpgrade = state.canExport;
    state.canReguid = state.canExport;
    state.canTrim = state.canExport;
    state.canInitialize = state.canExport;
    state.canClear = state.canExport;
    state.canDestroy = state.canExport;
    return state;
}

ConnectionContextMenuState buildConnectionContextMenuState(bool hasConn,
                                                           bool isDisconnected,
                                                           bool actionsLocked,
                                                           bool isLocalConnection,
                                                           bool isRedirectedToLocal,
                                                           bool isWindowsConnection,
                                                           bool hasWindowsUnixLayerReady) {
    ConnectionContextMenuState state;
    state.canConnect = !actionsLocked && hasConn && isDisconnected;
    state.canDisconnect = !actionsLocked && hasConn && !isDisconnected;
    state.canInstallMsys =
        hasConn && !actionsLocked && !isDisconnected && isWindowsConnection && !hasWindowsUnixLayerReady;
    state.canRefreshThis = hasConn && !isDisconnected && !actionsLocked;
    state.canRefreshAll = !actionsLocked;
    state.canEditDelete = hasConn && !actionsLocked && !isLocalConnection && !isRedirectedToLocal;
    state.canNewConnection = !actionsLocked;
    state.canNewPool = !actionsLocked && hasConn && !isDisconnected;
    return state;
}

bool isValidPoolRenameCandidate(const QString& name, QString* errorOut) {
    const QString trimmed = name.trimmed();
    QString error;
    if (trimmed.isEmpty()) {
        error = QStringLiteral("El nuevo nombre del pool no puede estar vacío.");
    } else if (trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char('@'))
               || trimmed.contains(QRegularExpression(QStringLiteral("\\s")))) {
        error = QStringLiteral("El nuevo nombre del pool no puede contener espacios, '/' ni '@'.");
    }
    if (errorOut) {
        *errorOut = error;
    }
    return error.isEmpty();
}

bool isPoolNameInUse(const QStringList& importedPools,
                     const QStringList& importablePools,
                     const QString& candidate,
                     const QString& originalPoolName) {
    const QString wanted = candidate.trimmed().toLower();
    const QString original = originalPoolName.trimmed().toLower();
    if (wanted.isEmpty()) {
        return false;
    }
    auto matches = [&](const QStringList& names) -> bool {
        for (const QString& entry : names) {
            const QString current = entry.trimmed().toLower();
            if (!current.isEmpty() && current == wanted && current != original) {
                return true;
            }
        }
        return false;
    };
    return matches(importedPools) || matches(importablePools);
}

} // namespace zfsmgr::uilogic
