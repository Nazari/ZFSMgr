#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QtWidgets>
#include <QRegularExpression>
#include <algorithm>
#include <functional>

namespace {
using mwhelpers::isMountedValueTrue;
using mwhelpers::oneLine;
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

void MainWindow::actionAdvancedCreateFromDir() {
    actionAdvancedCreateFromDir(currentConnContentSelection(m_connContentTree));
}

void MainWindow::actionAdvancedCreateFromDir(const DatasetSelectionContext& explicitCtx) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext curr = explicitCtx.valid ? explicitCtx : currentConnContentSelection(m_connContentTree);
    if (!curr.valid || curr.datasetName.isEmpty() || !curr.snapshotName.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto001"), QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = curr.datasetName.trimmed();
    const QString snap = curr.snapshotName.trimmed();
    if (ds.isEmpty() || !snap.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto002"), QStringLiteral("Debe seleccionar un dataset (no snapshot)."),
                                     QStringLiteral("You must select a dataset (not a snapshot)."),
                                     QStringLiteral("必须选择数据集（不能是快照）。")));
        return;
    }

    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = curr.connIdx;
    ctx.poolName = curr.poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

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
    dlg.setWindowTitle(trk(QStringLiteral("t_advdir_auto003"), QStringLiteral("Crear dataset desde directorio"),
                           QStringLiteral("Create dataset from directory"),
                           QStringLiteral("从目录创建数据集")));
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

    QLabel* pathLabel = new QLabel(trk(QStringLiteral("t_advdir_auto004"), QStringLiteral("Path"), QStringLiteral("Path"), QStringLiteral("路径")), formWidget);
    QLineEdit* pathEdit = new QLineEdit(formWidget);
    pathEdit->setText(ds + QStringLiteral("/new_dataset"));
    form->addWidget(pathLabel, row, 0);
    form->addWidget(pathEdit, row, 1, 1, 3);
    row++;

    QLabel* typeLabel = new QLabel(trk(QStringLiteral("t_advdir_auto005"), QStringLiteral("Tipo"), QStringLiteral("Type"), QStringLiteral("类型")), formWidget);
    QComboBox* typeCombo = new QComboBox(formWidget);
    typeCombo->addItem(QStringLiteral("filesystem"), QStringLiteral("filesystem"));
    typeCombo->setCurrentIndex(0);
    typeCombo->setEnabled(false);
    form->addWidget(typeLabel, row, 0);
    form->addWidget(typeCombo, row, 1);
    row++;

    QLabel* mountDirLabel = new QLabel(trk(QStringLiteral("t_advdir_auto006"), QStringLiteral("Directorio local"),
                                           QStringLiteral("Local directory"),
                                           QStringLiteral("本地目录")),
                                       formWidget);
    QLineEdit* mountDirEdit = new QLineEdit(formWidget);
    QPushButton* browseDirBtn = new QPushButton(
        trk(QStringLiteral("t_advdir_auto007"), QStringLiteral("Seleccionar..."), QStringLiteral("Select..."), QStringLiteral("选择...")),
        formWidget);
    form->addWidget(mountDirLabel, row, 0);
    form->addWidget(mountDirEdit, row, 1, 1, 2);
    form->addWidget(browseDirBtn, row, 3);
    row++;
    QObject::connect(browseDirBtn, &QPushButton::clicked, &dlg, [&]() {
        if (ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
            QMessageBox::warning(
                &dlg,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_advdir_auto008"), QStringLiteral("Conexión inválida para explorar directorios remotos."),
                    QStringLiteral("Invalid connection for remote directory browsing."),
                    QStringLiteral("用于远程目录浏览的连接无效。")));
            return;
        }
        const ConnectionProfile& prof = m_profiles[ctx.connIdx];
        const bool isWinRemote = isWindowsConnection(ctx.connIdx);

        auto parentPath = [&](const QString& current) -> QString {
            if (isWinRemote) {
                QString p = current.trimmed();
                if (p.isEmpty()) {
                    return QStringLiteral("C:\\");
                }
                p.replace('/', '\\');
                const QRegularExpression rootRx(QStringLiteral("^[A-Za-z]:\\\\?$"));
                if (rootRx.match(p).hasMatch()) {
                    return p.endsWith('\\') ? p : (p + QStringLiteral("\\"));
                }
                const int pos = p.lastIndexOf('\\');
                if (pos <= 2) {
                    return (p.size() >= 2) ? (p.left(2) + QStringLiteral("\\")) : QStringLiteral("C:\\");
                }
                return p.left(pos);
            }
            const QString p = current.trimmed();
            if (p.isEmpty() || p == QStringLiteral("/")) {
                return QStringLiteral("/");
            }
            const int pos = p.lastIndexOf('/');
            if (pos <= 0) {
                return QStringLiteral("/");
            }
            return p.left(pos);
        };

        auto joinPath = [&](const QString& base, const QString& name) -> QString {
            if (isWinRemote) {
                QString b = base.trimmed();
                if (b.isEmpty()) {
                    b = QStringLiteral("C:\\");
                }
                b.replace('/', '\\');
                return b.endsWith('\\') ? (b + name) : (b + QStringLiteral("\\") + name);
            }
            QString b = base.trimmed();
            if (b.isEmpty()) {
                b = QStringLiteral("/");
            }
            if (b == QStringLiteral("/")) {
                return b + name;
            }
            return b.endsWith('/') ? (b + name) : (b + QStringLiteral("/") + name);
        };

        auto listRemoteDirs = [&](const QString& basePath, QString& resolvedPath, QStringList& children, QString& errorMsg) -> bool {
            const QString fallbackRoot = isWinRemote ? QStringLiteral("C:\\") : QStringLiteral("/");
            const QString requested = basePath.trimmed().isEmpty() ? fallbackRoot : basePath.trimmed();
            QString remoteCmd;
            if (isWinRemote) {
                auto psSingle = [](const QString& v) {
                    QString out = v;
                    out.replace(QStringLiteral("'"), QStringLiteral("''"));
                    return QStringLiteral("'") + out + QStringLiteral("'");
                };
                remoteCmd = QStringLiteral(
                                "$base=%1; "
                                "if (Test-Path -LiteralPath $base -PathType Container) { $p=(Resolve-Path -LiteralPath $base).Path } else { $p='C:\\' }; "
                                "Write-Output $p; "
                                "Get-ChildItem -LiteralPath $p -Directory -Name -ErrorAction SilentlyContinue | Sort-Object")
                                .arg(psSingle(requested));
            } else {
                remoteCmd = QStringLiteral(
                                "BASE=%1; "
                                "if [ -d \"$BASE\" ]; then cd \"$BASE\" 2>/dev/null || cd /; else cd /; fi; "
                                "pwd; "
                                "ls -1A 2>/dev/null | while IFS= read -r e; do [ -d \"$e\" ] && echo \"$e\"; done | sort")
                                .arg(shSingleQuote(requested));
            }
            QString out;
            QString err;
            int rc = -1;
            if (!runSsh(prof, remoteCmd, 20000, out, err, rc) || rc != 0) {
                errorMsg = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
                return false;
            }
            QStringList lines = out.split('\n', Qt::KeepEmptyParts);
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
                lines.removeLast();
            }
            if (lines.isEmpty()) {
                errorMsg = QStringLiteral("empty remote response");
                return false;
            }
            resolvedPath = lines.takeFirst().trimmed();
            children.clear();
            for (const QString& ln : lines) {
                const QString v = ln.trimmed();
                if (!v.isEmpty()) {
                    children << v;
                }
            }
            return true;
        };

        QDialog picker(&dlg);
        picker.setModal(true);
        picker.resize(640, 460);
        picker.setWindowTitle(
            trk(QStringLiteral("t_advdir_auto009"), QStringLiteral("Seleccionar directorio remoto"),
                QStringLiteral("Select remote directory"),
                QStringLiteral("选择远程目录")));
        QVBoxLayout* pv = new QVBoxLayout(&picker);
        pv->setContentsMargins(10, 10, 10, 10);
        pv->setSpacing(8);
        QLabel* connInfo = new QLabel(
            trk(QStringLiteral("t_advdir_auto010"), QStringLiteral("Conexión: %1").arg(prof.name),
                QStringLiteral("Connection: %1").arg(prof.name),
                QStringLiteral("连接：%1").arg(prof.name)),
            &picker);
        pv->addWidget(connInfo);
        QLineEdit* currentPathEdit = new QLineEdit(&picker);
        currentPathEdit->setReadOnly(true);
        pv->addWidget(currentPathEdit);
        QListWidget* dirList = new QListWidget(&picker);
        dirList->setSelectionMode(QAbstractItemView::SingleSelection);
        pv->addWidget(dirList, 1);
        QHBoxLayout* navRow = new QHBoxLayout();
        QPushButton* upBtn = new QPushButton(trk(QStringLiteral("t_advdir_auto011"), QStringLiteral("Subir"), QStringLiteral("Up"), QStringLiteral("上级")), &picker);
        QPushButton* refreshBtn = new QPushButton(trk(QStringLiteral("t_advdir_auto012"), QStringLiteral("Actualizar"), QStringLiteral("Refresh"), QStringLiteral("刷新")), &picker);
        navRow->addWidget(upBtn);
        navRow->addWidget(refreshBtn);
        navRow->addStretch(1);
        pv->addLayout(navRow);
        QDialogButtonBox* pb = new QDialogButtonBox(&picker);
        QPushButton* cancelBtn = pb->addButton(trk(QStringLiteral("t_advdir_auto013"), QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
        QPushButton* selectBtn = pb->addButton(trk(QStringLiteral("t_advdir_auto014"), QStringLiteral("Seleccionar"), QStringLiteral("Select"), QStringLiteral("选择")), QDialogButtonBox::AcceptRole);
        pv->addWidget(pb);
        QObject::connect(cancelBtn, &QPushButton::clicked, &picker, &QDialog::reject);

        QString current = mountDirEdit->text().trimmed();
        if (current.isEmpty()) {
            current = isWinRemote ? QStringLiteral("C:\\") : QStringLiteral("/");
        }
        std::function<void()> reloadDir;
        reloadDir = [&]() {
            QString resolved;
            QStringList children;
            QString errMsg;
            dirList->clear();
            if (!listRemoteDirs(current, resolved, children, errMsg)) {
                QMessageBox::warning(
                    &picker,
                    QStringLiteral("ZFSMgr"),
                    trk(QStringLiteral("t_advdir_auto015"), QStringLiteral("No se pudo listar directorios remotos:\n%1").arg(errMsg),
                        QStringLiteral("Could not list remote directories:\n%1").arg(errMsg),
                        QStringLiteral("无法列出远程目录：\n%1").arg(errMsg)));
                return;
            }
            current = resolved;
            currentPathEdit->setText(current);
            const QString par = parentPath(current);
            if (par != current) {
                QListWidgetItem* upItem = new QListWidgetItem(QStringLiteral(".."), dirList);
                upItem->setData(Qt::UserRole, par);
            }
            for (const QString& ch : children) {
                QListWidgetItem* it = new QListWidgetItem(ch, dirList);
                it->setData(Qt::UserRole, joinPath(current, ch));
            }
            if (dirList->count() > 0) {
                dirList->setCurrentRow(0);
            }
        };
        QObject::connect(refreshBtn, &QPushButton::clicked, &picker, [&]() { reloadDir(); });
        QObject::connect(upBtn, &QPushButton::clicked, &picker, [&]() {
            current = parentPath(current);
            reloadDir();
        });
        QObject::connect(dirList, &QListWidget::itemDoubleClicked, &picker, [&](QListWidgetItem* item) {
            if (!item) {
                return;
            }
            const QString next = item->data(Qt::UserRole).toString().trimmed();
            if (next.isEmpty()) {
                return;
            }
            current = next;
            reloadDir();
        });
        QObject::connect(selectBtn, &QPushButton::clicked, &picker, [&]() {
            QString selectedPath = currentPathEdit->text().trimmed();
            if (QListWidgetItem* sel = dirList->currentItem()) {
                const QString itemPath = sel->data(Qt::UserRole).toString().trimmed();
                if (!itemPath.isEmpty()) {
                    selectedPath = itemPath;
                }
            }
            mountDirEdit->setText(selectedPath);
            picker.accept();
        });
        reloadDir();
        picker.exec();
    });

    QLabel* blocksizeLabel = new QLabel(trk(QStringLiteral("t_advdir_auto016"), QStringLiteral("Blocksize"), QStringLiteral("Blocksize"), QStringLiteral("块大小")), formWidget);
    QLineEdit* blocksizeEdit = new QLineEdit(formWidget);
    form->addWidget(blocksizeLabel, row, 0);
    form->addWidget(blocksizeEdit, row, 1);
    row++;

    QWidget* optsWidget = new QWidget(formWidget);
    QHBoxLayout* optsLay = new QHBoxLayout(optsWidget);
    optsLay->setContentsMargins(0, 0, 0, 0);
    optsLay->setSpacing(12);
    QCheckBox* parentsChk = new QCheckBox(trk(QStringLiteral("t_advdir_auto017"), QStringLiteral("Crear padres (-p)"), QStringLiteral("Create parents (-p)"), QStringLiteral("创建父级(-p)")), optsWidget);
    parentsChk->setChecked(true);
    optsLay->addWidget(parentsChk);
    QCheckBox* deleteSourceDirChk = new QCheckBox(
        trk(QStringLiteral("t_advdir_del_src01"),
            QStringLiteral("Borrar directorio fuente tras copiar"),
            QStringLiteral("Delete source directory after copy"),
            QStringLiteral("复制后删除源目录")),
        optsWidget);
    deleteSourceDirChk->setChecked(false);
    optsLay->addWidget(deleteSourceDirChk);
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(trk(QStringLiteral("t_advdir_auto018"), QStringLiteral("Argumentos extra"), QStringLiteral("Extra args"), QStringLiteral("额外参数")), formWidget);
    QLineEdit* extraEdit = new QLineEdit(formWidget);
    form->addWidget(extraLabel, row, 0);
    form->addWidget(extraEdit, row, 1, 1, 3);
    row++;

    root->addWidget(formWidget);

    QGroupBox* propsGroup = new QGroupBox(trk(QStringLiteral("t_advdir_auto019"), QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")), &dlg);
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
        {QStringLiteral("compression"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("on"), QStringLiteral("lz4"), QStringLiteral("gzip"), QStringLiteral("zstd"), QStringLiteral("zle")}},
        {QStringLiteral("atime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("xattr"), QStringLiteral("combo"), {QString(), QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa")}},
        {QStringLiteral("acltype"), QStringLiteral("combo"), {QString(), QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), QStringLiteral("combo"), {QString(), QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("recordsize"), QStringLiteral("entry"), {}},
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
                // Permite niveles, por ejemplo: zstd-19, gzip-9.
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
    QPushButton* cancelBtn = buttons->addButton(trk(QStringLiteral("t_advdir_auto020"), QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        trk(QStringLiteral("t_advdir_auto021"), QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")),
        QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    bool accepted = false;
    CreateDatasetOptions opt;
    QString selectedMountDir;
    QObject::connect(createBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString path = pathEdit->text().trimmed();
        const QString mountDir = mountDirEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto022"), QStringLiteral("Debe indicar el path del dataset."),
                                     QStringLiteral("Dataset path is required."),
                                     QStringLiteral("必须指定数据集路径。")));
            return;
        }
        if (mountDir.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto023"), QStringLiteral("Debe seleccionar un directorio local."),
                                     QStringLiteral("You must select a local directory."),
                                     QStringLiteral("必须选择本地目录。")));
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
        opt.dsType = QStringLiteral("filesystem");
        opt.volsize.clear();
        opt.blocksize = blocksizeEdit->text().trimmed();
        opt.parents = parentsChk->isChecked();
        opt.sparse = false;
        opt.nomount = false;
        opt.snapshotRecursive = false;
        opt.properties = properties;
        opt.extraArgs = extraEdit->text().trimmed();
        selectedMountDir = mountDir;
        accepted = true;
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted || !accepted) {
        return;
    }

    const bool isWin = isWindowsConnection(ctx.connIdx);
    const bool deleteSourceDir = deleteSourceDirChk->isChecked();
    const QString createCmd = buildZfsCreateCmd(opt);
    QString cmd;
    bool allowWindowsScript = false;
    if (!isWin) {
        cmd = QStringLiteral(
                  "set -e; "
                  "DATASET=%1; "
                  "SRC_DIR=%2; "
                  "RSYNC_OPTS='-aHWS'; "
                  "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                  "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                  "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                  "TMP_MP=$(mktemp -d /tmp/zfsmgr-fromdir-mp-XXXXXX); "
                  "BACKUP_DIR=''; "
                  "cleanup(){ "
                  "  rc=$?; "
                  "  if [ $rc -ne 0 ]; then "
                  "    if [ -n \"$BACKUP_DIR\" ] && [ ! -e \"$SRC_DIR\" ] && [ -d \"$BACKUP_DIR\" ]; then mv \"$BACKUP_DIR\" \"$SRC_DIR\" || true; fi; "
                  "  fi; "
                  "  rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; "
                  "  exit $rc; "
                  "}; "
                  "trap cleanup EXIT INT TERM; "
                  "[ -d \"$SRC_DIR\" ] || { echo 'source directory does not exist'; exit 2; }; "
                  "if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx \"$SRC_DIR\" >/dev/null 2>&1; then "
                  "  echo 'mountpoint already in use'; exit 3; "
                  "fi; "
                  "%3; "
                  "zfs set canmount=on \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$TMP_MP\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "normp(){ p=\"$1\"; [ -z \"$p\" ] && return; [ -d \"$p\" ] && (cd \"$p\" 2>/dev/null && pwd -P) || printf '%s' \"$p\"; }; "
                  "ACTIVE_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "N_ACTIVE=$(normp \"$ACTIVE_MP\"); N_TMP=$(normp \"$TMP_MP\"); "
                  "[ \"$N_ACTIVE\" = \"$N_TMP\" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 4; }; "
                  "rsync $RSYNC_OPTS \"$SRC_DIR\"/ \"$TMP_MP\"/; "
                  "if [ %4 -eq 1 ]; then "
                  "  BACKUP_DIR=\"$SRC_DIR.zfsmgr-bak-$$\"; "
                  "  i=0; "
                  "  while [ -e \"$BACKUP_DIR\" ]; do i=$((i+1)); BACKUP_DIR=\"$SRC_DIR.zfsmgr-bak-$$-$i\"; done; "
                  "  mv \"$SRC_DIR\" \"$BACKUP_DIR\"; "
                  "  mkdir -p \"$SRC_DIR\"; "
                  "  zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  zfs set mountpoint=\"$SRC_DIR\" \"$DATASET\"; "
                  "  zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  FINAL_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "  N_FINAL=$(normp \"$FINAL_MP\"); N_SRC=$(normp \"$SRC_DIR\"); "
                  "  if [ \"$N_FINAL\" != \"$N_SRC\" ]; then "
                  "    zfs set mountpoint=\"$TMP_MP\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "    zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "    rm -rf \"$SRC_DIR\"; "
                  "    mv \"$BACKUP_DIR\" \"$SRC_DIR\"; "
                  "    echo 'failed to switch mountpoint to destination directory'; "
                  "    exit 5; "
                  "  fi; "
                  "  rm -rf \"$BACKUP_DIR\"; "
                  "  BACKUP_DIR=''; "
                  "else "
                  "  zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  zfs set mountpoint=\"$SRC_DIR\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  zfs set canmount=off \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  echo \"[FROMDIR] source preserved at $SRC_DIR; dataset left unmounted (canmount=off)\"; "
                  "fi; "
                  "BACKUP_DIR=''; "
                  "trap - EXIT INT TERM; "
                  "rmdir \"$TMP_MP\" >/dev/null 2>&1 || true")
                  .arg(shSingleQuote(opt.datasetPath),
                       shSingleQuote(selectedMountDir),
                       createCmd,
                       deleteSourceDir ? QStringLiteral("1") : QStringLiteral("0"));
    } else {
        allowWindowsScript = true;
        auto psSingle = [](const QString& v) {
            QString out = v;
            out.replace(QStringLiteral("'"), QStringLiteral("''"));
            return QStringLiteral("'") + out + QStringLiteral("'");
        };
        cmd = QStringLiteral(
                  "$ErrorActionPreference = 'Stop'; "
                  "$dataset = %1; "
                  "$srcDir = %2; "
                  "$deleteSrc = %4; "
                  "if (-not (Test-Path -LiteralPath $srcDir -PathType Container)) { throw 'source directory does not exist'; } "
                  "$srcDir = (Resolve-Path -LiteralPath $srcDir).Path; "
                  "%3; "
                  "zfs set canmount=on $dataset 2>$null | Out-Null; "
                  "zfs mount $dataset 2>$null | Out-Null; "
                  "$activeMp = ''; "
                  "foreach ($line in @(zfs mount 2>$null)) { "
                  "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "    if ($Matches[1] -eq $dataset) { $activeMp = $Matches[2].Trim(); break; } "
                  "  } "
                  "} "
                  "if ([string]::IsNullOrWhiteSpace($activeMp)) { throw 'could not resolve effective mountpoint'; } "
                  "if ([string]::Equals($activeMp, $srcDir, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'mountpoint already in use'; } "
                  "$null = robocopy $srcDir $activeMp /E /COPYALL /R:1 /W:1 /NFL /NDL /NP; "
                  "if ($LASTEXITCODE -ge 8) { throw ('robocopy failed with code ' + $LASTEXITCODE); } "
                  "if ($deleteSrc -eq 1) { "
                  "  Get-ChildItem -LiteralPath $srcDir -Force -ErrorAction SilentlyContinue | ForEach-Object { "
                  "    Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue "
                  "  } "
                  "} else { "
                  "  zfs unmount $dataset 2>$null | Out-Null; "
                  "  zfs set canmount=off $dataset 2>$null | Out-Null; "
                  "  Write-Output ('[FROMDIR] source preserved at ' + $srcDir + '; dataset left unmounted (canmount=off)'); "
                  "} "
                  "Write-Output ('[FROMDIR] copied to effective mountpoint: ' + $activeMp)")
                  .arg(psSingle(opt.datasetPath),
                       psSingle(selectedMountDir),
                       createCmd,
                       deleteSourceDir ? QStringLiteral("1") : QStringLiteral("0"));
    }
    executeDatasetAction(QStringLiteral("conncontent"),
                         trk(QStringLiteral("t_advdir_auto024"), QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")),
                         ctx,
                         cmd,
                         90000,
                         allowWindowsScript);
}

void MainWindow::actionAdvancedToDir() {
    actionAdvancedToDir(currentConnContentSelection(m_connContentTree));
}

void MainWindow::actionAdvancedToDir(const DatasetSelectionContext& explicitCtx) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext curr = explicitCtx.valid ? explicitCtx : currentConnContentSelection(m_connContentTree);
    if (!curr.valid || curr.datasetName.isEmpty() || !curr.snapshotName.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto025"), QStringLiteral("Seleccione un dataset en Avanzado."),
                                     QStringLiteral("Select a dataset in Advanced."),
                                     QStringLiteral("请在高级页选择一个数据集。")));
        return;
    }
    const QString ds = curr.datasetName.trimmed();
    const QString snap = curr.snapshotName.trimmed();
    if (ds.isEmpty() || !snap.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto026"), QStringLiteral("Debe seleccionar un dataset (no snapshot)."),
                                     QStringLiteral("You must select a dataset (not a snapshot)."),
                                     QStringLiteral("必须选择数据集（不能是快照）。")));
        return;
    }

    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = curr.connIdx;
    ctx.poolName = curr.poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    QDialog dlg(this);
    dlg.setWindowTitle(trk(QStringLiteral("t_advdir_auto027"), QStringLiteral("Exportar dataset a directorio"),
                           QStringLiteral("Export dataset to directory"),
                           QStringLiteral("导出数据集到目录")));
    dlg.setModal(true);
    dlg.resize(720, 180);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QLabel* intro = new QLabel(
        trk(QStringLiteral("t_advdir_auto028"), QStringLiteral("Se copiará el contenido de %1 a un directorio local. Puede elegir si destruir el dataset fuente.")
                .arg(ds),
            QStringLiteral("Contents of %1 will be copied to a local directory. You can choose whether to destroy the source dataset.")
                .arg(ds),
            QStringLiteral("将把 %1 的内容复制到本地目录。您可以选择是否销毁源数据集。")
                .arg(ds)),
        &dlg);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QHBoxLayout* dirRow = new QHBoxLayout();
    QLabel* dirLabel = new QLabel(trk(QStringLiteral("t_advdir_auto029"), QStringLiteral("Directorio local"),
                                      QStringLiteral("Local directory"),
                                      QStringLiteral("本地目录")),
                                  &dlg);
    QLineEdit* dirEdit = new QLineEdit(&dlg);
    QPushButton* browseBtn = new QPushButton(
        trk(QStringLiteral("t_advdir_auto030"), QStringLiteral("Seleccionar..."), QStringLiteral("Select..."), QStringLiteral("选择...")),
        &dlg);
    dirRow->addWidget(dirLabel, 0);
    dirRow->addWidget(dirEdit, 1);
    dirRow->addWidget(browseBtn, 0);
    root->addLayout(dirRow);

    QCheckBox* deleteSourceDatasetChk = new QCheckBox(
        trk(QStringLiteral("t_advdir_del_ds001"),
            QStringLiteral("Borrar dataset fuente tras copiar"),
            QStringLiteral("Delete source dataset after copy"),
            QStringLiteral("复制后删除源数据集")),
        &dlg);
    deleteSourceDatasetChk->setChecked(false);
    root->addWidget(deleteSourceDatasetChk);

    QObject::connect(browseBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString picked = QFileDialog::getExistingDirectory(
            &dlg,
            trk(QStringLiteral("t_advdir_auto031"), QStringLiteral("Seleccionar directorio local"),
                QStringLiteral("Select local directory"),
                QStringLiteral("选择本地目录")),
            dirEdit->text().trimmed());
        if (!picked.trimmed().isEmpty()) {
            dirEdit->setText(picked);
        }
    });

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(trk(QStringLiteral("t_advdir_auto032"), QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* acceptBtn = buttons->addButton(trk(QStringLiteral("t_advdir_auto033"), QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(acceptBtn, &QPushButton::clicked, &dlg, [&]() {
        if (dirEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto034"), QStringLiteral("Debe seleccionar un directorio local."),
                                     QStringLiteral("You must select a local directory."),
                                     QStringLiteral("必须选择本地目录。")));
            return;
        }
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QString localDir = dirEdit->text().trimmed();
    const bool deleteSourceDataset = deleteSourceDatasetChk->isChecked();

    const bool isWin = isWindowsConnection(ctx.connIdx);
    QString cmd;
    bool allowWindowsScript = false;
    if (!isWin) {
        cmd = QStringLiteral(
                  "set -e; "
                  "DATASET=%1; "
                  "DST_DIR=%2; "
                  "RSYNC_OPTS='-aHWS'; "
                  "rsync -A --version >/dev/null 2>&1 && RSYNC_OPTS=\"$RSYNC_OPTS -A\"; "
                  "if rsync -X --version >/dev/null 2>&1; then RSYNC_OPTS=\"$RSYNC_OPTS -X\"; "
                  "elif rsync --help 2>/dev/null | grep -q -- '--extended-attributes'; then RSYNC_OPTS=\"$RSYNC_OPTS --extended-attributes\"; fi; "
                  "TMP_MP=$(mktemp -d /tmp/zfsmgr-todir-mp-XXXXXX); "
                  "TMP_OUT=$(mktemp -d /tmp/zfsmgr-todir-out-XXXXXX); "
                  "BACKUP_DIR=''; RESTORE_NEEDED=0; "
                  "OLD_MP=$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true); "
                  "OLD_MOUNTED=$(zfs get -H -o value mounted \"$DATASET\" 2>/dev/null || true); "
                  "cleanup(){ "
                  "  rc=$?; "
                  "  if [ $rc -ne 0 ]; then "
                  "    if [ \"$RESTORE_NEEDED\" = \"1\" ] && [ -n \"$BACKUP_DIR\" ] && [ -d \"$BACKUP_DIR\" ]; then "
                  "      rm -rf \"$DST_DIR\" >/dev/null 2>&1 || true; "
                  "      mv \"$BACKUP_DIR\" \"$DST_DIR\" >/dev/null 2>&1 || true; "
                  "    fi; "
                  "    if zfs list -H -o name \"$DATASET\" >/dev/null 2>&1; then "
                  "      zfs set mountpoint=\"$OLD_MP\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "      if [ \"$OLD_MOUNTED\" = \"yes\" ] || [ \"$OLD_MOUNTED\" = \"on\" ]; then zfs mount \"$DATASET\" >/dev/null 2>&1 || true; fi; "
                  "    fi; "
                  "  fi; "
                  "  rm -rf \"$TMP_MP\" >/dev/null 2>&1 || true; "
                  "  rm -rf \"$TMP_OUT\" >/dev/null 2>&1 || true; "
                  "  exit $rc; "
                  "}; "
                  "trap cleanup EXIT INT TERM; "
                  "if zfs mount 2>/dev/null | awk '{print $2}' | grep -Fx \"$DST_DIR\" >/dev/null 2>&1; then "
                  "  echo 'destination directory is already a zfs mountpoint'; exit 2; "
                  "fi; "
                  "zfs set canmount=on \"$DATASET\" >/dev/null 2>&1 || true; "
                  "zfs set mountpoint=\"$TMP_MP\" \"$DATASET\"; "
                  "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "normp(){ p=\"$1\"; [ -z \"$p\" ] && return; [ -d \"$p\" ] && (cd \"$p\" 2>/dev/null && pwd -P) || printf '%s' \"$p\"; }; "
                  "ACTIVE_MP=$(zfs mount 2>/dev/null | awk -v d=\"$DATASET\" '$1==d{print $2;exit}'); "
                  "N_ACTIVE=$(normp \"$ACTIVE_MP\"); N_TMP=$(normp \"$TMP_MP\"); "
                  "[ \"$N_ACTIVE\" = \"$N_TMP\" ] || { echo 'could not mount dataset on temporary mountpoint'; exit 3; }; "
                  "rsync $RSYNC_OPTS \"$TMP_MP\"/ \"$TMP_OUT\"/; "
                  "if [ -e \"$DST_DIR\" ]; then "
                  "  BACKUP_DIR=\"$DST_DIR.zfsmgr-bak-$$\"; "
                  "  i=0; while [ -e \"$BACKUP_DIR\" ]; do i=$((i+1)); BACKUP_DIR=\"$DST_DIR.zfsmgr-bak-$$-$i\"; done; "
                  "  mv \"$DST_DIR\" \"$BACKUP_DIR\"; "
                  "  RESTORE_NEEDED=1; "
                  "else "
                  "  mkdir -p \"$(dirname \"$DST_DIR\")\"; "
                  "fi; "
                  "mv \"$TMP_OUT\" \"$DST_DIR\"; "
                  "zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "if [ %3 -eq 1 ]; then "
                  "  zfs destroy -r \"$DATASET\"; "
                  "else "
                  "  zfs set mountpoint=\"$OLD_MP\" \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  zfs umount \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  zfs set canmount=off \"$DATASET\" >/dev/null 2>&1 || true; "
                  "  echo \"[TODIR] dataset preserved unmounted (canmount=off)\"; "
                  "fi; "
                  "if [ -n \"$BACKUP_DIR\" ]; then rm -rf \"$BACKUP_DIR\"; fi; "
                  "RESTORE_NEEDED=0; "
                  "trap - EXIT INT TERM; "
                  "rm -rf \"$TMP_MP\" >/dev/null 2>&1 || true")
                  .arg(shSingleQuote(ds),
                       shSingleQuote(localDir),
                       deleteSourceDataset ? QStringLiteral("1") : QStringLiteral("0"));
    } else {
        allowWindowsScript = true;
        auto psSingle = [](const QString& v) {
            QString out = v;
            out.replace(QStringLiteral("'"), QStringLiteral("''"));
            return QStringLiteral("'") + out + QStringLiteral("'");
        };
        cmd = QStringLiteral(
                  "$ErrorActionPreference = 'Stop'; "
                  "$dataset = %1; "
                  "$dstDir = %2; "
                  "$deleteSrc = %3; "
                  "$dstParent = Split-Path -Parent $dstDir; "
                  "if ([string]::IsNullOrWhiteSpace($dstParent)) { throw 'invalid destination directory'; } "
                  "if (-not (Test-Path -LiteralPath $dstParent)) { New-Item -ItemType Directory -Force -Path $dstParent | Out-Null; } "
                  "$mountRows = @(zfs mount 2>$null); "
                  "$used = $false; "
                  "foreach ($line in $mountRows) { "
                  "  if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "    $mp = $Matches[2].Trim(); "
                  "    if ([string]::Equals($mp, $dstDir, [System.StringComparison]::OrdinalIgnoreCase)) { $used = $true; break; } "
                  "  } "
                  "} "
                  "if ($used) { throw 'destination directory is already a zfs mountpoint'; } "
                  "$oldMp = (zfs get -H -o value mountpoint $dataset 2>$null | Select-Object -First 1); "
                  "$oldMounted = (zfs get -H -o value mounted $dataset 2>$null | Select-Object -First 1); "
                  "$tmpMp = Join-Path $env:TEMP ('zfsmgr-todir-mp-' + [Guid]::NewGuid().ToString('N')); "
                  "$tmpOut = Join-Path $env:TEMP ('zfsmgr-todir-out-' + [Guid]::NewGuid().ToString('N')); "
                  "New-Item -ItemType Directory -Force -Path $tmpMp | Out-Null; "
                  "New-Item -ItemType Directory -Force -Path $tmpOut | Out-Null; "
                  "$backupDir = ''; "
                  "$restoreNeeded = $false; "
                  "try { "
                  "  zfs set canmount=on $dataset 2>$null | Out-Null; "
                  "  zfs set mountpoint=$tmpMp $dataset | Out-Null; "
                  "  zfs mount $dataset 2>$null | Out-Null; "
                  "  $activeMp = ''; "
                  "  foreach ($line in @(zfs mount 2>$null)) { "
                  "    if ($line -match '^\\s*(\\S+)\\s+(.+)$') { "
                  "      if ($Matches[1] -eq $dataset) { $activeMp = $Matches[2].Trim(); break; } "
                  "    } "
                  "  } "
                  "  if (-not [string]::Equals($activeMp, $tmpMp, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'could not mount dataset on temporary mountpoint'; } "
                  "  $rc = (robocopy $tmpMp $tmpOut /E /COPYALL /R:1 /W:1 /NFL /NDL /NP); "
                  "  if ($LASTEXITCODE -ge 8) { throw ('robocopy failed with code ' + $LASTEXITCODE); } "
                  "  if (Test-Path -LiteralPath $dstDir) { "
                  "    $backupDir = $dstDir + '.zfsmgr-bak-' + [Guid]::NewGuid().ToString('N'); "
                  "    Move-Item -LiteralPath $dstDir -Destination $backupDir; "
                  "    $restoreNeeded = $true; "
                  "  } "
                  "  Move-Item -LiteralPath $tmpOut -Destination $dstDir; "
                  "  zfs unmount $dataset 2>$null | Out-Null; "
                  "  if ($deleteSrc -eq 1) { "
                  "    zfs destroy -r $dataset | Out-Null; "
                  "  } else { "
                  "    if ($oldMp) { zfs set mountpoint=$oldMp $dataset 2>$null | Out-Null; } "
                  "    zfs unmount $dataset 2>$null | Out-Null; "
                  "    zfs set canmount=off $dataset 2>$null | Out-Null; "
                  "    Write-Output '[TODIR] dataset preserved unmounted (canmount=off)'; "
                  "  } "
                  "  if ($backupDir -and (Test-Path -LiteralPath $backupDir)) { Remove-Item -LiteralPath $backupDir -Recurse -Force; } "
                  "  $restoreNeeded = $false; "
                  "} catch { "
                  "  if ($restoreNeeded -and $backupDir -and (Test-Path -LiteralPath $backupDir)) { "
                  "    if (Test-Path -LiteralPath $dstDir) { Remove-Item -LiteralPath $dstDir -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "    Move-Item -LiteralPath $backupDir -Destination $dstDir -ErrorAction SilentlyContinue; "
                  "  } "
                  "  if (zfs list -H -o name $dataset 2>$null) { "
                  "    if ($oldMp) { zfs set mountpoint=$oldMp $dataset 2>$null | Out-Null; } "
                  "    if ($oldMounted -match '^(yes|on)$') { zfs mount $dataset 2>$null | Out-Null; } "
                  "  } "
                  "  throw; "
                  "} finally { "
                  "  if (Test-Path -LiteralPath $tmpMp) { Remove-Item -LiteralPath $tmpMp -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "  if (Test-Path -LiteralPath $tmpOut) { Remove-Item -LiteralPath $tmpOut -Recurse -Force -ErrorAction SilentlyContinue; } "
                  "}")
                  .arg(psSingle(ds),
                       psSingle(localDir),
                       deleteSourceDataset ? QStringLiteral("1") : QStringLiteral("0"));
    }

    executeDatasetAction(QStringLiteral("conncontent"),
                         trk(QStringLiteral("t_advdir_auto035"), QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")),
                         ctx,
                         cmd,
                         0,
                         allowWindowsScript);
}
