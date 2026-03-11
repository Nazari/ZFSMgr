#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;
using mwhelpers::sshUserHostPort;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;

int selectedConnectionIndexFromTable(const QTableWidget* table) {
    if (!table) {
        return -1;
    }
    const int row = table->currentRow();
    if (row < 0 || row >= table->rowCount()) {
        return -1;
    }
    const QTableWidgetItem* it = table->item(row, 0);
    if (!it) {
        return -1;
    }
    bool ok = false;
    const int idx = it->data(Qt::UserRole).toInt(&ok);
    return ok ? idx : -1;
}

void setPoolRootTooltipFor(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& statusText) {
    if (!tree || connIdx < 0 || poolName.trimmed().isEmpty()) {
        return;
    }
    const QString poolKey = poolName.trimmed();
    const QString tt = statusText.toHtmlEscaped();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = tree->topLevelItem(i);
        if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
            continue;
        }
        const int rootConnIdx = root->data(0, kConnIdxRole).toInt();
        const QString rootPool = root->data(0, kPoolNameRole).toString().trimmed();
        if (rootConnIdx != connIdx || rootPool.compare(poolKey, Qt::CaseInsensitive) != 0) {
            continue;
        }
        root->setToolTip(
            0,
            QStringLiteral("<pre style=\"font-family:monospace; white-space:pre;\">%1</pre>").arg(tt));
        break;
    }
}
} // namespace

int MainWindow::findPoolRow(const QString& connection, const QString& pool) const {
    const QString connKey = connection.trimmed();
    const QString poolKey = pool.trimmed();
    for (int i = 0; i < m_poolListEntries.size(); ++i) {
        const auto& e = m_poolListEntries[i];
        if (e.connection.compare(connKey, Qt::CaseInsensitive) == 0
            && e.pool.compare(poolKey, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

QString MainWindow::poolDetailsCacheKey(int connIdx, const QString& poolName) const {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return QString();
    }
    return QStringLiteral("%1::%2")
        .arg(m_profiles[connIdx].name.trimmed().toLower(), poolName.trimmed().toLower());
}

void MainWindow::invalidatePoolDetailsCacheForConnection(int connIdx) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const QString prefix = QStringLiteral("%1::").arg(m_profiles[connIdx].name.trimmed().toLower());
    if (prefix.isEmpty()) {
        return;
    }
    auto it = m_poolDetailsCache.begin();
    while (it != m_poolDetailsCache.end()) {
        if (it.key().startsWith(prefix)) {
            it = m_poolDetailsCache.erase(it);
        } else {
            ++it;
        }
    }
    auto pit = m_datasetPropsCache.begin();
    while (pit != m_datasetPropsCache.end()) {
        if (pit.key().startsWith(prefix)) {
            pit = m_datasetPropsCache.erase(pit);
        } else {
            ++pit;
        }
    }
}

int MainWindow::selectedPoolRowFromTabs() const {
    if (!m_connectionsTable || !m_connectionEntityTabs) {
        return -1;
    }
    const int selectedConnIdx = selectedConnectionIndexFromTable(m_connectionsTable);
    if (selectedConnIdx < 0 || selectedConnIdx >= m_profiles.size()) {
        return -1;
    }
    const int tabIdx = m_connectionEntityTabs->currentIndex();
    if (tabIdx < 0 || tabIdx >= m_connectionEntityTabs->count()) {
        return -1;
    }
    const QString key = m_connectionEntityTabs->tabData(tabIdx).toString();
    const QStringList parts = key.split(':');
    if (parts.size() < 3 || parts.first() != QStringLiteral("pool")) {
        return -1;
    }
    bool ok = false;
    const int connIdx = parts.value(1).toInt(&ok);
    if (!ok || connIdx < 0 || connIdx >= m_profiles.size()) {
        return -1;
    }
    if (connIdx != selectedConnIdx) {
        return -1;
    }
    const QString connName = m_profiles[connIdx].name.trimmed();
    const QString poolName = parts.value(2).trimmed();
    if (connName.isEmpty() || poolName.isEmpty()) {
        return -1;
    }
    return findPoolRow(connName, poolName);
}

void MainWindow::exportPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    if (row < 0 || row >= m_poolListEntries.size()) {
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection;
    const QString poolName = pe.pool;
    const QString action = pe.action;
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        trk(QStringLiteral("t_export_pool_t1"), QStringLiteral("Exportar pool"), QStringLiteral("Export pool"), QStringLiteral("导出池")),
        trk(QStringLiteral("t_export_pool_q1"), QStringLiteral("¿Exportar pool %1 en %2?"),
            QStringLiteral("Export pool %1 on %2?"),
            QStringLiteral("在 %2 上导出池 %1？")).arg(poolName, connName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, QStringLiteral("zpool export %1").arg(shSingleQuote(poolName)));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Exportar"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio exportar %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error exportando %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_export_pool_e1"),
                                  QStringLiteral("Exportar falló:\n%1"),
                                  QStringLiteral("Export failed:\n%1"),
                                  QStringLiteral("导出失败：\n%1"))
                                  .arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin exportar %1::%2").arg(connName, poolName));
    invalidateDatasetCacheForPool(idx, poolName);
    appLog(QStringLiteral("DEBUG"),
           QStringLiteral("Caché invalidada tras exportar %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    appLog(QStringLiteral("INFO"), QStringLiteral("Refrescando conexión y listado de pools tras exportar: %1").arg(connName));
    refreshConnectionByIndex(idx);
    // Refuerzo explícito del refresco visual global de pools tras mutación.
    populateAllPoolsTables();
    refreshSelectedPoolDetails(true, true);
}

void MainWindow::importPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    if (row < 0 || row >= m_poolListEntries.size()) {
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection;
    const QString poolName = pe.pool;
    const QString poolState = pe.state.trimmed().toUpper();
    const QString action = pe.action;
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    if (poolState != QStringLiteral("ONLINE")) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Importar omitido %1::%2 (state=%3)")
                                      .arg(connName, poolName, poolState.isEmpty() ? QStringLiteral("UNKNOWN") : poolState));
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(trk(QStringLiteral("t_import_pool_w1"),
                           QStringLiteral("Importar pool: %1"),
                           QStringLiteral("Import pool: %1"),
                           QStringLiteral("导入池：%1")).arg(poolName));
    dlg.setModal(true);
    auto* lay = new QVBoxLayout(&dlg);

    auto* flagsBox = new QGroupBox(
        trk(QStringLiteral("t_flags_label001"), QStringLiteral("Flags"), QStringLiteral("Flags"), QStringLiteral("标志")), &dlg);
    auto* flagsLay = new QGridLayout(flagsBox);
    QCheckBox* forceCb = new QCheckBox(QStringLiteral("-f force"), flagsBox);
    QCheckBox* missingLogCb = new QCheckBox(QStringLiteral("-m missing log"), flagsBox);
    QCheckBox* noMountCb = new QCheckBox(QStringLiteral("-N do not mount"), flagsBox);
    QCheckBox* rewindCb = new QCheckBox(QStringLiteral("-F rewind"), flagsBox);
    QCheckBox* dryRunCb = new QCheckBox(QStringLiteral("-n dry run"), flagsBox);
    QCheckBox* destroyedCb = new QCheckBox(QStringLiteral("-D destroyed"), flagsBox);
    QCheckBox* extremeCb = new QCheckBox(QStringLiteral("-X extreme rewind"), flagsBox);
    QCheckBox* loadKeysCb = new QCheckBox(QStringLiteral("-l load keys"), flagsBox);
    flagsLay->addWidget(forceCb, 0, 0);
    flagsLay->addWidget(missingLogCb, 0, 1);
    flagsLay->addWidget(noMountCb, 1, 0);
    flagsLay->addWidget(rewindCb, 1, 1);
    flagsLay->addWidget(dryRunCb, 2, 0);
    flagsLay->addWidget(destroyedCb, 2, 1);
    flagsLay->addWidget(extremeCb, 3, 0);
    flagsLay->addWidget(loadKeysCb, 3, 1);
    lay->addWidget(flagsBox);

    auto* fieldsBox = new QGroupBox(
        trk(QStringLiteral("t_values_label01"), QStringLiteral("Valores"), QStringLiteral("Values"), QStringLiteral("参数值")), &dlg);
    auto* form = new QFormLayout(fieldsBox);
    QLineEdit* cachefileEd = new QLineEdit(fieldsBox);
    QLineEdit* altrootEd = new QLineEdit(fieldsBox);
    QLineEdit* dirsEd = new QLineEdit(fieldsBox);
    QLineEdit* mntoptsEd = new QLineEdit(fieldsBox);
    QLineEdit* propsEd = new QLineEdit(fieldsBox);
    QLineEdit* txgEd = new QLineEdit(fieldsBox);
    QLineEdit* newNameEd = new QLineEdit(fieldsBox);
    QLineEdit* extraEd = new QLineEdit(fieldsBox);
    form->addRow(QStringLiteral("cachefile"), cachefileEd);
    form->addRow(QStringLiteral("altroot"), altrootEd);
    form->addRow(QStringLiteral("directories (, )"), dirsEd);
    form->addRow(QStringLiteral("mntopts"), mntoptsEd);
    form->addRow(QStringLiteral("properties (, )"), propsEd);
    form->addRow(QStringLiteral("txg"), txgEd);
    form->addRow(QStringLiteral("new name"), newNameEd);
    form->addRow(QStringLiteral("extra args"), extraEd);
    lay->addWidget(fieldsBox);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    QStringList parts;
    parts << QStringLiteral("zpool import");
    if (forceCb->isChecked()) {
        parts << QStringLiteral("-f");
    }
    if (missingLogCb->isChecked()) {
        parts << QStringLiteral("-m");
    }
    if (noMountCb->isChecked()) {
        parts << QStringLiteral("-N");
    }
    if (rewindCb->isChecked()) {
        parts << QStringLiteral("-F");
    }
    if (dryRunCb->isChecked()) {
        parts << QStringLiteral("-n");
    }
    if (destroyedCb->isChecked()) {
        parts << QStringLiteral("-D");
    }
    if (extremeCb->isChecked()) {
        parts << QStringLiteral("-X");
    }
    if (loadKeysCb->isChecked()) {
        parts << QStringLiteral("-l");
    }
    if (!cachefileEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-c") << shSingleQuote(cachefileEd->text().trimmed());
    }
    if (!altrootEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-R") << shSingleQuote(altrootEd->text().trimmed());
    }
    for (const QString& d : dirsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString dd = d.trimmed();
        if (!dd.isEmpty()) {
            parts << QStringLiteral("-d") << shSingleQuote(dd);
        }
    }
    if (!mntoptsEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-o") << shSingleQuote(mntoptsEd->text().trimmed());
    }
    for (const QString& pval : propsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString pp = pval.trimmed();
        if (!pp.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(pp);
        }
    }
    if (!txgEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-T") << shSingleQuote(txgEd->text().trimmed());
    }
    parts << shSingleQuote(poolName);
    if (!newNameEd->text().trimmed().isEmpty()) {
        parts << shSingleQuote(newNameEd->text().trimmed());
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }

    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, parts.join(' '));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Importar"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio importar %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error importando %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_import_pool_e1"),
                                  QStringLiteral("Importar falló:\n%1"),
                                  QStringLiteral("Import failed:\n%1"),
                                  QStringLiteral("导入失败：\n%1"))
                                  .arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin importar %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
}

void MainWindow::scrubPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    if (row < 0 || row >= m_poolListEntries.size()) {
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection;
    const QString poolName = pe.pool;
    const QString poolState = pe.state.trimmed().toUpper();
    const QString action = pe.action;
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (poolState != QStringLiteral("ONLINE")) {
        return;
    }
    if (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        QStringLiteral("Scrub pool"),
        QStringLiteral("¿Iniciar scrub en pool %1 de %2?").arg(poolName, connName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, QStringLiteral("zpool scrub %1").arg(shSingleQuote(poolName)));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Scrub"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio scrub %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error scrub %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              QStringLiteral("Scrub falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin scrub %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
}

void MainWindow::destroyPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    if (row < 0 || row >= m_poolListEntries.size()) {
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection;
    const QString poolName = pe.pool;
    const QString action = pe.action;
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }
    const auto confirm = QMessageBox::warning(
        this,
        QStringLiteral("Destroy pool"),
        QStringLiteral("ATENCIÓN: se va a destruir el pool %1 en %2.\n¿Desea continuar?")
            .arg(poolName, connName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    const QString expectedPhrase = QStringLiteral("Quiero destruir el pool %1").arg(poolName);
    bool okText = false;
    const QString typed = QInputDialog::getText(
        this,
        QStringLiteral("Confirmación obligatoria"),
        QStringLiteral("Para confirmar, escriba exactamente:\n%1").arg(expectedPhrase),
        QLineEdit::Normal,
        QString(),
        &okText);
    if (!okText) {
        return;
    }
    if (typed.trimmed() != expectedPhrase) {
        QMessageBox::warning(
            this,
            QStringLiteral("Destroy pool"),
            QStringLiteral("Texto de confirmación incorrecto.\nOperación cancelada."));
        return;
    }

    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, QStringLiteral("zpool destroy %1").arg(shSingleQuote(poolName)));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Destroy"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio destroy %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 60000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error destroy %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              QStringLiteral("Destroy falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin destroy %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
    populateAllPoolsTables();
    refreshSelectedPoolDetails(true, true);
}

void MainWindow::populateAllPoolsTables() {
    m_poolListEntries.clear();
    for (int i = 0; i < m_states.size(); ++i) {
        if (i < m_profiles.size() && isConnectionDisconnected(i)) {
            continue;
        }
        if (i < m_profiles.size()) {
            const bool redirectedLocal = isConnectionRedirectedToLocal(i);
            if (redirectedLocal) {
                continue;
            }
        }
        const auto& st = m_states[i];
        for (const PoolImported& pool : st.importedPools) {
            PoolListEntry e;
            e.connection = pool.connection;
            e.pool = pool.pool;
            e.state = QStringLiteral("ONLINE");
            e.imported = QStringLiteral("Sí");
            e.reason.clear();
            e.action = QStringLiteral("Exportar");
            m_poolListEntries.push_back(std::move(e));
        }
        for (const PoolImportable& pool : st.importablePools) {
            const QString up = pool.state.trimmed().toUpper();
            PoolListEntry e;
            e.connection = pool.connection;
            e.pool = pool.pool;
            e.state = pool.state;
            e.imported = QStringLiteral("No");
            e.reason = pool.reason;
            e.action = (up == QStringLiteral("ONLINE")) ? pool.action : QString();
            m_poolListEntries.push_back(std::move(e));
        }
    }
    refreshSelectedPoolDetails(false, false);
    updatePoolManagementBoxTitle();
}


void MainWindow::refreshSelectedPoolDetails(bool forceRefresh, bool allowRemoteLoadIfMissing) {
    if (!m_poolPropsTable || !m_poolStatusText) {
        return;
    }
    setTablePopulationMode(m_poolPropsTable, true);
    m_poolPropsTable->setRowCount(0);
    m_poolStatusText->clear();
    if (m_poolStatusImportBtn) {
        m_poolStatusImportBtn->setEnabled(false);
    }
    if (m_poolStatusRefreshBtn) {
        m_poolStatusRefreshBtn->setProperty("zfsmgr_can_refresh", false);
        m_poolStatusRefreshBtn->setEnabled(false);
    }
    if (m_poolStatusExportBtn) {
        m_poolStatusExportBtn->setEnabled(false);
    }
    if (m_poolStatusScrubBtn) {
        m_poolStatusScrubBtn->setEnabled(false);
    }
    if (m_poolStatusDestroyBtn) {
        m_poolStatusDestroyBtn->setEnabled(false);
    }

    const int row = selectedPoolRowFromTabs();
    if (row < 0 || row >= m_poolListEntries.size()) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection;
    const QString poolName = pe.pool;
    const QString poolState = pe.state.trimmed().toUpper();
    const QString action = pe.action;
    const bool canExport = (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0);
    const bool canImport = (action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0
                            && poolState == QStringLiteral("ONLINE"));
    const bool canScrub = canExport && poolState == QStringLiteral("ONLINE");
    const bool canDestroy = canExport;
    if (m_poolStatusRefreshBtn) {
        m_poolStatusRefreshBtn->setProperty("zfsmgr_can_refresh", canExport);
        m_poolStatusRefreshBtn->setEnabled(!actionsLocked() && canExport);
    }
    if (m_poolStatusExportBtn) {
        m_poolStatusExportBtn->setEnabled(!actionsLocked() && canExport);
    }
    if (m_poolStatusImportBtn) {
        m_poolStatusImportBtn->setEnabled(!actionsLocked() && canImport);
    }
    if (m_poolStatusScrubBtn) {
        m_poolStatusScrubBtn->setEnabled(!actionsLocked() && canScrub);
    }
    if (m_poolStatusDestroyBtn) {
        m_poolStatusDestroyBtn->setEnabled(!actionsLocked() && canDestroy);
    }
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    if (!canExport) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0 || idx >= m_profiles.size()) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    const QString cacheKey = poolDetailsCacheKey(idx, poolName);

    auto loadFromCache = [this, &cacheKey, idx, poolName]() -> bool {
        const auto it = m_poolDetailsCache.constFind(cacheKey);
        if (it == m_poolDetailsCache.constEnd() || !it->loaded) {
            return false;
        }
        const PoolDetailsCacheEntry& cached = it.value();
        for (const QStringList& row : cached.propsRows) {
            if (row.size() < 3) {
                continue;
            }
            const int r = m_poolPropsTable->rowCount();
            m_poolPropsTable->insertRow(r);
            m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(row.value(0)));
            m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(row.value(1)));
            m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(row.value(2)));
        }
        m_poolStatusText->setPlainText(cached.statusText);
        setPoolRootTooltipFor(m_connContentTree, idx, poolName, cached.statusText);
        return true;
    };

    const bool cacheHit = loadFromCache();

    if (!forceRefresh && cacheHit) {
        if (m_connContentTree) {
            QTreeWidgetItem* sel = m_connContentTree->currentItem();
            if (sel && sel->data(0, kIsPoolRootRole).toBool()) {
                syncConnContentPoolColumns();
            }
        }
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    if (!allowRemoteLoadIfMissing) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }

    PoolDetailsCacheEntry fresh;
    QString out;
    QString err;
    int rc = -1;
    const QString propsCmd = withSudo(
        p, QStringLiteral("zpool get -H -o property,value,source all %1").arg(shSingleQuote(poolName)));
    if (runSsh(p, propsCmd, 20000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList parts = line.split('\t');
            if (parts.size() < 3) {
                continue;
            }
            const QStringList row{parts[0].trimmed(), parts[1].trimmed(), parts[2].trimmed()};
            fresh.propsRows.push_back(row);
            const int r = m_poolPropsTable->rowCount();
            m_poolPropsTable->insertRow(r);
            m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(row.value(0)));
            m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(row.value(1)));
            m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(row.value(2)));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    const QString stCmd = withSudo(
        p, QStringLiteral("zpool status -v %1").arg(shSingleQuote(poolName)));
    if (runSsh(p, stCmd, 20000, out, err, rc) && rc == 0) {
        fresh.statusText = out.trimmed();
        m_poolStatusText->setPlainText(fresh.statusText);
    } else {
        fresh.statusText = err.trimmed();
        m_poolStatusText->setPlainText(fresh.statusText);
    }
    setPoolRootTooltipFor(m_connContentTree, idx, poolName, fresh.statusText);
    fresh.loaded = true;
    m_poolDetailsCache.insert(cacheKey, fresh);
    if (m_connContentTree) {
        QTreeWidgetItem* sel = m_connContentTree->currentItem();
        if (sel && sel->data(0, kIsPoolRootRole).toBool()) {
            syncConnContentPoolColumns();
        }
    }
    setTablePopulationMode(m_poolPropsTable, false);
}
