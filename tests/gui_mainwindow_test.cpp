#include "mainwindow.h"
#include "connectionstore.h"
#include "transportsession.h"

#include <QApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
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
    // Borrar un dataset falló exactamente así —«unknown command: --mutate-shell-generic»—
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

        // La tabla de conexiones desapareció al unificar la interfaz en un solo
        // árbol, y ese árbol pasó a llamarse connContentTreeUnified: el panel se
        // crea con Role::Unified (mainwindow_ui.cpp), no con Top/Bottom.
        auto* unifiedTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeUnified"));
        QVERIFY(unifiedTree != nullptr);


        auto* logView = window.findChild<QPlainTextEdit*>(QStringLiteral("applicationLogView"));
        QVERIFY(logView != nullptr);
    }

    void appliesBaseFontToKeyWidgets() {
        MainWindow window(QStringLiteral("test"), QStringLiteral("en"));
        const QFont baseFont = QApplication::font();

        auto* unifiedTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeUnified"));
        QVERIFY(unifiedTree != nullptr);
        QCOMPARE(unifiedTree->font().pointSize(), baseFont.pointSize());

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
        QVERIFY(!topLevel.contains(QStringLiteral("GSA")));
        QVERIFY(!topLevel.contains(QStringLiteral("Refresh all connections")));

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
        // importable ofreciera «Reguid». Ninguna de las dos cosas ocurre en la aplicación:
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
            // Reenviar este lanza la MISMA transferencia por segunda vez.
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
        ses.log(TransportSession::Nivel::Info, QStringLiteral("nadie escucha"));  // no revienta

        QVector<QStringList> visto;
        ses.sink = [&visto](TransportSession::Nivel n, const std::string& connId,
                            const std::string& msg) {
            visto.push_back({QString::number(static_cast<int>(n)), QString::fromStdString(connId),
                             QString::fromStdString(msg)});
        };
        ses.log(TransportSession::Nivel::Warn, QStringLiteral("general"));
        ses.logConn(TransportSession::Nivel::Error, QStringLiteral("unib"), QStringLiteral("de conexión"));
        QCOMPARE(visto.size(), 2);
        QVERIFY(visto[0][1].isEmpty());
        QCOMPARE(visto[0][2], QStringLiteral("general"));
        QCOMPARE(visto[1][1], QStringLiteral("unib"));
        QCOMPARE(visto[1][2], QStringLiteral("de conexión"));
    }
};

QTEST_MAIN(GuiMainWindowTest)
#include "gui_mainwindow_test.moc"
