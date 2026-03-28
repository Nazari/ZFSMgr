#include "mainwindow.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>

QString MainWindow::connContentTokenForTree(const QTreeWidget* tree) const {
    if (!tree) {
        return m_connContentToken.trimmed();
    }
    auto tokenFromItem = [](QTreeWidgetItem* item) -> QString {
        if (!item) {
            return QString();
        }
        QTreeWidgetItem* owner = item;
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, Qt::UserRole + 12).toBool()) {
            owner = owner->parent();
        }
        if (!owner) {
            return QString();
        }
        const int connIdx = owner->data(0, Qt::UserRole + 10).toInt();
        const QString poolName = owner->data(0, Qt::UserRole + 11).toString().trimmed();
        if (connIdx < 0 || poolName.isEmpty()) {
            return QString();
        }
        return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    };
    if (QTreeWidgetItem* current = tree->currentItem()) {
        const QString token = tokenFromItem(current);
        if (!token.isEmpty()) {
            return token;
        }
    }
    const auto selected = tree->selectedItems();
    if (!selected.isEmpty()) {
        const QString token = tokenFromItem(selected.first());
        if (!token.isEmpty()) {
            return token;
        }
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = tree->topLevelItem(i);
        if (!root || !root->data(0, Qt::UserRole + 12).toBool()) {
            continue;
        }
        const QString token = tokenFromItem(root);
        if (!token.isEmpty()) {
            return token;
        }
    }
    return m_connContentToken.trimmed();
}

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
        return currentConnContentSelection(m_connContentTree);
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

MainWindow::DatasetSelectionContext MainWindow::currentConnContentSelection(const QTreeWidget* tree) const {
    DatasetSelectionContext ctx;
    if (!tree) {
        return ctx;
    }
    constexpr int connIdxRole = Qt::UserRole + 10;
    constexpr int poolNameRole = Qt::UserRole + 11;
    QString token;
    QString ds;
    QString snap;
    auto* sel = tree->currentItem();
    if (!sel) {
        const auto selected = tree->selectedItems();
        if (!selected.isEmpty()) {
            sel = selected.first();
        }
    }
    if (sel) {
        while (sel && sel->data(0, Qt::UserRole).toString().isEmpty() && sel->parent()) {
            sel = sel->parent();
        }
        if (sel) {
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
        token = connContentTokenForTree(tree);
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
