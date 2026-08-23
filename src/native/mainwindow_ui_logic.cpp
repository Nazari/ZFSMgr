#include "mainwindow_ui_logic.h"

#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include "zfsallow.h"

namespace zfsmgr::uilogic {

namespace {

namespace ZA = zfsmgr::base::zfsallow;

QStringList aQt(const std::vector<std::string>& v) {
    QStringList out;
    out.reserve(static_cast<int>(v.size()));
    for (const std::string& s : v) {
        out << QString::fromStdString(s);
    }
    return out;
}

// Los permisos, en orden y sin repetir.
//
// El orden importa aunque no lo parezca: `zfs allow` no lo mira, pero la comparación
// «¿cambió esta entrada?» sí, y dos listas con los mismos permisos en distinto orden
// producirían un unallow+allow que no cambia nada.
QStringList normalizaPermisos(const QStringList& tokens) {
    QSet<QString> vistos;
    QStringList out;
    for (const QString& t : tokens) {
        const QString v = t.trimmed();
        if (v.isEmpty() || vistos.contains(v)) {
            continue;
        }
        vistos.insert(v);
        out << v;
    }
    out.sort();
    return out;
}

std::vector<std::string> deQt(const QStringList& v) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(v.size()));
    for (const QString& s : v) {
        out.push_back(s.toStdString());
    }
    return out;
}

ZA::Who quienDe(const QString& targetType) {
    const QString t = targetType.trimmed().toLower();
    if (t == QStringLiteral("user")) {
        return ZA::Who::User;
    }
    if (t == QStringLiteral("group")) {
        return ZA::Who::Group;
    }
    return ZA::Who::Everyone;
}

// La clave que identifica a un destinatario dentro de un alcance. Si dos entradas tienen la
// misma, son la MISMA concesión y lo que cambió son sus permisos.
QString keyOf(const DatasetPermissionGrant& g) {
    return g.targetType.trimmed().toLower() + QLatin1Char('\x1f') + g.targetName.trimmed();
}

ZA::Entry entradaDe(const DatasetPermissionGrant& g, ZA::Scope alcance,
                      const QStringList& permisos) {
    ZA::Entry e;
    e.scope = alcance;
    e.who = quienDe(g.targetType);
    e.name = g.targetName.trimmed().toStdString();
    e.permissions = deQt(permisos);
    return e;
}

// El diff de un alcance: qué se retira y qué se concede para pasar de `antes` a `ahora`.
void comparaAlcance(const QVector<DatasetPermissionGrant>& antes,
                    const QVector<DatasetPermissionGrant>& ahora, ZA::Scope alcance,
                    const std::string& dataset, QList<QStringList>& salida) {
    QMap<QString, DatasetPermissionGrant> mapaAntes;
    QMap<QString, DatasetPermissionGrant> mapaAhora;
    for (const DatasetPermissionGrant& g : antes) {
        mapaAntes.insert(keyOf(g), g);
    }
    for (const DatasetPermissionGrant& g : ahora) {
        mapaAhora.insert(keyOf(g), g);
    }
    QStringList claves;
    for (auto it = mapaAntes.cbegin(); it != mapaAntes.cend(); ++it) {
        claves << it.key();
    }
    for (auto it = mapaAhora.cbegin(); it != mapaAhora.cend(); ++it) {
        if (!claves.contains(it.key())) {
            claves << it.key();
        }
    }
    claves.sort();  // orden estable: dos ejecuciones dan la misma lista de órdenes
    for (const QString& clave : claves) {
        const bool estaba = mapaAntes.contains(clave);
        const bool esta = mapaAhora.contains(clave);
        const QStringList permisosAntes = normalizaPermisos(mapaAntes.value(clave).permissions);
        const QStringList permisosAhora = normalizaPermisos(mapaAhora.value(clave).permissions);

        // Se quita: estaba y ya no, o sigue pero sin ningún permiso —que es lo mismo—.
        if (estaba && (!esta || permisosAhora.isEmpty())) {
            salida << aQt(ZA::argvUnallow(
                entradaDe(mapaAntes.value(clave), alcance, permisosAntes), dataset));
            continue;
        }
        // Se añade.
        if (!estaba && esta) {
            if (!permisosAhora.isEmpty()) {
                salida << aQt(ZA::argvAllow(
                    entradaDe(mapaAhora.value(clave), alcance, permisosAhora), dataset));
            }
            continue;
        }
        // Cambia: se retira lo viejo y se concede lo nuevo.
        //
        // No basta con conceder los que faltan: `zfs allow` SUMA, así que quitar un permiso
        // exige un `unallow` primero. Y no se hace nada si la lista es la misma, que es el
        // cuarto estado y el más frecuente.
        if (estaba && esta && permisosAntes != permisosAhora) {
            salida << aQt(ZA::argvUnallow(
                entradaDe(mapaAntes.value(clave), alcance, permisosAntes), dataset));
            if (!permisosAhora.isEmpty()) {
                salida << aQt(ZA::argvAllow(
                    entradaDe(mapaAhora.value(clave), alcance, permisosAhora), dataset));
            }
        }
    }
}

}  // namespace

QList<QStringList> permissionChangeCommands(const DatasetPermissionsCacheEntry& entry,
                                            const QString& datasetName) {
    QList<QStringList> salida;
    const std::string ds = datasetName.trimmed().toStdString();
    if (ds.empty()) {
        return salida;
    }
    comparaAlcance(entry.originalLocalGrants, entry.localGrants, ZA::Scope::Local, ds, salida);
    comparaAlcance(entry.originalDescendantGrants, entry.descendantGrants,
                   ZA::Scope::Descendants, ds, salida);
    comparaAlcance(entry.originalLocalDescendantGrants, entry.localDescendantGrants,
                   ZA::Scope::LocalAndDescendants, ds, salida);

    // «Al crear» (`-c`) no nombra destinatario: es para quien cree un descendiente. Por eso
    // no es una lista de concesiones sino una sola, y el diff es entre dos listas de
    // permisos.
    const QStringList crearAntes = normalizaPermisos(entry.originalCreatePermissions);
    const QStringList crearAhora = normalizaPermisos(entry.createPermissions);
    if (crearAntes != crearAhora) {
        ZA::Entry e;
        e.scope = ZA::Scope::OnCreate;
        e.permissions = deQt(crearAntes);
        salida << aQt(ZA::argvUnallow(e, ds));
        if (!crearAhora.isEmpty()) {
            ZA::Entry n;
            n.scope = ZA::Scope::OnCreate;
            n.permissions = deQt(crearAhora);
            salida << aQt(ZA::argvAllow(n, ds));
        }
    }

    // Los conjuntos con nombre (`-s @nombre`). Su «destinatario» es el propio nombre del
    // conjunto, con su arroba, y por eso `Who::Set` no añade ninguna bandera.
    QMap<QString, QStringList> conjAntes;
    QMap<QString, QStringList> conjAhora;
    for (const DatasetPermissionSet& s : entry.originalPermissionSets) {
        conjAntes.insert(s.name.trimmed(), normalizaPermisos(s.permissions));
    }
    for (const DatasetPermissionSet& s : entry.permissionSets) {
        conjAhora.insert(s.name.trimmed(), normalizaPermisos(s.permissions));
    }
    QStringList nombres;
    for (auto it = conjAntes.cbegin(); it != conjAntes.cend(); ++it) {
        nombres << it.key();
    }
    for (auto it = conjAhora.cbegin(); it != conjAhora.cend(); ++it) {
        if (!nombres.contains(it.key())) {
            nombres << it.key();
        }
    }
    nombres.sort();
    for (const QString& nombre : nombres) {
        if (nombre.isEmpty()) {
            continue;
        }
        const QStringList antes = conjAntes.value(nombre);
        const QStringList ahora = conjAhora.value(nombre);
        if (antes == ahora) {
            continue;
        }
        ZA::Entry e;
        e.scope = ZA::Scope::Set;
        e.who = ZA::Who::Set;
        e.name = nombre.toStdString();
        if (!antes.isEmpty()) {
            e.permissions = deQt(antes);
            salida << aQt(ZA::argvUnallow(e, ds));
        }
        if (!ahora.isEmpty()) {
            e.permissions = deQt(ahora);
            salida << aQt(ZA::argvAllow(e, ds));
        }
    }
    return salida;
}

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
                                                           bool isWindowsConnection) {
    ConnectionContextMenuState state;
    state.canConnect = !actionsLocked && hasConn && isDisconnected;
    state.canDisconnect = !actionsLocked && hasConn && !isDisconnected;
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
