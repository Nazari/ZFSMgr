#pragma once

// The window: navigation on the left, one page at a time on the right. It owns the services
// —one general, one per machine— and answers, on their behalf, the questions and the secrets
// the CLI asks for. Pages ask the services; they never start a process themselves.

#include "cliservice.h"

#include <QByteArray>
#include <QHash>
#include <QMainWindow>

class QListWidget;
class QStackedWidget;

namespace zfsmgr::gui {

class StateStore;
class StatusPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `general` is already running (it is how the master password was checked).
    MainWindow(QString cliPath, QByteArray masterPassword, CliService* general,
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

private:
    void wire(CliService* s);
    void askQuestion(CliService* s, const CliQuestion& q);
    void askSecret(CliService* s, int id, const QString& text);

    QString m_cliPath;
    QByteArray m_master;
    CliService* m_general;
    QHash<QString, CliService*> m_perMachine;
    QListWidget* m_nav{nullptr};
    QStackedWidget* m_pages{nullptr};
    StateStore* m_store{nullptr};
    StatusPage* m_status{nullptr};
    class MachinesPage* m_machines{nullptr};
};

}  // namespace zfsmgr::gui
