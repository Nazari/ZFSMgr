#include "adminpage.h"

#include "addmachinedialog.h"
#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace zfsmgr::gui {

namespace {

QJsonObject firstObject(const QString& out) {
    const qsizetype start = out.indexOf(QLatin1Char('{'));
    return start < 0 ? QJsonObject() : QJsonDocument::fromJson(out.mid(start).toUtf8()).object();
}

QString url(const QString& machine) {
    return QStringLiteral("zfsm://") + machine;
}

QString roleWord(const QString& role) {
    if (role == QLatin1String("pull")) {
        return L("pull only", "solo traer");
    }
    if (role == QLatin1String("full")) {
        return L("full", "completa");
    }
    return L("unknown", "no se sabe");
}

QPushButton* small(const QString& text, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return b;
}

}  // namespace

AdminPage::AdminPage(StateStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    auto* outer = new QVBoxLayout(this);
    auto* title = heading(L("Administration", "Administración"), this);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    title->setFont(f);
    outer->addWidget(title);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_scroll, 1);
    render();
}

void AdminPage::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    load();
}

void AdminPage::load() {
    if (m_loading) {
        return;
    }
    m_loading = true;
    m_store->general()->request(QStringLiteral("ls zfsm://"), this, [this](const CliResult& r) {
        m_connections = firstObject(r.out).value(QStringLiteral("connections")).toArray();
        render();
        for (const QJsonValue& v : m_connections) {
            const QJsonObject c = v.toObject();
            if (!c.value(QStringLiteral("connected")).toBool(true)) {
                continue;
            }
            const QString id = c.value(QStringLiteral("id")).toString();
            m_store->forMachine(id)->request(
                QStringLiteral("peers ") + url(id), this, [this, id](const CliResult& p) {
                    m_peers.insert(id, firstObject(p.out).value(QStringLiteral("peers")).toArray());
                    render();
                });
        }
        m_store->general()->request(
            QStringLiteral("install-daemon -a --check"), this, [this](const CliResult& d) {
                m_daemons = firstObject(d.out).value(QStringLiteral("items")).toArray();
                m_loading = false;
                render();
            });
    });
}

void AdminPage::act(const QString& line, const QString& done) {
    m_store->general()->request(line, this, [this, done](const CliResult& r) {
        Q_EMIT message(r.ok() ? done : r.err.trimmed());
        m_store->refresh();
        load();
    });
}

void AdminPage::render() {
    const int pos = m_scroll->verticalScrollBar()->value();
    auto* body = new QWidget();
    auto* lay = new QVBoxLayout(body);
    lay->setAlignment(Qt::AlignTop);

    // ── Connections.
    {
        auto* head = new QHBoxLayout();
        head->addWidget(heading(L("Connections", "Conexiones"), body));
        head->addStretch(1);
        auto* add = small(L("+ Add machine", "+ Añadir máquina"), body);
        connect(add, &QPushButton::clicked, this, &AdminPage::addMachineRequested);
        head->addWidget(add);
        lay->addLayout(head);
        QFrame* box = card(body);
        auto* g = new QGridLayout(box);
        const QStringList heads{L("Name", "Nombre"), L("Address", "Dirección"),
                                L("System", "Sistema"), L("State", "Estado")};
        for (int c = 0; c < heads.size(); ++c) {
            g->addWidget(muted(heads[c], box), 0, c);
        }
        int row = 1;
        for (const QJsonValue& v : m_connections) {
            const QJsonObject c = v.toObject();
            const QString id = c.value(QStringLiteral("id")).toString();
            const QString host = c.value(QStringLiteral("host")).toString();
            const QString user = c.value(QStringLiteral("user")).toString();
            const int port = c.value(QStringLiteral("port")).toInt();
            const QString addr =
                host.isEmpty()
                    ? L("this machine", "esta máquina")
                    : (user.isEmpty() ? host : user + QLatin1Char('@') + host)
                          + (port > 0 && port != 22 ? QStringLiteral(":%1").arg(port) : QString());
            g->addWidget(label(c.value(QStringLiteral("name")).toString(id), box, false), row, 0);
            g->addWidget(label(addr, box, false), row, 1);
            g->addWidget(label(c.value(QStringLiteral("os")).toString(), box, false), row, 2);
            g->addWidget(label(c.value(QStringLiteral("connected")).toBool(true)
                                   ? L("connected", "conectada")
                                   : L("disconnected", "desconectada"),
                               box, false),
                         row, 3);
            auto* test = small(L("Test", "Probar"), box);
            connect(test, &QPushButton::clicked, this, [this, id] {
                m_probeText.insert(id, L("Testing…", "Probando…"));
                render();
                m_store->forMachine(id)->request(
                    QStringLiteral("probe ") + url(id), this,
                    [this, id](const CliResult& r) {
                        m_probeText.insert(id, (r.out + r.err).trimmed());
                        render();
                    },
                    QStringLiteral("text"));
            });
            g->addWidget(test, row, 4);
            auto* edit = small(L("Edit…", "Editar…"), box);
            connect(edit, &QPushButton::clicked, this, [this, id] { editMachine(id); });
            g->addWidget(edit, row, 5);
            ++row;
            if (m_probeText.contains(id)) {
                auto* p = label(m_probeText.value(id), box);
                p->setStyleSheet(QStringLiteral("font-family:monospace"));
                g->addWidget(p, row, 0, 1, 6);
                ++row;
            }
        }
        g->setColumnStretch(1, 1);
        lay->addWidget(box);
        lay->addWidget(
            muted(L("«Test» checks that SSH gets there, which system there is and whether "
                    "the sudo password works. The CLI does it; this only shows the answer.",
                    "«Probar» comprueba que se llega por SSH, qué sistema hay y si la "
                    "contraseña de sudo vale. Lo hace el CLI; la ventana solo enseña el "
                    "resultado."),
                  body));
    }

    // ── Daemons.
    {
        int toUpdate = 0;
        for (const QJsonValue& v : m_daemons) {
            const QJsonObject d = v.toObject();
            const QString a = d.value(QStringLiteral("code")).toString();
            const QString ca = d.value(QStringLiteral("cli_code")).toString();
            if (a == QLatin1String("update") || a == QLatin1String("install")
                || ca == QLatin1String("update")) {
                ++toUpdate;
            }
        }
        auto* head = new QHBoxLayout();
        head->addWidget(heading(L("Daemons", "Daemons"), body));
        head->addStretch(1);
        if (toUpdate > 0) {
            auto* all = small(
                L("Update the %1 that need it", "Actualizar las %1 que lo necesitan").arg(toUpdate),
                body);
            connect(all, &QPushButton::clicked, this, [this] {
                act(QStringLiteral("install-daemon -a"),
                    L("Daemons updated.", "Daemons actualizados."));
            });
            head->addWidget(all);
        }
        lay->addLayout(head);
        QFrame* box = card(body);
        auto* g = new QGridLayout(box);
        const QStringList heads{L("Machine", "Máquina"), L("Daemon", "Daemon"),
                                L("What it would do", "Qué haría"), QStringLiteral("zfsmgr-cli")};
        for (int c = 0; c < heads.size(); ++c) {
            g->addWidget(muted(heads[c], box), 0, c);
        }
        int row = 1;
        if (m_daemons.isEmpty()) {
            g->addWidget(muted(L("Asking…", "Preguntando…"), box), row, 0, 1, 4);
        }
        for (const QJsonValue& v : m_daemons) {
            const QJsonObject d = v.toObject();
            const QString id = d.value(QStringLiteral("connection")).toString();
            g->addWidget(label(id, box, false), row, 0);
            g->addWidget(label(d.value(QStringLiteral("daemon")).toString(), box, false), row, 1);
            // The CLI's own words: already in the interface's language.
            g->addWidget(label(d.value(QStringLiteral("action")).toString(), box, false), row, 2);
            g->addWidget(label(d.value(QStringLiteral("cli_action")).toString(), box, false), row,
                         3);
            const QString a = d.value(QStringLiteral("code")).toString();
            if (a == QLatin1String("update") || a == QLatin1String("install")) {
                auto* b = small(L("Update", "Actualizar"), box);
                connect(b, &QPushButton::clicked, this, [this, id] {
                    act(QStringLiteral("install-daemon --on ") + url(id),
                        L("Daemon of %1 updated.", "Daemon de %1 actualizado.").arg(id));
                });
                g->addWidget(b, row, 4);
            }
            ++row;
        }
        g->setColumnStretch(2, 1);
        lay->addWidget(box);
        lay->addWidget(
            muted(L("Before reinstalling, the CLI checks the daemon is not copying anything.",
                    "Antes de reinstalar, el CLI comprueba que el daemon no esté copiando "
                    "nada."),
                  body));
    }

    // ── Credentials between machines.
    {
        lay->addWidget(
            heading(L("Credentials between machines", "Credenciales entre máquinas"), body));
        lay->addWidget(
            muted(L("For a machine to copy from another without going through here, it "
                    "needs a credential of it. FULL ones can do anything —sending needs "
                    "them—; PULL-ONLY ones can read and take copies, never delete.",
                    "Para que una máquina copie de otra sin pasar por aquí, necesita una "
                    "credencial suya. Las COMPLETAS pueden hacer de todo —las necesita "
                    "quien envía—; las de SOLO TRAER pueden leer y llevarse copias, nunca "
                    "borrar."),
                  body));
        QFrame* box = card(body);
        auto* g = new QGridLayout(box);
        g->addWidget(muted(L("Machine", "Máquina"), box), 0, 0);
        g->addWidget(muted(L("Can reach", "Puede llegar a"), box), 0, 1);
        int row = 1;
        for (const QJsonValue& v : m_connections) {
            const QJsonObject c = v.toObject();
            if (!c.value(QStringLiteral("connected")).toBool(true)) {
                continue;
            }
            const QString id = c.value(QStringLiteral("id")).toString();
            QStringList reach;
            for (const QJsonValue& p : m_peers.value(id)) {
                const QJsonObject po = p.toObject();
                reach << QStringLiteral("%1 (%2)").arg(
                    po.value(QStringLiteral("id")).toString(),
                    roleWord(po.value(QStringLiteral("role")).toString()));
            }
            g->addWidget(label(c.value(QStringLiteral("name")).toString(id), box, false), row, 0);
            g->addWidget(
                label(reach.isEmpty() ? L("none", "ninguna") : reach.join(QStringLiteral(", ")),
                      box),
                row, 1);
            auto* pull = small(L("Give pull-only…", "Dar solo traer…"), box);
            connect(pull, &QPushButton::clicked, this, [this, id] {
                act(QStringLiteral("peers --push --role pull --on ") + url(id),
                    L("Pull-only credentials given.", "Credenciales de solo traer entregadas."));
            });
            g->addWidget(pull, row, 2);
            auto* full = small(L("Give full…", "Dar completas…"), box);
            connect(full, &QPushButton::clicked, this, [this, id] {
                act(QStringLiteral("peers --push --role full --on ") + url(id),
                    L("Full credentials given.", "Credenciales completas entregadas."));
            });
            g->addWidget(full, row, 3);
            auto* listen = small(L("Listen on the network…", "Escuchar en la red…"), box);
            connect(listen, &QPushButton::clicked, this, [this, id] {
                act(QStringLiteral("peers --listen 0.0.0.0 --on ") + url(id),
                    L("Its daemon now listens on the network.", "Su daemon ya escucha en la red."));
            });
            g->addWidget(listen, row, 4);
            ++row;
        }
        g->setColumnStretch(1, 1);
        lay->addWidget(box);
        lay->addWidget(
            muted(L("«Listen on the network» is needed on the machine others copy FROM "
                    "(pulling) or TO (sending): by default its daemon only answers on the "
                    "machine itself. The daemon restarts.",
                    "«Escuchar en la red» hace falta en la máquina DE la que otras traen o "
                    "A la que otras envían: por omisión su daemon solo atiende en la propia "
                    "máquina. El daemon se reinicia."),
                  body));
    }

    m_scroll->setWidget(body);
    QTimer::singleShot(0, this, [this, pos] { m_scroll->verticalScrollBar()->setValue(pos); });
}

void AdminPage::editMachine(const QString& id) {
    QJsonObject c;
    for (const QJsonValue& v : m_connections) {
        if (v.toObject().value(QStringLiteral("id")).toString() == id) {
            c = v.toObject();
        }
    }
    auto* d = new QDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(L("Edit %1", "Editar %1").arg(id));
    d->setMinimumWidth(460);
    auto* lay = new QVBoxLayout(d);
    const bool local = c.value(QStringLiteral("host")).toString().isEmpty();
    auto* form = new QFormLayout();
    auto* name = new QLineEdit(c.value(QStringLiteral("name")).toString(), d);
    form->addRow(L("Name shown", "Nombre visible"), name);
    auto* host = new QLineEdit(c.value(QStringLiteral("host")).toString(), d);
    auto* user = new QLineEdit(c.value(QStringLiteral("user")).toString(), d);
    auto* port = new QSpinBox(d);
    port->setRange(1, 65535);
    port->setValue(c.value(QStringLiteral("port")).toInt(22) > 0
                       ? c.value(QStringLiteral("port")).toInt(22)
                       : 22);
    auto* key = new QLineEdit(d);
    key->setPlaceholderText(L("empty: keep as it is", "vacío: dejar como está"));
    if (!local) {
        form->addRow(L("Host", "Máquina (host)"), host);
        form->addRow(L("User", "Usuario"), user);
        form->addRow(L("Port", "Puerto"), port);
        form->addRow(L("SSH key", "Clave SSH"), key);
    }
    auto* sudo = new QCheckBox(L("Needs sudo", "Necesita sudo"), d);
    sudo->setChecked(c.value(QStringLiteral("sudo")).toBool());
    form->addRow(QString(), sudo);
    lay->addLayout(form);
    lay->addWidget(muted(L("The password is asked next: leave it empty to keep the stored one.",
                           "La contraseña se pregunta después: déjala vacía para conservar la "
                           "guardada."),
                         d));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, d,
            [this, d, id, c, local, name, host, user, port, key, sudo] {
                // Only what changed goes on the line.
                QString line = QStringLiteral("edit ") + url(id);
                if (name->text().trimmed() != c.value(QStringLiteral("name")).toString()) {
                    line += QStringLiteral(" --name ") + quoted(name->text().trimmed());
                }
                if (!local) {
                    if (host->text().trimmed() != c.value(QStringLiteral("host")).toString()) {
                        line += QStringLiteral(" --host ") + quoted(host->text().trimmed());
                    }
                    if (user->text().trimmed() != c.value(QStringLiteral("user")).toString()) {
                        line += QStringLiteral(" --user ") + quoted(user->text().trimmed());
                    }
                    if (port->value() != c.value(QStringLiteral("port")).toInt(22)) {
                        line += QStringLiteral(" --port %1").arg(port->value());
                    }
                    if (!key->text().trimmed().isEmpty()) {
                        line += QStringLiteral(" --key ") + quoted(key->text().trimmed());
                    }
                }
                if (sudo->isChecked() != c.value(QStringLiteral("sudo")).toBool()) {
                    line += sudo->isChecked() ? QStringLiteral(" --sudo")
                                              : QStringLiteral(" --no-sudo");
                }
                act(line, L("%1 changed.", "%1 cambiada.").arg(id));
                d->accept();
            });
    d->open();
}

}  // namespace zfsmgr::gui
