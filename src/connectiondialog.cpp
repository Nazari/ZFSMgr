#include "connectiondialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QComboBox>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>

ConnectionDialog::ConnectionDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Conexión"));
    resize(560, 380);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    form->addRow(QStringLiteral("Nombre"), m_nameEdit);

    m_connTypeCombo = new QComboBox(this);
    m_connTypeCombo->addItems({QStringLiteral("SSH"), QStringLiteral("PSRP")});
    form->addRow(QStringLiteral("Tipo"), m_connTypeCombo);

    m_osTypeCombo = new QComboBox(this);
    m_osTypeCombo->addItems({QStringLiteral("Linux"), QStringLiteral("macOS"), QStringLiteral("Windows")});
    form->addRow(QStringLiteral("SO"), m_osTypeCombo);

    m_transportCombo = new QComboBox(this);
    m_transportCombo->addItems({QStringLiteral("SSH"), QStringLiteral("PSRP")});
    form->addRow(QStringLiteral("Transporte"), m_transportCombo);

    m_hostEdit = new QLineEdit(this);
    form->addRow(QStringLiteral("Host"), m_hostEdit);

    m_portEdit = new QLineEdit(this);
    m_portEdit->setValidator(new QIntValidator(1, 65535, m_portEdit));
    m_portEdit->setText(QStringLiteral("22"));
    form->addRow(QStringLiteral("Puerto"), m_portEdit);

    m_userEdit = new QLineEdit(this);
    form->addRow(QStringLiteral("Usuario"), m_userEdit);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Password"), m_passwordEdit);

    m_keyEdit = new QLineEdit(this);
    form->addRow(QStringLiteral("Clave SSH"), m_keyEdit);

    m_sudoCheck = new QCheckBox(QStringLiteral("Usar sudo"), this);
    form->addRow(QStringLiteral("Privilegios"), m_sudoCheck);

    connect(m_osTypeCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateConnectionModeUi();
    });
    connect(m_connTypeCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateConnectionModeUi();
    });
    connect(m_transportCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateConnectionModeUi();
    });

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Aceptar"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancelar"));
    QPushButton* testBtn = buttons->addButton(QStringLiteral("Probar conexión"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(testBtn, &QPushButton::clicked, this, [this]() {
        testConnection();
    });
    root->addWidget(buttons);

    updateConnectionModeUi();
}

void ConnectionDialog::setProfile(const ConnectionProfile& profile) {
    m_id = profile.id;
    m_nameEdit->setText(profile.name);
    m_connTypeCombo->setCurrentText(profile.connType.isEmpty() ? QStringLiteral("SSH") : profile.connType);
    m_osTypeCombo->setCurrentText(profile.osType.isEmpty() ? QStringLiteral("Linux") : profile.osType);
    m_transportCombo->setCurrentText(profile.transport.isEmpty() ? QStringLiteral("SSH") : profile.transport);
    m_hostEdit->setText(profile.host);
    m_portEdit->setText(QString::number(profile.port > 0 ? profile.port : 22));
    m_userEdit->setText(profile.username);
    m_passwordEdit->setText(profile.password);
    m_keyEdit->setText(profile.keyPath);
    m_sudoCheck->setChecked(profile.useSudo);
    updateConnectionModeUi();
}

ConnectionProfile ConnectionDialog::profile() const {
    ConnectionProfile p;
    p.id = m_id;
    p.name = m_nameEdit->text().trimmed();
    p.connType = m_connTypeCombo->currentText().trimmed();
    p.osType = m_osTypeCombo->currentText().trimmed();
    p.transport = m_transportCombo->currentText().trimmed();
    p.host = m_hostEdit->text().trimmed();
    p.port = m_portEdit->text().toInt();
    if (p.port <= 0) {
        const bool psrpMode = (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0
                               || p.transport.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
        p.port = psrpMode ? 5985 : 22;
    }
    p.username = m_userEdit->text().trimmed();
    p.password = m_passwordEdit->text();
    p.keyPath = m_keyEdit->text().trimmed();
    p.useSudo = m_sudoCheck->isChecked();
    return p;
}

void ConnectionDialog::ensureDefaultPortForMode() {
    const QString connType = m_connTypeCombo->currentText().trimmed();
    const QString transport = m_transportCombo->currentText().trimmed();
    const bool isPsrp = (connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0
                         || transport.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
    const QString wantedPort = isPsrp ? QStringLiteral("5985") : QStringLiteral("22");
    const QString current = m_portEdit->text().trimmed();
    if (current.isEmpty() || current == m_lastAutoPort || current == QStringLiteral("22") || current == QStringLiteral("5985")) {
        m_portEdit->setText(wantedPort);
    }
    m_lastAutoPort = wantedPort;
}

void ConnectionDialog::updateConnectionModeUi() {
    const QString osType = m_osTypeCombo->currentText().trimmed();
    const QString connType = m_connTypeCombo->currentText().trimmed();
    const QString transport = m_transportCombo->currentText().trimmed();
    const bool isWindows = (osType.compare(QStringLiteral("Windows"), Qt::CaseInsensitive) == 0);
    const bool isPsrp = (connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0
                         || transport.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);

    if (isWindows && isPsrp) {
        QSignalBlocker b1(m_connTypeCombo);
        QSignalBlocker b2(m_transportCombo);
        m_connTypeCombo->setCurrentText(QStringLiteral("PSRP"));
        m_transportCombo->setCurrentText(QStringLiteral("PSRP"));
    } else if (!isPsrp) {
        QSignalBlocker b2(m_transportCombo);
        m_transportCombo->setCurrentText(QStringLiteral("SSH"));
    }

    const bool psrpMode = (m_connTypeCombo->currentText().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0
                           || m_transportCombo->currentText().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);

    m_keyEdit->setEnabled(!psrpMode);
    if (psrpMode) {
        m_keyEdit->clear();
    }
    m_sudoCheck->setEnabled(!psrpMode);
    if (psrpMode) {
        m_sudoCheck->setChecked(false);
    }

    if (psrpMode) {
        m_passwordEdit->setPlaceholderText(QStringLiteral("Credencial de Windows/PSRP"));
        m_portEdit->setPlaceholderText(QStringLiteral("5985"));
    } else {
        m_passwordEdit->setPlaceholderText(QStringLiteral("Password SSH"));
        m_portEdit->setPlaceholderText(QStringLiteral("22"));
    }

    ensureDefaultPortForMode();
}

bool ConnectionDialog::testSshConnection(const ConnectionProfile& p, QString& detail) const {
    detail.clear();
    const bool hasPassword = !p.password.trimmed().isEmpty();
    QString program = QStringLiteral("ssh");
    QStringList args;
    bool usingSshpass = false;
    if (hasPassword) {
        const QString sshpassExe = QStandardPaths::findExecutable(QStringLiteral("sshpass"));
        if (!sshpassExe.isEmpty()) {
            program = sshpassExe;
            args << "-p" << p.password << "ssh";
            usingSshpass = true;
        }
    }

    args << "-o" << (hasPassword ? "BatchMode=no" : "BatchMode=yes");
    args << "-o" << "ConnectTimeout=8";
    args << "-o" << "LogLevel=ERROR";
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
    if (hasPassword) {
        args << "-o" << "PreferredAuthentications=password,keyboard-interactive,publickey";
        args << "-o" << "NumberOfPasswordPrompts=1";
    }
    if (p.port > 0) {
        args << "-p" << QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        args << "-i" << p.keyPath;
    }
    args << QStringLiteral("%1@%2").arg(p.username, p.host);
    args << QStringLiteral("echo ZFSMGR_CONN_OK");

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(3000)) {
        detail = QStringLiteral("No se pudo iniciar %1").arg(program);
        return false;
    }
    if (hasPassword && !usingSshpass) {
        proc.write((p.password + QLatin1Char('\n')).toUtf8());
        proc.closeWriteChannel();
    }
    if (!proc.waitForFinished(12000)) {
        proc.kill();
        proc.waitForFinished(1000);
        detail = QStringLiteral("Timeout de conexión SSH");
        return false;
    }
    const int rc = proc.exitCode();
    const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    if (rc == 0 && out.contains(QStringLiteral("ZFSMGR_CONN_OK"))) {
        detail = QStringLiteral("SSH OK");
        return true;
    }
    detail = err.isEmpty() ? QStringLiteral("Error SSH (exit %1)").arg(rc) : err;
    return false;
}

void ConnectionDialog::testConnection() {
    const ConnectionProfile p = profile();
    if (p.host.isEmpty() || p.username.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             QStringLiteral("Complete al menos Host y Usuario para probar la conexión."));
        return;
    }
    if (p.port <= 0) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             QStringLiteral("Puerto inválido."));
        return;
    }

    const bool psrpMode = (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0
                           || p.transport.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
    if (psrpMode) {
        QMessageBox::information(this,
                                 QStringLiteral("ZFSMgr"),
                                 QStringLiteral("La prueba PSRP aún no valida autenticación en esta versión.\nUse SSH para prueba completa."));
        return;
    }

    QString detail;
    if (testSshConnection(p, detail)) {
        QMessageBox::information(this,
                                 QStringLiteral("ZFSMgr"),
                                 QStringLiteral("Conexión SSH correcta a %1@%2:%3")
                                     .arg(p.username, p.host)
                                     .arg(p.port));
        return;
    }
    QMessageBox::critical(this,
                          QStringLiteral("ZFSMgr"),
                          QStringLiteral("Fallo en prueba SSH:\n%1").arg(detail));
}
