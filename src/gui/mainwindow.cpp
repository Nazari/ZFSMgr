#include "mainwindow.h"

#include "questiondialog.h"
#include "statuspage.h"
#include "text.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>
#include <QStatusBar>

namespace zfsmgr::gui {

namespace {

// A page that is not there yet: says so, instead of an empty space.
QWidget* comingLater(const QString& what, QWidget* parent) {
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* l = new QLabel(L("%1 comes in a later phase. Meanwhile, the CLI does it.",
                           "%1 llega en una fase posterior. Mientras, lo hace el CLI.")
                             .arg(what),
                         w);
    l->setWordWrap(true);
    lay->addWidget(l);
    lay->addStretch(1);
    return w;
}

}  // namespace

MainWindow::MainWindow(QString cliPath, QByteArray masterPassword, CliService* general,
                       QWidget* parent)
    : QMainWindow(parent),
      m_cliPath(std::move(cliPath)),
      m_master(std::move(masterPassword)),
      m_general(general) {
    setWindowTitle(QStringLiteral("ZFSMgr"));
    resize(1100, 760);
    m_general->setParent(this);
    wire(m_general);

    auto* central = new QWidget(this);
    auto* lay = new QHBoxLayout(central);
    m_nav = new QListWidget(central);
    m_nav->setFixedWidth(180);
    m_nav->addItems({L("Status", "Estado"), L("Copies", "Copias"), L("Machines", "Máquinas"),
                     L("Jobs", "Trabajos")});
    lay->addWidget(m_nav);
    m_pages = new QStackedWidget(central);
    m_status =
        new StatusPage(m_general, [this](const QString& id) { return serviceFor(id); }, m_pages);
    m_pages->addWidget(m_status);
    m_pages->addWidget(comingLater(L("Copies", "Copias"), m_pages));
    m_pages->addWidget(comingLater(L("Machines", "Máquinas"), m_pages));
    m_pages->addWidget(comingLater(L("Jobs", "Trabajos"), m_pages));
    lay->addWidget(m_pages, 1);
    setCentralWidget(central);
    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_status, &StatusPage::attentionChanged, this, [this](int n) {
        m_nav->item(0)->setText(n > 0 ? L("Status (%1)", "Estado (%1)").arg(n)
                                      : L("Status", "Estado"));
    });
    m_nav->setCurrentRow(0);
    m_status->refresh();
}

MainWindow::~MainWindow() {
    m_master.fill('\0');
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
    d->setWindowTitle(QStringLiteral("ZFSMgr"));
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
