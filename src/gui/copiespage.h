#pragma once

// Copies: every dataset copied to another machine, one row each, and what can be done with
// one —copy now, pause or resume, stop copying— plus the assistant for a new one.
//
// A copy is a schedule that levels: PUSHED by the origin (`--to … --level`) or PULLED by the
// machine that keeps the copy (`--origin`). Each row says which, because it decides who can do
// what: from a machine that pulls, the original cannot be deleted.

#include "statusmodel.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

namespace zfsmgr::gui {

class StateStore;

class CopiesPage : public QWidget {
    Q_OBJECT
public:
    explicit CopiesPage(StateStore* store, QWidget* parent = nullptr);

Q_SIGNALS:
    void message(const QString& text);
    void newCopyRequested();

private:
    void render();
    void showDetail();
    void act(const QString& line, const QString& done);
    void changeKeep(const QString& on, const ScheduleRow& r);
    QVector<ScheduleRow> copies() const;

    StateStore* m_store;
    QVector<ScheduleRow> m_rows;
    QString m_selectedKey;  // machineId + dataset of the selected row, kept across refreshes
    QTableWidget* m_table{nullptr};
    QWidget* m_detail{nullptr};
    QVBoxLayout* m_detailLayout{nullptr};
};

}  // namespace zfsmgr::gui
