#include "mainwindow.h"

#include "buildstamp.h"

#include "copiespage.h"
#include "addmachinedialog.h"
#include "adminpage.h"
#include "createpooldialog.h"
#include "jobspage.h"
#include "machinespage.h"
#include "newcopywizard.h"
#include "questiondialog.h"
#include "statestore.h"
#include "statuspage.h"
#include "text.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QSettings>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>
#include <QStatusBar>

namespace zfsmgr::gui {

MainWindow::MainWindow(QString cliPath, QByteArray masterPassword, CliService* general,
                       QWidget* parent)
    : QMainWindow(parent),
      m_cliPath(std::move(cliPath)),
      m_master(std::move(masterPassword)),
      m_general(general) {
    setWindowTitle(appTitle());
    resize(1100, 760);
    m_general->setParent(this);
    wire(m_general);

    auto* central = new QWidget(this);
    auto* lay = new QHBoxLayout(central);
    auto* side = new QVBoxLayout();
    m_nav = new QListWidget(central);
    m_nav->setFixedWidth(180);
    m_nav->addItems({L("Status", "Estado"), L("Copies", "Copias"), L("Machines", "Máquinas"),
                     L("Jobs", "Trabajos"), L("Administration", "Administración")});
    side->addWidget(m_nav, 1);
    // Advanced mode: what a person who knows ZFS wants and the rest is better without. It is
    // remembered on this computer.
    m_advanced = new QCheckBox(L("Advanced mode", "Modo avanzado"), central);
    side->addWidget(m_advanced);
    lay->addLayout(side);
    m_pages = new QStackedWidget(central);
    m_store = new StateStore(m_general, [this](const QString& id) { return serviceFor(id); }, this);
    m_status = new StatusPage(m_store, m_pages);
    auto* copies = new CopiesPage(m_store, m_pages);
    auto* jobs = new JobsPage(m_store, m_pages);
    m_pages->addWidget(m_status);
    m_pages->addWidget(copies);
    auto* machines = new MachinesPage(m_store, m_pages);
    m_machines = machines;
    m_pages->addWidget(machines);
    m_pages->addWidget(jobs);
    const auto say = [this](const QString& text) { statusBar()->showMessage(text, 15000); };
    connect(copies, &CopiesPage::message, this, say);
    connect(jobs, &JobsPage::message, this, say);
    connect(machines, &MachinesPage::message, this, say);
    connect(this, &MainWindow::advancedChanged, machines, &MachinesPage::setAdvanced);
    connect(machines, &MachinesPage::createPoolRequested, this, [this](const QString& id) {
        auto* d = new CreatePoolDialog(m_store, id, this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        connect(d, &CreatePoolDialog::created, m_store, &StateStore::refresh);
        d->open();
    });
    connect(m_status, &StatusPage::machineRequested, this,
            [this](const QString& id) { showMachine(id); });
    connect(machines, &MachinesPage::addMachineRequested, this, &MainWindow::openAddMachine);
    connect(machines, &MachinesPage::copyRequested, this,
            [this](const QString& m, const QString& ds) { openNewCopy(m, ds); });
    connect(copies, &CopiesPage::newCopyRequested, this, [this] { openNewCopy(); });
    lay->addWidget(m_pages, 1);
    setCentralWidget(central);
    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_status, &StatusPage::attentionChanged, this, [this](int n) {
        m_nav->item(0)->setText(n > 0 ? L("Status (%1)", "Estado (%1)").arg(n)
                                      : L("Status", "Estado"));
    });
    auto* admin = new AdminPage(m_store, m_pages);
    m_pages->addWidget(admin);
    connect(admin, &AdminPage::message, this, say);
    connect(admin, &AdminPage::addMachineRequested, this, &MainWindow::openAddMachine);
    connect(m_advanced, &QCheckBox::toggled, this, [this](bool on) { setAdvanced(on); });
    m_advanced->setChecked(QSettings().value(QStringLiteral("gui/advanced"), false).toBool());
    setAdvanced(m_advanced->isChecked(), false);
    m_nav->setCurrentRow(0);
    m_store->refresh();
}

MainWindow::~MainWindow() {
    m_master.fill('\0');
}

void MainWindow::openNewCopy(const QString& machine, const QString& dataset) {
    auto* w = new NewCopyWizard(m_store, this);
    w->setAttribute(Qt::WA_DeleteOnClose);
    connect(w, &NewCopyWizard::created, m_store, &StateStore::refresh);
    if (!dataset.isEmpty()) {
        w->preselect(machine, dataset);
    }
    w->open();
}

void MainWindow::showMachine(const QString& machine, const QString& dataset) {
    showPage(2);
    m_machines->reveal(machine, dataset);
}

void MainWindow::openAddMachine() {
    auto* d = new AddMachineDialog(m_store, this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    connect(d, &AddMachineDialog::added, m_store, &StateStore::refresh);
    d->open();
}

bool MainWindow::advanced() const {
    return m_advanced->isChecked();
}

void MainWindow::setAdvanced(bool on, bool remember) {
    if (remember) {
        QSettings().setValue(QStringLiteral("gui/advanced"), on);
    }
    if (m_advanced->isChecked() != on) {
        const QSignalBlocker block(m_advanced);
        m_advanced->setChecked(on);
    }
    m_nav->item(4)->setHidden(!on);
    if (!on && m_nav->currentRow() == 4) {
        m_nav->setCurrentRow(0);
    }
    Q_EMIT advancedChanged(on);
}

void MainWindow::showPage(int index) {
    m_nav->setCurrentRow(index);
}

CliService* MainWindow::serviceFor(const QString& machine) {
    if (CliService* s = m_perMachine.value(machine)) {
        return s;
    }
    auto* s = new CliService(m_cliPath, uiLang(), this);
    wire(s);
    connect(s, &CliService::failed, this, [this, machine](const QString& why, bool) {
        statusBar()->showMessage(
            L("The service for %1 ended: %2", "El servicio de %1 terminó: %2").arg(machine, why),
            10000);
        // A new one on the next request.
        if (CliService* dead = m_perMachine.take(machine)) {
            dead->deleteLater();
        }
    });
    m_perMachine.insert(machine, s);
    s->start(m_master);
    return s;
}

void MainWindow::wire(CliService* s) {
    connect(s, &CliService::confirmRequested, this,
            [this, s](const CliQuestion& q) { askQuestion(s, q); });
    connect(s, &CliService::secretRequested, this,
            [this, s](int id, const QString& text) { askSecret(s, id, text); });
    connect(s, &CliService::ready, this, [this](const QString& warning) {
        if (!warning.isEmpty()) {
            statusBar()->showMessage(warning, 15000);
        }
    });
}

void MainWindow::askQuestion(CliService* s, const CliQuestion& q) {
    // open(), not exec(): no nested event loop. The answer goes when the dialog closes.
    auto* d = new QuestionDialog(q, this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    const int id = q.id;
    connect(d, &QDialog::finished, this, [s, d, id](int result) {
        s->answerConfirm(id, result == QDialog::Accepted, d->typed());
    });
    d->open();
}

void MainWindow::askSecret(CliService* s, int id, const QString& text) {
    auto* d = new QInputDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(appTitle());
    d->setLabelText(text);
    d->setTextEchoMode(QLineEdit::Password);
    connect(d, &QDialog::finished, this, [s, d, id](int result) {
        if (result == QDialog::Accepted) {
            s->answerSecret(id, d->textValue());
        } else {
            s->answerSecret(id, std::nullopt);
        }
    });
    d->open();
}

}  // namespace zfsmgr::gui
