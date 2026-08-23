#include "mainwindow_ui_logic.h"

#include <QtTest/QtTest>

using namespace zfsmgr::uilogic;

class UiLogicTest final : public QObject {
    Q_OBJECT

private:
    // Construye un grant sin ceremonia, que si no cada caso ocupa seis líneas.
    static DatasetPermissionGrant grant(const QString& tipo, const QString& nombre,
                                        const QStringList& permisos) {
        DatasetPermissionGrant g;
        g.targetType = tipo;
        g.targetName = nombre;
        g.permissions = permisos;
        return g;
    }
    static QString unaLinea(const QStringList& argv) { return argv.join(QLatin1Char(' ')); }
    static QStringList lineas(const QList<QStringList>& cmds) {
        QStringList out;
        for (const QStringList& c : cmds) {
            out << unaLinea(c);
        }
        return out;
    }

private Q_SLOTS:
    // ── El diff de permisos ────────────────────────────────────────────────────
    //
    // Cuatro estados por entrada —no estaba y se añade, estaba y se quita, estaba y cambia,
    // no cambia— por tres alcances, más «al crear» y los conjuntos. Nunca se habían probado:
    // la lógica vivía dentro de una lambda enterrada en una función de 400 líneas y no había
    // forma de llamarla sin abrir la interfaz.

    void permisosSinCambiosNoManaNingunaOrden() {
        DatasetPermissionsCacheEntry e;
        e.localGrants << grant("user", "juan", {"create", "mount"});
        e.originalLocalGrants << grant("user", "juan", {"mount", "create"});  // otro orden
        // El orden de los permisos NO es un cambio: si lo fuera, cada apertura de la ficha
        // produciría un unallow+allow que deja todo igual.
        QVERIFY(permissionChangeCommands(e, QStringLiteral("tank/d")).isEmpty());
    }

    void permisoNuevoSeConcede() {
        DatasetPermissionsCacheEntry e;
        e.localGrants << grant("user", "juan", {"create"});
        QCOMPARE(lineas(permissionChangeCommands(e, QStringLiteral("tank/d"))),
                 QStringList{QStringLiteral("allow -l -u juan create tank/d")});
    }

    void permisoQuitadoSeRetira() {
        DatasetPermissionsCacheEntry e;
        e.originalLocalGrants << grant("user", "juan", {"create"});
        QCOMPARE(lineas(permissionChangeCommands(e, QStringLiteral("tank/d"))),
                 QStringList{QStringLiteral("unallow -l -u juan create tank/d")});
    }

    void quedarseSinPermisosEsQuitarlo() {
        // Sigue la entrada pero con la lista vacía: es lo mismo que haberla borrado.
        DatasetPermissionsCacheEntry e;
        e.originalLocalGrants << grant("user", "juan", {"create"});
        e.localGrants << grant("user", "juan", {});
        QCOMPARE(lineas(permissionChangeCommands(e, QStringLiteral("tank/d"))),
                 QStringList{QStringLiteral("unallow -l -u juan create tank/d")});
    }

    void cambiarPermisosRetiraYConcede() {
        // **No basta con conceder lo que falta**: `zfs allow` SUMA, así que quitar un permiso
        // exige retirar primero. Sin el unallow, «create,mount» → «create» dejaría mount.
        DatasetPermissionsCacheEntry e;
        e.originalLocalGrants << grant("user", "juan", {"create", "mount"});
        e.localGrants << grant("user", "juan", {"create"});
        QCOMPARE(lineas(permissionChangeCommands(e, QStringLiteral("tank/d"))),
                 (QStringList{QStringLiteral("unallow -l -u juan create,mount tank/d"),
                              QStringLiteral("allow -l -u juan create tank/d")}));
    }

    void losTresAlcancesLlevanSuBandera() {
        DatasetPermissionsCacheEntry e;
        e.localGrants << grant("user", "a", {"mount"});
        e.descendantGrants << grant("user", "b", {"mount"});
        e.localDescendantGrants << grant("user", "c", {"mount"});
        // Local es `-l`, descendientes `-d`, y ambos NO lleva bandera: es lo que hace
        // `zfs allow` por omisión.
        QCOMPARE(lineas(permissionChangeCommands(e, QStringLiteral("tank/d"))),
                 (QStringList{QStringLiteral("allow -l -u a mount tank/d"),
                              QStringLiteral("allow -d -u b mount tank/d"),
                              QStringLiteral("allow -u c mount tank/d")}));
    }

    void gruposYTodosUsanSuPropiaBandera() {
        DatasetPermissionsCacheEntry e;
        e.localGrants << grant("group", "staff", {"mount"});
        e.localDescendantGrants << grant("everyone", "", {"snapshot"});
        const QStringList l = lineas(permissionChangeCommands(e, QStringLiteral("tank/d")));
        QVERIFY(l.contains(QStringLiteral("allow -l -g staff mount tank/d")));
        // «Todos» no nombra a nadie: el destinatario ES la bandera.
        QVERIFY(l.contains(QStringLiteral("allow -e snapshot tank/d")));
    }

    void alCrearNoNombraDestinatario() {
        DatasetPermissionsCacheEntry e;
        e.originalCreatePermissions = QStringList{QStringLiteral("mount")};
        e.createPermissions = QStringList{QStringLiteral("mount"), QStringLiteral("snapshot")};
        // `-c` es para quien cree un descendiente, así que no lleva -u/-g/-e ni nombre.
        QCOMPARE(lineas(permissionChangeCommands(e, QStringLiteral("tank/d"))),
                 (QStringList{QStringLiteral("unallow -c mount tank/d"),
                              QStringLiteral("allow -c mount,snapshot tank/d")}));
    }

    void conjuntosConNombreLlevanSuArroba() {
        DatasetPermissionsCacheEntry e;
        DatasetPermissionSet s;
        s.name = QStringLiteral("@basico");
        s.permissions = QStringList{QStringLiteral("mount"), QStringLiteral("snapshot")};
        e.permissionSets << s;
        // El nombre del conjunto va tal cual, con su arroba, y sin bandera de destinatario.
        QCOMPARE(lineas(permissionChangeCommands(e, QStringLiteral("tank/d"))),
                 QStringList{QStringLiteral("allow -s @basico mount,snapshot tank/d")});
    }

    void sinDatasetNoSeMandaNada() {
        DatasetPermissionsCacheEntry e;
        e.localGrants << grant("user", "juan", {"create"});
        QVERIFY(permissionChangeCommands(e, QString()).isEmpty());
    }

    void elOrdenEsEstableEntreEjecuciones() {
        // Dos entradas del mismo alcance: si el orden dependiera del recorrido de un hash,
        // dos aplicaciones idénticas producirían listas distintas y la vista previa cambiaría
        // sin motivo.
        DatasetPermissionsCacheEntry e;
        e.localGrants << grant("user", "zoe", {"mount"}) << grant("user", "ana", {"mount"});
        const QStringList a = lineas(permissionChangeCommands(e, QStringLiteral("tank/d")));
        const QStringList b = lineas(permissionChangeCommands(e, QStringLiteral("tank/d")));
        QCOMPARE(a, b);
        QCOMPARE(a.first(), QStringLiteral("allow -l -u ana mount tank/d"));
    }

    void poolMenuStateReflectsImportedPoolActions() {
        const PoolRootMenuState state =
            buildPoolRootMenuState(QStringLiteral("Exportar"), QStringLiteral("ONLINE"), true);
        QVERIFY(state.canRefresh);
        QVERIFY(!state.canImport);
        QVERIFY(state.canExport);
        QVERIFY(state.canHistory);
        QVERIFY(state.canSync);
        QVERIFY(state.canScrub);
        QVERIFY(state.canReguid);
        QVERIFY(state.canTrim);
        QVERIFY(state.canInitialize);
        QVERIFY(state.canDestroy);
    }

    void poolMenuStateReflectsImportablePoolActions() {
        const PoolRootMenuState state =
            buildPoolRootMenuState(QStringLiteral("Importar"), QStringLiteral("ONLINE"), true);
        QVERIFY(state.canRefresh);
        QVERIFY(state.canImport);
        QVERIFY(!state.canExport);
        QVERIFY(!state.canReguid);
    }

    void connectionMenuStateReflectsAvailability() {
        const ConnectionContextMenuState state =
            buildConnectionContextMenuState(true, false, false, false, false, true);
        QVERIFY(!state.canConnect);
        QVERIFY(state.canDisconnect);
        QVERIFY(state.canRefreshThis);
        QVERIFY(state.canRefreshAll);
        QVERIFY(state.canEditDelete);
        QVERIFY(state.canNewConnection);
        QVERIFY(state.canNewPool);
    }

    void invalidPoolRenameCandidatesAreRejected() {
        QString error;
        QVERIFY(!isValidPoolRenameCandidate(QString(), &error));
        QVERIFY(!error.isEmpty());

        QVERIFY(!isValidPoolRenameCandidate(QStringLiteral("bad/name"), &error));
        QVERIFY(error.contains(QStringLiteral("/")));

        QVERIFY(!isValidPoolRenameCandidate(QStringLiteral("bad name"), &error));
        QVERIFY(error.contains(QStringLiteral("espacios")));
    }

    void poolNameInUseDetectionIgnoresOriginalName() {
        const QStringList imported{QStringLiteral("tank1"), QStringLiteral("backup")};
        const QStringList importable{QStringLiteral("tank2")};

        QVERIFY(isPoolNameInUse(imported, importable, QStringLiteral("tank2")));
        QVERIFY(isPoolNameInUse(imported, importable, QStringLiteral("backup")));
        QVERIFY(!isPoolNameInUse(imported, importable, QStringLiteral("tank1"), QStringLiteral("tank1")));
        QVERIFY(!isPoolNameInUse(imported, importable, QStringLiteral("newpool")));
    }
};

QTEST_MAIN(UiLogicTest)
#include "ui_logic_test.moc"
