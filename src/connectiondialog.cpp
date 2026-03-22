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

namespace {
QString sanitizePsrpDetail(QString raw) {
    if (raw.isEmpty()) {
        return raw;
    }
    raw.replace(QStringLiteral("#< CLIXML"), QStringLiteral(""));
    raw.replace(QRegularExpression(QStringLiteral("_x[0-9A-Fa-f]{4}_")), QStringLiteral(""));
    raw.replace(QRegularExpression(QStringLiteral("<[^>]+>")), QStringLiteral(""));
    raw.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    raw.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    raw.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    raw.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    raw.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    return raw.simplified();
}

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

void setRequiredLabelState(QLabel* label, bool required) {
    if (!label) {
        return;
    }
    label->setStyleSheet(required
                             ? QStringLiteral("QLabel { color: #b00020; font-weight: 600; }")
                             : QString());
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
    m_connTypeCombo->addItems({QStringLiteral("SSH"), QStringLiteral("PSRP")});
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
    m_sudoCheck->setChecked(profile.useSudo);
    updateConnectionModeUi();
}

ConnectionProfile ConnectionDialog::profile() const {
    ConnectionProfile p;
    p.id = m_id;
    p.name = m_nameEdit->text().trimmed();
    p.connType = m_connTypeCombo->currentText().trimmed();
    p.osType = (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0)
                   ? QStringLiteral("Windows")
                   : m_detectedOsType.trimmed();
    p.host = m_hostEdit->text().trimmed();
    p.port = m_portEdit->text().toInt();
    if (p.port <= 0) {
        const bool psrpMode = (p.connType.compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
        p.port = psrpMode ? 5986 : 22;
    }
    p.sshAddressFamily = m_sshFamilyCombo ? m_sshFamilyCombo->currentData().toString().trimmed().toLower()
                                          : QStringLiteral("auto");
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
    if (m_sshFamilyCombo) {
        m_sshFamilyCombo->setEnabled(!psrpMode);
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
    updateDetectedOsLabel();
}

void ConnectionDialog::updateDetectedOsLabel() {
    if (!m_osInfoLabel) {
        return;
    }
    const bool psrpMode = (m_connTypeCombo
                           && m_connTypeCombo->currentText().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0);
    if (psrpMode) {
        m_osInfoLabel->setText(QStringLiteral("Windows | PSRP"));
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
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
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
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
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
    args << remoteCmd;

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(3000)) {
        err = trk(QStringLiteral("t_no_se_pudo_99f7f4"),
                  QStringLiteral("No se pudo iniciar %1"),
                  QStringLiteral("Could not start %1"),
                  QStringLiteral("无法启动 %1")).arg(program);
        return false;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        err = trk(QStringLiteral("t_timeout_de_0509c4"),
                  QStringLiteral("Timeout de conexión SSH"),
                  QStringLiteral("SSH connection timeout"),
                  QStringLiteral("SSH 连接超时"));
        return false;
    }
    out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
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

    const QString winCmd = QStringLiteral(
        "powershell -NoProfile -NonInteractive -Command "
        "\"$cv=Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'; "
        "$os=Get-CimInstance Win32_OperatingSystem; "
        "$name=$cv.ProductName; $ver=$cv.DisplayVersion; "
        "if([string]::IsNullOrWhiteSpace($ver)){$ver=$cv.ReleaseId}; "
        "if([string]::IsNullOrWhiteSpace($ver)){$ver=$os.Version}; "
        "Write-Output (($name + ' ' + $ver).Trim())\"");
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

bool ConnectionDialog::testPsrpConnection(const ConnectionProfile& p, QString& detail) const {
    detail.clear();
    QString program = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
    if (program.isEmpty()) {
        program = QStandardPaths::findExecutable(QStringLiteral("powershell"));
    }
    if (program.isEmpty()) {
        detail = trk(QStringLiteral("t_psrp_bin_nf001"),
                     QStringLiteral("No se encontró pwsh/powershell para validar PSRP."),
                     QStringLiteral("pwsh/powershell not found to validate PSRP."),
                     QStringLiteral("未找到 pwsh/powershell，无法验证 PSRP。"));
        return false;
    }

    const QString hostEsc = QString(p.host).replace('\'', QStringLiteral("''"));
    const QString userEsc = QString(p.username).replace('\'', QStringLiteral("''"));
    const QString passB64 = QString::fromLatin1(p.password.toUtf8().toBase64());
    const int port = (p.port > 0) ? p.port : 5986;

    const QString script = QStringLiteral(
        "$pwd=[System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%1')); "
        "$sec=ConvertTo-SecureString $pwd -AsPlainText -Force; "
        "$cred=New-Object System.Management.Automation.PSCredential('%2',$sec); "
        "$so=$null; "
        "try { $so=New-PSSessionOption -SkipCACheck -SkipCNCheck -SkipRevocationCheck } "
        "catch { $so=New-PSSessionOption -SkipCACheck -SkipCNCheck }; "
        "$r=$null; "
        "try { "
        "  $r=Invoke-Command -ComputerName '%3' -Port %4 -UseSSL -Authentication Negotiate -Credential $cred -SessionOption $so "
        "    -ScriptBlock { [System.Environment]::OSVersion.VersionString } -ErrorAction Stop 2>&1 "
        "} catch { "
        "  $r=Invoke-Command -ComputerName '%3' -Port %4 -UseSSL -Authentication Basic -Credential $cred -SessionOption $so "
        "    -ScriptBlock { [System.Environment]::OSVersion.VersionString } -ErrorAction Stop 2>&1 "
        "}; "
        "$rc=$LASTEXITCODE; "
        "$r | ForEach-Object { $_.ToString() }; "
        "if($rc -eq $null){ $rc=0 }; "
        "exit [int]$rc;")
                               .arg(passB64, userEsc, hostEsc, QString::number(port));

    const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
    const QString encoded = QString::fromLatin1(utf16.toBase64());
    QStringList args;
    args << "-NoProfile" << "-NonInteractive" << "-EncodedCommand" << encoded;

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(3000)) {
        detail = trk(QStringLiteral("t_no_se_pudo_99f7f4"),
                     QStringLiteral("No se pudo iniciar %1"),
                     QStringLiteral("Could not start %1"),
                     QStringLiteral("无法启动 %1")).arg(program);
        return false;
    }
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        proc.waitForFinished(1000);
        detail = trk(QStringLiteral("t_timeout_psrp001"),
                     QStringLiteral("Timeout de conexión PSRP"),
                     QStringLiteral("PSRP connection timeout"),
                     QStringLiteral("PSRP 连接超时"));
        return false;
    }

    const int rc = proc.exitCode();
    const QString out = sanitizePsrpDetail(QString::fromUtf8(proc.readAllStandardOutput()).trimmed());
    const QString err = sanitizePsrpDetail(QString::fromUtf8(proc.readAllStandardError()).trimmed());
    const QString merged = (out + QStringLiteral("\n") + err).trimmed();
    if (rc == 0 && !out.isEmpty()) {
        detail = out.section('\n', 0, 0).trimmed();
        return true;
    }
    if (merged.contains(QStringLiteral("no supported wsman client library"), Qt::CaseInsensitive)) {
        detail = trk(QStringLiteral("t_psrp_wsman_miss"),
                     QStringLiteral("PSRP no disponible: falta cliente WSMan en este sistema.\nInstale PSWSMan para PowerShell (ejemplo: Install-Module PSWSMan; Install-WSMan)."),
                     QStringLiteral("PSRP unavailable: WSMan client library is missing on this system.\nInstall PSWSMan for PowerShell (e.g. Install-Module PSWSMan; Install-WSMan)."),
                     QStringLiteral("PSRP 不可用：此系统缺少 WSMan 客户端库。\n请安装 PowerShell 的 PSWSMan（例如：Install-Module PSWSMan; Install-WSMan）。"));
        return false;
    }
    detail = err.isEmpty()
                 ? trk(QStringLiteral("t_error_psrp_001"),
                       QStringLiteral("Error PSRP (exit %1)"),
                       QStringLiteral("PSRP error (exit %1)"),
                       QStringLiteral("PSRP 错误（退出码 %1）")).arg(rc)
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
        if (p.password.trimmed().isEmpty()) {
            QMessageBox::warning(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_psrp_pwd_req001"),
                                     QStringLiteral("Para validar una conexión PSRP debe indicar password."),
                                     QStringLiteral("A password is required to validate a PSRP connection."),
                                     QStringLiteral("验证 PSRP 连接需要密码。")));
            return;
        }
        QString psrpDetail;
        if (testPsrpConnection(p, psrpDetail)) {
            QMessageBox::information(this,
                                     QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_psrp_ok_msg001"),
                                         QStringLiteral("Conexión PSRP correcta a %1@%2:%3\n%4"),
                                         QStringLiteral("PSRP connection successful to %1@%2:%3\n%4"),
                                         QStringLiteral("PSRP 连接成功：%1@%2:%3\n%4"))
                                         .arg(p.username, p.host)
                                         .arg(p.port)
                                         .arg(psrpDetail));
            return;
        }
        QMessageBox::critical(this,
                              QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_psrp_failmsg001"),
                                  QStringLiteral("Fallo en prueba PSRP:\n%1"),
                                  QStringLiteral("PSRP test failed:\n%1"),
                                  QStringLiteral("PSRP 测试失败：\n%1")).arg(psrpDetail));
        return;
    }

    QString detail;
    if (testSshConnection(p, detail)) {
        QString osType;
        QString flavor;
        QString osDetail;
        if (detectSshPlatform(p, osType, flavor, osDetail)) {
            m_detectedOsType = osType;
            m_detectedOsFlavor = flavor;
        } else {
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
                                     .arg(p.username, p.host)
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
    }
    QMessageBox::critical(this,
                          QStringLiteral("ZFSMgr"),
                          trk(QStringLiteral("t_fallo_en_p_f63bd9"),
                              QStringLiteral("Fallo en prueba SSH:\n%1"),
                              QStringLiteral("SSH test failed:\n%1"),
                              QStringLiteral("SSH 测试失败：\n%1")).arg(detail));
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
