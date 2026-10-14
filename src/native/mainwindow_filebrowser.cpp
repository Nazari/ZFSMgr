#include <functional>
#include "mainwindow.h"

#include <QJsonArray>
#include <QJsonObject>
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
// Los valores crudos de la entrada (permisos, propietario, grupo, tamaño, modificado).
// Se guardan porque las columnas de propiedades se pueden ampliar DESPUÉS de haber
// cargado el directorio, y sin el original no habría con qué rellenar las nuevas.
constexpr int kConnFileBrowserPropsRole = Qt::UserRole + 57;

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
    if (dirPath.isEmpty() || connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }

    const QSignalBlocker blocker(tree);
    while (browserNode->childCount() > 0) {
        delete browserNode->takeChild(0);
    }

    // The shell's `ls zfsm://<conn>#path/<dir>`: the same listing as at the shell, from the
    // daemon, on every platform.
    QJsonObject r;
    QString err;
    beginTransientUiBusy(QStringLiteral("Leyendo contenido..."));
    const bool leido = cliReadJson(connIdx, QStringLiteral("ls %1").arg(pathUrl(connIdx, dirPath)),
                                   r, &err, 30000);
    endTransientUiBusy();
    if (!leido || !err.isEmpty()) {
        auto* errItem = new QTreeWidgetItem(browserNode);
        errItem->setText(
            0, QStringLiteral("(error: %1)").arg(err.isEmpty() ? QStringLiteral("?") : err));
        errItem->setFlags(errItem->flags() & ~Qt::ItemIsUserCheckable);
        return;
    }
    QList<FileBrowserEntry> entries;
    for (const QJsonValue& v : r.value(QStringLiteral("entries")).toArray()) {
        const QJsonObject o = v.toObject();
        FileBrowserEntry e;
        e.name = o.value(QStringLiteral("name")).toString();
        const QString tipo = o.value(QStringLiteral("type")).toString();
        e.isDir = tipo == QStringLiteral("directory");
        e.isLink = tipo == QStringLiteral("symlink");
        e.permissions = o.value(QStringLiteral("mode")).toString();
        e.owner = o.value(QStringLiteral("owner")).toString();
        e.group = o.value(QStringLiteral("group")).toString();
        const QJsonValue tam = o.value(QStringLiteral("size"));
        e.size =
            tam.isDouble() ? QString::number(static_cast<qint64>(tam.toDouble())) : tam.toString();
        e.mtime = o.value(QStringLiteral("modified")).toString();
        if (!e.name.isEmpty()) {
            entries.push_back(e);
        }
    }

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

        writeFileBrowserPropCells(tree, entryItem,
                                  {e.permissions, e.owner, e.group, e.size, e.mtime});

        if (e.isDir) {
            auto* placeholder = new QTreeWidgetItem(entryItem);
            placeholder->setText(0, QStringLiteral("..."));
            placeholder->setFlags(placeholder->flags() & ~Qt::ItemIsUserCheckable);
        }
    }
}

// En qué columna empiezan las cinco propiedades de un fichero.
//
// En el árbol principal iban de la 4 en adelante, que eran las columnas C1, C2...
// compartidas con los datasets y sin rótulo propio. La pestaña «Contenido» del detalle
// tiene columnas suyas, con nombre, y empiezan en la 1: lo dice con una propiedad del
// árbol en vez de con un número repartido por dos sitios.
//
// Lo que no cabe en las columnas NO se pierde: sigue en el tooltip de la fila.
void MainWindow::writeFileBrowserPropCells(QTreeWidget* tree, QTreeWidgetItem* item,
                                           const QStringList& values) {
    if (!tree || !item) {
        return;
    }
    const QStringList propLabels = {
        trk(QStringLiteral("t_fb_perms_001"), QStringLiteral("permisos"),
            QStringLiteral("permissions")),
        trk(QStringLiteral("t_fb_owner_001"), QStringLiteral("propietario"),
            QStringLiteral("owner")),
        trk(QStringLiteral("t_fb_group_001"), QStringLiteral("grupo"),
            QStringLiteral("group")),
        trk(QStringLiteral("t_fb_size_001"), QStringLiteral("tamaño"),
            QStringLiteral("size")),
        trk(QStringLiteral("t_fb_mtime_001"), QStringLiteral("modificado"),
            QStringLiteral("modified")),
    };

    item->setData(0, kConnFileBrowserPropsRole, values);

    QStringList tip;
    for (int i = 0; i < values.size() && i < propLabels.size(); ++i) {
        tip << QStringLiteral("%1: %2").arg(propLabels.at(i), values.at(i));
    }
    item->setToolTip(0, tip.join(QLatin1Char('\n')));
    for (int i = 0; i < values.size(); ++i) {
        const QVariant base = tree->property("zfsmgr.fbPropBaseColumn");
        const int col = (base.isValid() ? base.toInt() : 4) + i;
        if (col >= tree->columnCount()) {
            break;  // lo que no cabe sigue estando en el tooltip de la fila
        }
        item->setText(col, values.at(i));
        item->setToolTip(col, QStringLiteral("%1: %2")
                                  .arg(propLabels.value(i), values.at(i)));
        item->setTextAlignment(col, Qt::AlignLeft | Qt::AlignVCenter);
    }
}
