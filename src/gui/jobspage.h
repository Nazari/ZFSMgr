#pragma once

// Jobs: what is copying now —with a button to stop it— and what finished, with when and how.
// The running ones come from the shared state; the finished ones are asked to each machine
// (`jobs --history`) when the page is shown and after every refresh while it is.

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QWidget>

class QScrollArea;
class QVBoxLayout;

namespace zfsmgr::gui {

class StateStore;

class JobsPage : public QWidget {
    Q_OBJECT
public:
    explicit JobsPage(StateStore* store, QWidget* parent = nullptr);

Q_SIGNALS:
    void message(const QString& text);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void loadHistory();
    void render();
    void stop(const QString& machineId, const QString& jobId);

    StateStore* m_store;
    QHash<QString, QJsonArray> m_history;  // machine id → its finished jobs
    QSet<QString> m_asked;                 // machines asked since the last refresh
    QScrollArea* m_scroll{nullptr};
};

}  // namespace zfsmgr::gui
