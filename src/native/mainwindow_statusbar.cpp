#include "mainwindow.h"

#include "agentversion.h"
#include "base/agentversion.h"
#include "commands/gsa.h"
#include "commands/requests.h"

#include <QLabel>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

#include <ctime>

// The status bar is the shell's status area: the same pieces, in the same order, built from
// the same core rules (gsa::soonestNext, agentversion::verdictFor). Only the drawing is the
// window's.

namespace {

// A time, short: «10:00» if it is today, otherwise with its date. In local time.
QString horaCorta(long long cuando) {
    const std::time_t t = static_cast<std::time_t>(cuando);
    const std::time_t hoy = std::time(nullptr);
    std::tm a{};
    std::tm b{};
#ifdef _WIN32
    localtime_s(&a, &t);
    localtime_s(&b, &hoy);
#else
    localtime_r(&t, &a);
    localtime_r(&hoy, &b);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf),
                  (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) ? "%H:%M" : "%Y-%m-%d %H:%M",
                  &a);
    return QString::fromLatin1(buf);
}

}  // namespace

void MainWindow::buildStatusBar() {
    // The bar belongs to the QMainWindow, not to the central widget, so it survives the
    // rebuild that a language change does: only its label is created once.
    if (!m_statusBarLabel) {
        m_statusBarLabel = new QLabel(this);
        m_statusBarLabel->setObjectName(QStringLiteral("statusBarLine"));
        m_statusBarLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusBar()->addWidget(m_statusBarLabel, 1);
    }
    if (!m_statusScheduleTimer) {
        m_statusScheduleTimer = new QTimer(this);
        m_statusScheduleTimer->setInterval(5 * 60 * 1000);
        connect(m_statusScheduleTimer, &QTimer::timeout, this,
                [this]() { refreshStatusBarSchedulesAsync(); });
        m_statusScheduleTimer->start();
    }
    composeStatusBar();
}

void MainWindow::composeStatusBar() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { composeStatusBar(); }, Qt::QueuedConnection);
        return;
    }
    if (!m_statusBarLabel) {
        return;
    }
    QStringList partes;
    if (!m_statusActivity.isEmpty()) {
        partes << m_statusActivity;
    }
    // The next scheduled snapshot, or the count of schedules in error.
    namespace G = zfsmgr::base::gsa;
    if (!m_statusSchedules.empty()) {
        std::vector<std::pair<std::string, G::DatasetState>> mal;
        std::vector<const G::ConnectionState*> sinComprobar;
        const G::Verdict v = G::summarize(m_statusSchedules, mal, sinComprobar);
        if (v == G::Verdict::NoSchedules) {
            partes << trk(QStringLiteral("t_est_prog_ninguna"),
                          QStringLiteral("sin programaciones"), QStringLiteral("no schedules"));
        } else {
            if (v == G::Verdict::WithErrors) {
                partes << trk(QStringLiteral("t_est_prog_mal"),
                              QStringLiteral("programaciones con errores: %1"),
                              QStringLiteral("schedules with errors: %1"))
                              .replace(QStringLiteral("%1"), QString::number(mal.size()));
            }
            std::string conexion;
            std::string dataset;
            const long long proxima = G::soonestNext(m_statusSchedules, conexion, dataset);
            if (proxima > 0) {
                partes << trk(QStringLiteral("t_est_prog_siguiente"),
                              QStringLiteral("siguiente: %1 %2/%3"),
                              QStringLiteral("next: %1 %2/%3"))
                              .replace(QStringLiteral("%1"), horaCorta(proxima))
                              .replace(QStringLiteral("%2"), QString::fromStdString(conexion))
                              .replace(QStringLiteral("%3"), QString::fromStdString(dataset));
            }
        }
    }
    // Daemons older than this client: what «install-daemon -a» would update.
    const std::string esperada = zfsmgr::base::agentversion::expected();
    QStringList viejos;
    for (int i = 0; i < m_conns.profiles.size() && i < m_conns.states.size(); ++i) {
        const QString version = m_conns.states[i].daemonVersion.trimmed();
        if (!version.isEmpty()
            && zfsmgr::base::agentversion::verdictFor(version.toStdString(), esperada)
                   == zfsmgr::base::agentversion::Verdict::Update) {
            viejos << m_conns.profiles[i].name;
        }
    }
    if (!viejos.isEmpty()) {
        partes << trk(QStringLiteral("t_est_daemon_viejo"), QStringLiteral("daemon anticuado: %1"),
                      QStringLiteral("outdated daemon: %1"))
                      .replace(QStringLiteral("%1"), viejos.join(QStringLiteral(", ")));
    }
    if (m_statusJobsRunning == 0) {
        partes << trk(QStringLiteral("t_est_sin_trabajos"), QStringLiteral("sin trabajos en curso"),
                      QStringLiteral("no jobs running"));
    } else if (m_statusJobsRunning > 0) {
        partes << trk(QStringLiteral("t_est_trabajos_n"), QStringLiteral("trabajos en curso: %1"),
                      QStringLiteral("jobs running: %1"))
                      .replace(QStringLiteral("%1"), QString::number(m_statusJobsRunning));
    }
    if (!m_statusLastError.isEmpty()) {
        partes << QStringLiteral("✗ ") + m_statusLastError;
    }
    m_statusBarLabel->setText(partes.join(QStringLiteral(" · ")));
}

void MainWindow::refreshStatusBarSchedulesAsync() {
    QVector<QPair<int, QString>> perfiles;
    for (int i = 0; i < m_conns.profiles.size() && i < m_conns.states.size(); ++i) {
        if (m_conns.states[i].daemonActive && !isConnectionDisconnected(i)) {
            const ConnectionProfile& p = m_conns.profiles[i];
            perfiles.push_back({i, p.name.isEmpty() ? p.id : p.name});
        }
    }
    if (perfiles.isEmpty()) {
        return;
    }
    // One task for all of them; the window waits for it when it is destroyed (~MainWindow).
    (void)QtConcurrent::run([this, perfiles]() {
        std::vector<zfsmgr::base::gsa::ConnectionState> estados;
        for (const auto& par : perfiles) {
            if (m_destroying.load()) {
                return;
            }
            zfsmgr::base::gsa::ConnectionState c;
            readScheduleStatus(par.first, c);
            c.connection = par.second.toStdString();
            estados.push_back(std::move(c));
        }
        QMetaObject::invokeMethod(this, [this, estados]() {
            m_statusSchedules = estados;
            composeStatusBar();
        });
    });
}
