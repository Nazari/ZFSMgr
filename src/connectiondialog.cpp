#include "connectiondialog.h"
#include "i18nmanager.h"
#include "mainwindow_helpers.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFont>
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
#include <QFrame>
#include <QRegularExpression>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>

namespace {
QString oneLine(QString text) {
    return text.replace('\n', ' ').replace('\r', ' ').simplified();
}

QString macosFlavorLabel(const QString& fullText, const QString& versionText) {
    const QString full = fullText.trimmed();
    if (!full.isEmpty()) {
        return full;
    }
    const QString version = versionText.trimmed();
    if (version.isEmpty()) {
        return QStringLiteral("macOS");
    }
    const int major = version.section('.', 0, 0).toInt();
    QString codename;
    if (major == 15) {
        codename = QStringLiteral("Sequoia");
    } else if (major == 16) {
        codename = QStringLiteral("Tahoe");
    }
    return codename.isEmpty() ? QStringLiteral("macOS %1").arg(version)
                              : QStringLiteral("macOS %1 %2").arg(codename, version);
}

QString encodedPowerShellCommand(const QString& script) {
    const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
    return QString::fromLatin1(utf16.toBase64());
}

void setRequiredLabelState(QLabel* label, bool required) {
    if (!label) {
        return;
    }
    label->setStyleSheet(required
                             ? QStringLiteral("QLabel { color: #b00020; font-weight: 600; }")
                             : QString());
}

void bindRequiredLineEditLabel(QLineEdit* edit, QLabel* label) {
    if (!edit || !label) {
        return;
    }
    auto refresh = [edit, label]() {
        setRequiredLabelState(label, edit->text().trimmed().isEmpty());
    };
    QObject::connect(edit, &QLineEdit::textChanged, label, [refresh](const QString&) {
        refresh();
    });
    refresh();
}

// Tapa la contraseña en una orden antes de escribirla en el registro del diálogo.
// Cubre las dos formas en que puede aparecer: incrustada literal y —desde que se
// codifica para sobrevivir a la normalización de macOS— como escapes octales.
QString maskProbeCommandSecrets(const QString& cmd) {
    QString out = cmd;
    out.replace(QRegularExpression(QStringLiteral("printf\\s+'%b\\\\n'\\s+'(?:\\\\0[0-7]{1,3})*'")),
                QStringLiteral("printf '%b\\n' [secret]"));
    out.replace(QRegularExpression(QStringLiteral("printf\\s+'%s\\\\n'\\s+'(?:[^'\\\\]|\\\\.)*'")),
                QStringLiteral("printf '%s\\n' [secret]"));
    return out;
}

QString tsNowForLog() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

void appendConnectionDialogTrace(const QString& level, const QString& msg) {
    const QString configRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configRoot.isEmpty()) {
        return;
    }
    const QString cfgDir = configRoot + QStringLiteral("/ZFSMgr");
    QDir().mkpath(cfgDir);
    QFile f(cfgDir + QStringLiteral("/application.log"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    const QString line = QStringLiteral("[%1] [%2] [ConnectionDialog] %3\n")
                             .arg(tsNowForLog(), level, msg);
    f.write(line.toUtf8());
    f.close();
}
} // namespace

ConnectionDialog::ConnectionDialog(const QString& language, QWidget* parent)
    : QDialog(parent) {
    m_language = language.trimmed().toLower();
    if (m_language.isEmpty()) {
        m_language = QStringLiteral("es");
    }
    setFont(QApplication::font());
    {
        const QFont baseUiFont = QApplication::font();
        const int baseUiPointSize = qMax(6, baseUiFont.pointSize());
        setStyleSheet(QStringLiteral(
            "QLabel, QLineEdit, QComboBox, QPushButton, QCheckBox, QGroupBox { "
            "font-family: '%1'; font-size: %2pt; }")
                          .arg(baseUiFont.family(),
                               QString::number(baseUiPointSize)));
    }
    setWindowTitle(trk(QStringLiteral("t_conexi_n_d70cf0"),
                       QStringLiteral("Conexión"),
                       QStringLiteral("Connection"),
                       QStringLiteral("连接")));
    resize(640, 320);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    m_osInfoLabel = new QLabel(this);
    m_osInfoLabel->setMinimumWidth(180);
    m_osInfoLabel->setFrameShape(QFrame::StyledPanel);
    m_osInfoLabel->setFrameShadow(QFrame::Sunken);
    m_osInfoLabel->setMargin(4);
    m_connTypeCombo = new QComboBox(this);
    m_connTypeCombo->addItems({QStringLiteral("SSH")});
    auto* nameOsRow = new QWidget(this);
    auto* nameOsLayout = new QHBoxLayout(nameOsRow);
    nameOsLayout->setContentsMargins(0, 0, 0, 0);
    nameOsLayout->setSpacing(8);
    auto* nameLbl = new QLabel(trk(QStringLiteral("t_nombre_e68491"),
                                   QStringLiteral("Nombre"),
                                   QStringLiteral("Name"),
                                   QStringLiteral("名称")), nameOsRow);
    auto* osLbl = new QLabel(trk(QStringLiteral("t_so_2290cf"),
                                 QStringLiteral("S.O."),
                                 QStringLiteral("OS"),
                                 QStringLiteral("系统")), nameOsRow);
    auto* typeLbl = new QLabel(trk(QStringLiteral("t_tipo_6cc619"),
                                   QStringLiteral("Tipo"),
                                   QStringLiteral("Type"),
                                   QStringLiteral("类型")), nameOsRow);
    nameLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    osLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    typeLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    nameLbl->setMinimumWidth(76);
    osLbl->setMinimumWidth(76);
    typeLbl->setMinimumWidth(76);
    setRequiredLabelState(nameLbl, true);
    bindRequiredLineEditLabel(m_nameEdit, nameLbl);
    nameOsLayout->addWidget(nameLbl, 0);
    nameOsLayout->addWidget(m_nameEdit, 2);
    nameOsLayout->addSpacing(12);
    nameOsLayout->addWidget(osLbl, 0);
    nameOsLayout->addWidget(m_osInfoLabel, 1);
    nameOsLayout->addSpacing(12);
    nameOsLayout->addWidget(typeLbl, 0);
    nameOsLayout->addWidget(m_connTypeCombo, 1);
    form->addRow(QString(), nameOsRow);

    m_hostEdit = new QLineEdit(this);
    m_portEdit = new QLineEdit(this);
    m_sshFamilyCombo = new QComboBox(this);
    m_sshFamilyCombo->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    m_sshFamilyCombo->addItem(QStringLiteral("IPv4"), QStringLiteral("ipv4"));
    m_sshFamilyCombo->addItem(QStringLiteral("IPv6"), QStringLiteral("ipv6"));
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
    auto* familyLbl = new QLabel(trk(QStringLiteral("t_ip_family_001"),
                                     QStringLiteral("IP"),
                                     QStringLiteral("IP"),
                                     QStringLiteral("IP")), hostPortRow);
    hostLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    portLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    familyLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hostLbl->setMinimumWidth(76);
    portLbl->setMinimumWidth(76);
    familyLbl->setMinimumWidth(52);
    setRequiredLabelState(hostLbl, true);
    bindRequiredLineEditLabel(m_hostEdit, hostLbl);
    m_portEdit->setMaximumWidth(110);
    m_sshFamilyCombo->setMaximumWidth(110);
    hostPortLayout->addWidget(hostLbl, 0);
    hostPortLayout->addWidget(m_hostEdit, 1);
    hostPortLayout->addSpacing(12);
    hostPortLayout->addWidget(portLbl, 0);
    hostPortLayout->addWidget(m_portEdit, 0);
    hostPortLayout->addSpacing(12);
    hostPortLayout->addWidget(familyLbl, 0);
    hostPortLayout->addWidget(m_sshFamilyCombo, 0);
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
    setRequiredLabelState(userLbl, true);
    bindRequiredLineEditLabel(m_userEdit, userLbl);
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
    connect(okBtn, &QPushButton::clicked, this, [this]() { acceptDialog(); });
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
    updateDetectedOsLabel();
    if (layout()) {
        layout()->activate();
    }
    setFixedSize(sizeHint());
}

void ConnectionDialog::setProfile(const ConnectionProfile& profile) {
    m_id = profile.id;
    m_nameEdit->setText(profile.name);
    m_detectedOsType = profile.osType.trimmed();
    m_detectedOsFlavor.clear();
    m_connTypeCombo->setCurrentText(profile.connType.isEmpty() ? QStringLiteral("SSH") : profile.connType);
    updateDetectedOsLabel();
    updateConnectionModeUi();
    m_hostEdit->setText(profile.host);
    m_portEdit->setText(QString::number(profile.port > 0 ? profile.port : 22));
    if (m_sshFamilyCombo) {
        const QString family = profile.sshAddressFamily.trimmed().toLower();
        const int idx = m_sshFamilyCombo->findData(
            (family == QStringLiteral("ipv4") || family == QStringLiteral("ipv6")) ? family : QStringLiteral("auto"));
        m_sshFamilyCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    m_userEdit->setText(profile.username);
    m_passwordEdit->setText(profile.password);
    m_keyEdit->setText(profile.keyPath);
    updateConnectionModeUi();
}

ConnectionProfile ConnectionDialog::profile() const {
    ConnectionProfile p;
    p.id = m_id;
    p.name = m_nameEdit->text().trimmed();
    p.connType = m_connTypeCombo->currentText().trimmed();
    p.osType = m_detectedOsType.trimmed();
    p.host = m_hostEdit->text().trimmed();
    p.port = m_portEdit->text().toInt();
    if (p.port <= 0) {
        p.port = 22;
    }
    p.sshAddressFamily = m_sshFamilyCombo ? m_sshFamilyCombo->currentData().toString().trimmed().toLower()
                                          : QStringLiteral("auto");
    p.username = m_userEdit->text().trimmed();
    p.password = m_passwordEdit->text();
    p.keyPath = m_keyEdit->text().trimmed();
    p.useSudo = true;
    return p;
}

void ConnectionDialog::ensureDefaultPortForMode() {
    // Se siguen reconociendo 5985/5986 como "puerto puesto por la aplicación" para que
    // un perfil que venía de PSRP recupere el 22 al editarlo, en vez de conservar un
    // puerto de WinRM al que ya nadie escucha.
    const QString wantedPort = QStringLiteral("22");
    const QString current = m_portEdit->text().trimmed();
    if (current.isEmpty() || current == m_lastAutoPort || current == QStringLiteral("22")
        || current == QStringLiteral("5985") || current == QStringLiteral("5986")) {
        m_portEdit->setText(wantedPort);
    }
    m_lastAutoPort = wantedPort;
}

void ConnectionDialog::updateConnectionModeUi() {
    m_keyEdit->setEnabled(true);
    if (m_keyBrowseBtn) {
        m_keyBrowseBtn->setEnabled(true);
    }
    if (m_sshFamilyCombo) {
        m_sshFamilyCombo->setEnabled(true);
    }
    m_passwordEdit->setPlaceholderText(trk(QStringLiteral("t_ssh_pwd_ph001"), QStringLiteral("Password SSH"), QStringLiteral("SSH password"), QStringLiteral("SSH 密码")));
    m_portEdit->setPlaceholderText(QStringLiteral("22"));

    ensureDefaultPortForMode();
    updateDetectedOsLabel();
}

void ConnectionDialog::updateDetectedOsLabel() {
    if (!m_osInfoLabel) {
        return;
    }
    const QString osType = m_detectedOsType.trimmed();
    const QString flavor = m_detectedOsFlavor.trimmed();
    if (osType.isEmpty() && flavor.isEmpty()) {
        m_osInfoLabel->setText(trk(QStringLiteral("t_os_detect_pending_001"),
                                   QStringLiteral("Pendiente de identificar"),
                                   QStringLiteral("Pending identification"),
                                   QStringLiteral("待识别")));
        return;
    }
    m_osInfoLabel->setText(flavor.isEmpty() ? osType : QStringLiteral("%1 | %2").arg(osType, flavor));
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
        const QString sshpassExe = mwhelpers::findLocalExecutable(QStringLiteral("sshpass"));
        if (!sshpassExe.isEmpty()) {
            program = sshpassExe;
            args << "-p" << p.password << "ssh";
            usingSshpass = true;
        }
    }

    args << "-o" << "BatchMode=yes";
    args << "-o" << "ConnectTimeout=8";
    args << "-o" << "LogLevel=ERROR";
    // Se verifica al host contra ~/.ssh/known_hosts (ver sshBaseCommand).
    args << "-o" << "StrictHostKeyChecking=accept-new";
    const QString sshFamily = p.sshAddressFamily.trimmed().toLower();
    if (sshFamily == QStringLiteral("ipv4")) {
        args << "-4";
    } else if (sshFamily == QStringLiteral("ipv6")) {
        args << "-6";
    }
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
    // Un cambio de clave de host es lo primero que hay que explicar aquí: al dar de
    // alta o reprobar una conexión es cuando el usuario puede reaccionar.
    const QString hostKeyHint = mwhelpers::sshHostKeyProblemHint(err);
    if (!hostKeyHint.isEmpty()) {
        detail = hostKeyHint;
        return false;
    }
    detail = err.isEmpty()
                 ? trk(QStringLiteral("t_error_ssh__30fa40"),
                       QStringLiteral("Error SSH (exit %1)"),
                       QStringLiteral("SSH error (exit %1)"),
                       QStringLiteral("SSH 错误（退出码 %1）")).arg(rc)
                 : err;
    return false;
}

bool ConnectionDialog::runSshProbe(const ConnectionProfile& p,
                                   const QString& remoteCmd,
                                   int timeoutMs,
                                   QString& out,
                                   QString& err) const {
    out.clear();
    err.clear();
    const bool hasPassword = !p.password.trimmed().isEmpty();
    QString program = QStringLiteral("ssh");
    QStringList args;
    if (hasPassword) {
        const QString sshpassExe = mwhelpers::findLocalExecutable(QStringLiteral("sshpass"));
        if (!sshpassExe.isEmpty()) {
            program = sshpassExe;
            args << "-p" << p.password << "ssh";
        }
    }
    args << "-o" << "BatchMode=yes";
    args << "-o" << QStringLiteral("ConnectTimeout=%1").arg(qMax(3, timeoutMs / 1000));
    args << "-o" << "LogLevel=ERROR";
    // Se verifica al host contra ~/.ssh/known_hosts (ver sshBaseCommand).
    args << "-o" << "StrictHostKeyChecking=accept-new";
    const QString sshFamily = p.sshAddressFamily.trimmed().toLower();
    if (sshFamily == QStringLiteral("ipv4")) {
        args << "-4";
    } else if (sshFamily == QStringLiteral("ipv6")) {
        args << "-6";
    }
    if (hasPassword) {
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
    args << mwhelpers::asciiSafeShellCommand(remoteCmd);

    appendConnectionDialogTrace(
        QStringLiteral("DEBUG"),
        QStringLiteral("SSH probe start: %1@%2:%3 cmd=\"%4\" timeoutMs=%5 auth=%6 key=%7")
            .arg(p.username,
                 p.host,
                 QString::number(p.port),
                 // Enmascarado ANTES de escribir. Este registro es propio del diálogo y
                 // no pasa por maskSecrets de la ventana principal, así que la
                 // comprobación de la contraseña de sudo la dejaba escrita: en la forma
                 // nueva son escapes octales, trivialmente reversibles.
                 oneLine(maskProbeCommandSecrets(remoteCmd)),
                 QString::number(timeoutMs),
                 hasPassword ? QStringLiteral("password/key") : QStringLiteral("key/agent"),
                 p.keyPath.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes")));

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(3000)) {
        err = trk(QStringLiteral("t_no_se_pudo_99f7f4"),
                  QStringLiteral("No se pudo iniciar %1"),
                  QStringLiteral("Could not start %1"),
                  QStringLiteral("无法启动 %1")).arg(program);
        appendConnectionDialogTrace(QStringLiteral("WARN"),
                                    QStringLiteral("SSH probe failed to start: program=%1 detail=%2")
                                        .arg(program, oneLine(err)));
        return false;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        err = trk(QStringLiteral("t_timeout_de_0509c4"),
                  QStringLiteral("Timeout de conexión SSH"),
                  QStringLiteral("SSH connection timeout"),
                  QStringLiteral("SSH 连接超时"));
        appendConnectionDialogTrace(QStringLiteral("WARN"),
                                    QStringLiteral("SSH probe timeout: %1@%2:%3 cmd=\"%4\"")
                                        .arg(p.username,
                                             p.host,
                                             QString::number(p.port),
                                             oneLine(remoteCmd)));
        return false;
    }
    out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    const bool ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    appendConnectionDialogTrace(
        ok ? QStringLiteral("DEBUG") : QStringLiteral("WARN"),
        QStringLiteral("SSH probe finished rc=%1 ok=%2 stdout=\"%3\" stderr=\"%4\"")
            .arg(QString::number(proc.exitCode()),
                 ok ? QStringLiteral("true") : QStringLiteral("false"),
                 oneLine(out),
                 oneLine(err)));
    return ok;
}

bool ConnectionDialog::detectSshPlatform(const ConnectionProfile& p,
                                         QString& osTypeOut,
                                         QString& flavorOut,
                                         QString& detailOut) const {
    osTypeOut.clear();
    flavorOut.clear();
    detailOut.clear();

    QString out;
    QString err;
    if (runSshProbe(p, QStringLiteral("uname -s"), 8000, out, err)) {
        const QString uname = oneLine(out);
        if (uname.compare(QStringLiteral("Linux"), Qt::CaseInsensitive) == 0) {
            osTypeOut = QStringLiteral("Linux");
            QString lOut;
            QString lErr;
            const QString cmd =
                QStringLiteral("sh -lc '. /etc/os-release 2>/dev/null; printf \"%s %s\" \"$NAME\" \"$VERSION_ID\"'");
            if (runSshProbe(p, cmd, 8000, lOut, lErr)) {
                flavorOut = oneLine(lOut);
            }
            if (flavorOut.isEmpty()) {
                flavorOut = QStringLiteral("Linux");
            }
            detailOut = flavorOut;
            return true;
        }
        if (uname.compare(QStringLiteral("Darwin"), Qt::CaseInsensitive) == 0) {
            osTypeOut = QStringLiteral("macOS");
            QString mOut;
            QString mErr;
            QString fullText;
            QString versionText;
            runSshProbe(p,
                        QStringLiteral("sh -lc 'system_profiler SPSoftwareDataType 2>/dev/null | sed -n \"s/^ *System Version: //p\" | head -1'"),
                        10000,
                        mOut,
                        mErr);
            fullText = oneLine(mOut);
            mOut.clear();
            mErr.clear();
            runSshProbe(p, QStringLiteral("sw_vers -productVersion"), 8000, mOut, mErr);
            versionText = oneLine(mOut);
            flavorOut = macosFlavorLabel(fullText, versionText);
            detailOut = flavorOut;
            return true;
        }
        if (uname.compare(QStringLiteral("FreeBSD"), Qt::CaseInsensitive) == 0) {
            osTypeOut = QStringLiteral("FreeBSD");
            QString fOut;
            QString fErr;
            if (runSshProbe(p, QStringLiteral("freebsd-version -k || freebsd-version || uname -r"), 8000, fOut, fErr)) {
                flavorOut = QStringLiteral("FreeBSD %1").arg(oneLine(fOut));
            }
            if (flavorOut.isEmpty()) {
                flavorOut = QStringLiteral("FreeBSD");
            }
            detailOut = flavorOut;
            return true;
        }
    }

    const QString winVerCmd = QStringLiteral("cmd.exe /c ver");
    const bool winVerOk = runSshProbe(p, winVerCmd, 12000, out, err)
                          || runSshProbe(p, QStringLiteral("ver"), 12000, out, err);
    if (winVerOk) {
        osTypeOut = QStringLiteral("Windows");
        flavorOut = oneLine(out);
        if (flavorOut.isEmpty()) {
            flavorOut = QStringLiteral("Windows");
        }
        detailOut = flavorOut;

        // Optional refinement via PowerShell (best-effort only).
        QString psOut;
        QString psErr;
        const QString winScript = QStringLiteral(
            "$cv=Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'; "
            "$os=Get-CimInstance Win32_OperatingSystem; "
            "$name=$cv.ProductName; $ver=$cv.DisplayVersion; "
            "if([string]::IsNullOrWhiteSpace($ver)){$ver=$cv.ReleaseId}; "
            "if([string]::IsNullOrWhiteSpace($ver)){$ver=$os.Version}; "
            "Write-Output (($name + ' ' + $ver).Trim())");
        const QString winCmd = QStringLiteral(
            "powershell -NoProfile -NonInteractive -EncodedCommand %1")
                                   .arg(encodedPowerShellCommand(winScript));
        if (runSshProbe(p, winCmd, 12000, psOut, psErr)) {
            const QString refined = oneLine(psOut);
            if (!refined.isEmpty()) {
                flavorOut = refined;
                detailOut = flavorOut;
            }
        }
        return true;
    }

    const QString winScript = QStringLiteral(
        "$cv=Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'; "
        "$os=Get-CimInstance Win32_OperatingSystem; "
        "$name=$cv.ProductName; $ver=$cv.DisplayVersion; "
        "if([string]::IsNullOrWhiteSpace($ver)){$ver=$cv.ReleaseId}; "
        "if([string]::IsNullOrWhiteSpace($ver)){$ver=$os.Version}; "
        "Write-Output (($name + ' ' + $ver).Trim())");
    const QString winCmd = QStringLiteral(
        "powershell -NoProfile -NonInteractive -EncodedCommand %1")
                               .arg(encodedPowerShellCommand(winScript));
    if (runSshProbe(p, winCmd, 12000, out, err)) {
        osTypeOut = QStringLiteral("Windows");
        flavorOut = oneLine(out);
        if (flavorOut.isEmpty()) {
            flavorOut = QStringLiteral("Windows");
        }
        detailOut = flavorOut;
        return true;
    }

    detailOut = oneLine(err);
    return false;
}

void ConnectionDialog::testConnection() {
    const ConnectionProfile p = profile();
    appendConnectionDialogTrace(
        QStringLiteral("INFO"),
        QStringLiteral("Test connection clicked: name=\"%1\" mode=%2 host=%3 port=%4 user=%5")
            .arg(p.name,
                 p.connType.isEmpty() ? QStringLiteral("SSH") : p.connType,
                 p.host,
                 QString::number(p.port),
                 p.username));
    if (p.host.isEmpty() || p.username.isEmpty()) {
        appendConnectionDialogTrace(QStringLiteral("WARN"),
                                    QStringLiteral("Test connection blocked: missing host/user"));
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_complete_a_77b969"),
                                 QStringLiteral("Complete al menos Host y Usuario para probar la conexión."),
                                 QStringLiteral("Fill at least Host and User to test the connection."),
                                 QStringLiteral("至少填写主机和用户后再测试连接。")));
        return;
    }
    if (p.port <= 0) {
        appendConnectionDialogTrace(QStringLiteral("WARN"),
                                    QStringLiteral("Test connection blocked: invalid port=%1")
                                        .arg(QString::number(p.port)));
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_puerto_inv_1bda91"),
                                 QStringLiteral("Puerto inválido."),
                                 QStringLiteral("Invalid port."),
                                 QStringLiteral("端口无效。")));
        return;
    }

    QString detail;
    if (testSshConnection(p, detail)) {
        appendConnectionDialogTrace(QStringLiteral("INFO"),
                                    QStringLiteral("SSH test success: %1@%2:%3")
                                        .arg(p.username, p.host, QString::number(p.port)));
        QString osType;
        QString flavor;
        QString osDetail;
        if (detectSshPlatform(p, osType, flavor, osDetail)) {
            appendConnectionDialogTrace(QStringLiteral("DEBUG"),
                                        QStringLiteral("SSH platform detected: osType=%1 flavor=\"%2\"")
                                            .arg(osType, oneLine(flavor)));
            m_detectedOsType = osType;
            m_detectedOsFlavor = flavor;
        } else {
            appendConnectionDialogTrace(QStringLiteral("WARN"),
                                        QStringLiteral("SSH platform detection failed: %1")
                                            .arg(oneLine(osDetail)));
            m_detectedOsType.clear();
            m_detectedOsFlavor.clear();
        }
        updateDetectedOsLabel();
        QMessageBox::information(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_conexi_n_s_62acc8"),
                                     QStringLiteral("Conexión SSH correcta a %1@%2:%3\nSistema: %4"),
                                     QStringLiteral("SSH connection successful to %1@%2:%3\nSystem: %4"),
                                     QStringLiteral("SSH 连接成功：%1@%2:%3\n系统：%4"))
                                     .arg(p.username)
                                     .arg(p.host)
                                     .arg(p.port)
                                     .arg(m_detectedOsFlavor.isEmpty()
                                              ? (m_detectedOsType.isEmpty()
                                                     ? trk(QStringLiteral("t_os_detect_pending_001"),
                                                           QStringLiteral("Pendiente de identificar"),
                                                           QStringLiteral("Pending identification"),
                                                           QStringLiteral("待识别"))
                                                     : m_detectedOsType)
                                              : QStringLiteral("%1 | %2").arg(m_detectedOsType, m_detectedOsFlavor)));
        return;
    }
    if (!p.password.trimmed().isEmpty() && mwhelpers::findLocalExecutable(QStringLiteral("sshpass")).isEmpty()) {
        detail += QStringLiteral("\n\nNota: para autenticación por password sin prompt interactivo, instale sshpass.");
        appendConnectionDialogTrace(
            QStringLiteral("INFO"),
            QStringLiteral("SSH test warning: password provided but sshpass executable not found"));
    }
    appendConnectionDialogTrace(QStringLiteral("WARN"),
                                QStringLiteral("SSH test failed: %1")
                                    .arg(oneLine(detail)));
    QMessageBox::critical(this,
                          QStringLiteral("ZFSMgr"),
                          trk(QStringLiteral("t_fallo_en_p_f63bd9"),
                              QStringLiteral("Fallo en prueba SSH:\n%1"),
                              QStringLiteral("SSH test failed:\n%1"),
                              QStringLiteral("SSH 测试失败：\n%1")).arg(detail));
}

mwhelpers::SudoCheck ConnectionDialog::checkRemoteSudoPassword(const ConnectionProfile& p,
                                                               QString& detailOut) const {
    detailOut.clear();
    if (!p.useSudo || mwhelpers::isWindowsOsType(p.osType) || p.password.isEmpty()) {
        return mwhelpers::SudoCheck::Ok;
    }
    // La orden exacta que se usará en producción, no una aproximación: así lo que se
    // valida es lo que después se ejecuta, incluida la codificación de la contraseña.
    const QString cmd = mwhelpers::withSudoCommand(p, QStringLiteral("true"));
    QString out;
    QString err;
    if (!runSshProbe(p, cmd, 15000, out, err)) {
        detailOut = mwhelpers::oneLine(err.isEmpty() ? out : err);
        if (mwhelpers::looksLikeSudoAuthFailure(detailOut)) {
            return mwhelpers::SudoCheck::WrongPassword;
        }
        return mwhelpers::SudoCheck::CouldNotCheck;
    }
    return mwhelpers::SudoCheck::Ok;
}

void ConnectionDialog::acceptDialog() {
    if (m_connTypeCombo
        && m_connTypeCombo->currentText().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0) {
        ConnectionProfile p = profile();
        if (p.host.isEmpty() || p.username.isEmpty()) {
            QMessageBox::warning(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_complete_a_77b969"),
                                     QStringLiteral("Complete al menos Host y Usuario para probar la conexión."),
                                     QStringLiteral("Fill at least Host and User to test the connection."),
                                     QStringLiteral("至少填写主机和用户后再测试连接。")));
            return;
        }
        QString osType;
        QString flavor;
        QString detail;
        if (!detectSshPlatform(p, osType, flavor, detail)) {
            QMessageBox::warning(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_detect_os_fail_001"),
                                     QStringLiteral("No se pudo identificar el sistema operativo remoto por SSH.\nPruebe la conexión antes de guardar.\n\n%1"),
                                     QStringLiteral("Could not identify the remote operating system over SSH.\nTest the connection before saving.\n\n%1"),
                                     QStringLiteral("无法通过 SSH 识别远程操作系统。\n请先测试连接再保存。\n\n%1"))
                                     .arg(detail));
            return;
        }
        m_detectedOsType = osType;
        m_detectedOsFlavor = flavor;
        updateDetectedOsLabel();

        // La contraseña de sudo se comprueba AQUÍ. Con SSH por clave, una contraseña
        // de sudo equivocada no impide conectar ni da ningún error al guardar: el
        // fallo aparece mucho más tarde y disfrazado de "el agente no está instalado".
        p.osType = osType;
        QString sudoDetail;
        const mwhelpers::SudoCheck sudoCheck = checkRemoteSudoPassword(p, sudoDetail);
        if (sudoCheck == mwhelpers::SudoCheck::WrongPassword) {
            appendConnectionDialogTrace(QStringLiteral("WARN"),
                                        QStringLiteral("Sudo password rejected on %1@%2")
                                            .arg(p.username, p.host));
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_conn_sudo_bad_001"),
                    QStringLiteral("La contraseña de sudo no es válida en %1@%2.\n%3\n\nCorríjala antes de guardar."),
                    QStringLiteral("The sudo password is not valid on %1@%2.\n%3\n\nFix it before saving."),
                    QStringLiteral("在 %1@%2 上 sudo 密码无效。\n%3\n\n请先更正再保存。"))
                    .arg(p.username, p.host, sudoDetail));
            return;
        }
        // No se pudo comprobar: se avisa y se deja guardar. Bloquear por no haber
        // podido comprobar impediría dar de alta una conexión por un fallo ajeno.
        if (sudoCheck == mwhelpers::SudoCheck::CouldNotCheck) {
            appendConnectionDialogTrace(QStringLiteral("WARN"),
                                        QStringLiteral("Sudo password unverified on %1@%2: %3")
                                            .arg(p.username, p.host, sudoDetail));
            const auto proceed = QMessageBox::question(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_conn_sudo_unchecked_001"),
                    QStringLiteral("No se pudo comprobar la contraseña de sudo en %1@%2:\n%3\n\n¿Guardar de todos modos?"),
                    QStringLiteral("Could not verify the sudo password on %1@%2:\n%3\n\nSave anyway?"),
                    QStringLiteral("无法验证 %1@%2 上的 sudo 密码：\n%3\n\n仍要保存吗？"))
                    .arg(p.username, p.host, sudoDetail),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);
            if (proceed != QMessageBox::Yes) {
                return;
            }
        }
    } else {
        m_detectedOsType = QStringLiteral("Windows");
        m_detectedOsFlavor = QStringLiteral("Windows");
        updateDetectedOsLabel();
    }
    accept();
}

QString ConnectionDialog::trk(const QString& key,
                              const QString& es,
                              const QString& en,
                              const QString& zh) const {
    return I18nManager::instance().translateKey(m_language, key, es, en, zh);
}
