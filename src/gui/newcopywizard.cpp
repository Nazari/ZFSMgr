#include "newcopywizard.h"

#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QButtonGroup>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace zfsmgr::gui {

namespace {

enum Step { kWhat, kWhere, kHow, kReview, kRunning };

QJsonObject firstObject(const QString& out) {
    const qsizetype start = out.indexOf(QLatin1Char('{'));
    return start < 0 ? QJsonObject() : QJsonDocument::fromJson(out.mid(start).toUtf8()).object();
}

QSpinBox* keepSpin(int value, QWidget* parent) {
    auto* s = new QSpinBox(parent);
    s->setRange(0, 999);
    s->setValue(value);
    return s;
}

}  // namespace

NewCopyWizard::NewCopyWizard(StateStore* store, QWidget* parent) : QDialog(parent), m_store(store) {
    setWindowTitle(L("New copy", "Nueva copia"));
    resize(720, 560);
    auto* lay = new QVBoxLayout(this);
    m_stepTitle = heading(QString(), this);
    lay->addWidget(m_stepTitle);
    m_steps = new QStackedWidget(this);
    lay->addWidget(m_steps, 1);

    // 1. What.
    {
        auto* w = new QWidget(m_steps);
        auto* l = new QVBoxLayout(w);
        l->addWidget(muted(L("Choose the dataset to copy. It is copied with everything inside it.",
                             "Elige el dataset de origen. Se copia con todo lo que tiene dentro."),
                           w));
        m_filter = new QLineEdit(w);
        m_filter->setPlaceholderText(L("Filter…", "Filtrar…"));
        l->addWidget(m_filter);
        m_origins = new QListWidget(w);
        l->addWidget(m_origins, 1);
        connect(m_filter, &QLineEdit::textChanged, this, &NewCopyWizard::fillOrigins);
        connect(m_origins, &QListWidget::currentRowChanged, this,
                [this] { m_next->setEnabled(chosenOrigin() != nullptr); });
        m_steps->addWidget(w);
    }
    // 2. Where.
    {
        auto* w = new QWidget(m_steps);
        auto* l = new QVBoxLayout(w);
        l->addWidget(muted(L("The machine that will keep the copy. Usually not the origin's.",
                             "La máquina que guardará la copia. Lo normal es una distinta del "
                             "origen."),
                           w));
        m_dests = new QListWidget(w);
        l->addWidget(m_dests, 1);
        auto* form = new QFormLayout();
        m_name = new QLineEdit(w);
        form->addRow(L("Name of the copy", "Nombre de la copia"), m_name);
        l->addLayout(form);
        connect(m_dests, &QListWidget::currentRowChanged, this, [this] {
            const Pool* p = chosenDest();
            const Dataset* o = chosenOrigin();
            if (p && o) {
                // Same path under the destination's pool: ppal/user/srvs → back/user/srvs.
                const qsizetype slash = o->name.indexOf(QLatin1Char('/'));
                m_name->setText(p->name + QLatin1Char('/')
                                + (slash < 0 ? o->name : o->name.mid(slash + 1)));
            }
            m_next->setEnabled(p != nullptr);
        });
        m_steps->addWidget(w);
    }
    // 3. How.
    {
        auto* w = new QWidget(m_steps);
        auto* l = new QVBoxLayout(w);
        m_once = new QRadioButton(L("Copy once, now", "Copiar una vez, ahora"), w);
        l->addWidget(m_once);
        l->addWidget(muted(L("A picture of how it is now. It does not update itself.",
                             "Una foto del estado actual. No se actualiza sola."),
                           w));
        m_keepUp = new QRadioButton(L("Keep it up to date", "Mantenerla al día"), w);
        l->addWidget(m_keepUp);
        l->addWidget(muted(L("Snapshots are taken regularly and each one reaches the copy.",
                             "Se hacen snapshots periódicos y cada uno llega a la copia."),
                           w));
        auto* modes = new QButtonGroup(w);
        modes->addButton(m_once);
        modes->addButton(m_keepUp);
        m_keepUp->setChecked(true);

        m_keepBox = new QWidget(w);
        auto* k = new QVBoxLayout(m_keepBox);
        k->addWidget(heading(L("Who makes the copy", "Quién hace la copia"), m_keepBox));
        m_pull = new QRadioButton(L("The copies machine pulls it (recommended)",
                                    "La máquina de copias la trae (recomendado)"),
                                  m_keepBox);
        m_push = new QRadioButton(L("The origin sends it", "El origen la envía"), m_keepBox);
        auto* who = new QButtonGroup(m_keepBox);
        who->addButton(m_pull);
        who->addButton(m_push);
        m_pull->setChecked(true);
        k->addWidget(m_pull);
        k->addWidget(muted(L("It asks the origin for what is new. From it the original cannot be "
                             "deleted, even if someone gets into it.",
                             "Pide lo nuevo al origen. Desde ella no se puede borrar el original, "
                             "aunque alguien entre en ella."),
                           m_keepBox));
        k->addWidget(m_push);
        k->addWidget(muted(L("The origin sends it after each snapshot. The copy keeps exactly what "
                             "the origin keeps.",
                             "El origen la manda tras cada snapshot. La copia guarda exactamente "
                             "lo mismo que el origen."),
                           m_keepBox));
        k->addWidget(heading(L("How many the copy keeps", "Cuántas guarda la copia"), m_keepBox));
        auto* nums = new QHBoxLayout();
        m_daily = keepSpin(7, m_keepBox);
        m_weekly = keepSpin(4, m_keepBox);
        m_monthly = keepSpin(6, m_keepBox);
        m_yearly = keepSpin(1, m_keepBox);
        nums->addWidget(label(L("Daily", "Diarias"), m_keepBox, false));
        nums->addWidget(m_daily);
        nums->addWidget(label(L("Weekly", "Semanales"), m_keepBox, false));
        nums->addWidget(m_weekly);
        nums->addWidget(label(L("Monthly", "Mensuales"), m_keepBox, false));
        nums->addWidget(m_monthly);
        nums->addWidget(label(L("Yearly", "Anuales"), m_keepBox, false));
        nums->addWidget(m_yearly);
        nums->addStretch(1);
        k->addLayout(nums);
        l->addWidget(m_keepBox);
        l->addStretch(1);
        connect(m_keepUp, &QRadioButton::toggled, m_keepBox, &QWidget::setEnabled);
        m_steps->addWidget(w);
    }
    // 4. Review.
    {
        auto* w = new QWidget(m_steps);
        auto* l = new QVBoxLayout(w);
        m_summary = label(QString(), w);
        l->addWidget(m_summary);
        l->addWidget(heading(L("Commands that will run", "Órdenes que se ejecutarán"), w));
        m_commands = new QPlainTextEdit(w);
        m_commands->setReadOnly(true);
        QFont mono(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::TypeWriter);
        m_commands->setFont(mono);
        l->addWidget(m_commands, 1);
        m_steps->addWidget(w);
    }
    // 5. Running.
    {
        auto* w = new QWidget(m_steps);
        auto* l = new QVBoxLayout(w);
        m_progress = new QListWidget(w);
        l->addWidget(m_progress, 1);
        m_steps->addWidget(w);
    }

    auto* buttons = new QHBoxLayout();
    m_back = new QPushButton(L("Back", "Atrás"), this);
    m_cancel = new QPushButton(L("Cancel", "Cancelar"), this);
    m_next = new QPushButton(L("Next", "Siguiente"), this);
    m_next->setDefault(true);
    buttons->addWidget(m_back);
    buttons->addStretch(1);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_next);
    lay->addLayout(buttons);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_back, &QPushButton::clicked, this, [this] { goTo(m_steps->currentIndex() - 1); });
    connect(m_next, &QPushButton::clicked, this, [this] {
        const int step = m_steps->currentIndex();
        if (step == kReview) {
            run();
            return;
        }
        if (step == kRunning) {
            accept();
            return;
        }
        goTo(step + 1);
    });

    goTo(kWhat);
    load();
}

void NewCopyWizard::load() {
    for (const MachineView& m : m_store->machines()) {
        if (!m.reachable()) {
            continue;
        }
        const QString id = m.id;
        const QString name = m.name;
        CliService* s = m_store->forMachine(id);
        ++m_pending;
        s->request(QStringLiteral("peers zfsm://") + id, this, [this, id](const CliResult& r) {
            QHash<QString, QString> roles;
            for (const QJsonValue& v :
                 firstObject(r.out).value(QStringLiteral("peers")).toArray()) {
                const QJsonObject o = v.toObject();
                QString role = o.value(QStringLiteral("role")).toString();
                if (role.isEmpty() || role == QLatin1String("-")) {
                    role = QStringLiteral("known");
                }
                roles.insert(o.value(QStringLiteral("uid")).toString(), role);
            }
            m_reaches.insert(id, roles);
            --m_pending;
        });
        ++m_pending;
        s->request(
            QStringLiteral("ls zfsm://") + id, this, [this, s, id, name](const CliResult& r) {
                --m_pending;
                for (const QJsonValue& v :
                     firstObject(r.out).value(QStringLiteral("pools")).toArray()) {
                    const QJsonObject p = v.toObject();
                    const QString pool = p.value(QStringLiteral("name")).toString();
                    if (!p.value(QStringLiteral("imported")).toBool(true) || pool.isEmpty()) {
                        continue;
                    }
                    m_pools.push_back({id, name, pool, p.value(QStringLiteral("free")).toString()});
                    ++m_pending;
                    s->request(
                        QStringLiteral("ls -r zfsm://%1/%2").arg(id, pool), this,
                        [this, id, name](const CliResult& rr) {
                            --m_pending;
                            for (const QJsonValue& e :
                                 firstObject(rr.out).value(QStringLiteral("entries")).toArray()) {
                                const QJsonObject d = e.toObject();
                                if (d.value(QStringLiteral("type")).toString()
                                    != QLatin1String("dataset")) {
                                    continue;
                                }
                                const QString enc =
                                    d.value(QStringLiteral("encryption")).toString();
                                m_datasets.push_back(
                                    {id, name, d.value(QStringLiteral("name")).toString(),
                                     d.value(QStringLiteral("used")).toVariant().toLongLong(),
                                     !enc.isEmpty() && enc != QLatin1String("off")
                                         && enc != QLatin1String("-")});
                            }
                            fillOrigins();
                        });
                }
                fillOrigins();
            });
    }
}

void NewCopyWizard::preselect(const QString& machine, const QString& dataset) {
    m_wantMachine = machine;
    m_wantDataset = dataset;
    fillOrigins();
}

void NewCopyWizard::fillOrigins() {
    QString keep = chosenOrigin() ? chosenOrigin()->machine + chosenOrigin()->name : QString();
    if (keep.isEmpty() && !m_wantDataset.isEmpty()) {
        keep = m_wantMachine + m_wantDataset;
    }
    const QString f = m_filter->text().trimmed();
    m_origins->clear();
    for (int i = 0; i < m_datasets.size(); ++i) {
        const Dataset& d = m_datasets[i];
        if (!f.isEmpty() && !d.name.contains(f, Qt::CaseInsensitive)
            && !d.machineName.contains(f, Qt::CaseInsensitive)) {
            continue;
        }
        auto* it = new QListWidgetItem(
            L("%1   on %2 · %3", "%1   en %2 · %3").arg(d.name, d.machineName, bytesText(d.used)),
            m_origins);
        it->setData(Qt::UserRole, i);
        if (d.machine + d.name == keep) {
            m_origins->setCurrentItem(it);
        }
    }
    if (m_pending > 0) {
        auto* it = new QListWidgetItem(L("Asking the machines…", "Preguntando a las máquinas…"),
                                       m_origins);
        it->setFlags(Qt::NoItemFlags);
    }
}

void NewCopyWizard::fillDestinations() {
    const Dataset* o = chosenOrigin();
    const QString keep = chosenDest() ? chosenDest()->machine + chosenDest()->name : QString();
    m_dests->clear();
    for (int i = 0; i < m_pools.size(); ++i) {
        const Pool& p = m_pools[i];
        auto* it = new QListWidgetItem(L("%1 · pool %2 · %3 free", "%1 · pool %2 · quedan %3")
                                           .arg(p.machineName, p.name, p.free),
                                       m_dests);
        it->setData(Qt::UserRole, i);
        // Another machine first: a copy on the same machine does not survive losing it.
        if (o && p.machine == o->machine) {
            it->setText(it->text()
                        + L("  (same machine as the origin)", "  (la misma máquina del origen)"));
        }
        if (p.machine + p.name == keep) {
            m_dests->setCurrentItem(it);
        }
    }
}

const NewCopyWizard::Dataset* NewCopyWizard::chosenOrigin() const {
    const QListWidgetItem* it = m_origins ? m_origins->currentItem() : nullptr;
    if (!it || !(it->flags() & Qt::ItemIsSelectable)) {
        return nullptr;
    }
    const int i = it->data(Qt::UserRole).toInt();
    return i >= 0 && i < m_datasets.size() ? &m_datasets[i] : nullptr;
}

const NewCopyWizard::Pool* NewCopyWizard::chosenDest() const {
    const QListWidgetItem* it = m_dests ? m_dests->currentItem() : nullptr;
    if (!it) {
        return nullptr;
    }
    const int i = it->data(Qt::UserRole).toInt();
    return i >= 0 && i < m_pools.size() ? &m_pools[i] : nullptr;
}

CopyRequest NewCopyWizard::request() const {
    CopyRequest r;
    const Dataset* o = chosenOrigin();
    const Pool* p = chosenDest();
    if (!o || !p) {
        return r;
    }
    r.originMachine = o->machine;
    r.originDataset = o->name;
    r.destMachine = p->machine;
    r.destDataset = m_name->text().trimmed();
    r.mode = m_once->isChecked()   ? CopyMode::Once
             : m_pull->isChecked() ? CopyMode::Pull
                                   : CopyMode::Push;
    r.keep = {m_daily->value(), m_weekly->value(), m_monthly->value(), m_yearly->value()};
    r.stamp = m_stamp;
    QString originUid;
    QString destUid;
    for (const MachineView& m : m_store->machines()) {
        if (m.id == o->machine) {
            originUid = m.uid;
        }
        if (m.id == p->machine) {
            destUid = m.uid;
        }
    }
    r.destRoleForOrigin =
        originUid.isEmpty() ? QString() : m_reaches.value(p->machine).value(originUid);
    r.originRoleForDest =
        destUid.isEmpty() ? QString() : m_reaches.value(o->machine).value(destUid);
    for (const ScheduleRow& s : scheduleRows(m_store->machines())) {
        if (s.machineId == o->machine && s.dataset == o->name) {
            r.originHasSchedule = true;
        }
    }
    return r;
}

void NewCopyWizard::goTo(int step) {
    step = qBound(0, step, int(kRunning));
    if (step == kWhere) {
        fillDestinations();
    }
    if (step == kReview) {
        m_stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"));
        review();
    }
    m_steps->setCurrentIndex(step);
    static const char* const titlesEn[] = {"What do you want to copy?", "Where to?", "How?",
                                           "Review and confirm", "Setting up the copy"};
    static const char* const titlesEs[] = {"¿Qué quieres copiar?", "¿Adónde?", "¿Cómo?",
                                           "Revisa y confirma", "Preparando la copia"};
    m_stepTitle->setText(L(titlesEn[step], titlesEs[step]));
    m_back->setEnabled(step > kWhat && step < kRunning);
    m_next->setText(step == kReview ? L("Create the copy", "Crear la copia")
                                    : L("Next", "Siguiente"));
    m_next->setEnabled(step == kWhat     ? chosenOrigin() != nullptr
                       : step == kWhere  ? chosenDest() != nullptr
                       : step == kReview ? copyProblem(request()).isEmpty()
                                         : true);
}

void NewCopyWizard::review() {
    const CopyRequest r = request();
    const QString problem = copyProblem(r);
    if (!problem.isEmpty()) {
        m_summary->setText(problem);
        m_commands->clear();
        return;
    }
    const Dataset* o = chosenOrigin();
    const Pool* p = chosenDest();
    QString text = L("Copy %1 from %2 to %3 as %4.", "Copiar %1 de %2 a %3 como %4.")
                       .arg(r.originDataset, o->machineName, p->machineName, r.destDataset);
    text += QLatin1Char(' ');
    switch (r.mode) {
        case CopyMode::Once:
            text += L("Once, now: it will not update itself.",
                      "Una vez, ahora: no se actualizará sola.");
            break;
        case CopyMode::Pull:
            text += L("%1 will pull what is new and keep %2.", "%1 traerá lo nuevo y guardará %2.")
                        .arg(p->machineName, keepText(QJsonObject{{"daily", r.keep.daily},
                                                                  {"weekly", r.keep.weekly},
                                                                  {"monthly", r.keep.monthly},
                                                                  {"yearly", r.keep.yearly}}));
            break;
        case CopyMode::Push:
            text += L("%1 will send it after each snapshot.", "%1 la enviará tras cada snapshot.")
                        .arg(o->machineName);
            break;
    }
    text +=
        QStringLiteral("\n\n")
        + L("It will take about %1; %2 has %3 free.", "Ocupará unos %1; en %2 quedan %3 libres.")
              .arg(bytesText(o->used), p->machineName, p->free);
    if (o->encrypted) {
        text += QStringLiteral("\n")
                + L("The origin is encrypted: the copy will be too, and %1 will not need the key.",
                    "El origen está cifrado: la copia también lo estará y %1 no necesitará la "
                    "clave.")
                      .arg(p->machineName);
    }
    m_summary->setText(text);
    m_commands->setPlainText(copyCommands(r).join(QLatin1Char('\n')));
}

void NewCopyWizard::run() {
    m_toRun = copyCommands(request());
    if (m_toRun.isEmpty()) {
        return;
    }
    m_progress->clear();
    for (const QString& c : m_toRun) {
        new QListWidgetItem(QStringLiteral("…  ") + c, m_progress);
    }
    goTo(kRunning);
    m_next->setEnabled(false);
    m_cancel->setEnabled(false);
    m_running = -1;
    runNext();
}

void NewCopyWizard::runNext() {
    ++m_running;
    if (m_running >= m_toRun.size()) {
        new QListWidgetItem(L("The copy is set up. Follow it in Copies and Jobs.",
                              "La copia está preparada. Síguela en Copias y Trabajos."),
                            m_progress);
        m_next->setText(L("Close", "Cerrar"));
        m_next->setEnabled(true);
        Q_EMIT created();
        return;
    }
    const int i = m_running;
    m_store->general()->request(m_toRun[i], this, [this, i](const CliResult& r) {
        QListWidgetItem* it = m_progress->item(i);
        if (r.ok()) {
            it->setText(QStringLiteral("✓  ") + m_toRun[i]);
            runNext();
            return;
        }
        it->setText(QStringLiteral("✕  ") + m_toRun[i]);
        it->setForeground(QColor(colourOf(QStringLiteral("error"))));
        new QListWidgetItem(r.err.trimmed().isEmpty() ? L("It failed.", "Falló.") : r.err.trimmed(),
                            m_progress);
        new QListWidgetItem(L("Nothing after it was run. What did run stays done.",
                              "No se ha ejecutado nada de lo que venía después. Lo ya hecho se "
                              "queda hecho."),
                            m_progress);
        m_next->setText(L("Close", "Cerrar"));
        m_next->setEnabled(true);
        m_cancel->setEnabled(false);
        Q_EMIT created();
    });
}

}  // namespace zfsmgr::gui
