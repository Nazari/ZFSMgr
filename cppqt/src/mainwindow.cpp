#include "mainwindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFont>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString tsNow() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

bool looksEncrypted(const QString& v) {
    return v.startsWith("encv1$");
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_store(QStringLiteral("ZFSMgr")) {
    buildUi();
    loadConnections();
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
    auto* dsLayout = new QVBoxLayout(datasetsTab);
    dsLayout->addWidget(new QLabel(QStringLiteral("Datasets (WIP migración C++/Qt)"), datasetsTab));
    dsLayout->addStretch(1);
    datasetsTab->setLayout(dsLayout);

    auto* advancedTab = new QWidget(m_leftTabs);
    auto* advLayout = new QVBoxLayout(advancedTab);
    advLayout->addWidget(new QLabel(QStringLiteral("Avanzado (WIP migración C++/Qt)"), advancedTab));
    advLayout->addStretch(1);
    advancedTab->setLayout(advLayout);

    m_leftTabs->addTab(connectionsTab, QStringLiteral("Conexiones"));
    m_leftTabs->addTab(datasetsTab, QStringLiteral("Datasets"));
    m_leftTabs->addTab(advancedTab, QStringLiteral("Avanzado"));
    leftLayout->addWidget(m_leftTabs, 1);

    auto* rightPane = new QWidget(topSplitter);
    auto* rightLayout = new QVBoxLayout(rightPane);

    m_rightTabs = new QTabWidget(rightPane);

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
    m_importablePoolsTable->setColumnCount(4);
    m_importablePoolsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Conexión"), QStringLiteral("Pool"), QStringLiteral("Estado"), QStringLiteral("Acción")});
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_importablePoolsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    importableLayout->addWidget(m_importablePoolsTable, 1);

    m_rightTabs->addTab(importedTab, QStringLiteral("Pools importados"));
    m_rightTabs->addTab(importableTab, QStringLiteral("Pools importables"));
    rightLayout->addWidget(m_rightTabs, 1);

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
    m_logView->setFont(mono);

    logLayout->addWidget(m_statusLabel);
    logLayout->addWidget(m_logView, 1);
    root->addWidget(logBox, 2);

    setCentralWidget(central);

    connect(m_btnRefreshAll, &QPushButton::clicked, this, [this]() { refreshAllConnections(); });
    connect(m_btnRefreshSelected, &QPushButton::clicked, this, [this]() { refreshSelectedConnection(); });
    connect(m_connectionsList, &QListWidget::itemSelectionChanged, this, [this]() { onConnectionSelectionChanged(); });
}

void MainWindow::loadConnections() {
    m_profiles = m_store.loadConnections();
    m_states.clear();
    m_states.resize(m_profiles.size());

    rebuildConnectionList();
    updateStatus(QStringLiteral("Estado: %1 conexiones cargadas").arg(m_profiles.size()));
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
}

void MainWindow::rebuildConnectionList() {
    m_connectionsList->clear();
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        const auto& s = m_states[i];

        const QString line1 = QStringLiteral("%1/%2").arg(p.name, p.connType);
        QString line2 = QStringLiteral("%1").arg(p.osType);
        if (!s.status.isEmpty()) {
            line2 += QStringLiteral("  [%1]").arg(s.status);
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

void MainWindow::refreshAllConnections() {
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refrescar todas las conexiones"));
    for (int i = 0; i < m_profiles.size(); ++i) {
        m_states[i] = refreshConnection(m_profiles[i]);
    }
    rebuildConnectionList();
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
    if (idx < m_connectionsList->count()) {
        m_connectionsList->setCurrentRow(idx);
    }
}

void MainWindow::onConnectionSelectionChanged() {
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        m_importedPoolsTable->setRowCount(0);
        m_importablePoolsTable->setRowCount(0);
        return;
    }

    const int idx = selected.first()->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    populatePoolsForConnection(m_profiles[idx]);
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

    if (looksEncrypted(p.username)) {
        state.status = QStringLiteral("WIP");
        state.detail = QStringLiteral("Usuario encriptado: pendiente diálogo de password maestro");
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: %2").arg(p.name, state.detail));
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> WIP").arg(p.name));
        return state;
    }

    if (p.host.isEmpty() || p.username.isEmpty()) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Host/usuario no definido");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }

    QStringList args;
    args << "-o" << "BatchMode=yes";
    args << "-o" << "ConnectTimeout=8";
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
    if (p.port > 0) {
        args << "-p" << QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        args << "-i" << p.keyPath;
    }
    args << QStringLiteral("%1@%2").arg(p.username, p.host);
    args << "uname -a";

    QProcess proc;
    proc.start(QStringLiteral("ssh"), args);
    if (!proc.waitForStarted(4000)) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("No se pudo iniciar ssh");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }

    if (!proc.waitForFinished(12000)) {
        proc.kill();
        proc.waitForFinished(1000);
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Timeout en ssh uname -a");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }

    const int rc = proc.exitCode();
    const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();

    if (rc == 0) {
        state.status = QStringLiteral("OK");
        state.detail = out.left(140);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> OK (%2)").arg(p.name, state.detail));
    } else {
        state.status = QStringLiteral("ERROR");
        state.detail = err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err.left(140);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
    }

    return state;
}

void MainWindow::populatePoolsForConnection(const ConnectionProfile& p) {
    m_importedPoolsTable->setRowCount(0);
    m_importablePoolsTable->setRowCount(0);

    const auto addImported = [&](const QString& conn, const QString& pool, const QString& action) {
        const int row = m_importedPoolsTable->rowCount();
        m_importedPoolsTable->insertRow(row);
        m_importedPoolsTable->setItem(row, 0, new QTableWidgetItem(conn));
        m_importedPoolsTable->setItem(row, 1, new QTableWidgetItem(pool));
        m_importedPoolsTable->setItem(row, 2, new QTableWidgetItem(action));
    };

    const auto addImportable = [&](const QString& conn, const QString& pool, const QString& state, const QString& action) {
        const int row = m_importablePoolsTable->rowCount();
        m_importablePoolsTable->insertRow(row);
        m_importablePoolsTable->setItem(row, 0, new QTableWidgetItem(conn));
        m_importablePoolsTable->setItem(row, 1, new QTableWidgetItem(pool));
        m_importablePoolsTable->setItem(row, 2, new QTableWidgetItem(state));
        m_importablePoolsTable->setItem(row, 3, new QTableWidgetItem(action));
    };

    // Placeholder de transición hasta migrar el parser zpool/zfs real.
    if (p.osType.compare(QStringLiteral("Linux"), Qt::CaseInsensitive) == 0) {
        addImported(p.name, QStringLiteral("(sin datos aún)"), QStringLiteral("Exportar"));
        addImportable(p.name, QStringLiteral("(sin datos aún)"), QStringLiteral("ONLINE"), QStringLiteral("Importar"));
    } else {
        addImported(p.name, QStringLiteral("Sin pools"), QStringLiteral("-"));
        addImportable(p.name, QStringLiteral("Sin pools"), QStringLiteral("-"), QStringLiteral("-"));
    }
}

void MainWindow::updateStatus(const QString& text) {
    m_statusLabel->setText(text);
}

void MainWindow::appLog(const QString& level, const QString& msg) {
    const QString line = QStringLiteral("[%1] [%2] %3").arg(tsNow(), level, msg);
    m_logView->appendPlainText(line);
}
