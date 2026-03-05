#include "mainwindow.h"
#include "mainwindow_helpers.h"

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

namespace {
using mwhelpers::shSingleQuote;

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
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
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
    dlg.setWindowTitle(tr3(QStringLiteral("Crear dataset"), QStringLiteral("Create dataset"), QStringLiteral("创建数据集")));
    dlg.setModal(true);
    dlg.resize(900, 760);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QWidget* formWidget = new QWidget(&dlg);
    QGridLayout* form = new QGridLayout(formWidget);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    int row = 0;

    QLabel* pathLabel = new QLabel(tr3(QStringLiteral("Path"), QStringLiteral("Path"), QStringLiteral("路径")), formWidget);
    QLineEdit* pathEdit = new QLineEdit(formWidget);
    pathEdit->setText(ctx.datasetName + QStringLiteral("/new_dataset"));
    form->addWidget(pathLabel, row, 0);
    form->addWidget(pathEdit, row, 1, 1, 3);
    row++;

    QLabel* typeLabel = new QLabel(tr3(QStringLiteral("Tipo"), QStringLiteral("Type"), QStringLiteral("类型")), formWidget);
    QComboBox* typeCombo = new QComboBox(formWidget);
    typeCombo->addItem(QStringLiteral("filesystem"), QStringLiteral("filesystem"));
    typeCombo->addItem(QStringLiteral("volume"), QStringLiteral("volume"));
    typeCombo->addItem(QStringLiteral("snapshot"), QStringLiteral("snapshot"));
    form->addWidget(typeLabel, row, 0);
    form->addWidget(typeCombo, row, 1);
    row++;

    QLabel* volsizeLabel = new QLabel(tr3(QStringLiteral("Volsize"), QStringLiteral("Volsize"), QStringLiteral("卷大小")), formWidget);
    QLineEdit* volsizeEdit = new QLineEdit(formWidget);
    form->addWidget(volsizeLabel, row, 0);
    form->addWidget(volsizeEdit, row, 1);
    row++;

    QLabel* blocksizeLabel = new QLabel(tr3(QStringLiteral("Blocksize"), QStringLiteral("Blocksize"), QStringLiteral("块大小")), formWidget);
    QLineEdit* blocksizeEdit = new QLineEdit(formWidget);
    form->addWidget(blocksizeLabel, row, 0);
    form->addWidget(blocksizeEdit, row, 1);
    row++;

    QWidget* optsWidget = new QWidget(formWidget);
    QHBoxLayout* optsLay = new QHBoxLayout(optsWidget);
    optsLay->setContentsMargins(0, 0, 0, 0);
    optsLay->setSpacing(12);
    QCheckBox* parentsChk = new QCheckBox(tr3(QStringLiteral("Crear padres (-p)"), QStringLiteral("Create parents (-p)"), QStringLiteral("创建父级(-p)")), optsWidget);
    QCheckBox* sparseChk = new QCheckBox(tr3(QStringLiteral("Sparse (-s)"), QStringLiteral("Sparse (-s)"), QStringLiteral("稀疏(-s)")), optsWidget);
    QCheckBox* nomountChk = new QCheckBox(tr3(QStringLiteral("No montar (-u)"), QStringLiteral("Do not mount (-u)"), QStringLiteral("不挂载(-u)")), optsWidget);
    parentsChk->setChecked(true);
    optsLay->addWidget(parentsChk);
    optsLay->addWidget(sparseChk);
    optsLay->addWidget(nomountChk);
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QCheckBox* snapRecursiveChk = new QCheckBox(
        tr3(QStringLiteral("Snapshot recursivo (-r)"), QStringLiteral("Recursive snapshot (-r)"), QStringLiteral("递归快照(-r)")),
        formWidget);
    form->addWidget(snapRecursiveChk, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(tr3(QStringLiteral("Argumentos extra"), QStringLiteral("Extra args"), QStringLiteral("额外参数")), formWidget);
    QLineEdit* extraEdit = new QLineEdit(formWidget);
    form->addWidget(extraLabel, row, 0);
    form->addWidget(extraEdit, row, 1, 1, 3);
    row++;

    root->addWidget(formWidget);

    QGroupBox* propsGroup = new QGroupBox(tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")), &dlg);
    QVBoxLayout* propsGroupLay = new QVBoxLayout(propsGroup);
    propsGroupLay->setContentsMargins(6, 6, 6, 6);
    propsGroupLay->setSpacing(4);

    QScrollArea* propsScroll = new QScrollArea(propsGroup);
    propsScroll->setWidgetResizable(true);
    QWidget* propsContainer = new QWidget(propsScroll);
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
    propsContainer->setLayout(propsGrid);
    propsScroll->setWidget(propsContainer);
    propsGroupLay->addWidget(propsScroll);
    root->addWidget(propsGroup, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        tr3(QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")),
        QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    auto setSuggestedPath = [&]() {
        const QString t = typeCombo->currentData().toString();
        const bool isSnapshot = t == QStringLiteral("snapshot");
        QString suggested = QStringLiteral("new_dataset");
        if (t == QStringLiteral("volume")) {
            suggested = QStringLiteral("new_volume");
        } else if (isSnapshot) {
            suggested = QStringLiteral("new_snapshot");
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

        const QString newPath = isSnapshot ? (prefix + suggested + QStringLiteral("@snap"))
                                           : (prefix + suggested);
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
    };
    QObject::connect(typeCombo, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&]() { applyTypeUi(); });
    applyTypeUi();

    bool accepted = false;
    CreateDatasetOptions opt;
    QObject::connect(createBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString path = pathEdit->text().trimmed();
        const QString dsType = typeCombo->currentData().toString().trimmed().toLower();
        const QString volsize = volsizeEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Debe indicar el path del dataset."),
                                     QStringLiteral("Dataset path is required."),
                                     QStringLiteral("必须指定数据集路径。")));
            return;
        }
        if (dsType == QStringLiteral("snapshot") && !path.contains('@')) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Para snapshot, el path debe incluir '@'."),
                                     QStringLiteral("For snapshot, path must include '@'."),
                                     QStringLiteral("快照路径必须包含'@'。")));
            return;
        }
        if (dsType == QStringLiteral("volume") && volsize.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("Para volume, Volsize es obligatorio."),
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
        accepted = true;
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted || !accepted) {
        return;
    }

    const QString actionLabel = (opt.dsType == QStringLiteral("snapshot"))
                                    ? tr3(QStringLiteral("Crear snapshot"), QStringLiteral("Create snapshot"), QStringLiteral("创建快照"))
                                    : tr3(QStringLiteral("Crear dataset"), QStringLiteral("Create dataset"), QStringLiteral("创建数据集"));
    const QString cmd = buildZfsCreateCmd(opt);
    executeDatasetAction(side, actionLabel, ctx, cmd);
}

void MainWindow::actionDeleteDatasetOrSnapshot(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }
    const QString target = ctx.snapshotName.isEmpty() ? ctx.datasetName : (ctx.datasetName + QStringLiteral("@") + ctx.snapshotName);
    const auto confirm1 = QMessageBox::question(
        this,
        tr3(QStringLiteral("Confirmar borrado"), QStringLiteral("Confirm deletion"), QStringLiteral("确认删除")),
        tr3(QStringLiteral("Se va a borrar:\n%1\n¿Continuar?"),
            QStringLiteral("This will be deleted:\n%1\nContinue?"),
            QStringLiteral("将要删除：\n%1\n是否继续？")).arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm1 != QMessageBox::Yes) {
        return;
    }
    const auto confirm2 = QMessageBox::question(
        this,
        tr3(QStringLiteral("Confirmar borrado (2/2)"), QStringLiteral("Confirm deletion (2/2)"), QStringLiteral("确认删除（2/2）")),
        tr3(QStringLiteral("Confirmación final de borrado:\n%1"),
            QStringLiteral("Final deletion confirmation:\n%1"),
            QStringLiteral("最终删除确认：\n%1")).arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm2 != QMessageBox::Yes) {
        return;
    }

    const auto askRec = QMessageBox::question(
        this,
        tr3(QStringLiteral("Borrado recursivo"), QStringLiteral("Recursive deletion"), QStringLiteral("递归删除")),
        ctx.snapshotName.isEmpty()
            ? tr3(QStringLiteral("¿Borrar recursivamente datasets/snapshots hijos?"),
                  QStringLiteral("Delete child datasets/snapshots recursively?"),
                  QStringLiteral("是否递归删除子数据集/快照？"))
            : tr3(QStringLiteral("¿Borrar recursivamente este snapshot en hijos descendientes?"),
                  QStringLiteral("Delete this snapshot recursively on descendant children?"),
                  QStringLiteral("是否在后代子项上递归删除此快照？")),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    const bool recursive = (askRec == QMessageBox::Yes);
    QString cmd;
    cmd = recursive ? QStringLiteral("zfs destroy -r %1").arg(shSingleQuote(target))
                    : QStringLiteral("zfs destroy %1").arg(shSingleQuote(target));
    executeDatasetAction(side, QStringLiteral("Borrar"), ctx, cmd, 90000);
}
