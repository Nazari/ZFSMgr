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
};

QTEST_MAIN(GuiMainWindowTest)
#include "gui_mainwindow_test.moc"
