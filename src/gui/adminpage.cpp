#include "adminpage.h"

#include "addmachinedialog.h"
#include "buildstamp.h"
#include "cliservice.h"
#include "keys.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
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
            // From each machine, to every other it holds a key of: on its own service, so a
            // machine that takes long only delays its own row.
            m_store->forMachine(id)->request(
                QStringLiteral("peers --check --on ") + url(id), this,
                [this, id](const CliResult& p) {
                    if (p.ok()) {
                        m_checks.insert(
                            id, firstObject(p.out).value(QStringLiteral("checks")).toArray());
                        m_checkError.remove(id);
                    } else {
                        m_checks.remove(id);
                        m_checkError.insert(id, p.err.trimmed());
                    }
                    render();
                });
        }
        m_store->general()->request(QStringLiteral("master-key"), this, [this](const CliResult& k) {
            m_keys = firstObject(k.out).value(QStringLiteral("keys")).toArray();
            render();
        });
        m_store->general()->request(
            QStringLiteral("install-daemon -a --check"), this, [this](const CliResult& d) {
                m_daemons = firstObject(d.out).value(QStringLiteral("items")).toArray();
                m_loading = false;
                render();
            });
    });
}

void AdminPage::act(const QString& line, const QString& done, int delayMs) {
    m_store->general()->request(line, this, [this, done, delayMs](const CliResult& r) {
        Q_EMIT message(r.ok() ? done : r.err.trimmed());
        m_store->refresh();
        // A daemon that restarts answers again a few seconds later: checked before, every
        // square towards it would read «does not get there».
        QTimer::singleShot(delayMs, this, [this] { load(); });
    });
}

// Registering asks for two touches —one to create the credential, one to read its secret— and
// takes as long as the person does: a note says so while it waits, and goes when it is done.
// A PIN, if the key wants one, comes as a secret question like any other.
void AdminPage::addKey() {
    auto* d = new QInputDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(L("Add a security key", "Añadir una llave de seguridad"));
    // Any FIDO2 key with hmac-secret will do, not one brand: empty, the CLI names it after the
    // model the key reports.
    d->setLabelText(L("Plug the key in. What should it be called? (empty: its model)",
                      "Conecta la llave. ¿Cómo quieres llamarla? (vacío: su modelo)"));
    connect(d, &QInputDialog::textValueSelected, this, [this](const QString& name) {
        auto* note = new QMessageBox(QMessageBox::Information,
                                     L("Add a security key", "Añadir una llave de seguridad"),
                                     L("Touch the key TWICE when it blinks: once to register it "
                                       "and once to read its secret.",
                                       "Toca la llave DOS veces cuando parpadee: una para "
                                       "registrarla y otra para leer su secreto."),
                                     QMessageBox::NoButton, this);
        note->setAttribute(Qt::WA_DeleteOnClose);
        note->setStandardButtons(QMessageBox::NoButton);
        note->open();
        const QPointer<QMessageBox> keep = note;
        QString line = QStringLiteral("master-key add");
        if (!name.trimmed().isEmpty()) {
            line += QStringLiteral(" --label ") + quoted(name.trimmed());
        }
        m_store->general()->request(line, this, [this, keep](const CliResult& r) {
            if (keep) {
                keep->done(0);
            }
            Q_EMIT message(r.ok() ? L("Key added: from now on it opens ZFSMgr.",
                                      "Llave añadida: desde ahora abre ZFSMgr.")
                                  : r.err.trimmed());
            if (!r.ok()) {
                showText(this, L("Add a security key", "Añadir una llave de seguridad"),
                         r.err.trimmed());
            }
            load();
        });
    });
    d->open();
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

    // ── Programs: zfsmgr-agent and zfsmgr-cli on each machine, each in its own columns —in
    // one row, «up to date» for one and «update» for the other read as a contradiction—, and
    // zfsmgr-gui with the zfsmgr-cli it runs, on this computer.
    {
        int daemons = 0;
        int clis = 0;
        for (const QJsonValue& v : m_daemons) {
            const QJsonObject d = v.toObject();
            const QString a = d.value(QStringLiteral("code")).toString();
            if (a == QLatin1String("update") || a == QLatin1String("install")) {
                ++daemons;
            }
            if (d.value(QStringLiteral("cli_code")).toString() == QLatin1String("update")) {
                ++clis;
            }
        }
        auto* head = new QHBoxLayout();
        head->addWidget(heading(L("Programs", "Programas"), body));
        head->addStretch(1);
        if (daemons + clis > 0) {
            QStringList what;
            if (daemons > 0) {
                what << QStringLiteral("%1 zfsmgr-agent").arg(daemons);
            }
            if (clis > 0) {
                what << QStringLiteral("%1 zfsmgr-cli").arg(clis);
            }
            auto* all =
                small(L("Update %1", "Actualizar %1").arg(what.join(L(" and ", " y "))), body);
            connect(all, &QPushButton::clicked, this, [this] {
                act(QStringLiteral("install-daemon -a"), L("Updated.", "Actualizado."));
            });
            head->addWidget(all);
        }
        lay->addLayout(head);
        // The two of THIS computer: the interface itself and the zfsmgr-cli it runs.
        // Both with their build's fingerprint: built together, they read the same.
        const QString gui = buildStamp();
        const QString cli = m_store->general()->cliVersion();
        lay->addWidget(label(L("On this computer: zfsmgr-gui %1, which runs zfsmgr-cli %2.",
                               "En este ordenador: zfsmgr-gui %1, que usa zfsmgr-cli %2.")
                                 .arg(gui, cli.isEmpty() ? QStringLiteral("?") : cli),
                             body));
        QFrame* box = card(body);
        auto* g = new QGridLayout(box);
        const QStringList heads{L("Machine", "Máquina"), QStringLiteral("zfsmgr-agent"),
                                L("What would be done", "Qué se haría"),
                                QStringLiteral("zfsmgr-cli"),
                                L("What would be done", "Qué se haría")};
        for (int c = 0; c < heads.size(); ++c) {
            g->addWidget(muted(heads[c], box), 0, c);
        }
        int row = 1;
        if (m_daemons.isEmpty()) {
            g->addWidget(muted(L("Asking…", "Preguntando…"), box), row, 0, 1, 5);
        }
        for (const QJsonValue& v : m_daemons) {
            const QJsonObject d = v.toObject();
            const QString id = d.value(QStringLiteral("connection")).toString();
            g->addWidget(label(id, box, false), row, 0);
            g->addWidget(label(d.value(QStringLiteral("daemon")).toString(), box, false), row, 1);
            // The CLI's own words: already in the interface's language.
            g->addWidget(label(d.value(QStringLiteral("action")).toString(), box), row, 2);
            g->addWidget(label(d.value(QStringLiteral("cli")).toString(), box, false), row, 3);
            g->addWidget(label(d.value(QStringLiteral("cli_action")).toString(), box), row, 4);
            const QString a = d.value(QStringLiteral("code")).toString();
            if (a == QLatin1String("update") || a == QLatin1String("install")) {
                auto* b = small(L("Update its zfsmgr-agent", "Actualizar su zfsmgr-agent"), box);
                connect(b, &QPushButton::clicked, this, [this, id] {
                    act(QStringLiteral("install-daemon --on ") + url(id),
                        L("Daemon of %1 updated.", "Daemon de %1 actualizado.").arg(id));
                });
                g->addWidget(b, row, 5);
            }
            ++row;
        }
        g->setColumnStretch(2, 1);
        g->setColumnStretch(4, 1);
        lay->addWidget(box);
        lay->addWidget(muted(L("zfsmgr-agent is the daemon that does the work on each machine; "
                               "updating it restarts it, and it is checked first that it is not "
                               "copying anything. zfsmgr-cli is the command line program; updating "
                               "it restarts nothing. zfsmgr-gui, this window, is only on this "
                               "computer.",
                               "zfsmgr-agent es el daemon que hace el trabajo en cada máquina; "
                               "actualizarlo lo reinicia, y antes se comprueba que no esté "
                               "copiando nada. zfsmgr-cli es el programa de línea de órdenes; "
                               "actualizarlo no reinicia nada. zfsmgr-gui, esta ventana, solo "
                               "está en este ordenador."),
                             body));
    }

    // ── Between machines: can the daemon on the left reach the one on top by itself, with the
    // credentials it holds? What a scheduled copy needs, tested for real (`peers --check`).
    {
        lay->addWidget(heading(L("Between machines", "Entre máquinas"), body));
        lay->addWidget(muted(
            L("Each square says whether the machine on the left can connect BY ITSELF to the one "
              "on top —what it needs to pull copies from it or to send them to it without going "
              "through this computer—. It needs two things: a key of the other one, and the other "
              "one listening on the network.",
              "Cada casilla dice si la máquina de la izquierda puede conectarse POR SU CUENTA a la "
              "de arriba: lo que necesita para traer copias de ella o enviárselas sin pasar por "
              "este ordenador. Le hacen falta dos cosas: una llave de la otra, y que la otra "
              "escuche en la red."),
            body));
        lay->addWidget(muted(
            L("Keys are full ones: with them a machine pulls copies from another and sends them "
              "to it. Adding a machine gives it the keys of all the others and gives it to all "
              "of them; «Give all the keys» does it again for everybody.",
              "Las llaves son completas: con ellas una máquina trae copias de otra y se las "
              "envía. Al añadir una máquina se le dan las de todas las demás y ella se les da a "
              "todas; «Dar todas las llaves» lo repite para todas."),
            body));
        QStringList ids;
        QHash<QString, QString> names;
        for (const QJsonValue& v : m_connections) {
            const QJsonObject c = v.toObject();
            if (c.value(QStringLiteral("connected")).toBool(true)) {
                const QString id = c.value(QStringLiteral("id")).toString();
                ids << id;
                names.insert(id, c.value(QStringLiteral("name")).toString(id));
            }
        }
        QFrame* box = card(body);
        auto* g = new QGridLayout(box);
        g->setSpacing(6);
        g->addWidget(label(L("from ↓  to →", "desde ↓  a →"), box, false), 0, 0);
        for (int c = 0; c < ids.size(); ++c) {
            g->addWidget(heading(names.value(ids[c]), box), 0, c + 1, Qt::AlignCenter);
        }
        // Full keys only: a machine handed the pull-only ones (`--role pull`, by hand in the CLI)
        // is given the full ones on top.
        const auto keyButtons = [this](QWidget* parent, QVBoxLayout* cl, const QString& from,
                                       const QString& text) {
            auto* b = small(text, parent);
            connect(b, &QPushButton::clicked, this, [this, from] {
                act(QStringLiteral("peers --push --role full --on ") + url(from),
                    L("Keys given.", "Llaves entregadas."));
            });
            cl->addWidget(b, 0, Qt::AlignLeft);
        };
        for (int r = 0; r < ids.size(); ++r) {
            const QString from = ids[r];
            g->addWidget(heading(names.value(from), box), r + 1, 0);
            const auto checks = m_checks.constFind(from);
            for (int c = 0; c < ids.size(); ++c) {
                const QString to = ids[c];
                auto* cell = new QFrame(box);
                cell->setFrameShape(QFrame::StyledPanel);
                auto* cl = new QVBoxLayout(cell);
                cl->setContentsMargins(6, 4, 6, 4);
                g->addWidget(cell, r + 1, c + 1);
                if (from == to) {
                    cl->addWidget(muted(QStringLiteral("—"), cell), 0, Qt::AlignCenter);
                    continue;
                }
                if (checks == m_checks.constEnd()) {
                    const QString why = m_checkError.value(from);
                    auto* x =
                        muted(why.isEmpty() ? L("… checking", "… comprobando")
                                            : L("? could not ask it", "? no se le pudo preguntar"),
                              cell);
                    x->setToolTip(why);
                    cl->addWidget(x);
                    continue;
                }
                QJsonObject chk;
                for (const QJsonValue& v : *checks) {
                    if (v.toObject().value(QStringLiteral("connection")).toString() == to) {
                        chk = v.toObject();
                    }
                }
                const QString state = chk.isEmpty() ? QStringLiteral("no_credentials")
                                                    : chk.value(QStringLiteral("state")).toString();
                const QString role = chk.value(QStringLiteral("role")).toString();
                const QString detail = chk.value(QStringLiteral("detail")).toString();
                QString text;
                QString look;
                const bool pullOnly = state == QLatin1String("ok") && role == QLatin1String("pull");
                if (pullOnly) {
                    look = QStringLiteral("warning");
                    text = L("▲ reaches it, but only to pull", "▲ llega, pero solo para traer");
                } else if (state == QLatin1String("ok")) {
                    look = QStringLiteral("ok");
                    text = L("● reaches it", "● llega");
                } else if (state == QLatin1String("no_credentials")) {
                    look = QStringLiteral("pending");
                    text = L("○ has no key", "○ sin llave");
                } else if (state == QLatin1String("unreachable")) {
                    look = QStringLiteral("error");
                    text = L("✕ does not get there", "✕ no llega");
                } else if (state == QLatin1String("refused")) {
                    look = QStringLiteral("error");
                    text = L("✕ its key is refused", "✕ le rechazan la llave");
                } else {
                    look = QStringLiteral("warning");
                    text = L("▲ unclear", "▲ no está claro");
                }
                auto* t = label(text, cell, false);
                t->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(look)));
                t->setToolTip(detail);
                cl->addWidget(t);
                // Why it does not get there. The daemon on the left looks for the one on top
                // at the address it was given with the keys (`host` of its check): if this
                // computer now has another one, that one is stale. If not, and some other
                // machine does reach the one on top, it does listen on the network: the left
                // one is on a network that does not see that address. Only when nobody
                // reaches it is «it answers only on itself» the likely cause.
                QString nowAt;
                bool isLocal = false;
                for (const QJsonValue& v : m_connections) {
                    const QJsonObject c = v.toObject();
                    if (c.value(QStringLiteral("id")).toString() == to) {
                        nowAt = c.value(QStringLiteral("host")).toString();
                        isLocal = c.value(QStringLiteral("type"))
                                      .toString()
                                      .compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive)
                                  == 0;
                    }
                }
                const QString lookedAt = chk.value(QStringLiteral("host")).toString();
                const bool stale = !isLocal && !nowAt.isEmpty() && !lookedAt.isEmpty()
                                   && lookedAt.compare(nowAt, Qt::CaseInsensitive) != 0;
                bool othersReach = false;
                for (auto it = m_checks.constBegin(); it != m_checks.constEnd(); ++it) {
                    if (it.key() == from || it.key() == to) {
                        continue;
                    }
                    for (const QJsonValue& v : it.value()) {
                        const QJsonObject o = v.toObject();
                        if (o.value(QStringLiteral("connection")).toString() == to
                            && o.value(QStringLiteral("state")).toString() == QLatin1String("ok")) {
                            othersReach = true;
                        }
                    }
                }
                if (state == QLatin1String("unreachable") && stale) {
                    t->setText(L("✕ looks for it at %1", "✕ la busca en %1").arg(lookedAt));
                    t->setToolTip(
                        L("%1 still has the address it was given with the keys (%2); it is %3 "
                          "now. Giving it keys again gives it the new one.\n\n%4",
                          "%1 sigue con la dirección que recibió con las llaves (%2); ahora es "
                          "%3. Darle llaves de nuevo le da la nueva.\n\n%4")
                            .arg(names.value(from), lookedAt, nowAt, detail));
                    keyButtons(cell, cl, from,
                               L("Give it the new address", "Darle la dirección nueva"));
                } else if (state == QLatin1String("unreachable") && othersReach) {
                    t->setText(L("✕ does not get there from here", "✕ no llega desde aquí"));
                    t->setToolTip(
                        L("%1 listens on the network —other machines reach it—, but %2 does not "
                          "get to %3: they are probably on different networks, or a firewall "
                          "stands between. Give %1 an address %2 can reach (Tailscale, a VPN) "
                          "and then keys again.\n\n%4",
                          "%1 escucha en la red —otras máquinas llegan a ella—, pero %2 no llega "
                          "a %3: lo normal es que estén en redes distintas o que haya un "
                          "cortafuegos en medio. Dale a %1 una dirección a la que %2 llegue "
                          "(Tailscale, una VPN) y después llaves de nuevo.\n\n%4")
                            .arg(names.value(to), names.value(from), lookedAt, detail));
                } else if (state == QLatin1String("unreachable")) {
                    // Nobody gets there: most likely the one on top answers only on itself.
                    auto* b =
                        small(L("Open %1 to the network", "Abrir %1 a la red").arg(names.value(to)),
                              cell);
                    b->setToolTip(L("Its daemon restarts.", "Su daemon se reinicia."));
                    connect(b, &QPushButton::clicked, this, [this, to] {
                        act(QStringLiteral("peers --listen 0.0.0.0 --on ") + url(to),
                            L("It listens on the network now.", "Ya escucha en la red."),
                            /*delayMs=*/8000);
                    });
                    cl->addWidget(b, 0, Qt::AlignLeft);
                } else if (pullOnly) {
                    keyButtons(cell, cl, from, L("Give it full keys", "Darle llaves completas"));
                } else if (state == QLatin1String("ok")) {
                    // Nothing to do.
                } else {
                    keyButtons(cell, cl, from,
                               state == QLatin1String("refused")
                                   ? L("Give keys again", "Darle llaves de nuevo")
                                   : L("Give keys", "Darle llaves"));
                }
            }
            // Keys for machines that are none of this client's: handed over by another one.
            if (checks != m_checks.constEnd()) {
                QStringList stray;
                for (const QJsonValue& v : *checks) {
                    const QJsonObject o = v.toObject();
                    const QString conn = o.value(QStringLiteral("connection")).toString();
                    if (conn.isEmpty() || conn == QLatin1String("-")) {
                        stray << o.value(QStringLiteral("id")).toString();
                    }
                }
                if (!stray.isEmpty()) {
                    auto* x =
                        muted(L("Also has keys of %1, which is none of your machines.",
                                "Tiene además llaves de %1, que no es ninguna de tus máquinas.")
                                  .arg(stray.join(QStringLiteral(", "))),
                              box);
                    g->addWidget(x, r + 1, ids.size() + 1);
                }
            }
        }
        for (int c = 1; c <= ids.size(); ++c) {
            g->setColumnStretch(c, 1);
        }
        lay->addWidget(box);
        auto* row = new QHBoxLayout();
        auto* again = small(L("Check again", "Comprobar de nuevo"), body);
        connect(again, &QPushButton::clicked, this, [this] {
            m_checks.clear();
            m_checkError.clear();
            render();
            load();
        });
        row->addWidget(again);
        auto* all = small(L("Give all the keys", "Dar todas las llaves"), body);
        all->setToolTip(L("Every machine gets the full keys of all the others.",
                          "Cada máquina recibe las llaves completas de todas las demás."));
        connect(all, &QPushButton::clicked, this, [this, all] {
            all->setEnabled(false);
            giveAllKeys(m_store, this, [this](const QStringList& given, const QStringList& failed) {
                Q_EMIT message(failed.isEmpty()
                                   ? L("Keys given to %1.", "Llaves entregadas a %1.")
                                         .arg(given.join(QStringLiteral(", ")))
                                   : L("Keys given to %1. Failed: %2",
                                       "Llaves entregadas a %1. "
                                       "Fallaron: %2")
                                         .arg(given.isEmpty() ? QStringLiteral("-")
                                                              : given.join(QStringLiteral(", ")),
                                              failed.join(QStringLiteral("; "))));
                m_checks.clear();
                m_checkError.clear();
                render();
                load();
            });
        });
        row->addWidget(all);
        row->addStretch(1);
        lay->addLayout(row);
    }

    // ── Opening ZFSMgr: the master password, and the FIDO2 security keys (any brand) registered to
    // open it besides it (`master-key`). Only with a master password: a key opens what it does.
    if (!m_keys.isEmpty()) {
        auto* head = new QHBoxLayout();
        head->addWidget(heading(L("Opening ZFSMgr", "Abrir ZFSMgr"), body));
        head->addStretch(1);
        auto* add = small(L("Add a security key…", "Añadir una llave de seguridad…"), body);
        add->setToolTip(
            L("A FIDO2 security key: touching it opens ZFSMgr instead of typing the "
              "master password, which keeps working.",
              "Una llave de seguridad FIDO2: tocarla abre ZFSMgr en vez de escribir "
              "la contraseña maestra, que sigue sirviendo."));
        connect(add, &QPushButton::clicked, this, &AdminPage::addKey);
        head->addWidget(add);
        lay->addLayout(head);
        QFrame* box = card(body);
        auto* g = new QGridLayout(box);
        int row = 0;
        for (const QJsonValue& v : m_keys) {
            const QJsonObject k = v.toObject();
            const bool password =
                k.value(QStringLiteral("type")).toString() == QLatin1String("password");
            g->addWidget(label(password ? L("Master password", "Contraseña maestra")
                                        : k.value(QStringLiteral("label")).toString(),
                               box),
                         row, 0);
            g->addWidget(muted(password ? L("always works", "sirve siempre")
                                        : L("key: touch it at start", "llave: tocarla al arrancar"),
                               box),
                         row, 1);
            if (!password) {
                const QString slot = k.value(QStringLiteral("slot")).toString();
                const QString name = k.value(QStringLiteral("label")).toString();
                auto* remove = small(L("Remove…", "Quitar…"), box);
                remove->setToolTip(
                    L("It will no longer open ZFSMgr. Nothing is deleted on the "
                      "key.",
                      "Dejará de abrir ZFSMgr. En la llave no se borra nada."));
                connect(remove, &QPushButton::clicked, this, [this, slot, name] {
                    act(QStringLiteral("master-key remove ") + slot,
                        L("Key %1 removed.", "Llave %1 quitada.").arg(name));
                });
                g->addWidget(remove, row, 2);
            }
            ++row;
        }
        g->setColumnStretch(1, 1);
        lay->addWidget(box);
    }

    // ── Language: the system's unless one is chosen; it applies on the next start, since every
    // text —the CLI's too— is chosen once.
    {
        lay->addWidget(heading(L("Language", "Idioma"), body));
        auto* row = new QHBoxLayout();
        auto* lang = new QComboBox(body);
        lang->addItem(L("The system's", "El del sistema"), QString());
        lang->addItem(QStringLiteral("Español"), QStringLiteral("es"));
        lang->addItem(QStringLiteral("English"), QStringLiteral("en"));
        lang->setCurrentIndex(
            qMax(0, lang->findData(QSettings().value(QStringLiteral("gui/lang")).toString())));
        connect(lang, &QComboBox::currentIndexChanged, this, [this, lang] {
            QSettings().setValue(QStringLiteral("gui/lang"), lang->currentData().toString());
            Q_EMIT message(L("The language changes the next time ZFSMgr is opened.",
                             "El idioma cambia la próxima vez que se abra ZFSMgr."));
        });
        row->addWidget(lang);
        row->addStretch(1);
        lay->addLayout(row);
    }

    replaceScrollWidget(m_scroll, body);
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
