#include "mainwindow.h"

#include <QJsonArray>
#include <QJsonObject>
#include "agentversion.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "daemonpayload.h"

#include <QtWidgets>
#include <QRegularExpression>
#include <algorithm>
#include <functional>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::oneLine;
using mwhelpers::shSingleQuote;

} // namespace

void MainWindow::actionAdvancedBreakdown() {
    actionAdvancedBreakdown(currentConnContentSelection(m_connContentTree));
}

void MainWindow::actionAdvancedBreakdown(const DatasetSelectionContext& explicitCtx) {
    const DatasetSelectionContext curr = explicitCtx.valid ? explicitCtx : currentConnContentSelection(m_connContentTree);
    if (!curr.valid || curr.datasetName.isEmpty() || !curr.snapshotName.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_sel_ds_001"), QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced.")));
        return;
    }
    if (!requireFeature(curr.connIdx, zfsmgr::caps::Feature::DirBreakdown)) {
        return;
    }
    const QString ds = curr.datasetName;
    if (ds.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = curr.connIdx;
    ctx.poolName = curr.poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();
    beginUiBusy();
    bool busyActive = true;
    auto stopBusy = [&]() {
        if (busyActive) {
            endUiBusy();
            busyActive = false;
        }
    };
    const ConnectionProfile p = m_conns.profiles[ctx.connIdx];
    // The shell's `breakdown --list`: the directories that can become datasets —those of a
    // child dataset already left out—, read from a temporary mount if it is not mounted.
    QStringList dirs;
    const QString resolvedMp;
    {
        QJsonObject r;
        QString err;
        if (!cliReadJson(ctx.connIdx,
                         QStringLiteral("breakdown --list --on %1").arg(zfsmUrl(ctx.connIdx, ds)),
                         r, &err, 240000)) {
            stopBusy();
            QMessageBox::warning(
                this, QStringLiteral("ZFSMgr"),
                err.isEmpty() ? trk(QStringLiteral("t_adv_break_ls01"),
                                    QStringLiteral("No se pudieron listar los directorios "
                                                   "para Desglosar."),
                                    QStringLiteral("Could not list directories for breakdown."))
                              : err);
            return;
        }
        for (const QJsonValue& v : r.value(QStringLiteral("directories")).toArray()) {
            dirs << v.toObject().value(QStringLiteral("dir")).toString();
        }
    }
    // The children already there, from the tree's cache: their names are taken.
    QStringList datasetsDetected;
    QSet<QString> childDatasetPathsLower;
    {
        const QString prefix = ds + QStringLiteral("/");
        const PoolDatasetCache cache =
            m_conns.poolDatasetCache.value(datasetCacheKey(ctx.connIdx, ctx.poolName));
        for (const DatasetRecord& rec : cache.datasets) {
            if (rec.name.startsWith(prefix)) {
                datasetsDetected << rec.name;
                childDatasetPathsLower.insert(rec.name.mid(prefix.size()).toLower());
            }
        }
    }
    if (dirs.isEmpty()) {
        const QString dirsText = dirs.isEmpty()
                                     ? trk(QStringLiteral("t_none_txt_0001"), QStringLiteral("(ninguno)"), QStringLiteral("(none)"))
                                     : dirs.join('\n');
        const QString datasetsText = datasetsDetected.isEmpty()
                                         ? trk(QStringLiteral("t_none_txt_0001"), QStringLiteral("(ninguno)"), QStringLiteral("(none)"))
                                         : datasetsDetected.join('\n');
        const QString mpText = resolvedMp.isEmpty()
                                   ? trk(QStringLiteral("t_unresolved001"), QStringLiteral("(sin resolver)"), QStringLiteral("(unresolved)"))
                                   : resolvedMp;

        stopBusy();
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_break_nod1"), QStringLiteral("No hay directorios para desglosar en el dataset seleccionado.\n\n"
                                                    "Dataset: %1\n"
                                                    "Mountpoint resuelto: %2\n\n"
                                                    "Directorios detectados:\n%3\n\n"
                                                    "Datasets detectados:\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText),
                                     QStringLiteral("No directories available to break down in selected dataset.\n\n"
                                                    "Dataset: %1\n"
                                                    "Resolved mountpoint: %2\n\n"
                                                    "Detected directories:\n%3\n\n"
                                                    "Detected datasets:\n%4")
                                         .arg(ds, mpText, dirsText, datasetsText)
                                         .arg(ds, mpText, dirsText, datasetsText)));
        return;
    }
    stopBusy();
    QStringList selectedDirs;
    QMap<QString, QString> invalidDirReasons;
    auto invalidDatasetPathReason = [](const QString& path) -> QString {
        const QString p = path.trimmed();
        if (p.isEmpty()) {
            return QStringLiteral("empty");
        }
        const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            return QStringLiteral("empty");
        }
        for (const QString& rawPart : parts) {
            const QString n = rawPart.trimmed();
            if (n.isEmpty()) {
                return QStringLiteral("empty path component");
            }
            if (n == QStringLiteral(".") || n == QStringLiteral("..")) {
                return QStringLiteral("reserved name");
            }
            for (const QChar c : n) {
                const ushort uc = c.unicode();
                if (uc < 0x20 || uc == 0x7F) {
                    return QStringLiteral("control character");
                }
            }
            // ZFS solo admite [a-zA-Z0-9_.:-] y espacio en un nombre de dataset. Antes
            // aquí solo se miraban '@', '#' y ',', así que un directorio con apóstrofo o
            // con acento —«Alessandro D'Avenia», «Canción»— se dejaba seleccionar y
            // después bloqueaba el botón Aceptar sin explicar por qué. En una biblioteca
            // de Calibre eso son miles de directorios y el diálogo quedaba inservible.
            for (const QChar c : n) {
                const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                                || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                                || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                                || c == QLatin1Char('_') || c == QLatin1Char('.')
                                || c == QLatin1Char(':') || c == QLatin1Char('-')
                                || c == QLatin1Char(' ');
                if (!ok) {
                    return QStringLiteral("ZFS no admite '%1' en un nombre de dataset").arg(c);
                }
            }
        }
        return QString();
    };
    for (const QString& dir : dirs) {
        const QString reason = invalidDatasetPathReason(dir);
        if (!reason.isEmpty()) {
            invalidDirReasons.insert(dir, reason);
        }
    }
    // Nombre del dataset en el que se convertirá cada directorio. No tiene por qué
    // reflejar la ruta: `zfs create` solo exige que exista el dataset PADRE por nombre,
    // no que el directorio de encima sea un dataset. Así se puede convertir `a/b/c`
    // dejando `a` y `b` como directorios normales, con el nombre plano `a:b:c`.
    // Los tramos que sí serán dataset se separan con '/', los que no con ':' — de los
    // caracteres que ZFS admite ([a-zA-Z0-9_.:- ] y espacio) es el que menos se
    // confunde con un nombre de directorio corriente.
    // Re-edición: la semilla trae los pares (directorio, nombre) que se pidieron.
    QMap<QString, QString> seededNames;

    MainWindow::TreeNameColumn nameColumn;
    nameColumn.header = trk(QStringLiteral("t_col_dataset_001"),
                            QStringLiteral("Dataset resultante"),
                            QStringLiteral("Resulting dataset"));
    nameColumn.editable = true;
    nameColumn.takenNames = childDatasetPathsLower;
    // Los nombres que se habían elegido, para no reproponer otros encima.
    nameColumn.initialNames = seededNames;
    nameColumn.propose = [childDatasetPathsLower](const QString& path,
                                                  const QSet<QString>& checked) {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString name;
        QString prefix;
        for (int i = 0; i < parts.size(); ++i) {
            if (i == 0) {
                name = parts[0];
                prefix = parts[0];
                continue;
            }
            const bool prefixIsDataset = checked.contains(prefix)
                                         || childDatasetPathsLower.contains(prefix.toLower());
            name += (prefixIsDataset ? QStringLiteral("/") : QStringLiteral(":")) + parts[i];
            prefix += QStringLiteral("/") + parts[i];
        }
        return name;
    };

    if (!selectTreeItemsDialog(
            trk(QStringLiteral("t_adv_break_tit1"), QStringLiteral("Desglosar: seleccionar directorios"),
                QStringLiteral("Break down: select directories")),
            trk(QStringLiteral("t_adv_break_msg1"), QStringLiteral("Seleccione los directorios que desea desglosar en subdatasets."),
                QStringLiteral("Select directories to split into subdatasets.")),
            dirs,
            selectedDirs,
            trk(QStringLiteral("t_adv_break_mp001"),
                QStringLiteral("Mountpoint usado para explorar directorios: %1").arg(resolvedMp),
                QStringLiteral("Mountpoint used to scan directories: %1").arg(resolvedMp).arg(resolvedMp)),
            invalidDirReasons,
            isWindowsConnection(p) ? nullptr : &nameColumn)) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_adv_break_can1"), QStringLiteral("Desglosar cancelado o sin selección."),
                   QStringLiteral("Break down canceled or no selection.")));
        return;
    }
    for (const QString& d : selectedDirs) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("[BREAKDOWN] pendiente: %1 -> %2")
                                             .arg(d, nameColumn.chosen.value(d, d)));
    }

    if (isWindowsConnection(p)) {
        QMessageBox::information(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_desglosar_no_win_001"),
                QStringLiteral("Desglosar no está disponible en conexiones Windows: allí los "
                               "datasets se montan bajo la letra de unidad del pool y no "
                               "admiten un punto de montaje propio, así que el dataset nuevo "
                               "no quedaría donde está el directorio."),
                QStringLiteral("Break down is not available on Windows connections: datasets "
                               "there mount under the pool's drive letter and cannot have "
                               "their own mountpoint, so the new dataset would not end up "
                               "where the directory is.")));
        return;
    }
    // The shell's `breakdown`, in pairs: each directory with the dataset that replaces it. It
    // runs as a job of the daemon; the Jobs box follows it.
    QStringList linea{QStringLiteral("breakdown")};
    for (const QString& d : selectedDirs) {
        linea << cliQuote(d) << cliQuote(nameColumn.chosen.value(d, d));
    }
    linea << QStringLiteral("--on") << zfsmUrl(ctx.connIdx, ds);
    runCli(linea.join(QLatin1Char(' ')), [this, ctx](const CliService::Result& r) {
        if (r.rc == 0 && m_refreshAfterCli) {
            refreshAllJobsAsync();
            refreshAfterCli(ctx);
        }
    });
}

void MainWindow::actionAdvancedAssemble() {
    actionAdvancedAssemble(currentConnContentSelection(m_connContentTree));
}

void MainWindow::actionAdvancedAssemble(const DatasetSelectionContext& explicitCtx) {
    const DatasetSelectionContext curr = explicitCtx.valid ? explicitCtx : currentConnContentSelection(m_connContentTree);
    if (!curr.valid || curr.datasetName.isEmpty() || !curr.snapshotName.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_sel_ds_001"), QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced.")));
        return;
    }
    if (!requireFeature(curr.connIdx, zfsmgr::caps::Feature::DirAssemble)) {
        return;
    }
    const QString ds = curr.datasetName;
    if (ds.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = curr.connIdx;
    ctx.poolName = curr.poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();
    beginUiBusy();
    bool busyActive = true;
    auto stopBusy = [&]() {
        if (busyActive) {
            endUiBusy();
            busyActive = false;
        }
    };
    // Its descendants, from the shell's `ls -r`; whether it can be done unmounted, the shell
    // says when it runs.
    QStringList children;
    {
        QJsonObject r;
        QString err;
        if (!cliReadJson(ctx.connIdx, QStringLiteral("ls -r %1").arg(zfsmUrl(ctx.connIdx, ds)), r,
                         &err, 120000)) {
            stopBusy();
            QMessageBox::warning(
                this, QStringLiteral("ZFSMgr"),
                err.isEmpty()
                    ? trk(QStringLiteral("t_adv_ass_list01"),
                          QStringLiteral("No se pudieron listar subdatasets para ensamblar."),
                          QStringLiteral("Could not list child datasets for assemble."))
                    : err);
            return;
        }
        for (const QJsonValue& v : r.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString nombre = o.value(QStringLiteral("name")).toString();
            if (o.value(QStringLiteral("type")).toString() != QStringLiteral("snapshot")
                && nombre != ds && !nombre.isEmpty()) {
                children << nombre;
            }
        }
    }
    if (children.isEmpty()) {
        stopBusy();
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_adv_ass_none01"), QStringLiteral("No hay subdatasets para ensamblar."),
                                     QStringLiteral("No child datasets available to assemble.")));
        return;
    }
    stopBusy();
    QStringList childPaths;
    childPaths.reserve(children.size());
    QMap<QString, QString> childPathToDataset;
    const QString childPrefix = ds + QStringLiteral("/");
    for (const QString& childDataset : children) {
        if (!childDataset.startsWith(childPrefix)) {
            continue;
        }
        const QString rel = childDataset.mid(childPrefix.size()).trimmed();
        if (rel.isEmpty()) {
            continue;
        }
        childPaths.push_back(rel);
        childPathToDataset.insert(rel, childDataset);
    }
    QStringList selectedChildPaths;
    // Columna informativa: qué dataset es cada fila. Aquí el nombre es un hecho, no una
    // propuesta, así que no se edita. Tras absorberlo, los datasets que cuelguen de él
    // se conservan y pasan a llamarse por su ruta bajo el padre con ':' en lugar de '/'.

    MainWindow::TreeNameColumn assembleNames;
    assembleNames.header = trk(QStringLiteral("t_col_dataset_002"),
                               QStringLiteral("Dataset actual"),
                               QStringLiteral("Current dataset"));
    assembleNames.editable = false;
    assembleNames.propose = [childPathToDataset](const QString& path, const QSet<QString>&) {
        return childPathToDataset.value(path);
    };

    if (!selectTreeItemsDialog(
            trk(QStringLiteral("t_adv_ass_tit001"), QStringLiteral("Ensamblar: seleccionar subdatasets"),
                QStringLiteral("Assemble: select child datasets")),
            trk(QStringLiteral("t_adv_ass_msg001"), QStringLiteral("Seleccione los subdatasets que desea ensamblar en el dataset padre."),
                QStringLiteral("Select child datasets to assemble into parent dataset.")),
            childPaths,
            selectedChildPaths,
            QString(),
            {},
            &assembleNames)) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_adv_ass_can001"), QStringLiteral("Ensamblar cancelado o sin selección."),
                   QStringLiteral("Assemble canceled or no selection.")));
        return;
    }
    QStringList selectedChildren;
    selectedChildren.reserve(selectedChildPaths.size());
    for (const QString& rel : selectedChildPaths) {
        const QString full = childPathToDataset.value(rel).trimmed();
        if (!full.isEmpty()) {
            selectedChildren.push_back(full);
        }
    }
    if (selectedChildren.isEmpty()) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_adv_ass_can001"), QStringLiteral("Ensamblar cancelado o sin selección."),
                   QStringLiteral("Assemble canceled or no selection.")));
        return;
    }
    for (const QString& child : selectedChildren) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("[ASSEMBLE] pendiente: %1").arg(child));
    }
    // The shell's `assemble`: the children go back to being directories, as a job.
    QStringList linea{QStringLiteral("assemble")};
    for (const QString& child : std::as_const(selectedChildren)) {
        linea << cliQuote(child);
    }
    linea << QStringLiteral("--on") << zfsmUrl(ctx.connIdx, ds);
    runCli(linea.join(QLatin1Char(' ')), [this, ctx](const CliService::Result& r) {
        if (r.rc == 0 && m_refreshAfterCli) {
            refreshAllJobsAsync();
            refreshAfterCli(ctx);
        }
    });
}

