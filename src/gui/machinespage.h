#pragma once

// Machines: a tree —machine, pool, dataset— and the detail of what is chosen, with what can be
// done to it. Every level is asked to the CLI when it is opened (`ls`, `ls -r`, `probe`), on
// the service of its own machine, so a slow machine only slows its own branch.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QWidget>

#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QScrollArea;
class QHBoxLayout;

namespace zfsmgr::gui {

class StateStore;

class MachinesPage : public QWidget {
    Q_OBJECT
public:
    explicit MachinesPage(StateStore* store, QWidget* parent = nullptr);
    // Opens the tree down to that machine, or to one of its datasets, and chooses it.
    void reveal(const QString& machine, const QString& dataset = {});
    // Advanced mode: the pool's maintenance, creating and importing pools.
    void setAdvanced(bool on);

Q_SIGNALS:
    void message(const QString& text);
    void addMachineRequested();
    // «Copy…» on a dataset: the assistant, with the origin already chosen.
    void copyRequested(const QString& machine, const QString& dataset);
    void createPoolRequested(const QString& machine);

private:
    enum Kind { Machine = 1, Pool, Dataset };

    void rebuildMachines();
    void expand(QTreeWidgetItem* item);
    void loadPool(QTreeWidgetItem* poolItem);
    void showDetail();
    void showMachine(const QString& id);
    void showPool(const QString& machine, const QString& pool);
    void showDataset(const QString& machine, const QString& dataset);
    QWidget* datasetSummary(const QString& machine, const QString& dataset);
    QWidget* filesTab(const QString& machine, const QString& dataset);
    QWidget* propertiesTab(const QString& machine, const QString& dataset);
    QWidget* organizeTab(const QString& machine, const QString& dataset);
    // machinespage_manage.cpp: name, mounting, keys, permissions; sending; snapshot extras.
    QWidget* manageTab(const QString& machine, const QString& dataset);
    QWidget* sendTab(const QString& machine, const QString& dataset);
    void snapshotExtras(QHBoxLayout* row, QWidget* w, const QString& machine,
                        const QString& dataset, const std::function<QString()>& chosen);
    void newDataset(const QString& machine, const QString& parent);
    // Runs a read on the machine's service, as text, and shows what it wrote in a window.
    void showOutput(const QString& machine, const QString& line, const QString& title);
    // The dataset's properties: true when they are here; false asks for them (once) and the
    // detail is drawn again when they arrive.
    bool ensureProps(const QString& machine, const QString& dataset);
    QString propOf(const QString& machine, const QString& dataset, const QString& name) const;
    // Runs a command line on the general service, says how it went and refreshes.
    // `then`: what to do once it has finished —for instance, check the machine again—.
    void act(const QString& line, const QString& done, bool reloadTree = true,
             std::function<void()> then = {});
    void setDetail(QWidget* w);
    // The tree's item for a machine (empty pool) or for one of its pools; null if gone.
    QTreeWidgetItem* findItem(const QString& machine, const QString& pool) const;

    StateStore* m_store;
    QTreeWidget* m_tree{nullptr};
    QScrollArea* m_detail{nullptr};
    QHash<QString, QJsonObject> m_probe;   // machine → its `probe`
    QHash<QString, QJsonArray> m_pools;    // machine → `ls zfsm://m` pools
    QHash<QString, QJsonArray> m_entries;  // machine/pool → `ls -r` entries
    QHash<QString, QJsonObject> m_status;  // machine/pool → `status` (JSON)
    bool m_advanced{false};
    int m_datasetTab{0};                     // the dataset tab chosen, kept across rebuilds
    QHash<QString, QString> m_filesPath;     // machine/dataset → directory being browsed
    QHash<QString, QJsonArray> m_files;      // machine/dataset#dir → its entries
    QHash<QString, QJsonArray> m_props;      // machine/dataset → its properties
    QSet<QString> m_propsAsked;              // machine/dataset whose properties are on the way
    QHash<QString, QJsonArray> m_breakdown;  // machine/dataset → `breakdown --list`
    void importPool(const QString& machine);
    void forgetPools(const QString& machine);
    QString m_revealMachine;  // what reveal() still has to reach
    QString m_revealDataset;
    void continueReveal();
};

}  // namespace zfsmgr::gui
