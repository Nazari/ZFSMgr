#include "machinespage.h"

#include "addmachinedialog.h"
#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QDateTime>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QTabWidget>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace zfsmgr::gui {

namespace {

constexpr int kKindRole = Qt::UserRole;
constexpr int kMachineRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;

QJsonObject firstObject(const QString& out) {
    const qsizetype start = out.indexOf(QLatin1Char('{'));
    return start < 0 ? QJsonObject() : QJsonDocument::fromJson(out.mid(start).toUtf8()).object();
}

QString poolOf(const QString& dataset) {
    return dataset.section(QLatin1Char('/'), 0, 0);
}

QString url(const QString& machine, const QString& name = {}) {
    return QStringLiteral("zfsm://") + machine
           + (name.isEmpty() ? QString() : QStringLiteral("/") + name);
}

QString when(const QJsonValue& creation) {
    const qint64 t = creation.toVariant().toLongLong();
    return t > 0 ? QLocale().toString(QDateTime::fromSecsSinceEpoch(t), QLocale::ShortFormat)
                 : QString();
}

// A word for each of `probe`'s fixed codes.
QString probeWord(const QString& what, const QString& code) {
    if (code == QLatin1String("ok")) {
        return L("● fine", "● bien");
    }
    if (what == QLatin1String("sudo")) {
        if (code == QLatin1String("not_used")) {
            return L("not used", "no se usa");
        }
        if (code == QLatin1String("wrong_password")) {
            return L("✕ does not accept the stored password", "✕ no acepta la contraseña guardada");
        }
        if (code == QLatin1String("needs_password")) {
            return L("✕ asks for a password the connection does not have",
                     "✕ pide una contraseña que la conexión no tiene");
        }
    }
    if (what == QLatin1String("daemon")) {
        if (code == QLatin1String("outdated")) {
            return L("▲ outdated", "▲ anticuado");
        }
        if (code == QLatin1String("latest")) {
            return L("● the newest there is to install", "● el más nuevo que hay para instalar");
        }
        if (code == QLatin1String("unreachable")) {
            return L("✕ cannot be reached", "✕ no se llega a él");
        }
    }
    if (what == QLatin1String("openzfs")) {
        if (code == QLatin1String("too_old")) {
            return L("✕ too old", "✕ demasiado antiguo");
        }
        if (code == QLatin1String("missing")) {
            return L("✕ not installed", "✕ no está instalado");
        }
        if (code == QLatin1String("not_loaded")) {
            return L("✕ not loaded", "✕ no está cargado");
        }
    }
    return code;
}

QPushButton* button(const QString& text, QWidget* parent) {
    return new QPushButton(text, parent);
}

}  // namespace

MachinesPage::MachinesPage(StateStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    auto* outer = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    auto* title = heading(L("Machines", "Máquinas"), this);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    title->setFont(f);
    top->addWidget(title);
    top->addStretch(1);
    auto* add = button(L("+ Add", "+ Añadir"), this);
    connect(add, &QPushButton::clicked, this, &MachinesPage::addMachineRequested);
    top->addWidget(add);
    outer->addLayout(top);

    auto* split = new QSplitter(this);
    m_tree = new QTreeWidget(split);
    m_tree->setHeaderHidden(true);
    m_tree->setMinimumWidth(240);
    m_detail = new QScrollArea(split);
    m_detail->setWidgetResizable(true);
    m_detail->setFrameShape(QFrame::NoFrame);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 700});
    outer->addWidget(split, 1);

    // For captures without a screen: which dataset tab opens first.
    m_datasetTab = qEnvironmentVariableIntValue("ZFSMGR_GUI_TAB");
    connect(m_tree, &QTreeWidget::itemExpanded, this, &MachinesPage::expand);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &MachinesPage::showDetail);
    connect(m_store, &StateStore::changed, this, [this] {
        rebuildMachines();
        continueReveal();
    });
    rebuildMachines();
    showDetail();
}

void MachinesPage::rebuildMachines() {
    // Rebuilt only when the list of machines changes: a refresh every minute must not close
    // what the person opened.
    QStringList now;
    for (const MachineView& m : m_store->machines()) {
        now << m.id + (m.connected ? QStringLiteral("+") : QStringLiteral("-"));
    }
    QStringList shown;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        shown << m_tree->topLevelItem(i)->data(0, kNameRole).toString();
    }
    if (now == shown) {
        return;
    }
    m_tree->clear();
    for (const MachineView& m : m_store->machines()) {
        auto* it = new QTreeWidgetItem(m_tree);
        it->setText(0, m.connected ? m.name : m.name + L("  (disconnected)", "  (desconectada)"));
        it->setData(0, kKindRole, int(Machine));
        it->setData(0, kMachineRole, m.id);
        it->setData(0, kNameRole, m.id + (m.connected ? QStringLiteral("+") : QStringLiteral("-")));
        if (m.connected) {
            it->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
    }
}

void MachinesPage::expand(QTreeWidgetItem* item) {
    const int kind = item->data(0, kKindRole).toInt();
    const QString machine = item->data(0, kMachineRole).toString();
    if (kind == Machine && item->childCount() == 0) {
        m_store->forMachine(machine)->request(
            QStringLiteral("ls ") + url(machine), this, [this, machine](const CliResult& r) {
                const QJsonArray pools =
                    firstObject(r.out).value(QStringLiteral("pools")).toArray();
                m_pools.insert(machine, pools);
                // Looked up again: the tree may have been rebuilt while the answer travelled.
                QTreeWidgetItem* item = findItem(machine, QString());
                if (!item || item->childCount() > 0) {
                    return;
                }
                if (pools.isEmpty()) {
                    item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
                }
                for (const QJsonValue& v : pools) {
                    const QJsonObject p = v.toObject();
                    auto* pi = new QTreeWidgetItem(item);
                    pi->setText(0, p.value(QStringLiteral("name")).toString());
                    pi->setData(0, kKindRole, int(Pool));
                    pi->setData(0, kMachineRole, machine);
                    pi->setData(0, kNameRole, p.value(QStringLiteral("name")).toString());
                    if (p.value(QStringLiteral("imported")).toBool(true)) {
                        pi->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                    }
                }
                continueReveal();
            });
    }
    if (kind == Pool && item->childCount() == 0) {
        loadPool(item);
    }
}

void MachinesPage::loadPool(QTreeWidgetItem* poolItem) {
    const QString machine = poolItem->data(0, kMachineRole).toString();
    const QString pool = poolItem->data(0, kNameRole).toString();
    m_store->forMachine(machine)->request(
        QStringLiteral("ls -r ") + url(machine, pool), this,
        [this, machine, pool](const CliResult& r) {
            const QJsonArray entries =
                firstObject(r.out).value(QStringLiteral("entries")).toArray();
            m_entries.insert(machine + QLatin1Char('/') + pool, entries);
            QTreeWidgetItem* poolItem = findItem(machine, pool);
            if (!poolItem) {
                return;
            }
            // What was chosen inside, chosen again after rebuilding the branch.
            QString chosen;
            if (QTreeWidgetItem* cur = m_tree->currentItem();
                cur && cur->data(0, kMachineRole).toString() == machine
                && cur->data(0, kKindRole).toInt() == Dataset) {
                chosen = cur->data(0, kNameRole).toString();
            }
            // The datasets as a tree, by their names: «pool/a/b» hangs from «pool/a».
            const auto children = poolItem->takeChildren();
            qDeleteAll(children);
            QHash<QString, QTreeWidgetItem*> byName;
            byName.insert(pool, poolItem);
            for (const QJsonValue& v : entries) {
                const QJsonObject e = v.toObject();
                const QString name = e.value(QStringLiteral("name")).toString();
                if (e.value(QStringLiteral("type")).toString() == QLatin1String("snapshot")
                    || name == pool) {
                    continue;
                }
                QTreeWidgetItem* parent =
                    byName.value(name.section(QLatin1Char('/'), 0, -2), poolItem);
                auto* di = new QTreeWidgetItem(parent);
                di->setText(0, name.section(QLatin1Char('/'), -1));
                di->setData(0, kKindRole, int(Dataset));
                di->setData(0, kMachineRole, machine);
                di->setData(0, kNameRole, name);
                byName.insert(name, di);
            }
            if (poolItem->childCount() == 0) {
                poolItem->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
            }
            if (QTreeWidgetItem* again = byName.value(chosen); again && !chosen.isEmpty()) {
                m_tree->setCurrentItem(again);
            }
            continueReveal();
            showDetail();
        });
}

void MachinesPage::setDetail(QWidget* w) {
    m_detail->setWidget(w);
}

void MachinesPage::reveal(const QString& machine, const QString& dataset) {
    m_revealMachine = machine;
    m_revealDataset = dataset;
    continueReveal();
}

// One step further each time something arrives: the machine, its pools, the pool's datasets.
void MachinesPage::continueReveal() {
    if (m_revealMachine.isEmpty()) {
        return;
    }
    QTreeWidgetItem* mi = findItem(m_revealMachine, QString());
    if (!mi) {
        return;  // the list of machines has not arrived yet
    }
    if (m_revealDataset.isEmpty()) {
        m_tree->setCurrentItem(mi);
        m_revealMachine.clear();
        return;
    }
    mi->setExpanded(true);
    QTreeWidgetItem* pi = findItem(m_revealMachine, poolOf(m_revealDataset));
    if (!pi) {
        return;  // its pools are on their way
    }
    pi->setExpanded(true);
    const auto items = m_tree->findItems(m_revealDataset.section(QLatin1Char('/'), -1),
                                         Qt::MatchExactly | Qt::MatchRecursive);
    for (QTreeWidgetItem* it : items) {
        if (it->data(0, kMachineRole).toString() == m_revealMachine
            && it->data(0, kNameRole).toString() == m_revealDataset) {
            for (QTreeWidgetItem* p = it->parent(); p; p = p->parent()) {
                p->setExpanded(true);
            }
            m_tree->setCurrentItem(it);
            m_revealMachine.clear();
            m_revealDataset.clear();
            return;
        }
    }
}

QTreeWidgetItem* MachinesPage::findItem(const QString& machine, const QString& pool) const {
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* mi = m_tree->topLevelItem(i);
        if (mi->data(0, kMachineRole).toString() != machine) {
            continue;
        }
        if (pool.isEmpty()) {
            return mi;
        }
        for (int j = 0; j < mi->childCount(); ++j) {
            if (mi->child(j)->data(0, kNameRole).toString() == pool) {
                return mi->child(j);
            }
        }
    }
    return nullptr;
}

void MachinesPage::showDetail() {
    QTreeWidgetItem* it = m_tree->currentItem();
    if (!it) {
        auto* w = new QWidget();
        auto* l = new QVBoxLayout(w);
        l->addWidget(muted(
            L("Choose a machine, a pool or a dataset.", "Elige una máquina, un pool o un dataset."),
            w));
        l->addStretch(1);
        setDetail(w);
        return;
    }
    const QString machine = it->data(0, kMachineRole).toString();
    switch (it->data(0, kKindRole).toInt()) {
        case Machine:
            showMachine(machine);
            break;
        case Pool:
            showPool(machine, it->data(0, kNameRole).toString());
            break;
        case Dataset:
            showDataset(machine, it->data(0, kNameRole).toString());
            break;
    }
}

void MachinesPage::showMachine(const QString& id) {
    const MachineView* mv = nullptr;
    for (const MachineView& m : m_store->machines()) {
        if (m.id == id) {
            mv = &m;
        }
    }
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->addWidget(heading(mv ? mv->name : id, w));

    if (mv && !mv->connected) {
        l->addWidget(muted(L("Disconnected: the interface does not ask it anything until it is "
                             "connected again.",
                             "Desconectada: la interfaz no le pregunta nada hasta que se vuelva a "
                             "conectar."),
                           w));
    }
    const auto probeIt = m_probe.constFind(id);
    if (probeIt == m_probe.constEnd()) {
        l->addWidget(muted(L("Checking the machine…", "Comprobando la máquina…"), w));
        // Disconnected or not: checking is how one decides to connect it.
        m_store->forMachine(id)->request(QStringLiteral("probe ") + url(id), this,
                                         [this, id](const CliResult& r) {
                                             m_probe.insert(id, firstObject(r.out));
                                             showDetail();
                                         });
    } else {
        const QJsonObject p = *probeIt;
        auto* form = new QFormLayout();
        if (!p.value(QStringLiteral("reachable")).toBool()) {
            form->addRow(
                L("Answers", "Responde"),
                label(L("✕ no: %1", "✕ no: %1").arg(p.value(QStringLiteral("reason")).toString()),
                      w));
        } else {
            form->addRow(L("System", "Sistema"),
                         label(p.value(QStringLiteral("os_detail")).toString(), w));
            form->addRow(
                QStringLiteral("sudo"),
                label(probeWord(QStringLiteral("sudo"), p.value(QStringLiteral("sudo")).toString()),
                      w));
            form->addRow(
                QStringLiteral("OpenZFS"),
                label(p.value(QStringLiteral("openzfs_version")).toString() + QStringLiteral("  ")
                          + probeWord(QStringLiteral("openzfs"),
                                      p.value(QStringLiteral("openzfs")).toString()),
                      w));
            form->addRow(
                L("Daemon", "Daemon"),
                label(p.value(QStringLiteral("daemon_version")).toString() + QStringLiteral("  ")
                          + probeWord(QStringLiteral("daemon"),
                                      p.value(QStringLiteral("daemon")).toString()),
                      w));
        }
        l->addLayout(form);
        for (const QJsonValue& v : p.value(QStringLiteral("warnings")).toArray()) {
            const QJsonObject wv = v.toObject();
            const bool err = wv.value(QStringLiteral("level")).toString() == QLatin1String("error");
            auto* x = label((err ? QStringLiteral("✕ ") : QStringLiteral("▲ "))
                                + wv.value(QStringLiteral("text")).toString(),
                            w);
            x->setStyleSheet(
                QStringLiteral("color:%1")
                    .arg(colourOf(err ? QStringLiteral("error") : QStringLiteral("warning"))));
            l->addWidget(x);
        }
    }

    auto* buttons = new QHBoxLayout();
    auto* again = button(L("Check again", "Comprobar de nuevo"), w);
    connect(again, &QPushButton::clicked, this, [this, id] {
        m_probe.remove(id);
        showDetail();
    });
    buttons->addWidget(again);
    const bool connected = mv ? mv->connected : true;
    auto* conn = button(connected ? L("Disconnect", "Desconectar") : L("Connect", "Conectar"), w);
    connect(conn, &QPushButton::clicked, this, [this, id, connected] {
        act((connected ? QStringLiteral("disconnect ") : QStringLiteral("connect ")) + url(id),
            connected ? L("Disconnected.", "Desconectada.") : L("Connected.", "Conectada."));
    });
    buttons->addWidget(conn);
    auto* daemon =
        button(L("Install or update the daemon…", "Instalar o actualizar el daemon…"), w);
    connect(daemon, &QPushButton::clicked, this, [this, id] {
        // Checked again when the installation has FINISHED: asked before, the answer came
        // from the old daemon and stayed, so the button seemed to do nothing.
        act(QStringLiteral("install-daemon --on ") + url(id),
            L("Daemon installed.", "Daemon instalado."), false, [this, id] {
                m_probe.remove(id);
                showDetail();
            });
    });
    buttons->addWidget(daemon);
    if (m_advanced && connected) {
        auto* create = button(L("Create a pool…", "Crear un pool…"), w);
        connect(create, &QPushButton::clicked, this,
                [this, id] { Q_EMIT createPoolRequested(id); });
        buttons->addWidget(create);
        auto* import = button(L("Import a pool…", "Importar un pool…"), w);
        connect(import, &QPushButton::clicked, this, [this, id] { importPool(id); });
        buttons->addWidget(import);
    }
    auto* remove = button(L("Remove…", "Quitar…"), w);
    connect(remove, &QPushButton::clicked, this, [this, id] {
        act(QStringLiteral("destroy ") + url(id), L("Connection removed.", "Conexión quitada."));
    });
    buttons->addWidget(remove);
    buttons->addStretch(1);
    l->addLayout(buttons);
    l->addStretch(1);
    setDetail(w);
}

void MachinesPage::setAdvanced(bool on) {
    m_advanced = on;
    showDetail();
}

void MachinesPage::showPool(const QString& machine, const QString& pool) {
    const QString key = machine + QLatin1Char('/') + pool;
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->addWidget(muted(url(machine, pool), w));
    l->addWidget(heading(pool, w));
    QJsonObject p;
    for (const QJsonValue& v : m_pools.value(machine)) {
        if (v.toObject().value(QStringLiteral("name")).toString() == pool) {
            p = v.toObject();
        }
    }
    QString status = QStringLiteral("ok");
    for (const MachineView& m : m_store->machines()) {
        if (m.id != machine) {
            continue;
        }
        for (const QJsonValue& v : m.overview.value(QStringLiteral("pools")).toArray()) {
            if (v.toObject().value(QStringLiteral("name")).toString() == pool) {
                status = v.toObject().value(QStringLiteral("status")).toString();
            }
        }
    }
    auto* head = new QHBoxLayout();
    head->addWidget(stateLabel(status, w));
    head->addWidget(muted(L("%1 · %2 in use · %3 free", "%1 · %2 ocupado · quedan %3")
                              .arg(p.value(QStringLiteral("size")).toString(),
                                   p.value(QStringLiteral("capacity")).toString(),
                                   p.value(QStringLiteral("free")).toString()),
                          w));
    head->addStretch(1);
    l->addLayout(head);

    const auto st = m_status.constFind(key);
    if (st == m_status.constEnd()) {
        l->addWidget(muted(L("Asking for its disks…", "Preguntando por sus discos…"), w));
        m_store->forMachine(machine)->request(QStringLiteral("status ") + url(machine, pool), this,
                                              [this, key](const CliResult& r) {
                                                  m_status.insert(key, firstObject(r.out));
                                                  showDetail();
                                              });
    } else {
        // zpool's own explanation and advice, when there is something to say.
        const QPair<QString, QString> says[] = {
            {QStringLiteral("status"), L("What happens", "Qué pasa")},
            {QStringLiteral("action"), L("What to do", "Qué hacer")}};
        for (const auto& s : says) {
            const QString text = st->value(s.first).toString();
            if (!text.isEmpty()) {
                auto* t = label(s.second + QStringLiteral(": ") + text, w);
                if (status != QLatin1String("ok")) {
                    t->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(status)));
                }
                l->addWidget(t);
            }
        }
        const QJsonArray devs = st->value(QStringLiteral("devices")).toArray();
        auto* table = new QTableWidget(int(devs.size()), 3, w);
        table->setHorizontalHeaderLabels(
            {L("Disk", "Disco"), L("State", "Estado"),
             L("Errors (read/write/checksum)", "Errores (lectura/escritura/suma)")});
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        for (int i = 0; i < devs.size(); ++i) {
            const QJsonObject d = devs[i].toObject();
            const QString indent(2 * d.value(QStringLiteral("depth")).toInt(), QLatin1Char(' '));
            QString name = indent + d.value(QStringLiteral("name")).toString();
            const QString note = d.value(QStringLiteral("note")).toString();
            if (!note.isEmpty()) {
                name += QStringLiteral("  (") + note + QLatin1Char(')');
            }
            table->setItem(i, 0, new QTableWidgetItem(name));
            const QString s = d.value(QStringLiteral("state")).toString();
            auto* si = new QTableWidgetItem(s);
            const QString look = s == QLatin1String("ONLINE")     ? QStringLiteral("ok")
                                 : s.isEmpty()                    ? QString()
                                 : s == QLatin1String("DEGRADED") ? QStringLiteral("warning")
                                                                  : QStringLiteral("error");
            si->setForeground(QColor(colourOf(look)));
            table->setItem(i, 1, si);
            const QString errs = d.value(QStringLiteral("read")).toString().isEmpty()
                                     ? QString()
                                     : QStringLiteral("%1 / %2 / %3")
                                           .arg(d.value(QStringLiteral("read")).toString(),
                                                d.value(QStringLiteral("write")).toString(),
                                                d.value(QStringLiteral("cksum")).toString());
            table->setItem(i, 2, new QTableWidgetItem(errs));
        }
        table->setMinimumHeight(qMin(60 + 30 * int(devs.size()), 320));
        l->addWidget(table);
        const QString scan = st->value(QStringLiteral("scan")).toString();
        l->addWidget(muted(L("Last check: %1", "Última comprobación: %1")
                               .arg(scan.isEmpty() ? L("never", "nunca") : scan),
                           w));
        const QString errors = st->value(QStringLiteral("errors")).toString();
        if (!errors.isEmpty() && !errors.startsWith(QLatin1String("No known"))) {
            auto* e = label(errors, w);
            e->setStyleSheet(QStringLiteral("color:%1").arg(colourOf(QStringLiteral("error"))));
            l->addWidget(e);
        }
    }

    const auto again = [this, key](const QString& line, const QString& done) {
        m_status.remove(key);
        act(line, done, false);
    };
    // In rows of three: in one row they pushed the page wider than the window.
    auto* actions = new QGridLayout();
    int n = 0;
    const auto place = [actions, &n](QPushButton* b) {
        actions->addWidget(b, n / 3, n % 3);
        ++n;
    };
    auto* scrub = button(L("Check the data (scrub)", "Comprobar los datos (scrub)"), w);
    connect(scrub, &QPushButton::clicked, this, [again, machine, pool] {
        // With --on: the pool verbs do not take the URL as a loose argument.
        again(QStringLiteral("scrub --on ") + url(machine, pool),
              L("Checking: it reads the whole pool and fixes what it can.",
                "Comprobando: lee todo el pool y corrige lo que pueda."));
    });
    place(scrub);
    if (m_advanced) {
        struct More {
            QString verb;
            QString text;
            QString done;
        };
        const More more[] = {
            {QStringLiteral("trim"), L("Free blocks (trim)", "Liberar bloques (trim)"),
             L("Trimming: only useful on SSDs.", "Liberando: solo sirve en SSD.")},
            {QStringLiteral("clear"), L("Reset errors…", "Poner a cero los errores…"),
             L("Errors reset.", "Errores a cero.")},
            {QStringLiteral("upgrade"), L("Upgrade format…", "Actualizar formato…"),
             L("Pool upgraded.", "Pool actualizado.")},
            {QStringLiteral("reguid"), L("New identifier…", "Identificador nuevo…"),
             L("Identifier changed.", "Identificador cambiado.")},
            {QStringLiteral("export"), L("Export…", "Exportar…"),
             L("Exported: its disks can go to another machine.",
               "Exportado: sus discos pueden ir a otra máquina.")},
        };
        for (const More& m : more) {
            auto* b = button(m.text, w);
            const QString line = m.verb + QStringLiteral(" --on ") + url(machine, pool);
            const QString done = m.done;
            const bool leaves = m.verb == QLatin1String("export");
            connect(b, &QPushButton::clicked, this, [this, again, line, done, leaves, machine] {
                if (leaves) {
                    forgetPools(machine);
                }
                again(line, done);
            });
            place(b);
        }
    }
    actions->setColumnStretch(3, 1);
    l->addLayout(actions);
    l->addStretch(1);
    setDetail(w);
}

// The machine's pools are listed again on next opening: one left or one arrived.
void MachinesPage::forgetPools(const QString& machine) {
    m_pools.remove(machine);
    if (QTreeWidgetItem* mi = findItem(machine, QString())) {
        qDeleteAll(mi->takeChildren());
        mi->setExpanded(false);
        mi->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

void MachinesPage::importPool(const QString& machine) {
    m_store->forMachine(machine)->request(
        QStringLiteral("import ") + url(machine), this, [this, machine](const CliResult& r) {
            QStringList names;
            for (const QJsonValue& v :
                 firstObject(r.out).value(QStringLiteral("importable_pools")).toArray()) {
                const QJsonObject p = v.toObject();
                names << p.value(QStringLiteral("pool"))
                             .toString(p.value(QStringLiteral("name")).toString());
            }
            if (names.isEmpty()) {
                Q_EMIT message(
                    L("No pool to import on %1.", "No hay ningún pool que importar en %1.")
                        .arg(machine));
                return;
            }
            auto* d = new QInputDialog(this);
            d->setAttribute(Qt::WA_DeleteOnClose);
            d->setWindowTitle(L("Import a pool", "Importar un pool"));
            d->setLabelText(
                L("Pools that can be imported on %1:", "Pools que se pueden importar en %1:")
                    .arg(machine));
            d->setComboBoxItems(names);
            connect(d, &QInputDialog::textValueSelected, this,
                    [this, machine](const QString& name) {
                        forgetPools(machine);
                        act(QStringLiteral("import %1 --on %2").arg(name, url(machine)),
                            L("Pool %1 imported.", "Pool %1 importado.").arg(name), false);
                    });
            d->open();
        });
}

void MachinesPage::showDataset(const QString& machine, const QString& dataset) {
    // Tabs: the summary and the files for everyone; properties and organizing in advanced mode.
    // The chosen tab is kept: the detail is rebuilt every time an answer arrives.
    auto* tabs = new QTabWidget();
    tabs->addTab(datasetSummary(machine, dataset), L("Summary", "Resumen"));
    tabs->addTab(filesTab(machine, dataset), L("Files", "Ficheros"));
    if (m_advanced) {
        tabs->addTab(propertiesTab(machine, dataset), L("Properties", "Propiedades"));
        tabs->addTab(organizeTab(machine, dataset), L("Organize", "Organizar"));
    }
    tabs->setCurrentIndex(qMin(m_datasetTab, tabs->count() - 1));
    connect(tabs, &QTabWidget::currentChanged, this, [this](int i) { m_datasetTab = i; });
    setDetail(tabs);
}

QWidget* MachinesPage::filesTab(const QString& machine, const QString& dataset) {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    const QString key = machine + QLatin1Char('/') + dataset;
    const QString rel = m_filesPath.value(key);
    auto* top = new QHBoxLayout();
    auto* up = button(L("Up", "Subir"), w);
    up->setEnabled(!rel.isEmpty());
    connect(up, &QPushButton::clicked, this, [this, key, rel] {
        m_filesPath.insert(
            key, rel.contains(QLatin1Char('/')) ? rel.section(QLatin1Char('/'), 0, -2) : QString());
        showDetail();
    });
    top->addWidget(up);
    top->addWidget(muted(QLatin1Char('/') + rel, w), 1);
    l->addLayout(top);

    const QString listKey = key + QLatin1Char('#') + rel;
    const auto it = m_files.constFind(listKey);
    if (it == m_files.constEnd()) {
        l->addWidget(muted(L("Reading the directory…", "Leyendo el directorio…"), w));
        m_store->forMachine(machine)->request(
            QStringLiteral("ls ") + url(machine, dataset) + QStringLiteral("#content")
                + (rel.isEmpty() ? QString() : QLatin1Char('/') + rel),
            this, [this, listKey](const CliResult& r) {
                QJsonArray entries = firstObject(r.out).value(QStringLiteral("entries")).toArray();
                if (!r.ok() && entries.isEmpty()) {
                    QJsonObject why;
                    why.insert(QStringLiteral("error"), r.err.trimmed());
                    entries.append(why);
                }
                m_files.insert(listKey, entries);
                showDetail();
            });
        l->addStretch(1);
        return w;
    }
    const QJsonArray entries = *it;
    if (entries.size() == 1 && entries[0].toObject().contains(QStringLiteral("error"))) {
        l->addWidget(label(entries[0].toObject().value(QStringLiteral("error")).toString(), w));
        l->addStretch(1);
        return w;
    }
    auto* table = new QTableWidget(int(entries.size()), 3, w);
    table->setHorizontalHeaderLabels(
        {L("Name", "Nombre"), L("Size", "Tamaño"), L("Modified", "Modificado")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject e = entries[i].toObject();
        const bool dir = e.value(QStringLiteral("type")).toString() == QLatin1String("directory");
        auto* n = new QTableWidgetItem((dir ? QStringLiteral("▸ ") : QStringLiteral("  "))
                                       + e.value(QStringLiteral("name")).toString());
        n->setData(Qt::UserRole, dir ? e.value(QStringLiteral("name")).toString() : QString());
        table->setItem(i, 0, n);
        table->setItem(
            i, 1,
            new QTableWidgetItem(
                dir ? QString()
                    : bytesText(e.value(QStringLiteral("size")).toVariant().toLongLong())));
        table->setItem(i, 2, new QTableWidgetItem(e.value(QStringLiteral("modified")).toString()));
    }
    // Into a directory with a double click.
    connect(table, &QTableWidget::cellDoubleClicked, this, [this, table, key, rel](int row, int) {
        const QString sub = table->item(row, 0)->data(Qt::UserRole).toString();
        if (!sub.isEmpty()) {
            m_filesPath.insert(key, rel.isEmpty() ? sub : rel + QLatin1Char('/') + sub);
            showDetail();
        }
    });
    l->addWidget(table, 1);
    l->addWidget(muted(
        L("Double click a directory to go into it.", "Doble clic en un directorio para entrar."),
        w));
    return w;
}

QWidget* MachinesPage::propertiesTab(const QString& machine, const QString& dataset) {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->addWidget(muted(L("Every ZFS property with where it comes from: set here (local), inherited "
                         "from a parent, or the default. Double click a value to change it.",
                         "Todas las propiedades de ZFS con su origen: puesta aquí (local), "
                         "heredada de un padre o la de por defecto. Doble clic en un valor para "
                         "cambiarlo."),
                       w));
    const QString key = machine + QLatin1Char('/') + dataset;
    const auto it = m_props.constFind(key);
    if (it == m_props.constEnd()) {
        l->addWidget(muted(L("Reading its properties…", "Leyendo sus propiedades…"), w));
        m_store->forMachine(machine)->request(
            QStringLiteral("ls ") + url(machine, dataset) + QStringLiteral("#properties"), this,
            [this, key](const CliResult& r) {
                m_props.insert(key,
                               firstObject(r.out).value(QStringLiteral("properties")).toArray());
                showDetail();
            });
        l->addStretch(1);
        return w;
    }
    const QJsonArray props = *it;
    auto* table = new QTableWidget(int(props.size()), 3, w);
    table->setHorizontalHeaderLabels(
        {L("Property", "Propiedad"), L("Value", "Valor"), L("From", "Origen")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    for (int i = 0; i < props.size(); ++i) {
        const QJsonObject p = props[i].toObject();
        table->setItem(i, 0, new QTableWidgetItem(p.value(QStringLiteral("property")).toString()));
        table->setItem(i, 1, new QTableWidgetItem(p.value(QStringLiteral("value")).toString()));
        table->setItem(i, 2, new QTableWidgetItem(p.value(QStringLiteral("source")).toString()));
    }
    // Changing one: a value sets it here; empty puts it back to inherited. The CLI refuses what
    // cannot be changed (a computed property) and says why.
    connect(
        table, &QTableWidget::cellDoubleClicked, this,
        [this, table, machine, dataset, key](int row, int) {
            const QString prop = table->item(row, 0)->text();
            auto* d = new QInputDialog(this);
            d->setAttribute(Qt::WA_DeleteOnClose);
            d->setWindowTitle(prop);
            d->setLabelText(
                L("New value for %1 (empty: inherit it):", "Valor nuevo de %1 (vacío: heredarlo):")
                    .arg(prop));
            d->setTextValue(table->item(row, 1)->text());
            connect(d, &QInputDialog::textValueSelected, this,
                    [this, prop, machine, dataset, key](const QString& v) {
                        m_props.remove(key);
                        const QString on = QStringLiteral(" --on ") + url(machine, dataset);
                        act(v.trimmed().isEmpty()
                                ? QStringLiteral("inherit ") + prop + on
                                : QStringLiteral("set %1=%2").arg(prop, quoted(v.trimmed())) + on,
                            L("%1 changed.", "%1 cambiada.").arg(prop), false);
                    });
            d->open();
        });
    l->addWidget(table, 1);
    return w;
}

QWidget* MachinesPage::organizeTab(const QString& machine, const QString& dataset) {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    const QString on = QStringLiteral(" --on ") + url(machine, dataset);
    const QString key = machine + QLatin1Char('/') + dataset;

    // ── Directories into datasets (breakdown).
    l->addWidget(heading(L("Turn directories into datasets", "Convertir carpetas en datasets"), w));
    l->addWidget(muted(L("Each directory chosen becomes a child dataset in the same place: from "
                         "outside it looks the same, but it can have its own snapshots, quota and "
                         "copies. Every file is moved once and the originals are deleted at the "
                         "end, when all copies are verified. It runs as a job.",
                         "Cada carpeta marcada pasa a ser un dataset hijo en el mismo sitio: desde "
                         "fuera se ve igual, pero puede tener sus propios snapshots, cuota y "
                         "copias. Cada fichero se mueve una sola vez y los originales se borran "
                         "al final, cuando todas las copias están verificadas. Se hace como "
                         "trabajo."),
                       w));
    const auto dirs = m_breakdown.constFind(key);
    if (dirs == m_breakdown.constEnd()) {
        l->addWidget(muted(L("Measuring its directories…", "Midiendo sus carpetas…"), w));
        m_store->forMachine(machine)->request(
            QStringLiteral("breakdown --list") + on, this, [this, key](const CliResult& r) {
                QJsonArray d = firstObject(r.out).value(QStringLiteral("directories")).toArray();
                m_breakdown.insert(key, d);
                showDetail();
            });
    } else {
        auto* table = new QTableWidget(int(dirs->size()), 4, w);
        table->setHorizontalHeaderLabels({L("Directory", "Carpeta"), L("Uses", "Ocupa"),
                                          L("Files", "Ficheros"),
                                          L("New dataset", "Dataset nuevo")});
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        for (int i = 0; i < dirs->size(); ++i) {
            const QJsonObject d = (*dirs)[i].toObject();
            const QString dir = d.value(QStringLiteral("dir")).toString();
            auto* c = new QTableWidgetItem(dir);
            c->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            c->setCheckState(Qt::Unchecked);
            table->setItem(i, 0, c);
            auto* b = new QTableWidgetItem(
                bytesText(d.value(QStringLiteral("bytes")).toVariant().toLongLong()));
            b->setFlags(Qt::ItemIsEnabled);
            table->setItem(i, 1, b);
            auto* f =
                new QTableWidgetItem(QString::number(d.value(QStringLiteral("files")).toInt()));
            f->setFlags(Qt::ItemIsEnabled);
            table->setItem(i, 2, f);
            // A dataset name cannot hold «/» as a level that does not exist, nor a leading dot.
            QString name = dir;
            name.replace(QLatin1Char('/'), QLatin1Char(':'));
            while (name.startsWith(QLatin1Char('.'))) {
                name.remove(0, 1);
            }
            table->setItem(i, 3, new QTableWidgetItem(name));
        }
        table->setMinimumHeight(qMin(60 + 30 * int(dirs->size()), 300));
        l->addWidget(table);
        auto* go = button(L("Turn into datasets…", "Convertir en datasets…"), w);
        connect(go, &QPushButton::clicked, this, [this, table, on, key] {
            QStringList pairs;
            for (int i = 0; i < table->rowCount(); ++i) {
                if (table->item(i, 0)->checkState() == Qt::Checked) {
                    pairs << quoted(table->item(i, 0)->text())
                          << quoted(table->item(i, 3)->text().trimmed());
                }
            }
            if (pairs.isEmpty()) {
                Q_EMIT message(L("Tick the directories first.", "Marca antes las carpetas."));
                return;
            }
            m_breakdown.remove(key);
            act(QStringLiteral("breakdown ") + pairs.join(QLatin1Char(' ')) + on,
                L("Running as a job: follow it in Jobs.",
                  "En marcha como trabajo: síguelo en Trabajos."));
        });
        l->addWidget(go, 0, Qt::AlignLeft);
    }

    // ── Child datasets back into directories (assemble).
    QStringList children;
    for (const QJsonValue& v : m_entries.value(machine + QLatin1Char('/') + poolOf(dataset))) {
        const QJsonObject e = v.toObject();
        const QString name = e.value(QStringLiteral("name")).toString();
        if (e.value(QStringLiteral("type")).toString() == QLatin1String("dataset")
            && name.startsWith(dataset + QLatin1Char('/'))
            && !name.mid(dataset.size() + 1).contains(QLatin1Char('/'))) {
            children << name.mid(dataset.size() + 1);
        }
    }
    l->addWidget(heading(
        L("Turn child datasets into directories", "Convertir datasets hijos en carpetas"), w));
    l->addWidget(muted(L("The opposite: a child dataset becomes an ordinary directory again; its "
                         "files stay where they are. Its own snapshots, quota and properties are "
                         "lost, and its scheduled copies stop.",
                         "Lo contrario: un dataset hijo vuelve a ser una carpeta normal; los "
                         "ficheros se quedan donde están. Se pierden sus snapshots propios, su "
                         "cuota y sus propiedades, y sus copias programadas dejan de hacerse."),
                       w));
    if (children.isEmpty()) {
        l->addWidget(muted(L("It has no child datasets.", "No tiene datasets hijos."), w));
    } else {
        auto* list = new QListWidget(w);
        for (const QString& c : children) {
            auto* it = new QListWidgetItem(c, list);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            it->setCheckState(Qt::Unchecked);
        }
        list->setMaximumHeight(qMin(30 + 24 * int(children.size()), 200));
        l->addWidget(list);
        auto* go = button(L("Turn into directories…", "Convertir en carpetas…"), w);
        connect(go, &QPushButton::clicked, this, [this, list, on] {
            QStringList chosen;
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->checkState() == Qt::Checked) {
                    chosen << quoted(list->item(i)->text());
                }
            }
            if (chosen.isEmpty()) {
                Q_EMIT message(L("Tick the datasets first.", "Marca antes los datasets."));
                return;
            }
            act(QStringLiteral("assemble ") + chosen.join(QLatin1Char(' ')) + on,
                L("Running as a job: follow it in Jobs.",
                  "En marcha como trabajo: síguelo en Trabajos."));
        });
        l->addWidget(go, 0, Qt::AlignLeft);
    }

    // ── Into an ordinary directory (todir).
    l->addWidget(heading(L("Copy to an ordinary directory", "Volcar a una carpeta"), w));
    l->addWidget(
        muted(L("Copies all its contents to an ordinary directory of the same machine, for "
                "instance to take it out of ZFS or to an external disk.",
                "Copia todo su contenido a una carpeta corriente de la misma máquina, por "
                "ejemplo para sacarlo de ZFS o llevarlo a un disco externo."),
              w));
    {
        auto* row = new QHBoxLayout();
        auto* dir = new QLineEdit(w);
        dir->setPlaceholderText(L("destination directory, e.g. /mnt/disk/media",
                                  "carpeta de destino, p. ej. /mnt/disco/media"));
        auto* del = new QCheckBox(
            L("Destroy the dataset afterwards", "Destruir el dataset al terminar"), w);
        auto* go = button(L("Copy to the directory…", "Volcar a la carpeta…"), w);
        connect(go, &QPushButton::clicked, this, [this, dir, del, on] {
            if (dir->text().trimmed().isEmpty()) {
                return;
            }
            act(QStringLiteral("todir ") + quoted(dir->text().trimmed())
                    + (del->isChecked() ? QStringLiteral(" --delete-source") : QString()) + on,
                L("Running as a job: follow it in Jobs.",
                  "En marcha como trabajo: síguelo en Trabajos."));
        });
        row->addWidget(dir, 1);
        row->addWidget(del);
        row->addWidget(go);
        l->addLayout(row);
    }

    // ── From an ordinary directory (fromdir), of this or another machine.
    l->addWidget(heading(L("Bring a directory in", "Traer una carpeta"), w));
    l->addWidget(muted(L("Brings the contents of an ordinary directory —of this machine or "
                         "another— into this dataset. Deleting the source happens only after the "
                         "copy is checked identical.",
                         "Trae el contenido de una carpeta corriente —de esta máquina o de otra— "
                         "a este dataset. Borrar el origen solo ocurre tras comprobar que la copia "
                         "es idéntica."),
                       w));
    {
        auto* form = new QFormLayout();
        auto* from = new QComboBox(w);
        for (const MachineView& m : m_store->machines()) {
            if (m.connected) {
                from->addItem(m.name, m.id);
            }
        }
        from->setCurrentIndex(qMax(0, from->findData(machine)));
        form->addRow(L("From machine", "De la máquina"), from);
        auto* dir = new QLineEdit(w);
        dir->setPlaceholderText(L("source directory", "carpeta de origen"));
        form->addRow(L("Directory", "Carpeta"), dir);
        auto* sub = new QLineEdit(w);
        sub->setPlaceholderText(
            L("optional: where inside this dataset", "opcional: dónde dentro de este dataset"));
        form->addRow(L("Inside", "Dentro de"), sub);
        auto* del = new QCheckBox(
            L("Delete the source afterwards", "Borrar la carpeta de origen al terminar"), w);
        form->addRow(QString(), del);
        l->addLayout(form);
        auto* go = button(L("Bring the directory…", "Traer la carpeta…"), w);
        connect(go, &QPushButton::clicked, this, [this, from, dir, sub, del, on] {
            const QString d = dir->text().trimmed();
            if (d.isEmpty()) {
                return;
            }
            // The source as a URL with its machine: zfsm://<machine>#path/<directory>.
            QString line = QStringLiteral("fromdir ")
                           + quoted(QStringLiteral("zfsm://") + from->currentData().toString()
                                    + QStringLiteral("#path")
                                    + (d.startsWith(QLatin1Char('/')) ? d : QLatin1Char('/') + d));
            if (!sub->text().trimmed().isEmpty()) {
                line += QStringLiteral(" --subdir ") + quoted(sub->text().trimmed());
            }
            if (del->isChecked()) {
                line += QStringLiteral(" --delete-source");
            }
            act(line + on, L("Copied.", "Copiado."));
        });
        l->addWidget(go, 0, Qt::AlignLeft);
    }
    l->addStretch(1);
    return w;
}

QWidget* MachinesPage::datasetSummary(const QString& machine, const QString& dataset) {
    const QJsonArray entries = m_entries.value(machine + QLatin1Char('/') + poolOf(dataset));
    QJsonObject ds;
    QVector<QJsonObject> snaps;
    for (const QJsonValue& v : entries) {
        const QJsonObject e = v.toObject();
        const QString name = e.value(QStringLiteral("name")).toString();
        if (name == dataset) {
            ds = e;
        } else if (name.startsWith(dataset + QLatin1Char('@'))) {
            snaps.push_back(e);
        }
    }
    std::sort(snaps.begin(), snaps.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return a.value(QStringLiteral("creation")).toVariant().toLongLong()
               > b.value(QStringLiteral("creation")).toVariant().toLongLong();
    });

    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->addWidget(muted(url(machine, dataset), w));
    l->addWidget(heading(dataset.section(QLatin1Char('/'), -1), w));

    auto* actions = new QHBoxLayout();
    auto* snapNow = button(L("Snapshot now", "Snapshot ahora"), w);
    connect(snapNow, &QPushButton::clicked, this, [this, machine, dataset] {
        const QString name =
            QStringLiteral("manual-")
            + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        act(QStringLiteral("create @%1 --on %2").arg(name, url(machine, dataset)),
            L("Snapshot %1 taken.", "Hecho el snapshot %1.").arg(name));
    });
    actions->addWidget(snapNow);
    auto* copy = button(L("Copy…", "Copiar…"), w);
    connect(copy, &QPushButton::clicked, this,
            [this, machine, dataset] { Q_EMIT copyRequested(machine, dataset); });
    actions->addWidget(copy);
    auto* destroy = button(L("Destroy…", "Destruir…"), w);
    connect(destroy, &QPushButton::clicked, this, [this, machine, dataset] {
        // The CLI says what is lost and asks for the name: the dialog shows it.
        act(QStringLiteral("destroy -r ") + url(machine, dataset), L("Destroyed.", "Destruido."));
    });
    actions->addWidget(destroy);
    actions->addStretch(1);
    l->addLayout(actions);

    auto* form = new QFormLayout();
    form->addRow(L("Uses", "Ocupa"),
                 label(bytesText(ds.value(QStringLiteral("used")).toVariant().toLongLong()), w));
    const QString mp = ds.value(QStringLiteral("mountpoint")).toString();
    form->addRow(L("Mounted at", "Montado en"),
                 label(ds.value(QStringLiteral("mounted")).toString() == QLatin1String("yes")
                           ? mp
                           : L("not mounted", "sin montar"),
                       w));
    const QString enc = ds.value(QStringLiteral("encryption")).toString();
    form->addRow(L("Encrypted", "Cifrado"),
                 label(enc.isEmpty() || enc == QLatin1String("off") ? L("no", "no") : enc, w));
    form->addRow(L("Snapshots", "Snapshots"), label(QString::number(snaps.size()), w));
    QStringList copies;
    const QString here = url(machine, dataset);
    for (const ScheduleRow& r : scheduleRows(m_store->machines())) {
        if (r.kind == QLatin1String("pull") && r.peer == here) {
            copies << L("%1 pulls it into %2", "la trae %1 a %2").arg(r.machine, r.dataset);
        } else if (r.kind == QLatin1String("push") && r.machineId == machine
                   && r.dataset == dataset) {
            copies << L("sent to %1", "se envía a %1").arg(r.peer);
        } else if (r.kind == QLatin1String("pull") && r.machineId == machine
                   && r.dataset == dataset) {
            copies << L("it is a copy pulled from %1", "es una copia traída de %1").arg(r.peer);
        }
    }
    form->addRow(
        L("Copies", "Copias"),
        label(copies.isEmpty() ? L("none", "ninguna") : copies.join(QStringLiteral("\n")), w));
    l->addLayout(form);

    l->addWidget(heading(L("Snapshots", "Snapshots"), w));
    if (snaps.isEmpty()) {
        l->addWidget(muted(L("None.", "Ninguno."), w));
    } else {
        auto* table = new QTableWidget(int(snaps.size()), 3, w);
        table->setHorizontalHeaderLabels(
            {L("Snapshot", "Snapshot"), L("Taken", "Creado"), L("Uses", "Ocupa")});
        table->verticalHeader()->setVisible(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        for (int i = 0; i < snaps.size(); ++i) {
            const QJsonObject& s = snaps[i];
            const QString full = s.value(QStringLiteral("name")).toString();
            auto* n = new QTableWidgetItem(full.section(QLatin1Char('@'), 1));
            n->setData(Qt::UserRole, full);
            table->setItem(i, 0, n);
            table->setItem(i, 1, new QTableWidgetItem(when(s.value(QStringLiteral("creation")))));
            table->setItem(i, 2,
                           new QTableWidgetItem(bytesText(
                               s.value(QStringLiteral("used")).toVariant().toLongLong())));
        }
        table->setMinimumHeight(220);
        l->addWidget(table);
        auto* snapActions = new QHBoxLayout();
        auto* back = button(L("Go back to this one…", "Volver a este…"), w);
        auto* del = button(L("Delete…", "Borrar…"), w);
        back->setEnabled(false);
        del->setEnabled(false);
        connect(table, &QTableWidget::itemSelectionChanged, w, [table, back, del] {
            const bool any = table->currentRow() >= 0;
            back->setEnabled(any);
            del->setEnabled(any);
        });
        const auto chosen = [table]() {
            const int row = table->currentRow();
            return row < 0 ? QString() : table->item(row, 0)->data(Qt::UserRole).toString();
        };
        connect(back, &QPushButton::clicked, this, [this, machine, chosen] {
            const QString s = chosen();
            if (!s.isEmpty()) {
                // -r: the later snapshots go too; the CLI lists them and asks for the name.
                act(QStringLiteral("rollback -r ") + url(machine, s),
                    L("Back to %1.", "Vuelto a %1.").arg(s));
            }
        });
        connect(del, &QPushButton::clicked, this, [this, machine, chosen] {
            const QString s = chosen();
            if (!s.isEmpty()) {
                act(QStringLiteral("destroy ") + url(machine, s),
                    L("Snapshot deleted.", "Snapshot borrado."));
            }
        });
        snapActions->addWidget(back);
        snapActions->addWidget(del);
        snapActions->addStretch(1);
        l->addLayout(snapActions);
    }
    l->addStretch(1);
    return w;
}

void MachinesPage::act(const QString& line, const QString& done, bool reloadTree,
                       std::function<void()> then) {
    m_store->general()->request(line, this, [this, done, reloadTree, then](const CliResult& r) {
        Q_EMIT message(r.ok() ? done : r.err.trimmed());
        if (then) {
            then();
        }
        if (reloadTree) {
            // The open pools are asked again: what changed is in one of them.
            for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* mi = m_tree->topLevelItem(i);
                for (int j = 0; j < mi->childCount(); ++j) {
                    QTreeWidgetItem* pi = mi->child(j);
                    if (pi->isExpanded() || pi->childCount() > 0) {
                        loadPool(pi);
                    }
                }
            }
        }
        m_store->refresh();
    });
}

}  // namespace zfsmgr::gui
