#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
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

QTreeWidgetItem* findItemByDatasetName(QTreeWidget* tree, const QString& datasetName) {
    if (!tree || datasetName.trimmed().isEmpty()) {
        return nullptr;
    }
    const QString wanted = datasetName.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        if (!item) {
            return nullptr;
        }
        if (item->data(0, Qt::UserRole).toString().trimmed() == wanted) {
            return item;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(item->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* found = rec(tree->topLevelItem(i))) {
            return found;
        }
    }
    return nullptr;
}

QTreeWidgetItem* findItemByLabel(QTreeWidgetItem* parent, const QString& label) {
    if (!parent || label.trimmed().isEmpty()) {
        return nullptr;
    }
    const QString wanted = label.trimmed();
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        if (!item) {
            return nullptr;
        }
        if (item->text(0).trimmed() == wanted) {
            return item;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            if (QTreeWidgetItem* found = rec(item->child(i))) {
                return found;
            }
        }
        return nullptr;
    };
    for (int i = 0; i < parent->childCount(); ++i) {
        if (QTreeWidgetItem* found = rec(parent->child(i))) {
            return found;
        }
    }
    return nullptr;
}

QTreeWidgetItem* findItemByLabels(QTreeWidgetItem* parent, const QStringList& labels) {
    for (const QString& label : labels) {
        if (QTreeWidgetItem* found = findItemByLabel(parent, label)) {
            return found;
        }
    }
    return nullptr;
}

QRect subtreeRect(QTreeWidget* tree, QTreeWidgetItem* item) {
    QRect rect;
    if (!tree || !item) {
        return rect;
    }
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) {
        if (!node) {
            return;
        }
        rect = rect.united(tree->visualItemRect(node));
        if (!node->isExpanded()) {
            return;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            rec(node->child(i));
        }
    };
    rec(item);
    return rect;
}

QPixmap grabTreeViewportRect(QTreeWidget* tree, const QRect& rawRect) {
    if (!tree || rawRect.isNull()) {
        return QPixmap();
    }
    QRect rect = rawRect.adjusted(-12, -10, 12, 10);
    rect = rect.intersected(tree->viewport()->rect());
    if (rect.isEmpty()) {
        return QPixmap();
    }
    return tree->viewport()->grab(rect);
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

    window.configureSingleConnectionUiTestState(profile,
                                                {QStringLiteral("tank1")},
                                                {QStringLiteral("tank2")});
    window.setConnectionGsaStateForTest(0, true, true, QStringLiteral("0.10.0rc1.5"));
    // Los nodos de «Datasets programados» y «Permisos» solo existen si su opción de
    // visualización está puesta. Sin esto no aparecían, sus capturas se saltaban en silencio
    // y en la ayuda se quedaban las de la vez anterior — que es como acabaron siendo de hace
    // meses sin que nadie se enterara.
    //
    // Solo se pone esta: `setShowPoolInfoNodeForTest` y `setShowInlineGsaNodeForTest` están
    // DECLARADAS en mainwindow.h y no implementadas en ninguna parte, así que usarlas no
    // falla al compilar sino al enlazar. Se deja dicho aquí para que quien las busque no
    // pierda el rato.
    window.setShowAutomaticSnapshotsForTest(true);
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
        {MainWindow::UiTestPropertySeed{QStringLiteral("org.fc16.gsa:activado"), QStringLiteral("on")},
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

    window.selectDatasetForTest(QStringLiteral("tank1/user"), false);
    mainTree->expandAll();
    app.processEvents();
    app.processEvents();

    if (!savePixmap(window.grab(), outputPath(outDir, QStringLiteral("main-window.png")), &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }
    // Un solo árbol, una sola captura.
    //
    // Aquí se guardaba el MISMO `mainTree->grab()` dos veces, como «top-tree» y como
    // «bottom-tree». Venía de cuando la vista tenía dos árboles; ahora es unificado
    // (`connContentTreeUnified`), así que la segunda salía byte a byte igual que la primera
    // —comprobado con md5— y no la usaba nadie.
    if (!savePixmap(mainTree->grab(), outputPath(outDir, QStringLiteral("top-tree.png")), &error)) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }

    QTreeWidgetItem* connectionRoot = mainTree->topLevelItemCount() > 0 ? mainTree->topLevelItem(0) : nullptr;
    QTreeWidgetItem* poolRoot = connectionRoot && connectionRoot->childCount() > 0 ? connectionRoot->child(0) : nullptr;
    if (poolRoot) {
        poolRoot->setExpanded(true);
        if (QTreeWidgetItem* autoNode = findItemByLabels(poolRoot,
                                                        {QStringLiteral("Datasets programados"),
                                                         QStringLiteral("Scheduled datasets")})) {
            autoNode->setExpanded(true);
            app.processEvents();
            const QPixmap pix = grabTreeViewportRect(mainTree, subtreeRect(mainTree, autoNode));
            if (!savePixmap(pix, outputPath(outDir, QStringLiteral("schedule-snapshots-node.png")), &error)) {
                fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
                return 1;
            }
        }
    }

    if (QTreeWidgetItem* datasetItem = findItemByDatasetName(mainTree, QStringLiteral("tank1/user"))) {
        if (QTreeWidgetItem* permsNode = findItemByLabels(datasetItem,
                                                          {QStringLiteral("Permisos"),
                                                           QStringLiteral("Permissions")})) {
            permsNode->setExpanded(true);
            app.processEvents();
            const QPixmap pix = grabTreeViewportRect(mainTree, subtreeRect(mainTree, permsNode));
            if (!savePixmap(pix, outputPath(outDir, QStringLiteral("permissions-node.png")), &error)) {
                fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
                return 1;
            }
        }
    }

    const QStringList connectionMenuLabels = window.connectionContextMenuTopLevelLabelsForTest();
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
    if (!savePixmap(renderMenuPixmap(QStringLiteral("Conexión"), connectionMenuLabels, connectionSubmenus),
                    outputPath(outDir, QStringLiteral("connection-context-menu.png")),
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
        QStringLiteral("top-tree.png"),
        QStringLiteral("connection-context-menu.png"),
        QStringLiteral("connection-refresh-menu.png"),
        QStringLiteral("pool-context-menu-imported.png"),
        QStringLiteral("pool-context-menu-importable.png"),
    };
    // Estas dos NO se pueden generar hoy, y por eso están fuera de la lista de exigidas en
    // vez de hacer fallar cada pasada. El motivo no es del generador:
    //
    //  - «Datasets programados» solo aparece si están puestas DOS opciones de visualización
    //    a la vez, y una de ellas —`setShowPoolInfoNodeForTest`— está declarada en
    //    mainwindow.h y NO implementada. Es el punto 9 del backlog antiguo.
    //
    //    Probado el 2026-08-22: sembrar la programación con `stageGsaDraftForTest` —que es
    //    el gancho que parece hecho para esto— NO basta. El nodo cuelga además de esas
    //    opciones de visualización, así que hace falta implementar el gancho que falta; no
    //    es cuestión de datos.
    //  - «Permisos» tampoco aparece en el árbol de demostración.
    //
    // Se avisa en cada pasada para que no se olvide: lo que hay en disco con esos nombres es
    // de la última vez que se pudo, y puede no parecerse a la aplicación de hoy.
    const QStringList noSePueden = {
        QStringLiteral("schedule-snapshots-node.png"),
        QStringLiteral("permissions-node.png"),
    };
    for (const QString& nombre : noSePueden) {
        if (QFileInfo(outputPath(outDir, nombre)).exists()) {
            fprintf(stderr,
                    "aviso: %s no se regenera (ver el comentario de noSePueden); "
                    "lo que hay en disco es antiguo\n",
                    nombre.toLocal8Bit().constData());
        }
    }
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
