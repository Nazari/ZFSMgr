#pragma once

// Administration (advanced mode): the connections, the daemons and the credentials between
// machines. Everything is the CLI's: `ls zfsm://`, `probe`, `edit`, `install-daemon -a --check`,
// `peers`. The page only shows what they answer and sends what the person chooses.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QScrollArea;
class QTableWidget;

namespace zfsmgr::gui {

class StateStore;

class AdminPage : public QWidget {
    Q_OBJECT
public:
    explicit AdminPage(StateStore* store, QWidget* parent = nullptr);

Q_SIGNALS:
    void message(const QString& text);
    void addMachineRequested();

protected:
    void showEvent(QShowEvent* e) override;

private:
    void load();
    void render();
    void act(const QString& line, const QString& done);
    void editMachine(const QString& id);

    StateStore* m_store;
    QScrollArea* m_scroll{nullptr};
    QJsonArray m_connections;             // `ls zfsm://`
    QJsonArray m_daemons;                 // `install-daemon -a --check`
    QHash<QString, QJsonArray> m_peers;   // machine → `peers`
    QHash<QString, QString> m_probeText;  // machine → what «Test» said
    bool m_loading{false};
};

}  // namespace zfsmgr::gui
