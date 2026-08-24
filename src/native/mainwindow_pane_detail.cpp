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
#include "mainwindow_connectiondatasettreedelegate.h"

#include "commands/gsa.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStackedWidget>
#include <QMenu>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

// Los mismos números que en el árbol y en `mainwindow_filebrowser.cpp`: son roles de
// item, y no hay una cabecera común donde vivan.
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kConnContentNodeRole = Qt::UserRole + 19;
constexpr int kConnSnapshotGroupNodeRole = Qt::UserRole + 42;
constexpr int kConnSnapshotItemRole = Qt::UserRole + 43;
constexpr int kConnSnapshotGuidRole = Qt::UserRole + 48;
constexpr int kConnSnapshotGroupIdRole = Qt::UserRole + 49;
constexpr int kConnFileBrowserNodeRole = Qt::UserRole + 53;
constexpr int kConnFileBrowserPathRole = Qt::UserRole + 54;
constexpr int kConnFileBrowserLoadedRole = Qt::UserRole + 56;
// Estos son propios del detalle: describen las delegaciones de la pestaña de permisos.
constexpr int kPaneGrantScopeRole = Qt::UserRole + 27;
constexpr int kPaneGrantTargetTypeRole = Qt::UserRole + 28;
constexpr int kPaneGrantTargetNameRole = Qt::UserRole + 29;
constexpr int kPanePermTokenRole = Qt::UserRole + 30;
constexpr int kPaneGrantNodeRole = Qt::UserRole + 32;


// La raya que separa los dos juegos de columnas de una fila doblada.
//
// Con «Propiedad | Valor | Propiedad | Valor» las cuatro columnas se leen como una sola
// tira de cuatro y hay que contar para saber qué valor es de qué propiedad. La rejilla de
// la tabla pinta todas las separaciones iguales, así que la del medio —la que de verdad
// separa un grupo del siguiente— no se distingue de las de dentro de un grupo.
//
// Aquí se repinta más gruesa y más oscura solo en esa frontera: la última columna de cada
// juego, salvo la del final de la fila, que no separa nada.
class PairGroupBorderDelegate final : public QStyledItemDelegate {
public:
    PairGroupBorderDelegate(int perItem, QObject* parent)
        : QStyledItemDelegate(parent), m_perItem(perItem) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (!painter || !index.isValid() || m_perItem <= 0) {
            return;
        }
        const int col = index.column();
        if ((col + 1) % m_perItem != 0) {
            return;
        }
        if (!index.model() || col + 1 >= index.model()->columnCount()) {
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        const QColor border = option.palette.color(QPalette::Mid).darker(140);
        const QRect r = option.rect;
        painter->fillRect(QRect(r.right() - 1, r.top(), 2, r.height()), border);
        painter->restore();
    }

private:
    int m_perItem{2};
};

// Las tablas del detalle se parecen lo bastante como para que configurarlas a mano una
// por una fuera una invitación a que se separaran sin querer.
//
// `pairs` es cuántas veces se repite el juego de columnas en la MISMA fila: con dos, una
// tabla de «Propiedad | Valor» pasa a ser «Propiedad | Valor | Propiedad | Valor» y
// enseña el doble de datos en la mitad del alto. Un par nombre/valor no llega a los 300
// píxeles y el detalle mide más de 800: la otra mitad era margen derecho.
QTableWidget* makeDetailTable(QWidget* parent, const QStringList& headers, int pairs = 1) {
    QStringList allHeaders;
    for (int p = 0; p < pairs; ++p) {
        allHeaders += headers;
    }
    auto* table = new QTableWidget(0, allHeaders.size(), parent);
    table->setHorizontalHeaderLabels(allHeaders);
    table->verticalHeader()->setVisible(false);
    // Filas compactas. Por omisión Qt las deja a la altura de un botón, que para una línea
    // de texto es casi el doble de lo necesario: en una tabla de treinta propiedades eso
    // son diez filas menos a la vista.
    if (QHeaderView* vheader = table->verticalHeader()) {
        vheader->setDefaultSectionSize(20);
        vheader->setSectionResizeMode(QHeaderView::Fixed);
        vheader->setMinimumSectionSize(16);
    }
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    if (QHeaderView* header = table->horizontalHeader()) {
        // El ancho sobrante se lo quedan las columnas de VALOR, no la última. Con
        // `stretchLastSection` la casilla de «heredada» se llevaba media tabla para
        // enseñar una marca de siete píxeles.
        header->setStretchLastSection(false);
        const int perPair = headers.size();
        for (int col = 0; col < allHeaders.size(); ++col) {
            const bool isValue = (col % perPair) == 1;
            header->setSectionResizeMode(col, isValue ? QHeaderView::Stretch
                                                      : QHeaderView::Interactive);
            if ((col % perPair) == 0) {
                // Ancho para un nombre de propiedad entero. Por omisión Qt da unos 100 px y
                // los nombres largos —`org.fc16.gsa:semanal`, `usedbysnapshots`— salían
                // cortados con puntos suspensivos, que en una tabla de propiedades es
                // justo la columna que no se puede recortar.
                table->setColumnWidth(col, 150);
            }
            if (perPair > 2 && (col % perPair) == 2) {
                table->setColumnWidth(col, 90);
            }
        }
        if (pairs > 1) {
            table->setItemDelegate(new PairGroupBorderDelegate(perPair, table));
        }
    }
    return table;
}

// Rellena la tabla con `pairs` juegos por fila. Los grupos NO se mezclan: cada uno
// empieza en su fila, porque el corte entre el perfil de la conexión y su diagnóstico es
// lo que hace legible la tabla.
void setPairedRows(QTableWidget* table,
                   const QVector<QVector<QStringList>>& groups,
                   int perItem,
                   int pairs) {
    if (!table) {
        return;
    }
    table->setRowCount(0);
    bool first = true;
    for (const QVector<QStringList>& group : groups) {
        if (group.isEmpty()) {
            continue;
        }
        if (!first) {
            table->insertRow(table->rowCount());
        }
        first = false;
        for (int i = 0; i < group.size(); i += pairs) {
            const int r = table->rowCount();
            table->insertRow(r);
            for (int k = 0; k < pairs && (i + k) < group.size(); ++k) {
                const QStringList& item = group.at(i + k);
                for (int c = 0; c < perItem; ++c) {
                    table->setItem(r, k * perItem + c, new QTableWidgetItem(item.value(c)));
                }
            }
        }
    }
    for (int col = 0; col < table->columnCount(); ++col) {
        if (col % perItem != 1) {
            table->resizeColumnToContents(col);
        }
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
    pane.detailTitle->setTextFormat(Qt::RichText);
    pane.detailTitle->setTextInteractionFlags(Qt::TextBrowserInteraction);
    pane.detailTitle->setOpenExternalLinks(false);
    // Los dos primeros tramos del rótulo son enlaces.
    //
    // La conexión, porque no está en el árbol —se elige en el desplegable—, así que al
    // perderse del rótulo no quedaba forma de volver a su ficha.
    //
    // El pool, porque su ficha —propiedades y `zpool status`— NO SE PODÍA ALCANZAR de
    // ninguna manera: el nodo del pool en el árbol y el dataset raíz del pool son el
    // mismo item, de modo que marcarlo siempre traía nombre de dataset y el detalle se
    // iba por la rama del dataset. La página estaba construida y rellenándose para nadie.
    connect(pane.detailTitle, &QLabel::linkActivated, this, [this, paneIdx](const QString& href) {
        if (href != QStringLiteral("conn") && href != QStringLiteral("pool")) {
            return;
        }
        m_datasetPanes[paneIdx].detailForced = href;
        updatePaneDetail(paneIdx);
    });
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

    pane.connDetailTable = makeDetailTable(pane.detailStack, {colProp, colValue}, 2);
    pane.connDetailTable->setObjectName(isOrigin ? QStringLiteral("originConnDetailTable")
                                                : QStringLiteral("destinationConnDetailTable"));
    pane.detailStack->addWidget(pane.connDetailTable);

    auto* poolPage = new QWidget(pane.detailStack);
    auto* poolLayout = new QVBoxLayout(poolPage);
    poolLayout->setContentsMargins(0, 0, 0, 0);
    poolLayout->setSpacing(2);
    pane.poolDetailTable = makeDetailTable(poolPage, {colProp, colValue, colSource}, 2);
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

    // Contenido: el navegador de ficheros del dataset —o del snapshot, por su .zfs—.
    //
    // Era un nodo «Contenido» colgando de cada dataset del árbol, que al desplegarse
    // llenaba de ficheros el mismo sitio donde están los datasets. Un árbol de datasets
    // con directorios dentro deja de poder leerse de un vistazo, que es para lo que sirve.
    // Aquí tiene columnas propias con su rótulo, en vez de las C1...C10 sin nombre.
    pane.datasetContentTree = new QTreeWidget(pane.datasetTabs);
    pane.datasetContentTree->setObjectName(isOrigin ? QStringLiteral("originContentTree")
                                                    : QStringLiteral("destinationContentTree"));
    pane.datasetContentTree->setColumnCount(6);
    pane.datasetContentTree->setHeaderLabels(
        {trk(QStringLiteral("t_fb_col_name_001"), QStringLiteral("Nombre"), QStringLiteral("Name")),
         trk(QStringLiteral("t_fb_perms_001"), QStringLiteral("permisos"), QStringLiteral("permissions")),
         trk(QStringLiteral("t_fb_owner_001"), QStringLiteral("propietario"), QStringLiteral("owner")),
         trk(QStringLiteral("t_fb_group_001"), QStringLiteral("grupo"), QStringLiteral("group")),
         trk(QStringLiteral("t_fb_size_001"), QStringLiteral("tamaño"), QStringLiteral("size")),
         trk(QStringLiteral("t_fb_mtime_001"), QStringLiteral("modificado"), QStringLiteral("modified"))});
    pane.datasetContentTree->setProperty("zfsmgr.fbPropBaseColumn", 1);
    pane.datasetContentTree->setUniformRowHeights(true);
    pane.datasetContentTree->setRootIsDecorated(true);
    if (QHeaderView* header = pane.datasetContentTree->header()) {
        header->setStretchLastSection(false);
        header->setSectionResizeMode(0, QHeaderView::Stretch);
        pane.datasetContentTree->setColumnWidth(1, 90);
        pane.datasetContentTree->setColumnWidth(2, 90);
        pane.datasetContentTree->setColumnWidth(3, 90);
        pane.datasetContentTree->setColumnWidth(4, 80);
        pane.datasetContentTree->setColumnWidth(5, 130);
    }
    // El listado de un directorio se pide al abrirlo, no antes: un dataset puede tener
    // miles de ficheros y nadie los quiere todos por haberlo marcado.
    connect(pane.datasetContentTree, &QTreeWidget::itemExpanded, this,
            [this, paneIdx](QTreeWidgetItem* item) {
                populateFileBrowserNode(m_datasetPanes[paneIdx].datasetContentTree, item);
            });
    pane.datasetTabs->addTab(pane.datasetContentTree,
                             trk(QStringLiteral("t_content_node_001"),
                                 QStringLiteral("Contenido"),
                                 QStringLiteral("Content")));

    // Snapshots: eran el nodo «@» del árbol, con sus grupos —Horarios, Diarios...— y
    // debajo de cada snapshot otro nivel más de nodos. Cuatro niveles de despliegue por
    // encima de un dataset para llegar a una instantánea, en la misma columna donde se
    // navegan los datasets.
    //
    // Los items llevan LOS MISMOS roles que llevaban en el árbol, y por eso el menú
    // contextual de siempre —borrar, revertir, clonar, holds, las seis acciones de
    // transferencia— funciona aquí sin tocarlo: el delegado resuelve conexión, pool y
    // dataset leyendo los roles del item, no mirando de qué árbol viene.
    pane.datasetSnapsTree = new QTreeWidget(pane.datasetTabs);
    pane.datasetSnapsTree->setObjectName(isOrigin ? QStringLiteral("originSnapsTree")
                                                  : QStringLiteral("destinationSnapsTree"));
    // DOS columnas, y la segunda oculta.
    //
    // La 1 no se enseña pero no está vacía: guarda el nombre del snapshot en
    // `Qt::UserRole`, que es de donde lo lee el menú contextual del delegado. Es el mismo
    // reparto que en el árbol, y por eso las acciones funcionan aquí sin tocarlas.
    //
    // Se probó a enseñar además creación, usado y referenciado: esas propiedades se leen
    // de la máquina cuando se marca el snapshot, así que la tabla salía con tres columnas
    // en blanco. Enseñar columnas que casi siempre están vacías es peor que no tenerlas.
    pane.datasetSnapsTree->setColumnCount(2);
    pane.datasetSnapsTree->setHeaderLabels(
        {trk(QStringLiteral("t_snap_col_name_001"), QStringLiteral("Snapshot"), QStringLiteral("Snapshot")),
         QString()});
    pane.datasetSnapsTree->setColumnHidden(1, true);
    pane.datasetSnapsTree->setUniformRowHeights(true);
    if (QHeaderView* header = pane.datasetSnapsTree->header()) {
        header->setStretchLastSection(true);
    }
    pane.datasetSnapsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pane.datasetSnapsTree, &QWidget::customContextMenuRequested, this,
            [this, paneIdx](const QPoint& pos) {
                DatasetPane& p = m_datasetPanes[paneIdx];
                if (!p.datasetSnapsTree || !p.delegate) {
                    return;
                }
                if (QTreeWidgetItem* item = p.datasetSnapsTree->itemAt(pos)) {
                    p.delegate->showGeneralMenu(p.datasetSnapsTree, paneIdx == 1, item, pos);
                }
            });
    // Marcar un snapshot aquí es marcarlo COMO ORIGEN o COMO DESTINO del panel, igual que
    // marcarlo en el árbol cuando estaba dentro. Sin esto, las cinco acciones de
    // transferencia que parten de una instantánea se quedarían sin forma de elegirla.
    connect(pane.datasetSnapsTree, &QTreeWidget::itemSelectionChanged, this,
            [this, paneIdx]() {
                DatasetPane& p = m_datasetPanes[paneIdx];
                if (!p.datasetSnapsTree) {
                    return;
                }
                QTreeWidgetItem* item = p.datasetSnapsTree->currentItem();
                if (!item || !item->data(0, kConnSnapshotItemRole).toBool()) {
                    return;
                }
                p.snapshotSel = item->data(1, Qt::UserRole).toString();
                p.detailForced.clear();
                setSelectedDataset(paneIdx == 0 ? QStringLiteral("origin") : QStringLiteral("dest"),
                                   item->data(0, Qt::UserRole).toString(),
                                   p.snapshotSel);
                // Y que el detalle pase a hablar del snapshot: sus propiedades, su
                // contenido y sus holds, no los del dataset que lo contiene.
                updatePaneDetail(paneIdx);
            });
    pane.datasetTabs->addTab(pane.datasetSnapsTree,
                             trk(QStringLiteral("t_detail_tab_snaps_001"),
                                 QStringLiteral("Snapshots"),
                                 QStringLiteral("Snapshots")));

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

// El camino completo de lo que se está mirando: «Local / tpool / datos@snap».
//
// Antes el rótulo enseñaba solo el último tramo —el nombre del objeto— y el de la
// conexión se perdía en cuanto se marcaba algo. La conexión no está en el árbol, se elige
// en un desplegable, así que al perderse no quedaba forma de volver a su ficha: por eso
// ese tramo va como enlace y los demás como texto.
QString MainWindow::paneDetailPathHtml(int connIdx, const QString& poolName,
                                       const QString& datasetName,
                                       const QString& snapshotName) const {
    const QString connName = (connIdx >= 0 && connIdx < m_conns.profiles.size())
                                 ? m_conns.profiles.at(connIdx).name
                                 : QString();
    QStringList parts;
    parts << QStringLiteral("<a href=\"conn\">%1</a>")
                 .arg(connName.isEmpty() ? QStringLiteral("?") : connName.toHtmlEscaped());
    if (!poolName.trimmed().isEmpty()) {
        parts << QStringLiteral("<a href=\"pool\">%1</a>").arg(poolName.trimmed().toHtmlEscaped());
    }
    // El dataset ya lleva el pool delante («tpool/datos»): se le quita para no repetirlo.
    QString object = datasetName.trimmed();
    const QString prefix = poolName.trimmed() + QLatin1Char('/');
    if (!poolName.trimmed().isEmpty() && object.startsWith(prefix)) {
        object = object.mid(prefix.size());
    } else if (object == poolName.trimmed()) {
        object.clear();
    }
    if (!snapshotName.trimmed().isEmpty()) {
        object = object.isEmpty() ? QStringLiteral("@%1").arg(snapshotName.trimmed())
                                  : QStringLiteral("%1@%2").arg(object, snapshotName.trimmed());
    }
    if (!object.isEmpty()) {
        parts << object.toHtmlEscaped();
    }
    return parts.join(QStringLiteral(" / "));
}

// La ficha de la conexión del panel: su perfil y su diagnóstico.
void MainWindow::showPaneConnectionDetail(int paneIdx, int connIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (!pane.detailStack || !pane.connDetailTable) {
        return;
    }
    pane.detailStack->setCurrentWidget(pane.connDetailTable);
    const auto toItems = [](const QVector<QPair<QString, QString>>& in) {
        QVector<QStringList> out;
        for (const auto& row : in) {
            out.push_back({row.first, row.second});
        }
        return out;
    };
    setPairedRows(pane.connDetailTable,
                  {toItems(connectionProfileRows(connIdx)), toItems(connectionInfoRows(connIdx))},
                  2, 2);
}

// La ficha del pool: sus propiedades y su `zpool status`.
//
// `allowLoad` distingue quién pregunta. Al repintar por un cambio de selección NO se pide
// nada por red: el detalle se rehace a cada movimiento del cursor y eso colgaría la
// interfaz. Pulsar el pool en el rótulo es un gesto deliberado, y ahí sí toca preguntar
// a la máquina; si no, la ficha salía vacía, que es lo que pasaba: la página no la
// alcanzaba nadie, así que sus propiedades no se habían leído nunca.
void MainWindow::showPanePoolDetail(int paneIdx, int connIdx, const QString& poolName,
                                    bool allowLoad) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (!pane.detailStack || !pane.poolDetailTable || !pane.poolDetailStatus) {
        return;
    }
    pane.detailStack->setCurrentWidget(pane.poolDetailTable->parentWidget());
    pane.poolDetailTable->setRowCount(0);
    pane.poolDetailStatus->clear();
    // Las propiedades y el estado viven en DOS sitios y no siempre en los dos: el modelo
    // —`poolInfo->runtime`— y la caché de detalles. `ensurePoolDetailsLoaded()` se da por
    // satisfecho con que haya UNO de los dos cargado, así que un pool con solo el
    // `zpool status` leído se daba por completo y las propiedades no se pedían nunca. Se
    // mira en los dos, y se encarga la lectura si faltan las propiedades.
    QVector<QStringList> rows;
    QString statusText;
    if (const PoolInfo* poolInfo = findPoolInfo(connIdx, poolName)) {
        rows = poolInfo->runtime.zpoolPropertyRows;
        statusText = poolInfo->runtime.poolStatusText;
    }
    if (const PoolDetailsCacheEntry* entry = poolDetailsEntry(connIdx, poolName)) {
        if (rows.isEmpty()) {
            rows = entry->propsRows;
        }
        if (statusText.trimmed().isEmpty()) {
            statusText = entry->statusText;
        }
    }
    if (allowLoad && rows.isEmpty()) {
        schedulePoolDetailsLoad(connIdx, poolName);
    }
    QVector<QStringList> items;
    for (const QStringList& row : std::as_const(rows)) {
        if (row.size() >= 3) {
            items.push_back({row.value(0), row.value(1), row.value(2)});
        }
    }
    setPairedRows(pane.poolDetailTable, {items}, 3, 2);
    pane.poolDetailStatus->setPlainText(statusText);
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
        showPaneConnectionDetail(paneIdx, pane.connIdx);
        pane.detailTitle->setText(paneDetailPathHtml(pane.connIdx, QString(), QString(), QString()));
        return;
    }

    const DatasetSelectionContext ctx = currentConnContentSelection(tree);
    const int connIdx = ctx.valid ? ctx.connIdx : pane.connIdx;
    const QString poolName = ctx.poolName.trimmed();
    const QString dataset = ctx.datasetName.trimmed();
    // El snapshot puede venir de dos sitios: del árbol —ya no, desde que salió de él— o
    // de la pestaña «Snapshots». Lo segundo es lo que hay ahora.
    const QString snapshot = ctx.snapshotName.trimmed().isEmpty() ? pane.snapshotSel
                                                                  : ctx.snapshotName.trimmed();

    if (!dataset.isEmpty() && connIdx >= 0 && !poolName.isEmpty()) {
        const QString objectName = snapshot.isEmpty()
                                       ? dataset
                                       : QStringLiteral("%1@%2").arg(dataset, snapshot);
        pane.detailTitle->setText(paneDetailPathHtml(connIdx, poolName, dataset, snapshot));
        // El tramo pulsado manda sobre lo marcado, pero sin cambiar la marca: el camino
        // se sigue enseñando entero y volver al objeto es marcar cualquier cosa.
        if (pane.detailForced == QStringLiteral("conn")) {
            showPaneConnectionDetail(paneIdx, connIdx);
            return;
        }
        if (pane.detailForced == QStringLiteral("pool")) {
            showPanePoolDetail(paneIdx, connIdx, poolName, true);
            return;
        }
        pane.detailStack->setCurrentWidget(pane.datasetTabs);
        const bool isSnapshot = !snapshot.isEmpty();
        // La pestaña que no aplica NO se enseña, en vez de enseñarse apagada.
        //
        // Un snapshot no delega permisos y un dataset no tiene holds. Estaban las tres
        // siempre, con la que no tocaba en gris; y una pestaña en gris no dice «esto no
        // aplica aquí», dice «esto está roto» —hubo que preguntarlo—. Quitarla y ponerla
        // no cuesta nada: el widget se conserva, solo cambia de sitio.
        const auto showTab = [this, &pane](QWidget* page, bool wanted, int at, const QString& title) {
            if (!page || !pane.datasetTabs) {
                return;
            }
            const int idx = pane.datasetTabs->indexOf(page);
            if (wanted && idx < 0) {
                pane.datasetTabs->insertTab(qMin(at, pane.datasetTabs->count()), page, title);
            } else if (!wanted && idx >= 0) {
                pane.datasetTabs->removeTab(idx);
                // `removeTab` deja el widget SIN padre: sin esto, al destruirse la ventana
                // nadie lo borra.
                page->setParent(pane.datasetTabs);
                page->hide();
            }
        };
        showTab(pane.datasetPermsTree, !isSnapshot, 3,
                trk(QStringLiteral("t_detail_tab_perms_001"),
                    QStringLiteral("Permisos"),
                    QStringLiteral("Permissions")));
        showTab(pane.datasetHoldsTable, isSnapshot, 4,
                trk(QStringLiteral("t_detail_tab_holds_001"),
                    QStringLiteral("Holds"),
                    QStringLiteral("Holds")));
        fillPanePermissions(paneIdx, connIdx, poolName, dataset);
        fillPaneHolds(paneIdx, connIdx, poolName, dataset, snapshot);
        fillPaneContent(paneIdx, connIdx, poolName, dataset, snapshot);
        fillPaneSnapshots(paneIdx, connIdx, poolName, dataset, snapshot);
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
        pane.detailTitle->setText(paneDetailPathHtml(connIdx, poolName, dataset, snapshot));
        showPanePoolDetail(paneIdx, connIdx, poolName, false);
        return;
    }

    showPaneConnectionDetail(paneIdx, pane.connIdx);
    pane.detailTitle->setText(paneDetailPathHtml(connIdx, poolName, dataset, snapshot));
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

// El navegador de ficheros de lo que esté marcado: el punto de montaje del dataset, o el
// directorio del snapshot dentro de `.zfs/snapshot`.
//
// Solo se pone la raíz; los ficheros se piden al abrir cada directorio. Un dataset puede
// tener miles y nadie los quiere todos por haberlo marcado.
void MainWindow::fillPaneContent(int paneIdx, int connIdx, const QString& poolName,
                                 const QString& datasetName, const QString& snapshotName) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    QTreeWidget* tree = pane.datasetContentTree;
    if (!tree) {
        return;
    }
    const QSignalBlocker blocker(tree);
    tree->clear();
    if (connIdx < 0 || poolName.trimmed().isEmpty() || datasetName.trimmed().isEmpty()) {
        return;
    }
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return;
    }
    const QString mountpoint = dsInfo->runtime.properties.value(QStringLiteral("mountpoint")).trimmed();
    const QString mounted = dsInfo->runtime.properties.value(QStringLiteral("mounted")).trimmed();
    QString path = effectiveMountPath(connIdx, poolName, datasetName, mountpoint, mounted);
    const bool isWindows = isWindowsConnection(connIdx);
    if (!snapshotName.trimmed().isEmpty()) {
        // El snapshot se navega por el `.zfs` del dataset, no por un punto de montaje
        // suyo: no lo tiene. Se le quita el separador final al del dataset, salvo si ES
        // la raíz —«/» o «Z:\»—, que sí lo lleva.
        QString base = path;
        while (base.size() > 1
               && (base.endsWith(QLatin1Char('/')) || base.endsWith(QLatin1Char('\\')))) {
            const QString withoutSep = base.left(base.size() - 1);
            if (withoutSep.endsWith(QLatin1Char(':')) || withoutSep.isEmpty()) {
                break;
            }
            base = withoutSep;
        }
        path = isWindows
                   ? (base + QStringLiteral("\\.zfs\\snapshot\\") + snapshotName.trimmed())
                   : (base + QStringLiteral("/.zfs/snapshot/") + snapshotName.trimmed());
    }
    if (path.isEmpty() || path == QStringLiteral("none")
        || (snapshotName.trimmed().isEmpty() && mounted != QStringLiteral("yes"))) {
        auto* note = new QTreeWidgetItem(tree);
        note->setText(0, trk(QStringLiteral("t_fb_not_mounted_001"),
                             QStringLiteral("Sin montar: no hay contenido que enseñar"),
                             QStringLiteral("Not mounted: no content to show")));
        QFont f = note->font(0);
        f.setItalic(true);
        note->setFont(0, f);
        note->setFlags(note->flags() & ~Qt::ItemIsSelectable);
        return;
    }
    auto* root = new QTreeWidgetItem(tree);
    root->setText(0, path);
    root->setData(0, kConnFileBrowserNodeRole, true);
    root->setData(0, kConnFileBrowserPathRole, path);
    root->setData(0, kConnFileBrowserLoadedRole, false);
    root->setData(0, kConnIdxRole, connIdx);
    root->setFlags(root->flags() & ~Qt::ItemIsUserCheckable);
    auto* placeholder = new QTreeWidgetItem(root);
    placeholder->setText(0, QStringLiteral("..."));
    placeholder->setFlags(placeholder->flags() & ~Qt::ItemIsUserCheckable);
}

// Los snapshots del dataset, agrupados por su clase como lo estaban en el árbol.
//
// El reparto en Horarios, Diarios, Semanales... lo decide la capa base —`groupSnapshots()`,
// la misma regla que usa el intérprete—; aquí solo se dibuja. Un snapshot sin clase
// —creado a mano— va suelto, sin grupo.
void MainWindow::fillPaneSnapshots(int paneIdx, int connIdx, const QString& poolName,
                                   const QString& datasetName, const QString& currentSnapshot) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    QTreeWidget* tree = pane.datasetSnapsTree;
    if (!tree) {
        return;
    }
    const QSignalBlocker blocker(tree);
    tree->clear();
    if (connIdx < 0 || poolName.trimmed().isEmpty() || datasetName.trimmed().isEmpty()) {
        return;
    }
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, datasetName);
    if (!dsInfo) {
        return;
    }
    const QStringList snaps = dsInfo->runtime.directSnapshots;
    if (snaps.isEmpty()) {
        auto* none = new QTreeWidgetItem(tree);
        none->setText(0, trk(QStringLiteral("t_snap_none_001"),
                             QStringLiteral("Sin snapshots"),
                             QStringLiteral("No snapshots")));
        QFont f = none->font(0);
        f.setItalic(true);
        none->setFont(0, f);
        none->setFlags(none->flags() & ~Qt::ItemIsSelectable);
        return;
    }

    QTreeWidgetItem* selected = nullptr;
    const auto addSnapshot = [&](QTreeWidgetItem* parent, const QString& snapName) {
        auto* snapItem = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        snapItem->setText(0, snapName);
        // Los mismos roles que en el árbol: es lo que hace que el menú contextual del
        // delegado funcione aquí sin cambiarlo.
        snapItem->setData(0, Qt::UserRole, datasetName);
        snapItem->setData(1, Qt::UserRole, snapName);
        snapItem->setData(0, kConnSnapshotItemRole, true);
        snapItem->setData(0, kConnContentNodeRole, true);
        snapItem->setData(0, kConnIdxRole, connIdx);
        snapItem->setData(0, kPoolNameRole, poolName);
        snapItem->setFlags(snapItem->flags() & ~Qt::ItemIsUserCheckable);
        const QString fullName = QStringLiteral("%1@%2").arg(datasetName, snapName);
        if (const DSInfo* snapInfo = findDsInfo(connIdx, poolName, fullName)) {
            snapItem->setData(0, kConnSnapshotGuidRole,
                              snapInfo->runtime.properties.value(QStringLiteral("guid")).trimmed());
        }
        if (!currentSnapshot.isEmpty() && snapName == currentSnapshot) {
            selected = snapItem;
        }
    };

    std::vector<std::string> nombres;
    nombres.reserve(static_cast<std::size_t>(snaps.size()));
    for (const QString& sn : snaps) {
        nombres.push_back(sn.trimmed().toStdString());
    }
    const auto classLabel = [this](const QString& klass) {
        if (klass == QStringLiteral("hourly")) {
            return trk(QStringLiteral("t_ctx_snap_group_hourly"), QStringLiteral("Horarios"), QStringLiteral("Hourly"));
        }
        if (klass == QStringLiteral("daily")) {
            return trk(QStringLiteral("t_ctx_snap_group_daily"), QStringLiteral("Diarios"), QStringLiteral("Daily"));
        }
        if (klass == QStringLiteral("weekly")) {
            return trk(QStringLiteral("t_ctx_snap_group_weekly"), QStringLiteral("Semanales"), QStringLiteral("Weekly"));
        }
        if (klass == QStringLiteral("monthly")) {
            return trk(QStringLiteral("t_ctx_snap_group_monthly"), QStringLiteral("Mensuales"), QStringLiteral("Monthly"));
        }
        if (klass == QStringLiteral("yearly")) {
            return trk(QStringLiteral("t_ctx_snap_group_yearly"), QStringLiteral("Anuales"), QStringLiteral("Yearly"));
        }
        return klass;
    };
    for (const auto& grupo : zfsmgr::base::gsa::groupSnapshots(nombres)) {
        const QString klass = QString::fromStdString(grupo.first);
        if (klass.isEmpty()) {
            for (const std::string& snapName : grupo.second) {
                addSnapshot(nullptr, QString::fromStdString(snapName));
            }
            continue;
        }
        auto* groupNode = new QTreeWidgetItem(tree);
        groupNode->setText(0, classLabel(klass));
        groupNode->setData(0, kConnContentNodeRole, true);
        groupNode->setData(0, kConnSnapshotGroupNodeRole, true);
        groupNode->setData(0, kConnSnapshotGroupIdRole, klass);
        groupNode->setData(0, kConnIdxRole, connIdx);
        groupNode->setData(0, kPoolNameRole, poolName);
        groupNode->setFlags(groupNode->flags() & ~Qt::ItemIsUserCheckable);
        QFont bold = groupNode->font(0);
        bold.setBold(true);
        groupNode->setFont(0, bold);
        for (const std::string& snapName : grupo.second) {
            addSnapshot(groupNode, QString::fromStdString(snapName));
        }
        groupNode->setExpanded(true);
    }
    if (selected) {
        tree->setCurrentItem(selected);
    }
}
