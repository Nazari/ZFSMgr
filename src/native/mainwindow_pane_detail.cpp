// El frame de detalle de cada panel.
//
// Debajo de cada árbol, separado por un partidor, hay un frame que enseña el detalle de
// lo que esté marcado arriba. Sustituye a dos cosas que estaban repartidas por sitios
// raros:
//
//  - Las columnas C1...C10 del propio árbol, que llevaban las propiedades del dataset
//    tumbadas en horizontal, dos filas por bloque —una de nombres y otra de valores—.
//    Eran ilegibles con más de cuatro propiedades y obligaban a que el árbol tuviera
//    tantas columnas como propiedades quisiera enseñarse.
//  - Los nodos «Properties» e «Info» que colgaban de cada conexión. Al pasar la conexión
//    al desplegable de encima del panel, esos nodos se quedaron sin raíz de la que colgar.
//
// Con el detalle debajo, el árbol vuelve a ser un árbol: una columna con nombres.

#include "mainwindow.h"

#include "connectiondatasettreepane.h"
#include "connectiondatasettreewidget.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStackedWidget>
#include <QMenu>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

// Las tres tablas se parecen lo bastante como para que configurarlas a mano tres veces
// fuera una invitación a que se separaran sin querer.
QTableWidget* makeDetailTable(QWidget* parent, const QStringList& headers) {
    auto* table = new QTableWidget(0, headers.size(), parent);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    if (QHeaderView* header = table->horizontalHeader()) {
        // El ancho sobrante se lo queda el VALOR, no la última columna. Con
        // `stretchLastSection` la casilla de «heredada» se llevaba media tabla para
        // enseñar una marca de siete píxeles.
        header->setStretchLastSection(false);
        for (int col = 0; col < headers.size(); ++col) {
            header->setSectionResizeMode(col, col == 1 ? QHeaderView::Stretch
                                                       : QHeaderView::Interactive);
        }
        if (headers.size() > 2) {
            table->setColumnWidth(2, 90);
        }
    }
    return table;
}

void setDetailRows(QTableWidget* table, const QVector<QPair<QString, QString>>& rows) {
    if (!table) {
        return;
    }
    table->setRowCount(0);
    for (const auto& row : rows) {
        const int r = table->rowCount();
        table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(row.first));
        table->setItem(r, 1, new QTableWidgetItem(row.second));
    }
    if (table->columnCount() > 0) {
        table->resizeColumnToContents(0);
    }
}

}  // namespace

QWidget* MainWindow::buildPaneDetail(int paneIdx, QWidget* parent) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const bool isOrigin = (paneIdx == 0);

    auto* frame = new QWidget(parent);
    frame->setObjectName(isOrigin ? QStringLiteral("originDetailFrame")
                                  : QStringLiteral("destinationDetailFrame"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    pane.detailTitle = new QLabel(frame);
    QFont titleFont = pane.detailTitle->font();
    titleFont.setBold(true);
    pane.detailTitle->setFont(titleFont);
    // Una sola línea y elidida: el título lleva rutas de dataset, y con wordWrap una ruta
    // larga se come el alto del detalle, que es justo lo que hay que mirar.
    pane.detailTitle->setWordWrap(false);
    pane.detailTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(pane.detailTitle, 0);

    pane.detailStack = new QStackedWidget(frame);

    const QString colProp = trk(QStringLiteral("t_detail_col_prop_001"),
                                QStringLiteral("Propiedad"),
                                QStringLiteral("Property"));
    const QString colValue = trk(QStringLiteral("t_detail_col_value_001"),
                                 QStringLiteral("Valor"),
                                 QStringLiteral("Value"));
    const QString colSource = trk(QStringLiteral("t_detail_col_source_001"),
                                  QStringLiteral("Origen"),
                                  QStringLiteral("Source"));
    const QString colInherited = trk(QStringLiteral("t_detail_col_inherited_001"),
                                     QStringLiteral("Heredada"),
                                     QStringLiteral("Inherited"));

    pane.connDetailTable = makeDetailTable(pane.detailStack, {colProp, colValue});
    pane.connDetailTable->setObjectName(isOrigin ? QStringLiteral("originConnDetailTable")
                                                : QStringLiteral("destinationConnDetailTable"));
    pane.detailStack->addWidget(pane.connDetailTable);

    auto* poolPage = new QWidget(pane.detailStack);
    auto* poolLayout = new QVBoxLayout(poolPage);
    poolLayout->setContentsMargins(0, 0, 0, 0);
    poolLayout->setSpacing(2);
    pane.poolDetailTable = makeDetailTable(poolPage, {colProp, colValue, colSource});
    pane.poolDetailTable->setObjectName(isOrigin ? QStringLiteral("originPoolDetailTable")
                                                 : QStringLiteral("destinationPoolDetailTable"));
    pane.poolDetailStatus = new QPlainTextEdit(poolPage);
    pane.poolDetailStatus->setReadOnly(true);
    pane.poolDetailStatus->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = pane.poolDetailStatus->font();
    mono.setFamily(QStringLiteral("monospace"));
    pane.poolDetailStatus->setFont(mono);
    poolLayout->addWidget(pane.poolDetailTable, 3);
    poolLayout->addWidget(pane.poolDetailStatus, 2);
    pane.detailStack->addWidget(poolPage);

    // La tabla de propiedades del dataset NO es una tabla más: es la que rellena
    // `refreshDatasetProperties()` y edita `onDatasetPropsCellChanged()`, con sus
    // desplegables para las propiedades de valores cerrados, su casilla de «heredada» y
    // su borrador contra el que trabajan Aplicar y Deshacer. Todo eso estaba escrito
    // desde hacía tiempo apuntando a `m_connContentPropsTable`, un miembro que nunca
    // llegaba a asignarse: la edición de propiedades se hacía por las columnas C1...C10
    // del árbol. Aquí recupera su sitio.
    pane.datasetTabs = new QTabWidget(pane.detailStack);
    pane.datasetTabs->setDocumentMode(true);
    pane.datasetDetailTable = makeDetailTable(pane.datasetTabs, {colProp, colValue, colInherited});
    pane.datasetDetailTable->setObjectName(isOrigin ? QStringLiteral("originDatasetDetailTable")
                                                    : QStringLiteral("destinationDatasetDetailTable"));
    pane.datasetDetailTable->setEditTriggers(QAbstractItemView::DoubleClicked
                                             | QAbstractItemView::EditKeyPressed
                                             | QAbstractItemView::AnyKeyPressed);
    connect(pane.datasetDetailTable, &QTableWidget::cellChanged, this,
            &MainWindow::onDatasetPropsCellChanged);
    pane.datasetTabs->addTab(pane.datasetDetailTable,
                             trk(QStringLiteral("t_detail_tab_props_001"),
                                 QStringLiteral("Propiedades"),
                                 QStringLiteral("Properties")));

    pane.datasetPermsTree = new QTreeWidget(pane.datasetTabs);
    pane.datasetPermsTree->setObjectName(isOrigin ? QStringLiteral("originDatasetPermsTree")
                                                  : QStringLiteral("destinationDatasetPermsTree"));
    pane.datasetPermsTree->setColumnCount(1);
    pane.datasetPermsTree->setHeaderLabels({trk(QStringLiteral("t_detail_perms_col_001"),
                                                QStringLiteral("Delegación / permiso"),
                                                QStringLiteral("Delegation / permission"))});
    pane.datasetPermsTree->setRootIsDecorated(true);
    pane.datasetPermsTree->setUniformRowHeights(true);
    connect(pane.datasetPermsTree, &QTreeWidget::itemChanged, this,
            [this, paneIdx](QTreeWidgetItem* item, int column) {
                if (column == 0) {
                    commitPanePermissionGrant(paneIdx, item);
                }
            });
    pane.datasetTabs->addTab(pane.datasetPermsTree,
                             trk(QStringLiteral("t_detail_tab_perms_001"),
                                 QStringLiteral("Permisos"),
                                 QStringLiteral("Permissions")));
    // Holds: solo tiene sentido para un snapshot, así que la pestaña se apaga cuando lo
    // marcado es un dataset. Estaban también en el árbol, como nodo «Holds (n)» con sus
    // filas de propiedades en las columnas C1...C10.
    pane.datasetHoldsTable = makeDetailTable(pane.datasetTabs,
                                             {trk(QStringLiteral("t_hold_col_tag_001"),
                                                  QStringLiteral("Hold"),
                                                  QStringLiteral("Hold")),
                                              trk(QStringLiteral("t_hold_col_ts_001"),
                                                  QStringLiteral("Fecha"),
                                                  QStringLiteral("Timestamp"))});
    pane.datasetHoldsTable->setObjectName(isOrigin ? QStringLiteral("originDatasetHoldsTable")
                                                   : QStringLiteral("destinationDatasetHoldsTable"));
    pane.datasetHoldsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pane.datasetHoldsTable, &QWidget::customContextMenuRequested, this,
            [this, paneIdx](const QPoint& pos) {
                DatasetPane& p = m_datasetPanes[paneIdx];
                QTableWidget* table = p.datasetHoldsTable;
                if (!table) {
                    return;
                }
                QTableWidgetItem* cell = table->itemAt(pos);
                if (!cell) {
                    return;
                }
                QTableWidgetItem* tagCell = table->item(cell->row(), 0);
                if (!tagCell) {
                    return;
                }
                QMenu menu(table);
                QAction* release = menu.addAction(trk(QStringLiteral("t_release_hold_title001"),
                                                      QStringLiteral("Liberar"),
                                                      QStringLiteral("Release")));
                if (menu.exec(table->viewport()->mapToGlobal(pos)) != release) {
                    return;
                }
                releaseSnapshotHoldNamed(table->property("zfsmgr.holdsConnIdx").toInt(),
                                         table->property("zfsmgr.holdsPool").toString(),
                                         table->property("zfsmgr.holdsDataset").toString(),
                                         table->property("zfsmgr.holdsSnapshot").toString(),
                                         tagCell->text());
            });
    pane.datasetTabs->addTab(pane.datasetHoldsTable,
                             trk(QStringLiteral("t_detail_tab_holds_001"),
                                 QStringLiteral("Holds"),
                                 QStringLiteral("Holds")));

    // Los permisos se leen al ABRIR su pestaña, no al marcar el dataset. Leerlos con la
    // selección sería una llamada remota por cada movimiento del cursor; abrir la pestaña
    // es un gesto deliberado y ahí sí toca preguntar a la máquina. Es el mismo trato que
    // tenían cuando había que desplegar el nodo «Permisos» del árbol.
    connect(pane.datasetTabs, &QTabWidget::currentChanged, this,
            [this, paneIdx](int index) {
                DatasetPane& p = m_datasetPanes[paneIdx];
                if (!p.datasetTabs || !p.datasetPermsTree
                    || p.datasetTabs->widget(index) != p.datasetPermsTree) {
                    return;
                }
                const int connIdx = p.datasetPermsTree->property("zfsmgr.permsConnIdx").toInt();
                const QString poolName = p.datasetPermsTree->property("zfsmgr.permsPool").toString();
                const QString dataset = p.datasetPermsTree->property("zfsmgr.permsDataset").toString();
                if (connIdx < 0 || poolName.isEmpty() || dataset.isEmpty()) {
                    return;
                }
                const DatasetPermissionsCacheEntry* entry =
                    datasetPermissionsEntry(connIdx, poolName, dataset);
                if (entry && entry->loaded) {
                    return;
                }
                if (ensureDatasetPermissionsLoaded(connIdx, poolName, dataset)) {
                    fillPanePermissions(paneIdx, connIdx, poolName, dataset);
                }
            });
    pane.detailStack->addWidget(pane.datasetTabs);

    layout->addWidget(pane.detailStack, 1);
    return frame;
}

// Los campos del perfil de la conexión, los mismos que enseñaba el nodo «Properties».
// La contraseña sale enmascarada: esto es una vista, y el sitio de cambiarla es el
// diálogo de editar la conexión, que además sabe volver a cifrarla.
QVector<QPair<QString, QString>> MainWindow::connectionProfileRows(int connIdx) const {
    QVector<QPair<QString, QString>> rows;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return rows;
    }
    const ConnectionProfile& p = m_conns.profiles.at(connIdx);
    rows.push_back({QStringLiteral("name"), p.name});
    rows.push_back({QStringLiteral("machine_uid"), p.machineUid});
    rows.push_back({QStringLiteral("conn_type"), p.connType});
    rows.push_back({QStringLiteral("os_type"), p.osType});
    rows.push_back({QStringLiteral("host"), p.host});
    rows.push_back({QStringLiteral("port"), QString::number(p.port)});
    rows.push_back({QStringLiteral("username"), p.username});
    rows.push_back({QStringLiteral("password"),
                    p.password.isEmpty() ? QString() : QStringLiteral("********")});
    rows.push_back({QStringLiteral("key_path"), p.keyPath});
    rows.push_back({QStringLiteral("use_sudo"),
                    p.useSudo ? QStringLiteral("true") : QStringLiteral("false")});
    return rows;
}

// Lo que enseñaba el nodo «Info»: estado, sistema, ZFS y daemon. Es diagnóstico —el
// porqué de que una conexión salga en rojo— y por eso no se ha quedado por el camino al
// retirar los nodos auxiliares.
QVector<QPair<QString, QString>> MainWindow::connectionInfoRows(int connIdx) const {
    QVector<QPair<QString, QString>> rows;
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || connIdx >= m_conns.states.size()) {
        return rows;
    }
    const ConnectionRuntimeState& st = m_conns.states.at(connIdx);
    const auto listOrNone = [this](const QStringList& values) -> QString {
        return values.isEmpty() ? trk(QStringLiteral("t_none_001"),
                                      QStringLiteral("(ninguno)"),
                                      QStringLiteral("(none)"))
                                : values.join(QStringLiteral(", "));
    };
    const QString dash = QStringLiteral("-");
    rows.push_back({trk(QStringLiteral("t_status_001"),
                        QStringLiteral("Estado"),
                        QStringLiteral("Status")),
                    st.status.trimmed().isEmpty() ? dash : st.status.trimmed()});
    const QString colorReason = connectionStateColorReason(connIdx).trimmed();
    if (!colorReason.isEmpty()) {
        rows.push_back({trk(QStringLiteral("t_color_reason_001"),
                            QStringLiteral("Motivo del color"),
                            QStringLiteral("Color reason")),
                        colorReason});
    }
    rows.push_back({trk(QStringLiteral("t_os_001"),
                        QStringLiteral("Sistema operativo"),
                        QStringLiteral("Operating system")),
                    st.osLine.trimmed().isEmpty() ? dash : st.osLine.trimmed()});
    rows.push_back({trk(QStringLiteral("t_conn_method_001"),
                        QStringLiteral("Método de conexión"),
                        QStringLiteral("Connection method")),
                    st.connectionMethod.trimmed().isEmpty()
                        ? m_conns.profiles.at(connIdx).connType.trimmed()
                        : st.connectionMethod.trimmed()});
    rows.push_back({QStringLiteral("OpenZFS"),
                    st.zfsVersionFull.trimmed().isEmpty()
                        ? (st.zfsVersion.trimmed().isEmpty()
                               ? dash
                               : QStringLiteral("OpenZFS %1").arg(st.zfsVersion.trimmed()))
                        : st.zfsVersionFull.trimmed()});
    rows.push_back({trk(QStringLiteral("t_conn_agent_001"),
                        QStringLiteral("Daemon"),
                        QStringLiteral("Daemon")),
                    !st.daemonInstalled
                        ? trk(QStringLiteral("t_conn_agent_not_installed_001"),
                              QStringLiteral("no instalado"),
                              QStringLiteral("not installed"))
                        : QStringLiteral("%1 | %2")
                              .arg(st.daemonVersion.trimmed().isEmpty()
                                       ? dash
                                       : st.daemonVersion.trimmed(),
                                   st.daemonActive
                                       ? trk(QStringLiteral("t_conn_agent_running_001"),
                                             QStringLiteral("en marcha"),
                                             QStringLiteral("running"))
                                       : trk(QStringLiteral("t_conn_agent_stopped_001"),
                                             QStringLiteral("parado"),
                                             QStringLiteral("stopped")))});
    rows.push_back({trk(QStringLiteral("t_helper_platform_001"),
                        QStringLiteral("Plataforma instalación auxiliar"),
                        QStringLiteral("Helper install platform")),
                    st.helperPlatformLabel.trimmed().isEmpty() ? dash
                                                               : st.helperPlatformLabel.trimmed()});
    rows.push_back({trk(QStringLiteral("t_package_manager_001"),
                        QStringLiteral("Gestor de paquetes"),
                        QStringLiteral("Package manager")),
                    st.helperPackageManagerLabel.trimmed().isEmpty()
                        ? dash
                        : st.helperPackageManagerLabel.trimmed()});
    rows.push_back({trk(QStringLiteral("t_installable_commands_001"),
                        QStringLiteral("Comandos instalables desde ZFSMgr"),
                        QStringLiteral("Commands installable from ZFSMgr")),
                    listOrNone(st.helperInstallableCommands)});
    return rows;
}

void MainWindow::updatePaneDetailForTree(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    for (int paneIdx = 0; paneIdx < 2; ++paneIdx) {
        const DatasetPane& pane = m_datasetPanes[paneIdx];
        if (pane.treeWidget && pane.treeWidget->tree() == tree) {
            updatePaneDetail(paneIdx);
            return;
        }
    }
}

void MainWindow::updatePaneDetail(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (!pane.detailStack || !pane.treeWidget) {
        return;
    }
    QTreeWidget* tree = pane.treeWidget->tree();
    QTreeWidgetItem* item = tree ? tree->currentItem() : nullptr;
    if (tree && !item) {
        const auto selected = tree->selectedItems();
        if (!selected.isEmpty()) {
            item = selected.first();
        }
    }

    // Sin nada marcado, el detalle es el de la conexión del panel. Es lo que hace que
    // Properties e Info sigan estando a un clic aunque ya no cuelguen de ningún nodo.
    if (!item) {
        pane.detailStack->setCurrentWidget(pane.connDetailTable);
        QVector<QPair<QString, QString>> rows = connectionProfileRows(pane.connIdx);
        const QVector<QPair<QString, QString>> info = connectionInfoRows(pane.connIdx);
        if (!info.isEmpty()) {
            rows.push_back({QString(), QString()});
            rows.append(info);
        }
        setDetailRows(pane.connDetailTable, rows);
        pane.detailTitle->setText(
            (pane.connIdx >= 0 && pane.connIdx < m_conns.profiles.size())
                ? m_conns.profiles.at(pane.connIdx).name
                : trk(QStringLiteral("t_detail_nothing_001"),
                      QStringLiteral("(sin conexión)"),
                      QStringLiteral("(no connection)")));
        return;
    }

    const DatasetSelectionContext ctx = currentConnContentSelection(tree);
    const int connIdx = ctx.valid ? ctx.connIdx : pane.connIdx;
    const QString poolName = ctx.poolName.trimmed();
    const QString dataset = ctx.datasetName.trimmed();

    if (!dataset.isEmpty() && connIdx >= 0 && !poolName.isEmpty()) {
        const QString objectName = ctx.snapshotName.trimmed().isEmpty()
                                       ? dataset
                                       : QStringLiteral("%1@%2").arg(dataset, ctx.snapshotName.trimmed());
        pane.detailStack->setCurrentWidget(pane.datasetTabs);
        pane.detailTitle->setText(objectName);
        const bool isSnapshot = !ctx.snapshotName.trimmed().isEmpty();
        // Cada pestaña solo para lo que le toca: un snapshot no delega permisos y un
        // dataset no tiene holds. Apagarlas dice por qué está vacía sin tener que abrirla.
        pane.datasetTabs->setTabEnabled(pane.datasetTabs->indexOf(pane.datasetPermsTree), !isSnapshot);
        pane.datasetTabs->setTabEnabled(pane.datasetTabs->indexOf(pane.datasetHoldsTable), isSnapshot);
        if (!pane.datasetTabs->isTabEnabled(pane.datasetTabs->currentIndex())) {
            pane.datasetTabs->setCurrentWidget(pane.datasetDetailTable);
        }
        fillPanePermissions(paneIdx, connIdx, poolName, dataset);
        fillPaneHolds(paneIdx, connIdx, poolName, dataset, ctx.snapshotName.trimmed());
        // De los dos paneles solo uno está vivo a la vez, y es el que se acaba de tocar.
        // El estado del borrador —qué propiedad se ha cambiado, cuál era su valor
        // original, si estaba heredada— es uno solo: `m_propsToken`, `m_propsDataset`,
        // `m_propsOriginalValues`. Apuntar aquí la tabla activa es lo que dice a quién
        // pertenece ese estado. La del otro panel se queda con lo último que enseñó,
        // que sigue siendo cierto, y vuelve a estar viva en cuanto se marque algo en él.
        m_connContentPropsTable = pane.datasetDetailTable;
        refreshConnContentPropertiesFor(tree);
        return;
    }

    if (!poolName.isEmpty() && connIdx >= 0) {
        pane.detailStack->setCurrentWidget(pane.poolDetailTable->parentWidget());
        pane.poolDetailTable->setRowCount(0);
        pane.poolDetailStatus->clear();
        pane.detailTitle->setText(poolName);
        // Sin carga remota desde aquí: el detalle se repinta a cada cambio de selección y
        // pedirlo por red en cada paso del cursor colgaría la interfaz. Lo que haya en
        // caché se enseña; lo que no, lo trae el refresco.
        if (const PoolDetailsCacheEntry* entry = poolDetailsEntry(connIdx, poolName)) {
            for (const QStringList& row : entry->propsRows) {
                if (row.size() < 3) {
                    continue;
                }
                const int r = pane.poolDetailTable->rowCount();
                pane.poolDetailTable->insertRow(r);
                pane.poolDetailTable->setItem(r, 0, new QTableWidgetItem(row.value(0)));
                pane.poolDetailTable->setItem(r, 1, new QTableWidgetItem(row.value(1)));
                pane.poolDetailTable->setItem(r, 2, new QTableWidgetItem(row.value(2)));
            }
            pane.poolDetailStatus->setPlainText(entry->statusText);
        }
        pane.poolDetailTable->resizeColumnToContents(0);
        return;
    }

    pane.detailStack->setCurrentWidget(pane.connDetailTable);
    setDetailRows(pane.connDetailTable, connectionProfileRows(pane.connIdx));
    pane.detailTitle->setText(item->text(0));
}

// ── Permisos ────────────────────────────────────────────────────────────────────
//
// La segunda pestaña del detalle de un dataset. Es un árbol de dos niveles: cada
// delegación —a quién y con qué ámbito— y debajo sus permisos con casilla.
//
// Lo mismo se editaba dentro del árbol principal, con los permisos tumbados en las
// columnas C1...C10: dos filas por bloque, una de nombres y otra de casillas, cortadas
// cada `m_connPropColumnsSetting` columnas. Con veintitantos permisos delegables eso eran
// cinco bloques que había que leer en zigzag. Aquí cada permiso es una fila con su marca.

namespace {
constexpr int kPaneGrantScopeRole = Qt::UserRole + 27;
constexpr int kPaneGrantTargetTypeRole = Qt::UserRole + 28;
constexpr int kPaneGrantTargetNameRole = Qt::UserRole + 29;
constexpr int kPaneGrantNodeRole = Qt::UserRole + 32;
constexpr int kPanePermTokenRole = Qt::UserRole + 30;
}  // namespace

void MainWindow::fillPanePermissions(int paneIdx, int connIdx, const QString& poolName,
                                     const QString& datasetName) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    QTreeWidget* tree = pane.datasetPermsTree;
    if (!tree) {
        return;
    }
    const QSignalBlocker blocker(tree);
    tree->clear();
    // Antes de cualquier salida: de estas tres propiedades depende que abrir la pestaña
    // sepa qué permisos pedir, y el camino que sale antes es justo el de «aún no leídos».
    tree->setProperty("zfsmgr.permsConnIdx", connIdx);
    tree->setProperty("zfsmgr.permsPool", poolName);
    tree->setProperty("zfsmgr.permsDataset", datasetName);
    if (connIdx < 0 || poolName.trimmed().isEmpty() || datasetName.trimmed().isEmpty()
        || datasetName.contains(QLatin1Char('@'))) {
        return;
    }
    // Lo que ya esté en caché. Sin lectura remota: el detalle se repinta a cada cambio de
    // selección, y pedir los permisos por red en cada paso del cursor colgaría la
    // interfaz. Los trae el mismo camino que los traía antes —abrir el nodo, refrescar—.
    const DatasetPermissionsCacheEntry* entry =
        datasetPermissionsEntry(connIdx, poolName, datasetName);
    if (!entry || !entry->loaded) {
        auto* pending = new QTreeWidgetItem(tree);
        pending->setText(0, trk(QStringLiteral("t_perm_not_loaded_001"),
                                QStringLiteral("Permisos sin leer todavía"),
                                QStringLiteral("Permissions not read yet")));
        QFont f = pending->font(0);
        f.setItalic(true);
        pending->setFont(0, f);
        pending->setFlags(pending->flags() & ~Qt::ItemIsSelectable);
        return;
    }

    QVector<DatasetPermissionGrant> allGrants = entry->localGrants;
    allGrants += entry->descendantGrants;
    allGrants += entry->localDescendantGrants;
    if (allGrants.isEmpty()) {
        auto* none = new QTreeWidgetItem(tree);
        none->setText(0, trk(QStringLiteral("t_perm_none_001"),
                             QStringLiteral("Sin delegaciones"),
                             QStringLiteral("No delegations")));
        QFont f = none->font(0);
        f.setItalic(true);
        none->setFont(0, f);
        none->setFlags(none->flags() & ~Qt::ItemIsSelectable);
        return;
    }

    const auto scopeLabel = [this](const QString& scope) {
        const QString s = scope.trimmed().toLower();
        if (s == QStringLiteral("local")) {
            return trk(QStringLiteral("t_perm_scope_local_001"),
                       QStringLiteral("Local"), QStringLiteral("Local"));
        }
        if (s == QStringLiteral("descendant")) {
            return trk(QStringLiteral("t_perm_scope_desc_001"),
                       QStringLiteral("Descendientes"), QStringLiteral("Descendants"));
        }
        return trk(QStringLiteral("t_perm_scope_localdesc_001"),
                   QStringLiteral("Local y descendientes"),
                   QStringLiteral("Local and descendants"));
    };
    const QStringList tokens = availableDelegablePermissions(datasetName, connIdx, poolName);
    for (const DatasetPermissionGrant& grant : std::as_const(allGrants)) {
        QString who = trk(QStringLiteral("t_everyone_001"),
                          QStringLiteral("Everyone"), QStringLiteral("Everyone"));
        if (grant.targetType == QStringLiteral("user")) {
            who = trk(QStringLiteral("t_user_with_name_001"),
                      QStringLiteral("Usuario %1"), QStringLiteral("User %1")).arg(grant.targetName);
        } else if (grant.targetType == QStringLiteral("group")) {
            who = trk(QStringLiteral("t_group_with_name_001"),
                      QStringLiteral("Grupo %1"), QStringLiteral("Group %1")).arg(grant.targetName);
        }
        auto* grantNode = new QTreeWidgetItem(tree);
        grantNode->setText(0, QStringLiteral("%1 — %2").arg(who, scopeLabel(grant.scope)));
        grantNode->setData(0, kPaneGrantNodeRole, true);
        grantNode->setData(0, kPaneGrantScopeRole, grant.scope);
        grantNode->setData(0, kPaneGrantTargetTypeRole, grant.targetType);
        grantNode->setData(0, kPaneGrantTargetNameRole, grant.targetName);
        grantNode->setFlags(grantNode->flags() & ~Qt::ItemIsUserCheckable);
        QFont bold = grantNode->font(0);
        bold.setBold(true);
        grantNode->setFont(0, bold);
        // Una delegación que aún no se ha aplicado se lee en cursiva: existe en el
        // borrador y no en la máquina.
        if (grant.pending) {
            QFont f = grantNode->font(0);
            f.setItalic(true);
            grantNode->setFont(0, f);
        }
        for (const QString& token : tokens) {
            auto* tokenNode = new QTreeWidgetItem(grantNode);
            tokenNode->setText(0, token);
            tokenNode->setData(0, kPanePermTokenRole, token);
            tokenNode->setFlags(tokenNode->flags() | Qt::ItemIsUserCheckable);
            tokenNode->setCheckState(0, grant.permissions.contains(token, Qt::CaseInsensitive)
                                            ? Qt::Checked
                                            : Qt::Unchecked);
        }
        grantNode->setExpanded(false);
    }
}

// Marcar o desmarcar un permiso reescribe la delegación entera, no ese permiso suelto:
// `zfs allow` recibe la lista completa de quien delega, así que la lista de la máquina es
// la que sale de recorrer las casillas.
void MainWindow::commitPanePermissionGrant(int paneIdx, QTreeWidgetItem* tokenNode) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    QTreeWidget* tree = pane.datasetPermsTree;
    if (!tree || !tokenNode) {
        return;
    }
    QTreeWidgetItem* grantNode = tokenNode->parent();
    if (!grantNode || !grantNode->data(0, kPaneGrantNodeRole).toBool()) {
        return;
    }
    const int connIdx = tree->property("zfsmgr.permsConnIdx").toInt();
    const QString poolName = tree->property("zfsmgr.permsPool").toString();
    const QString datasetName = tree->property("zfsmgr.permsDataset").toString();
    if (connIdx < 0 || poolName.isEmpty() || datasetName.isEmpty()) {
        return;
    }
    const QString scope = grantNode->data(0, kPaneGrantScopeRole).toString();
    const QString targetType = grantNode->data(0, kPaneGrantTargetTypeRole).toString();
    const QString targetName = grantNode->data(0, kPaneGrantTargetNameRole).toString();

    QStringList checked;
    for (int i = 0; i < grantNode->childCount(); ++i) {
        QTreeWidgetItem* child = grantNode->child(i);
        if (child && child->checkState(0) == Qt::Checked) {
            checked.push_back(child->data(0, kPanePermTokenRole).toString().trimmed());
        }
    }
    checked.removeAll(QString());
    checked.sort(Qt::CaseInsensitive);

    DatasetPermissionsCacheEntry* entry =
        datasetPermissionsEntryMutable(connIdx, poolName, datasetName);
    if (!entry) {
        return;
    }
    auto updateGrantList = [&](QVector<DatasetPermissionGrant>& grants) -> bool {
        for (DatasetPermissionGrant& g : grants) {
            if (g.scope == scope && g.targetType == targetType && g.targetName == targetName) {
                g.permissions = checked;
                entry->dirty = true;
                return true;
            }
        }
        return false;
    };
    if (!updateGrantList(entry->localGrants)
        && !updateGrantList(entry->descendantGrants)
        && !updateGrantList(entry->localDescendantGrants)) {
        return;
    }
    mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
    updateApplyPropsButtonState();
}

// Los holds de un snapshot. Se leen aquí y no al abrir la pestaña —al revés que los
// permisos— porque `ensureDatasetSnapshotHoldsLoaded()` responde de caché salvo la
// primera vez, y la lista es de dos columnas: no hay nada que ahorrar difiriéndola.
void MainWindow::fillPaneHolds(int paneIdx, int connIdx, const QString& poolName,
                               const QString& datasetName, const QString& snapshotName) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    QTableWidget* table = pane.datasetHoldsTable;
    if (!table) {
        return;
    }
    table->setRowCount(0);
    table->setProperty("zfsmgr.holdsConnIdx", connIdx);
    table->setProperty("zfsmgr.holdsPool", poolName);
    table->setProperty("zfsmgr.holdsDataset", datasetName);
    table->setProperty("zfsmgr.holdsSnapshot", snapshotName);
    if (connIdx < 0 || poolName.trimmed().isEmpty() || datasetName.trimmed().isEmpty()
        || snapshotName.trimmed().isEmpty()) {
        return;
    }
    const QString objectName = QStringLiteral("%1@%2").arg(datasetName, snapshotName);
    if (!ensureDatasetSnapshotHoldsLoaded(connIdx, poolName, objectName)) {
        return;
    }
    for (const auto& hold : datasetSnapshotHolds(connIdx, poolName, objectName)) {
        const int r = table->rowCount();
        table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(hold.first));
        table->setItem(r, 1, new QTableWidgetItem(hold.second));
    }
    table->resizeColumnToContents(0);
}
