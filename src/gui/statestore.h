#pragma once

// What the interface knows about the machines right now: `ls zfsm://` and the `overview` of
// each one. Shared by the pages that read it (Status, Copies, Jobs), so a minute's refresh
// asks each machine once, not once per page.
//
// Each machine is asked through its OWN service, in parallel: one that is off costs its own
// wait, never the others'. `changed()` fires as each answer arrives.

#include "statusmodel.h"

#include <QObject>

#include <functional>

class QTimer;

namespace zfsmgr::gui {

class CliService;

class StateStore : public QObject {
    Q_OBJECT
public:
    StateStore(CliService* general, std::function<CliService*(const QString& id)> forMachine,
               QObject* parent = nullptr);

    void refresh();

    const QVector<MachineView>& machines() const { return m_machines; }
    qint64 updatedAt() const { return m_updatedAt; }
    bool listFailed() const { return m_listFailed; }
    const QString& listError() const { return m_listError; }

    CliService* general() const { return m_general; }
    CliService* forMachine(const QString& id) const { return m_forMachine(id); }

Q_SIGNALS:
    void changed();
    void refreshing();

private:
    CliService* m_general;
    std::function<CliService*(const QString&)> m_forMachine;
    QVector<MachineView> m_machines;
    qint64 m_updatedAt{0};
    int m_generation{0};  // answers from an older refresh are dropped
    bool m_listFailed{false};
    QString m_listError;
    QTimer* m_auto{nullptr};
};

}  // namespace zfsmgr::gui
