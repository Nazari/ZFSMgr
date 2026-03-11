#include "mainwindow.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>

MainWindow::DatasetSelectionContext MainWindow::currentDatasetSelection(const QString& side) const {
    DatasetSelectionContext ctx;
    if (m_transferSelectionOverrideActive) {
        if (side == QStringLiteral("origin") && m_transferSelectionOverrideOrigin.valid) {
            return m_transferSelectionOverrideOrigin;
        }
        if (side == QStringLiteral("dest") && m_transferSelectionOverrideDest.valid) {
            return m_transferSelectionOverrideDest;
        }
    }
    constexpr int connIdxRole = Qt::UserRole + 10;
    constexpr int poolNameRole = Qt::UserRole + 11;
    QString token;
    QString ds;
    QString snap;
    if (side == QStringLiteral("origin")) {
        if (m_connActionOrigin.valid) {
            return m_connActionOrigin;
        }
        return ctx;
    } else if (side == QStringLiteral("dest")) {
        if (m_connActionDest.valid) {
            return m_connActionDest;
        }
        return ctx;
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
    if (connIdx < 0 || connIdx >= m_profiles.size() || ds.isEmpty()) {
        return ctx;
    }
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = token.mid(sep + 2);
    ctx.datasetName = ds;
    ctx.snapshotName = snap;
    return ctx;
}
