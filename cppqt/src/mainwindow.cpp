#include "mainwindow.h"

#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_store(QStringLiteral("ZFSMgr")) {
    buildUi();
    loadConnections();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("ZFSMgr (C++/Qt)"));
    resize(1400, 900);

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
    connButtons->addWidget(new QPushButton(QStringLiteral("Nueva"), connectionsTab));
    connButtons->addWidget(new QPushButton(QStringLiteral("Refrescar todo"), connectionsTab));
    connButtons->addStretch(1);
    connLayout->addLayout(connButtons);

    connectionsTab->setLayout(connLayout);

    auto* datasetsTab = new QWidget(m_leftTabs);
    auto* dsLayout = new QVBoxLayout(datasetsTab);
    auto* dsLabel = new QLabel(QStringLiteral("UI de Datasets (WIP migración)"), datasetsTab);
    dsLayout->addWidget(dsLabel);
    dsLayout->addStretch(1);
    datasetsTab->setLayout(dsLayout);

    auto* advancedTab = new QWidget(m_leftTabs);
    auto* advLayout = new QVBoxLayout(advancedTab);
    advLayout->addWidget(new QLabel(QStringLiteral("Avanzado (WIP migración)"), advancedTab));
    advLayout->addStretch(1);
    advancedTab->setLayout(advLayout);

    m_leftTabs->addTab(connectionsTab, QStringLiteral("Conexiones"));
    m_leftTabs->addTab(datasetsTab, QStringLiteral("Datasets"));
    m_leftTabs->addTab(advancedTab, QStringLiteral("Avanzado"));
    leftLayout->addWidget(m_leftTabs, 1);

    auto* rightPane = new QWidget(topSplitter);
    auto* rightLayout = new QVBoxLayout(rightPane);

    m_rightTabs = new QTabWidget(rightPane);
    auto* detailsPools = new QWidget(m_rightTabs);
    auto* poolLayout = new QVBoxLayout(detailsPools);
    poolLayout->addWidget(new QLabel(QStringLiteral("Detalle de conexiones/pools (WIP)"), detailsPools));
    poolLayout->addStretch(1);
    m_rightTabs->addTab(detailsPools, QStringLiteral("Detalle"));

    auto* detailsDatasets = new QWidget(m_rightTabs);
    auto* datasetsLayout = new QVBoxLayout(detailsDatasets);
    datasetsLayout->addWidget(new QLabel(QStringLiteral("Propiedades del dataset (WIP)"), detailsDatasets));
    datasetsLayout->addStretch(1);
    m_rightTabs->addTab(detailsDatasets, QStringLiteral("Propiedades"));

    rightLayout->addWidget(m_rightTabs, 1);

    topSplitter->addWidget(leftPane);
    topSplitter->addWidget(rightPane);
    topSplitter->setStretchFactor(0, 3);
    topSplitter->setStretchFactor(1, 7);
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
}

void MainWindow::loadConnections() {
    m_connectionsList->clear();

    const QVector<ConnectionProfile> profiles = m_store.loadConnections();
    for (const ConnectionProfile& p : profiles) {
        const QString line1 = QStringLiteral("%1/%2").arg(p.name, p.connType);
        const QString line2 = QStringLiteral("%1 %2").arg(p.osType, p.host);
        auto* item = new QListWidgetItem(line1 + "\n" + line2, m_connectionsList);
        item->setToolTip(QStringLiteral("ID: %1\nHost: %2\nPort: %3")
                             .arg(p.id, p.host)
                             .arg(p.port));
    }

    updateStatus(QStringLiteral("Estado: %1 conexiones cargadas").arg(profiles.size()));
    m_logView->appendPlainText(QStringLiteral("[NORMAL] Loaded %1 connections from %2")
                                   .arg(profiles.size())
                                   .arg(m_store.iniPath()));
}

void MainWindow::updateStatus(const QString& text) {
    m_statusLabel->setText(text);
}
