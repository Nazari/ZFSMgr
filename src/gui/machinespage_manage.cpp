// Machines, advanced mode: what a dataset can have done to it beyond the summary —its name,
// mounting, encryption keys, delegated permissions, a clone made independent— and sending it
// or its files to another dataset. Everything is a CLI command line; the CLI asks what has to
// be confirmed (as `confirm` events) and the passphrases (as `secret` events).

#include "machinespage.h"

#include "addmachinedialog.h"
#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <algorithm>

namespace zfsmgr::gui {

namespace {

QString url(const QString& machine, const QString& name = {}) {
    return QStringLiteral("zfsm://") + machine
           + (name.isEmpty() ? QString() : QStringLiteral("/") + name);
}

QPushButton* button(const QString& text, QWidget* parent) {
    return new QPushButton(text, parent);
}

// A one-line question, opened with open(). `done` gets the text when accepted and not empty.
void askLine(QWidget* parent, const QString& title, const QString& text, const QString& initial,
             std::function<void(const QString&)> done) {
    auto* d = new QInputDialog(parent);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(title);
    d->setLabelText(text);
    d->setTextValue(initial);
    QObject::connect(d, &QInputDialog::textValueSelected, parent, [done](const QString& v) {
        if (!v.trimmed().isEmpty()) {
            done(v.trimmed());
        }
    });
    d->open();
}

// The machines that answer, for a «where to» choice; the given one first.
void fillMachines(QComboBox* box, const QVector<MachineView>& machines, const QString& first) {
    for (const MachineView& m : machines) {
        if (m.connected) {
            box->addItem(m.name, m.id);
        }
    }
    box->setCurrentIndex(qMax(0, box->findData(first)));
}

}  // namespace

QString MachinesPage::propOf(const QString& machine, const QString& dataset,
                             const QString& name) const {
    for (const QJsonValue& v : m_props.value(machine + QLatin1Char('/') + dataset)) {
        const QJsonObject p = v.toObject();
        if (p.value(QStringLiteral("property")).toString() == name) {
            return p.value(QStringLiteral("value")).toString();
        }
    }
    return {};
}

bool MachinesPage::ensureProps(const QString& machine, const QString& dataset) {
    const QString key = machine + QLatin1Char('/') + dataset;
    if (m_props.contains(key)) {
        return true;
    }
    if (!m_propsAsked.contains(key)) {
        m_propsAsked.insert(key);
        m_store->forMachine(machine)->request(
            QStringLiteral("ls ") + url(machine, dataset) + QStringLiteral("#properties"), this,
            [this, key](const CliResult& r) {
                m_propsAsked.remove(key);
                const qsizetype start = r.out.indexOf(QLatin1Char('{'));
                const QJsonObject o =
                    start < 0 ? QJsonObject()
                              : QJsonDocument::fromJson(r.out.mid(start).toUtf8()).object();
                m_props.insert(key, o.value(QStringLiteral("properties")).toArray());
                showDetail();
            });
    }
    return false;
}

void MachinesPage::showOutput(const QString& machine, const QString& line, const QString& title) {
    m_store->forMachine(machine)->request(
        line, this,
        [this, title](const CliResult& r) {
            showText(this, title, r.ok() || r.err.trimmed().isEmpty() ? r.out : r.err);
        },
        QStringLiteral("text"));
}

void MachinesPage::newDataset(const QString& machine, const QString& parent) {
    auto* d = new QDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(L("New dataset", "Dataset nuevo"));
    auto* lay = new QVBoxLayout(d);
    lay->addWidget(muted(L("Created inside %1.", "Se crea dentro de %1.").arg(parent), d));
    auto* form = new QFormLayout();
    auto* name = new QLineEdit(d);
    name->setPlaceholderText(L("e.g. photos", "p. ej. fotos"));
    form->addRow(L("Name", "Nombre"), name);
    auto* props = new QLineEdit(d);
    props->setPlaceholderText(QStringLiteral("compression=zstd atime=off"));
    form->addRow(L("Properties (optional)", "Propiedades (opcional)"), props);
    auto* volume = new QCheckBox(L("A volume (a block device), not a filesystem",
                                   "Un volumen (un dispositivo de bloques), no un sistema de "
                                   "ficheros"),
                                 d);
    form->addRow(QString(), volume);
    auto* size = new QLineEdit(d);
    size->setPlaceholderText(L("size, e.g. 20G", "tamaño, p. ej. 20G"));
    size->setEnabled(false);
    form->addRow(L("Size", "Tamaño"), size);
    connect(volume, &QCheckBox::toggled, size, &QLineEdit::setEnabled);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, d,
            [this, d, name, props, volume, size, machine, parent] {
                const QString n = name->text().trimmed();
                if (n.isEmpty() || (volume->isChecked() && size->text().trimmed().isEmpty())) {
                    return;
                }
                QString line = QStringLiteral("create ") + quoted(n);
                for (const QString& p : props->text().split(
                         QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
                    line += QLatin1Char(' ') + quoted(p);
                }
                if (volume->isChecked()) {
                    line += QStringLiteral(" -V ") + quoted(size->text().trimmed());
                }
                act(line + QStringLiteral(" --on ") + url(machine, parent),
                    L("Dataset %1 created.", "Creado el dataset %1.")
                        .arg(parent + QLatin1Char('/') + n));
                d->accept();
            });
    d->open();
}

QWidget* MachinesPage::manageTab(const QString& machine, const QString& dataset) {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    const QString here = url(machine, dataset);
    const QString on = QStringLiteral(" --on ") + here;
    if (!ensureProps(machine, dataset)) {
        l->addWidget(muted(L("Reading its properties…", "Leyendo sus propiedades…"), w));
        l->addStretch(1);
        return w;
    }

    // ── Name and place.
    l->addWidget(heading(L("Name and place", "Nombre y sitio"), w));
    l->addWidget(muted(L("Renaming keeps its snapshots and copies. Writing another parent moves "
                         "it inside the same pool.",
                         "Renombrar conserva sus snapshots y copias. Escribir otro padre lo mueve "
                         "dentro del mismo pool."),
                       w));
    {
        auto* row = new QHBoxLayout();
        auto* rename = button(L("Rename or move…", "Renombrar o mover…"), w);
        connect(rename, &QPushButton::clicked, this, [this, machine, dataset, here] {
            askLine(this, L("Rename", "Renombrar"),
                    L("New full name (pool/…/name):", "Nombre completo nuevo (pool/…/nombre):"),
                    dataset, [this, machine, dataset, here](const QString& v) {
                        if (v == dataset) {
                            return;
                        }
                        // Same parent: the short form. Another parent: both names, the URL first.
                        const QString parent = dataset.section(QLatin1Char('/'), 0, -2);
                        const QString line =
                            v.section(QLatin1Char('/'), 0, -2) == parent
                                ? QStringLiteral("rename %1 --on %2")
                                      .arg(quoted(v.section(QLatin1Char('/'), -1)), here)
                                : QStringLiteral("rename %1 %2").arg(here, quoted(v));
                        act(line, L("Now it is %1.", "Ahora es %1.").arg(v));
                    });
        });
        row->addWidget(rename);
        auto* child = button(L("New dataset inside…", "Dataset nuevo dentro…"), w);
        connect(child, &QPushButton::clicked, this,
                [this, machine, dataset] { newDataset(machine, dataset); });
        row->addWidget(child);
        row->addStretch(1);
        l->addLayout(row);
    }

    // ── Mounting.
    QJsonObject entry;
    for (const QJsonValue& v :
         m_entries.value(machine + QLatin1Char('/') + dataset.section(QLatin1Char('/'), 0, 0))) {
        if (v.toObject().value(QStringLiteral("name")).toString() == dataset) {
            entry = v.toObject();
        }
    }
    if (entry.value(QStringLiteral("type")).toString() != QLatin1String("volume")) {
        l->addWidget(heading(L("Mounting", "Montaje"), w));
        const bool mounted =
            entry.value(QStringLiteral("mounted")).toString() == QLatin1String("yes");
        l->addWidget(label(mounted ? L("Mounted at %1.", "Montado en %1.")
                                         .arg(entry.value(QStringLiteral("mountpoint")).toString())
                                   : L("Not mounted: its files cannot be reached.",
                                       "Sin montar: no se llega a sus ficheros."),
                           w));
        auto* b = button(mounted ? L("Unmount", "Desmontar") : L("Mount", "Montar"), w);
        connect(b, &QPushButton::clicked, this, [this, mounted, on] {
            act((mounted ? QStringLiteral("unmount") : QStringLiteral("mount")) + on,
                mounted ? L("Unmounted.", "Desmontado.") : L("Mounted.", "Montado."));
        });
        l->addWidget(b, 0, Qt::AlignLeft);
    }

    // ── A clone: it depends on the snapshot it came from until promoted.
    const QString origin = propOf(machine, dataset, QStringLiteral("origin"));
    if (!origin.isEmpty() && origin != QLatin1String("-")) {
        l->addWidget(heading(L("Clone", "Clon"), w));
        l->addWidget(muted(L("It was cloned from %1, and that snapshot cannot be deleted while "
                             "this exists. Making it independent swaps the roles: the "
                             "snapshots up to that point move here.",
                             "Se clonó de %1, y ese snapshot no se puede borrar mientras esto "
                             "exista. Hacerlo independiente cambia los papeles: los snapshots "
                             "hasta ese punto pasan a este.")
                               .arg(origin),
                           w));
        auto* b = button(L("Make independent (promote)", "Hacer independiente (promote)"), w);
        connect(b, &QPushButton::clicked, this, [this, on] {
            act(QStringLiteral("promote") + on, L("Independent now.", "Ya es independiente."));
        });
        l->addWidget(b, 0, Qt::AlignLeft);
    }

    // ── Encryption keys.
    const QString enc = propOf(machine, dataset, QStringLiteral("encryption"));
    if (!enc.isEmpty() && enc != QLatin1String("off")) {
        const QString status = propOf(machine, dataset, QStringLiteral("keystatus"));
        const QString root = propOf(machine, dataset, QStringLiteral("encryptionroot"));
        l->addWidget(heading(L("Encryption", "Cifrado"), w));
        const bool loaded = status == QLatin1String("available");
        QString text = loaded ? L("The key is loaded: its data can be read.",
                                  "La clave está cargada: sus datos se pueden leer.")
                              : L("The key is not loaded: nothing inside can be read or mounted.",
                                  "La clave no está cargada: no se puede leer ni montar nada.");
        if (!root.isEmpty() && root != dataset) {
            text += QLatin1Char(' ') + L("It uses the key of %1.", "Usa la clave de %1.").arg(root);
        }
        l->addWidget(label(text, w));
        auto* row = new QHBoxLayout();
        if (loaded) {
            auto* unload = button(L("Lock (unload the key)", "Cerrar (descargar la clave)"), w);
            connect(unload, &QPushButton::clicked, this, [this, machine, dataset, on] {
                m_props.remove(machine + QLatin1Char('/') + dataset);
                act(QStringLiteral("unload-key") + on, L("Locked.", "Cerrado."));
            });
            row->addWidget(unload);
            auto* change = button(L("Change the passphrase…", "Cambiar la frase de paso…"), w);
            connect(change, &QPushButton::clicked, this, [this, on] {
                act(QStringLiteral("change-key") + on,
                    L("Passphrase changed.", "Frase de paso cambiada."), false);
            });
            row->addWidget(change);
        } else {
            auto* load = button(L("Unlock (load the key)…", "Abrir (cargar la clave)…"), w);
            connect(load, &QPushButton::clicked, this, [this, machine, dataset, on] {
                m_props.remove(machine + QLatin1Char('/') + dataset);
                act(QStringLiteral("load-key") + on, L("Unlocked.", "Abierto."));
            });
            row->addWidget(load);
        }
        row->addStretch(1);
        l->addLayout(row);
    }

    // ── Delegated permissions (zfs allow).
    l->addWidget(heading(L("Delegated permissions", "Permisos delegados"), w));
    l->addWidget(muted(L("Lets a user or group who is not root do some ZFS things here: take "
                         "snapshots, send, mount… Permissions are ZFS's names, separated by "
                         "commas: snapshot,send,hold,mount.",
                         "Deja a un usuario o grupo que no es root hacer algunas cosas de ZFS "
                         "aquí: hacer snapshots, enviar, montar… Los permisos son los nombres de "
                         "ZFS, separados por comas: snapshot,send,hold,mount."),
                       w));
    {
        auto* see = button(L("See the permissions", "Ver los permisos"), w);
        connect(see, &QPushButton::clicked, this, [this, machine, dataset, on] {
            showOutput(machine, QStringLiteral("allow") + on,
                       L("Permissions on %1", "Permisos en %1").arg(dataset));
        });
        l->addWidget(see, 0, Qt::AlignLeft);
        auto* form = new QFormLayout();
        auto* kind = new QComboBox(w);
        kind->addItem(L("User", "Usuario"), QStringLiteral("user"));
        kind->addItem(L("Group", "Grupo"), QStringLiteral("group"));
        kind->addItem(L("Everyone", "Todos"), QStringLiteral("everyone"));
        kind->addItem(L("Named set (@name)", "Conjunto con nombre (@nombre)"),
                      QStringLiteral("set"));
        form->addRow(L("To", "A"), kind);
        auto* who = new QLineEdit(w);
        form->addRow(L("Name", "Nombre"), who);
        connect(kind, &QComboBox::currentIndexChanged, who,
                [kind, who] { who->setEnabled(kind->currentData() != QLatin1String("everyone")); });
        auto* perms = new QLineEdit(w);
        perms->setPlaceholderText(QStringLiteral("snapshot,send,hold"));
        form->addRow(L("Permissions", "Permisos"), perms);
        auto* scope = new QComboBox(w);
        scope->addItem(L("Here and below", "Aquí y debajo"), QString());
        scope->addItem(L("Only here", "Solo aquí"), QStringLiteral(" --local"));
        scope->addItem(L("Only below", "Solo debajo"), QStringLiteral(" --descend"));
        form->addRow(L("Where", "Dónde"), scope);
        l->addLayout(form);
        const auto line = [kind, who, perms, scope, on](bool grant) {
            const QString k = kind->currentData().toString();
            QString s = grant ? QStringLiteral("allow") : QStringLiteral("unallow");
            if (k == QLatin1String("everyone")) {
                s += QStringLiteral(" --everyone");
            } else {
                QString name = who->text().trimmed();
                if (name.isEmpty()) {
                    return QString();
                }
                if (k == QLatin1String("set") && !name.startsWith(QLatin1Char('@'))) {
                    name.prepend(QLatin1Char('@'));
                }
                s += QStringLiteral(" --%1 %2").arg(k, quoted(name));
            }
            const QStringList p = perms->text().split(QRegularExpression(QStringLiteral("[,\\s]+")),
                                                      Qt::SkipEmptyParts);
            if (grant && p.isEmpty()) {
                return QString();
            }
            for (const QString& x : p) {
                s += QLatin1Char(' ') + quoted(x);
            }
            return s + scope->currentData().toString() + on;
        };
        auto* row = new QHBoxLayout();
        auto* grant = button(L("Grant", "Conceder"), w);
        connect(grant, &QPushButton::clicked, this, [this, line] {
            const QString s = line(true);
            if (s.isEmpty()) {
                Q_EMIT message(
                    L("Say to whom and which permissions.", "Di a quién y qué permisos."));
                return;
            }
            act(s, L("Granted.", "Concedido."), false);
        });
        row->addWidget(grant);
        auto* revoke = button(L("Revoke…", "Quitar…"), w);
        connect(revoke, &QPushButton::clicked, this, [this, line] {
            const QString s = line(false);
            if (s.isEmpty()) {
                Q_EMIT message(L("Say to whom.", "Di a quién."));
                return;
            }
            act(s, L("Revoked.", "Quitado."), false);
        });
        row->addWidget(revoke);
        row->addWidget(muted(L("Revoking with no permissions written takes them all.",
                               "Quitar sin escribir permisos los quita todos."),
                             w),
                       1);
        l->addLayout(row);
    }
    l->addStretch(1);
    return w;
}

QWidget* MachinesPage::sendTab(const QString& machine, const QString& dataset) {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    QStringList snaps;
    for (const QJsonValue& v :
         m_entries.value(machine + QLatin1Char('/') + dataset.section(QLatin1Char('/'), 0, 0))) {
        const QString name = v.toObject().value(QStringLiteral("name")).toString();
        if (name.startsWith(dataset + QLatin1Char('@'))) {
            snaps.prepend(name.section(QLatin1Char('@'), 1));  // the newest first
        }
    }

    // ── A snapshot to another dataset (zfs send | zfs receive, as a job).
    l->addWidget(heading(L("Send a snapshot", "Enviar un snapshot"), w));
    l->addWidget(muted(L("Once, by hand. For a copy that keeps itself up to date there is "
                         "«Copy…» in the summary.",
                         "Una vez, a mano. Para una copia que se mantenga al día está "
                         "«Copiar…» en el resumen."),
                       w));
    {
        auto* form = new QFormLayout();
        auto* snap = new QComboBox(w);
        snap->addItems(snaps);
        form->addRow(L("Snapshot", "Snapshot"), snap);
        auto* to = new QComboBox(w);
        fillMachines(to, m_store->machines(), machine);
        form->addRow(L("To machine", "A la máquina"), to);
        auto* dest = new QLineEdit(w);
        dest->setPlaceholderText(
            L("pool/dataset, e.g. backup/photos", "pool/dataset, p. ej. copias/fotos"));
        form->addRow(L("Dataset", "Dataset"), dest);
        auto* recursive = new QCheckBox(L("With its children (-R)", "Con sus hijos (-R)"), w);
        form->addRow(QString(), recursive);
        auto* raw = new QCheckBox(L("Raw (-w)", "En crudo (-w)"), w);
        raw->setToolTip(L("Encrypted data travels encrypted, without its key.",
                          "Lo cifrado viaja cifrado, sin su clave."));
        form->addRow(QString(), raw);
        auto* level =
            new QCheckBox(L("Only what changed (--level)", "Solo lo cambiado (--level)"), w);
        level->setToolTip(L("Only what changed since the newest snapshot both have.",
                            "Solo lo cambiado desde el snapshot más nuevo que tengan los dos."));
        form->addRow(QString(), level);
        auto* partial = new QComboBox(w);
        partial->addItem(L("Stop and tell me", "Parar y avisarme"), QString());
        partial->addItem(L("Continue it", "Continuarla"), QStringLiteral(" --resume"));
        partial->addItem(L("Discard it and start again", "Descartarla y empezar de nuevo"),
                         QStringLiteral(" --discard-partial"));
        form->addRow(L("If cut off before", "Si quedó a medias"), partial);
        l->addLayout(form);
        l->addWidget(muted(L("«If cut off before»: what to do when the destination has a receive "
                             "that was interrupted.",
                             "«Si quedó a medias»: qué hacer si el destino tiene una recepción "
                             "que se interrumpió."),
                           w));
        auto* go = button(L("Send", "Enviar"), w);
        go->setEnabled(!snaps.isEmpty());
        connect(go, &QPushButton::clicked, this, [=] {
            const QString d = dest->text().trimmed();
            if (d.isEmpty() || snap->currentText().isEmpty()) {
                Q_EMIT message(L("Say where it goes.", "Di adónde va."));
                return;
            }
            QString line = QStringLiteral("send %1 --from %2@%3")
                               .arg(url(to->currentData().toString(), d), url(machine, dataset),
                                    snap->currentText());
            if (recursive->isChecked()) {
                line += QStringLiteral(" -R");
            }
            if (raw->isChecked()) {
                line += QStringLiteral(" -w");
            }
            if (level->isChecked()) {
                line += QStringLiteral(" --level");
            }
            line += partial->currentData().toString();
            act(line, L("Sending as a job: follow it in Jobs.",
                        "Enviando como trabajo: síguelo en Trabajos."));
        });
        l->addWidget(go, 0, Qt::AlignLeft);
    }

    // ── Its files to another mounted tree (the agent's own rsync).
    l->addWidget(heading(L("Sync its files", "Sincronizar sus ficheros"), w));
    l->addWidget(muted(L("Copies FILES, not snapshots, to another mounted dataset of this or "
                         "another machine. Without deleting, it only adds and updates.",
                         "Copia FICHEROS, no snapshots, a otro dataset montado de esta máquina "
                         "o de otra. Sin borrar, solo añade y actualiza."),
                       w));
    {
        auto* form = new QFormLayout();
        auto* to = new QComboBox(w);
        fillMachines(to, m_store->machines(), machine);
        form->addRow(L("To machine", "A la máquina"), to);
        auto* dest = new QLineEdit(w);
        dest->setPlaceholderText(L("pool/dataset", "pool/dataset"));
        form->addRow(L("Dataset", "Dataset"), dest);
        auto* del = new QCheckBox(L("Delete what is extra there", "Borrar lo que sobre allí"), w);
        del->setToolTip(
            L("--delete: the destination ends up identical, and that includes deleting.",
              "--delete: el destino queda idéntico, y eso incluye borrar."));
        form->addRow(QString(), del);
        l->addLayout(form);
        const auto line = [=]() {
            const QString d = dest->text().trimmed();
            if (d.isEmpty()) {
                return QString();
            }
            return QStringLiteral("rsync ") + url(to->currentData().toString(), d)
                   + (del->isChecked() ? QStringLiteral(" --delete") : QString())
                   + QStringLiteral(" --on ") + url(machine, dataset);
        };
        auto* row = new QHBoxLayout();
        auto* check = button(L("See what it would do", "Ver qué haría"), w);
        connect(check, &QPushButton::clicked, this, [this, line, machine] {
            const QString s = line();
            if (!s.isEmpty()) {
                showOutput(machine, s + QStringLiteral(" --check --wait"),
                           L("What syncing would do", "Qué haría sincronizar"));
            }
        });
        row->addWidget(check);
        auto* go = button(L("Sync", "Sincronizar"), w);
        connect(go, &QPushButton::clicked, this, [this, line] {
            const QString s = line();
            if (s.isEmpty()) {
                Q_EMIT message(L("Say where it goes.", "Di adónde va."));
                return;
            }
            act(s,
                L("Syncing as a job: follow it in Jobs.",
                  "Sincronizando como trabajo: síguelo en Trabajos."),
                false);
        });
        row->addWidget(go);
        row->addStretch(1);
        l->addLayout(row);
    }
    l->addStretch(1);
    return w;
}

void MachinesPage::snapshotExtras(QHBoxLayout* row, QWidget* w, const QString& machine,
                                  const QString& dataset, const std::function<QString()>& chosen) {
    // What only makes sense with one chosen: shown always, it answers «choose one» otherwise.
    const auto withChosen = [this, chosen](const std::function<void(const QString&)>& f) {
        return [this, chosen, f] {
            const QString s = chosen();
            if (s.isEmpty()) {
                Q_EMIT message(L("Choose a snapshot first.", "Elige antes un snapshot."));
                return;
            }
            f(s);
        };
    };
    auto* changes = button(L("Changes since…", "Cambios desde…"), w);
    connect(changes, &QPushButton::clicked, this,
            withChosen([this, machine, dataset](const QString& s) {
                showOutput(
                    machine,
                    QStringLiteral("diff %1 --from %2").arg(url(machine, dataset), url(machine, s)),
                    L("Changes since %1", "Cambios desde %1").arg(s));
            }));
    row->addWidget(changes);
    auto* clone = button(L("Clone…", "Clonar…"), w);
    connect(
        clone, &QPushButton::clicked, this, withChosen([this, machine, dataset](const QString& s) {
            askLine(this, L("Clone", "Clonar"),
                    L("Name of the new dataset (full: pool/…/name):",
                      "Nombre del dataset nuevo (completo: pool/…/nombre):"),
                    dataset + QStringLiteral("-") + s.section(QLatin1Char('@'), 1),
                    [this, machine, s](const QString& v) {
                        act(QStringLiteral("clone %1 --from %2").arg(quoted(v), url(machine, s)),
                            L("Cloned into %1.", "Clonado en %1.").arg(v));
                    });
        }));
    row->addWidget(clone);
    auto* holds = button(L("Holds…", "Retenciones…"), w);
    connect(holds, &QPushButton::clicked, this, withChosen([this, machine](const QString& s) {
                showOutput(machine, QStringLiteral("holds ") + url(machine, s),
                           L("Holds of %1", "Retenciones de %1").arg(s));
            }));
    row->addWidget(holds);
    auto* hold = button(L("Hold…", "Retener…"), w);
    connect(hold, &QPushButton::clicked, this, withChosen([this, machine](const QString& s) {
                askLine(this, L("Hold", "Retener"),
                        L("A label: while it is there, the snapshot cannot be deleted.",
                          "Una etiqueta: mientras esté, el snapshot no se puede borrar."),
                        QStringLiteral("keep"), [this, machine, s](const QString& tag) {
                            act(QStringLiteral("hold %1 --on %2").arg(quoted(tag), url(machine, s)),
                                L("Held.", "Retenido."), false);
                        });
            }));
    row->addWidget(hold);
    auto* release = button(L("Release…", "Soltar…"), w);
    connect(
        release, &QPushButton::clicked, this, withChosen([this, machine](const QString& s) {
            askLine(
                this, L("Release", "Soltar"),
                L("The label of the hold to remove:", "La etiqueta de la retención que se quita:"),
                QString(), [this, machine, s](const QString& tag) {
                    act(QStringLiteral("release %1 --on %2").arg(quoted(tag), url(machine, s)),
                        L("Released.", "Soltado."), false);
                });
        }));
    row->addWidget(release);
}

}  // namespace zfsmgr::gui
