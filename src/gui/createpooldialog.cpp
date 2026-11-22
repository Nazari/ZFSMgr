#include "createpooldialog.h"

#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace zfsmgr::gui {

namespace {

QJsonObject firstObject(const QString& out) {
    const qsizetype start = out.indexOf(QLatin1Char('{'));
    return start < 0 ? QJsonObject() : QJsonDocument::fromJson(out.mid(start).toUtf8()).object();
}

}  // namespace

CreatePoolDialog::CreatePoolDialog(StateStore* store, QString machine, QWidget* parent)
    : QDialog(parent), m_store(store), m_machine(std::move(machine)) {
    setWindowTitle(L("Create a pool on %1", "Crear un pool en %1").arg(m_machine));
    setMinimumSize(560, 480);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    m_name = new QLineEdit(this);
    form->addRow(L("Name", "Nombre"), m_name);
    m_layout = new QComboBox(this);
    m_layout->addItem(L("Separate disks: all the space, no redundancy",
                        "Discos sueltos: todo el espacio, sin redundancia"),
                      QString());
    m_layout->addItem(L("Mirror: every disk holds everything; survives all but one failing",
                        "Espejo: cada disco lo guarda todo; aguanta que fallen todos menos uno"),
                      QStringLiteral("mirror"));
    m_layout->addItem(L("raidz1: survives one disk failing (3 disks or more)",
                        "raidz1: aguanta que falle un disco (3 discos o más)"),
                      QStringLiteral("raidz1"));
    m_layout->addItem(
        L("raidz2: survives two failing (4 or more)", "raidz2: aguanta que fallen dos (4 o más)"),
        QStringLiteral("raidz2"));
    m_layout->addItem(L("raidz3: survives three failing (5 or more)",
                        "raidz3: aguanta que fallen tres (5 o más)"),
                      QStringLiteral("raidz3"));
    form->addRow(L("How they are arranged", "Cómo se reparten"), m_layout);
    m_optsAs = new QComboBox(this);
    m_optsAs->addItem(L("none", "ninguno"), QString());
    for (const MachineView& m : m_store->machines()) {
        for (const QJsonValue& v : m.overview.value(QStringLiteral("pools")).toArray()) {
            const QString p = v.toObject().value(QStringLiteral("name")).toString();
            m_optsAs->addItem(QStringLiteral("%1 (%2)").arg(p, m.name),
                              QStringLiteral("zfsm://%1/%2").arg(m.id, p));
        }
    }
    form->addRow(L("Options like", "Opciones como las de"), m_optsAs);
    lay->addLayout(form);

    lay->addWidget(heading(L("Free disks", "Discos libres"), this));
    lay->addWidget(muted(
        L("What is on the disks chosen is LOST.", "Lo que haya en los discos elegidos SE PIERDE."),
        this));
    m_disks = new QListWidget(this);
    lay->addWidget(m_disks, 1);
    m_command = label(QString(), this);
    m_command->setStyleSheet(QStringLiteral("font-family:monospace"));
    lay->addWidget(m_command);
    m_result = label(QString(), this);
    lay->addWidget(m_result);
    lay->addWidget(muted(L("The rules —how many disks each arrangement needs— are zpool's: the "
                           "window only shows its answer.",
                           "Las reglas —cuántos discos hacen falta— las decide zpool: la ventana "
                           "solo enseña su respuesta."),
                         this));

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(L("Close", "Cerrar"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    auto* go = new QPushButton(L("Create the pool…", "Crear el pool…"), this);
    connect(go, &QPushButton::clicked, this, [this, go] {
        const QString cmd = command();
        if (cmd.isEmpty()) {
            return;
        }
        go->setEnabled(false);
        m_result->setText(L("Creating…", "Creando…"));
        // The CLI lists what is lost and asks for the pool's name before writing.
        m_store->general()->request(cmd, this, [this, go](const CliResult& r) {
            go->setEnabled(true);
            if (!r.ok()) {
                m_result->setText(r.err.trimmed());
                return;
            }
            Q_EMIT created();
            accept();
        });
    });
    buttons->addWidget(go);
    lay->addLayout(buttons);

    connect(m_name, &QLineEdit::textChanged, this, &CreatePoolDialog::update);
    connect(m_layout, &QComboBox::currentIndexChanged, this, &CreatePoolDialog::update);
    connect(m_optsAs, &QComboBox::currentIndexChanged, this, &CreatePoolDialog::update);
    connect(m_disks, &QListWidget::itemChanged, this, &CreatePoolDialog::update);

    m_disks->addItem(L("Asking for the disks…", "Preguntando por los discos…"));
    m_store->forMachine(m_machine)->request(
        QStringLiteral("devices zfsm://") + m_machine, this, [this](const CliResult& r) {
            m_disks->clear();
            const QJsonArray all = firstObject(r.out).value(QStringLiteral("devices")).toArray();
            // Free: claimed by no pool, not in use, no filesystem, no mountpoint, and —for a
            // whole disk— no partition of it in use either.
            QStringList busyParents;
            for (const QJsonValue& v : all) {
                const QJsonObject d = v.toObject();
                if (d.value(QStringLiteral("inuse")).toBool()
                    || !d.value(QStringLiteral("mountpoint")).toString().isEmpty()
                    || !d.value(QStringLiteral("pool")).toString().isEmpty()
                    || !d.value(QStringLiteral("fstype")).toString().isEmpty()) {
                    busyParents << d.value(QStringLiteral("parent")).toString();
                }
            }
            for (const QJsonValue& v : all) {
                const QJsonObject d = v.toObject();
                const QString path = d.value(QStringLiteral("path")).toString();
                if (d.value(QStringLiteral("inuse")).toBool()
                    || !d.value(QStringLiteral("mountpoint")).toString().isEmpty()
                    || !d.value(QStringLiteral("pool")).toString().isEmpty()
                    || !d.value(QStringLiteral("fstype")).toString().isEmpty()
                    || busyParents.contains(path)) {
                    continue;
                }
                // The stable name (by-id) when there is one: /dev/sdb changes between boots.
                const QJsonArray aliases = d.value(QStringLiteral("aliases")).toArray();
                const QString stable = aliases.isEmpty() ? path : aliases.first().toString();
                auto* it = new QListWidgetItem(
                    QStringLiteral("%1   %2").arg(
                        stable,
                        bytesText(d.value(QStringLiteral("size")).toVariant().toLongLong())),
                    m_disks);
                it->setData(Qt::UserRole, stable);
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(Qt::Unchecked);
            }
            if (m_disks->count() == 0) {
                auto* it = new QListWidgetItem(L("No free disk on this machine.",
                                                 "No hay ningún disco libre en esta máquina."),
                                               m_disks);
                it->setFlags(Qt::NoItemFlags);
            }
            update();
        });
    update();
}

QString CreatePoolDialog::command() const {
    static const QRegularExpression nameRx(QStringLiteral("^[A-Za-z][A-Za-z0-9_.:-]*$"));
    const QString name = m_name->text().trimmed();
    QStringList disks;
    for (int i = 0; i < m_disks->count(); ++i) {
        const QListWidgetItem* it = m_disks->item(i);
        if ((it->flags() & Qt::ItemIsUserCheckable) && it->checkState() == Qt::Checked) {
            disks << it->data(Qt::UserRole).toString();
        }
    }
    if (!nameRx.match(name).hasMatch() || disks.isEmpty()) {
        return {};
    }
    QString cmd = QStringLiteral("create ") + name;
    const QString layout = m_layout->currentData().toString();
    if (!layout.isEmpty()) {
        cmd += QLatin1Char(' ') + layout;
    }
    cmd += QLatin1Char(' ') + disks.join(QLatin1Char(' '));
    const QString like = m_optsAs->currentData().toString();
    if (!like.isEmpty()) {
        cmd += QStringLiteral(" --opts-as ") + like;
    }
    return cmd + QStringLiteral(" --on zfsm://") + m_machine;
}

void CreatePoolDialog::update() {
    const QString cmd = command();
    m_command->setText(cmd.isEmpty() ? L("Give it a name and choose its disks.",
                                         "Dale un nombre y elige sus discos.")
                                     : cmd);
}

}  // namespace zfsmgr::gui
