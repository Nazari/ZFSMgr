#include "mainwindow.h"
#include "connectionstore.h"
#include "transportsession.h"

#include <QApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QSplitter>
#include <QTreeWidget>
#include <QMenu>
#include <QTabWidget>
#include <QMenuBar>
#include <QtTest/QtTest>

#include <algorithm>

class GuiMainWindowTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        qputenv("ZFSMGR_TEST_MODE", QByteArrayLiteral("1"));
    }

    // PSRP se retiró como transporte. Un perfil guardado con PSRP debe convertirse a
    // SSH, y sobre todo debe SOLTAR el puerto 5986: es de WinRM, y conservarlo deja
    // una conexión rota sin explicación, que es peor que la de partida.
    void psrpProfilesMigrateToSshAndDropWinrmPort() {
        ConnectionProfile winrm;
        winrm.connType = QStringLiteral("PSRP");
        winrm.port = 5986;
        QVERIFY(ConnectionStore::migratePsrpProfileToSshForTest(winrm));
        QCOMPARE(winrm.connType, QStringLiteral("SSH"));
        QCOMPARE(winrm.port, 22);
        QCOMPARE(winrm.osType, QStringLiteral("Windows"));

        ConnectionProfile noPort;
        noPort.connType = QStringLiteral("psrp");   // sin distinguir mayúsculas
        noPort.port = 0;
        QVERIFY(ConnectionStore::migratePsrpProfileToSshForTest(noPort));
        QCOMPARE(noPort.port, 22);

        // Un puerto elegido a mano no se pisa: puede ser un SSH en puerto no estándar.
        ConnectionProfile custom;
        custom.connType = QStringLiteral("PSRP");
        custom.port = 2222;
        QVERIFY(ConnectionStore::migratePsrpProfileToSshForTest(custom));
        QCOMPARE(custom.port, 2222);

        // Y no debe tocar las conexiones que ya eran SSH.
        ConnectionProfile ssh;
        ssh.connType = QStringLiteral("SSH");
        ssh.port = 2200;
        QVERIFY(!ConnectionStore::migratePsrpProfileToSshForTest(ssh));
        QCOMPARE(ssh.port, 2200);
    }

    // El corte por separador decía "no entrecomillado" pero no lo comprobaba: cortaba
    // en el primer ';', '&' o '|' aunque estuviera dentro de un argumento protegido.
    // Con --mutate-advanced-todir ese argumento es un directorio que elige el usuario.
    void agentArgExtractionRespectsQuotedSeparators() {
        const QString cmd =
            QStringLiteral("PATH=\"$PATH:/sbin\"; export PATH; /usr/local/libexec/zfsmgr-agent "
                           "--mutate-advanced-todir 'tank/x' '/home/x/Copias & Backups' '1'");
        const QStringList args = MainWindow::extractAgentArgsForTest(cmd);
        QCOMPARE(args.size(), 4);
        QCOMPARE(args.at(0), QStringLiteral("--mutate-advanced-todir"));
        QCOMPARE(args.at(2), QStringLiteral("/home/x/Copias & Backups"));
        QCOMPARE(args.at(3), QStringLiteral("1"));

        // Un separador de verdad, fuera de comillas, sí debe cortar.
        const QStringList cut = MainWindow::extractAgentArgsForTest(
            QStringLiteral("/usr/local/libexec/zfsmgr-agent --dump-zfs-mount ; rm -rf /"));
        QCOMPARE(cut.size(), 1);
        QCOMPARE(cut.at(0), QStringLiteral("--dump-zfs-mount"));
    }

    // El extractor decide si una orden se DESVÍA al RPC. Los verbos que el daemon no
    // sirve por ahí no deben desviarse nunca: hacerlo garantiza un "unknown command".
    // Delete un dataset falló exactamente así —«unknown command: --mutate-shell-generic»—
    // porque runAgentCommand sí lo comprobaba y este camino heredado no.
    void agentArgExtractionSkipsCliOnlyVerbs() {
        const QStringList cliOnly = {
            QStringLiteral("--mutate-shell-generic"),
            QStringLiteral("--mutate-advanced-fromdir"),
            QStringLiteral("--mutate-sync-temp-tar-source"),
            QStringLiteral("--mutate-sync-temp-tar-dest"),
        };
        for (const QString& verb : cliOnly) {
            const QStringList args = MainWindow::extractAgentArgsForTest(
                QStringLiteral("/usr/local/libexec/zfsmgr-agent %1 'cGF5bG9hZA=='").arg(verb));
            QVERIFY2(args.isEmpty(),
                     qPrintable(QStringLiteral("%1 no debe desviarse al RPC").arg(verb)));
        }
        // Y uno que sí se sirve por RPC tiene que seguir desviándose.
        const QStringList ok = MainWindow::extractAgentArgsForTest(
            QStringLiteral("/usr/local/libexec/zfsmgr-agent --mutate-zfs-destroy 'tank/x' '0' ''"));
        QCOMPARE(ok.value(0), QStringLiteral("--mutate-zfs-destroy"));
    }

    // Estos tres ejercitan la capa de transporte de mentira. Comprueban QUÉ se le pide
    // al agente, que es lo que ningún test podía ver hasta ahora: los cuatro binarios
    // no ejecutan nada fuera de este equipo.
    //
    // Los tres fallos que motivaron esta capa —una orden con la ruta destrozada, otra
    // que dejó de usar el daemon, y datos que dejaron de rellenarse— compilaban y
    // pasaban todos los tests.

    // Una lectura debe pedirle al agente el verbo y los argumentos exactos, y NO debe
    // salir nada por shell.
    void datasetPropertyReadGoesToTheAgentByArgv() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank")}, {});
        window.setConnectionDaemonStateForTest(0, true, true);
        window.setAgentTransportForTest(
            [](const std::vector<std::string>&, std::string& out, std::string& err, int& rc) {
                out = "filesystem";
                err.clear();
                rc = 0;
                return true;
            });

        QString value;
        QVERIFY(window.getDatasetPropertyForTest(0, QStringLiteral("tank/ds"),
                                                 QStringLiteral("type"), value));
        QCOMPARE(value, QStringLiteral("filesystem"));

        const auto calls = window.agentCallsForTest();
        QCOMPARE(calls.size(), 1);
        QCOMPARE(calls.at(0).argv,
                 (std::vector<std::string>{"--dump-zfs-get-prop", "type", "tank/ds"}));
        QVERIFY2(calls.at(0).shellCommand.empty(),
                 "la lectura no debe construirse como cadena de shell");
    }

    // Sin daemon no hay camino alternativo: la lectura falla y no se intenta nada.
    // Antes esto caía a un "zfs get" por shell y el fallo del agente quedaba oculto.
    void readWithoutDaemonFailsInsteadOfFallingBackToShell() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank")}, {});
        window.setConnectionDaemonStateForTest(0, false, false);
        window.setAgentTransportForTest(
            [](const std::vector<std::string>&, std::string&, std::string&, int& rc) { rc = 0; return true; });

        QString value;
        QVERIFY(!window.getDatasetPropertyForTest(0, QStringLiteral("tank/ds"),
                                                  QStringLiteral("type"), value));
        QVERIFY2(window.agentCallsForTest().isEmpty(),
                 "sin daemon no debe salir ninguna orden, ni al agente ni por shell");
    }

    // Si el agente responde con error, NO debe intentarse nada por shell. Es el
    // escenario exacto en el que reaparecería un respaldo, y el que hace que un fallo
    // del daemon quede oculto: la operación "funciona" y nadie se entera.
    void failingAgentCallDoesNotFallBackToShell() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank")}, {});
        window.setConnectionDaemonStateForTest(0, true, true);
        window.setAgentTransportForTest(
            [](const std::vector<std::string>&, std::string& out, std::string& err, int& rc) {
                out.clear();
                err = "el agente falla a propósito";
                rc = 1;
                return true;
            });

        QString value;
        QVERIFY(!window.getDatasetPropertyForTest(0, QStringLiteral("tank/ds"),
                                                  QStringLiteral("type"), value));
        const auto calls = window.agentCallsForTest();
        QCOMPARE(calls.size(), 1);
        QVERIFY2(calls.at(0).shellCommand.empty(),
                 "tras fallar el agente no debe intentarse el comando clásico por shell");
    }

    // Un argumento con '&' llega entero. Es el caso que truncaba la orden y hacía
    // perder el destino y el indicador de borrar el origen en Hacia Dir.
    void argumentsWithShellSeparatorsSurviveIntact() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank")}, {});
        window.setConnectionDaemonStateForTest(0, true, true);
        window.setAgentTransportForTest(
            [](const std::vector<std::string>&, std::string& out, std::string&, int& rc) {
                out = "on";
                rc = 0;
                return true;
            });

        QString value;
        const QString hostile = QStringLiteral("tank/Copias & Backups; rm -rf /");
        QVERIFY(window.getDatasetPropertyForTest(0, hostile, QStringLiteral("mounted"), value));
        const auto calls = window.agentCallsForTest();
        QCOMPARE(calls.size(), 1);
        QCOMPARE(calls.at(0).argv.size(), std::size_t(3));
        QCOMPARE(QString::fromStdString(calls.at(0).argv.at(2)), hostile);
    }

    void createsMainWindowWithStableObjectNames() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));

        QCOMPARE(window.objectName(), QStringLiteral("mainWindow"));

        // Dos árboles fijos, no uno unificado: el de arriba es el ORIGEN y el de abajo
        // el DESTINO —Role::Top y Role::Bottom en mainwindow_ui.cpp—, cada uno con su
        // pareja de desplegables encima. Se comprueban aquí los cuatro nombres porque
        // son la dirección por la que el resto del código y estas pruebas los alcanzan.
        QVERIFY(window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeTop")) != nullptr);
        QVERIFY(window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeBottom")) != nullptr);
        QVERIFY(window.findChild<QComboBox*>(QStringLiteral("originConnCombo")) != nullptr);
        QVERIFY(window.findChild<QComboBox*>(QStringLiteral("originPoolCombo")) != nullptr);
        QVERIFY(window.findChild<QComboBox*>(QStringLiteral("destinationConnCombo")) != nullptr);
        QVERIFY(window.findChild<QComboBox*>(QStringLiteral("destinationPoolCombo")) != nullptr);
        QVERIFY2(window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeUnified")) == nullptr,
                 "el árbol unificado con todas las conexiones dentro ya no existe");

        // UN solo partidor para los dos paneles, con sus tres filas. Se comprueba el
        // número de secciones porque es lo que se rompe al «añadir un divisor aquí»: dos
        // partidores, uno por panel, y el reparto vertical de los dos lados se descuadra.
        auto* rows = window.findChild<QSplitter*>(QStringLiteral("zfsmgrPaneRowsSplit"));
        QVERIFY2(rows, "no hay partidor compartido de filas");
        QCOMPARE(rows->orientation(), Qt::Vertical);
        QCOMPARE(rows->count(), 3);
        QVERIFY2(!window.findChild<QSplitter*>(QStringLiteral("zfsmgrDatasetPanesSplit")),
                 "entre los dos paneles no debe haber divisor: el ancho va al 50%");

        // Cada panel con su log de conexión debajo.
        QVERIFY(window.findChild<QPlainTextEdit*>(QStringLiteral("originConnLogView")) != nullptr);
        QVERIFY(window.findChild<QPlainTextEdit*>(QStringLiteral("destinationConnLogView")) != nullptr);


        auto* logView = window.findChild<QPlainTextEdit*>(QStringLiteral("applicationLogView"));
        QVERIFY(logView != nullptr);
    }

    void appliesBaseFontToKeyWidgets() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        const QFont baseFont = QApplication::font();

        auto* originTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeTop"));
        QVERIFY(originTree != nullptr);
        QCOMPARE(originTree->font().pointSize(), baseFont.pointSize());

    }

    // Antes se llamaba togglingPoolInfoDoesNotDropPoolRoots y conmutaba la casilla
    // «Mostrar información del pool». Esa casilla ya no existe —no la leía nadie—, pero lo
    // que la prueba garantizaba de verdad sí importa y se conserva: reconstruir el detalle
    // de la conexión no se lleva por delante los pools del árbol.
    void poolRootsSurviveConnectionRebuild() {
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

        auto tienenLosDosPools = [](const QStringList& pools) {
            return pools.size() == 2
                   && std::any_of(pools.cbegin(), pools.cend(), [](const QString& n) { return n.contains(QStringLiteral("tank1")); })
                   && std::any_of(pools.cbegin(), pools.cend(), [](const QString& n) { return n.contains(QStringLiteral("tank2")); });
        };
        QVERIFY(tienenLosDosPools(window.topLevelPoolNamesForTest(false)));
        QVERIFY(tienenLosDosPools(window.topLevelPoolNamesForTest(true)));

        window.rebuildConnectionDetailsForTest();
        QVERIFY(tienenLosDosPools(window.topLevelPoolNamesForTest(false)));
        QVERIFY(tienenLosDosPools(window.topLevelPoolNamesForTest(true)));
    }

    // El testigo «<conexión>::<pool>» se construye en un sitio y se descompone en otro.
    // Mientras llevó la POSICIÓN de la conexión, descomponerlo era un toInt(); al pasar a
    // llevar su identificador estable, esos toInt() empezaron a fallar en silencio y el
    // borrador de propiedades entero dejó de guardarse: «Programar snapshots» preparaba
    // algo que nadie sabía volver a leer, así que no aparecía por ninguna parte.
    void gsaDraftSurvivesTheConnectionToken() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        // Un identificador que NO es un número: es lo que rompía el camino de vuelta.
        profile.id = QStringLiteral("8f1c-not-a-number");
        profile.name = QStringLiteral("Unibody");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;

        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("sback")}, {});
        window.configurePoolDatasetsForTest(
            0,
            QStringLiteral("sback"),
            {MainWindow::UiTestDatasetSeed{QStringLiteral("sback"),
                                           QStringLiteral("/sback"),
                                           QStringLiteral("on"),
                                           QStringLiteral("yes"),
                                           {}}});

        QVERIFY(window.scheduledDatasetsForTest(0, QStringLiteral("sback")).isEmpty());

        window.stageGsaDraftForTest(0,
                                    QStringLiteral("sback"),
                                    QStringLiteral("sback"),
                                    {{QStringLiteral("org.fc16.gsa:activado"), QStringLiteral("on")},
                                     {QStringLiteral("org.fc16.gsa:diario"), QStringLiteral("7")}});

        const QStringList programados = window.scheduledDatasetsForTest(0, QStringLiteral("sback"));
        QCOMPARE(programados, QStringList{QStringLiteral("sback")});
    }

    // Las tres pruebas que había aquí —togglingInlineGsaNodeHidesAndShowsProgramarSnapshots,
    // gsaNodeKeepsExpandedStateAfterConnContentRebuild y
    // automaticSnapshotsAreFilteredFromDatasetWhenHidden— se retiran con la función que
    // ejercitaban. Estaban en QSKIP desde el refactor del árbol unificado de abril, o sea
    // que llevaban meses sin comprobar nada, y su sujeto —las casillas de «mostrar el
    // nodo X en línea»— ya no existe.

    // Un SIGSEGV real: marcar un snapshot en la pestaña «Snapshots» del detalle y pedir
    // «Enviar» reventaba en `m_conns.profiles[src.connIdx]` con `connIdx = -1`.
    //
    // La pestaña no es el árbol: la fila sabe a qué conexión y pool pertenece, pero
    // `setSelectedDataset()` daba la selección por válida con solo el nombre del dataset y
    // se marchaba antes de completarla. Mientras el árbol conservara su selección no se
    // notaba, porque el contexto se copiaba del anterior; en cuanto el árbol se quedaba sin
    // selección —un refresco, un cambio de pool— la selección quedaba armada apuntando a
    // ninguna conexión, y la primera acción de dos extremos indexaba fuera de rango.
    //
    // Se comprueban las dos mitades del arreglo: que la fila aporte conexión y pool, y que
    // una selección con un índice imposible no pueda quedarse guardada como válida.
    void snapshotPickedInDetailTabCarriesItsConnection() {
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
                                           {}},
             MainWindow::UiTestDatasetSeed{QStringLiteral("tank1/datos"),
                                           QStringLiteral("/tank1/datos"),
                                           QStringLiteral("on"),
                                           QStringLiteral("yes"),
                                           {QStringLiteral("diario")}}});
        window.rebuildConnectionDetailsForTest();

        QVERIFY(window.selectDatasetForTest(QStringLiteral("tank1/datos")));

        // El árbol se queda sin selección, que es como lo deja un refresco. La pestaña
        // «Snapshots» sigue mostrando sus filas: es justo el hueco por el que entraba.
        window.clearTransferSelectionForTest(QStringLiteral("origin"));
        QVERIFY(!window.transferSelectionForTest(QStringLiteral("origin")).valid);

        QVERIFY(window.selectSnapshotInPaneDetailForTest(QStringLiteral("tank1/datos"),
                                                         QStringLiteral("diario")));

        const MainWindow::TransferSelectionForTest sel =
            window.transferSelectionForTest(QStringLiteral("origin"));
        QVERIFY(sel.valid);
        QCOMPARE(sel.connIdx, 0);   // antes: -1, y de ahí el core
        QCOMPARE(sel.poolName, QStringLiteral("tank1"));
        QCOMPARE(sel.datasetName, QStringLiteral("tank1/datos"));
        QCOMPARE(sel.snapshotName, QStringLiteral("diario"));

        // Y el cortafuegos: ni por debajo del rango ni por encima. La segunda es la que
        // deja una conexión borrada, cuyo índice sigue guardado en la selección.
        QVERIFY(!window.forceTransferSelectionForTest(QStringLiteral("origin"), -1,
                                                      QStringLiteral("tank1"),
                                                      QStringLiteral("tank1/datos"),
                                                      QString())
                     .valid);
        QVERIFY(!window.forceTransferSelectionForTest(QStringLiteral("dest"), 7,
                                                      QStringLiteral("tank1"),
                                                      QStringLiteral("tank1/datos"),
                                                      QString())
                     .valid);
    }

    // El progreso de una transferencia con origen LOCAL no llegaba nunca al panel.
    //
    // `pollDaemonJobs()` preguntaba por el trabajo con `tryRunRemoteAgentRpcViaTunnel()`, y
    // esa función rechaza de plano cualquier conexión que no sea SSH —«Local» es otro tipo—
    // devolviendo false sin intentar nada. El bucle hacía `continue` y el trabajo no se
    // consultaba jamás: ni progreso, ni enterarse de que había terminado. Y no fallaba: se
    // quedaba callado, que es lo que costó verlo.
    //
    // El transporte de prueba solo intercepta el camino de `runAgentCommand`, o sea el que
    // sirve a Local. Así que si el sondeo vuelve a irse por el túnel, aquí no llega ninguna
    // llamada y los bytes se quedan a cero.
    void localJobProgressReachesTheTransfersPanel() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank1")}, {});
        window.setConnectionDaemonStateForTest(0, true, true);

        window.setAgentTransportForTest(
            [](const std::vector<std::string>& argv, std::string& out, std::string& err,
               int& rc) {
                err.clear();
                rc = 0;
                if (!argv.empty() && argv[0] == "--job-status") {
                    out = "STATE=running\nBYTES=3600034328\nRATE_MIB_S=38.5\nELAPSED_SECS=90\n";
                    return true;
                }
                out.clear();
                return true;
            });

        window.addActiveDaemonJobForTest(0, 0, QStringLiteral("dc813986b04ce990"));
        QCOMPARE(window.daemonJobBytesForTest(QStringLiteral("dc813986b04ce990")), quint64(0));

        window.pollDaemonJobsForTest();

        // Los bytes de la transferencia real que destapó el fallo.
        QCOMPARE(window.daemonJobBytesForTest(QStringLiteral("dc813986b04ce990")),
                 quint64(3600034328ULL));

        // Y que se preguntó por el trabajo que era, no por otro.
        bool preguntoPorElTrabajo = false;
        for (const auto& call : window.agentCallsForTest()) {
            if (call.argv.size() >= 2 && call.argv[0] == "--job-status"
                && call.argv[1] == "dc813986b04ce990") {
                preguntoPorElTrabajo = true;
            }
        }
        QVERIFY(preguntoPorElTrabajo);
    }

    // La lista de Transferencias se rehace cada 2,5 segundos con el progreso, y `clear()`
    // se llevaba la selección por delante: marcar una para cancelarla y llegar al botón era
    // una carrera contra el siguiente refresco.
    void transfersListKeepsTheSelectionAcrossRefreshes() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank1")}, {});

        window.addActiveDaemonJobForTest(0, 0, QStringLiteral("job-uno"));
        window.addActiveDaemonJobForTest(0, 0, QStringLiteral("job-dos"));
        window.refreshTransfersListForTest();

        // La SEGUNDA, para que no valga con que se quede en la fila 0 por casualidad.
        QVERIFY(window.selectTransferJobForTest(QStringLiteral("job-dos")));
        QCOMPARE(window.selectedTransferJobForTest(), QStringLiteral("job-dos"));

        window.refreshTransfersListForTest();
        QCOMPARE(window.selectedTransferJobForTest(), QStringLiteral("job-dos"));

        // Y si el trabajo marcado desaparece —terminó y se fue de la lista—, no se hereda
        // la marca a otro: cancelar «el que quedó donde estaba» sería peor que no cancelar.
        window.addActiveDaemonJobForTest(0, 0, QStringLiteral("job-tres"));
        window.refreshTransfersListForTest();
        QVERIFY(window.selectTransferJobForTest(QStringLiteral("job-tres")));
        window.removeActiveDaemonJobForTest(QStringLiteral("job-tres"));
        window.refreshTransfersListForTest();
        QVERIFY(window.selectedTransferJobForTest().isEmpty()
                || window.selectedTransferJobForTest() == QStringLiteral("job-tres"));
    }

    // Al arrancar la interfaz con una transferencia ya lanzada, el panel salía vacío aunque
    // el `zfs send` siguiera vivo. El rastreo de trabajos huérfanos existía y NO se ejecutaba
    // nunca: se comprobaba «el daemon acaba de ponerse en marcha» leyendo el estado guardado
    // DESPUÉS de haberlo sobrescrito con el nuevo, o sea `X && !X`.
    void orphanedJobsAreLookedForWhenTheDaemonComesUp() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank1")}, {});

        window.setAgentTransportForTest(
            [](const std::vector<std::string>& argv, std::string& out, std::string& err,
               int& rc) {
                err.clear();
                rc = 0;
                out.clear();
                if (!argv.empty() && argv[0] == "--job-list") {
                    out = "JOB={\"id\":\"648b64bba8a7a74c\",\"state\":\"running\"}\n";
                }
                return true;
            });

        window.deliverDaemonBecameActiveForTest(0);

        bool preguntoPorLosTrabajos = false;
        for (const auto& call : window.agentCallsForTest()) {
            if (!call.argv.empty() && call.argv[0] == "--job-list") {
                preguntoPorLosTrabajos = true;
            }
        }
        QVERIFY2(preguntoPorLosTrabajos,
                 "nadie preguntó por los trabajos en marcha al activarse el daemon");
    }

    // Una máquina apagada salía en el desplegable igual que las demás, y solo al elegirla
    // aparecía un árbol vacío — que se lee como «este pool no tiene nada», no como «esta
    // máquina no responde».
    void unreachableConnectionIsMarkedInTheCombo() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("oldlau");
        profile.name = QStringLiteral("oldlau");
        profile.connType = QStringLiteral("SSH");
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank")}, {});

        window.setConnectionStatusForTest(0, QStringLiteral("OK"));
        QStringList filas = window.paneConnectionComboEntriesForTest();
        QCOMPARE(filas.size(), 1);
        QCOMPARE(filas.at(0), QStringLiteral("oldlau|viva"));

        // La máquina se apaga: la validación deja de pasar.
        window.setConnectionStatusForTest(0, QStringLiteral("ERROR"));
        filas = window.paneConnectionComboEntriesForTest();
        QCOMPARE(filas.size(), 1);
        // Color Y palabras: con una sola de las dos no vale.
        QCOMPARE(filas.at(0), QStringLiteral("oldlau  (no connection)|caida"));

        // Y vuelve a la normalidad cuando responde otra vez, que es lo que pasa al
        // encenderla y refrescar.
        window.setConnectionStatusForTest(0, QStringLiteral("OK"));
        QCOMPARE(window.paneConnectionComboEntriesForTest().at(0),
                 QStringLiteral("oldlau|viva"));
    }

    // Las filas alternas de las listas y tablas tienen que llevar su color EN LA HOJA DE
    // ESTILO. Es la única propiedad que no se declaraba, así que caía en la paleta — y en
    // macOS la paleta se fuerza oscura mientras la hoja pinta todo claro: filas alternas
    // casi negras con texto casi negro encima.
    //
    // Se comprueba sobre la hoja de la ventana de verdad, no sobre una copia del literal.
    void alternatingRowsDeclareTheirColour() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        const QString hoja = window.styleSheet();
        QVERIFY2(!hoja.isEmpty(), "la ventana no tiene hoja de estilo");

        // La regla que las cubre debe traer las TRES cosas: fondo, color de texto y fondo
        // alterno. Con dos de las tres vuelve el problema.
        const int inicio = hoja.indexOf(QStringLiteral("QListWidget, QTableWidget, QTreeWidget"));
        QVERIFY2(inicio >= 0, "no está la regla de listas y tablas");
        const int fin = hoja.indexOf(QLatin1Char('}'), inicio);
        QVERIFY(fin > inicio);
        const QString regla = hoja.mid(inicio, fin - inicio);
        QVERIFY2(regla.contains(QStringLiteral("alternate-background-color")),
                 "las filas alternas no declaran color: caerán en la paleta");
        QVERIFY(regla.contains(QStringLiteral("background")));
        QVERIFY(regla.contains(QStringLiteral("color: #")));
    }

    void connectionsMenuGroupsRefreshAndGsa() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        profile.useSudo = true;

        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank1")}, {});

        // Los rótulos salen del menú «Conexiones» DE VERDAD, no de una lista copiada: por
        // eso ahora se comprueba que estén los que tiene que haber, y que no aparezca lo
        // que se agrupó dentro de un submenú.
        const QStringList topLevel = window.connectionsMenuLabelsForTest();
        QVERIFY(topLevel.contains(QStringLiteral("New Connection")));
        QVERIFY(!topLevel.contains(QStringLiteral("GSA")));
        // «Refrescar» de esta conexión lleva el nombre dentro, así que se busca por prefijo.
        bool hasRefresh = false;
        for (const QString& label : topLevel) {
            if (label.startsWith(QStringLiteral("Refresh"))) {
                hasRefresh = true;
                break;
            }
        }
        QVERIFY(hasRefresh);

        const QStringList refreshLabels = window.connectionRefreshMenuLabelsForTest();
        QCOMPARE(refreshLabels.size(), 2);
        QCOMPARE(refreshLabels.at(0), QStringLiteral("This connection"));
        QCOMPARE(refreshLabels.at(1), QStringLiteral("All connections"));

    }

    void poolContextMenuSeparatesImportedFromImportable() {
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

        // Los dos menús son EXCLUYENTES, y antes este test afirmaba lo contrario.
        //
        // Pedía que el pool ya importado ofreciera «Importar renombrando» y que el
        // importable ofreciera «Reguid». None_ de las dos cosas ocurre en la aplicación:
        // en `buildPoolRootMenuState`, `canImport` exige acción «Importar», y Reguid
        // —como Scrub, Destroy y el resto— cuelga de `canExport`, que exige la contraria.
        // Un menú con Importar, Exportar y Reguid a la vez no existe.
        //
        // Pasaba porque `poolContextMenuLabelsForTest` devolvía una lista fija con TODAS
        // las etiquetas y tiraba el estado que calculaba justo encima. Al hacer que
        // devuelva lo que el estado dice, el test se cayó y enseñó lo que llevaba
        // afirmando. Se comprueba en los dos sentidos —lo que está y lo que NO— porque
        // solo con `contains` una lista fija vuelve a pasar sin que nadie se entere.
        const QStringList importedPoolMenu = window.poolContextMenuLabelsForTest(QStringLiteral("tank1"), false);
        QVERIFY(importedPoolMenu.contains(QStringLiteral("Reguid")));
        // «Export» y no «Exportar»: la ventana de prueba se crea con idioma "en" y esa
        // etiqueta SÍ pasa por `trk()`. «Reguid» e «Importar renombrando» no —van como
        // literales sin traducir—, y por eso se leen igual en los tres idiomas.
        QVERIFY(importedPoolMenu.contains(QStringLiteral("Export")));
        QVERIFY(!importedPoolMenu.contains(QStringLiteral("Importar renombrando")));

        const QStringList importablePoolMenu = window.poolContextMenuLabelsForTest(QStringLiteral("tank2"), false);
        QVERIFY(importablePoolMenu.contains(QStringLiteral("Importar renombrando")));
        QVERIFY(!importablePoolMenu.contains(QStringLiteral("Export")));
        QVERIFY(!importablePoolMenu.contains(QStringLiteral("Reguid")));
    }

    // Un lote que toca varias conexiones debe repintar el árbol UNA vez, no una por
    // conexión. Lo que se rompió: al arrancar, la actualización automática de daemons
    // refrescaba cada conexión por separado y cada refresco repintaba el árbol entero,
    // así que el usuario veía el árbol rehacerse tantas veces como conexiones tuviera.
    void batchedConnectionWorkRepaintsTheTreeOnce() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        ConnectionProfile profile;
        profile.id = QStringLiteral("local");
        profile.name = QStringLiteral("Local");
        profile.connType = QStringLiteral("Local");
        window.configureSingleConnectionUiTestState(profile, {QStringLiteral("tank")}, {});

        const auto before = window.uiRebuildCountsForTest();
        window.runWithDeferredUiRebuildForTest([&window]() {
            // Cuatro conexiones actualizándose = cuatro reconstrucciones pedidas.
            for (int i = 0; i < 4; ++i) {
                window.requestConnectionsUiRebuildForTest();
            }
        });
        const auto after = window.uiRebuildCountsForTest();
        QCOMPARE(after.table - before.table, 1);
        QCOMPARE(after.pools - before.pools, 1);
        // rebuildConnectionsTable() termina llamándolo, así que también una sola vez.
        QCOMPARE(after.nodeDetails - before.nodeDetails, 1);

        // Y sin el guardián, cada petición se ejecuta: el guardián agrupa, no suprime.
        const auto beforeUngrouped = window.uiRebuildCountsForTest();
        window.requestConnectionsUiRebuildForTest();
        window.requestConnectionsUiRebuildForTest();
        const auto afterUngrouped = window.uiRebuildCountsForTest();
        QCOMPARE(afterUngrouped.table - beforeUngrouped.table, 2);
    }

    // Qué se considera mutante decide si una orden se REENVÍA tras una respuesta
    // ambigua del daemon. Cerrar el túnel no aborta el trabajo remoto —comprobado—,
    // así que clasificar mal una destructiva la ejecuta dos veces solapadas, y estas
    // hacen `zfs destroy -r` y borran directorios origen.
    void destructiveAgentCommandsAreNeverRetried() {
        const QStringList mustBeMutating = {
            QStringLiteral("--mutate-advanced-todir"),
            QStringLiteral("--mutate-advanced-breakdown"),
            QStringLiteral("--mutate-advanced-assemble"),
            QStringLiteral("--mutate-rsync-local"),
            QStringLiteral("--mutate-zfs-destroy"),
            QStringLiteral("--zfs-pipe-local"),
            QStringLiteral("--zfs-send-to-peer"),
            QStringLiteral("--zfs-recv-listen"),
            QStringLiteral("--repair-alt-mountpoints"),
            // Reenviar este lanza la MISMA transfer por segunda vez.
            QStringLiteral("--job-submit"),
            QStringLiteral("--job-cancel"),
        };
        for (const QString& c : mustBeMutating) {
            QVERIFY2(MainWindow::isMutatingAgentCommandForTest({c}),
                     qPrintable(QStringLiteral("debe considerarse mutante: %1").arg(c)));
        }
        // Y las lecturas no, o se perdería el respaldo para algo que no hace daño
        // repetir.
        const QStringList mustBeReads = {
            QStringLiteral("--dump-zpool-list"),
            QStringLiteral("--dump-zfs-list-all"),
            QStringLiteral("--dump-refresh-basics"),
            QStringLiteral("--dump-daemon-log"),
            QStringLiteral("--health"),
        };
        for (const QString& c : mustBeReads) {
            QVERIFY2(!MainWindow::isMutatingAgentCommandForTest({c}),
                     qPrintable(QStringLiteral("no debe considerarse mutante: %1").arg(c)));
        }
        QVERIFY(!MainWindow::isMutatingAgentCommandForTest({}));
    }

    // El punto de unión por el que se piden credenciales sin depender de que haya una
    // ventana. Es lo que permite que la cadena del transporte pueda usarse desde un CLI.
    //
    // Sin proveedor puesto debe decir que NO: intentar una operación con sudo sin
    // credenciales es peor que no intentarla, porque a los tres fallos pam_faillock
    // bloquea la cuenta diez minutos.
    void credentialProviderIsAskedAndCancelIsHonoured() {
        TransportSession ses;
        QString usuario;
        QString clave;
        QVERIFY2(!ses.askCredentials(QStringLiteral("motivo"), usuario, clave),
                 "sin proveedor puesto NO debe intentarlo");

        QString motivoVisto;
        ses.credentialProvider = [&motivoVisto](const std::string& motivo, std::string& u,
                                                std::string& c) {
            motivoVisto = QString::fromStdString(motivo);
            u = "linarese";
            c = "secreta";
            return true;
        };
        QVERIFY(ses.askCredentials(QStringLiteral("por qué se piden"), usuario, clave));
        QCOMPARE(motivoVisto, QStringLiteral("por qué se piden"));
        QCOMPARE(usuario, QStringLiteral("linarese"));
        QCOMPARE(clave, QStringLiteral("secreta"));

        // Cancelar tiene que propagarse tal cual: quien llama debe poder abortar.
        ses.credentialProvider = [](const std::string&, std::string&, std::string&) { return false; };
        QString u2;
        QString c2;
        QVERIFY(!ses.askCredentials(QStringLiteral("x"), u2, c2));
    }

    // El destino del registro, por el mismo motivo: el transporte cuenta lo que hace sin
    // nombrar appLog. Sin destino puesto no debe reventar, solo no contarlo.
    void logSinkReceivesLevelAndConnection() {
        TransportSession ses;
        ses.log(TransportSession::Level::Info, QStringLiteral("nadie escucha"));  // no revienta

        QVector<QStringList> visto;
        ses.sink = [&visto](TransportSession::Level n, const std::string& connId,
                            const std::string& msg) {
            visto.push_back({QString::number(static_cast<int>(n)), QString::fromStdString(connId),
                             QString::fromStdString(msg)});
        };
        ses.log(TransportSession::Level::Warn, QStringLiteral("general"));
        ses.logConn(TransportSession::Level::Error, QStringLiteral("unib"), QStringLiteral("de conexión"));
        QCOMPARE(visto.size(), 2);
        QVERIFY(visto[0][1].isEmpty());
        QCOMPARE(visto[0][2], QStringLiteral("general"));
        QCOMPARE(visto[1][1], QStringLiteral("unib"));
        QCOMPARE(visto[1][2], QStringLiteral("de conexión"));
    }

    // La barra de menús, con la forma que se pidió.
    //
    // Se comprueba aquí y no mirando una captura porque estas cuatro cosas se deshacen
    // solas: basta con que alguien añada una entrada «donde encaje» para que Idioma
    // se salga de Ajustes o que Ajustes reaparezca como pestaña. Lo que
    // afirma es la ESTRUCTURA, no los rótulos: la ventana se abre en inglés y los textos
    // salen del catálogo.
    void menuBarHasTheAgreedShape() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        QMenuBar* bar = window.menuBar();
        QVERIFY(bar);

        const auto topLevel = [bar]() {
            QStringList out;
            for (QAction* a : bar->actions()) {
                if (a && a->menu()) {
                    out << a->menu()->objectName() + a->text();
                }
            }
            return out;
        };
        const auto menuNamed = [bar](const QString& text) -> QMenu* {
            for (QAction* a : bar->actions()) {
                if (a && a->menu() && a->text().contains(text, Qt::CaseInsensitive)) {
                    return a->menu();
                }
            }
            return nullptr;
        };
        const auto hasAction = [](QMenu* m, const QString& text) {
            if (!m) return false;
            for (QAction* a : m->actions()) {
                if (a && a->text().contains(text, Qt::CaseInsensitive)) return true;
            }
            return false;
        };
        const auto submenuNamed = [](QMenu* m, const QString& text) -> QMenu* {
            if (!m) return nullptr;
            for (QAction* a : m->actions()) {
                if (a && a->menu() && a->text().contains(text, Qt::CaseInsensitive)) {
                    return a->menu();
                }
            }
            return nullptr;
        };

        // 1. Idioma NO está suelto en la barra: se comprueba con el resto de Ajustes.
        QVERIFY2(!menuNamed(QStringLiteral("Language")),
                 qPrintable(QStringLiteral("Idioma volvió a la barra: ")
                            + topLevel().join(QStringLiteral(", "))));

        // 1b. Gestionar conexiones vive en su propio menú de la barra, no en el menú
        // contextual de un nodo del árbol: ese nodo ya no existe. Se rellena al abrirlo,
        // así que hay que provocar la señal antes de mirar dentro.
        QMenu* connections = menuNamed(QStringLiteral("Connections"));
        QVERIFY2(connections, qPrintable(QStringLiteral("no hay menú Conexiones en la barra: ")
                                         + topLevel().join(QStringLiteral(", "))));
        Q_EMIT connections->aboutToShow();
        QVERIFY(hasAction(connections, QStringLiteral("New Connection")));
        QVERIFY(hasAction(connections, QStringLiteral("Refresh all")));
        QVERIFY(hasAction(connections, QStringLiteral("New Pool")));
        QVERIFY(hasAction(connections, QStringLiteral("daemon")));

        // 2. Comprobar conectividad vive en Ayuda, y separada por una barra.
        QMenu* help = menuNamed(QStringLiteral("Ayuda"));
        if (!help) help = menuNamed(QStringLiteral("Help"));
        QVERIFY(help);
        QVERIFY(hasAction(help, QStringLiteral("connectivity")));
        bool foundConnectivity = false;
        bool separatorAfter = false;
        for (QAction* a : help->actions()) {
            if (foundConnectivity) {
                separatorAfter = a && a->isSeparator();
                break;
            }
            if (a && a->text().contains(QStringLiteral("connectivity"), Qt::CaseInsensitive)) {
                foundConnectivity = true;
            }
        }
        QVERIFY2(separatorAfter,
                 "«Comprobar conectividad» tiene que quedar separada del resto por una barra");

        // 3. Ajustes es de primer nivel, con Log dentro y la casilla de confirmación.
        QMenu* settings = menuNamed(QStringLiteral("Settings"));
        QVERIFY2(settings, qPrintable(QStringLiteral("no hay menú Ajustes en la barra: ")
                                      + topLevel().join(QStringLiteral(", "))));
        QMenu* language = submenuNamed(settings, QStringLiteral("Language"));
        QVERIFY2(language, "«Ajustes» tiene que llevar el submenú «Idioma»");
        QVERIFY(hasAction(language, QStringLiteral("Espa")));
        QVERIFY(hasAction(language, QStringLiteral("English")));
        QMenu* logs = submenuNamed(settings, QStringLiteral("Logs"));
        QVERIFY2(logs, "«Ajustes» tiene que llevar el submenú «Logs»");
        // Y los tres submenús de Logs, que son listas cerradas.
        QVERIFY(submenuNamed(logs, QStringLiteral("level")));
        QVERIFY(submenuNamed(logs, QStringLiteral("lines")));
        QVERIFY(submenuNamed(logs, QStringLiteral("size")));
        QVERIFY(hasAction(logs, QStringLiteral("Clear")));
        QVERIFY(hasAction(logs, QStringLiteral("Copy")));

        // La casilla, y con ella el miembro que `mainwindow_dialogs` mantiene al día. Que
        // sea MARCABLE es la mitad del asunto: como acción normal no se vería el estado.
        QAction* confirm = nullptr;
        for (QAction* a : settings->actions()) {
            if (a && a->isCheckable()) {
                confirm = a;
                break;
            }
        }
        QVERIFY2(confirm, "«Ajustes» tiene que llevar la casilla de confirmación");
        QVERIFY(confirm->text().contains(QStringLiteral("confirmation"), Qt::CaseInsensitive));

        // 4. Y ya NO hay una pestaña «Ajustes» abajo.
        const auto tabs = window.findChildren<QTabWidget*>();
        for (QTabWidget* t : tabs) {
            for (int i = 0; i < t->count(); ++i) {
                QVERIFY2(!t->tabText(i).contains(QStringLiteral("Settings"), Qt::CaseInsensitive),
                         "«Ajustes» volvió a ser una pestaña");
            }
        }
    }
};

QTEST_MAIN(GuiMainWindowTest)
#include "gui_mainwindow_test.moc"
