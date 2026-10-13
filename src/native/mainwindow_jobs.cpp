#include "mainwindow.h"

#include "base/agentmsg.h"
#include "base/json.h"
#include "base/strutil.h"
#include "commands/requests.h"

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
            QString out;
            QString err;
            int rc = -1;
            if (!runAgentOnConnection(par.second,
                                      mwhelpers::argvQt(zfsmgr::commands::requests::jobList()),
                                      5000, out, err, rc)
                || rc != 0) {
                continue;
            }
            alguna = true;
            for (const QString& linea : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (!linea.startsWith(QStringLiteral("JOB="))) {
                    continue;
                }
                zfsmgr::base::json::Value j;
                std::string e;
                if (!zfsmgr::base::json::parse(linea.mid(4).toStdString(), j, &e)) {
                    continue;
                }
                DaemonJobRow f;
                f.connIdx = par.first;
                f.connName = par.second.name;
                f.id = QString::fromStdString(j["id"].toString());
                f.state = QString::fromStdString(j["state"].toString());
                f.type = QString::fromStdString(j["type"].toString());
                f.snap = QString::fromStdString(j["snap"].toString());
                f.bytes = static_cast<qulonglong>(j["bytes"].toInt(0));
                f.rate = j["rate"].toDouble();
                f.elapsed = j["elapsed"].toInt(0);
                f.error = QString::fromStdString(zfsmgr::base::agentmsg::renderJsonArgs(
                    j["error_code"].toString(), zfsmgr::base::json::toCompact(j["error_args"])));
                filas.push_back(f);
            }
        }
        QMetaObject::invokeMethod(this, [this, filas, alguna]() {
            m_allJobs = filas;
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
    for (const ActiveDaemonJob& job : std::as_const(m_activeDaemonJobs)) {
        if (vistos.contains(job.jobId)) {
            continue;
        }
        DaemonJobRow f;
        f.connIdx = job.srcConnIdx;
        f.connName = (job.srcConnIdx >= 0 && job.srcConnIdx < m_conns.profiles.size())
                         ? m_conns.profiles[job.srcConnIdx].name
                         : QString();
        f.id = job.jobId;
        f.state = job.state;
        f.type = QStringLiteral("send-to-peer");
        f.snap = job.displayLabel;
        f.bytes = job.bytesTransferred;
        f.rate = job.rateMiBs;
        f.elapsed = job.elapsedSecs;
        f.error = job.errorText;
        filas.push_back(f);
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
    QString out;
    QString err;
    int rc = -1;
    runAgentOnConnection(
        m_conns.profiles[connIdx],
        mwhelpers::argvQt(zfsmgr::commands::requests::cancelJob(jobId.toStdString())), 5000, out,
        err, rc);
    if (rc != 0) {
        appLog(QStringLiteral("ERROR"), QStringLiteral("job %1: %2").arg(jobId, err.trimmed()));
        return;
    }
    for (ActiveDaemonJob& j : m_activeDaemonJobs) {
        if (j.jobId == jobId) {
            j.state = QStringLiteral("cancelled");
            break;
        }
    }
    appLog(QStringLiteral("INFO"), QStringLiteral("Job %1 cancelado por el usuario").arg(jobId));
    updateJobsListWidget();
    refreshAllJobsAsync();
}
