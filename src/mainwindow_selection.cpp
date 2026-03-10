#include "mainwindow.h"

#include <QComboBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>

MainWindow::DatasetSelectionContext MainWindow::currentDatasetSelection(const QString& side) const {
    DatasetSelectionContext ctx;
    constexpr int connIdxRole = Qt::UserRole + 10;
    constexpr int poolNameRole = Qt::UserRole + 11;
    QString token;
    QString ds;
    QString snap;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
        ds = m_originSelectedDataset;
        snap = m_originSelectedSnapshot;
        if ((token.isEmpty() || ds.isEmpty()) && m_connActionOrigin.valid) {
            return m_connActionOrigin;
        }
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();
        ds = m_destSelectedDataset;
        snap = m_destSelectedSnapshot;
        if ((token.isEmpty() || ds.isEmpty()) && m_connActionDest.valid) {
            return m_connActionDest;
        }
    } else if (side == QStringLiteral("conncontent")) {
        if (m_connContentTree) {
            const auto selected = m_connContentTree->selectedItems();
            if (!selected.isEmpty()) {
                auto* sel = selected.first();
                while (sel && sel->data(0, Qt::UserRole).toString().isEmpty() && sel->parent()) {
                    sel = sel->parent();
                }
                ds = sel->data(0, Qt::UserRole).toString();
                snap = sel->data(1, Qt::UserRole).toString();
                const int itemConnIdx = sel->data(0, connIdxRole).toInt();
                const QString itemPool = sel->data(0, poolNameRole).toString();
                if (itemConnIdx >= 0 && !itemPool.isEmpty()) {
                    token = QStringLiteral("%1::%2").arg(itemConnIdx).arg(itemPool);
                }
            }
        }
        if (token.isEmpty()) {
            token = m_connContentToken;
        }
    } else {
        return ctx;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return ctx;
    }
    const int connIdx = token.left(sep).toInt();
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return ctx;
    }
    const QString pool = token.mid(sep + 2);
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
