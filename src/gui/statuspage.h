#pragma once

// The status panel: the first thing the interface shows. How every machine is, what needs
// attention, the scheduled copies and what is copying now. Read-only.
//
// Each machine is asked through its OWN service, in parallel: a machine that is off costs its
// own wait, never the others'. The panel fills in as the answers arrive.

#include "statusmodel.h"

#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace zfsmgr::gui {

class CliService;

class StatusPage : public QWidget {
    Q_OBJECT
public:
    // `general`: the service for what is not about one machine (`ls`). `forMachine`: the
    // service of one machine, created on first use.
    StatusPage(CliService* general, std::function<CliService*(const QString& id)> forMachine,
               QWidget* parent = nullptr);

    void refresh();
    const QVector<MachineView>& machines() const { return m_machines; }

Q_SIGNALS:
    // How many things need attention, for the navigation badge.
    void attentionChanged(int count);

private:
    void render();
    void updateAgo();

    CliService* m_general;
    std::function<CliService*(const QString&)> m_forMachine;
    QVector<MachineView> m_machines;
    qint64 m_updatedAt{0};
    int m_generation{0};  // answers from an older refresh are dropped
    bool m_listFailed{false};
    QString m_listError;

    QLabel* m_ago{nullptr};
    QPushButton* m_refresh{nullptr};
    QScrollArea* m_scroll{nullptr};
    QWidget* m_body{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};
    QTimer* m_auto{nullptr};
    QTimer* m_tick{nullptr};
};

}  // namespace zfsmgr::gui
