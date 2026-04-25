#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QApplication>
#include <QSignalBlocker>
#include <QStyle>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {
using mwhelpers::shSingleQuote;

constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kConnFileBrowserNodeRole = Qt::UserRole + 53;
constexpr int kConnFileBrowserPathRole = Qt::UserRole + 54;
constexpr int kConnFileBrowserIsDirRole = Qt::UserRole + 55;
constexpr int kConnFileBrowserLoadedRole = Qt::UserRole + 56;
constexpr int kConnRootInlineFieldRole = Qt::UserRole + 38;
constexpr int kConnPropRowKindRole = Qt::UserRole + 16;
constexpr int kConnInlineCellUsedRole = Qt::UserRole + 32;

struct FileBrowserEntry {
    QString permissions;
    QString owner;
    QString group;
    QString size;
    QString mtime;
    QString name;
    bool isDir = false;
    bool isLink = false;
};

static QString humanReadableSize(qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    }
    if (bytes < 1024LL * 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    }
    return QStringLiteral("%1 GB").arg(bytes / (1024LL * 1024 * 1024));
}

static QList<FileBrowserEntry> parseLsOutput(const QString& out) {
    QList<FileBrowserEntry> result;
    const QStringList lines = out.split('\n', Qt::KeepEmptyParts);
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("total ")) || line.isEmpty()) {
            continue;
        }
        // Expected: perms nlinks owner group size month day time name
        // Example:  drwxr-xr-x  2 user group 4096 Jan 15 12:34 dirname
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 9) {
            continue;
        }
        FileBrowserEntry e;
        e.permissions = parts.at(0);
        e.owner = parts.at(2);
        e.group = parts.at(3);
        const qint64 rawSize = parts.at(4).toLongLong();
        e.size = humanReadableSize(rawSize);
        e.mtime = parts.at(5) + QStringLiteral(" ") + parts.at(6) + QStringLiteral(" ") + parts.at(7);
        // name: everything from index 8 onward (handles spaces)
        QStringList nameParts;
        for (int i = 8; i < parts.size(); ++i) {
            const QString& p = parts.at(i);
            if (p == QStringLiteral("->")) {
                break;
            }
            nameParts << p;
        }
        e.name = nameParts.join(' ');
        if (e.name.isEmpty()) {
            continue;
        }
        e.isDir = e.permissions.startsWith(QLatin1Char('d'));
        e.isLink = e.permissions.startsWith(QLatin1Char('l'));
        result.push_back(e);
    }
    return result;
}

} // namespace

void MainWindow::populateFileBrowserNode(QTreeWidget* tree, QTreeWidgetItem* browserNode) {
    if (!tree || !browserNode) {
        return;
    }
    if (browserNode->data(0, kConnFileBrowserLoadedRole).toBool()) {
        return;
    }
    browserNode->setData(0, kConnFileBrowserLoadedRole, true);

    const int connIdx = browserNode->data(0, kConnIdxRole).toInt();
    const QString dirPath = browserNode->data(0, kConnFileBrowserPathRole).toString().trimmed();
    if (dirPath.isEmpty() || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }

    const QSignalBlocker blocker(tree);
    while (browserNode->childCount() > 0) {
        delete browserNode->takeChild(0);
    }

    const ConnectionProfile& prof = m_profiles[connIdx];
    const QString remoteCmd = QStringLiteral(
                                  "p=%1; "
                                  "if [ -d \"$p\" ]; then ls -lA \"$p\" 2>&1; else echo \"not a directory: $p\" >&2; exit 2; fi")
                                  .arg(shSingleQuote(dirPath));

    QString out;
    QString err;
    int rc = -1;
    beginTransientUiBusy(QStringLiteral("Leyendo contenido..."));
    const bool ran = runSsh(prof, remoteCmd, 20000, out, err, rc);
    endTransientUiBusy();

    if (!ran || rc != 0) {
        auto* errItem = new QTreeWidgetItem(browserNode);
        errItem->setText(0, QStringLiteral("(error: %1)").arg(err.trimmed().isEmpty() ? out.trimmed() : err.trimmed()));
        errItem->setFlags(errItem->flags() & ~Qt::ItemIsUserCheckable);
        return;
    }

    const QList<FileBrowserEntry> entries = parseLsOutput(out);
    if (entries.isEmpty()) {
        auto* emptyItem = new QTreeWidgetItem(browserNode);
        emptyItem->setText(0, QStringLiteral("(vacío)"));
        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsUserCheckable);
        return;
    }

    // Determine inline property columns available
    const int propCols = qMax(1, tree->columnCount() - 4);

    auto addInlineProps = [&](QTreeWidgetItem* parent, const QVector<QPair<QString, QString>>& props) {
        for (int base = 0; base < props.size(); base += propCols) {
            auto* rowNames = new QTreeWidgetItem();
            rowNames->setData(0, kConnRootInlineFieldRole, true);
            rowNames->setData(0, kConnPropRowKindRole, 1);
            rowNames->setData(0, kConnIdxRole, connIdx);
            rowNames->setFlags(rowNames->flags() & ~Qt::ItemIsUserCheckable);
            auto* rowValues = new QTreeWidgetItem();
            rowValues->setData(0, kConnRootInlineFieldRole, true);
            rowValues->setData(0, kConnPropRowKindRole, 2);
            rowValues->setData(0, kConnIdxRole, connIdx);
            rowValues->setFlags((rowValues->flags() & ~Qt::ItemIsEditable) & ~Qt::ItemIsUserCheckable);
            rowValues->setSizeHint(0, QSize(0, 33));
            parent->addChild(rowNames);
            parent->addChild(rowValues);
            for (int off = 0; off < propCols; ++off) {
                const int idx = base + off;
                if (idx >= props.size()) {
                    break;
                }
                const int col = 4 + off;
                if (col >= tree->columnCount()) {
                    break;
                }
                rowNames->setData(col, kConnInlineCellUsedRole, true);
                rowValues->setData(col, kConnInlineCellUsedRole, true);
                rowNames->setText(col, props.at(idx).first);
                rowNames->setTextAlignment(col, Qt::AlignCenter);
                rowValues->setText(col, props.at(idx).second);
                rowValues->setTextAlignment(col, Qt::AlignCenter);
            }
        }
    };

    const QIcon dirIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    const QIcon linkIcon = QApplication::style()->standardIcon(QStyle::SP_FileLinkIcon);

    // Sort: dirs first, then files
    QList<FileBrowserEntry> sorted;
    for (const FileBrowserEntry& e : entries) {
        if (e.isDir) {
            sorted.prepend(e);
        } else {
            sorted.append(e);
        }
    }

    for (const FileBrowserEntry& e : sorted) {
        auto* entryItem = new QTreeWidgetItem(browserNode);
        entryItem->setText(0, e.name);
        entryItem->setIcon(0, e.isDir ? dirIcon : (e.isLink ? linkIcon : fileIcon));
        entryItem->setData(0, kConnFileBrowserNodeRole, e.isDir);
        entryItem->setData(0, kConnFileBrowserPathRole, dirPath + QStringLiteral("/") + e.name);
        entryItem->setData(0, kConnFileBrowserIsDirRole, e.isDir);
        entryItem->setData(0, kConnFileBrowserLoadedRole, false);
        entryItem->setData(0, kConnIdxRole, connIdx);
        entryItem->setFlags(entryItem->flags() & ~Qt::ItemIsUserCheckable);

        const QVector<QPair<QString, QString>> props = {
            {QStringLiteral("permisos"), e.permissions},
            {QStringLiteral("propietario"), e.owner},
            {QStringLiteral("grupo"), e.group},
            {QStringLiteral("tamaño"), e.size},
            {QStringLiteral("modificado"), e.mtime},
        };
        addInlineProps(entryItem, props);

        if (e.isDir) {
            auto* placeholder = new QTreeWidgetItem(entryItem);
            placeholder->setText(0, QStringLiteral("..."));
            placeholder->setFlags(placeholder->flags() & ~Qt::ItemIsUserCheckable);
        }
    }
}
