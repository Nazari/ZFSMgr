#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {


bool ensureDir(const QString& path, QString* errorOut = nullptr) {
    QDir dir;
    if (dir.mkpath(path)) {
        return true;
    }
    if (errorOut) {
        *errorOut = QStringLiteral("No se pudo crear el directorio de salida: %1").arg(path);
    }
    return false;
}

bool savePixmap(const QPixmap& pixmap, const QString& path, QString* errorOut = nullptr) {
    if (pixmap.isNull()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Pixmap vacío al guardar %1").arg(path);
        }
        return false;
    }
    if (!pixmap.save(path)) {
        if (errorOut) {
            *errorOut = QStringLiteral("No se pudo guardar %1").arg(path);
        }
        return false;
    }
    return true;
}

QPixmap renderMenuPixmap(const QString& title,
                         const QStringList& labels,
                         const QMap<QString, QStringList>& submenus = {}) {
    QMenu menu;
    menu.setTitle(title);
    for (const QString& label : labels) {
        const QString trimmed = label.trimmed();
        if (trimmed.isEmpty()) {
            menu.addSeparator();
            continue;
        }
        const auto subIt = submenus.constFind(trimmed);
        if (subIt != submenus.cend()) {
            QMenu* submenu = menu.addMenu(trimmed);
            for (const QString& childLabel : subIt.value()) {
                submenu->addAction(childLabel);
            }
        } else {
            menu.addAction(trimmed);
        }
    }
    menu.ensurePolished();
    menu.adjustSize();
    menu.resize(menu.sizeHint());
    return menu.grab();
}

QString outputPath(const QString& dir, const QString& name) {
    return QDir(dir).filePath(name);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("ZFSMGR_TEST_MODE", QByteArrayLiteral("1"));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ZFSMgr UI Doc Capture"));

    const QString outDir = (argc >= 2) ? QString::fromLocal8Bit(argv[1]).trimmed()
                                       : QStringLiteral("help/img/auto");
    QString error;
    if (!ensureDir(outDir, &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }

    MainWindow window(QStringLiteral("test"), QStringLiteral("es"));
    window.resize(1680, 1040);

    ConnectionProfile profile;
    profile.id = QStringLiteral("local");
    profile.name = QStringLiteral("Local");
    profile.connType = QStringLiteral("Local");
    profile.useSudo = true;

    // Un transporte de mentira que responde «no» en el acto.
    //
    // El estado de demostración marca el daemon como utilizable —sin eso no se pueden
    // listar los datasets—, y con la puerta abierta el árbol intenta hablar con el agente
    // de verdad: SSH a una máquina que no existe, con sus tiempos de espera. La captura se
    // quedaba colgada. Los datos ya están sembrados en la caché, así que lo único que hace
    // falta es que las llamadas que sobren vuelvan enseguida.
    window.setAgentTransportForTest(
        [](const std::vector<std::string>&, std::string& out, std::string& err, int& rc) {
            out.clear();
            err = "capture: sin transporte";
            rc = 1;
            return true;
        });

    window.configureSingleConnectionUiTestState(profile,
                                                {QStringLiteral("tank1")},
                                                {QStringLiteral("tank2")});
    // Después de sembrar el estado, que lo reemplaza entero, y con el transporte de
    // mentira ya puesto: leer los datasets de un pool pasa por `requireDaemonForRead()`,
    // que sin esto responde «la conexión aún no se ha refrescado» y deja el árbol vacío.
    window.setConnectionDaemonStateForTest(0, true, true);
    window.setConnectionGsaStateForTest(0, true, true, QStringLiteral("0.10.0rc1.5"));
    window.configurePoolDatasetsForTest(
        0,
        QStringLiteral("tank1"),
        {MainWindow::UiTestDatasetSeed{QStringLiteral("tank1"),
                                       QStringLiteral("/tank1"),
                                       QStringLiteral("on"),
                                       QStringLiteral("yes"),
                                       {QStringLiteral("manual-001")}},
         MainWindow::UiTestDatasetSeed{QStringLiteral("tank1/user"),
                                       QStringLiteral("/tank1/user"),
                                       QStringLiteral("on"),
                                       QStringLiteral("yes"),
                                       {QStringLiteral("manual-002"),
                                        QStringLiteral("GSA-hourly-20260324-083718")}},
         MainWindow::UiTestDatasetSeed{QStringLiteral("tank1/user/bin"),
                                       QStringLiteral("/tank1/user/bin"),
                                       QStringLiteral("on"),
                                       QStringLiteral("yes"),
                                       {}},
         MainWindow::UiTestDatasetSeed{QStringLiteral("tank1/user/work"),
                                       QStringLiteral("/tank1/user/work"),
                                       QStringLiteral("on"),
                                       QStringLiteral("yes"),
                                       {}}});
    window.configureDatasetPropertiesForTest(
        0,
        QStringLiteral("tank1/user"),
        QStringLiteral("filesystem"),
        // Las cuatro primeras filas de la tabla son fijas —nombre, punto de montaje,
        // canmount y tamaño— y salían VACÍAS: estas filas sustituyen a las propiedades del
        // objeto, y la siembra de datasets solo llena el registro, no las propiedades.
        {MainWindow::UiTestPropertySeed{QStringLiteral("mountpoint"), QStringLiteral("/tank1/user")},
         MainWindow::UiTestPropertySeed{QStringLiteral("canmount"), QStringLiteral("on")},
         MainWindow::UiTestPropertySeed{QStringLiteral("mounted"), QStringLiteral("yes")},
         MainWindow::UiTestPropertySeed{QStringLiteral("used"), QStringLiteral("18,4G")},
         MainWindow::UiTestPropertySeed{QStringLiteral("referenced"), QStringLiteral("12,1G")},
         MainWindow::UiTestPropertySeed{QStringLiteral("compression"), QStringLiteral("lz4")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:activado"), QStringLiteral("on")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:recursivo"), QStringLiteral("on")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:horario"), QStringLiteral("3")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:diario"), QStringLiteral("-")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:semanal"), QStringLiteral("-")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:mensual"), QStringLiteral("-")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:anual"), QStringLiteral("-")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:nivelar"), QStringLiteral("off")},
         MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:destino"), QStringLiteral("-")}});
    window.rebuildConnectionDetailsForTest();

    window.show();
    app.processEvents();
    app.processEvents();

    // Con qué comparar después para saber si una captura se escribió DE VERDAD en esta
    // pasada, o si lo que hay en disco es la de la vez anterior.
    const QDateTime arranque = QDateTime::currentDateTime().addSecs(-1);

    QTreeWidget* mainTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeUnified"));
    if (!mainTree) {
        mainTree = window.findChild<QTreeWidget*>(QStringLiteral("connContentTreeTop"));
    }
    if (!mainTree) {
        fprintf(stderr, "No se encontró el árbol principal de conexión\n");
        return 1;
    }

    // Un dataset marcado en el panel de ORIGEN antes de la foto: con los dos árboles
    // vacíos de selección, el detalle de abajo enseña la ficha de la conexión y la ventana
    // no cuenta lo que hay que contar —el árbol arriba, el detalle con sus pestañas
    // debajo, y el log de esa conexión al pie—.
    window.selectDatasetForTest(QStringLiteral("tank1/user"), false);
    mainTree->expandAll();
    app.processEvents();
    app.processEvents();

    // «Listo» y no «Loading…»: la ventana de demostración nunca termina de cargar nada
    // —no hay máquina al otro lado—, y esa palabra en una captura de la ayuda hace pensar
    // que la aplicación se queda pillada.
    if (auto* status = window.findChild<QTextEdit*>(QStringLiteral("statusText"))) {
        status->setPlainText(QStringLiteral("Listo"));
    }
    if (auto* progress = window.findChild<QTextEdit*>(QStringLiteral("lastDetailText"))) {
        progress->setPlainText(
            QStringLiteral("[2026-08-24 12:00:00] [NORMAL] Refresco de Local finalizado"));
    }
    app.processEvents();

    if (!savePixmap(window.grab(), outputPath(outDir, QStringLiteral("main-window.png")), &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }

    // El frame de detalle del panel de origen, solo.
    //
    // Aquí se guardaban antes «top-tree», «schedule-snapshots-node» y «permissions-node»,
    // que eran trozos del árbol: los dos últimos, nodos que colgaban de un dataset. Esos
    // nodos ya no existen —propiedades, permisos, contenido y snapshots son PESTAÑAS del
    // detalle—, así que lo que hay que enseñar es el detalle entero con su barra.
    if (QWidget* detail = window.findChild<QWidget*>(QStringLiteral("originDetailFrame"))) {
        if (!savePixmap(detail->grab(), outputPath(outDir, QStringLiteral("detail-tabs.png")), &error)) {
            fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
            return 1;
        }
    }

    const QStringList connectionMenuLabels = window.connectionsMenuLabelsForTest();
    const QStringList refreshMenuLabels = window.connectionRefreshMenuLabelsForTest();
    const QStringList importedPoolLabels = window.poolContextMenuLabelsForTest(QStringLiteral("tank1"), false);
    const QStringList importablePoolLabels = window.poolContextMenuLabelsForTest(QStringLiteral("tank2"), false);

    if (qEnvironmentVariableIsSet("ZFSMGR_CAPTURE_DEBUG")) {
        fprintf(stderr, "[dbg] importado(tank1)=%s\n",
                importedPoolLabels.join(QStringLiteral("|")).toLocal8Bit().constData());
        fprintf(stderr, "[dbg] importable(tank2)=%s\n",
                importablePoolLabels.join(QStringLiteral("|")).toLocal8Bit().constData());
    }

    QMap<QString, QStringList> connectionSubmenus;
    connectionSubmenus.insert(QStringLiteral("Refrescar"), refreshMenuLabels);
    if (!savePixmap(renderMenuPixmap(QStringLiteral("Conexiones"), connectionMenuLabels, connectionSubmenus),
                    outputPath(outDir, QStringLiteral("connections-menu.png")),
                    &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }
    if (!savePixmap(renderMenuPixmap(QStringLiteral("Refrescar"), refreshMenuLabels),
                    outputPath(outDir, QStringLiteral("connection-refresh-menu.png")),
                    &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }

    auto buildPoolMenuPixmap = [](const QStringList& labels) {
        QStringList topLevel;
        QStringList management;
        for (const QString& label : labels) {
            const QString trimmed = label.trimmed();
            if (trimmed == QStringLiteral("Sync")
                || trimmed == QStringLiteral("Scrub")
                || trimmed == QStringLiteral("Upgrade")
                || trimmed == QStringLiteral("Reguid")
                || trimmed == QStringLiteral("Trim")
                || trimmed == QStringLiteral("Initialize")
                || trimmed == QStringLiteral("Destroy")) {
                management.push_back(trimmed);
            } else {
                topLevel.push_back(trimmed);
            }
        }
        QMap<QString, QStringList> submenus;
        if (!management.isEmpty()) {
            topLevel.push_back(QStringLiteral("Gestión"));
            submenus.insert(QStringLiteral("Gestión"), management);
        }
        return renderMenuPixmap(QStringLiteral("Pool"), topLevel, submenus);
    };

    if (!savePixmap(buildPoolMenuPixmap(importedPoolLabels),
                    outputPath(outDir, QStringLiteral("pool-context-menu-imported.png")),
                    &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }
    if (!savePixmap(buildPoolMenuPixmap(importablePoolLabels),
                    outputPath(outDir, QStringLiteral("pool-context-menu-importable.png")),
                    &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }

    // **Y se comprueba que estén todas.**
    //
    // Varias capturas cuelgan de encontrar un nodo concreto del árbol, y si no aparecía se
    // saltaban SIN DECIR NADA: la herramienta terminaba con éxito y en el disco se quedaba
    // la imagen vieja. Así es como la ayuda acabó mostrando capturas de hace meses mientras
    // el generador decía que había ido bien. Si falta alguna, se dice y se falla.
    const QStringList esperadas = {
        QStringLiteral("main-window.png"),
        QStringLiteral("detail-tabs.png"),
        QStringLiteral("connections-menu.png"),
        QStringLiteral("connection-refresh-menu.png"),
        QStringLiteral("pool-context-menu-imported.png"),
        QStringLiteral("pool-context-menu-importable.png"),
    };
    QStringList faltan;
    for (const QString& nombre : esperadas) {
        const QFileInfo fi(outputPath(outDir, nombre));
        // Vale con que exista Y se haya escrito en esta pasada: una imagen vieja que se
        // queda es justo lo que hay que detectar.
        if (!fi.exists() || fi.lastModified() < arranque) {
            faltan.push_back(nombre);
        }
    }
    if (!faltan.isEmpty()) {
        fprintf(stderr,
                "no se generaron %d capturas y en disco quedan las anteriores: %s\n",
                static_cast<int>(faltan.size()),
                faltan.join(QStringLiteral(", ")).toLocal8Bit().constData());
        return 1;
    }

    return 0;
}
