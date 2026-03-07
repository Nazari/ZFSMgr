#include "connectiondialog.h"
#include "i18nmanager.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QComboBox>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QHBoxLayout>
#include <QFileDialog>

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
    resize(640, 320);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    form->addRow(trk(QStringLiteral("t_nombre_e68491"),
                     QStringLiteral("Nombre"),
                     QStringLiteral("Name"),
                     QStringLiteral("名称")),
                 m_nameEdit);

    m_osTypeCombo = new QComboBox(this);
    m_osTypeCombo->addItems({QStringLiteral("Linux"), QStringLiteral("macOS"), QStringLiteral("Windows")});
    m_connTypeCombo = new QComboBox(this);
    m_connTypeCombo->addItems({QStringLiteral("SSH")});
    auto* osTypeRow = new QWidget(this);
    auto* osTypeLayout = new QHBoxLayout(osTypeRow);
    osTypeLayout->setContentsMargins(0, 0, 0, 0);
    osTypeLayout->setSpacing(8);
    auto* osLbl = new QLabel(trk(QStringLiteral("t_so_2290cf"),
                                 QStringLiteral("S.O."),
                                 QStringLiteral("OS"),
                                 QStringLiteral("系统")), osTypeRow);
    auto* typeLbl = new QLabel(trk(QStringLiteral("t_tipo_6cc619"),
                                   QStringLiteral("Tipo"),
                                   QStringLiteral("Type"),
                                   QStringLiteral("类型")), osTypeRow);
    osLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    typeLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    osLbl->setMinimumWidth(76);
    typeLbl->setMinimumWidth(76);
    osTypeLayout->addWidget(osLbl, 0);
    osTypeLayout->addWidget(m_osTypeCombo, 1);
    osTypeLayout->addSpacing(12);
    osTypeLayout->addWidget(typeLbl, 0);
    osTypeLayout->addWidget(m_connTypeCombo, 1);
    form->addRow(QString(), osTypeRow);

    m_hostEdit = new QLineEdit(this);
    m_portEdit = new QLineEdit(this);
    m_portEdit->setValidator(new QIntValidator(1, 65535, m_portEdit));
    m_portEdit->setText(QStringLiteral("22"));
    auto* hostPortRow = new QWidget(this);
    auto* hostPortLayout = new QHBoxLayout(hostPortRow);
    hostPortLayout->setContentsMargins(0, 0, 0, 0);
    hostPortLayout->setSpacing(8);
    auto* hostLbl = new QLabel(trk(QStringLiteral("t_host_3960ec"),
                                   QStringLiteral("Host"),
                                   QStringLiteral("Host"),
                                   QStringLiteral("主机")), hostPortRow);
    auto* portLbl = new QLabel(trk(QStringLiteral("t_puerto_095508"),
                                   QStringLiteral("Port"),
                                   QStringLiteral("Port"),
                                   QStringLiteral("端口")), hostPortRow);
    hostLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    portLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hostLbl->setMinimumWidth(76);
    portLbl->setMinimumWidth(76);
    m_portEdit->setMaximumWidth(110);
    hostPortLayout->addWidget(hostLbl, 0);
    hostPortLayout->addWidget(m_hostEdit, 1);
    hostPortLayout->addSpacing(12);
    hostPortLayout->addWidget(portLbl, 0);
    hostPortLayout->addWidget(m_portEdit, 0);
    form->addRow(QString(), hostPortRow);

    m_userEdit = new QLineEdit(this);
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    auto* userPassRow = new QWidget(this);
    auto* userPassLayout = new QHBoxLayout(userPassRow);
    userPassLayout->setContentsMargins(0, 0, 0, 0);
    userPassLayout->setSpacing(8);
    auto* userLbl = new QLabel(trk(QStringLiteral("t_usuario_3f2ecd"),
                                   QStringLiteral("Usuario"),
                                   QStringLiteral("User"),
                                   QStringLiteral("用户")), userPassRow);
    auto* passLbl = new QLabel(trk(QStringLiteral("t_password_8be3c9"),
                                   QStringLiteral("Password"),
                                   QStringLiteral("Password"),
                                   QStringLiteral("密码")), userPassRow);
    userLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    passLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    userLbl->setMinimumWidth(76);
    passLbl->setMinimumWidth(76);
    userPassLayout->addWidget(userLbl, 0);
    userPassLayout->addWidget(m_userEdit, 1);
    userPassLayout->addSpacing(12);
    userPassLayout->addWidget(passLbl, 0);
    userPassLayout->addWidget(m_passwordEdit, 1);
    form->addRow(QString(), userPassRow);

    m_keyEdit = new QLineEdit(this);
    m_keyBrowseBtn = new QPushButton(
        trk(QStringLiteral("t_browse_btn001"),
            QStringLiteral("Examinar..."),
            QStringLiteral("Browse..."),
            QStringLiteral("浏览...")),
        this);
    auto* keyRow = new QWidget(this);
    auto* keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(8);
    auto* keyLbl = new QLabel(trk(QStringLiteral("t_clave_ssh_37a1aa"),
                                  QStringLiteral("Clave privada SSH"),
                                  QStringLiteral("SSH private key"),
                                  QStringLiteral("SSH 私钥")), keyRow);
    keyLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    keyLbl->setMinimumWidth(76);
    keyLayout->addWidget(keyLbl, 0);
    keyLayout->addWidget(m_keyEdit, 1);
    keyLayout->addWidget(m_keyBrowseBtn, 0);
    form->addRow(QString(), keyRow);

    m_sudoCheck = new QCheckBox(trk(QStringLiteral("t_usar_sudo_e14aff"),
                                    QStringLiteral("Usar sudo"),
                                    QStringLiteral("Use sudo"),
                                    QStringLiteral("使用 sudo")),
                                this);
    m_privilegesRow = new QWidget(this);
    auto* privLayout = new QHBoxLayout(m_privilegesRow);
    privLayout->setContentsMargins(0, 0, 0, 0);
    privLayout->setSpacing(8);
    auto* privLbl = new QLabel(trk(QStringLiteral("t_privilegio_1cb58a"),
                                   QStringLiteral("Privilegios"),
                                   QStringLiteral("Privileges"),
                                   QStringLiteral("权限")), m_privilegesRow);
    privLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    privLbl->setMinimumWidth(76);
    privLayout->addWidget(privLbl, 0);
    privLayout->addWidget(m_sudoCheck, 0, Qt::AlignLeft);
    privLayout->addStretch(1);
    form->addRow(QString(), m_privilegesRow);

    connect(m_osTypeCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateConnectionModeUi();
    });
    connect(m_connTypeCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateConnectionModeUi();
    });
    connect(m_keyBrowseBtn, &QPushButton::clicked, this, [this]() { browsePrivateKey(); });

    root->addLayout(form);

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 6, 0, 0);
    btnRow->setSpacing(8);
    auto* testBtn = new QPushButton(trk(QStringLiteral("t_probar_con_956752"),
                                        QStringLiteral("Probar conexión"),
                                        QStringLiteral("Test connection"),
                                        QStringLiteral("测试连接")), this);
    auto* okBtn = new QPushButton(trk(QStringLiteral("t_aceptar_8f9f73"),
                                      QStringLiteral("Aceptar"),
                                      QStringLiteral("Accept"),
                                      QStringLiteral("确认")), this);
    auto* cancelBtn = new QPushButton(trk(QStringLiteral("t_cancelar_c111e0"),
                                          QStringLiteral("Cancelar"),
                                          QStringLiteral("Cancel"),
                                          QStringLiteral("取消")), this);
    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(testBtn, &QPushButton::clicked, this, [this]() {
        testConnection();
    });
    btnRow->addWidget(testBtn, 0);
    btnRow->addStretch(1);
    btnRow->addWidget(okBtn, 0);
    btnRow->addWidget(cancelBtn, 0);
    root->addLayout(btnRow);

    updateConnectionModeUi();
    if (layout()) {
        layout()->activate();
    }
    setFixedSize(sizeHint());
}

void ConnectionDialog::setProfile(const ConnectionProfile& profile) {
    m_id = profile.id;
    m_nameEdit->setText(profile.name);
    m_osTypeCombo->setCurrentText(profile.osType.isEmpty() ? QStringLiteral("Linux") : profile.osType);
    updateConnectionModeUi();
    m_connTypeCombo->setCurrentText(profile.connType.isEmpty() ? QStringLiteral("SSH") : profile.connType);
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
    p.transport = p.connType;
    p.host = m_hostEdit->text().trimmed();
    p.port = m_portEdit->text().toInt();
    if (p.port <= 0) {
        const bool psrpMode = (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
        p.port = psrpMode ? 5986 : 22;
    }
    p.username = m_userEdit->text().trimmed();
    p.password = m_passwordEdit->text();
    p.keyPath = m_keyEdit->text().trimmed();
    p.useSudo = m_sudoCheck->isChecked();
    return p;
}

void ConnectionDialog::ensureDefaultPortForMode() {
    const QString connType = m_connTypeCombo->currentText().trimmed();
    const bool isPsrp = (connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
    const QString wantedPort = isPsrp ? QStringLiteral("5986") : QStringLiteral("22");
    const QString current = m_portEdit->text().trimmed();
    if (current.isEmpty() || current == m_lastAutoPort || current == QStringLiteral("22") || current == QStringLiteral("5985") || current == QStringLiteral("5986")) {
        m_portEdit->setText(wantedPort);
    }
    m_lastAutoPort = wantedPort;
}

void ConnectionDialog::updateConnectionModeUi() {
    const QString osType = m_osTypeCombo->currentText().trimmed();
    const bool isWindows = (osType.compare(QStringLiteral("Windows"), Qt::CaseInsensitive) == 0);
    {
        QSignalBlocker b(m_connTypeCombo);
        const QString prev = m_connTypeCombo->currentText().trimmed().toUpper();
        m_connTypeCombo->clear();
        if (isWindows) {
            m_connTypeCombo->addItems({QStringLiteral("SSH"), QStringLiteral("PSRP")});
            m_connTypeCombo->setCurrentText(prev == QStringLiteral("PSRP") ? QStringLiteral("PSRP") : QStringLiteral("SSH"));
        } else {
            m_connTypeCombo->addItem(QStringLiteral("SSH"));
            m_connTypeCombo->setCurrentText(QStringLiteral("SSH"));
        }
    }
    const bool psrpMode = (m_connTypeCombo->currentText().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);

    m_keyEdit->setEnabled(!psrpMode);
    if (m_keyBrowseBtn) {
        m_keyBrowseBtn->setEnabled(!psrpMode);
    }
    if (psrpMode) {
        m_keyEdit->clear();
    }
    m_sudoCheck->setEnabled(!psrpMode);
    if (psrpMode) {
        m_sudoCheck->setChecked(false);
    }
    if (m_privilegesRow) {
        m_privilegesRow->setVisible(!psrpMode);
    }

    if (psrpMode) {
        m_passwordEdit->setPlaceholderText(trk(QStringLiteral("t_psrp_cred_ph01"), QStringLiteral("Credencial de Windows/PSRP"),
                                               QStringLiteral("Windows/PSRP credential"),
                                               QStringLiteral("Windows/PSRP 凭据")));
        m_portEdit->setPlaceholderText(QStringLiteral("5986"));
    } else {
        m_passwordEdit->setPlaceholderText(trk(QStringLiteral("t_ssh_pwd_ph001"), QStringLiteral("Password SSH"), QStringLiteral("SSH password"), QStringLiteral("SSH 密码")));
        m_portEdit->setPlaceholderText(QStringLiteral("22"));
    }

    ensureDefaultPortForMode();
}

void ConnectionDialog::browsePrivateKey() {
    const QString selected = QFileDialog::getOpenFileName(
        this,
        trk(QStringLiteral("t_pick_ssh_key001"),
            QStringLiteral("Seleccionar clave privada SSH"),
            QStringLiteral("Select SSH private key"),
            QStringLiteral("选择 SSH 私钥")),
        m_keyEdit ? m_keyEdit->text().trimmed() : QString(),
        trk(QStringLiteral("t_all_files_001"),
            QStringLiteral("Todos los archivos (*)"),
            QStringLiteral("All files (*)"),
            QStringLiteral("所有文件 (*)")));
    if (!selected.isEmpty() && m_keyEdit) {
        m_keyEdit->setText(selected);
    }
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

    const bool psrpMode = (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
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
