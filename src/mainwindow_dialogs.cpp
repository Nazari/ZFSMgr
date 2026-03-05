#include "mainwindow.h"
#include "i18nmanager.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
QString trkl(const QString& lang,
             const QString& key,
             const QString& es = QString(),
             const QString& en = QString(),
             const QString& zh = QString()) {
    return I18nManager::instance().translateKey(lang, key, es, en, zh);
}

QString prettifyCommandText(const QString& cmd) {
    QString pretty = cmd.trimmed();
    pretty.replace(QStringLiteral(" && "), QStringLiteral(" &&\n  "));
    pretty.replace(QStringLiteral(" || "), QStringLiteral(" ||\n  "));
    pretty.replace(QStringLiteral(" | "), QStringLiteral(" |\n  "));
    pretty.replace(QStringLiteral("; "), QStringLiteral(";\n"));
    return pretty;
}

QString decodePowerShellEncodedCommand(const QString& encoded) {
    const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
    if (decoded.isEmpty()) {
        return QString();
    }
    QByteArray utf16 = decoded;
    if ((utf16.size() % 2) != 0) {
        utf16.chop(1);
    }
    if (utf16.isEmpty()) {
        return QString();
    }
    const QString script = QString::fromUtf16(reinterpret_cast<const char16_t*>(utf16.constData()),
                                              utf16.size() / 2);
    return script.trimmed();
}

QString pseudoStepForSegment(const QString& segmentRaw, const QString& lang) {
    const QString segment = segmentRaw.trimmed();
    const QString s = segment.toLower();
    if (s.contains(QStringLiteral("ssh "))) {
        if (s.contains(QStringLiteral("zfs send"))) {
            return trkl(lang,
                        QStringLiteral("t_psd_ssh_send01"),
                        QStringLiteral("Conectar por SSH al origen y enviar stream ZFS (`zfs send`)."),
                        QStringLiteral("Connect over SSH to source and send ZFS stream (`zfs send`)."),
                        QStringLiteral("通过 SSH 连接源端并发送 ZFS 数据流（`zfs send`）。"));
        }
        if (s.contains(QStringLiteral("zfs recv"))) {
            return trkl(lang,
                        QStringLiteral("t_psd_ssh_recv01"),
                        QStringLiteral("Conectar por SSH al destino y recibir stream ZFS (`zfs recv`)."),
                        QStringLiteral("Connect over SSH to target and receive ZFS stream (`zfs recv`)."),
                        QStringLiteral("通过 SSH 连接目标端并接收 ZFS 数据流（`zfs recv`）。"));
        }
        if (s.contains(QStringLiteral("zpool export"))) {
            return trkl(lang,
                        QStringLiteral("t_psd_ssh_exp01"),
                        QStringLiteral("Conectar por SSH y exportar pool (`zpool export`)."),
                        QStringLiteral("Connect over SSH and export pool (`zpool export`)."),
                        QStringLiteral("通过 SSH 连接并导出池（`zpool export`）。"));
        }
        if (s.contains(QStringLiteral("zpool import"))) {
            return trkl(lang,
                        QStringLiteral("t_psd_ssh_imp01"),
                        QStringLiteral("Conectar por SSH e importar pool (`zpool import`)."),
                        QStringLiteral("Connect over SSH and import pool (`zpool import`)."),
                        QStringLiteral("通过 SSH 连接并导入池（`zpool import`）。"));
        }
        return trkl(lang,
                    QStringLiteral("t_psd_ssh_exec01"),
                    QStringLiteral("Conectar por SSH y ejecutar comando remoto."),
                    QStringLiteral("Connect over SSH and execute remote command."),
                    QStringLiteral("通过 SSH 连接并执行远程命令。"));
    }
    if (s.contains(QStringLiteral("pv -trab"))) {
        return trkl(lang,
                    QStringLiteral("t_psd_pv_prog01"),
                    QStringLiteral("Mostrar progreso de transferencia con `pv`."),
                    QStringLiteral("Show transfer progress with `pv`."),
                    QStringLiteral("用 `pv` 显示传输进度。"));
    }
    if (s.contains(QStringLiteral("zfs send"))) {
        return trkl(lang,
                    QStringLiteral("t_psd_send_gen01"),
                    QStringLiteral("Generar stream ZFS desde snapshot/dataset (`zfs send`)."),
                    QStringLiteral("Generate ZFS stream from snapshot/dataset (`zfs send`)."),
                    QStringLiteral("从快照/数据集生成 ZFS 数据流（`zfs send`）。"));
    }
    if (s.contains(QStringLiteral("zfs recv"))) {
        return trkl(lang,
                    QStringLiteral("t_psd_recv_app01"),
                    QStringLiteral("Aplicar stream ZFS en destino (`zfs recv`)."),
                    QStringLiteral("Apply ZFS stream on target (`zfs recv`)."),
                    QStringLiteral("在目标端应用 ZFS 数据流（`zfs recv`）。"));
    }
    if (s.contains(QStringLiteral("zfs rollback"))) {
        return trkl(lang,
                    QStringLiteral("t_psd_rollbck01"),
                    QStringLiteral("Revertir dataset al snapshot seleccionado (`zfs rollback`)."),
                    QStringLiteral("Rollback dataset to selected snapshot (`zfs rollback`)."),
                    QStringLiteral("将数据集回滚到选定快照（`zfs rollback`）。"));
    }
    if (s.contains(QStringLiteral("zfs mount")) || s.contains(QStringLiteral("zfs unmount"))) {
        return trkl(lang,
                    QStringLiteral("t_psd_mountop01"),
                    QStringLiteral("Montar/desmontar dataset ZFS."),
                    QStringLiteral("Mount/unmount ZFS dataset."),
                    QStringLiteral("挂载/卸载 ZFS 数据集。"));
    }
    if (s.contains(QStringLiteral("zfs set ")) || s.contains(QStringLiteral("zfs get "))) {
        return trkl(lang,
                    QStringLiteral("t_psd_zfsprop01"),
                    QStringLiteral("Modificar/consultar propiedades ZFS."),
                    QStringLiteral("Modify/query ZFS properties."),
                    QStringLiteral("修改/查询 ZFS 属性。"));
    }
    if (s.contains(QStringLiteral("powershell "))) {
        return trkl(lang,
                    QStringLiteral("t_psd_ps_exec01"),
                    QStringLiteral("Ejecutar script PowerShell."),
                    QStringLiteral("Execute PowerShell script."),
                    QStringLiteral("执行 PowerShell 脚本。"));
    }
    if (s.contains(QStringLiteral("sudo "))) {
        return trkl(lang,
                    QStringLiteral("t_psd_sudo_exec1"),
                    QStringLiteral("Elevar permisos con sudo y ejecutar comando."),
                    QStringLiteral("Elevate with sudo and execute command."),
                    QStringLiteral("通过 sudo 提权并执行命令。"));
    }
    return trkl(lang,
                QStringLiteral("t_psd_subcmd_001"),
                QStringLiteral("Ejecutar subcomando: %1"),
                QStringLiteral("Execute subcommand: %1"),
                QStringLiteral("执行子命令：%1")).arg(segment.left(120));
}

QString formatCommandPreview(const QString& input, const QString& lang) {
    QString header;
    QString body = input;
    const int nl = input.indexOf('\n');
    if (nl >= 0) {
        header = input.left(nl).trimmed();
        body = input.mid(nl + 1).trimmed();
    } else {
        body = input.trimmed();
    }
    if (body.isEmpty()) {
        return input;
    }

    const QString pretty = prettifyCommandText(body);

    QStringList pseudo;
    const QStringList segments = body.split('|', Qt::SkipEmptyParts);
    pseudo.reserve(segments.size());
    for (const QString& seg : segments) {
        pseudo.push_back(pseudoStepForSegment(seg, lang));
    }

    QStringList decodedBlocks;
    const QRegularExpression psRx(
        QStringLiteral("powershell\\s+[^\\n\\r]*?-EncodedCommand\\s+(['\\\"]?)([A-Za-z0-9+/=]+)\\1"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = psRx.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString enc = m.captured(2).trimmed();
        if (enc.isEmpty()) {
            continue;
        }
        const QString dec = decodePowerShellEncodedCommand(enc);
        if (!dec.isEmpty()) {
            decodedBlocks.push_back(prettifyCommandText(dec));
        }
    }

    QStringList out;
    if (!header.isEmpty()) {
        out.push_back(header);
    }
    out.push_back(trkl(lang,
                       QStringLiteral("t_psd_read_sum01"),
                       QStringLiteral("Resumen legible:"),
                       QStringLiteral("Readable summary:"),
                       QStringLiteral("可读摘要：")));
    if (pseudo.isEmpty()) {
        out.push_back(trkl(lang,
                           QStringLiteral("t_psd_exec_cmd01"),
                           QStringLiteral("  1. Ejecutar comando."),
                           QStringLiteral("  1. Execute command."),
                           QStringLiteral("  1. 执行命令。")));
    } else {
        for (int i = 0; i < pseudo.size(); ++i) {
            out.push_back(QStringLiteral("  %1. %2").arg(i + 1).arg(pseudo[i]));
        }
    }

    if (!decodedBlocks.isEmpty()) {
        out.push_back(QStringLiteral(""));
        out.push_back(trkl(lang,
                           QStringLiteral("t_psd_ps_dec001"),
                           QStringLiteral("PowerShell decodificado:"),
                           QStringLiteral("Decoded PowerShell:"),
                           QStringLiteral("解码后的 PowerShell：")));
        for (int i = 0; i < decodedBlocks.size(); ++i) {
            out.push_back(QStringLiteral("  [script %1]").arg(i + 1));
            out.push_back(QStringLiteral("  ") + decodedBlocks[i]);
        }
    }

    out.push_back(QStringLiteral(""));
    out.push_back(trkl(lang,
                       QStringLiteral("t_psd_cmd_real01"),
                       QStringLiteral("Comando real (formateado):"),
                       QStringLiteral("Actual command (formatted):"),
                       QStringLiteral("实际命令（已格式化）：")));
    out.push_back(QStringLiteral("  ") + pretty);
    return out.join(QStringLiteral("\n"));
}
} // namespace

bool MainWindow::confirmActionExecution(const QString& actionName, const QStringList& commands, bool forceDialog) {
    if (!forceDialog && !m_actionConfirmEnabled) {
        return true;
    }
    if (commands.isEmpty()) {
        return true;
    }
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(980, 520);
    dlg.setWindowTitle(trk(QStringLiteral("t_confirm_ej_001"),
                           QStringLiteral("Confirmar ejecución"),
                           QStringLiteral("Confirm execution"),
                           QStringLiteral("确认执行")));

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    QLabel* intro = new QLabel(
        trk(QStringLiteral("t_confirm_cmds_001"),
            QStringLiteral("Se van a ejecutar estos comandos para la acción: %1").arg(actionName),
            QStringLiteral("These commands will be executed for action: %1").arg(actionName),
            QStringLiteral("将为该操作执行以下命令：%1").arg(actionName)),
        &dlg);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QStringList rendered;
    rendered.reserve(commands.size());
    for (const QString& cmd : commands) {
        rendered.push_back(formatCommandPreview(cmd, m_language));
    }

    QPlainTextEdit* txt = new QPlainTextEdit(&dlg);
    txt->setReadOnly(true);
    txt->setPlainText(rendered.join(QStringLiteral("\n\n")));
    root->addWidget(txt, 1);

    QDialogButtonBox* box = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = box->addButton(trk(QStringLiteral("t_cancelar_c111e0"),
                                                QStringLiteral("Cancelar"),
                                                QStringLiteral("Cancel"),
                                                QStringLiteral("取消")),
                                            QDialogButtonBox::RejectRole);
    QPushButton* okBtn = box->addButton(trk(QStringLiteral("t_aceptar_8f9f73"),
                                            QStringLiteral("Aceptar"),
                                            QStringLiteral("Accept"),
                                            QStringLiteral("确认")),
                                        QDialogButtonBox::AcceptRole);
    root->addWidget(box);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    const bool accepted = (dlg.exec() == QDialog::Accepted);
    if (!accepted) {
        appLog(QStringLiteral("INFO"), trk(QStringLiteral("t_acc_cancel_usr1"),
                                           QStringLiteral("Acción cancelada por el usuario: %1").arg(actionName),
                                           QStringLiteral("Action canceled by user: %1").arg(actionName),
                                           QStringLiteral("用户已取消操作：%1").arg(actionName)));
    }
    return accepted;
}

bool MainWindow::selectItemsDialog(const QString& title, const QString& intro, const QStringList& items, QStringList& selected) {
    selected.clear();
    if (items.isEmpty()) {
        return false;
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(640, 520);
    dlg.setWindowTitle(title);
    QVBoxLayout* root = new QVBoxLayout(&dlg);

    QLabel* introLbl = new QLabel(intro, &dlg);
    introLbl->setWordWrap(true);
    root->addWidget(introLbl);

    QListWidget* list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    for (const QString& item : items) {
        auto* lw = new QListWidgetItem(item, list);
        lw->setFlags(lw->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        lw->setCheckState(Qt::Checked);
    }
    root->addWidget(list, 1);

    QHBoxLayout* tools = new QHBoxLayout();
    QPushButton* allBtn = new QPushButton(trk(QStringLiteral("t_sel_all_001"),
                                              QStringLiteral("Seleccionar todo"),
                                              QStringLiteral("Select all"),
                                              QStringLiteral("全选")),
                                          &dlg);
    QPushButton* noneBtn = new QPushButton(trk(QStringLiteral("t_sel_none_001"),
                                               QStringLiteral("Deseleccionar todo"),
                                               QStringLiteral("Clear all"),
                                               QStringLiteral("全不选")),
                                           &dlg);
    tools->addWidget(allBtn);
    tools->addWidget(noneBtn);
    tools->addStretch(1);
    root->addLayout(tools);

    QObject::connect(allBtn, &QPushButton::clicked, &dlg, [list]() {
        for (int i = 0; i < list->count(); ++i) {
            list->item(i)->setCheckState(Qt::Checked);
        }
    });
    QObject::connect(noneBtn, &QPushButton::clicked, &dlg, [list]() {
        for (int i = 0; i < list->count(); ++i) {
            list->item(i)->setCheckState(Qt::Unchecked);
        }
    });

    QDialogButtonBox* box = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = box->addButton(trk(QStringLiteral("t_cancelar_c111e0"),
                                                QStringLiteral("Cancelar"),
                                                QStringLiteral("Cancel"),
                                                QStringLiteral("取消")),
                                            QDialogButtonBox::RejectRole);
    QPushButton* okBtn = box->addButton(trk(QStringLiteral("t_aceptar_8f9f73"),
                                            QStringLiteral("Aceptar"),
                                            QStringLiteral("Accept"),
                                            QStringLiteral("确认")),
                                        QDialogButtonBox::AcceptRole);
    root->addWidget(box);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem* it = list->item(i);
        if (it && it->checkState() == Qt::Checked) {
            selected.push_back(it->text());
        }
    }
    return !selected.isEmpty();
}

void MainWindow::openConfigurationDialog() {
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(trk(QStringLiteral("t_config_001"),
                           QStringLiteral("Configuración"),
                           QStringLiteral("Configuration"),
                           QStringLiteral("配置")));
    dlg.resize(500, 240);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    QFormLayout* form = new QFormLayout();

    QComboBox* langCombo = new QComboBox(&dlg);
    langCombo->addItem(QStringLiteral("Español"), QStringLiteral("es"));
    langCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    langCombo->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    int idx = langCombo->findData(m_language);
    langCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    form->addRow(trk(QStringLiteral("t_idioma_009433"),
                     QStringLiteral("Idioma"),
                     QStringLiteral("Language"),
                     QStringLiteral("语言")),
                 langCombo);

    QCheckBox* confirmChk = new QCheckBox(
        trk(QStringLiteral("t_show_confirm_001"),
            QStringLiteral("Mostrar confirmación antes de ejecutar acciones"),
            QStringLiteral("Show confirmation before executing actions"),
            QStringLiteral("执行操作前显示确认")),
        &dlg);
    confirmChk->setChecked(m_actionConfirmEnabled);
    form->addRow(QString(), confirmChk);

    QSpinBox* logSizeSpin = new QSpinBox(&dlg);
    logSizeSpin->setRange(1, 1024);
    logSizeSpin->setSuffix(QStringLiteral(" MB"));
    logSizeSpin->setValue(qMax(1, m_logMaxSizeMb));
    form->addRow(trk(QStringLiteral("t_log_max_rot_001"),
                     QStringLiteral("Tamaño máximo log rotativo"),
                     QStringLiteral("Max rotating log size"),
                     QStringLiteral("滚动日志最大大小")),
                 logSizeSpin);

    root->addLayout(form);
    root->addStretch(1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(trk(QStringLiteral("t_cancelar_c111e0"),
                                                    QStringLiteral("Cancelar"),
                                                    QStringLiteral("Cancel"),
                                                    QStringLiteral("取消")),
                                                QDialogButtonBox::RejectRole);
    QPushButton* okBtn = buttons->addButton(trk(QStringLiteral("t_aceptar_8f9f73"),
                                                QStringLiteral("Aceptar"),
                                                QStringLiteral("Accept"),
                                                QStringLiteral("确认")),
                                            QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString newLang = langCombo->currentData().toString().trimmed().toLower();
    const bool newConfirm = confirmChk->isChecked();
    const int newLogMaxMb = qMax(1, logSizeSpin->value());
    const bool langChanged = (newLang != m_language);
    m_language = newLang.isEmpty() ? QStringLiteral("es") : newLang;
    m_store.setLanguage(m_language);
    m_actionConfirmEnabled = newConfirm;
    m_logMaxSizeMb = newLogMaxMb;
    saveUiSettings();
    appLog(QStringLiteral("INFO"),
           QStringLiteral("Configuración actualizada: idioma=%1, confirmación=%2, log_max_mb=%3")
               .arg(m_language,
                    m_actionConfirmEnabled ? QStringLiteral("on") : QStringLiteral("off"),
                    QString::number(m_logMaxSizeMb)));
    if (langChanged) {
        applyLanguageLive();
    }
}
