#pragma once

// Machines: a tree —machine, pool, dataset— and the detail of what is chosen, with what can be
// done to it. Every level is asked to the CLI when it is opened (`ls`, `ls -r`, `probe`), on
// the service of its own machine, so a slow machine only slows its own branch.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QScrollArea;

namespace zfsmgr::gui {

class StateStore;

class MachinesPage : public QWidget {
    Q_OBJECT
public:
    explicit MachinesPage(StateStore* store, QWidget* parent = nullptr);
    // Opens the tree down to that machine, or to one of its datasets, and chooses it.
    void reveal(const QString& machine, const QString& dataset = {});

Q_SIGNALS:
    void message(const QString& text);
    void addMachineRequested();
    // «Copy…» on a dataset: the assistant, with the origin already chosen.
    void copyRequested(const QString& machine, const QString& dataset);

private:
    enum Kind { Machine = 1, Pool, Dataset };

    void rebuildMachines();
    void expand(QTreeWidgetItem* item);
    void loadPool(QTreeWidgetItem* poolItem);
    void showDetail();
    void showMachine(const QString& id);
    void showPool(const QString& machine, const QString& pool);
    void showDataset(const QString& machine, const QString& dataset);
    // Runs a command line on the general service, says how it went and refreshes.
    void act(const QString& line, const QString& done, bool reloadTree = true);
    void setDetail(QWidget* w);
    // The tree's item for a machine (empty pool) or for one of its pools; null if gone.
    QTreeWidgetItem* findItem(const QString& machine, const QString& pool) const;

    StateStore* m_store;
    QTreeWidget* m_tree{nullptr};
    QScrollArea* m_detail{nullptr};
    QHash<QString, QJsonObject> m_probe;   // machine → its `probe`
    QHash<QString, QJsonArray> m_pools;    // machine → `ls zfsm://m` pools
    QHash<QString, QJsonArray> m_entries;  // machine/pool → `ls -r` entries
    QString m_revealMachine;               // what reveal() still has to reach
    QString m_revealDataset;
    void continueReveal();
};

}  // namespace zfsmgr::gui
