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

#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStackedWidget>
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
        header->setStretchLastSection(true);
        for (int col = 0; col + 1 < headers.size(); ++col) {
            header->setSectionResizeMode(col, QHeaderView::Interactive);
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

    pane.datasetDetailTable = makeDetailTable(pane.detailStack, {colProp, colValue, colSource});
    pane.datasetDetailTable->setObjectName(isOrigin ? QStringLiteral("originDatasetDetailTable")
                                                    : QStringLiteral("destinationDatasetDetailTable"));
    pane.detailStack->addWidget(pane.datasetDetailTable);

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
        pane.detailStack->setCurrentWidget(pane.datasetDetailTable);
        pane.datasetDetailTable->setRowCount(0);
        const QVector<DatasetPropCacheRow> rows =
            datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
        for (const DatasetPropCacheRow& row : rows) {
            const int r = pane.datasetDetailTable->rowCount();
            pane.datasetDetailTable->insertRow(r);
            pane.datasetDetailTable->setItem(r, 0, new QTableWidgetItem(row.prop));
            pane.datasetDetailTable->setItem(r, 1, new QTableWidgetItem(row.value));
            pane.datasetDetailTable->setItem(r, 2, new QTableWidgetItem(row.source));
        }
        pane.datasetDetailTable->resizeColumnToContents(0);
        pane.detailTitle->setText(objectName);
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
