#include "mainwindow_ui_logic.h"

#include <QtTest/QtTest>

using namespace zfsmgr::uilogic;

class UiLogicTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
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
            buildConnectionContextMenuState(true, false, false, false, false, true, false);
        QVERIFY(!state.canConnect);
        QVERIFY(state.canDisconnect);
        QVERIFY(state.canInstallMsys);
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
