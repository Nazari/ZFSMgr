#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QBrush>
#include <QColor>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>

namespace {
using mwhelpers::isMountedValueTrue;
} // namespace

void MainWindow::updateTransferButtonsState() {
    if (actionsLocked()) {
        if (m_btnCopy) m_btnCopy->setEnabled(false);
        if (m_btnLevel) m_btnLevel->setEnabled(false);
        if (m_btnSync) m_btnSync->setEnabled(false);
        if (m_btnAdvancedBreakdown) m_btnAdvancedBreakdown->setEnabled(false);
        if (m_btnAdvancedAssemble) m_btnAdvancedAssemble->setEnabled(false);
        if (m_btnAdvancedFromDir) m_btnAdvancedFromDir->setEnabled(false);
        if (m_btnAdvancedToDir) m_btnAdvancedToDir->setEnabled(false);
        return;
    }
    const bool srcDs = !m_originSelectedDataset.isEmpty();
    const bool srcSnap = !m_originSelectedSnapshot.isEmpty();
    const bool dstDs = !m_destSelectedDataset.isEmpty();
    const bool dstSnap = !m_destSelectedSnapshot.isEmpty();
    const QString srcSel = srcDs ? (srcSnap ? QStringLiteral("%1@%2").arg(m_originSelectedDataset, m_originSelectedSnapshot)
                                            : m_originSelectedDataset)
                                 : QString();
    const QString dstSel = dstDs ? (dstSnap ? QStringLiteral("%1@%2").arg(m_destSelectedDataset, m_destSelectedSnapshot)
                                            : m_destSelectedDataset)
                                 : QString();
    const bool sameSelection = !srcSel.isEmpty() && (srcSel == dstSel);
    const DatasetSelectionContext srcCtx = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dstCtx = currentDatasetSelection(QStringLiteral("dest"));
    const bool srcSelectionConsistent = srcCtx.valid
        && srcCtx.datasetName == m_originSelectedDataset
        && srcCtx.snapshotName == m_originSelectedSnapshot;
    const bool dstSelectionConsistent = dstCtx.valid
        && dstCtx.datasetName == m_destSelectedDataset
        && dstCtx.snapshotName == m_destSelectedSnapshot;
    auto datasetMountedInCache = [this](const DatasetSelectionContext& c) -> bool {
        if (!c.valid || c.datasetName.isEmpty()) {
            return false;
        }
        const QString key = datasetCacheKey(c.connIdx, c.poolName);
        const auto it = m_poolDatasetCache.constFind(key);
        if (it == m_poolDatasetCache.constEnd() || !it->loaded) {
            return false;
        }
        const auto recIt = it->recordByName.constFind(c.datasetName);
        if (recIt == it->recordByName.constEnd()) {
            return false;
        }
        return isMountedValueTrue(recIt->mounted);
    };
    const bool syncReady = srcDs && !srcSnap && dstDs && !dstSnap && !sameSelection
        && srcSelectionConsistent && dstSelectionConsistent
        && datasetMountedInCache(srcCtx) && datasetMountedInCache(dstCtx);
    m_btnCopy->setEnabled(srcDs && srcSnap && dstDs && !dstSnap);
    m_btnLevel->setEnabled(srcDs && dstDs && !dstSnap && !sameSelection);
    m_btnSync->setEnabled(syncReady);
    const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("advanced"));
    bool advDatasetOnly = actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty();
    if (advDatasetOnly) {
        const QString key = datasetCacheKey(actx.connIdx, actx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
            advDatasetOnly = false;
        } else {
            bool allMounted = true;
            const QString base = actx.datasetName;
            const QString pref = base + QStringLiteral("/");
            for (auto it = cacheIt->recordByName.constBegin(); it != cacheIt->recordByName.constEnd(); ++it) {
                const QString& ds = it.key();
                if (ds != base && !ds.startsWith(pref)) {
                    continue;
                }
                if (!isMountedValueTrue(it.value().mounted)) {
                    allMounted = false;
                    break;
                }
            }
            advDatasetOnly = allMounted;
        }
    }
    if (m_btnAdvancedBreakdown) {
        m_btnAdvancedBreakdown->setEnabled(advDatasetOnly);
    }
    if (m_btnAdvancedAssemble) {
        m_btnAdvancedAssemble->setEnabled(advDatasetOnly);
    }
    if (m_btnAdvancedFromDir) {
        m_btnAdvancedFromDir->setEnabled(actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty());
    }
    if (m_btnAdvancedToDir) {
        m_btnAdvancedToDir->setEnabled(actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty());
    }
}

void MainWindow::populateMountedDatasetsTables() {
    auto fill = [this](QTableWidget* table) {
        if (!table) {
            return;
        }
        setTablePopulationMode(table, true);
        table->setRowCount(0);
        QMap<QString, int> mountpointCountByConn;
        struct RowData {
            QString conn;
            QString dataset;
            QString mountpoint;
        };
        QVector<RowData> allRows;
        for (int i = 0; i < m_states.size() && i < m_profiles.size(); ++i) {
            const QString connName = m_profiles[i].name;
            const auto& rows = m_states[i].mountedDatasets;
            for (const auto& pair : rows) {
                allRows.push_back({connName, pair.first, pair.second});
                mountpointCountByConn[connName + QStringLiteral("::") + pair.second] += 1;
            }
        }
        for (const RowData& row : allRows) {
            const int r = table->rowCount();
            table->insertRow(r);
            auto* dsItem = new QTableWidgetItem(QStringLiteral("%1::%2").arg(row.conn, row.dataset));
            auto* mpItem = new QTableWidgetItem(row.mountpoint);
            const bool duplicated = mountpointCountByConn.value(row.conn + QStringLiteral("::") + row.mountpoint, 0) > 1;
            if (duplicated) {
                const QColor redWarn(QStringLiteral("#b22a2a"));
                dsItem->setForeground(QBrush(redWarn));
                mpItem->setForeground(QBrush(redWarn));
            }
            table->setItem(r, 0, dsItem);
            table->setItem(r, 1, mpItem);
        }
        setTablePopulationMode(table, false);
    };
    fill(m_mountedDatasetsTableLeft);
    fill(m_mountedDatasetsTableAdv);
}
