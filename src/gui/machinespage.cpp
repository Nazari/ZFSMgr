#include "machinespage.h"

#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
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
            QStringLiteral("ls ") + url(machine), [this, machine](const CliResult& r) {
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
        QStringLiteral("ls -r ") + url(machine, pool), [this, machine, pool](const CliResult& r) {
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
        m_store->forMachine(id)->request(QStringLiteral("probe ") + url(id),
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
        m_probe.remove(id);
        act(QStringLiteral("install-daemon --on ") + url(id),
            L("Daemon installed.", "Daemon instalado."));
    });
    buttons->addWidget(daemon);
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

void MachinesPage::showPool(const QString& machine, const QString& pool) {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
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
    l->addWidget(stateLabel(status, w));
    auto* form = new QFormLayout();
    form->addRow(L("State", "Estado"), label(p.value(QStringLiteral("health")).toString(), w));
    form->addRow(L("Size", "Tamaño"), label(p.value(QStringLiteral("size")).toString(), w));
    form->addRow(L("Free", "Libre"), label(p.value(QStringLiteral("free")).toString(), w));
    form->addRow(L("In use", "Ocupado"), label(p.value(QStringLiteral("capacity")).toString(), w));
    l->addLayout(form);
    l->addStretch(1);
    setDetail(w);
}

void MachinesPage::showDataset(const QString& machine, const QString& dataset) {
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
    setDetail(w);
}

void MachinesPage::act(const QString& line, const QString& done, bool reloadTree) {
    m_store->general()->request(line, [this, done, reloadTree](const CliResult& r) {
        Q_EMIT message(r.ok() ? done : r.err.trimmed());
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
