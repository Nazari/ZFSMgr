#pragma once

// The window: navigation on the left, one page at a time on the right. It owns the services
// —one general, one per machine— and answers, on their behalf, the questions and the secrets
// the CLI asks for. Pages ask the services; they never start a process themselves.

#include "cliservice.h"

#include <QByteArray>
#include <QHash>
#include <QMainWindow>

#include <functional>

class QCheckBox;
class QListWidget;
class QStackedWidget;

namespace zfsmgr::gui {

class StateStore;
class StatusPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `general` is already running (it is how the master password was checked).
    // `secret`: the master password, or —`isDataKey`— the data key the general service
    // opened; the services of each machine start with it.
    MainWindow(QString cliPath, QByteArray secret, bool isDataKey, CliService* general,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    // The service of one machine, started on first use.
    CliService* serviceFor(const QString& machine);
    // 0 Status, 1 Copies, 2 Machines, 3 Jobs.
    void showPage(int index);
    void openNewCopy(const QString& machine = {}, const QString& dataset = {});
    void openAddMachine();
    // The Machines page, opened down to that machine or dataset.
    void showMachine(const QString& machine, const QString& dataset = {});
    // `remember`: kept for the next start. Tests switch it on without touching the setting.
    void setAdvanced(bool on, bool remember = true);
    bool advanced() const;
    // After the welcome created the master password: this computer, step by step —its sudo
    // password if it has OpenZFS, its daemon if it has none— and then the Machines page.
    void startFirstRun();

Q_SIGNALS:
    void advancedChanged(bool on);

private:
    // Asked from another machine to close: says who and closes in a few seconds, unless the
    // person here chooses to keep it open.
    void closeAsked(const QString& host, const QString& user);
    void wire(CliService* s);
    void askQuestion(CliService* s, const CliQuestion& q);
    void askSecret(CliService* s, int id, const QString& text);
    void firstRunStep();
    // The step's check of this computer, when it has a Local connection.
    void probeHere();
    void firstRunDone(const QString& note);
    void choose(const QString& text, const QString& yes, const QString& no,
                std::function<void(bool)> done);

    QString m_cliPath;
    QByteArray m_master;
    bool m_masterIsDataKey{false};
    CliService* m_general;
    QHash<QString, CliService*> m_perMachine;
    QListWidget* m_nav{nullptr};
    QCheckBox* m_advanced{nullptr};
    QStackedWidget* m_pages{nullptr};
    StateStore* m_store{nullptr};
    StatusPage* m_status{nullptr};
    class MachinesPage* m_machines{nullptr};
    bool m_sudoOffered{false};
    bool m_daemonOffered{false};
};

}  // namespace zfsmgr::gui
