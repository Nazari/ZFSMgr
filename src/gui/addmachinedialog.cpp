#include "addmachinedialog.h"

#include "cliservice.h"
#include "statestore.h"
#include "text.h"
#include "uikit.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

namespace zfsmgr::gui {

QString quoted(const QString& value) {
    if (!value.contains(QLatin1Char(' ')) && !value.contains(QLatin1Char('"'))
        && !value.contains(QLatin1Char('\''))) {
        return value;
    }
    QString v = value;
    v.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    v.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + v + QLatin1Char('"');
}

AddMachineDialog::AddMachineDialog(StateStore* store, QWidget* parent)
    : QDialog(parent), m_store(store) {
    setWindowTitle(L("Add a machine", "Añadir una máquina"));
    setMinimumWidth(520);
    auto* lay = new QVBoxLayout(this);
    lay->addWidget(muted(L("How to reach it by SSH. Its system is asked to the machine itself; the "
                           "password, if it needs one, is asked next and stored encrypted.",
                           "Cómo se llega a ella por SSH. El sistema se le pregunta a la propia "
                           "máquina; la contraseña, si hace falta, se pide después y se guarda "
                           "cifrada."),
                         this));
    auto* form = new QFormLayout();
    m_id = new QLineEdit(this);
    m_id->setPlaceholderText(L("e.g. backup", "p. ej. copias"));
    form->addRow(L("Name", "Nombre"), m_id);
    m_host = new QLineEdit(this);
    form->addRow(L("Host", "Máquina (host)"), m_host);
    m_user = new QLineEdit(this);
    form->addRow(L("User", "Usuario"), m_user);
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(22);
    form->addRow(L("Port", "Puerto"), m_port);
    auto* keyRow = new QHBoxLayout();
    m_key = new QLineEdit(this);
    m_key->setPlaceholderText(L("empty: with a password", "vacío: con contraseña"));
    auto* browse = new QPushButton(L("Choose…", "Elegir…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        auto* d = new QFileDialog(this, L("SSH key", "Clave SSH"),
                                  QDir::homePath() + QStringLiteral("/.ssh"));
        d->setAttribute(Qt::WA_DeleteOnClose);
        connect(d, &QFileDialog::fileSelected, m_key, &QLineEdit::setText);
        d->open();
    });
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(browse);
    form->addRow(L("SSH key", "Clave SSH"), keyRow);
    m_sudo = new QCheckBox(L("Needs sudo to manage ZFS", "Necesita sudo para manejar ZFS"), this);
    form->addRow(QString(), m_sudo);
    lay->addLayout(form);

    m_result = label(QString(), this);
    m_result->setTextFormat(Qt::PlainText);
    lay->addWidget(m_result);

    auto* buttons = new QHBoxLayout();
    m_install = new QPushButton(L("Install the daemon…", "Instalar el daemon…"), this);
    m_install->setVisible(false);
    connect(m_install, &QPushButton::clicked, this, [this] {
        m_install->setEnabled(false);
        m_result->setText(L("Installing the daemon…", "Instalando el daemon…"));
        m_store->general()->request(QStringLiteral("install-daemon --on zfsm://") + m_created, this,
                                    [this](const CliResult& r) {
                                        m_install->setEnabled(true);
                                        if (!r.ok()) {
                                            m_result->setText(r.err.trimmed());
                                            return;
                                        }
                                        probe();
                                    });
    });
    buttons->addWidget(m_install);
    buttons->addStretch(1);
    auto* close = new QPushButton(L("Close", "Cerrar"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(close);
    m_go = new QPushButton(L("Add", "Añadir"), this);
    m_go->setDefault(true);
    connect(m_go, &QPushButton::clicked, this, &AddMachineDialog::create);
    buttons->addWidget(m_go);
    lay->addLayout(buttons);
}

QString AddMachineDialog::problem() const {
    static const QRegularExpression idRx(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    if (!idRx.match(m_id->text().trimmed()).hasMatch()) {
        return L("The name: letters, digits, «.», «_» or «-», without spaces.",
                 "El nombre: letras, cifras, «.», «_» o «-», sin espacios.");
    }
    if (m_host->text().trimmed().isEmpty() || m_host->text().contains(QLatin1Char(' '))) {
        return L("The host, without spaces.", "La máquina (host), sin espacios.");
    }
    if (m_user->text().trimmed().isEmpty() || m_user->text().contains(QLatin1Char(' '))) {
        return L("The user, without spaces.", "El usuario, sin espacios.");
    }
    return {};
}

void AddMachineDialog::create() {
    const QString why = problem();
    if (!why.isEmpty()) {
        m_result->setText(why);
        return;
    }
    const QString id = m_id->text().trimmed();
    QString line = QStringLiteral("create %1 --type SSH --host %2 --user %3")
                       .arg(id, m_host->text().trimmed(), m_user->text().trimmed());
    if (m_port->value() != 22) {
        line += QStringLiteral(" --port %1").arg(m_port->value());
    }
    if (!m_key->text().trimmed().isEmpty()) {
        line += QStringLiteral(" --key ") + quoted(m_key->text().trimmed());
    }
    if (m_sudo->isChecked()) {
        line += QStringLiteral(" --sudo");
    }
    m_go->setEnabled(false);
    m_result->setText(
        L("Asking the machine which system it is…", "Preguntando a la máquina qué sistema es…"));
    m_store->general()->request(line, this, [this, id](const CliResult& r) {
        if (!r.ok()) {
            m_go->setEnabled(true);
            m_result->setText(r.err.trimmed());
            return;
        }
        m_created = id;
        for (QLineEdit* e : {m_id, m_host, m_user, m_key}) {
            e->setEnabled(false);
        }
        m_port->setEnabled(false);
        m_sudo->setEnabled(false);
        Q_EMIT added();
        probe();
    });
}

void AddMachineDialog::probe() {
    m_result->setText(L("Checking it…", "Comprobándola…"));
    m_store->general()->request(
        QStringLiteral("probe zfsm://") + m_created, this, [this](const CliResult& r) {
            const qsizetype start = r.out.indexOf(QLatin1Char('{'));
            const QJsonObject p = start < 0
                                      ? QJsonObject()
                                      : QJsonDocument::fromJson(r.out.mid(start).toUtf8()).object();
            QStringList lines;
            if (p.value(QStringLiteral("reachable")).toBool()) {
                lines << L("Added: %1, OpenZFS %2.", "Añadida: %1, OpenZFS %2.")
                             .arg(p.value(QStringLiteral("os_detail")).toString(),
                                  p.value(QStringLiteral("openzfs_version")).toString());
            }
            for (const QJsonValue& v : p.value(QStringLiteral("warnings")).toArray()) {
                const QJsonObject w = v.toObject();
                lines << (w.value(QStringLiteral("level")).toString() == QLatin1String("error")
                              ? QStringLiteral("✕ ")
                              : QStringLiteral("▲ "))
                             + w.value(QStringLiteral("text")).toString();
            }
            if (p.value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
                lines << L("Everything works.", "Todo funciona.");
            }
            m_result->setText(lines.join(QLatin1Char('\n')));
            const QString daemon = p.value(QStringLiteral("daemon")).toString();
            m_install->setVisible(
                p.value(QStringLiteral("reachable")).toBool()
                && (daemon == QLatin1String("unreachable") || daemon == QLatin1String("outdated")));
        });
}

}  // namespace zfsmgr::gui
