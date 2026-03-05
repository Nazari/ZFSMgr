#include "connectiondialog.h"
#include "i18nmanager.h"

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

ConnectionDialog::ConnectionDialog(const QString& language, QWidget* parent)
    : QDialog(parent) {
    m_language = language.trimmed().toLower();
    if (m_language.isEmpty()) {
        m_language = QStringLiteral("es");
    }
    setWindowTitle(trk(QStringLiteral("t_conexi_n_d70cf0"),
                       QStringLiteral("Conexión"),
                       QStringLiteral("Connection"),
                       QStringLiteral("连接")));
    resize(560, 380);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    form->addRow(trk(QStringLiteral("t_nombre_e68491"),
                     QStringLiteral("Nombre"),
                     QStringLiteral("Name"),
                     QStringLiteral("名称")),
                 m_nameEdit);

    m_connTypeCombo = new QComboBox(this);
    m_connTypeCombo->addItems({QStringLiteral("SSH"), QStringLiteral("PSRP")});
    form->addRow(trk(QStringLiteral("t_tipo_6cc619"),
                     QStringLiteral("Tipo"),
                     QStringLiteral("Type"),
                     QStringLiteral("类型")),
                 m_connTypeCombo);

    m_osTypeCombo = new QComboBox(this);
    m_osTypeCombo->addItems({QStringLiteral("Linux"), QStringLiteral("macOS"), QStringLiteral("Windows")});
    form->addRow(trk(QStringLiteral("t_so_2290cf"),
                     QStringLiteral("SO"),
                     QStringLiteral("OS"),
                     QStringLiteral("系统")),
                 m_osTypeCombo);

    m_transportCombo = new QComboBox(this);
    m_transportCombo->addItems({QStringLiteral("SSH"), QStringLiteral("PSRP")});
    form->addRow(trk(QStringLiteral("t_transporte_62220c"),
                     QStringLiteral("Transporte"),
                     QStringLiteral("Transport"),
                     QStringLiteral("传输")),
                 m_transportCombo);

    m_hostEdit = new QLineEdit(this);
    form->addRow(trk(QStringLiteral("t_host_3960ec"),
                     QStringLiteral("Host"),
                     QStringLiteral("Host"),
                     QStringLiteral("主机")),
                 m_hostEdit);

    m_portEdit = new QLineEdit(this);
    m_portEdit->setValidator(new QIntValidator(1, 65535, m_portEdit));
    m_portEdit->setText(QStringLiteral("22"));
    form->addRow(trk(QStringLiteral("t_puerto_095508"),
                     QStringLiteral("Puerto"),
                     QStringLiteral("Port"),
                     QStringLiteral("端口")),
                 m_portEdit);

    m_userEdit = new QLineEdit(this);
    form->addRow(trk(QStringLiteral("t_usuario_3f2ecd"),
                     QStringLiteral("Usuario"),
                     QStringLiteral("User"),
                     QStringLiteral("用户")),
                 m_userEdit);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(trk(QStringLiteral("t_password_8be3c9"),
                     QStringLiteral("Password"),
                     QStringLiteral("Password"),
                     QStringLiteral("密码")),
                 m_passwordEdit);

    m_keyEdit = new QLineEdit(this);
    form->addRow(trk(QStringLiteral("t_clave_ssh_37a1aa"),
                     QStringLiteral("Clave SSH"),
                     QStringLiteral("SSH key"),
                     QStringLiteral("SSH 密钥")),
                 m_keyEdit);

    m_sudoCheck = new QCheckBox(trk(QStringLiteral("t_usar_sudo_e14aff"),
                                    QStringLiteral("Usar sudo"),
                                    QStringLiteral("Use sudo"),
                                    QStringLiteral("使用 sudo")),
                                this);
    form->addRow(trk(QStringLiteral("t_privilegio_1cb58a"),
                     QStringLiteral("Privilegios"),
                     QStringLiteral("Privileges"),
                     QStringLiteral("权限")),
                 m_sudoCheck);

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
    buttons->button(QDialogButtonBox::Ok)->setText(trk(QStringLiteral("t_aceptar_8f9f73"),
                                                       QStringLiteral("Aceptar"),
                                                       QStringLiteral("Accept"),
                                                       QStringLiteral("确认")));
    buttons->button(QDialogButtonBox::Cancel)->setText(trk(QStringLiteral("t_cancelar_c111e0"),
                                                           QStringLiteral("Cancelar"),
                                                           QStringLiteral("Cancel"),
                                                           QStringLiteral("取消")));
    QPushButton* testBtn = buttons->addButton(trk(QStringLiteral("t_probar_con_956752"),
                                                  QStringLiteral("Probar conexión"),
                                                  QStringLiteral("Test connection"),
                                                  QStringLiteral("测试连接")),
                                              QDialogButtonBox::ActionRole);
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
        m_passwordEdit->setPlaceholderText(tr3(QStringLiteral("Credencial de Windows/PSRP"),
                                               QStringLiteral("Windows/PSRP credential"),
                                               QStringLiteral("Windows/PSRP 凭据")));
        m_portEdit->setPlaceholderText(QStringLiteral("5985"));
    } else {
        m_passwordEdit->setPlaceholderText(tr3(QStringLiteral("Password SSH"), QStringLiteral("SSH password"), QStringLiteral("SSH 密码")));
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

    args << "-o" << "BatchMode=yes";
    args << "-o" << "ConnectTimeout=8";
    args << "-o" << "LogLevel=ERROR";
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
    if (hasPassword && usingSshpass) {
        args << "-o" << "BatchMode=no";
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
        detail = trk(QStringLiteral("t_no_se_pudo_99f7f4"),
                     QStringLiteral("No se pudo iniciar %1"),
                     QStringLiteral("Could not start %1"),
                     QStringLiteral("无法启动 %1")).arg(program);
        return false;
    }
    if (!proc.waitForFinished(12000)) {
        proc.kill();
        proc.waitForFinished(1000);
        detail = trk(QStringLiteral("t_timeout_de_0509c4"),
                     QStringLiteral("Timeout de conexión SSH"),
                     QStringLiteral("SSH connection timeout"),
                     QStringLiteral("SSH 连接超时"));
        return false;
    }
    const int rc = proc.exitCode();
    const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    if (rc == 0 && out.contains(QStringLiteral("ZFSMGR_CONN_OK"))) {
        detail = trk(QStringLiteral("t_ssh_ok_c1b8e6"),
                     QStringLiteral("SSH OK"),
                     QStringLiteral("SSH OK"),
                     QStringLiteral("SSH 正常"));
        return true;
    }
    detail = err.isEmpty()
                 ? trk(QStringLiteral("t_error_ssh__30fa40"),
                       QStringLiteral("Error SSH (exit %1)"),
                       QStringLiteral("SSH error (exit %1)"),
                       QStringLiteral("SSH 错误（退出码 %1）")).arg(rc)
                 : err;
    return false;
}

void ConnectionDialog::testConnection() {
    const ConnectionProfile p = profile();
    if (p.host.isEmpty() || p.username.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_complete_a_77b969"),
                                 QStringLiteral("Complete al menos Host y Usuario para probar la conexión."),
                                 QStringLiteral("Fill at least Host and User to test the connection."),
                                 QStringLiteral("至少填写主机和用户后再测试连接。")));
        return;
    }
    if (p.port <= 0) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_puerto_inv_1bda91"),
                                 QStringLiteral("Puerto inválido."),
                                 QStringLiteral("Invalid port."),
                                 QStringLiteral("端口无效。")));
        return;
    }

    const bool psrpMode = (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0
                           || p.transport.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
    if (psrpMode) {
        QMessageBox::information(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_la_prueba__40007d"),
                                     QStringLiteral("La prueba PSRP aún no valida autenticación en esta versión.\nUse SSH para prueba completa."),
                                     QStringLiteral("PSRP test does not validate authentication yet in this version.\nUse SSH for full test."),
                                     QStringLiteral("此版本中 PSRP 测试尚不验证认证。\n请使用 SSH 进行完整测试。")));
        return;
    }

    QString detail;
    if (testSshConnection(p, detail)) {
        QMessageBox::information(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_conexi_n_s_62acc8"),
                                     QStringLiteral("Conexión SSH correcta a %1@%2:%3"),
                                     QStringLiteral("SSH connection successful to %1@%2:%3"),
                                     QStringLiteral("SSH 连接成功：%1@%2:%3"))
                                     .arg(p.username, p.host)
                                     .arg(p.port));
        return;
    }
    if (!p.password.trimmed().isEmpty() && QStandardPaths::findExecutable(QStringLiteral("sshpass")).isEmpty()) {
        detail += QStringLiteral("\n\nNota: para autenticación por password sin prompt interactivo, instale sshpass.");
    }
    QMessageBox::critical(this,
                          QStringLiteral("ZFSMgr"),
                          trk(QStringLiteral("t_fallo_en_p_f63bd9"),
                              QStringLiteral("Fallo en prueba SSH:\n%1"),
                              QStringLiteral("SSH test failed:\n%1"),
                              QStringLiteral("SSH 测试失败：\n%1")).arg(detail));
}

QString ConnectionDialog::trk(const QString& key,
                              const QString& es,
                              const QString& en,
                              const QString& zh) const {
    return I18nManager::instance().translateKey(m_language, key, es, en, zh);
}

QString ConnectionDialog::tr3(const QString& es, const QString& en, const QString& zh) const {
    return I18nManager::instance().translate(m_language, es, en, zh);
}
