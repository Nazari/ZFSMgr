#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

namespace {
using mwhelpers::shSingleQuote;

void setRequiredLabelState(QLabel* label, bool required) {
    if (!label) {
        return;
    }
    label->setStyleSheet(required
                             ? QStringLiteral("QLabel { color: #b00020; font-weight: 600; }")
                             : QString());
}

void bindRequiredLineEditLabel(QLineEdit* edit, QLabel* label, const std::function<bool()>& requirementActive) {
    if (!edit || !label) {
        return;
    }
    auto refresh = [edit, label, requirementActive]() {
        setRequiredLabelState(label, requirementActive() && edit->text().trimmed().isEmpty());
    };
    QObject::connect(edit, &QLineEdit::textChanged, label, [refresh](const QString&) {
        refresh();
    });
    refresh();
}

struct CreateDatasetOptions {
    QString datasetPath;
    QString dsType;
    QString volsize;
    QString blocksize;
    bool parents{true};
    bool sparse{false};
    bool nomount{false};
    bool snapshotRecursive{false};
    QStringList properties;
    QString extraArgs;
    QString encryptionPassphrase;
};

QString buildZfsCreateCmd(const CreateDatasetOptions& opt) {
    const QString dsType = opt.dsType.trimmed().toLower();
    if (dsType == QStringLiteral("snapshot")) {
        QStringList parts;
        parts << QStringLiteral("zfs") << QStringLiteral("snapshot");
        if (opt.snapshotRecursive) {
            parts << QStringLiteral("-r");
        }
        for (const QString& p : opt.properties) {
            const QString pp = p.trimmed();
            if (!pp.isEmpty()) {
                parts << QStringLiteral("-o") << shSingleQuote(pp);
            }
        }
        if (!opt.extraArgs.trimmed().isEmpty()) {
            parts << opt.extraArgs.trimmed();
        }
        parts << shSingleQuote(opt.datasetPath.trimmed());
        return parts.join(' ');
    }

    QStringList parts;
    parts << QStringLiteral("zfs") << QStringLiteral("create");
    if (opt.parents) {
        parts << QStringLiteral("-p");
    }
    if (opt.sparse) {
        parts << QStringLiteral("-s");
    }
    if (opt.nomount) {
        parts << QStringLiteral("-u");
    }
    if (!opt.blocksize.trimmed().isEmpty()) {
        parts << QStringLiteral("-b") << shSingleQuote(opt.blocksize.trimmed());
    }
    if (dsType == QStringLiteral("volume") && !opt.volsize.trimmed().isEmpty()) {
        parts << QStringLiteral("-V") << shSingleQuote(opt.volsize.trimmed());
    }
    for (const QString& p : opt.properties) {
        const QString pp = p.trimmed();
        if (!pp.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(pp);
        }
    }
    if (!opt.extraArgs.trimmed().isEmpty()) {
        parts << opt.extraArgs.trimmed();
    }
    parts << shSingleQuote(opt.datasetPath.trimmed());
    return parts.join(' ');
}
} // namespace

void MainWindow::actionCreateChildDataset(const QString& side) {
    actionCreateChildDataset(side, currentDatasetSelection(side));
}

void MainWindow::actionCreateChildDataset(const QString& side, const DatasetSelectionContext& explicitCtx) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = explicitCtx.valid ? explicitCtx : currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    struct PropSpec {
        QString name;
        QString kind;
        QStringList values;
    };
    struct PropEditor {
        QString name;
        QLineEdit* edit{nullptr};
        QComboBox* combo{nullptr};
    };

    QDialog dlg(this);
    dlg.setWindowTitle(trk(QStringLiteral("t_create_ds_001"),
                           QStringLiteral("Crear dataset"),
                           QStringLiteral("Create dataset"),
                           QStringLiteral("创建数据集")));
    dlg.setModal(true);
    dlg.setFont(QApplication::font());
    dlg.resize(700, 660);
    dlg.setMinimumSize(620, 560);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QWidget* formWidget = new QWidget(&dlg);
    formWidget->setFont(QApplication::font());
    QGridLayout* form = new QGridLayout(formWidget);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    int row = 0;

    QLabel* pathLabel = new QLabel(trk(QStringLiteral("t_path_lbl_001"),
                                       QStringLiteral("Path"),
                                       QStringLiteral("Path"),
                                       QStringLiteral("路径")),
                                   formWidget);
    QLineEdit* pathEdit = new QLineEdit(formWidget);
    pathEdit->setText(ctx.datasetName + QStringLiteral("/new_dataset"));
    form->addWidget(pathLabel, row, 0);
    form->addWidget(pathEdit, row, 1, 1, 3);
    row++;

    QLabel* typeLabel = new QLabel(trk(QStringLiteral("t_tipo_6cc619"),
                                       QStringLiteral("Tipo"),
                                       QStringLiteral("Type"),
                                       QStringLiteral("类型")),
                                   formWidget);
    QComboBox* typeCombo = new QComboBox(formWidget);
    typeCombo->addItem(QStringLiteral("filesystem"), QStringLiteral("filesystem"));
    typeCombo->addItem(QStringLiteral("volume"), QStringLiteral("volume"));
    typeCombo->addItem(QStringLiteral("snapshot"), QStringLiteral("snapshot"));
    form->addWidget(typeLabel, row, 0);
    form->addWidget(typeCombo, row, 1);
    row++;

    QLabel* volsizeLabel = new QLabel(trk(QStringLiteral("t_volsize_lbl001"),
                                          QStringLiteral("Volsize"),
                                          QStringLiteral("Volsize"),
                                          QStringLiteral("卷大小")),
                                      formWidget);
    QLineEdit* volsizeEdit = new QLineEdit(formWidget);
    form->addWidget(volsizeLabel, row, 0);
    form->addWidget(volsizeEdit, row, 1);
    row++;

    QLabel* blocksizeLabel = new QLabel(trk(QStringLiteral("t_blocksize001"),
                                            QStringLiteral("Blocksize"),
                                            QStringLiteral("Blocksize"),
                                            QStringLiteral("块大小")),
                                        formWidget);
    QLineEdit* blocksizeEdit = new QLineEdit(formWidget);
    form->addWidget(blocksizeLabel, row, 0);
    form->addWidget(blocksizeEdit, row, 1);
    row++;

    QWidget* optsWidget = new QWidget(formWidget);
    QHBoxLayout* optsLay = new QHBoxLayout(optsWidget);
    optsLay->setContentsMargins(0, 0, 0, 0);
    optsLay->setSpacing(12);
    QCheckBox* parentsChk = new QCheckBox(trk(QStringLiteral("t_create_parents01"),
                                              QStringLiteral("Crear padres (-p)"),
                                              QStringLiteral("Create parents (-p)"),
                                              QStringLiteral("创建父级(-p)")),
                                          optsWidget);
    QCheckBox* sparseChk = new QCheckBox(trk(QStringLiteral("t_sparse_opt_001"),
                                             QStringLiteral("Sparse (-s)"),
                                             QStringLiteral("Sparse (-s)"),
                                             QStringLiteral("稀疏(-s)")),
                                         optsWidget);
    QCheckBox* nomountChk = new QCheckBox(trk(QStringLiteral("t_nomount_opt001"),
                                              QStringLiteral("No montar (-u)"),
                                              QStringLiteral("Do not mount (-u)"),
                                              QStringLiteral("不挂载(-u)")),
                                          optsWidget);
    parentsChk->setChecked(true);
    optsLay->addWidget(parentsChk);
    optsLay->addWidget(sparseChk);
    optsLay->addWidget(nomountChk);
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QCheckBox* snapRecursiveChk = new QCheckBox(
        trk(QStringLiteral("t_snap_rec_opt01"),
            QStringLiteral("Snapshot recursivo (-r)"),
            QStringLiteral("Recursive snapshot (-r)"),
            QStringLiteral("递归快照(-r)")),
        formWidget);
    form->addWidget(snapRecursiveChk, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(trk(QStringLiteral("t_extra_args_001"),
                                        QStringLiteral("Argumentos extra"),
                                        QStringLiteral("Extra args"),
                                        QStringLiteral("额外参数")),
                                    formWidget);
    QLineEdit* extraEdit = new QLineEdit(formWidget);
    form->addWidget(extraLabel, row, 0);
    form->addWidget(extraEdit, row, 1, 1, 3);
    row++;

    QLabel* encPassLabel = new QLabel(trk(QStringLiteral("t_create_ds_encpass_001"),
                                          QStringLiteral("Passphrase cifrado"),
                                          QStringLiteral("Encryption passphrase"),
                                          QStringLiteral("加密口令")),
                                      formWidget);
    QLineEdit* encPassEdit = new QLineEdit(formWidget);
    encPassEdit->setEchoMode(QLineEdit::Password);
    form->addWidget(encPassLabel, row, 0);
    form->addWidget(encPassEdit, row, 1, 1, 3);
    row++;

    QLabel* encPass2Label = new QLabel(trk(QStringLiteral("t_create_ds_encpass_002"),
                                           QStringLiteral("Repetir passphrase"),
                                           QStringLiteral("Repeat passphrase"),
                                           QStringLiteral("重复口令")),
                                       formWidget);
    QLineEdit* encPass2Edit = new QLineEdit(formWidget);
    encPass2Edit->setEchoMode(QLineEdit::Password);
    form->addWidget(encPass2Label, row, 0);
    form->addWidget(encPass2Edit, row, 1, 1, 3);
    row++;

    root->addWidget(formWidget);

    QGroupBox* propsGroup = new QGroupBox(trk(QStringLiteral("t_props_tab_001"),
                                              QStringLiteral("Propiedades"),
                                              QStringLiteral("Properties"),
                                              QStringLiteral("属性")),
                                          &dlg);
    propsGroup->setFont(QApplication::font());
    QVBoxLayout* propsGroupLay = new QVBoxLayout(propsGroup);
    propsGroupLay->setContentsMargins(6, 6, 6, 6);
    propsGroupLay->setSpacing(4);

    QScrollArea* propsScroll = new QScrollArea(propsGroup);
    propsScroll->setFont(QApplication::font());
    propsScroll->setWidgetResizable(true);
    QWidget* propsContainer = new QWidget(propsScroll);
    propsContainer->setFont(QApplication::font());
    QGridLayout* propsGrid = new QGridLayout(propsContainer);
    propsGrid->setHorizontalSpacing(8);
    propsGrid->setVerticalSpacing(4);

    const QList<PropSpec> propSpecs = {
        {QStringLiteral("mountpoint"), QStringLiteral("entry"), {}},
        {QStringLiteral("canmount"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("noauto")}},
        {QStringLiteral("compression"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("lz4"), QStringLiteral("gzip"), QStringLiteral("zstd"), QStringLiteral("zle")}},
        {QStringLiteral("atime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("xattr"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa")}},
        {QStringLiteral("acltype"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), QStringLiteral("combo"), {QString(), QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("recordsize"), QStringLiteral("entry"), {}},
        {QStringLiteral("volblocksize"), QStringLiteral("entry"), {}},
        {QStringLiteral("quota"), QStringLiteral("entry"), {}},
        {QStringLiteral("reservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("refquota"), QStringLiteral("entry"), {}},
        {QStringLiteral("refreservation"), QStringLiteral("entry"), {}},
        {QStringLiteral("copies"), QStringLiteral("combo"), {QString(), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}},
        {QStringLiteral("checksum"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("fletcher2"), QStringLiteral("fletcher4"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr")}},
        {QStringLiteral("sync"), QStringLiteral("combo"), {QString(), QStringLiteral("standard"), QStringLiteral("always"), QStringLiteral("disabled")}},
        {QStringLiteral("logbias"), QStringLiteral("combo"), {QString(), QStringLiteral("latency"), QStringLiteral("throughput")}},
        {QStringLiteral("primarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("secondarycache"), QStringLiteral("combo"), {QString(), QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("dedup"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("verify"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein")}},
        {QStringLiteral("encryption"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("aes-128-ccm"), QStringLiteral("aes-192-ccm"), QStringLiteral("aes-256-ccm"), QStringLiteral("aes-128-gcm"), QStringLiteral("aes-192-gcm"), QStringLiteral("aes-256-gcm")}},
        {QStringLiteral("keyformat"), QStringLiteral("combo"), {QString(), QStringLiteral("passphrase"), QStringLiteral("raw"), QStringLiteral("hex")}},
        {QStringLiteral("keylocation"), QStringLiteral("entry"), {}},
        {QStringLiteral("normalization"), QStringLiteral("combo"), {QString(), QStringLiteral("none"), QStringLiteral("formC"), QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")}},
        {QStringLiteral("casesensitivity"), QStringLiteral("combo"), {QString(), QStringLiteral("sensitive"), QStringLiteral("insensitive"), QStringLiteral("mixed")}},
        {QStringLiteral("utf8only"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
    };

    QList<PropEditor> propEditors;
    propEditors.reserve(propSpecs.size());
    for (int i = 0; i < propSpecs.size(); ++i) {
        const PropSpec& spec = propSpecs[i];
        const int r = i / 2;
        const int cBase = (i % 2) * 2;
        QLabel* lbl = new QLabel(spec.name, propsContainer);
        propsGrid->addWidget(lbl, r, cBase);
        PropEditor editor;
        editor.name = spec.name;
        if (spec.kind == QStringLiteral("combo")) {
            QComboBox* cb = new QComboBox(propsContainer);
            cb->addItems(spec.values);
            if (spec.name == QStringLiteral("compression")) {
                cb->setEditable(true);
                cb->setInsertPolicy(QComboBox::NoInsert);
            }
            editor.combo = cb;
            propsGrid->addWidget(cb, r, cBase + 1);
        } else {
            QLineEdit* le = new QLineEdit(propsContainer);
            editor.edit = le;
            propsGrid->addWidget(le, r, cBase + 1);
        }
        propEditors.push_back(editor);
    }

    auto propValue = [&](const QString& name) -> QString {
        for (const PropEditor& pe : propEditors) {
            if (pe.name.compare(name, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (pe.combo) {
                return pe.combo->currentText().trimmed();
            }
            if (pe.edit) {
                return pe.edit->text().trimmed();
            }
            return QString();
        }
        return QString();
    };
    auto updateEncryptionPassphraseUi = [&]() {
        const QString enc = propValue(QStringLiteral("encryption")).toLower();
        const QString keyformat = propValue(QStringLiteral("keyformat")).toLower();
        const QString keylocation = propValue(QStringLiteral("keylocation")).trimmed().toLower();
        const bool needsPromptPassphrase =
            (enc == QStringLiteral("on") || enc.startsWith(QStringLiteral("aes-")))
            && keyformat == QStringLiteral("passphrase")
            && keylocation == QStringLiteral("prompt");
        encPassLabel->setVisible(needsPromptPassphrase);
        encPassEdit->setVisible(needsPromptPassphrase);
        encPass2Label->setVisible(needsPromptPassphrase);
        encPass2Edit->setVisible(needsPromptPassphrase);
        setRequiredLabelState(encPassLabel, needsPromptPassphrase);
        setRequiredLabelState(encPass2Label, needsPromptPassphrase);
        if (!needsPromptPassphrase) {
            encPassEdit->clear();
            encPass2Edit->clear();
        }
    };
    for (const PropEditor& pe : propEditors) {
        if (pe.name.compare(QStringLiteral("encryption"), Qt::CaseInsensitive) == 0 && pe.combo) {
            QObject::connect(pe.combo, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, updateEncryptionPassphraseUi);
        } else if (pe.name.compare(QStringLiteral("keyformat"), Qt::CaseInsensitive) == 0 && pe.combo) {
            QObject::connect(pe.combo, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, updateEncryptionPassphraseUi);
        } else if (pe.name.compare(QStringLiteral("keylocation"), Qt::CaseInsensitive) == 0 && pe.edit) {
            QObject::connect(pe.edit, &QLineEdit::textChanged, &dlg, updateEncryptionPassphraseUi);
        }
    }
    updateEncryptionPassphraseUi();
    propsContainer->setLayout(propsGrid);
    propsScroll->setWidget(propsContainer);
    propsGroupLay->addWidget(propsScroll);
    root->addWidget(propsGroup, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(trk(QStringLiteral("t_cancelar_c111e0"),
                                                    QStringLiteral("Cancelar"),
                                                    QStringLiteral("Cancel"),
                                                    QStringLiteral("取消")),
                                                QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        trk(QStringLiteral("t_create_btn_001"),
            QStringLiteral("Crear"),
            QStringLiteral("Create"),
            QStringLiteral("创建")),
        QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    const QFont baseFont = QApplication::font();
    const QList<QWidget*> allWidgets = dlg.findChildren<QWidget*>();
    for (QWidget* w : allWidgets) {
        if (w) {
            w->setFont(baseFont);
        }
    }
    setRequiredLabelState(pathLabel, true);
    setRequiredLabelState(volsizeLabel, false);
    setRequiredLabelState(encPassLabel, false);
    setRequiredLabelState(encPass2Label, false);
    bindRequiredLineEditLabel(pathEdit, pathLabel, []() { return true; });
    bindRequiredLineEditLabel(volsizeEdit, volsizeLabel, [typeCombo]() {
        return typeCombo && typeCombo->currentData().toString() == QStringLiteral("volume");
    });
    bindRequiredLineEditLabel(encPassEdit, encPassLabel, [&, typeCombo]() {
        Q_UNUSED(typeCombo);
        const QString enc = propValue(QStringLiteral("encryption")).toLower();
        const QString keyformat = propValue(QStringLiteral("keyformat")).toLower();
        const QString keylocation = propValue(QStringLiteral("keylocation")).trimmed().toLower();
        return (enc == QStringLiteral("on") || enc.startsWith(QStringLiteral("aes-")))
               && keyformat == QStringLiteral("passphrase")
               && keylocation == QStringLiteral("prompt");
    });
    bindRequiredLineEditLabel(encPass2Edit, encPass2Label, [&, typeCombo]() {
        Q_UNUSED(typeCombo);
        const QString enc = propValue(QStringLiteral("encryption")).toLower();
        const QString keyformat = propValue(QStringLiteral("keyformat")).toLower();
        const QString keylocation = propValue(QStringLiteral("keylocation")).trimmed().toLower();
        return (enc == QStringLiteral("on") || enc.startsWith(QStringLiteral("aes-")))
               && keyformat == QStringLiteral("passphrase")
               && keylocation == QStringLiteral("prompt");
    });

    auto setSuggestedPath = [&]() {
        const QString t = typeCombo->currentData().toString();
        const bool isSnapshot = t == QStringLiteral("snapshot");
        QString suggested = QStringLiteral("new_dataset");
        if (t == QStringLiteral("volume")) {
            suggested = QStringLiteral("new_volume");
        }

        if (isSnapshot) {
            const QString snapPath = ctx.datasetName + QStringLiteral("@snap");
            pathEdit->setText(snapPath);
            pathEdit->setFocus();
            const int snapPos = snapPath.indexOf('@');
            pathEdit->setSelection(snapPos >= 0 ? snapPos + 1 : snapPath.size(), QStringLiteral("snap").size());
            return;
        }

        QString current = pathEdit->text().trimmed();
        QString noSnap = current;
        const int atPos = noSnap.indexOf('@');
        if (atPos >= 0) {
            noSnap = noSnap.left(atPos);
        }
        QString prefix;
        const int slash = noSnap.lastIndexOf('/');
        if (slash >= 0) {
            prefix = noSnap.left(slash + 1);
        } else if (!ctx.datasetName.isEmpty()) {
            prefix = ctx.datasetName + QStringLiteral("/");
        }

        const QString newPath = prefix + suggested;
        pathEdit->setText(newPath);
        pathEdit->setFocus();
        pathEdit->setSelection(prefix.size(), suggested.size());
    };

    auto applyTypeUi = [&]() {
        const QString t = typeCombo->currentData().toString();
        const bool isVolume = t == QStringLiteral("volume");
        const bool isSnapshot = t == QStringLiteral("snapshot");

        volsizeLabel->setVisible(isVolume);
        volsizeEdit->setVisible(isVolume);
        setRequiredLabelState(volsizeLabel, isVolume);
        if (!isVolume) {
            volsizeEdit->clear();
        }

        blocksizeLabel->setVisible(!isSnapshot);
        blocksizeEdit->setVisible(!isSnapshot);
        if (isSnapshot) {
            blocksizeEdit->clear();
            parentsChk->setChecked(false);
            sparseChk->setChecked(false);
            nomountChk->setChecked(false);
        }

        parentsChk->setVisible(!isSnapshot);
        sparseChk->setVisible(!isSnapshot);
        nomountChk->setVisible(!isSnapshot);
        snapRecursiveChk->setVisible(isSnapshot);
        if (!isSnapshot) {
            snapRecursiveChk->setChecked(false);
        }
        setSuggestedPath();
        setRequiredLabelState(pathLabel, true);
    };
    QObject::connect(typeCombo, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&]() { applyTypeUi(); });
    applyTypeUi();

    QObject::connect(createBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString path = pathEdit->text().trimmed();
        const QString dsType = typeCombo->currentData().toString().trimmed().toLower();
        const QString volsize = volsizeEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_need_ds_path001"),
                                     QStringLiteral("Debe indicar el path del dataset."),
                                     QStringLiteral("Dataset path is required."),
                                     QStringLiteral("必须指定数据集路径。")));
            return;
        }
        if (dsType == QStringLiteral("snapshot") && !path.contains('@')) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_snap_need_at01"),
                                     QStringLiteral("Para snapshot, el path debe incluir '@'."),
                                     QStringLiteral("For snapshot, path must include '@'."),
                                     QStringLiteral("快照路径必须包含'@'。")));
            return;
        }
        if (dsType == QStringLiteral("volume") && volsize.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_vol_need_size01"),
                                     QStringLiteral("Para volume, Volsize es obligatorio."),
                                     QStringLiteral("For volume, Volsize is required."),
                                     QStringLiteral("卷类型必须填写 Volsize。")));
            return;
        }

        QStringList properties;
        for (const PropEditor& pe : propEditors) {
            QString v;
            if (pe.combo) {
                v = pe.combo->currentText().trimmed();
            } else if (pe.edit) {
                v = pe.edit->text().trimmed();
            }
            if (!v.isEmpty()) {
                properties.push_back(pe.name + QStringLiteral("=") + v);
            }
        }

        const QString enc = propValue(QStringLiteral("encryption")).toLower();
        const QString keyformat = propValue(QStringLiteral("keyformat")).toLower();
        const QString keylocation = propValue(QStringLiteral("keylocation")).trimmed().toLower();
        const bool needsPromptPassphrase =
            (enc == QStringLiteral("on") || enc.startsWith(QStringLiteral("aes-")))
            && keyformat == QStringLiteral("passphrase")
            && keylocation == QStringLiteral("prompt");
        if (needsPromptPassphrase) {
            if (encPassEdit->text().isEmpty() || encPass2Edit->text().isEmpty()) {
                QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_create_ds_encpass_req_001"),
                                         QStringLiteral("Debe indicar y repetir la passphrase de cifrado."),
                                         QStringLiteral("You must enter and repeat the encryption passphrase."),
                                         QStringLiteral("必须输入并重复加密口令。")));
                return;
            }
            if (encPassEdit->text() != encPass2Edit->text()) {
                QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_create_ds_encpass_req_002"),
                                         QStringLiteral("Las passphrases de cifrado no coinciden."),
                                         QStringLiteral("Encryption passphrases do not match."),
                                         QStringLiteral("加密口令不匹配。")));
                return;
            }
        }

        CreateDatasetOptions opt;
        opt.datasetPath = path;
        opt.dsType = dsType;
        opt.volsize = volsize;
        opt.blocksize = blocksizeEdit->text().trimmed();
        opt.parents = parentsChk->isChecked();
        opt.sparse = sparseChk->isChecked();
        opt.nomount = nomountChk->isChecked();
        opt.snapshotRecursive = snapRecursiveChk->isChecked();
        opt.properties = properties;
        opt.extraArgs = extraEdit->text().trimmed();
        opt.encryptionPassphrase = needsPromptPassphrase ? encPassEdit->text() : QString();

        const QString actionLabel = (opt.dsType == QStringLiteral("snapshot"))
                                        ? trk(QStringLiteral("t_create_snap001"),
                                              QStringLiteral("Crear snapshot"),
                                              QStringLiteral("Create snapshot"),
                                              QStringLiteral("创建快照"))
                                        : trk(QStringLiteral("t_create_ds_001"),
                                              QStringLiteral("Crear dataset"),
                                              QStringLiteral("Create dataset"),
                                              QStringLiteral("创建数据集"));
        const QString cmd = buildZfsCreateCmd(opt);
        QByteArray stdinPayload;
        if (!opt.encryptionPassphrase.isEmpty()) {
            stdinPayload = opt.encryptionPassphrase.toUtf8();
            stdinPayload += '\n';
            stdinPayload += opt.encryptionPassphrase.toUtf8();
            stdinPayload += '\n';
        }
        const auto refreshAfterCreate = [this, side, ctx]() {
            invalidatePoolDatasetListingCache(ctx.connIdx, ctx.poolName);
            if (side == QStringLiteral("conncontent")) {
                reloadConnContentPool(ctx.connIdx, ctx.poolName);
            } else {
                reloadDatasetSide(side);
            }
        };
        if (executeDatasetAction(side,
                                 actionLabel,
                                 ctx,
                                 cmd,
                                 45000,
                                 false,
                                 stdinPayload,
                                 false,
                                 refreshAfterCreate)) {
            dlg.accept();
        }
    });

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
}

void MainWindow::actionDeleteDatasetOrSnapshot(const QString& side) {
    actionDeleteDatasetOrSnapshot(side, currentDatasetSelection(side));
}

void MainWindow::actionDeleteDatasetOrSnapshot(const QString& side, const DatasetSelectionContext& explicitCtx) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = explicitCtx.valid ? explicitCtx : currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }
    const QString target = ctx.snapshotName.isEmpty() ? ctx.datasetName : (ctx.datasetName + QStringLiteral("@") + ctx.snapshotName);
    const auto askRec = QMessageBox::question(
        this,
        trk(QStringLiteral("t_recursive_del01"),
            QStringLiteral("Borrado recursivo"),
            QStringLiteral("Recursive deletion"),
            QStringLiteral("递归删除")),
        ctx.snapshotName.isEmpty()
            ? trk(QStringLiteral("t_recursive_q_ds1"),
                  QStringLiteral("¿Borrar recursivamente datasets/snapshots hijos?"),
                  QStringLiteral("Delete child datasets/snapshots recursively?"),
                  QStringLiteral("是否递归删除子数据集/快照？"))
            : trk(QStringLiteral("t_recursive_q_sn1"),
                  QStringLiteral("¿Borrar recursivamente este snapshot en hijos descendientes?"),
                  QStringLiteral("Delete this snapshot recursively on descendant children?"),
                  QStringLiteral("是否在后代子项上递归删除此快照？")),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    const bool recursive = (askRec == QMessageBox::Yes);
    const auto confirm = QMessageBox::question(
        this,
        trk(QStringLiteral("t_confirm_del_001"),
            QStringLiteral("Confirmar borrado"),
            QStringLiteral("Confirm deletion"),
            QStringLiteral("确认删除")),
        trk(QStringLiteral("t_confirm_del_msg1"),
            QStringLiteral("Se añadirá a cambios pendientes el borrado%2 de:\n%1\n¿Continuar?"),
            QStringLiteral("A%2 delete for the following will be added to pending changes:\n%1\nContinue?"),
            QStringLiteral("将把以下对象的%2删除加入待处理更改：\n%1\n是否继续？"))
            .arg(target,
                 recursive
                     ? trk(QStringLiteral("t_recursive_suffix_001"),
                           QStringLiteral(" recursivo"),
                           QStringLiteral(" recursive"),
                           QStringLiteral(" 递归"))
                     : QString()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    QString cmd;
    cmd = recursive ? QStringLiteral("zfs destroy -r %1").arg(shSingleQuote(target))
                    : QStringLiteral("zfs destroy %1").arg(shSingleQuote(target));
    ConnectionProfile cp = m_profiles[ctx.connIdx];
    if (isLocalConnection(cp) && !isWindowsConnection(cp)) {
        cp.useSudo = true;
        if (!ensureLocalSudoCredentials(cp)) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Borrar cancelado: faltan credenciales sudo locales"));
            return;
        }
    }
    const QString fullCmd = sshExecFromLocal(cp, withSudo(cp, cmd));
    auto connPoolLabel = [this](const DatasetSelectionContext& selCtx) {
        if (!selCtx.valid || selCtx.connIdx < 0 || selCtx.connIdx >= m_profiles.size() || selCtx.poolName.trimmed().isEmpty()) {
            return QString();
        }
        const ConnectionProfile& p = m_profiles.at(selCtx.connIdx);
        const QString connLabel = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
        return QStringLiteral("%1::%2").arg(connLabel, selCtx.poolName.trimmed());
    };
    QString errorText;
    if (!queuePendingShellAction(PendingShellActionDraft{
            connPoolLabel(ctx),
            recursive
                ? QStringLiteral("Borrar recursivamente %1").arg(target)
                : QStringLiteral("Borrar %1").arg(target),
            fullCmd,
            90000,
            false,
            {},
            ctx,
            PendingShellActionDraft::RefreshScope::TargetOnly},
            &errorText)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Cambio pendiente añadido: %1  %2")
               .arg(connPoolLabel(ctx),
                    recursive
                        ? QStringLiteral("Borrar recursivamente %1").arg(target)
                        : QStringLiteral("Borrar %1").arg(target)));
    updateApplyPropsButtonState();
}
