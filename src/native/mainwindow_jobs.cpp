#include "mainwindow.h"

#include "base/agentmsg.h"
#include "base/json.h"
#include "base/strutil.h"
#include <QJsonArray>
#include <QJsonObject>

#include <QHeaderView>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QtConcurrent/QtConcurrent>

// The Jobs box: the jobs of EVERY connection with a daemon, as the shell's `jobs -a` lists
// them, plus the ones this window launched (kept with their final state). It used to show only
// the window's own jobs, so a levelling of a schedule or a send started from the shell never
// appeared.

namespace {

bool estaActivo(const QString& estado) {
    return estado == QStringLiteral("running") || estado == QStringLiteral("queued");
}

}  // namespace

void MainWindow::refreshAllJobsAsync() {
    QVector<QPair<int, ConnectionProfile>> perfiles;
    for (int i = 0; i < m_conns.profiles.size() && i < m_conns.states.size(); ++i) {
        if (m_conns.states[i].daemonActive && !isConnectionDisconnected(i)) {
            perfiles.push_back({i, m_conns.profiles[i]});
        }
    }
    (void)QtConcurrent::run([this, perfiles]() {
        QVector<DaemonJobRow> filas;
        bool alguna = false;
        for (const auto& par : perfiles) {
            if (m_destroying.load()) {
                return;
            }
            // The shell's `jobs --history`: every job of that daemon, with its error worded.
            QJsonObject respuesta;
            if (!cliReadJson(par.first,
                             QStringLiteral("jobs --history --on %1").arg(zfsmUrl(par.first)),
                             respuesta, nullptr, 15000)) {
                continue;
            }
            alguna = true;
            for (const QJsonValue& v : respuesta.value(QStringLiteral("jobs")).toArray()) {
                const QJsonObject j = v.toObject();
                DaemonJobRow f;
                f.connIdx = par.first;
                f.connName = par.second.name;
                f.id = j.value(QStringLiteral("id")).toString();
                f.state = j.value(QStringLiteral("state")).toString();
                f.type = j.value(QStringLiteral("type")).toString();
                f.snap = j.value(QStringLiteral("snap")).toString();
                f.bytes = static_cast<qulonglong>(j.value(QStringLiteral("bytes")).toDouble());
                f.rate = j.value(QStringLiteral("rate")).toVariant().toDouble();
                f.elapsed = j.value(QStringLiteral("elapsed")).toInt();
                f.error = j.value(QStringLiteral("error")).toString();
                filas.push_back(f);
            }
        }
        QMetaObject::invokeMethod(this, [this, filas, alguna]() {
            // A job that was running and is not any more changed something: its machine
            // is read again, as the window did when it followed its own jobs.
            QSet<int> terminadas;
            QHash<QString, QString> estadoNuevo;
            for (const DaemonJobRow& f : filas) {
                estadoNuevo.insert(f.id, f.state);
            }
            for (const DaemonJobRow& antes : std::as_const(m_allJobs)) {
                if (estaActivo(antes.state) && !estaActivo(estadoNuevo.value(antes.id))
                    && antes.connIdx >= 0) {
                    terminadas.insert(antes.connIdx);
                }
            }
            m_allJobs = filas;
            for (int conn : std::as_const(terminadas)) {
                if (conn < m_conns.profiles.size()) {
                    refreshConnectionByIndex(conn);
                }
            }
            int activos = 0;
            for (const DaemonJobRow& f : filas) {
                activos += estaActivo(f.state) ? 1 : 0;
            }
            m_statusJobsRunning = alguna ? activos : -1;
            if (m_allJobsTimer) {
                m_allJobsTimer->setInterval(activos > 0 ? 2500 : 30000);
            }
            updateJobsListWidget();
            composeStatusBar();
        });
    });
}

void MainWindow::updateJobsListWidget() {
    if (!m_jobsListWidget) {
        return;
    }
    // The marked row is remembered by connection and job id, not by row number: between two
    // refreshes the list may have changed order or lost a job that ended.
    QString marcado;
    if (QTreeWidgetItem* actual = m_jobsListWidget->currentItem()) {
        marcado = actual->data(0, Qt::UserRole).toString();
    }
    // Active jobs of every daemon, then this window's own jobs not already listed.
    QVector<DaemonJobRow> filas;
    QSet<QString> vistos;
    for (const DaemonJobRow& f : std::as_const(m_allJobs)) {
        if (estaActivo(f.state)) {
            filas.push_back(f);
            vistos.insert(f.id);
        }
    }
    const QSignalBlocker sinSenales(m_jobsListWidget);
    m_jobsListWidget->clear();
    for (const DaemonJobRow& f : std::as_const(filas)) {
        auto* item = new QTreeWidgetItem(m_jobsListWidget);
        item->setText(0, f.connName);
        item->setText(1, f.id);
        item->setText(2, f.state);
        item->setText(3, f.type);
        item->setText(4, f.snap);
        item->setText(5, QString::fromStdString(zfsmgr::base::humanSize(std::to_string(f.bytes))));
        item->setText(6, QString::number(f.rate, 'f', 1));
        item->setText(7, QString::number(f.elapsed));
        item->setText(8, f.error);
        item->setToolTip(8, f.error);
        item->setData(0, Qt::UserRole, f.id);
        item->setData(0, Qt::UserRole + 1, f.connIdx);
        if (!marcado.isEmpty() && f.id == marcado) {
            m_jobsListWidget->setCurrentItem(item);
        }
    }
}

void MainWindow::cancelSelectedJob() {
    if (!m_jobsListWidget) {
        return;
    }
    QTreeWidgetItem* item = m_jobsListWidget->currentItem();
    if (!item) {
        return;
    }
    const QString jobId = item->data(0, Qt::UserRole).toString();
    const int connIdx = item->data(0, Qt::UserRole + 1).toInt();
    if (jobId.isEmpty() || connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }
    runCli(QStringLiteral("job cancel %1 --on %2").arg(jobId, zfsmUrl(connIdx)),
           [this](const CliService::Result& r) {
               if (r.rc != 0) {
                   return;
               }
               if (m_refreshAfterCli) {
                   refreshAllJobsAsync();
               }
           });
}
