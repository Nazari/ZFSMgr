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
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;
using mwhelpers::sshUserHostPort;
} // namespace

void MainWindow::exportPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    if (!connItem || !poolItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    const QString action = stateItem ? stateItem->data(Qt::UserRole + 1).toString().trimmed() : QString();
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
    setActionsLocked(false);
    appLog(QStringLiteral("INFO"), QStringLiteral("Refrescando conexión y listado de pools tras exportar: %1").arg(connName));
    refreshConnectionByIndex(idx);
    // Refuerzo explícito del refresco visual global de pools tras mutación.
    populateAllPoolsTables();
    refreshSelectedPoolDetails();
}

void MainWindow::importPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    if (!connItem || !poolItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    const QString poolState = stateItem ? stateItem->text().trimmed().toUpper() : QString();
    const QString action = stateItem ? stateItem->data(Qt::UserRole + 1).toString().trimmed() : QString();
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
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    if (!connItem || !poolItem || !stateItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    const QString poolState = stateItem->text().trimmed().toUpper();
    const QString action = stateItem->data(Qt::UserRole + 1).toString().trimmed();
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
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    if (!connItem || !poolItem || !stateItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    const QString action = stateItem->data(Qt::UserRole + 1).toString().trimmed();
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
    refreshSelectedPoolDetails();
}

void MainWindow::populateAllPoolsTables() {
    setTablePopulationMode(m_importedPoolsTable, true);
    m_importedPoolsTable->setRowCount(0);
    for (int i = 0; i < m_states.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& pool : st.importedPools) {
            const int row = m_importedPoolsTable->rowCount();
            m_importedPoolsTable->insertRow(row);
            m_importedPoolsTable->setItem(row, 0, new QTableWidgetItem(pool.connection));
            m_importedPoolsTable->setItem(row, 1, new QTableWidgetItem(pool.pool));
            auto* state = new QTableWidgetItem(QStringLiteral("ONLINE"));
            state->setForeground(QBrush(QColor("#1f7a1f")));
            state->setData(Qt::UserRole + 1, QStringLiteral("Exportar"));
            m_importedPoolsTable->setItem(row, 2, state);
            m_importedPoolsTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("Sí")));
            m_importedPoolsTable->setItem(row, 4, new QTableWidgetItem(QString()));
        }
        for (const PoolImportable& pool : st.importablePools) {
            const int row = m_importedPoolsTable->rowCount();
            m_importedPoolsTable->insertRow(row);
            m_importedPoolsTable->setItem(row, 0, new QTableWidgetItem(pool.connection));
            m_importedPoolsTable->setItem(row, 1, new QTableWidgetItem(pool.pool));
            auto* state = new QTableWidgetItem(pool.state);
            const QString up = pool.state.trimmed().toUpper();
            state->setForeground(QBrush((up == QStringLiteral("ONLINE")) ? QColor("#1f7a1f") : QColor("#a12a2a")));
            const QString action = (up == QStringLiteral("ONLINE")) ? pool.action : QString();
            state->setData(Qt::UserRole + 1, action);
            m_importedPoolsTable->setItem(row, 2, state);
            m_importedPoolsTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("No")));
            m_importedPoolsTable->setItem(row, 4, new QTableWidgetItem(pool.reason));
        }
    }
    setTablePopulationMode(m_importedPoolsTable, false);
    refreshSelectedPoolDetails();
    populateMountedDatasetsTables();
    updatePoolManagementBoxTitle();
}


void MainWindow::refreshSelectedPoolDetails() {
    if (!m_poolPropsTable || !m_poolStatusText || !m_importedPoolsTable) {
        return;
    }
    setTablePopulationMode(m_poolPropsTable, true);
    m_poolPropsTable->setRowCount(0);
    m_poolStatusText->clear();
    if (m_poolStatusImportBtn) {
        m_poolStatusImportBtn->setEnabled(false);
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

    const auto sel = m_importedPoolsTable->selectedItems();
    if (sel.isEmpty()) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const int row = sel.first()->row();
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    if (!connItem || !poolItem) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    QTableWidgetItem* stateItem = m_importedPoolsTable->item(row, 2);
    const QString poolState = stateItem ? stateItem->text().trimmed().toUpper() : QString();
    const QString action = stateItem ? stateItem->data(Qt::UserRole + 1).toString().trimmed() : QString();
    const bool canExport = (action.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0);
    const bool canImport = (action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0
                            && poolState == QStringLiteral("ONLINE"));
    const bool canScrub = canExport && poolState == QStringLiteral("ONLINE");
    const bool canDestroy = canExport;
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
            const int r = m_poolPropsTable->rowCount();
            m_poolPropsTable->insertRow(r);
            m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(parts[0].trimmed()));
            m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(parts[1].trimmed()));
            m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(parts[2].trimmed()));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    const QString stCmd = withSudo(
        p, QStringLiteral("zpool status -v %1").arg(shSingleQuote(poolName)));
    if (runSsh(p, stCmd, 20000, out, err, rc) && rc == 0) {
        m_poolStatusText->setPlainText(out.trimmed());
    } else {
        m_poolStatusText->setPlainText(err.trimmed());
    }
    setTablePopulationMode(m_poolPropsTable, false);
}
