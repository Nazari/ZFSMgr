#include "statestore.h"

#include "cliservice.h"

#include <QDateTime>
#include <QTimer>

namespace zfsmgr::gui {

namespace {
constexpr int kAutoRefreshMs = 60 * 1000;
}

StateStore::StateStore(CliService* general, std::function<CliService*(const QString&)> forMachine,
                       QObject* parent)
    : QObject(parent), m_general(general), m_forMachine(std::move(forMachine)) {
    m_auto = new QTimer(this);
    m_auto->setInterval(kAutoRefreshMs);
    connect(m_auto, &QTimer::timeout, this, &StateStore::refresh);
    m_auto->start();
}

void StateStore::refresh() {
    const int gen = ++m_generation;
    Q_EMIT refreshing();
    m_general->request(QStringLiteral("ls zfsm://"), this, [this, gen](const CliResult& r) {
        if (gen != m_generation) {
            return;
        }
        if (!r.ok()) {
            m_listFailed = true;
            m_listError = r.err.trimmed();
            Q_EMIT changed();
            return;
        }
        m_listFailed = false;
        // The previous answers stay until the new ones arrive: nothing blanks out every
        // minute.
        QVector<MachineView> fresh = machinesFromLs(r.out);
        for (MachineView& m : fresh) {
            for (const MachineView& old : m_machines) {
                if (old.id == m.id) {
                    m.overview = old.overview;
                    m.loaded = old.loaded;
                    m.failure = old.failure;
                }
            }
        }
        m_machines = fresh;
        Q_EMIT changed();
        for (const MachineView& m : m_machines) {
            if (!m.connected) {
                continue;
            }
            const QString id = m.id;
            m_forMachine(id)->request(
                QStringLiteral("overview zfsm://") + id, this, [this, gen, id](const CliResult& o) {
                    if (gen != m_generation) {
                        return;
                    }
                    for (MachineView& mv : m_machines) {
                        if (mv.id != id) {
                            continue;
                        }
                        // overview answers rc 0 also for a machine that does not respond:
                        // that is in its JSON. No JSON is the service failing.
                        if (o.out.contains(QLatin1Char('{'))) {
                            applyOverview(mv, o.out);
                        } else {
                            mv.loaded = true;
                            mv.failure = o.err.trimmed();
                        }
                    }
                    m_updatedAt = QDateTime::currentSecsSinceEpoch();
                    Q_EMIT changed();
                });
        }
    });
}

}  // namespace zfsmgr::gui
