#include "mainwindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QComboBox>
#include <QTextStream>

namespace {

QString tsNow() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

QString oneLine(const QString& v) {
    QString x = v.simplified();
    return x.left(220);
}

QString parentDatasetName(const QString& dataset) {
    const int slash = dataset.lastIndexOf('/');
    if (slash <= 0) {
        return QString();
    }
    return dataset.left(slash);
}

QString shSingleQuote(const QString& s) {
    QString out = s;
    out.replace('\'', "'\"'\"'");
    return QStringLiteral("'") + out + QStringLiteral("'");
}

} // namespace

MainWindow::MainWindow(const QString& masterPassword, QWidget* parent)
    : QMainWindow(parent)
    , m_store(QStringLiteral("ZFSMgr")) {
    m_store.setMasterPassword(masterPassword);
    initLogPersistence();
    buildUi();
    loadConnections();
    QTimer::singleShot(0, this, [this]() {
        refreshAllConnections();
    });
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("ZFSMgr (C++/Qt)"));
    resize(1500, 920);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    auto* topSplitter = new QSplitter(Qt::Horizontal, central);

    auto* leftPane = new QWidget(topSplitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    m_leftTabs = new QTabWidget(leftPane);

    auto* connectionsTab = new QWidget(m_leftTabs);
    auto* connLayout = new QVBoxLayout(connectionsTab);
    m_connectionsList = new QListWidget(connectionsTab);
    m_connectionsList->setAlternatingRowColors(true);
    connLayout->addWidget(m_connectionsList, 1);

    auto* connButtons = new QHBoxLayout();
    m_btnNew = new QPushButton(QStringLiteral("Nueva"), connectionsTab);
    m_btnRefreshAll = new QPushButton(QStringLiteral("Refrescar todo"), connectionsTab);
    m_btnRefreshSelected = new QPushButton(QStringLiteral("Refrescar"), connectionsTab);
    connButtons->addWidget(m_btnNew);
    connButtons->addWidget(m_btnRefreshSelected);
    connButtons->addWidget(m_btnRefreshAll);
    connButtons->addStretch(1);
    connLayout->addLayout(connButtons);
    connectionsTab->setLayout(connLayout);

    auto* datasetsTab = new QWidget(m_leftTabs);
    auto* dsLeftTabLayout = new QVBoxLayout(datasetsTab);
    auto* transferBox = new QGroupBox(QStringLiteral("Origen-->Destino"), datasetsTab);
    auto* transferLayout = new QVBoxLayout(transferBox);
    m_transferOriginLabel = new QLabel(QStringLiteral("Origen: Dataset (seleccione)"), transferBox);
    m_transferDestLabel = new QLabel(QStringLiteral("Destino: Dataset (seleccione)"), transferBox);
    m_transferOriginLabel->setWordWrap(true);
    m_transferDestLabel->setWordWrap(true);
    m_btnCopy = new QPushButton(QStringLiteral("Copiar"), transferBox);
    m_btnLevel = new QPushButton(QStringLiteral("Nivelar"), transferBox);
    m_btnSync = new QPushButton(QStringLiteral("Sincronizar"), transferBox);
    m_btnCopy->setEnabled(false);
    m_btnLevel->setEnabled(false);
    m_btnSync->setEnabled(false);
    transferLayout->addWidget(m_transferOriginLabel);
    transferLayout->addWidget(m_transferDestLabel);
    transferLayout->addWidget(m_btnCopy);
    transferLayout->addWidget(m_btnLevel);
    transferLayout->addWidget(m_btnSync);
    dsLeftTabLayout->addWidget(transferBox);
    dsLeftTabLayout->addWidget(new QLabel(QStringLiteral("Detalle en panel derecho"), datasetsTab));
    dsLeftTabLayout->addStretch(1);
    datasetsTab->setLayout(dsLeftTabLayout);

    auto* advancedTab = new QWidget(m_leftTabs);
    auto* advLeftTabLayout = new QVBoxLayout(advancedTab);
    m_advSelectionLabel = new QLabel(QStringLiteral("Dataset: (seleccione)"), advancedTab);
    m_advSelectionLabel->setWordWrap(true);
    advLeftTabLayout->addWidget(m_advSelectionLabel);
    advLeftTabLayout->addWidget(new QLabel(QStringLiteral("Detalle en panel derecho"), advancedTab));
    advLeftTabLayout->addStretch(1);
    advancedTab->setLayout(advLeftTabLayout);

    m_leftTabs->addTab(connectionsTab, QStringLiteral("Conexiones"));
    m_leftTabs->addTab(datasetsTab, QStringLiteral("Datasets"));
    m_leftTabs->addTab(advancedTab, QStringLiteral("Avanzado"));
    leftLayout->addWidget(m_leftTabs, 1);

    auto* rightPane = new QWidget(topSplitter);
    auto* rightLayout = new QVBoxLayout(rightPane);
    m_rightStack = new QStackedWidget(rightPane);

    auto* rightConnectionsPage = new QWidget(m_rightStack);
    auto* rightConnectionsLayout = new QVBoxLayout(rightConnectionsPage);
    m_rightTabs = new QTabWidget(rightConnectionsPage);

    auto* importedTab = new QWidget(m_rightTabs);
    auto* importedLayout = new QVBoxLayout(importedTab);
    m_importedPoolsTable = new QTableWidget(importedTab);
    m_importedPoolsTable->setColumnCount(3);
    m_importedPoolsTable->setHorizontalHeaderLabels({QStringLiteral("Conexión"), QStringLiteral("Pool"), QStringLiteral("Acción")});
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importedPoolsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    importedLayout->addWidget(m_importedPoolsTable, 1);

    auto* importableTab = new QWidget(m_rightTabs);
    auto* importableLayout = new QVBoxLayout(importableTab);
    m_importablePoolsTable = new QTableWidget(importableTab);
    m_importablePoolsTable->setColumnCount(5);
    m_importablePoolsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Conexión"), QStringLiteral("Pool"), QStringLiteral("Estado"), QStringLiteral("Motivo"), QStringLiteral("Acción")});
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_importablePoolsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    importableLayout->addWidget(m_importablePoolsTable, 1);

    m_rightTabs->addTab(importedTab, QStringLiteral("Pools importados"));
    m_rightTabs->addTab(importableTab, QStringLiteral("Pools importables"));
    rightConnectionsLayout->addWidget(m_rightTabs, 1);

    auto* rightDatasetsPage = new QWidget(m_rightStack);
    auto* rightDatasetsLayout = new QVBoxLayout(rightDatasetsPage);
    auto* dsSplitter = new QSplitter(Qt::Horizontal, rightDatasetsPage);

    auto* dsLeft = new QWidget(dsSplitter);
    auto* dsLeftLayout = new QVBoxLayout(dsLeft);

    auto* originBox = new QGroupBox(QStringLiteral("Origen"), dsLeft);
    auto* originLayout = new QVBoxLayout(originBox);
    m_originPoolCombo = new QComboBox(originBox);
    m_originPoolCombo->setMinimumContentsLength(24);
    m_originPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_originTree = new QTreeWidget(originBox);
    m_originTree->setColumnCount(2);
    m_originTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot")});
    m_originTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_originTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_originSelectionLabel = new QLabel(QStringLiteral("Dataset: (seleccione)"), originBox);
    originLayout->addWidget(m_originPoolCombo);
    originLayout->addWidget(m_originTree, 1);
    originLayout->addWidget(m_originSelectionLabel);

    auto* destBox = new QGroupBox(QStringLiteral("Destino"), dsLeft);
    auto* destLayout = new QVBoxLayout(destBox);
    m_destPoolCombo = new QComboBox(destBox);
    m_destPoolCombo->setMinimumContentsLength(24);
    m_destPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_destTree = new QTreeWidget(destBox);
    m_destTree->setColumnCount(2);
    m_destTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot")});
    m_destTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_destTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_destSelectionLabel = new QLabel(QStringLiteral("Dataset: (seleccione)"), destBox);
    destLayout->addWidget(m_destPoolCombo);
    destLayout->addWidget(m_destTree, 1);
    destLayout->addWidget(m_destSelectionLabel);

    dsLeftLayout->addWidget(originBox, 1);
    dsLeftLayout->addWidget(destBox, 1);

    auto* propsBox = new QGroupBox(QStringLiteral("Propiedades del dataset"), dsSplitter);
    auto* propsLayout = new QVBoxLayout(propsBox);
    m_datasetPropsTable = new QTableWidget(propsBox);
    m_datasetPropsTable->setColumnCount(2);
    m_datasetPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor")});
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_datasetPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_btnApplyDatasetProps = new QPushButton(QStringLiteral("Aplicar cambios"), propsBox);
    m_btnApplyDatasetProps->setEnabled(false);
    propsLayout->addWidget(m_datasetPropsTable, 1);
    propsLayout->addWidget(m_btnApplyDatasetProps, 0, Qt::AlignRight);

    dsSplitter->addWidget(dsLeft);
    dsSplitter->addWidget(propsBox);
    dsSplitter->setStretchFactor(0, 62);
    dsSplitter->setStretchFactor(1, 38);
    rightDatasetsLayout->addWidget(dsSplitter, 1);

    auto* rightAdvancedPage = new QWidget(m_rightStack);
    auto* rightAdvancedLayout = new QVBoxLayout(rightAdvancedPage);
    m_advPoolCombo = new QComboBox(rightAdvancedPage);
    m_advPoolCombo->setMinimumContentsLength(24);
    m_advPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_advTree = new QTreeWidget(rightAdvancedPage);
    m_advTree->setColumnCount(2);
    m_advTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot")});
    m_advTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_advTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_btnAdvancedBreakdown = new QPushButton(QStringLiteral("Desglosar"), rightAdvancedPage);
    m_btnAdvancedAssemble = new QPushButton(QStringLiteral("Ensamblar"), rightAdvancedPage);
    rightAdvancedLayout->addWidget(m_advPoolCombo);
    rightAdvancedLayout->addWidget(m_advTree, 1);
    rightAdvancedLayout->addWidget(m_btnAdvancedBreakdown);
    rightAdvancedLayout->addWidget(m_btnAdvancedAssemble);

    m_rightStack->addWidget(rightConnectionsPage);
    m_rightStack->addWidget(rightDatasetsPage);
    m_rightStack->addWidget(rightAdvancedPage);
    rightLayout->addWidget(m_rightStack, 1);

    topSplitter->addWidget(leftPane);
    topSplitter->addWidget(rightPane);
    topSplitter->setStretchFactor(0, 35);
    topSplitter->setStretchFactor(1, 65);
    root->addWidget(topSplitter, 4);

    auto* logBox = new QGroupBox(QStringLiteral("Log combinado"), central);
    auto* logLayout = new QVBoxLayout(logBox);
    m_statusLabel = new QLabel(QStringLiteral("Estado: Listo"), logBox);
    m_logView = new QPlainTextEdit(logBox);
    m_logView->setReadOnly(true);
    QFont mono = m_logView->font();
    mono.setFamily(QStringLiteral("Monospace"));
    mono.setPointSize(9);
    m_logView->setFont(mono);
    logLayout->addWidget(m_statusLabel);
    logLayout->addWidget(m_logView, 1);
    root->addWidget(logBox, 2);

    setCentralWidget(central);

    connect(m_btnRefreshAll, &QPushButton::clicked, this, [this]() { refreshAllConnections(); });
    connect(m_btnRefreshSelected, &QPushButton::clicked, this, [this]() { refreshSelectedConnection(); });
    connect(m_connectionsList, &QListWidget::itemSelectionChanged, this, [this]() { onConnectionSelectionChanged(); });
    connect(m_leftTabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx >= 0 && idx < m_rightStack->count()) {
            m_rightStack->setCurrentIndex(idx);
        }
        if (idx == 0) {
            populateAllPoolsTables();
        }
    });
    connect(m_originPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onOriginPoolChanged(); });
    connect(m_destPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onDestPoolChanged(); });
    connect(m_advPoolCombo, &QComboBox::currentIndexChanged, this, [this]() {
        m_originSelectedDataset.clear();
        m_originSelectedSnapshot.clear();
        const QString token = m_advPoolCombo->currentData().toString();
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep <= 0) {
            m_advTree->clear();
            m_advSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
            return;
        }
        const int connIdx = token.left(sep).toInt();
        const QString poolName = token.mid(sep + 2);
        populateDatasetTree(m_advTree, connIdx, poolName, QStringLiteral("origin"));
        m_advSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
    });
    connect(m_originTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onOriginTreeSelectionChanged(); });
    connect(m_destTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onDestTreeSelectionChanged(); });
    connect(m_originTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        onOriginTreeItemDoubleClicked(item, col);
    });
    connect(m_destTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        onDestTreeItemDoubleClicked(item, col);
    });
    m_originTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_destTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_originTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onOriginTreeContextMenuRequested(pos);
    });
    connect(m_destTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onDestTreeContextMenuRequested(pos);
    });
    connect(m_datasetPropsTable, &QTableWidget::cellChanged, this, [this](int row, int col) {
        onDatasetPropsCellChanged(row, col);
    });
    connect(m_btnApplyDatasetProps, &QPushButton::clicked, this, [this]() {
        applyDatasetPropertyChanges();
    });
    connect(m_btnCopy, &QPushButton::clicked, this, [this]() { actionCopySnapshot(); });
    connect(m_btnLevel, &QPushButton::clicked, this, [this]() { actionLevelSnapshot(); });
    connect(m_btnSync, &QPushButton::clicked, this, [this]() { actionSyncDatasets(); });
    connect(m_advTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        const auto selected = m_advTree->selectedItems();
        if (selected.isEmpty()) {
            m_advSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
            return;
        }
        auto* it = selected.first();
        const QString ds = it->data(0, Qt::UserRole).toString();
        const QString snap = it->data(1, Qt::UserRole).toString();
        if (!ds.isEmpty() && !snap.isEmpty()) {
            m_advSelectionLabel->setText(QStringLiteral("Snapshot: %1@%2").arg(ds, snap));
        } else if (!ds.isEmpty()) {
            m_advSelectionLabel->setText(QStringLiteral("Dataset: %1").arg(ds));
        } else {
            m_advSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
        }
    });
    connect(m_advTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        if (!item || col != 1) {
            return;
        }
        const QString ds = item->data(0, Qt::UserRole).toString();
        if (ds.isEmpty()) {
            return;
        }
        const QString token = m_advPoolCombo->currentData().toString();
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep <= 0) {
            return;
        }
        const int connIdx = token.left(sep).toInt();
        const QString poolName = token.mid(sep + 2);
        const QString key = datasetCacheKey(connIdx, poolName);
        const auto it = m_poolDatasetCache.constFind(key);
        if (it == m_poolDatasetCache.constEnd()) {
            return;
        }
        QStringList options;
        options << QStringLiteral("(seleccione)");
        options += it.value().snapshotsByDataset.value(ds);
        if (options.size() <= 1) {
            return;
        }
        bool ok = false;
        const QString chosen = QInputDialog::getItem(this,
                                                     QStringLiteral("Snapshot avanzado"),
                                                     QStringLiteral("Seleccione snapshot"),
                                                     options,
                                                     0,
                                                     false,
                                                     &ok);
        if (!ok) {
            return;
        }
        if (chosen == QStringLiteral("(seleccione)")) {
            item->setText(1, QStringLiteral("(seleccione)"));
            item->setData(1, Qt::UserRole, QString());
        } else {
            item->setText(1, chosen);
            item->setData(1, Qt::UserRole, chosen);
        }
    });
    connect(m_btnAdvancedBreakdown, &QPushButton::clicked, this, [this]() { actionAdvancedBreakdown(); });
    connect(m_btnAdvancedAssemble, &QPushButton::clicked, this, [this]() { actionAdvancedAssemble(); });
}

void MainWindow::loadConnections() {
    const LoadResult loaded = m_store.loadConnections();
    m_profiles = loaded.profiles;
    m_states.clear();
    m_states.resize(m_profiles.size());

    rebuildConnectionList();
    updateStatus(QStringLiteral("Estado: %1 conexiones cargadas").arg(m_profiles.size()));
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }
    rebuildDatasetPoolSelectors();
}

void MainWindow::rebuildConnectionList() {
    m_connectionsList->clear();
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        const auto& s = m_states[i];

        const QString line1 = QStringLiteral("%1/%2").arg(p.name, p.connType);
        QString line2 = QStringLiteral("%1").arg(p.osType);
        if (!s.status.isEmpty()) {
            line2 += QStringLiteral("  [") + s.status + QStringLiteral("]");
        }

        auto* item = new QListWidgetItem(line1 + "\n" + line2, m_connectionsList);
        item->setData(Qt::UserRole, i);
        item->setToolTip(QStringLiteral("Host: %1\nPort: %2\nEstado: %3\nDetalle: %4")
                             .arg(p.host)
                             .arg(p.port)
                             .arg(s.status)
                             .arg(s.detail));
    }
}

void MainWindow::rebuildDatasetPoolSelectors() {
    m_originPoolCombo->blockSignals(true);
    m_destPoolCombo->blockSignals(true);
    m_advPoolCombo->blockSignals(true);

    const QString originPrev = m_originPoolCombo->currentData().toString();
    const QString destPrev = m_destPoolCombo->currentData().toString();
    const QString advPrev = m_advPoolCombo->currentData().toString();

    m_originPoolCombo->clear();
    m_destPoolCombo->clear();
    m_advPoolCombo->clear();

    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& p : st.importedPools) {
            if (p.pool.isEmpty() || p.pool == QStringLiteral("Sin pools")) {
                continue;
            }
            const QString token = QStringLiteral("%1::%2").arg(i).arg(p.pool);
            const QString label = QStringLiteral("%1::%2").arg(m_profiles[i].name, p.pool);
            m_originPoolCombo->addItem(label, token);
            m_destPoolCombo->addItem(label, token);
            m_advPoolCombo->addItem(label, token);
        }
    }

    auto restoreCurrent = [](QComboBox* combo, const QString& token) {
        if (combo->count() <= 0) {
            return;
        }
        int idx = combo->findData(token);
        if (idx < 0) {
            idx = 0;
        }
        combo->setCurrentIndex(idx);
    };
    restoreCurrent(m_originPoolCombo, originPrev);
    restoreCurrent(m_destPoolCombo, destPrev);
    restoreCurrent(m_advPoolCombo, advPrev);

    m_originPoolCombo->blockSignals(false);
    m_destPoolCombo->blockSignals(false);
    m_advPoolCombo->blockSignals(false);
    onOriginPoolChanged();
    onDestPoolChanged();
}

void MainWindow::refreshAllConnections() {
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refrescar todas las conexiones"));
    for (int i = 0; i < m_profiles.size(); ++i) {
        m_states[i] = refreshConnection(m_profiles[i]);
    }
    rebuildConnectionList();
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
    updateStatus(QStringLiteral("Estado: refresco finalizado"));
}

void MainWindow::refreshSelectedConnection() {
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const int idx = selected.first()->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    m_states[idx] = refreshConnection(m_profiles[idx]);
    rebuildConnectionList();
    rebuildDatasetPoolSelectors();
    if (idx < m_connectionsList->count()) {
        m_connectionsList->setCurrentRow(idx);
    }
    populateAllPoolsTables();
}

void MainWindow::onConnectionSelectionChanged() {
    if (m_leftTabs->currentIndex() == 0) {
        populateAllPoolsTables();
    }
}

void MainWindow::onOriginPoolChanged() {
    m_originSelectedDataset.clear();
    m_originSelectedSnapshot.clear();
    const QString token = m_originPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_originTree->clear();
        m_originSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_originTree, connIdx, poolName, QStringLiteral("origin"));
    refreshDatasetProperties(QStringLiteral("origin"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onDestPoolChanged() {
    m_destSelectedDataset.clear();
    m_destSelectedSnapshot.clear();
    const QString token = m_destPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_destTree->clear();
        m_destSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_destTree, connIdx, poolName, QStringLiteral("dest"));
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onOriginTreeSelectionChanged() {
    const auto selected = m_originTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("origin"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("origin"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onDestTreeSelectionChanged() {
    const auto selected = m_destTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("dest"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("dest"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onOriginTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    if (!item || col != 1) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_originPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const QString key = datasetCacheKey(connIdx, poolName);
    const auto it = m_poolDatasetCache.constFind(key);
    if (it == m_poolDatasetCache.constEnd()) {
        return;
    }
    QStringList options;
    options << QStringLiteral("(seleccione)");
    options += it.value().snapshotsByDataset.value(ds);
    if (options.size() <= 1) {
        return;
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this,
                                                 QStringLiteral("Snapshot origen"),
                                                 QStringLiteral("Seleccione snapshot"),
                                                 options,
                                                 0,
                                                 false,
                                                 &ok);
    if (!ok) {
        return;
    }
    if (chosen == QStringLiteral("(seleccione)")) {
        item->setText(1, QStringLiteral("(seleccione)"));
        item->setData(1, Qt::UserRole, QString());
        setSelectedDataset(QStringLiteral("origin"), ds, QString());
    } else {
        item->setText(1, chosen);
        item->setData(1, Qt::UserRole, chosen);
        setSelectedDataset(QStringLiteral("origin"), ds, chosen);
    }
}

void MainWindow::onDestTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    if (!item || col != 1) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_destPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const QString key = datasetCacheKey(connIdx, poolName);
    const auto it = m_poolDatasetCache.constFind(key);
    if (it == m_poolDatasetCache.constEnd()) {
        return;
    }
    QStringList options;
    options << QStringLiteral("(seleccione)");
    options += it.value().snapshotsByDataset.value(ds);
    if (options.size() <= 1) {
        return;
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this,
                                                 QStringLiteral("Snapshot destino"),
                                                 QStringLiteral("Seleccione snapshot"),
                                                 options,
                                                 0,
                                                 false,
                                                 &ok);
    if (!ok) {
        return;
    }
    if (chosen == QStringLiteral("(seleccione)")) {
        item->setText(1, QStringLiteral("(seleccione)"));
        item->setData(1, Qt::UserRole, QString());
        setSelectedDataset(QStringLiteral("dest"), ds, QString());
    } else {
        item->setText(1, chosen);
        item->setData(1, Qt::UserRole, chosen);
        setSelectedDataset(QStringLiteral("dest"), ds, chosen);
    }
}

void MainWindow::onOriginTreeContextMenuRequested(const QPoint& pos) {
    showDatasetContextMenu(QStringLiteral("origin"), m_originTree, pos);
}

void MainWindow::onDestTreeContextMenuRequested(const QPoint& pos) {
    showDatasetContextMenu(QStringLiteral("dest"), m_destTree, pos);
}

bool MainWindow::runSsh(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs, QString& out, QString& err, int& rc) {
    out.clear();
    err.clear();
    rc = -1;

    QStringList args;
    args << "-o" << "BatchMode=yes";
    args << "-o" << "ConnectTimeout=10";
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
    if (p.port > 0) {
        args << "-p" << QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        args << "-i" << p.keyPath;
    }
    args << QStringLiteral("%1@%2").arg(p.username, p.host);
    args << remoteCmd;

    QProcess proc;
    proc.start(QStringLiteral("ssh"), args);
    if (!proc.waitForStarted(4000)) {
        err = QStringLiteral("No se pudo iniciar ssh");
        return false;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        err = QStringLiteral("Timeout");
        return false;
    }

    rc = proc.exitCode();
    out = QString::fromUtf8(proc.readAllStandardOutput());
    err = QString::fromUtf8(proc.readAllStandardError());
    return true;
}

bool MainWindow::getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut) {
    valueOut.clear();
    if (connIdx < 0 || connIdx >= m_profiles.size() || dataset.isEmpty() || prop.isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd =
        QStringLiteral("zfs get -H -o value %1 %2").arg(shSingleQuote(prop), shSingleQuote(dataset));
    if (p.useSudo) {
        cmd = QStringLiteral("sudo -n ") + cmd;
    }
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 15000, out, err, rc) || rc != 0) {
        return false;
    }
    valueOut = out.trimmed();
    return true;
}

QString MainWindow::datasetCacheKey(int connIdx, const QString& poolName) const {
    return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
}

bool MainWindow::ensureDatasetsLoaded(int connIdx, const QString& poolName) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    PoolDatasetCache& cache = m_poolDatasetCache[key];
    if (cache.loaded) {
        return true;
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd = QStringLiteral(
        "zfs list -H -p -t filesystem,volume,snapshot "
        "-o name,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r %1")
                      .arg(poolName);
    if (p.useSudo) {
        cmd = QStringLiteral("sudo -n ") + cmd;
    }

    QString out;
    QString err;
    int rc = -1;
    appLog(QStringLiteral("INFO"), QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));
    if (!runSsh(p, cmd, 35000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("WARN"), QStringLiteral("Failed datasets %1::%2 -> %3")
                                        .arg(p.name, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        return false;
    }

    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList f = line.split('\t');
        if (f.size() < 9) {
            continue;
        }
        const QString name = f[0].trimmed();
        if (name.isEmpty()) {
            continue;
        }
        DatasetRecord rec{name, f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]};
        if (name.contains('@')) {
            const QString ds = name.section('@', 0, 0);
            cache.snapshotsByDataset[ds].push_back(name.section('@', 1));
        } else {
            cache.datasets.push_back(rec);
            cache.recordByName[name] = rec;
        }
    }
    cache.loaded = true;
    appLog(QStringLiteral("DEBUG"), QStringLiteral("Datasets loaded %1::%2 (%3)")
                                     .arg(p.name)
                                     .arg(poolName)
                                     .arg(cache.datasets.size()));
    return true;
}

void MainWindow::populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side) {
    tree->clear();
    if (!ensureDatasetsLoaded(connIdx, poolName)) {
        return;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    const PoolDatasetCache& cache = m_poolDatasetCache[key];

    QMap<QString, QTreeWidgetItem*> byName;
    for (const DatasetRecord& rec : cache.datasets) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, rec.name);
        const QStringList snaps = cache.snapshotsByDataset.value(rec.name);
        if (!snaps.isEmpty()) {
            item->setText(1, snaps.first());
            item->setData(1, Qt::UserRole, snaps.first());
        } else {
            item->setText(1, QStringLiteral("(seleccione)"));
            item->setData(1, Qt::UserRole, QString());
        }
        item->setData(0, Qt::UserRole, rec.name);
        byName.insert(rec.name, item);
    }

    for (const DatasetRecord& rec : cache.datasets) {
        QTreeWidgetItem* item = byName.value(rec.name, nullptr);
        if (!item) {
            continue;
        }
        const QString parent = parentDatasetName(rec.name);
        QTreeWidgetItem* parentItem = byName.value(parent, nullptr);
        if (parentItem) {
            parentItem->addChild(item);
        } else {
            tree->addTopLevelItem(item);
        }
    }
    tree->expandToDepth(1);

    if (side == QStringLiteral("origin")) {
        m_originSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
    } else {
        m_destSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
    }
}

void MainWindow::refreshDatasetProperties(const QString& side) {
    const QString dataset = (side == QStringLiteral("origin")) ? m_originSelectedDataset : m_destSelectedDataset;
    if (dataset.isEmpty()) {
        m_datasetPropsTable->setRowCount(0);
        m_propsDataset.clear();
        m_propsSide = side;
        m_propsOriginalValues.clear();
        m_propsDirty = false;
        updateApplyPropsButtonState();
        return;
    }

    const QString token = (side == QStringLiteral("origin")) ? m_originPoolCombo->currentData().toString()
                                                              : m_destPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const QString key = datasetCacheKey(connIdx, poolName);
    const auto it = m_poolDatasetCache.constFind(key);
    if (it == m_poolDatasetCache.constEnd()) {
        return;
    }
    const PoolDatasetCache& cache = it.value();
    const auto recIt = cache.recordByName.constFind(dataset);
    if (recIt == cache.recordByName.constEnd()) {
        return;
    }
    const DatasetRecord& rec = recIt.value();

    struct Row {
        QString k;
        QString v;
    };
    const QVector<Row> rows = {
        {QStringLiteral("dataset"), rec.name},
        {QStringLiteral("mounted"), rec.mounted},
        {QStringLiteral("mountpoint"), rec.mountpoint},
        {QStringLiteral("used"), rec.used},
        {QStringLiteral("compressratio"), rec.compressRatio},
        {QStringLiteral("encryption"), rec.encryption},
        {QStringLiteral("creation"), rec.creation},
        {QStringLiteral("referenced"), rec.referenced},
        {QStringLiteral("canmount"), rec.canmount},
    };

    m_loadingPropsTable = true;
    m_datasetPropsTable->setRowCount(0);
    m_propsOriginalValues.clear();
    m_propsSide = side;
    m_propsDataset = rec.name;
    for (const Row& row : rows) {
        const int r = m_datasetPropsTable->rowCount();
        m_datasetPropsTable->insertRow(r);
        m_datasetPropsTable->setItem(r, 0, new QTableWidgetItem(row.k));
        auto* v = new QTableWidgetItem(row.v);
        if (row.k == QStringLiteral("dataset")) {
            v->setFlags(v->flags() & ~Qt::ItemIsEditable);
        }
        m_datasetPropsTable->setItem(r, 1, v);
        m_propsOriginalValues[row.k] = row.v;
    }
    m_propsDirty = false;
    m_loadingPropsTable = false;
    updateApplyPropsButtonState();
}

void MainWindow::setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName) {
    if (side == QStringLiteral("origin")) {
        m_originSelectedDataset = datasetName;
        m_originSelectedSnapshot = snapshotName;
        if (datasetName.isEmpty()) {
            m_originSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
        } else if (snapshotName.isEmpty()) {
            m_originSelectionLabel->setText(QStringLiteral("Dataset: %1").arg(datasetName));
        } else {
            m_originSelectionLabel->setText(QStringLiteral("Snapshot: %1@%2").arg(datasetName, snapshotName));
        }
        refreshDatasetProperties(QStringLiteral("origin"));
        refreshTransferSelectionLabels();
        updateTransferButtonsState();
        return;
    }
    m_destSelectedDataset = datasetName;
    m_destSelectedSnapshot = snapshotName;
    if (datasetName.isEmpty()) {
        m_destSelectionLabel->setText(QStringLiteral("Dataset: (seleccione)"));
    } else if (snapshotName.isEmpty()) {
        m_destSelectionLabel->setText(QStringLiteral("Dataset: %1").arg(datasetName));
    } else {
        m_destSelectionLabel->setText(QStringLiteral("Snapshot: %1@%2").arg(datasetName, snapshotName));
    }
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::refreshTransferSelectionLabels() {
    if (!m_originSelectedDataset.isEmpty()) {
        if (!m_originSelectedSnapshot.isEmpty()) {
            m_transferOriginLabel->setText(QStringLiteral("Origen: Snapshot %1@%2").arg(m_originSelectedDataset, m_originSelectedSnapshot));
        } else {
            m_transferOriginLabel->setText(QStringLiteral("Origen: Dataset %1").arg(m_originSelectedDataset));
        }
    } else {
        m_transferOriginLabel->setText(QStringLiteral("Origen: Dataset (seleccione)"));
    }

    if (!m_destSelectedDataset.isEmpty()) {
        if (!m_destSelectedSnapshot.isEmpty()) {
            m_transferDestLabel->setText(QStringLiteral("Destino: Snapshot %1@%2").arg(m_destSelectedDataset, m_destSelectedSnapshot));
        } else {
            m_transferDestLabel->setText(QStringLiteral("Destino: Dataset %1").arg(m_destSelectedDataset));
        }
    } else {
        m_transferDestLabel->setText(QStringLiteral("Destino: Dataset (seleccione)"));
    }
}

void MainWindow::updateTransferButtonsState() {
    const bool srcDs = !m_originSelectedDataset.isEmpty();
    const bool srcSnap = !m_originSelectedSnapshot.isEmpty();
    const bool dstDs = !m_destSelectedDataset.isEmpty();
    const bool dstSnap = !m_destSelectedSnapshot.isEmpty();
    m_btnCopy->setEnabled(srcDs && srcSnap && dstDs && !dstSnap);
    m_btnLevel->setEnabled(srcDs && srcSnap && dstDs && !dstSnap);
    m_btnSync->setEnabled(srcDs && !srcSnap && dstDs && !dstSnap);
}

bool MainWindow::runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs) {
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1").arg(displayLabel));
    appLog(QStringLiteral("INFO"), QStringLiteral("$ %1").arg(command));
    QProcess proc;
    proc.start(QStringLiteral("sh"), QStringList{QStringLiteral("-lc"), command});
    if (!proc.waitForStarted(4000)) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("No se pudo iniciar comando local"));
        return false;
    }
    if (timeoutMs > 0) {
        if (!proc.waitForFinished(timeoutMs)) {
            proc.kill();
            proc.waitForFinished(1000);
            appLog(QStringLiteral("NORMAL"), QStringLiteral("Timeout en comando local"));
            return false;
        }
    } else {
        proc.waitForFinished(-1);
    }
    const int rc = proc.exitCode();
    const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    if (!out.isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(err));
    }
    if (rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizó con error %1").arg(rc));
        return false;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizado correctamente"));
    return true;
}

void MainWindow::actionCopySnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const QString srcSnap = src.datasetName + QStringLiteral("@") + src.snapshotName;
    const QString recvTarget = dst.datasetName + QStringLiteral("/") + src.datasetName.section('/', -1);

    QString srcSsh = QStringLiteral("ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (sp.port > 0) {
        srcSsh += QStringLiteral(" -p ") + QString::number(sp.port);
    }
    if (!sp.keyPath.isEmpty()) {
        srcSsh += QStringLiteral(" -i ") + shSingleQuote(sp.keyPath);
    }
    srcSsh += QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    QString dstSsh = QStringLiteral("ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (dp.port > 0) {
        dstSsh += QStringLiteral(" -p ") + QString::number(dp.port);
    }
    if (!dp.keyPath.isEmpty()) {
        dstSsh += QStringLiteral(" -i ") + shSingleQuote(dp.keyPath);
    }
    dstSsh += QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    QString sendCmd = QStringLiteral("zfs send -wLec %1").arg(shSingleQuote(srcSnap));
    if (sp.useSudo) {
        sendCmd = QStringLiteral("sudo -n ") + sendCmd;
    }
    QString recvCmd = QStringLiteral("zfs recv -F %1").arg(shSingleQuote(recvTarget));
    if (dp.useSudo) {
        recvCmd = QStringLiteral("sudo -n ") + recvCmd;
    }

    const QString pipeline =
        srcSsh + QStringLiteral(" ") + shSingleQuote(sendCmd)
        + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab) || cat) | ")
        + dstSsh + QStringLiteral(" ") + shSingleQuote(recvCmd);

    if (runLocalCommand(QStringLiteral("Copiar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionLevelSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const QString srcSnap = src.datasetName + QStringLiteral("@") + src.snapshotName;
    const QString recvTarget = dst.datasetName;

    QString srcSsh = QStringLiteral("ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (sp.port > 0) {
        srcSsh += QStringLiteral(" -p ") + QString::number(sp.port);
    }
    if (!sp.keyPath.isEmpty()) {
        srcSsh += QStringLiteral(" -i ") + shSingleQuote(sp.keyPath);
    }
    srcSsh += QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    QString dstSsh = QStringLiteral("ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (dp.port > 0) {
        dstSsh += QStringLiteral(" -p ") + QString::number(dp.port);
    }
    if (!dp.keyPath.isEmpty()) {
        dstSsh += QStringLiteral(" -i ") + shSingleQuote(dp.keyPath);
    }
    dstSsh += QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    QString sendCmd = QStringLiteral("zfs send -wLecR %1").arg(shSingleQuote(srcSnap));
    if (sp.useSudo) {
        sendCmd = QStringLiteral("sudo -n ") + sendCmd;
    }
    QString recvCmd = QStringLiteral("zfs recv -F %1").arg(shSingleQuote(recvTarget));
    if (dp.useSudo) {
        recvCmd = QStringLiteral("sudo -n ") + recvCmd;
    }

    const QString pipeline =
        srcSsh + QStringLiteral(" ") + shSingleQuote(sendCmd)
        + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab) || cat) | ")
        + dstSsh + QStringLiteral(" ") + shSingleQuote(recvCmd);

    if (runLocalCommand(QStringLiteral("Nivelar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionSyncDatasets() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];

    QString srcMp;
    QString dstMp;
    QString srcMounted;
    QString dstMounted;
    if (!getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mountpoint"), srcMp)
        || !getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mounted"), srcMounted)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mountpoint"), dstMp)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mounted"), dstMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudieron leer mountpoints para sincronizar."));
        return;
    }
    if (srcMounted != QStringLiteral("yes") || dstMounted != QStringLiteral("yes")) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             QStringLiteral("Origen y destino deben estar montados para sincronizar.\nOrigen=%1 Destino=%2").arg(srcMounted, dstMounted));
        return;
    }

    QString srcSsh = QStringLiteral("ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (sp.port > 0) {
        srcSsh += QStringLiteral(" -p ") + QString::number(sp.port);
    }
    if (!sp.keyPath.isEmpty()) {
        srcSsh += QStringLiteral(" -i ") + shSingleQuote(sp.keyPath);
    }
    srcSsh += QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    QString dstSsh = QStringLiteral("ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (dp.port > 0) {
        dstSsh += QStringLiteral(" -p ") + QString::number(dp.port);
    }
    if (!dp.keyPath.isEmpty()) {
        dstSsh += QStringLiteral(" -i ") + shSingleQuote(dp.keyPath);
    }
    dstSsh += QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    QString remoteRsync =
        QStringLiteral("rsync -aHAWXS --delete --info=progress2 -e ")
        + shSingleQuote(QStringLiteral("ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null")
                        + (dp.port > 0 ? QStringLiteral(" -p ") + QString::number(dp.port) : QString())
                        + (!dp.keyPath.isEmpty() ? QStringLiteral(" -i ") + dp.keyPath : QString()))
        + QStringLiteral(" %1/ %2:%3/")
              .arg(shSingleQuote(srcMp),
                   shSingleQuote(dp.username + QStringLiteral("@") + dp.host),
                   shSingleQuote(dstMp));
    if (sp.useSudo) {
        remoteRsync = QStringLiteral("sudo -n ") + remoteRsync;
    }
    const QString command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
    if (runLocalCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command, 0)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionAdvancedBreakdown() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset en Avanzado."));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const QString cmd =
        QStringLiteral("set -e; DATASET=%1; MP=$(zfs get -H -o value mountpoint \"$DATASET\"); "
                       "[ \"$MP\" = \"none\" ] && { echo \"mountpoint=none\"; exit 2; }; "
                       "for d in \"$MP\"/*; do [ -d \"$d\" ] || continue; bn=$(basename \"$d\"); "
                       "zfs list -H -o name \"$DATASET/$bn\" >/dev/null 2>&1 || "
                       "{ zfs create \"$DATASET/$bn\"; rsync -aHAWXS --remove-source-files \"$d\"/ \"$MP/$bn\"/; }; done")
            .arg(shSingleQuote(ds));
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Desglosar"), ctx, cmd, 0)) {
        m_advSelectionLabel->setText(QStringLiteral("Dataset: %1").arg(ds));
    }
}

void MainWindow::actionAdvancedAssemble() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset en Avanzado."));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const QString cmd =
        QStringLiteral("set -e; DATASET=%1; MP=$(zfs get -H -o value mountpoint \"$DATASET\"); "
                       "[ \"$MP\" = \"none\" ] && { echo \"mountpoint=none\"; exit 2; }; "
                       "for child in $(zfs list -H -o name -r \"$DATASET\" | tail -n +2); do bn=${child##*/}; "
                       "CMP=$(zfs get -H -o value mountpoint \"$child\"); [ \"$CMP\" = \"none\" ] && continue; "
                       "mkdir -p \"$MP/$bn\"; rsync -aHAWXS \"$CMP\"/ \"$MP/$bn\"/; zfs destroy -r \"$child\"; done")
            .arg(shSingleQuote(ds));
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Ensamblar"), ctx, cmd, 0)) {
        m_advSelectionLabel->setText(QStringLiteral("Dataset: %1").arg(ds));
    }
}

void MainWindow::onDatasetPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || col != 1) {
        return;
    }
    QTableWidgetItem* pk = m_datasetPropsTable->item(row, 0);
    QTableWidgetItem* pv = m_datasetPropsTable->item(row, 1);
    if (!pk || !pv) {
        return;
    }
    const QString key = pk->text().trimmed();
    const QString value = pv->text();
    const QString orig = m_propsOriginalValues.value(key);
    if (value != orig) {
        m_propsDirty = true;
    } else {
        m_propsDirty = false;
        for (int r = 0; r < m_datasetPropsTable->rowCount(); ++r) {
            QTableWidgetItem* rk = m_datasetPropsTable->item(r, 0);
            QTableWidgetItem* rv = m_datasetPropsTable->item(r, 1);
            if (!rk || !rv) {
                continue;
            }
            if (rv->text() != m_propsOriginalValues.value(rk->text().trimmed())) {
                m_propsDirty = true;
                break;
            }
        }
    }
    updateApplyPropsButtonState();
}

void MainWindow::applyDatasetPropertyChanges() {
    if (!m_propsDirty || m_propsDataset.isEmpty() || m_propsSide.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    if (!ctx.valid || ctx.datasetName != m_propsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset activo para aplicar cambios."));
        return;
    }

    QStringList subcmds;
    for (int r = 0; r < m_datasetPropsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = m_datasetPropsTable->item(r, 0);
        QTableWidgetItem* pv = m_datasetPropsTable->item(r, 1);
        if (!pk || !pv) {
            continue;
        }
        const QString prop = pk->text().trimmed();
        if (prop.isEmpty() || prop == QStringLiteral("dataset")) {
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_propsOriginalValues.value(prop).trimmed();
        if (now == old) {
            continue;
        }
        if (now.compare(QStringLiteral("inherit"), Qt::CaseInsensitive) == 0
            || now.compare(QStringLiteral("(inherit)"), Qt::CaseInsensitive) == 0) {
            subcmds << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(prop), shSingleQuote(ctx.datasetName));
        } else {
            const QString assign = prop + QStringLiteral("=") + now;
            subcmds << QStringLiteral("zfs set %1 %2").arg(shSingleQuote(assign), shSingleQuote(ctx.datasetName));
        }
    }
    if (subcmds.isEmpty()) {
        m_propsDirty = false;
        updateApplyPropsButtonState();
        return;
    }

    const QString cmd = QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
    if (executeDatasetAction(m_propsSide, QStringLiteral("Aplicar propiedades"), ctx, cmd, 60000)) {
        m_propsDirty = false;
        updateApplyPropsButtonState();
    }
}

void MainWindow::updateApplyPropsButtonState() {
    const DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    const bool eligible = ctx.valid && ctx.snapshotName.isEmpty() && (ctx.datasetName == m_propsDataset);
    m_btnApplyDatasetProps->setEnabled(m_propsDirty && eligible);
}

void MainWindow::initLogPersistence() {
    const QString dir = m_store.configDir();
    if (dir.isEmpty()) {
        return;
    }
    m_appLogPath = dir + "/application.log";
    rotateLogIfNeeded();
}

void MainWindow::rotateLogIfNeeded() {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    constexpr qint64 maxBytes = 2 * 1024 * 1024;
    constexpr int backups = 5;

    QFileInfo fi(m_appLogPath);
    if (!fi.exists() || fi.size() < maxBytes) {
        return;
    }

    for (int i = backups; i >= 1; --i) {
        const QString src = (i == 1) ? m_appLogPath : (m_appLogPath + "." + QString::number(i - 1));
        const QString dst = m_appLogPath + "." + QString::number(i);
        if (QFile::exists(dst)) {
            QFile::remove(dst);
        }
        if (QFile::exists(src)) {
            QFile::rename(src, dst);
        }
    }
}

void MainWindow::appendLogToFile(const QString& line) {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    rotateLogIfNeeded();
    QFile f(m_appLogPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << line << '\n';
    ts.flush();
}

MainWindow::DatasetSelectionContext MainWindow::currentDatasetSelection(const QString& side) const {
    DatasetSelectionContext ctx;
    const QString token = (side == QStringLiteral("origin")) ? m_originPoolCombo->currentData().toString()
                                                              : m_destPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return ctx;
    }
    const int connIdx = token.left(sep).toInt();
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return ctx;
    }
    const QString pool = token.mid(sep + 2);
    const QString ds = (side == QStringLiteral("origin")) ? m_originSelectedDataset : m_destSelectedDataset;
    const QString snap = (side == QStringLiteral("origin")) ? m_originSelectedSnapshot : m_destSelectedSnapshot;
    if (ds.isEmpty()) {
        return ctx;
    }
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = pool;
    ctx.datasetName = ds;
    ctx.snapshotName = snap;
    return ctx;
}

void MainWindow::showDatasetContextMenu(const QString& side, QTreeWidget* tree, const QPoint& pos) {
    QTreeWidgetItem* item = tree->itemAt(pos);
    if (!item) {
        return;
    }
    tree->setCurrentItem(item);
    if (side == QStringLiteral("origin")) {
        onOriginTreeSelectionChanged();
    } else {
        onDestTreeSelectionChanged();
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }

    QMenu menu(this);
    QAction* mountAct = menu.addAction(QStringLiteral("Montar"));
    QAction* umountAct = menu.addAction(QStringLiteral("Desmontar"));
    menu.addSeparator();
    QAction* createAct = menu.addAction(QStringLiteral("Crear hijo"));
    QAction* deleteAct = menu.addAction(QStringLiteral("Borrar"));

    if (!ctx.snapshotName.isEmpty()) {
        mountAct->setEnabled(false);
        umountAct->setEnabled(false);
        createAct->setEnabled(false);
    }

    QAction* picked = menu.exec(tree->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == mountAct) {
        actionMountDataset(side);
    } else if (picked == umountAct) {
        actionUmountDataset(side);
    } else if (picked == createAct) {
        actionCreateChildDataset(side);
    } else if (picked == deleteAct) {
        actionDeleteDatasetOrSnapshot(side);
    }
}

bool MainWindow::executeDatasetAction(const QString& side, const QString& actionName, const DatasetSelectionContext& ctx, const QString& cmd, int timeoutMs) {
    if (!ctx.valid) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString remoteCmd = cmd;
    if (p.useSudo) {
        remoteCmd = QStringLiteral("sudo -n ") + remoteCmd;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 %2::%3").arg(actionName, p.name, ctx.datasetName));
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, remoteCmd, timeoutMs, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error en %1: %2")
                   .arg(actionName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("%1 falló:\n%2").arg(actionName, err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        return false;
    }
    if (!out.trimmed().isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 finalizado").arg(actionName));
    invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
    reloadDatasetSide(side);
    return true;
}

void MainWindow::invalidateDatasetCacheForPool(int connIdx, const QString& poolName) {
    m_poolDatasetCache.remove(datasetCacheKey(connIdx, poolName));
}

void MainWindow::reloadDatasetSide(const QString& side) {
    if (side == QStringLiteral("origin")) {
        onOriginPoolChanged();
    } else {
        onDestPoolChanged();
    }
}

void MainWindow::actionMountDataset(const QString& side) {
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    const QString cmd = QStringLiteral("zfs mount %1").arg(dsQ);
    executeDatasetAction(side, QStringLiteral("Montar"), ctx, cmd);
}

void MainWindow::actionUmountDataset(const QString& side) {
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    const QString hasChildrenCmd = QStringLiteral("zfs mount | awk '{print $1}' | grep -E '^%1/' -q").arg(ctx.datasetName);

    QString out;
    QString err;
    int rc = -1;
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString checkCmd = p.useSudo ? (QStringLiteral("sudo -n ") + hasChildrenCmd) : hasChildrenCmd;
    const bool ran = runSsh(p, checkCmd, 12000, out, err, rc);
    bool hasChildrenMounted = ran && rc == 0;
    QString cmd;
    if (hasChildrenMounted) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("ZFSMgr"),
            QStringLiteral("Hay hijos montados bajo %1.\n¿Desmontar recursivamente?").arg(ctx.datasetName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Desmontar abortado por usuario"));
            return;
        }
        cmd = QStringLiteral(
            "zfs mount | awk '{print $1}' | grep -E '^%1(/|$)' | awk '{print length, $0}' | sort -rn | cut -d' ' -f2- | "
            "while IFS= read -r ds; do [ -n \"$ds\" ] && zfs umount \"$ds\"; done")
                  .arg(ctx.datasetName);
    } else {
        cmd = QStringLiteral("zfs umount %1").arg(dsQ);
    }
    executeDatasetAction(side, QStringLiteral("Desmontar"), ctx, cmd);
}

void MainWindow::actionCreateChildDataset(const QString& side) {
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString leaf = QInputDialog::getText(
        this,
        QStringLiteral("Crear dataset"),
        QStringLiteral("Nombre hijo debajo de %1").arg(ctx.datasetName),
        QLineEdit::Normal,
        QString(),
        &ok);
    if (!ok || leaf.trimmed().isEmpty()) {
        return;
    }
    const QString child = ctx.datasetName + QStringLiteral("/") + leaf.trimmed();
    const QString cmd = QStringLiteral("zfs create %1").arg(shSingleQuote(child));
    executeDatasetAction(side, QStringLiteral("Crear dataset"), ctx, cmd);
}

void MainWindow::actionDeleteDatasetOrSnapshot(const QString& side) {
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }
    const QString target = ctx.snapshotName.isEmpty() ? ctx.datasetName : (ctx.datasetName + QStringLiteral("@") + ctx.snapshotName);
    const auto confirm1 = QMessageBox::question(
        this,
        QStringLiteral("Confirmar borrado"),
        QStringLiteral("Se va a borrar:\n%1\n¿Continuar?").arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm1 != QMessageBox::Yes) {
        return;
    }
    const auto confirm2 = QMessageBox::question(
        this,
        QStringLiteral("Confirmar borrado (2/2)"),
        QStringLiteral("Confirmación final de borrado:\n%1").arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm2 != QMessageBox::Yes) {
        return;
    }

    bool recursive = false;
    if (ctx.snapshotName.isEmpty()) {
        const auto askRec = QMessageBox::question(
            this,
            QStringLiteral("Borrado recursivo"),
            QStringLiteral("¿Borrar recursivamente datasets/snapshots hijos?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        recursive = (askRec == QMessageBox::Yes);
    }
    QString cmd;
    if (ctx.snapshotName.isEmpty()) {
        cmd = recursive ? QStringLiteral("zfs destroy -r %1").arg(shSingleQuote(target))
                        : QStringLiteral("zfs destroy %1").arg(shSingleQuote(target));
    } else {
        cmd = QStringLiteral("zfs destroy %1").arg(shSingleQuote(target));
    }
    executeDatasetAction(side, QStringLiteral("Borrar"), ctx, cmd, 90000);
}

MainWindow::ConnectionRuntimeState MainWindow::refreshConnection(const ConnectionProfile& p) {
    ConnectionRuntimeState state;
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio refresh: %1 [%2]").arg(p.name, p.connType));

    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Tipo de conexión no soportado aún en cppqt");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }
    if (p.host.isEmpty() || p.username.isEmpty()) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Host/usuario no definido");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }

    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, QStringLiteral("uname -a"), 12000, out, err, rc) || rc != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }
    state.status = QStringLiteral("OK");
    state.detail = oneLine(out);

    QString zpoolListCmd = QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio");
    QString zpoolImportCmd = QStringLiteral("zpool import");
    if (p.useSudo) {
        zpoolListCmd = QStringLiteral("sudo -n ") + zpoolListCmd;
        zpoolImportCmd = QStringLiteral("sudo -n ") + zpoolImportCmd;
    }

    out.clear();
    err.clear();
    rc = -1;
    if (runSsh(p, zpoolListCmd, 18000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString poolName = line.section('\t', 0, 0).trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            state.importedPools.push_back(PoolImported{p.name, poolName, QStringLiteral("Exportar")});
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zpool list -> %2").arg(p.name, oneLine(err)));
    }

    out.clear();
    err.clear();
    rc = -1;
    if (runSsh(p, zpoolImportCmd, 18000, out, err, rc) && rc == 0) {
        QString currentPool;
        QString currentState;
        QString currentReason;
        const QStringList lines = out.split('\n');
        auto flushCurrent = [&]() {
            if (currentPool.isEmpty()) {
                return;
            }
            state.importablePools.push_back(PoolImportable{
                p.name,
                currentPool,
                currentState.isEmpty() ? QStringLiteral("UNKNOWN") : currentState,
                currentReason,
                QStringLiteral("Importar"),
            });
            currentPool.clear();
            currentState.clear();
            currentReason.clear();
        };

        for (QString line : lines) {
            line = line.trimmed();
            if (line.startsWith(QStringLiteral("pool: "))) {
                flushCurrent();
                currentPool = line.mid(QStringLiteral("pool: ").size()).trimmed();
                continue;
            }
            if (line.startsWith(QStringLiteral("state: "))) {
                currentState = line.mid(QStringLiteral("state: ").size()).trimmed();
                continue;
            }
            if (line.startsWith(QStringLiteral("status: "))) {
                currentReason = line.mid(QStringLiteral("status: ").size()).trimmed();
                continue;
            }
            if (line.startsWith(QStringLiteral("cannot import"))) {
                if (!currentReason.isEmpty()) {
                    currentReason += QStringLiteral(" ");
                }
                currentReason += line;
            }
        }
        flushCurrent();
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zpool import -> %2").arg(p.name, oneLine(err)));
    }

    if (state.importedPools.isEmpty()) {
        state.importedPools.push_back(PoolImported{p.name, QStringLiteral("Sin pools"), QStringLiteral("-")});
    }
    if (state.importablePools.isEmpty()) {
        state.importablePools.push_back(PoolImportable{p.name, QStringLiteral("Sin pools"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-")});
    }

    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> OK (%2)").arg(p.name, state.detail));
    return state;
}

void MainWindow::populateAllPoolsTables() {
    m_importedPoolsTable->setRowCount(0);
    m_importablePoolsTable->setRowCount(0);
    for (int i = 0; i < m_states.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& pool : st.importedPools) {
            const int row = m_importedPoolsTable->rowCount();
            m_importedPoolsTable->insertRow(row);
            m_importedPoolsTable->setItem(row, 0, new QTableWidgetItem(pool.connection));
            m_importedPoolsTable->setItem(row, 1, new QTableWidgetItem(pool.pool));
            m_importedPoolsTable->setItem(row, 2, new QTableWidgetItem(pool.action));
        }
        for (const PoolImportable& pool : st.importablePools) {
            const int row = m_importablePoolsTable->rowCount();
            m_importablePoolsTable->insertRow(row);
            m_importablePoolsTable->setItem(row, 0, new QTableWidgetItem(pool.connection));
            m_importablePoolsTable->setItem(row, 1, new QTableWidgetItem(pool.pool));
            m_importablePoolsTable->setItem(row, 2, new QTableWidgetItem(pool.state));
            m_importablePoolsTable->setItem(row, 3, new QTableWidgetItem(pool.reason));
            m_importablePoolsTable->setItem(row, 4, new QTableWidgetItem(pool.action));
        }
    }
}

void MainWindow::updateStatus(const QString& text) {
    m_statusLabel->setText(text);
}

void MainWindow::appLog(const QString& level, const QString& msg) {
    const QString line = QStringLiteral("[%1] [%2] %3").arg(tsNow(), level, msg);
    m_logView->appendPlainText(line);
    appendLogToFile(line);
}
