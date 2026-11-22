#pragma once

// The assistant for a new copy: what, where, how, and a review that shows the exact commands
// before running them (copyplan.h). It asks every machine for its pools, datasets and the
// daemons it already reaches when it opens, and runs the commands one by one through the
// general service: the questions the CLI asks on the way (handing over credentials) arrive as
// dialogs like everywhere else.

#include "copyplan.h"

#include <QDialog>
#include <QHash>
#include <QStringList>
#include <QVector>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;

namespace zfsmgr::gui {

class StateStore;

class NewCopyWizard : public QDialog {
    Q_OBJECT
public:
    explicit NewCopyWizard(StateStore* store, QWidget* parent = nullptr);
    // Chooses this origin as soon as the machines have listed it («Copy…» on a dataset).
    void preselect(const QString& machine, const QString& dataset);

Q_SIGNALS:
    void created();

private:
    struct Dataset {
        QString machine;  // connection id
        QString machineName;
        QString name;
        qint64 used{0};
        bool encrypted{false};
    };
    struct Pool {
        QString machine;
        QString machineName;
        QString name;
        QString free;
    };

    void load();
    void fillOrigins();
    void fillDestinations();
    void goTo(int step);
    void review();
    void run();
    void runNext();
    CopyRequest request() const;
    const Dataset* chosenOrigin() const;
    const Pool* chosenDest() const;

    StateStore* m_store;
    QVector<Dataset> m_datasets;
    QVector<Pool> m_pools;
    // machine id → (uid of another machine → the role its daemon has for it)
    QHash<QString, QHash<QString, QString>> m_reaches;
    int m_pending{0};

    QStackedWidget* m_steps{nullptr};
    QLabel* m_stepTitle{nullptr};
    QLineEdit* m_filter{nullptr};
    QListWidget* m_origins{nullptr};
    QListWidget* m_dests{nullptr};
    QLineEdit* m_name{nullptr};
    QRadioButton* m_once{nullptr};
    QRadioButton* m_keepUp{nullptr};
    QRadioButton* m_pull{nullptr};
    QRadioButton* m_push{nullptr};
    QWidget* m_keepBox{nullptr};
    QSpinBox* m_daily{nullptr};
    QSpinBox* m_weekly{nullptr};
    QSpinBox* m_monthly{nullptr};
    QSpinBox* m_yearly{nullptr};
    QLabel* m_summary{nullptr};
    QPlainTextEdit* m_commands{nullptr};
    QListWidget* m_progress{nullptr};
    QPushButton* m_back{nullptr};
    QPushButton* m_next{nullptr};
    QPushButton* m_cancel{nullptr};
    QStringList m_toRun;
    int m_running{-1};
    QString m_stamp;
    QString m_wantMachine;
    QString m_wantDataset;
};

}  // namespace zfsmgr::gui
