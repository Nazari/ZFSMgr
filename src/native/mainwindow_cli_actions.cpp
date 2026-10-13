#include "mainwindow.h"

#include <QDialog>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QLineEdit>

// The window's actions on datasets and snapshots, as the shell's commands. What used to be
// built here as `zfs …` strings wrapped in sudo is now one line of the shell each.

CliService::Result MainWindow::runCliAndWait(const QString& line, const QString& format) {
    CliService::Result res;
    bool hecho = false;
    QEventLoop espera;
    runCli(
        line,
        [&](const CliService::Result& r) {
            res = r;
            hecho = true;
            espera.quit();
        },
        format);
    if (!hecho) {
        espera.exec();
    }
    return res;
}

bool MainWindow::cliRunOn(const DatasetSelectionContext& ctx, const QString& verb,
                          const QString& rest) {
    if (!ctx.valid || ctx.connIdx < 0) {
        return false;
    }
    QString linea = verb;
    if (!rest.trimmed().isEmpty()) {
        linea += QLatin1Char(' ') + rest.trimmed();
    }
    linea += QStringLiteral(" --on ") + zfsmUrl(ctx.connIdx, ctx.datasetName, ctx.snapshotName);
    const CliService::Result r = runCliAndWait(linea);
    if (r.rc == 0) {
        refreshAfterCli(ctx);
    }
    return r.rc == 0;
}

void MainWindow::cliDestroy(const DatasetSelectionContext& ctx, const QString& flags) {
    cliRunOn(ctx, QStringLiteral("destroy"), flags);
}

// A hold needs its name: that is the window asking for data, not deciding anything.
void MainWindow::cliHold(const DatasetSelectionContext& ctx) {
    bool ok = false;
    const QString tag =
        QInputDialog::getText(
            this, trk(QStringLiteral("t_new_hold_title001"), QStringLiteral("Nuevo Hold")),
            trk(QStringLiteral("t_new_hold_prompt001"), QStringLiteral("Nombre del hold")),
            QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (ok && !tag.isEmpty()) {
        cliRunOn(ctx, QStringLiteral("hold"), tag);
    }
}

MainWindow::DatasetSelectionContext MainWindow::selectionFor(int connIdx, const QString& dataset,
                                                             const QString& snapshot) {
    DatasetSelectionContext c;
    c.valid = connIdx >= 0 && !dataset.isEmpty();
    c.connIdx = connIdx;
    c.poolName = dataset.section(QLatin1Char('/'), 0, 0);
    c.datasetName = dataset;
    c.snapshotName = snapshot;
    return c;
}

bool MainWindow::mountDatasetForTest(int connIdx, const QString& ds) {
    return mountDataset(QStringLiteral("conncontent"), selectionFor(connIdx, ds, QString()));
}

void MainWindow::destroyForTest(int connIdx, const QString& ds, const QString& snap,
                                const QString& flags) {
    cliDestroy(selectionFor(connIdx, ds, snap), flags);
}

bool MainWindow::renameForTest(int connIdx, const QString& src, const QString& target) {
    return runDatasetRenameNow(
        PendingDatasetRenameDraft{connIdx, src.section(QLatin1Char('/'), 0, 0), src, target},
        nullptr);
}

void MainWindow::releaseHoldForTest(int connIdx, const QString& ds, const QString& snap,
                                    const QString& tag) {
    releaseSnapshotHoldNamed(connIdx, ds.section(QLatin1Char('/'), 0, 0), ds, snap, tag);
}

bool MainWindow::cliPool(int connIdx, const QString& pool, const QString& verb,
                         const QString& rest) {
    QString linea = verb;
    if (!rest.trimmed().isEmpty()) {
        linea += QLatin1Char(' ') + rest.trimmed();
    }
    linea += QStringLiteral(" --on ") + zfsmUrl(connIdx, pool);
    const bool ok = runCliAndWait(linea).rc == 0;
    if (ok && m_refreshAfterCli) {
        refreshConnectionByIndex(connIdx);
    }
    return ok;
}

bool MainWindow::cliImportPool(int connIdx, const QString& pool, const QString& newName,
                               bool force) {
    // By GUID when the listing has it: two importable pools may share a name, and
    // `zpool import` takes either.
    QString cual = pool;
    const QString conexion = (connIdx >= 0 && connIdx < m_conns.profiles.size())
                                 ? m_conns.profiles[connIdx].name
                                 : QString();
    for (const PoolListEntry& pe : std::as_const(m_conns.poolListEntries)) {
        if (pe.connection == conexion && pe.pool == pool && !pe.guid.trimmed().isEmpty()) {
            cual = pe.guid.trimmed();
            break;
        }
    }
    QString linea = QStringLiteral("import ") + cual;
    if (!newName.isEmpty()) {
        linea += QStringLiteral(" --as ") + newName;
    }
    if (force) {
        linea += QStringLiteral(" -f");
    }
    linea += QStringLiteral(" --on ") + zfsmUrl(connIdx);
    const bool ok = runCliAndWait(linea).rc == 0;
    if (ok && m_refreshAfterCli) {
        refreshConnectionByIndex(connIdx);
    }
    return ok;
}

void MainWindow::cliShowOutput(const QString& line, const QString& title) {
    // Text, as the shell prints it: the window does not reformat what it does not decide.
    const CliService::Result salida = runCliAndWait(line, QStringLiteral("text"));
    if (salida.rc == 0) {
        showCliText(title, salida.out);
    }
}

void MainWindow::showCliText(const QString& title, const QString& text) {
    if (text.trimmed().isEmpty()) {
        return;
    }
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(title);
    auto* lay = new QVBoxLayout(dlg);
    auto* vista = new QPlainTextEdit(dlg);
    vista->setReadOnly(true);
    vista->setLineWrapMode(QPlainTextEdit::NoWrap);
    vista->setFont(m_logView ? m_logView->font() : font());
    vista->setPlainText(text);
    lay->addWidget(vista);
    dlg->resize(900, 500);
    dlg->show();
}
