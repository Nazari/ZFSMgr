#include "mainwindow.h"
#include "i18nmanager.h"
#include "mainwindow_helpers.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
QString trkl(const QString& lang,
             const QString& key,
             const QString& es = QString(),
             const QString& en = QString(),
             const QString& zh = QString()) {
    return I18nManager::instance().translateKey(lang, key, es, en, zh);
}

class ToggleCheckEventFilter final : public QObject {
public:
    explicit ToggleCheckEventFilter(QCheckBox* checkbox, QObject* parent = nullptr)
        : QObject(parent)
        , m_checkbox(checkbox) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (!m_checkbox) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() != QEvent::MouseButtonRelease) {
            return QObject::eventFilter(watched, event);
        }
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() != Qt::LeftButton) {
            return QObject::eventFilter(watched, event);
        }
        m_checkbox->setChecked(!m_checkbox->isChecked());
        return true;
    }

private:
    QPointer<QCheckBox> m_checkbox;
};

class RelayoutOnResizeEventFilter final : public QObject {
public:
    explicit RelayoutOnResizeEventFilter(std::function<void()> fn, QObject* parent = nullptr)
        : QObject(parent)
        , m_fn(std::move(fn)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (event && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
            if (m_fn) {
                m_fn();
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_fn;
};

class MultiMoveListWidget final : public QListWidget {
public:
    explicit MultiMoveListWidget(QWidget* parent = nullptr)
        : QListWidget(parent) {}

    std::function<void()> onInternalReorder;

protected:
    void dropEvent(QDropEvent* event) override {
        if (!event || dragDropMode() != QAbstractItemView::InternalMove) {
            QListWidget::dropEvent(event);
            return;
        }
        QList<QListWidgetItem*> selected = selectedItems();
        if (selected.size() <= 1) {
            QListWidget::dropEvent(event);
            if (event->isAccepted() && onInternalReorder) {
                onInternalReorder();
            }
            return;
        }

        QList<int> selectedRows;
        QStringList movedTexts;
        for (QListWidgetItem* item : selected) {
            if (!item) {
                continue;
            }
            selectedRows.push_back(row(item));
        }
        std::sort(selectedRows.begin(), selectedRows.end());
        selectedRows.erase(std::unique(selectedRows.begin(), selectedRows.end()), selectedRows.end());
        for (int rowIndex : std::as_const(selectedRows)) {
            if (QListWidgetItem* item = this->item(rowIndex)) {
                movedTexts.push_back(item->text());
            }
        }
        if (movedTexts.isEmpty()) {
            QListWidget::dropEvent(event);
            return;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint dropPos = event->position().toPoint();
#else
        const QPoint dropPos = event->pos();
#endif
        int insertRow = count();
        if (QListWidgetItem* target = itemAt(dropPos)) {
            insertRow = row(target);
            const QRect vr = visualItemRect(target);
            if (dropPos.y() > vr.center().y()) {
                ++insertRow;
            }
        }

        int removedBeforeInsert = 0;
        for (int rowIndex : std::as_const(selectedRows)) {
            if (rowIndex < insertRow) {
                ++removedBeforeInsert;
            }
        }
        insertRow -= removedBeforeInsert;
        insertRow = qBound(0, insertRow, count());

        {
            const QSignalBlocker blockModel(model());
            const QSignalBlocker blockList(this);
            for (int i = selectedRows.size() - 1; i >= 0; --i) {
                delete takeItem(selectedRows.at(i));
            }
            for (int i = 0; i < movedTexts.size(); ++i) {
                auto* item = new QListWidgetItem(movedTexts.at(i));
                insertItem(insertRow + i, item);
                item->setSelected(true);
            }
        }
        setCurrentRow(insertRow);
        event->acceptProposedAction();
        if (onInternalReorder) {
            onInternalReorder();
        }
    }
};

QString prettifyCommandText(const QString& cmd) {
    QString pretty = cmd.trimmed();
    pretty.replace(QStringLiteral(" && "), QStringLiteral(" &&\n  "));
    pretty.replace(QStringLiteral(" || "), QStringLiteral(" ||\n  "));
    pretty.replace(QStringLiteral(" | "), QStringLiteral(" |\n  "));
    pretty.replace(QStringLiteral("; "), QStringLiteral(";\n"));
    return pretty;
}

QString maskSecretsForPreview(const QString& input) {
    QString out = input;

    const auto replaceAll = [&out](const QRegularExpression& rx, const QString& replacement) {
        out.replace(rx, replacement);
    };

    replaceAll(QRegularExpression(QStringLiteral("(SSHPASS=)([^\\s]+)")),
               QStringLiteral("\\1[secret]"));
    replaceAll(QRegularExpression(QStringLiteral("(printf\\s+'%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'(?=\\s*\\|\\s*sudo\\s+-S)")),
               QStringLiteral("\\1'[secret]'"));
    replaceAll(QRegularExpression(QStringLiteral("(printf\\s+'%s\\\\n'\\s+)'(?:[^'\\\\]|\\\\.)*'(?=\\s*;\\s*cat\\s*;\\s*}\\s*\\|\\s*sudo\\s+-S)")),
               QStringLiteral("\\1'[secret]'"));

    return out;
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
        rendered.push_back(formatCommandPreview(maskSecretsForPreview(cmd), m_language));
    }

    QPlainTextEdit* txt = new QPlainTextEdit(&dlg);
    txt->setReadOnly(true);
    txt->setPlainText(rendered.join(QStringLiteral("\n\n")));
    root->addWidget(txt, 1);

    QHBoxLayout* footer = new QHBoxLayout();
    QCheckBox* confirmCb = new QCheckBox(
        trk(QStringLiteral("t_confirm_before_exec_001"),
            QStringLiteral("Confirmar acciones antes de ejecutar"),
            QStringLiteral("Confirm actions before executing"),
            QStringLiteral("执行前确认操作")),
        &dlg);
    confirmCb->setChecked(m_actionConfirmEnabled);
    footer->addWidget(confirmCb);
    footer->addStretch(1);

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
    footer->addWidget(box);
    root->addLayout(footer);
    QObject::connect(confirmCb, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_confirmActionsMenuAction) {
            if (m_confirmActionsMenuAction->isChecked() != checked) {
                m_confirmActionsMenuAction->setChecked(checked);
            }
            return;
        }
        m_actionConfirmEnabled = checked;
        saveUiSettings();
    });
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

bool MainWindow::selectItemsDialog(const QString& title,
                                   const QString& intro,
                                   const QStringList& items,
                                   QStringList& selected,
                                   const QString& detail,
                                   const QMap<QString, QString>& invalidItems) {
    if (items.isEmpty()) {
        return false;
    }
    const QStringList initialSelected = selected;

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(760, 560);
    dlg.setWindowTitle(title);
    QVBoxLayout* root = new QVBoxLayout(&dlg);

    QLabel* introLbl = new QLabel(intro, &dlg);
    introLbl->setWordWrap(true);
    root->addWidget(introLbl);

    if (!detail.trimmed().isEmpty()) {
        QLabel* detailLbl = new QLabel(detail, &dlg);
        detailLbl->setWordWrap(true);
        detailLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailLbl->setStyleSheet(QStringLiteral("QLabel { color: #4b6170; }"));
        root->addWidget(detailLbl);
    }

    QScrollArea* scroll = new QScrollArea(&dlg);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* grid = new QGridLayout(content);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(4);
    grid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    content->setLayout(grid);
    scroll->setWidget(content);

    const QFontMetrics fm(dlg.font());
    int maxTextWidth = 0;
    for (const QString& item : items) {
        maxTextWidth = qMax(maxTextWidth, fm.horizontalAdvance(item));
    }
    const int maxCellWidth = qBound(86, maxTextWidth + 20, 180);
    QMap<QString, QString> invalidByLower;
    for (auto it = invalidItems.cbegin(); it != invalidItems.cend(); ++it) {
        invalidByLower.insert(it.key().trimmed().toLower(), it.value().trimmed());
    }
    QVector<QCheckBox*> checkboxes;
    checkboxes.reserve(items.size());
    QVector<QWidget*> cards;
    cards.reserve(items.size());
    QVector<QLabel*> labels;
    labels.reserve(items.size());
    for (int i = 0; i < items.size(); ++i) {
        const QString& item = items.at(i);
        auto* card = new QWidget(content);
        card->setFixedWidth(maxCellWidth);
        card->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        card->setStyleSheet(QStringLiteral(
            "QWidget {"
            " border: 1px solid #b9c9d6;"
            " border-radius: 4px;"
            " background: #f4f8fb;"
            " color: #132531;"
            "}"
            "QWidget:hover {"
            " background: #e9f2f8;"
            "}"
            "QLabel {"
            " border: 0;"
            " background: transparent;"
            " color: #132531;"
            " font-weight: 600;"
            " padding: 0;"
            "}"
            "QCheckBox {"
            " border: 0;"
            " background: transparent;"
            " color: #132531;"
            " padding: 0;"
            " spacing: 8px;"
            "}"
        ));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setSizeConstraint(QLayout::SetFixedSize);
        cardLayout->setContentsMargins(6, 4, 6, 4);
        cardLayout->setSpacing(3);
        auto* label = new QLabel(item, card);
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        label->setFixedWidth(maxCellWidth - 12);
        auto* cb = new QCheckBox(QString(), card);
        const QString invalidReason = invalidByLower.value(item.trimmed().toLower());
        const bool isInvalid = !invalidReason.isEmpty();
        cb->setChecked(!isInvalid && initialSelected.contains(item, Qt::CaseInsensitive));
        cb->setEnabled(!isInvalid);
        cb->setProperty("itemText", item);
        if (isInvalid) {
            const QString tip = trk(QStringLiteral("t_invalid_dataset_name_001"),
                                    QStringLiteral("Nombre de dataset no válido: %1").arg(invalidReason),
                                    QStringLiteral("Invalid dataset name: %1").arg(invalidReason),
                                    QStringLiteral("无效的数据集名称：%1").arg(invalidReason));
            cb->setToolTip(tip);
            label->setToolTip(tip);
            card->setToolTip(tip);
            card->setStyleSheet(QStringLiteral(
                "QWidget {"
                " border: 1px solid #d97878;"
                " border-radius: 4px;"
                " background: #fff1f1;"
                " color: #6a1d1d;"
                "}"
                "QWidget:hover {"
                " background: #ffe6e6;"
                "}"
                "QLabel {"
                " border: 0;"
                " background: transparent;"
                " color: #8b2020;"
                " font-weight: 600;"
                " padding: 0;"
                "}"
                "QCheckBox {"
                " border: 0;"
                " background: transparent;"
                " color: #8b2020;"
                " padding: 0;"
                " spacing: 8px;"
                "}"
            ));
        }
        auto* toggleFilter = new ToggleCheckEventFilter(cb, card);
        card->installEventFilter(toggleFilter);
        label->installEventFilter(toggleFilter);
        cardLayout->addWidget(label);
        cardLayout->addWidget(cb, 0, Qt::AlignLeft);
        grid->addWidget(card, i, 0);
        checkboxes.push_back(cb);
        cards.push_back(card);
        labels.push_back(label);
    }
    auto relayoutCards = [grid, scroll, cards, labels, maxCellWidth]() {
        if (!grid || !scroll) {
            return;
        }
        const QMargins margins = grid->contentsMargins();
        const int spacing = qMax(0, grid->horizontalSpacing());
        const int viewportWidth = qMax(1, scroll->viewport()->width());
        const int available = qMax(1, viewportWidth - margins.left() - margins.right());
        const int cardWidth = qBound(86, qMin(maxCellWidth, available), maxCellWidth);
        const int columns = qMax(1, (available + spacing) / (cardWidth + spacing));

        for (int i = 0; i < cards.size(); ++i) {
            if (QWidget* card = cards.at(i)) {
                card->setFixedWidth(cardWidth);
            }
            if (QLabel* lbl = labels.at(i)) {
                lbl->setFixedWidth(cardWidth - 12);
            }
            if (QWidget* card = cards.at(i)) {
                grid->addWidget(card, i / columns, i % columns);
            }
        }
        for (int col = 0; col < 32; ++col) {
            grid->setColumnStretch(col, 0);
        }
        for (int row = 0; row < 512; ++row) {
            grid->setRowStretch(row, 0);
        }
        grid->setColumnStretch(columns, 1);
        grid->setRowStretch((cards.size() / columns) + 1, 1);
        grid->invalidate();
    };
    relayoutCards();
    auto* relayoutFilter = new RelayoutOnResizeEventFilter(relayoutCards, &dlg);
    scroll->viewport()->installEventFilter(relayoutFilter);
    scroll->installEventFilter(relayoutFilter);
    root->addWidget(scroll, 1);

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

    QObject::connect(allBtn, &QPushButton::clicked, &dlg, [checkboxes]() {
        for (QCheckBox* cb : checkboxes) {
            if (cb && cb->isEnabled()) {
                cb->setChecked(true);
            }
        }
    });
    QObject::connect(noneBtn, &QPushButton::clicked, &dlg, [checkboxes]() {
        for (QCheckBox* cb : checkboxes) {
            if (cb) {
                cb->setChecked(false);
            }
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
    selected.clear();
    for (QCheckBox* cb : checkboxes) {
        if (cb && cb->isChecked()) {
            selected.push_back(cb->property("itemText").toString());
        }
    }
    return true;
}

bool MainWindow::editInlinePropertiesDialog(const QString& title,
                                            const QString& intro,
                                            const QStringList& items,
                                            QStringList& selected,
                                            QVector<InlinePropGroupConfig>& groups,
                                            const QString& initialGroupName) {
    if (items.isEmpty()) {
        return false;
    }

    auto normalizeProps = [](const QStringList& in) {
        QStringList out;
        QSet<QString> seen;
        for (const QString& raw : in) {
            const QString t = raw.trimmed();
            const QString k = t.toLower();
            if (t.isEmpty() || seen.contains(k)) {
                continue;
            }
            seen.insert(k);
            out.push_back(t);
        }
        return out;
    };
    auto normalizeGroups = [&normalizeProps](const QVector<InlinePropGroupConfig>& in) {
        QVector<InlinePropGroupConfig> out;
        QSet<QString> seenNames;
        for (const InlinePropGroupConfig& raw : in) {
            InlinePropGroupConfig cfg;
            cfg.name = raw.name.trimmed();
            const QString key = cfg.name.toLower();
            if (cfg.name.isEmpty() || seenNames.contains(key)) {
                continue;
            }
            seenNames.insert(key);
            cfg.props = normalizeProps(raw.props);
            out.push_back(cfg);
        }
        return out;
    };

    QStringList workingSelected = normalizeProps(selected);
    QVector<InlinePropGroupConfig> workingGroups = normalizeGroups(groups);

    struct GroupTabState {
        QString name;
        QWidget* page{nullptr};
        QListWidget* available{nullptr};
        QListWidget* shown{nullptr};
    };

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(1120, 760);
    dlg.setWindowTitle(title);
    QVBoxLayout* root = new QVBoxLayout(&dlg);

    QLabel* introLbl = new QLabel(intro, &dlg);
    introLbl->setWordWrap(true);
    root->addWidget(introLbl);

    auto* tabsBar = new QHBoxLayout();
    auto* tabs = new QTabWidget(&dlg);
    QPushButton* newGroupBtn = new QPushButton(trk(QStringLiteral("t_new_group_001"),
                                                   QStringLiteral("Nuevo grupo"),
                                                   QStringLiteral("New group"),
                                                   QStringLiteral("新建分组")),
                                               &dlg);
    QPushButton* renameGroupBtn = new QPushButton(trk(QStringLiteral("t_rename_group_001"),
                                                      QStringLiteral("Renombrar"),
                                                      QStringLiteral("Rename"),
                                                      QStringLiteral("重命名")),
                                                  &dlg);
    QPushButton* deleteGroupBtn = new QPushButton(trk(QStringLiteral("t_delete_group_001"),
                                                      QStringLiteral("Eliminar"),
                                                      QStringLiteral("Delete"),
                                                      QStringLiteral("删除")),
                                                  &dlg);
    tabsBar->addWidget(tabs, 1);
    auto* sideBtns = new QVBoxLayout();
    sideBtns->addWidget(newGroupBtn);
    sideBtns->addWidget(renameGroupBtn);
    sideBtns->addWidget(deleteGroupBtn);
    sideBtns->addStretch(1);
    tabsBar->addLayout(sideBtns);
    root->addLayout(tabsBar, 1);

    auto createReorderList = [&](QWidget* parent) {
        auto* list = new MultiMoveListWidget(parent);
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        list->setDragEnabled(true);
        list->viewport()->setAcceptDrops(true);
        list->setDropIndicatorShown(true);
        list->setDragDropMode(QAbstractItemView::InternalMove);
        list->setDefaultDropAction(Qt::MoveAction);
        return list;
    };
    auto listValues = [](QListWidget* list) {
        QStringList out;
        if (!list) {
            return out;
        }
        for (int i = 0; i < list->count(); ++i) {
            if (QListWidgetItem* item = list->item(i)) {
                out.push_back(item->text().trimmed());
            }
        }
        return out;
    };
    auto setListValues = [&](QListWidget* list, const QStringList& values) {
        if (!list) {
            return;
        }
        list->clear();
        for (const QString& value : values) {
            list->addItem(value);
        }
    };
    auto subtractOrdered = [](const QStringList& base, const QStringList& remove) {
        QStringList out;
        for (const QString& candidate : base) {
            if (!remove.contains(candidate, Qt::CaseInsensitive)) {
                out.push_back(candidate);
            }
        }
        return out;
    };
    auto keepOrderedSubset = [](const QStringList& orderedCandidates, const QStringList& allowed) {
        QStringList out;
        for (const QString& candidate : orderedCandidates) {
            if (allowed.contains(candidate, Qt::CaseInsensitive) && !out.contains(candidate, Qt::CaseInsensitive)) {
                out.push_back(candidate);
            }
        }
        return out;
    };
    std::function<void()> refreshGroupTabs;
    bool refreshingGroupLists = false;
    std::function<void(QListWidget*, QListWidget*)> moveSelected;

    auto createDualListPage = [&](QWidget* parent,
                                  const QString& leftTitle,
                                  const QString& rightTitle,
                                  QListWidget*& availableOut,
                                  QListWidget*& shownOut) {
        auto* page = new QWidget(parent);
        auto* layout = new QHBoxLayout(page);

        auto* leftCol = new QVBoxLayout();
        auto* leftLbl = new QLabel(leftTitle, page);
        leftCol->addWidget(leftLbl);
        availableOut = new QListWidget(page);
        availableOut->setSelectionMode(QAbstractItemView::ExtendedSelection);
        leftCol->addWidget(availableOut, 1);

        auto* centerCol = new QVBoxLayout();
        centerCol->addStretch(1);
        auto* addBtn = new QPushButton(QStringLiteral(">"), page);
        auto* removeBtn = new QPushButton(QStringLiteral("<"), page);
        centerCol->addWidget(addBtn);
        centerCol->addWidget(removeBtn);
        centerCol->addStretch(1);

        auto* rightCol = new QVBoxLayout();
        auto* rightLbl = new QLabel(rightTitle, page);
        rightCol->addWidget(rightLbl);
        shownOut = createReorderList(page);
        rightCol->addWidget(shownOut, 1);

        layout->addLayout(leftCol, 1);
        layout->addLayout(centerCol);
        layout->addLayout(rightCol, 1);

        QObject::connect(addBtn, &QPushButton::clicked, page, [&moveSelected, availableOut, shownOut]() {
            moveSelected(availableOut, shownOut);
        });
        QObject::connect(removeBtn, &QPushButton::clicked, page, [&moveSelected, availableOut, shownOut]() {
            moveSelected(shownOut, availableOut);
        });
        QObject::connect(availableOut, &QListWidget::itemDoubleClicked, page, [&moveSelected, availableOut, shownOut](QListWidgetItem*) {
            moveSelected(availableOut, shownOut);
        });
        QObject::connect(shownOut, &QListWidget::itemDoubleClicked, page, [&moveSelected, availableOut, shownOut](QListWidgetItem*) {
            moveSelected(shownOut, availableOut);
        });
        return page;
    };

    QListWidget* visibleAvailable = nullptr;
    QListWidget* visibleShown = nullptr;
    QWidget* visiblePage = createDualListPage(
        &dlg,
        trk(QStringLiteral("t_available_props_001"),
            QStringLiteral("Disponibles"),
            QStringLiteral("Available"),
            QStringLiteral("可用")),
        trk(QStringLiteral("t_visible_props_title_001"),
            QStringLiteral("Visibles"),
            QStringLiteral("Visible"),
            QStringLiteral("可见")),
        visibleAvailable,
        visibleShown);
    tabs->addTab(visiblePage,
                 trk(QStringLiteral("t_visible_props_tab_001"),
                     QStringLiteral("Todas"),
                     QStringLiteral("All"),
                     QStringLiteral("全部")));

    QVector<GroupTabState> groupTabs;

    auto refreshVisibleLists = [&]() {
        workingSelected = normalizeProps(listValues(visibleShown));
        setListValues(visibleShown, workingSelected);
        setListValues(visibleAvailable, subtractOrdered(items, workingSelected));
    };

    std::function<void(GroupTabState&)> connectGroupTabModels;
    moveSelected = [&](QListWidget* from, QListWidget* to) {
        if (!from || !to) {
            return;
        }
        if (refreshingGroupLists) {
            return;
        }
        refreshingGroupLists = true;
        QStringList movedTexts;
        QList<int> rowsToRemove;
        for (QListWidgetItem* item : from->selectedItems()) {
            if (!item) {
                continue;
            }
            movedTexts.push_back(item->text().trimmed());
            rowsToRemove.push_back(from->row(item));
        }
        std::sort(rowsToRemove.begin(), rowsToRemove.end(), std::greater<int>());
        rowsToRemove.erase(std::unique(rowsToRemove.begin(), rowsToRemove.end()), rowsToRemove.end());
        {
            const QSignalBlocker blockFromModel(from->model());
            const QSignalBlocker blockToModel(to->model());
            const QSignalBlocker blockFrom(from);
            const QSignalBlocker blockTo(to);
            for (int row : rowsToRemove) {
                if (row >= 0 && row < from->count()) {
                    delete from->takeItem(row);
                }
            }
            for (const QString& text : movedTexts) {
                if (!text.isEmpty() && to->findItems(text, Qt::MatchFixedString).isEmpty()) {
                    to->addItem(text);
                }
            }
        }
        refreshingGroupLists = false;
        if (refreshGroupTabs) {
            refreshGroupTabs();
        }
    };
    auto createGroupTab = [&](const QString& groupName, const QStringList& groupProps) {
        GroupTabState state;
        state.name = groupName.trimmed();
        state.page = createDualListPage(
            &dlg,
            trk(QStringLiteral("t_group_available_props_001"),
                QStringLiteral("Todas"),
                QStringLiteral("All"),
                QStringLiteral("全部")),
            trk(QStringLiteral("t_group_visible_props_001"),
                QStringLiteral("Propiedades del grupo"),
                QStringLiteral("Group properties"),
                QStringLiteral("分组属性")),
            state.available,
            state.shown);
        tabs->addTab(state.page, state.name);
        groupTabs.push_back(state);
        refreshingGroupLists = true;
        setListValues(groupTabs.back().shown, normalizeProps(groupProps));
        refreshingGroupLists = false;
        if (auto* reorderList = dynamic_cast<MultiMoveListWidget*>(groupTabs.back().shown)) {
            reorderList->onInternalReorder = [&]() {
                if (refreshGroupTabs) {
                    refreshGroupTabs();
                }
            };
        }
        if (connectGroupTabModels) {
            connectGroupTabModels(groupTabs.back());
        }
    };

    for (const InlinePropGroupConfig& cfg : workingGroups) {
        createGroupTab(cfg.name, cfg.props);
    }

    refreshGroupTabs = [&]() {
        if (refreshingGroupLists) {
            return;
        }
        refreshingGroupLists = true;
        refreshVisibleLists();
        for (GroupTabState& tab : groupTabs) {
            QStringList shown = normalizeProps(listValues(tab.shown));
            shown = keepOrderedSubset(shown, items);
            setListValues(tab.shown, shown);
            setListValues(tab.available, subtractOrdered(items, shown));
        }
        const bool isGroupTab = tabs->currentIndex() > 0;
        renameGroupBtn->setEnabled(isGroupTab);
        deleteGroupBtn->setEnabled(isGroupTab);
        refreshingGroupLists = false;
    };

    setListValues(visibleShown, workingSelected);
    setListValues(visibleAvailable, subtractOrdered(items, workingSelected));
    if (auto* reorderList = dynamic_cast<MultiMoveListWidget*>(visibleShown)) {
        reorderList->onInternalReorder = [&]() {
            if (refreshGroupTabs) {
                refreshGroupTabs();
            }
        };
    }
    refreshGroupTabs();

    QObject::connect(visibleShown->model(), &QAbstractItemModel::rowsMoved, &dlg, [&](const QModelIndex&, int, int, const QModelIndex&, int) {
        refreshGroupTabs();
    });
    QObject::connect(visibleShown->model(), &QAbstractItemModel::rowsInserted, &dlg, [&](const QModelIndex&, int, int) {
        refreshGroupTabs();
    });
    QObject::connect(visibleShown->model(), &QAbstractItemModel::rowsRemoved, &dlg, [&](const QModelIndex&, int, int) {
        refreshGroupTabs();
    });
    QObject::connect(visibleAvailable->model(), &QAbstractItemModel::rowsInserted, &dlg, [&](const QModelIndex&, int, int) {
        refreshGroupTabs();
    });
    QObject::connect(visibleAvailable->model(), &QAbstractItemModel::rowsRemoved, &dlg, [&](const QModelIndex&, int, int) {
        refreshGroupTabs();
    });
    QObject::connect(tabs, &QTabWidget::currentChanged, &dlg, [&](int) {
        refreshGroupTabs();
    });

    QObject::connect(newGroupBtn, &QPushButton::clicked, &dlg, [&]() {
        bool ok = false;
        const QString name = QInputDialog::getText(&dlg,
                                                   trk(QStringLiteral("t_new_group_001"),
                                                       QStringLiteral("Nuevo grupo"),
                                                       QStringLiteral("New group"),
                                                       QStringLiteral("新建分组")),
                                                   trk(QStringLiteral("t_group_name_001"),
                                                       QStringLiteral("Nombre del grupo"),
                                                       QStringLiteral("Group name"),
                                                       QStringLiteral("分组名称")),
                                                   QLineEdit::Normal,
                                                   QString(),
                                                   &ok)
                                 .trimmed();
        if (!ok || name.isEmpty()) {
            return;
        }
        for (const GroupTabState& tab : groupTabs) {
            if (tab.name.compare(name, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(&dlg,
                                     QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_group_exists_001"),
                                         QStringLiteral("Ya existe un grupo con ese nombre."),
                                         QStringLiteral("A group with that name already exists."),
                                         QStringLiteral("该名称的分组已存在。")));
                return;
            }
        }
        createGroupTab(name, {});
        tabs->setCurrentIndex(tabs->count() - 1);
        refreshGroupTabs();
    });

    QObject::connect(renameGroupBtn, &QPushButton::clicked, &dlg, [&]() {
        const int idx = tabs->currentIndex() - 1;
        if (idx < 0 || idx >= groupTabs.size()) {
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getText(&dlg,
                                                   trk(QStringLiteral("t_rename_group_001"),
                                                       QStringLiteral("Renombrar grupo"),
                                                       QStringLiteral("Rename group"),
                                                       QStringLiteral("重命名分组")),
                                                   trk(QStringLiteral("t_group_name_001"),
                                                       QStringLiteral("Nombre del grupo"),
                                                       QStringLiteral("Group name"),
                                                       QStringLiteral("分组名称")),
                                                   QLineEdit::Normal,
                                                   groupTabs[idx].name,
                                                   &ok)
                                 .trimmed();
        if (!ok || name.isEmpty()) {
            return;
        }
        for (int i = 0; i < groupTabs.size(); ++i) {
            if (i != idx && groupTabs[i].name.compare(name, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(&dlg,
                                     QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_group_exists_001"),
                                         QStringLiteral("Ya existe un grupo con ese nombre."),
                                         QStringLiteral("A group with that name already exists."),
                                         QStringLiteral("该名称的分组已存在。")));
                return;
            }
        }
        groupTabs[idx].name = name;
        tabs->setTabText(idx + 1, name);
    });

    QObject::connect(deleteGroupBtn, &QPushButton::clicked, &dlg, [&]() {
        const int idx = tabs->currentIndex() - 1;
        if (idx < 0 || idx >= groupTabs.size()) {
            return;
        }
        QWidget* page = groupTabs[idx].page;
        groupTabs.removeAt(idx);
        tabs->removeTab(idx + 1);
        if (page) {
            page->deleteLater();
        }
        if (tabs->count() > 0) {
            tabs->setCurrentIndex(qMin(idx + 1, tabs->count() - 1));
        }
        refreshGroupTabs();
    });

    connectGroupTabModels = [&](GroupTabState& tab) {
        QObject::connect(tab.shown->model(), &QAbstractItemModel::rowsMoved, &dlg, [&](const QModelIndex&, int, int, const QModelIndex&, int) {
            refreshGroupTabs();
        });
        QObject::connect(tab.shown->model(), &QAbstractItemModel::rowsInserted, &dlg, [&](const QModelIndex&, int, int) {
            refreshGroupTabs();
        });
        QObject::connect(tab.shown->model(), &QAbstractItemModel::rowsRemoved, &dlg, [&](const QModelIndex&, int, int) {
            refreshGroupTabs();
        });
        QObject::connect(tab.available->model(), &QAbstractItemModel::rowsInserted, &dlg, [&](const QModelIndex&, int, int) {
            refreshGroupTabs();
        });
        QObject::connect(tab.available->model(), &QAbstractItemModel::rowsRemoved, &dlg, [&](const QModelIndex&, int, int) {
            refreshGroupTabs();
        });
    };
    for (GroupTabState& tab : groupTabs) {
        connectGroupTabModels(tab);
    }

    if (!initialGroupName.trimmed().isEmpty()) {
        for (int i = 0; i < groupTabs.size(); ++i) {
            if (groupTabs[i].name.compare(initialGroupName.trimmed(), Qt::CaseInsensitive) == 0) {
                tabs->setCurrentIndex(i + 1);
                break;
            }
        }
    }

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

    refreshGroupTabs();
    selected = normalizeProps(listValues(visibleShown));
    QVector<InlinePropGroupConfig> outGroups;
    for (const GroupTabState& tab : groupTabs) {
        InlinePropGroupConfig cfg;
        cfg.name = tab.name.trimmed();
        cfg.props = normalizeProps(listValues(tab.shown));
        if (!cfg.name.isEmpty() && !cfg.props.isEmpty()) {
            outGroups.push_back(cfg);
        }
    }
    groups = outGroups;
    return true;
}
