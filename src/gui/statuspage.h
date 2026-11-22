#pragma once

// The status panel: the first thing the interface shows. How every machine is, what needs
// attention, the scheduled copies and what is copying now. Read-only.
//
// Each machine is asked through its OWN service, in parallel: a machine that is off costs its
// own wait, never the others'. The panel fills in as the answers arrive.

#include "statusmodel.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace zfsmgr::gui {

class StateStore;

class StatusPage : public QWidget {
    Q_OBJECT
public:
    explicit StatusPage(StateStore* store, QWidget* parent = nullptr);

Q_SIGNALS:
    // How many things need attention, for the navigation badge.
    void attentionChanged(int count);

private:
    void render();
    void updateAgo();

    StateStore* m_store;

    QLabel* m_ago{nullptr};
    QPushButton* m_refresh{nullptr};
    QScrollArea* m_scroll{nullptr};
    QWidget* m_body{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};
    QTimer* m_tick{nullptr};
};

}  // namespace zfsmgr::gui
