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

    const ConnectionProfile prof = m_profiles[connIdx];
    const QString browserScript = QStringLiteral(
                                      "p=%1; "
                                      "if [ -d \"$p\" ]; then "
                                      "  ls -lA \"$p\" 2>&1; "
                                      "elif [ -e \"$p\" ]; then "
                                      "  echo \"not a directory: $p\" >&2; exit 2; "
                                      "else "
                                      "  echo \"path not found: $p\" >&2; exit 3; "
                                      "fi")
                                      .arg(shSingleQuote(dirPath));
    const QString remoteCmdRaw = QStringLiteral("sh -lc %1").arg(shSingleQuote(browserScript));
    const QString remoteCmd = withSudo(prof, remoteCmdRaw);

    QString out;
    QString err;
    int rc = -1;
    beginTransientUiBusy(QStringLiteral("Leyendo contenido..."));
    const bool ran = runSsh(prof, remoteCmd, 20000, out, err, rc);
    endTransientUiBusy();

    if (!ran || rc != 0) {
        auto* errItem = new QTreeWidgetItem(browserNode);
        // Con err y out vacíos el mensaje quedaba en "(error: )", que no dice nada de
        // nada. Al menos el código de salida acota si fue la orden o el transporte.
        const QString detail = !err.trimmed().isEmpty()   ? err.trimmed()
                               : !out.trimmed().isEmpty() ? out.trimmed()
                                                          : QStringLiteral("sin detalle (código %1)").arg(rc);
        errItem->setText(0, QStringLiteral("(error: %1)").arg(detail));
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

    // Las propiedades se escriben en las columnas 4 en adelante. Si el árbol no tiene
    // tantas, el bucle de abajo sale sin escribir NADA y quedan dos filas vacías bajo
    // cada entrada: se ve el triángulo, se expande, y no hay nada. El qMax(1,...) de
    // arriba tapa ese caso, así que se deja dicho en el registro.
    // Las propiedades se escriben a partir de la columna 4. Si no hay tantas no se
    // pierden —siguen en el tooltip de la fila— pero conviene que conste.
    if (tree->columnCount() <= 4) {
        appLog(QStringLiteral("WARN"),
               QStringLiteral("[contenido] el árbol solo tiene %1 columnas; las propiedades "
                              "de fichero solo se verán en el tooltip")
                   .arg(tree->columnCount()));
    }

    // Las propiedades van en la PROPIA fila de cada entrada, no colgando de ella.
    // Antes se añadían como dos filas hijas (nombres y valores) a todas las entradas,
    // ficheros incluidos: eso daba triángulo a los ficheros —que no tienen nada dentro—
    // y obligaba a expandir para ver algo tan básico como el tamaño. Ahora el triángulo
    // significa exactamente una cosa: que se puede entrar.
    //
    // Las columnas 4 en adelante no tienen rótulo fijo (son C1, C2, ... compartidas con
    // los datasets), así que cada celda lleva el nombre de la propiedad en su tooltip y
    // la fila entera lo lleva completo.
    const QStringList propLabels = {
        trk(QStringLiteral("t_fb_perms_001"), QStringLiteral("permisos"),
            QStringLiteral("permissions"), QStringLiteral("权限")),
        trk(QStringLiteral("t_fb_owner_001"), QStringLiteral("propietario"),
            QStringLiteral("owner"), QStringLiteral("所有者")),
        trk(QStringLiteral("t_fb_group_001"), QStringLiteral("grupo"),
            QStringLiteral("group"), QStringLiteral("组")),
        trk(QStringLiteral("t_fb_size_001"), QStringLiteral("tamaño"),
            QStringLiteral("size"), QStringLiteral("大小")),
        trk(QStringLiteral("t_fb_mtime_001"), QStringLiteral("modificado"),
            QStringLiteral("modified"), QStringLiteral("修改时间")),
    };
    auto setEntryProps = [&](QTreeWidgetItem* item, const QStringList& values) {
        QStringList tip;
        for (int i = 0; i < values.size() && i < propLabels.size(); ++i) {
            tip << QStringLiteral("%1: %2").arg(propLabels.at(i), values.at(i));
        }
        item->setToolTip(0, tip.join(QLatin1Char('\n')));
        for (int i = 0; i < values.size(); ++i) {
            const int col = 4 + i;
            if (col >= tree->columnCount()) {
                break;  // lo que no cabe sigue estando en el tooltip de la fila
            }
            item->setText(col, values.at(i));
            item->setToolTip(col, QStringLiteral("%1: %2")
                                      .arg(propLabels.value(i), values.at(i)));
            item->setTextAlignment(col, Qt::AlignLeft | Qt::AlignVCenter);
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

        setEntryProps(entryItem, {e.permissions, e.owner, e.group, e.size, e.mtime});

        if (e.isDir) {
            auto* placeholder = new QTreeWidgetItem(entryItem);
            placeholder->setText(0, QStringLiteral("..."));
            placeholder->setFlags(placeholder->flags() & ~Qt::ItemIsUserCheckable);
        }
    }
}
