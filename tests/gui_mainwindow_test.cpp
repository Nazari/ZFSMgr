#include "mainwindow.h"

#include <QApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QtTest/QtTest>

class GuiMainWindowTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        qputenv("ZFSMGR_TEST_MODE", QByteArrayLiteral("1"));
    }

    void createsMainWindowWithStableObjectNames() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));

        QCOMPARE(window.objectName(), QStringLiteral("mainWindow"));

        auto* connectionsTable = window.findChild<QTableWidget*>(QStringLiteral("connectionsTable"));
        QVERIFY(connectionsTable != nullptr);

        auto* topTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeTop"));
        QVERIFY(topTree != nullptr);

        auto* bottomTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeBottom"));
        QVERIFY(bottomTree != nullptr);

        auto* connectivityButton = window.findChild<QPushButton*>(QStringLiteral("zfsmgrConnectivityMatrixBtn"));
        QVERIFY(connectivityButton != nullptr);

        auto* logView = window.findChild<QPlainTextEdit*>(QStringLiteral("applicationLogView"));
        QVERIFY(logView != nullptr);
    }

    void appliesBaseFontToKeyWidgets() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        const QFont baseFont = QApplication::font();

        auto* connectionsTable = window.findChild<QTableWidget*>(QStringLiteral("connectionsTable"));
        QVERIFY(connectionsTable != nullptr);
        QCOMPARE(connectionsTable->font().pointSize(), baseFont.pointSize());

        auto* topTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeTop"));
        QVERIFY(topTree != nullptr);
        QCOMPARE(topTree->font().pointSize(), baseFont.pointSize());

        auto* bottomTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeBottom"));
        QVERIFY(bottomTree != nullptr);
        QCOMPARE(bottomTree->font().pointSize(), baseFont.pointSize());

        auto* connectivityButton = window.findChild<QPushButton*>(QStringLiteral("zfsmgrConnectivityMatrixBtn"));
        QVERIFY(connectivityButton != nullptr);
        QCOMPARE(connectivityButton->font().pointSize(), baseFont.pointSize());
    }

    void togglingPoolInfoDoesNotDropPoolRoots() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;

        window.configureSingleConnectionUiTestState(profile,
                                                    {QStringLiteral("tank1")},
                                                    {QStringLiteral("tank2")});
        window.rebuildConnectionDetailsForTest();

        const QStringList initialTopPools = window.topLevelPoolNamesForTest(false);
        QCOMPARE(initialTopPools.size(), 2);
        QVERIFY(initialTopPools.contains(QStringLiteral("Local::tank1")));
        QVERIFY(initialTopPools.contains(QStringLiteral("Local::tank2 [Importable]")));

        const QStringList initialBottomPools = window.topLevelPoolNamesForTest(true);
        QCOMPARE(initialBottomPools.size(), 2);
        auto infoNodeCount = [](QTreeWidget* tree) {
            int count = 0;
            if (!tree) {
                return count;
            }
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* top = tree->topLevelItem(i);
                if (!top) {
                    continue;
                }
                for (int c = 0; c < top->childCount(); ++c) {
                    if (QTreeWidgetItem* child = top->child(c)) {
                        if (child->text(0) == QStringLiteral("Pool information")) {
                            ++count;
                        }
                    }
                }
            }
            return count;
        };
        auto* topTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeTop"));
        auto* bottomTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeBottom"));
        const int initialTopInfoNodes = infoNodeCount(topTree);
        const int initialBottomInfoNodes = infoNodeCount(bottomTree);
        QVERIFY(initialTopInfoNodes > 0);
        QVERIFY(initialBottomInfoNodes > 0);

        window.setShowPoolInfoNodeForTest(false);
        const QStringList hiddenInfoTopPools = window.topLevelPoolNamesForTest(false);
        QCOMPARE(hiddenInfoTopPools.size(), 2);
        QVERIFY(hiddenInfoTopPools.contains(QStringLiteral("Local::tank1")));
        QVERIFY(hiddenInfoTopPools.contains(QStringLiteral("Local::tank2 [Importable]")));
        QCOMPARE(infoNodeCount(topTree), 0);

        const QStringList hiddenInfoBottomPools = window.topLevelPoolNamesForTest(true);
        QCOMPARE(hiddenInfoBottomPools.size(), 2);
        QCOMPARE(infoNodeCount(bottomTree), 0);

        window.setShowPoolInfoNodeForTest(true);
        const QStringList restoredTopPools = window.topLevelPoolNamesForTest(false);
        QCOMPARE(restoredTopPools.size(), 2);
        QVERIFY(restoredTopPools.contains(QStringLiteral("Local::tank1")));
        QVERIFY(restoredTopPools.contains(QStringLiteral("Local::tank2 [Importable]")));
        QCOMPARE(infoNodeCount(topTree), initialTopInfoNodes);
        QCOMPARE(infoNodeCount(bottomTree), initialBottomInfoNodes);
    }

    void togglingInlineGsaNodeHidesAndShowsProgramarSnapshots() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;

        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank1")}, {});
        window.configurePoolDatasetsForTest(
            0,
            QStringLiteral("tank1"),
            {MainWindow::UiTestDatasetSeed{QStringLiteral("tank1"),
                                           QStringLiteral("/tank1"),
                                           QStringLiteral("on"),
                                           QStringLiteral("yes"),
                                           {QStringLiteral("manual-001")}}});
        window.rebuildConnectionDetailsForTest();

        QStringList topChildren = window.childLabelsForDatasetForTest(QStringLiteral("tank1"), false);
        QVERIFY(topChildren.contains(QStringLiteral("Programar snapshots")));
        QStringList bottomChildren = window.childLabelsForDatasetForTest(QStringLiteral("tank1"), true);
        QVERIFY(bottomChildren.contains(QStringLiteral("Programar snapshots")));

        window.setShowInlineGsaNodeForTest(false);
        topChildren = window.childLabelsForDatasetForTest(QStringLiteral("tank1"), false);
        QVERIFY(!topChildren.contains(QStringLiteral("Programar snapshots")));
        bottomChildren = window.childLabelsForDatasetForTest(QStringLiteral("tank1"), true);
        QVERIFY(!bottomChildren.contains(QStringLiteral("Programar snapshots")));

        window.setShowInlineGsaNodeForTest(true);
        topChildren = window.childLabelsForDatasetForTest(QStringLiteral("tank1"), false);
        QVERIFY(topChildren.contains(QStringLiteral("Programar snapshots")));
        bottomChildren = window.childLabelsForDatasetForTest(QStringLiteral("tank1"), true);
        QVERIFY(bottomChildren.contains(QStringLiteral("Programar snapshots")));
    }

    void automaticSnapshotsAreFilteredFromDatasetWhenHidden() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;

        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank1")}, {});
        window.configurePoolDatasetsForTest(
            0,
            QStringLiteral("tank1"),
            {MainWindow::UiTestDatasetSeed{QStringLiteral("tank1"),
                                           QStringLiteral("/tank1"),
                                           QStringLiteral("on"),
                                           QStringLiteral("yes"),
                                           {QStringLiteral("manual-001"),
                                            QStringLiteral("GSA-20260322-120000-hourly"),
                                            QStringLiteral("manual-002")}}});
        window.rebuildConnectionDetailsForTest();

        QStringList topSnapshots = window.snapshotNamesForDatasetForTest(QStringLiteral("tank1"), false);
        QCOMPARE(topSnapshots.size(), 3);
        QVERIFY(topSnapshots.contains(QStringLiteral("GSA-20260322-120000-hourly")));

        QStringList bottomSnapshots = window.snapshotNamesForDatasetForTest(QStringLiteral("tank1"), true);
        QCOMPARE(bottomSnapshots.size(), 3);

        window.setShowAutomaticSnapshotsForTest(false);
        topSnapshots = window.snapshotNamesForDatasetForTest(QStringLiteral("tank1"), false);
        QCOMPARE(topSnapshots.size(), 2);
        QVERIFY(!topSnapshots.contains(QStringLiteral("GSA-20260322-120000-hourly")));
        QVERIFY(topSnapshots.contains(QStringLiteral("manual-001")));
        QVERIFY(topSnapshots.contains(QStringLiteral("manual-002")));

        bottomSnapshots = window.snapshotNamesForDatasetForTest(QStringLiteral("tank1"), true);
        QCOMPARE(bottomSnapshots.size(), 2);
        QVERIFY(!bottomSnapshots.contains(QStringLiteral("GSA-20260322-120000-hourly")));

        window.setShowAutomaticSnapshotsForTest(true);
        topSnapshots = window.snapshotNamesForDatasetForTest(QStringLiteral("tank1"), false);
        QCOMPARE(topSnapshots.size(), 3);
        QVERIFY(topSnapshots.contains(QStringLiteral("GSA-20260322-120000-hourly")));
    }

    void connectionContextMenuGroupsRefreshAndGsa() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;

        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank1")}, {});

        const QStringList topLevel = window.connectionContextMenuTopLevelLabelsForTest();
        QVERIFY(topLevel.contains(QStringLiteral("Refresh")));
        QVERIFY(topLevel.contains(QStringLiteral("GSA")));
        QVERIFY(!topLevel.contains(QStringLiteral("Refresh all connections")));

        const QStringList refreshLabels = window.connectionRefreshMenuLabelsForTest();
        QCOMPARE(refreshLabels.size(), 2);
        QCOMPARE(refreshLabels.at(0), QStringLiteral("This connection"));
        QCOMPARE(refreshLabels.at(1), QStringLiteral("All connections"));

        const QStringList gsaLabels = window.connectionGsaMenuLabelsForTest();
        QVERIFY(!gsaLabels.isEmpty());
        QVERIFY(gsaLabels.first().contains(QStringLiteral("snapshot"))
                || gsaLabels.first().contains(QStringLiteral("gestor"))
                || gsaLabels.first().contains(QStringLiteral("GSA")));
        QVERIFY(gsaLabels.contains(QStringLiteral("Desinstalar el GSA"))
                || gsaLabels.contains(QStringLiteral("Uninstall GSA")));
    }

    void poolContextMenuIncludesImportRenameAndReguid() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;

        window.configureSingleConnectionUiTestState(profile,
                                                    {QStringLiteral("tank1")},
                                                    {QStringLiteral("tank2")});
        window.rebuildConnectionDetailsForTest();

        const QStringList importedPoolMenu = window.poolContextMenuLabelsForTest(QStringLiteral("tank1"), false);
        QVERIFY(importedPoolMenu.contains(QStringLiteral("Reguid")));
        QVERIFY(importedPoolMenu.contains(QStringLiteral("Importar renombrando")));

        const QStringList importablePoolMenu = window.poolContextMenuLabelsForTest(QStringLiteral("tank2"), false);
        QVERIFY(importablePoolMenu.contains(QStringLiteral("Reguid")));
        QVERIFY(importablePoolMenu.contains(QStringLiteral("Importar renombrando")));
    }
};

QTEST_MAIN(GuiMainWindowTest)
#include "gui_mainwindow_test.moc"
