#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "mainwindow_ui_logic.h"

#include <QBrush>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
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

QString importTargetForPoolEntry(const QString& poolGuid, const QString& poolName) {
    const QString guid = poolGuid.trimmed();
    return guid.isEmpty() ? poolName.trimmed() : guid;
}

QStringList csvArgs(const QString& raw) {
    QStringList out;
    const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString t = part.trimmed();
        if (!t.isEmpty()) {
            out.push_back(t);
        }
    }
    return out;
}

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

} // namespace

QString MainWindow::formatPoolStatusTooltipHtml(const QString& statusText) const {
    const QString trimmed = statusText.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    return QStringLiteral("<pre style=\"font-family:'SF Mono','Menlo','Monaco','Consolas','Liberation Mono',monospace; white-space:pre;\">%1</pre>")
        .arg(trimmed.toHtmlEscaped());
}

QString MainWindow::cachedPoolStatusTooltipHtml(int connIdx, const QString& poolName) const {
    const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
    if (cacheKey.isEmpty()) {
        return QString();
    }
    const auto it = m_poolDetailsCache.constFind(cacheKey);
    if (it == m_poolDetailsCache.cend()) {
        return QString();
    }
    return formatPoolStatusTooltipHtml(it->statusText);
}

void MainWindow::applyPoolRootTooltipForTree(QTreeWidget* tree,
                                             int connIdx,
                                             const QString& poolName,
                                             const QString& statusText) const {
    if (!tree || connIdx < 0 || poolName.trimmed().isEmpty()) {
        return;
    }
    const QString poolKey = poolName.trimmed();
    const QString tooltipHtml = formatPoolStatusTooltipHtml(statusText);
    const QString fallbackText = QStringLiteral("%1::%2")
        .arg((connIdx >= 0 && connIdx < m_profiles.size()) ? m_profiles[connIdx].name : QString(),
             poolKey);
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = tree->topLevelItem(i);
        if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
            continue;
        }
        const int rootConnIdx = root->data(0, kConnIdxRole).toInt();
        const QString rootPool = root->data(0, kPoolNameRole).toString().trimmed();
        if (rootConnIdx != connIdx || rootPool != poolKey) {
            continue;
        }
        root->setToolTip(0, tooltipHtml.isEmpty() ? fallbackText : tooltipHtml);
        break;
    }
}

void MainWindow::applyPoolRootTooltipToVisibleTrees(int connIdx,
                                                    const QString& poolName,
                                                    const QString& statusText) const {
    applyPoolRootTooltipForTree(m_connContentTree, connIdx, poolName, statusText);
}

void MainWindow::cachePoolStatusTextsForConnection(int connIdx, const ConnectionRuntimeState& state) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    for (auto it = state.poolStatusByName.cbegin(); it != state.poolStatusByName.cend(); ++it) {
        const QString poolName = it.key().trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
        if (cacheKey.isEmpty()) {
            continue;
        }
        PoolDetailsCacheEntry entry = m_poolDetailsCache.value(cacheKey);
        entry.statusText = it.value().trimmed();
        m_poolDetailsCache.insert(cacheKey, entry);
        rebuildConnInfoFor(connIdx);
        applyPoolRootTooltipToVisibleTrees(connIdx, poolName, entry.statusText);
    }
}

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
    auto inFlight = m_poolDetailsLoadsInFlight.begin();
    while (inFlight != m_poolDetailsLoadsInFlight.end()) {
        if (inFlight->startsWith(prefix)) {
            inFlight = m_poolDetailsLoadsInFlight.erase(inFlight);
        } else {
            ++inFlight;
        }
    }
    if (const ConnInfo* connInfo = findConnInfo(connIdx)) {
        for (auto itPool = connInfo->poolsByStableId.cbegin(); itPool != connInfo->poolsByStableId.cend(); ++itPool) {
            removeDatasetPropertyEntriesForPool(connIdx, itPool->key.poolName);
        }
    }
}

int MainWindow::selectedPoolRowFromTabs() const {
    return -1;
}

void MainWindow::refreshPoolStatusNow(int connIdx, const QString& poolName) {
    const QString trimmedPool = poolName.trimmed();
    if (connIdx < 0 || connIdx >= m_profiles.size() || trimmedPool.isEmpty()) {
        return;
    }

    beginTransientUiBusy(trk(QStringLiteral("t_pool_refresh_status_busy001"),
                             QStringLiteral("Actualizando estado del pool..."),
                             QStringLiteral("Refreshing pool status..."),
                             QStringLiteral("正在刷新池状态...")));
    struct BusyGuard final {
        MainWindow* self{nullptr};
        ~BusyGuard() {
            if (self) {
                self->endTransientUiBusy();
            }
        }
    } busyGuard{this};

    const ConnectionProfile profile = m_profiles[connIdx];
    QString out;
    QString err;
    int rc = -1;
    const QString cmd = withSudo(
        profile,
        (isWindowsConnection(profile) || isLocalConnection(profile))
            ? mwhelpers::withUnixSearchPathCommand(
                  QStringLiteral("zpool status -v %1").arg(shSingleQuote(trimmedPool)))
            : remoteScriptCommand(profile, QStringLiteral("zfsmgr-zpool-status"), {trimmedPool}));
    if ((!runSsh(profile, cmd, 20000, out, err, rc) || rc != 0)
        && !isWindowsConnection(profile)
        && isLocalConnection(profile)) {
        const QString fallbackCmd = withSudo(
            profile,
            mwhelpers::withUnixSearchPathCommand(
                QStringLiteral("zpool status -v %1").arg(shSingleQuote(trimmedPool))));
        out.clear();
        err.clear();
        rc = -1;
        (void)runSsh(profile, fallbackCmd, 20000, out, err, rc);
    }
    if (rc != 0) {
        const QString errText = err.trimmed().isEmpty() ? oneLine(out).trimmed() : err.trimmed();
        appLog(QStringLiteral("WARN"),
               QStringLiteral("No se pudo refrescar estado de pool %1 en %2: %3")
                   .arg(trimmedPool, profile.name, errText));
        QMessageBox::warning(this,
                             trk(QStringLiteral("t_pool_refresh_status_fail_t1"),
                                 QStringLiteral("Refresh status"),
                                 QStringLiteral("Refresh status"),
                                 QStringLiteral("刷新状态")),
                             trk(QStringLiteral("t_pool_refresh_status_fail_q1"),
                                 QStringLiteral("No se pudo obtener el estado del pool %1.\n%2"),
                                 QStringLiteral("Could not get status for pool %1.\n%2"),
                                 QStringLiteral("无法获取池 %1 的状态。\n%2"))
                                 .arg(trimmedPool, errText));
        return;
    }

    PoolDetailsCacheEntry entry = m_poolDetailsCache.value(poolDetailsCacheKey(connIdx, trimmedPool));
    entry.loaded = true;
    entry.statusText = out.trimmed();
    m_poolDetailsCache.insert(poolDetailsCacheKey(connIdx, trimmedPool), entry);
    if (connIdx >= 0 && connIdx < m_states.size()) {
        m_states[connIdx].poolStatusByName.insert(trimmedPool, entry.statusText);
    }
    rebuildConnInfoFor(connIdx);
    applyPoolRootTooltipToVisibleTrees(connIdx, trimmedPool, entry.statusText);
    const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(trimmedPool);
    if (m_connContentTree) {
        syncConnContentPoolColumnsFor(m_connContentTree, token);
    }
    if (m_bottomConnContentTree && m_bottomConnContentTree != m_connContentTree) {
        syncConnContentPoolColumnsFor(m_bottomConnContentTree, token);
    }
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
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_export_dlg_t1"),
            QStringLiteral("Exportar pool: %1::%2"),
            QStringLiteral("Export pool: %1::%2"))
            .arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QCheckBox* allPoolsCb = new QCheckBox(trk(QStringLiteral("t_pool_export_all_001"),
                                              QStringLiteral("Exportar todos los pools (-a)"),
                                              QStringLiteral("Export all pools (-a)")),
                                          &dlg);
    QCheckBox* forceCb = new QCheckBox(trk(QStringLiteral("t_pool_export_force_001"),
                                           QStringLiteral("Forzar desmontaje (-f)"),
                                           QStringLiteral("Force unmount (-f)")),
                                       &dlg);
    QLineEdit* poolsEd = new QLineEdit(&dlg);
    poolsEd->setPlaceholderText(trk(QStringLiteral("t_pool_export_pools_ph_001"),
                                    QStringLiteral("pool2, pool3, ... (opcional)"),
                                    QStringLiteral("pool2, pool3, ... (optional)")));
    QLineEdit* extraEd = new QLineEdit(&dlg);
    allPoolsCb->setToolTip(trk(QStringLiteral("t_pool_export_all_tt_001"),
                               QStringLiteral("Si se marca, ejecuta export global y no usa el pool seleccionado."),
                               QStringLiteral("If checked, performs global export and ignores selected pool.")));
    forceCb->setToolTip(trk(QStringLiteral("t_pool_export_force_tt_001"),
                            QStringLiteral("Fuerza el desmontaje de datasets ocupados durante la exportación."),
                            QStringLiteral("Force unmount of busy datasets while exporting.")));
    auto* poolsLbl = new QLabel(trk(QStringLiteral("t_pool_export_pools_lbl_001"),
                                    QStringLiteral("Pools adicionales"),
                                    QStringLiteral("Additional pools")), &dlg);
    poolsLbl->setToolTip(trk(QStringLiteral("t_pool_export_pools_tt_001"),
                             QStringLiteral("Lista opcional (coma-separada) de pools extra a exportar."),
                             QStringLiteral("Optional comma-separated list of extra pools to export.")));
    poolsEd->setToolTip(poolsLbl->toolTip());
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(QString(), allPoolsCb);
    form->addRow(QString(), forceCb);
    form->addRow(poolsLbl, poolsEd);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Exportar pool cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool export");
    if (allPoolsCb->isChecked()) {
        parts << QStringLiteral("-a");
    }
    if (forceCb->isChecked()) {
        parts << QStringLiteral("-f");
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    if (!allPoolsCb->isChecked()) {
        parts << shSingleQuote(poolName);
    }
    for (const QString& extraPool : csvArgs(poolsEd->text())) {
        parts << shSingleQuote(extraPool);
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Exportar"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Export"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            false)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_export_pool_t1"),
                                  QStringLiteral("Exportar pool"),
                                  QStringLiteral("Export pool")),
                              trk(QStringLiteral("t_pool_export_e1"),
                                  QStringLiteral("No se pudo exportar el pool:\n%1"),
                                  QStringLiteral("Could not export pool:\n%1"))
                                  .arg(errorText));
        return;
    }
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
    const QString importTarget = importTargetForPoolEntry(pe.guid, pe.pool);
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

    auto poolNameInUseForConnection = [this](int connIdx, const QString& candidate, const QString& originalPoolName) -> bool {
        if (connIdx < 0 || connIdx >= m_states.size()) {
            return false;
        }
        const ConnectionRuntimeState& st = m_states[connIdx];
        QStringList importedPools;
        for (const PoolImported& pool : st.importedPools) {
            importedPools.push_back(pool.pool);
        }
        QStringList importablePools;
        for (const PoolImportable& pool : st.importablePools) {
            importablePools.push_back(pool.pool);
        }
        return zfsmgr::uilogic::isPoolNameInUse(importedPools, importablePools, candidate, originalPoolName);
    };

    QDialog dlg(this);
    dlg.setFont(QApplication::font());
    {
        const QFont baseUiFont = QApplication::font();
        const int baseUiPointSize = qMax(6, baseUiFont.pointSize());
        dlg.setStyleSheet(QStringLiteral(
            "QLabel, QLineEdit, QComboBox, QPushButton, QCheckBox, QGroupBox { "
            "font-family: '%1'; font-size: %2pt; }")
                              .arg(baseUiFont.family(),
                                   QString::number(baseUiPointSize)));
    }
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
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&, idx, poolName]() {
        const QString candidate = newNameEd->text().trimmed();
        if (!candidate.isEmpty()) {
            QString validationError;
            if (!zfsmgr::uilogic::isValidPoolRenameCandidate(candidate, &validationError)) {
                QMessageBox::warning(
                    &dlg,
                    QStringLiteral("ZFSMgr"),
                    validationError);
                return;
            }
            if (poolNameInUseForConnection(idx, candidate, poolName)) {
                QMessageBox::warning(
                    &dlg,
                    QStringLiteral("ZFSMgr"),
                    trk(QStringLiteral("t_import_pool_name_used_001"),
                        QStringLiteral("Ya existe un pool con ese nombre en esa conexión."),
                        QStringLiteral("A pool with that name already exists in that connection."),
                        QStringLiteral("该连接中已存在同名存储池。")));
                return;
            }
        }
        dlg.accept();
    });
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
    parts << shSingleQuote(importTarget);
    if (!newNameEd->text().trimmed().isEmpty()) {
        parts << shSingleQuote(newNameEd->text().trimmed());
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Importar pool cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Importar"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Import"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            false)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_import_btn001"),
                                  QStringLiteral("Importar"),
                                  QStringLiteral("Import")),
                              trk(QStringLiteral("t_pool_import_e1"),
                                  QStringLiteral("No se pudo importar el pool:\n%1"),
                                  QStringLiteral("Could not import pool:\n%1"))
                                  .arg(errorText));
        return;
    }
}

void MainWindow::importPoolRenamingFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    if (row < 0 || row >= m_poolListEntries.size()) {
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection.trimmed();
    const QString poolName = pe.pool.trimmed();
    const QString importTarget = importTargetForPoolEntry(pe.guid, pe.pool);
    const QString poolState = pe.state.trimmed().toUpper();
    const QString action = pe.action.trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (action.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) != 0
        || poolState != QStringLiteral("ONLINE")) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0 || idx >= m_states.size()) {
        return;
    }

    auto poolNameInUseForConnection = [this](int connIdx, const QString& candidate, const QString& originalPoolName) -> bool {
        if (connIdx < 0 || connIdx >= m_states.size()) {
            return false;
        }
        const ConnectionRuntimeState& st = m_states[connIdx];
        QStringList importedPools;
        for (const PoolImported& pool : st.importedPools) {
            importedPools.push_back(pool.pool);
        }
        QStringList importablePools;
        for (const PoolImportable& pool : st.importablePools) {
            importablePools.push_back(pool.pool);
        }
        return zfsmgr::uilogic::isPoolNameInUse(importedPools, importablePools, candidate, originalPoolName);
    };

    QString requestedNewName;
    while (true) {
        bool ok = false;
        const QString newName = QInputDialog::getText(
            this,
            QStringLiteral("Importar renombrando"),
            QStringLiteral("Nuevo nombre del pool"),
            QLineEdit::Normal,
            poolName,
            &ok).trimmed();
        if (!ok) {
            return;
        }
        QString validationError;
        if (!zfsmgr::uilogic::isValidPoolRenameCandidate(newName, &validationError)) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                validationError);
            continue;
        }
        if (poolNameInUseForConnection(idx, newName, poolName)) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                QStringLiteral("Ya existe un pool con ese nombre en esa conexión."));
            continue;
        }
        requestedNewName = newName;
        break;
    }

    // Reusar el diálogo de importación estándar, precargando el nuevo nombre.
    QDialog dlg(this);
    dlg.setFont(QApplication::font());
    {
        const QFont baseUiFont = QApplication::font();
        const int baseUiPointSize = qMax(6, baseUiFont.pointSize());
        dlg.setStyleSheet(QStringLiteral(
            "QLabel, QLineEdit, QComboBox, QPushButton, QCheckBox, QGroupBox { "
            "font-family: '%1'; font-size: %2pt; }")
                              .arg(baseUiFont.family(),
                                   QString::number(baseUiPointSize)));
    }
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
    newNameEd->setText(requestedNewName);
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
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&, idx, poolName]() {
        const QString candidate = newNameEd->text().trimmed();
        QString validationError;
        if (!zfsmgr::uilogic::isValidPoolRenameCandidate(candidate, &validationError)) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"), validationError);
            return;
        }
        if (poolNameInUseForConnection(idx, candidate, poolName)) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 QStringLiteral("Ya existe un pool con ese nombre en esa conexión."));
            return;
        }
        dlg.accept();
    });
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
    parts << shSingleQuote(importTarget) << shSingleQuote(newNameEd->text().trimmed());
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Importar renombrando cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Importar renombrando"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Import (rename)"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            false)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_import_btn001"),
                                  QStringLiteral("Importar"),
                                  QStringLiteral("Import")),
                              trk(QStringLiteral("t_pool_import_rename_e1"),
                                  QStringLiteral("No se pudo importar/renombrar el pool:\n%1"),
                                  QStringLiteral("Could not import/rename pool:\n%1"))
                                  .arg(errorText));
        return;
    }
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
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_scrub_dlg_t1"),
            QStringLiteral("Scrub pool: %1::%2"),
            QStringLiteral("Scrub pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QComboBox* modeCombo = new QComboBox(&dlg);
    modeCombo->addItem(trk(QStringLiteral("t_pool_scrub_mode_start_001"),
                           QStringLiteral("Iniciar/Reanudar"),
                           QStringLiteral("Start/Resume")), QString());
    modeCombo->addItem(trk(QStringLiteral("t_pool_scrub_mode_pause_001"),
                           QStringLiteral("Pausar (-p)"),
                           QStringLiteral("Pause (-p)")), QStringLiteral("-p"));
    modeCombo->addItem(trk(QStringLiteral("t_pool_scrub_mode_stop_001"),
                           QStringLiteral("Detener (-s)"),
                           QStringLiteral("Stop (-s)")), QStringLiteral("-s"));
    QCheckBox* waitCb = new QCheckBox(trk(QStringLiteral("t_pool_scrub_wait_001"),
                                          QStringLiteral("Esperar a fin (-w)"),
                                          QStringLiteral("Wait for completion (-w)")), &dlg);
    QCheckBox* errorOnlyCb = new QCheckBox(trk(QStringLiteral("t_pool_scrub_erroronly_001"),
                                               QStringLiteral("Solo bloques con error (-e, si soportado)"),
                                               QStringLiteral("Only error blocks (-e, if supported)")), &dlg);
    QLineEdit* extraEd = new QLineEdit(&dlg);
    auto* modeLbl = new QLabel(trk(QStringLiteral("t_pool_param_mode_001"),
                                   QStringLiteral("Modo"),
                                   QStringLiteral("Mode")), &dlg);
    modeLbl->setToolTip(trk(QStringLiteral("t_pool_scrub_mode_tt_001"),
                            QStringLiteral("Selecciona si se inicia/reanuda, pausa o detiene el scrub."),
                            QStringLiteral("Select whether to start/resume, pause, or stop the scrub.")));
    modeCombo->setToolTip(modeLbl->toolTip());
    waitCb->setToolTip(trk(QStringLiteral("t_pool_scrub_wait_tt_001"),
                           QStringLiteral("Bloquea la orden hasta que termine el scrub."),
                           QStringLiteral("Block until scrub completes.")));
    errorOnlyCb->setToolTip(trk(QStringLiteral("t_pool_scrub_erroronly_tt_001"),
                                QStringLiteral("Scrub solo sobre bloques con errores conocidos (si la versión lo soporta)."),
                                QStringLiteral("Scrub only known error blocks (if supported).")));
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(modeLbl, modeCombo);
    form->addRow(QString(), waitCb);
    form->addRow(QString(), errorOnlyCb);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Scrub cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool scrub");
    const QString modeFlag = modeCombo->currentData().toString().trimmed();
    if (!modeFlag.isEmpty()) {
        parts << modeFlag;
    }
    if (waitCb->isChecked()) {
        parts << QStringLiteral("-w");
    }
    if (errorOnlyCb->isChecked()) {
        parts << QStringLiteral("-e");
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    parts << shSingleQuote(poolName);
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Scrub"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Scrub"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_scrub_t1"),
                                  QStringLiteral("Scrub pool")),
                              trk(QStringLiteral("t_pool_scrub_e1"),
                                  QStringLiteral("No se pudo iniciar scrub:\n%1"))
                                  .arg(errorText));
        return;
    }
    refreshPoolStatusNow(idx, poolName);
}

void MainWindow::upgradePoolFromRow(int row) {
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

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_upgrade_dlg_t1"),
            QStringLiteral("Upgrade pool: %1::%2"),
            QStringLiteral("Upgrade pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QCheckBox* listVersionsCb = new QCheckBox(trk(QStringLiteral("t_pool_upgrade_listver_001"),
                                                  QStringLiteral("Listar versiones soportadas (-v)"),
                                                  QStringLiteral("List supported versions (-v)")), &dlg);
    QLineEdit* versionEd = new QLineEdit(&dlg);
    versionEd->setPlaceholderText(trk(QStringLiteral("t_pool_upgrade_version_ph_001"),
                                      QStringLiteral("ej: 5000  (usa -V)"),
                                      QStringLiteral("e.g. 5000 (uses -V)")));
    QLineEdit* extraEd = new QLineEdit(&dlg);
    listVersionsCb->setToolTip(trk(QStringLiteral("t_pool_upgrade_listver_tt_001"),
                                   QStringLiteral("Muestra versiones/flags de feature soportadas sin modificar el pool."),
                                   QStringLiteral("Show supported versions/features without modifying the pool.")));
    auto* versionLbl = new QLabel(trk(QStringLiteral("t_pool_upgrade_version_lbl_001"),
                                      QStringLiteral("Versión objetivo"),
                                      QStringLiteral("Target version")), &dlg);
    versionLbl->setToolTip(trk(QStringLiteral("t_pool_upgrade_version_tt_001"),
                               QStringLiteral("Versión a aplicar con -V. Déjalo vacío para upgrade por defecto."),
                               QStringLiteral("Version to apply with -V. Leave empty for default upgrade.")));
    versionEd->setToolTip(versionLbl->toolTip());
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(QString(), listVersionsCb);
    form->addRow(versionLbl, versionEd);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Upgrade cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool upgrade");
    if (listVersionsCb->isChecked()) {
        parts << QStringLiteral("-v");
    } else {
        if (!versionEd->text().trimmed().isEmpty()) {
            parts << QStringLiteral("-V") << shSingleQuote(versionEd->text().trimmed());
        }
        parts << shSingleQuote(poolName);
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Upgrade"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Upgrade"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_upgrade_t1"),
                                  QStringLiteral("Upgrade pool")),
                              trk(QStringLiteral("t_pool_upgrade_e1"),
                                  QStringLiteral("No se pudo ejecutar upgrade:\n%1"))
                                  .arg(errorText));
        return;
    }
    if (!listVersionsCb->isChecked()) {
        refreshPoolStatusNow(idx, poolName);
    }
}

void MainWindow::reguidPoolFromRow(int row) {
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

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_reguid_dlg_t1"),
            QStringLiteral("Reguid pool: %1::%2"),
            QStringLiteral("Reguid pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QLineEdit* guidEd = new QLineEdit(&dlg);
    guidEd->setPlaceholderText(trk(QStringLiteral("t_pool_reguid_guid_ph_001"),
                                   QStringLiteral("GUID hex opcional para -g"),
                                   QStringLiteral("Optional hex GUID for -g")));
    QLineEdit* extraEd = new QLineEdit(&dlg);
    auto* guidLbl = new QLabel(QStringLiteral("GUID"), &dlg);
    guidLbl->setToolTip(trk(QStringLiteral("t_pool_reguid_guid_tt_001"),
                            QStringLiteral("GUID opcional a asignar al pool (si la versión lo soporta)."),
                            QStringLiteral("Optional GUID to assign to the pool (if supported).")));
    guidEd->setToolTip(guidLbl->toolTip());
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(guidLbl, guidEd);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Reguid cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool reguid");
    if (!guidEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-g") << shSingleQuote(guidEd->text().trimmed());
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    parts << shSingleQuote(poolName);
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Reguid"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Reguid"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_reguid_t1"),
                                  QStringLiteral("Reguid pool")),
                              trk(QStringLiteral("t_pool_reguid_e1"),
                                  QStringLiteral("No se pudo ejecutar reguid:\n%1"))
                                  .arg(errorText));
        return;
    }
    refreshPoolStatusNow(idx, poolName);
}

void MainWindow::clearPoolFromRow(int row) {
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

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_clear_dlg_t1"),
            QStringLiteral("Clear pool: %1::%2"),
            QStringLiteral("Clear pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QLineEdit* devicesEd = new QLineEdit(&dlg);
    devicesEd->setPlaceholderText(trk(QStringLiteral("t_pool_devices_ph_001"),
                                      QStringLiteral("device1, device2, ..."),
                                      QStringLiteral("device1, device2, ...")));
    QCheckBox* powerCb = new QCheckBox(trk(QStringLiteral("t_pool_clear_power_001"),
                                           QStringLiteral("Power clear (--power, si soportado)"),
                                           QStringLiteral("Power clear (--power, if supported)")), &dlg);
    QLineEdit* extraEd = new QLineEdit(&dlg);
    auto* devicesLbl = new QLabel(trk(QStringLiteral("t_pool_devices_lbl_001"),
                                      QStringLiteral("Dispositivos"),
                                      QStringLiteral("Devices")), &dlg);
    devicesLbl->setToolTip(trk(QStringLiteral("t_pool_clear_devices_tt_001"),
                               QStringLiteral("Lista opcional de dispositivos separados por comas para limpiar solo esos errores."),
                               QStringLiteral("Optional comma-separated device list to clear only those errors.")));
    devicesEd->setToolTip(devicesLbl->toolTip());
    powerCb->setToolTip(trk(QStringLiteral("t_pool_clear_power_tt_001"),
                            QStringLiteral("Solicita limpieza de estado de energía en dispositivos que lo soporten."),
                            QStringLiteral("Request power-state clear on devices that support it.")));
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(devicesLbl, devicesEd);
    form->addRow(QString(), powerCb);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Clear cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool clear");
    if (powerCb->isChecked()) {
        parts << QStringLiteral("--power");
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    parts << shSingleQuote(poolName);
    for (const QString& dev : csvArgs(devicesEd->text())) {
        parts << shSingleQuote(dev);
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Clear"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Clear"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_clear_t1"),
                                  QStringLiteral("Clear pool"),
                                  QStringLiteral("Clear pool")),
                              trk(QStringLiteral("t_pool_clear_e1"),
                                  QStringLiteral("No se pudo ejecutar clear:\n%1"),
                                  QStringLiteral("Could not execute clear:\n%1"))
                                  .arg(errorText));
        return;
    }
    refreshPoolStatusNow(idx, poolName);
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
        trk(QStringLiteral("t_pool_destroy_t1"),
            QStringLiteral("Destroy pool"),
            QStringLiteral("Destroy pool")),
        trk(QStringLiteral("t_pool_destroy_warn_001"),
            QStringLiteral("ATENCIÓN: se va a destruir el pool %1 en %2.\n¿Desea continuar?"),
            QStringLiteral("WARNING: pool %1 on %2 will be destroyed.\nDo you want to continue?"))
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
        trk(QStringLiteral("t_pool_destroy_confirm_t1"),
            QStringLiteral("Confirmación obligatoria"),
            QStringLiteral("Mandatory confirmation")),
        trk(QStringLiteral("t_pool_destroy_confirm_q1"),
            QStringLiteral("Para confirmar, escriba exactamente:\n%1"),
            QStringLiteral("To confirm, type exactly:\n%1")).arg(expectedPhrase),
        QLineEdit::Normal,
        QString(),
        &okText);
    if (!okText) {
        return;
    }
    if (typed.trimmed() != expectedPhrase) {
        QMessageBox::warning(
            this,
            trk(QStringLiteral("t_pool_destroy_t1"),
                QStringLiteral("Destroy pool"),
                QStringLiteral("Destroy pool")),
            trk(QStringLiteral("t_pool_destroy_confirm_bad_001"),
                QStringLiteral("Texto de confirmación incorrecto.\nOperación cancelada."),
                QStringLiteral("Confirmation text is incorrect.\nOperation canceled.")));
        return;
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_destroy_dlg_t1"),
            QStringLiteral("Destroy pool: %1::%2"),
            QStringLiteral("Destroy pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QCheckBox* forceCb = new QCheckBox(trk(QStringLiteral("t_pool_destroy_force_001"),
                                           QStringLiteral("Forzar desmontaje/dispositivos ocupados (-f)"),
                                           QStringLiteral("Force unmount/busy devices (-f)")), &dlg);
    QLineEdit* extraEd = new QLineEdit(&dlg);
    forceCb->setToolTip(trk(QStringLiteral("t_pool_destroy_force_tt_001"),
                            QStringLiteral("Fuerza destroy si hay dispositivos ocupados o bloqueos de desmontaje."),
                            QStringLiteral("Force destroy when devices are busy or unmount is blocked.")));
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(QString(), forceCb);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Destroy pool cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool destroy");
    if (forceCb->isChecked()) {
        parts << QStringLiteral("-f");
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    parts << shSingleQuote(poolName);
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Destroy"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Destroy"),
                            cmd,
                            60000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_destroy_t1"),
                                  QStringLiteral("Destroy pool")),
                              trk(QStringLiteral("t_pool_destroy_e1"),
                                  QStringLiteral("No se pudo destruir el pool:\n%1"))
                                  .arg(errorText));
        return;
    }
}

void MainWindow::syncPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    if (row < 0 || row >= m_poolListEntries.size()) {
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection;
    const QString poolName = pe.pool;
    const QString poolAction = pe.action.trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (poolAction.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_sync_dlg_t1"),
            QStringLiteral("Sync pool: %1::%2"),
            QStringLiteral("Sync pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QCheckBox* allPoolsCb = new QCheckBox(trk(QStringLiteral("t_pool_sync_all_001"),
                                              QStringLiteral("Sincronizar todos los pools (sin argumento pool)"),
                                              QStringLiteral("Sync all pools (no pool argument)")), &dlg);
    QLineEdit* poolsEd = new QLineEdit(&dlg);
    poolsEd->setPlaceholderText(trk(QStringLiteral("t_pool_sync_pools_ph_001"),
                                    QStringLiteral("pool2, pool3, ... (opcional)"),
                                    QStringLiteral("pool2, pool3, ... (optional)")));
    QLineEdit* extraEd = new QLineEdit(&dlg);
    allPoolsCb->setToolTip(trk(QStringLiteral("t_pool_sync_all_tt_001"),
                               QStringLiteral("Si se marca, no se pasa el pool actual y se sincronizan todos."),
                               QStringLiteral("If checked, no current pool is passed and all pools are synced.")));
    auto* poolsLbl = new QLabel(trk(QStringLiteral("t_pool_sync_pools_lbl_001"),
                                    QStringLiteral("Pools adicionales"),
                                    QStringLiteral("Additional pools")), &dlg);
    poolsLbl->setToolTip(trk(QStringLiteral("t_pool_sync_pools_tt_001"),
                             QStringLiteral("Lista opcional (separada por comas) de pools extra a sincronizar."),
                             QStringLiteral("Optional comma-separated list of extra pools to sync.")));
    poolsEd->setToolTip(poolsLbl->toolTip());
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(QString(), allPoolsCb);
    form->addRow(poolsLbl, poolsEd);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Sync cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool sync");
    if (!allPoolsCb->isChecked()) {
        parts << shSingleQuote(poolName);
    }
    for (const QString& extraPool : csvArgs(poolsEd->text())) {
        parts << shSingleQuote(extraPool);
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Sync"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Sync"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_sync_t1"),
                                  QStringLiteral("Sync pool"),
                                  QStringLiteral("Sync pool")),
                              trk(QStringLiteral("t_pool_sync_e1"),
                                  QStringLiteral("No se pudo ejecutar sync:\n%1"),
                                  QStringLiteral("Could not execute sync:\n%1"))
                                  .arg(errorText));
        return;
    }
    if (!allPoolsCb->isChecked()) {
        refreshPoolStatusNow(idx, poolName);
    }
}

void MainWindow::trimPoolFromRow(int row) {
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
    const QString poolAction = pe.action.trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (poolState != QStringLiteral("ONLINE")) {
        return;
    }
    if (poolAction.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_trim_dlg_t1"),
            QStringLiteral("Trim pool: %1::%2"),
            QStringLiteral("Trim pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QComboBox* modeCombo = new QComboBox(&dlg);
    modeCombo->addItem(trk(QStringLiteral("t_pool_trim_mode_start_001"),
                           QStringLiteral("Iniciar"),
                           QStringLiteral("Start")), QString());
    modeCombo->addItem(trk(QStringLiteral("t_pool_trim_mode_cancel_001"),
                           QStringLiteral("Cancelar (-c)"),
                           QStringLiteral("Cancel (-c)")), QStringLiteral("-c"));
    modeCombo->addItem(trk(QStringLiteral("t_pool_trim_mode_pause_001"),
                           QStringLiteral("Pausar (-s)"),
                           QStringLiteral("Suspend (-s)")), QStringLiteral("-s"));
    QCheckBox* waitCb = new QCheckBox(trk(QStringLiteral("t_pool_trim_wait_001"),
                                          QStringLiteral("Esperar a fin (-w)"),
                                          QStringLiteral("Wait for completion (-w)")), &dlg);
    QCheckBox* secureCb = new QCheckBox(trk(QStringLiteral("t_pool_trim_secure_001"),
                                            QStringLiteral("Secure trim (-d)"),
                                            QStringLiteral("Secure trim (-d)")), &dlg);
    QLineEdit* rateEd = new QLineEdit(&dlg);
    rateEd->setPlaceholderText(trk(QStringLiteral("t_pool_trim_rate_ph_001"),
                                   QStringLiteral("Rate para -r (ej: 1G)"),
                                   QStringLiteral("Rate for -r (e.g. 1G)")));
    QLineEdit* devicesEd = new QLineEdit(&dlg);
    devicesEd->setPlaceholderText(trk(QStringLiteral("t_pool_devices_ph_001"),
                                      QStringLiteral("device1, device2, ..."),
                                      QStringLiteral("device1, device2, ...")));
    QLineEdit* extraEd = new QLineEdit(&dlg);
    auto* modeLbl = new QLabel(trk(QStringLiteral("t_pool_param_mode_001"),
                                   QStringLiteral("Modo"),
                                   QStringLiteral("Mode")), &dlg);
    modeLbl->setToolTip(trk(QStringLiteral("t_pool_trim_mode_tt_001"),
                            QStringLiteral("Acción sobre el trim: iniciar, cancelar o pausar."),
                            QStringLiteral("Trim action: start, cancel, or suspend.")));
    modeCombo->setToolTip(modeLbl->toolTip());
    waitCb->setToolTip(trk(QStringLiteral("t_pool_trim_wait_tt_001"),
                           QStringLiteral("Bloquea la orden hasta finalizar el trim."),
                           QStringLiteral("Block until trim completes.")));
    secureCb->setToolTip(trk(QStringLiteral("t_pool_trim_secure_tt_001"),
                             QStringLiteral("Solicita secure trim si está soportado."),
                             QStringLiteral("Request secure trim if supported.")));
    auto* rateLbl = new QLabel(QStringLiteral("Rate"), &dlg);
    rateLbl->setToolTip(trk(QStringLiteral("t_pool_trim_rate_tt_001"),
                            QStringLiteral("Límite de velocidad para trim (-r)."),
                            QStringLiteral("Rate limit for trim (-r).")));
    rateEd->setToolTip(rateLbl->toolTip());
    auto* devicesLbl = new QLabel(trk(QStringLiteral("t_pool_devices_lbl_001"),
                                      QStringLiteral("Dispositivos"),
                                      QStringLiteral("Devices")), &dlg);
    devicesLbl->setToolTip(trk(QStringLiteral("t_pool_trim_devices_tt_001"),
                               QStringLiteral("Dispositivos opcionales (coma-separados) para aplicar trim selectivo."),
                               QStringLiteral("Optional comma-separated devices for selective trim.")));
    devicesEd->setToolTip(devicesLbl->toolTip());
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(modeLbl, modeCombo);
    form->addRow(QString(), waitCb);
    form->addRow(QString(), secureCb);
    form->addRow(rateLbl, rateEd);
    form->addRow(devicesLbl, devicesEd);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Trim cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool trim");
    const QString modeFlag = modeCombo->currentData().toString().trimmed();
    if (!modeFlag.isEmpty()) {
        parts << modeFlag;
    }
    if (waitCb->isChecked()) {
        parts << QStringLiteral("-w");
    }
    if (secureCb->isChecked()) {
        parts << QStringLiteral("-d");
    }
    if (!rateEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-r") << shSingleQuote(rateEd->text().trimmed());
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    parts << shSingleQuote(poolName);
    for (const QString& dev : csvArgs(devicesEd->text())) {
        parts << shSingleQuote(dev);
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Trim"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Trim"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_trim_t1"),
                                  QStringLiteral("Trim pool"),
                                  QStringLiteral("Trim pool")),
                              trk(QStringLiteral("t_pool_trim_e1"),
                                  QStringLiteral("No se pudo ejecutar trim:\n%1"),
                                  QStringLiteral("Could not execute trim:\n%1"))
                                  .arg(errorText));
        return;
    }
    refreshPoolStatusNow(idx, poolName);
}

void MainWindow::initializePoolFromRow(int row) {
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
    const QString poolAction = pe.action.trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (poolState != QStringLiteral("ONLINE")) {
        return;
    }
    if (poolAction.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) != 0) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_init_dlg_t1"),
            QStringLiteral("Initialize pool: %1::%2"),
            QStringLiteral("Initialize pool: %1::%2")).arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();
    QComboBox* modeCombo = new QComboBox(&dlg);
    modeCombo->addItem(trk(QStringLiteral("t_pool_init_mode_start_001"),
                           QStringLiteral("Iniciar"),
                           QStringLiteral("Start")), QString());
    modeCombo->addItem(trk(QStringLiteral("t_pool_init_mode_cancel_001"),
                           QStringLiteral("Cancelar (-c)"),
                           QStringLiteral("Cancel (-c)")), QStringLiteral("-c"));
    modeCombo->addItem(trk(QStringLiteral("t_pool_init_mode_pause_001"),
                           QStringLiteral("Pausar (-s)"),
                           QStringLiteral("Suspend (-s)")), QStringLiteral("-s"));
    modeCombo->addItem(trk(QStringLiteral("t_pool_init_mode_uninit_001"),
                           QStringLiteral("Deshacer init (-u, si soportado)"),
                           QStringLiteral("Undo init (-u, if supported)")), QStringLiteral("-u"));
    QCheckBox* waitCb = new QCheckBox(trk(QStringLiteral("t_pool_init_wait_001"),
                                          QStringLiteral("Esperar a fin (-w, si soportado)"),
                                          QStringLiteral("Wait for completion (-w, if supported)")), &dlg);
    QLineEdit* devicesEd = new QLineEdit(&dlg);
    devicesEd->setPlaceholderText(trk(QStringLiteral("t_pool_devices_ph_001"),
                                      QStringLiteral("device1, device2, ..."),
                                      QStringLiteral("device1, device2, ...")));
    QLineEdit* extraEd = new QLineEdit(&dlg);
    auto* modeLbl = new QLabel(trk(QStringLiteral("t_pool_param_mode_001"),
                                   QStringLiteral("Modo"),
                                   QStringLiteral("Mode")), &dlg);
    modeLbl->setToolTip(trk(QStringLiteral("t_pool_init_mode_tt_001"),
                            QStringLiteral("Acción sobre initialize: iniciar, cancelar, pausar o deshacer."),
                            QStringLiteral("Initialize action: start, cancel, suspend, or undo.")));
    modeCombo->setToolTip(modeLbl->toolTip());
    waitCb->setToolTip(trk(QStringLiteral("t_pool_init_wait_tt_001"),
                           QStringLiteral("Bloquea la orden hasta finalizar initialize (si soportado)."),
                           QStringLiteral("Block until initialize completes (if supported).")));
    auto* devicesLbl = new QLabel(trk(QStringLiteral("t_pool_devices_lbl_001"),
                                      QStringLiteral("Dispositivos"),
                                      QStringLiteral("Devices")), &dlg);
    devicesLbl->setToolTip(trk(QStringLiteral("t_pool_init_devices_tt_001"),
                               QStringLiteral("Dispositivos opcionales (coma-separados) para initialize selectivo."),
                               QStringLiteral("Optional comma-separated devices for selective initialize.")));
    devicesEd->setToolTip(devicesLbl->toolTip());
    auto* extraLbl = new QLabel(trk(QStringLiteral("t_pool_param_extra_001"),
                                    QStringLiteral("Args extra"),
                                    QStringLiteral("Extra args")), &dlg);
    extraLbl->setToolTip(trk(QStringLiteral("t_pool_param_extra_tt_001"),
                             QStringLiteral("Argumentos adicionales avanzados que se añadirán al comando."),
                             QStringLiteral("Advanced extra arguments appended to the command.")));
    extraEd->setToolTip(extraLbl->toolTip());
    form->addRow(modeLbl, modeCombo);
    form->addRow(QString(), waitCb);
    form->addRow(devicesLbl, devicesEd);
    form->addRow(extraLbl, extraEd);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionProfile p = m_profiles[idx];
    if (isLocalConnection(p) && !isWindowsConnection(p)) {
        p.useSudo = true;
        if (!ensureLocalSudoCredentials(p)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Initialize cancelada: faltan credenciales sudo locales"));
            return;
        }
    }
    QStringList parts;
    parts << QStringLiteral("zpool initialize");
    const QString modeFlag = modeCombo->currentData().toString().trimmed();
    if (!modeFlag.isEmpty()) {
        parts << modeFlag;
    }
    if (waitCb->isChecked()) {
        parts << QStringLiteral("-w");
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }
    parts << shSingleQuote(poolName);
    for (const QString& dev : csvArgs(devicesEd->text())) {
        parts << shSingleQuote(dev);
    }
    QString rawCmd = parts.join(' ');
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(sshUserHostPort(p))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Initialize"), {preview})) {
        return;
    }
    QString errorText;
    if (!executePoolCommand(idx,
                            poolName,
                            QStringLiteral("Initialize"),
                            cmd,
                            45000,
                            &errorText,
                            true,
                            true)) {
        QMessageBox::critical(this,
                              trk(QStringLiteral("t_pool_init_t1"),
                                  QStringLiteral("Initialize pool"),
                                  QStringLiteral("Initialize pool")),
                              trk(QStringLiteral("t_pool_init_e1"),
                                  QStringLiteral("No se pudo ejecutar initialize:\n%1"),
                                  QStringLiteral("Could not execute initialize:\n%1"))
                                  .arg(errorText));
        return;
    }
    refreshPoolStatusNow(idx, poolName);
}

void MainWindow::showPoolHistoryFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    if (row < 0 || row >= m_poolListEntries.size()) {
        return;
    }
    const auto& pe = m_poolListEntries[row];
    const QString connName = pe.connection.trimmed();
    const QString poolName = pe.pool.trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }

    const ConnectionProfile& p = m_profiles[idx];
    QString rawCmd = QStringLiteral("zpool history %1").arg(shSingleQuote(poolName));
    if (!isWindowsConnection(p)) {
        rawCmd = mwhelpers::withUnixSearchPathCommand(rawCmd);
    }
    const QString cmd = withSudo(p, rawCmd);
    QString out;
    QString detail;
    if (!fetchPoolCommandOutput(idx, poolName, QStringLiteral("Historial"), cmd, &out, &detail, 45000)) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error historial %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(detail)));
        QMessageBox::critical(
            this,
            trk(QStringLiteral("t_pool_history_t1"),
                QStringLiteral("Historial de pool")),
            trk(QStringLiteral("t_pool_history_e1"),
                QStringLiteral("No se pudo obtener el historial:\n%1")).arg(detail));
        return;
    }

    QStringList lines = out.split('\n');
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(980, 600);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_pool_history_w1"),
            QStringLiteral("Historial de %1::%2"))
            .arg(connName, poolName));
    auto* lay = new QVBoxLayout(&dlg);
    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(1);
    table->setHorizontalHeaderLabels(
        {trk(QStringLiteral("t_pool_history_c1"),
             QStringLiteral("Línea"))});
    table->setRowCount(lines.size());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setWordWrap(true);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    for (int r = 0; r < lines.size(); ++r) {
        auto* it = new QTableWidgetItem(lines.value(r));
        it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table->setItem(r, 0, it);
    }
    lay->addWidget(table, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    lay->addWidget(bb);
    dlg.exec();
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
            e.guid = pool.guid;
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
        applyPoolRootTooltipToVisibleTrees(idx, poolName, cached.statusText);
        return true;
    };

    const bool cacheHit = loadFromCache();

    if (!forceRefresh && cacheHit) {
        auto syncTreeIfPoolRootSelected = [this](QTreeWidget* tree) {
            if (!tree) {
                return;
            }
            QTreeWidgetItem* sel = tree->currentItem();
            if (sel && sel->data(0, kIsPoolRootRole).toBool()) {
                syncConnContentPoolColumnsFor(tree, connContentTokenForTree(tree));
            }
        };
        syncTreeIfPoolRootSelected(m_connContentTree);
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    if (!allowRemoteLoadIfMissing) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }

    if (!ensurePoolDetailsLoaded(idx, poolName)) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    const PoolDetailsCacheEntry* loaded = poolDetailsEntry(idx, poolName);
    if (!loaded) {
        setTablePopulationMode(m_poolPropsTable, false);
        return;
    }
    for (const QStringList& row : loaded->propsRows) {
        if (row.size() < 3) {
            continue;
        }
        const int r = m_poolPropsTable->rowCount();
        m_poolPropsTable->insertRow(r);
        m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(row.value(0)));
        m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(row.value(1)));
        m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(row.value(2)));
    }
    m_poolStatusText->setPlainText(loaded->statusText);
    applyPoolRootTooltipToVisibleTrees(idx, poolName, loaded->statusText);
    auto syncTreeIfPoolRootSelected = [this](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        QTreeWidgetItem* sel = tree->currentItem();
        if (sel && sel->data(0, kIsPoolRootRole).toBool()) {
            syncConnContentPoolColumnsFor(tree, connContentTokenForTree(tree));
        }
    };
    syncTreeIfPoolRootSelected(m_connContentTree);
    setTablePopulationMode(m_poolPropsTable, false);
}
