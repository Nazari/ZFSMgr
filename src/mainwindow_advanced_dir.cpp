#include "mainwindow.h"
#include "agentversion.h"
#include "mainwindow_helpers.h"
#include "daemonpayload.h"

#include <QtWidgets>
#include <QJsonArray>
#include <QJsonDocument>
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
    QString encryptionPassphrase;
};

// Lo mismo que buildZfsCreateCmd pero como argv, sin entrecomillar: va por RPC, donde
// no hay intérprete que deshacer las comillas. Existe para que la frase de cifrado no
// tenga que viajar dentro de una orden de shell.
QStringList buildZfsCreateArgv(const CreateDatasetOptions& opt) {
    const QString dsType = opt.dsType.trimmed().toLower();
    QStringList a;
    if (dsType == QStringLiteral("snapshot")) {
        a << QStringLiteral("snapshot");
        if (opt.snapshotRecursive) {
            a << QStringLiteral("-r");
        }
    } else {
        a << QStringLiteral("create");
        if (opt.parents) {
            a << QStringLiteral("-p");
        }
        if (opt.sparse) {
            a << QStringLiteral("-s");
        }
        if (opt.nomount) {
            a << QStringLiteral("-u");
        }
        if (!opt.blocksize.trimmed().isEmpty()) {
            a << QStringLiteral("-b") << opt.blocksize.trimmed();
        }
        if (dsType == QStringLiteral("volume") && !opt.volsize.trimmed().isEmpty()) {
            a << QStringLiteral("-V") << opt.volsize.trimmed();
        }
    }
    for (const QString& prop : opt.properties) {
        const QString pp = prop.trimmed();
        if (!pp.isEmpty()) {
            a << QStringLiteral("-o") << pp;
        }
    }
    // extraArgs es texto libre del usuario: se trocea por espacios, que es lo mismo que
    // hacía el shell, pero sin que un ';' pueda encadenar nada.
    const QStringList extra = opt.extraArgs.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                                            Qt::SkipEmptyParts);
    a += extra;
    a << opt.datasetPath.trimmed();
    return a;
}

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
    if (!requireFeature(curr.connIdx, zfsmgr::caps::Feature::DirBreakdown)) {
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
    struct DirTab {
        int connIdx{-1};
        bool windows{false};
        QTreeWidget* tree{nullptr};
    };

    auto psSingle = [](const QString& v) {
        QString out = v;
        out.replace(QStringLiteral("'"), QStringLiteral("''"));
        return QStringLiteral("'") + out + QStringLiteral("'");
    };
    auto normalizePathForCompare = [](QString path, bool windows) {
        QString p = path.trimmed();
        if (windows) {
            p.replace('/', '\\');
            while (p.endsWith('\\') && p.size() > 3) {
                p.chop(1);
            }
            return p.toLower();
        }
        while (p.endsWith('/') && p.size() > 1) {
            p.chop(1);
        }
        return p;
    };
    auto isAncestorPath = [&](const QString& candidateAncestor, const QString& candidatePath, bool windows) {
        const QString anc = normalizePathForCompare(candidateAncestor, windows);
        const QString val = normalizePathForCompare(candidatePath, windows);
        if (anc.isEmpty() || val.isEmpty()) {
            return false;
        }
        if (anc == val) {
            return true;
        }
        if (windows) {
            return val.startsWith(anc + QStringLiteral("\\"));
        }
        if (anc == QStringLiteral("/")) {
            return val.startsWith(QStringLiteral("/"));
        }
        return val.startsWith(anc + QStringLiteral("/"));
    };

    QDialog dlg(this);
    dlg.setWindowTitle(trk(QStringLiteral("t_advdir_auto003"), QStringLiteral("Crear dataset desde directorio"),
                           QStringLiteral("Create dataset from directory"),
                           QStringLiteral("从目录创建数据集")));
    dlg.setModal(true);
    dlg.resize(960, 820);

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
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(trk(QStringLiteral("t_advdir_auto018"), QStringLiteral("Argumentos extra"), QStringLiteral("Extra args"), QStringLiteral("额外参数")), formWidget);
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
        // El primero, y obligatorio. No estaba: si el pool tiene mountpoint=none —cosa
        // corriente— el dataset lo heredaba, no había dónde escribir, y la copia se
        // quedaba colgada moviendo cero bytes sin explicar nada. No se puede dejar a
        // elección: sin punto de montaje esta acción no puede funcionar.
        {QStringLiteral("mountpoint"), QStringLiteral("entry"), {}},
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
        const int r = i / 4;
        const int cBase = (i % 4) * 2;
        const bool required = (spec.name == QStringLiteral("mountpoint"));
        QLabel* lbl = new QLabel(required ? (spec.name + QStringLiteral(" *")) : spec.name,
                                 propsContainer);
        if (required) {
            QFont lf = lbl->font();
            lf.setBold(true);
            lbl->setFont(lf);
            lbl->setToolTip(trk(QStringLiteral("t_advdir_mp_required_tip001"),
                                QStringLiteral("Obligatorio: el dataset tiene que quedar montado "
                                               "en algún sitio para poder copiar dentro."),
                                QStringLiteral("Required: the dataset must end up mounted "
                                               "somewhere for the copy to have a destination."),
                                QStringLiteral("必填：数据集必须挂载到某个位置，复制才有目标。")));
        }
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
            if (required) {
                le->setPlaceholderText(QStringLiteral("/mnt/datos"));
            }
            editor.edit = le;
            propsGrid->addWidget(le, r, cBase + 1);
        }
        propEditors.push_back(editor);
    }
    propsContainer->setLayout(propsGrid);
    propsScroll->setWidget(propsContainer);
    propsGroupLay->addWidget(propsScroll);
    root->addWidget(propsGroup);

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

    enum DataRole {
        RolePath = Qt::UserRole + 1,
        RoleConnIdx,
        RoleLoaded
    };
    QVector<DirTab> dirTabs;
    QHash<int, int> tabIndexByConn;

    auto listRemoteDirs = [&](int connIdx, const QString& basePath, QStringList& children, QString& errorMsg) -> bool {
        if (connIdx < 0 || connIdx >= m_profiles.size()) {
            errorMsg = QStringLiteral("invalid connection");
            return false;
        }
        const ConnectionProfile prof = m_profiles[connIdx];
        const bool isWinRemote = isWindowsConnection(connIdx);
        QString remoteCmd;
        if (isWinRemote) {
            const QString normalized = basePath.trimmed().replace('/', '\\');
            if (normalized.isEmpty()) {
                remoteCmd = QStringLiteral(
                    "Get-PSDrive -PSProvider FileSystem | Select-Object -ExpandProperty Root | Sort-Object");
            } else {
                remoteCmd = QStringLiteral(
                                "$p=%1; "
                                "if (-not (Test-Path -LiteralPath $p -PathType Container)) { exit 2; } "
                                "Get-ChildItem -LiteralPath $p -Directory -Name -ErrorAction SilentlyContinue | Sort-Object")
                                .arg(psSingle(normalized));
            }
        } else {
            const QString requested = basePath.trimmed().isEmpty() ? QStringLiteral("/") : basePath.trimmed();
            remoteCmd = QStringLiteral(
                            "BASE=%1; "
                            "if [ -d \"$BASE\" ]; then cd \"$BASE\" 2>/dev/null || exit 2; else exit 2; fi; "
                            "ls -1A 2>/dev/null | while IFS= read -r e; do [ -d \"$e\" ] && printf '%s\\n' \"$e\"; done | sort")
                            .arg(shSingleQuote(requested));
        }
        QString out;
        QString err;
        int rc = -1;
        if (!runSsh(prof, remoteCmd, 25000, out, err, rc) || rc != 0) {
            errorMsg = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
            return false;
        }
        children.clear();
        const QStringList lines = out.split('\n', Qt::KeepEmptyParts);
        for (const QString& line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                children.push_back(trimmed);
            }
        }
        return true;
    };
    auto joinRemotePath = [](const QString& base, const QString& name, bool windows) -> QString {
        if (windows) {
            QString b = base.trimmed();
            b.replace('/', '\\');
            if (b.endsWith('\\')) {
                return b + name;
            }
            return b + QStringLiteral("\\") + name;
        }
        QString b = base.trimmed();
        if (b.isEmpty() || b == QStringLiteral("/")) {
            return QStringLiteral("/") + name;
        }
        return b.endsWith('/') ? (b + name) : (b + QStringLiteral("/") + name);
    };
    std::function<void(QTreeWidgetItem*)> loadTreeChildren;
    loadTreeChildren = [&](QTreeWidgetItem* parent) {
        if (!parent || parent->data(0, RoleLoaded).toBool()) {
            return;
        }
        const int connIdx = parent->data(0, RoleConnIdx).toInt();
        const QString basePath = parent->data(0, RolePath).toString();
        const bool win = isWindowsConnection(connIdx);
        QStringList children;
        QString err;
        if (!listRemoteDirs(connIdx, basePath, children, err)) {
            QMessageBox::warning(
                &dlg,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_advdir_tree_list_err"),
                    QStringLiteral("No se pudieron listar directorios para %1:\n%2")
                        .arg(m_profiles.value(connIdx).name, err),
                    QStringLiteral("Could not list directories for %1:\n%2")
                        .arg(m_profiles.value(connIdx).name, err),
                    QStringLiteral("无法列出 %1 的目录：\n%2")
                        .arg(m_profiles.value(connIdx).name, err)));
            return;
        }
        while (parent->childCount() > 0) {
            delete parent->takeChild(0);
        }
        for (const QString& name : children) {
            const QString childPath = joinRemotePath(basePath, name, win);
            QTreeWidgetItem* item = new QTreeWidgetItem(parent);
            item->setText(0, name);
            item->setData(0, RolePath, childPath);
            item->setData(0, RoleConnIdx, connIdx);
            item->setData(0, RoleLoaded, false);
            // Sin tristate, ni automático ni de usuario. Con ItemIsUserTristate el
            // primer clic dejaba el nodo en PartiallyChecked —un guion, no una marca—,
            // y la recogida solo cuenta Qt::Checked, así que parecía que no se marcaba
            // nada. Y ItemIsAutoTristate, encima, recalcula el estado del padre a partir
            // de sus hijos: un directorio con contenido volvía solo a Unchecked.
            //
            // Ninguna de las dos hace falta aquí: la recogida ya se queda con el marcado
            // más alto de cada rama (chooseThis = checked && !parentChecked), porque tar
            // copia el directorio entero.
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setCheckState(0, parent->checkState(0) == Qt::Checked ? Qt::Checked : Qt::Unchecked);
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
        parent->setChildIndicatorPolicy(children.isEmpty()
                                            ? QTreeWidgetItem::DontShowIndicatorWhenChildless
                                            : QTreeWidgetItem::ShowIndicator);
        parent->setData(0, RoleLoaded, true);
    };

    QGroupBox* dirsGroup = new QGroupBox(
        trk(QStringLiteral("t_advdir_tabs_title"),
            QStringLiteral("Directorios origen por conexión"),
            QStringLiteral("Source directories by connection"),
            QStringLiteral("按连接选择源目录")),
        &dlg);
    QVBoxLayout* dirsLay = new QVBoxLayout(dirsGroup);
    dirsLay->setContentsMargins(6, 6, 6, 6);
    dirsLay->setSpacing(4);
    QLabel* dirsHint = new QLabel(
        trk(QStringLiteral("t_advdir_tabs_hint2"),
            QStringLiteral("Marque los directorios a copiar. Si marca uno solo, su CONTENIDO va "
                           "a la raíz del dataset; si marca varios, cada uno va a un "
                           "subdirectorio con su nombre."),
            QStringLiteral("Check the directories to copy. If you check just one, its CONTENTS "
                           "go to the dataset root; if you check several, each goes into a "
                           "subdirectory named after it."),
            QStringLiteral("勾选要复制的目录。只勾选一个时，其内容会放到数据集根目录；勾选多个时，"
                           "每个会放入以其名称命名的子目录。")),
        dirsGroup);
    dirsHint->setWordWrap(true);
    dirsLay->addWidget(dirsHint);
    QCheckBox* deleteSourceDirChk = new QCheckBox(
        trk(QStringLiteral("t_advdir_del_src01"),
            QStringLiteral("Borrar directorios fuente tras copiar"),
            QStringLiteral("Delete source directories after copy"),
            QStringLiteral("复制后删除源目录")),
        dirsGroup);
    deleteSourceDirChk->setChecked(false);
    dirsLay->addWidget(deleteSourceDirChk);

    QTabWidget* dirTabsWidget = new QTabWidget(dirsGroup);
    dirsLay->addWidget(dirTabsWidget, 1);
    root->addWidget(dirsGroup, 1);

    for (int i = 0; i < m_profiles.size(); ++i) {
        if (isConnectionDisconnected(i)) {
            continue;
        }
        QWidget* page = new QWidget(dirTabsWidget);
        QVBoxLayout* pageLay = new QVBoxLayout(page);
        pageLay->setContentsMargins(0, 0, 0, 0);
        QTreeWidget* tree = new QTreeWidget(page);
        tree->setHeaderHidden(true);
        tree->setSelectionMode(QAbstractItemView::SingleSelection);
        tree->setAlternatingRowColors(true);
        pageLay->addWidget(tree);
        dirTabsWidget->addTab(page, m_profiles[i].name);

        DirTab t;
        t.connIdx = i;
        t.windows = isWindowsConnection(i);
        t.tree = tree;
        tabIndexByConn.insert(i, dirTabs.size());
        dirTabs.push_back(t);

        if (!t.windows) {
            QTreeWidgetItem* rootItem = new QTreeWidgetItem(tree);
            rootItem->setText(0, QStringLiteral("/"));
            rootItem->setData(0, RolePath, QStringLiteral("/"));
            rootItem->setData(0, RoleConnIdx, i);
            rootItem->setData(0, RoleLoaded, false);
            rootItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            rootItem->setCheckState(0, Qt::Unchecked);
            rootItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        } else {
            QStringList drives;
            QString err;
            if (listRemoteDirs(i, QString(), drives, err)) {
                for (const QString& drv : drives) {
                    QString drivePath = drv.trimmed();
                    if (drivePath.isEmpty()) {
                        continue;
                    }
                    drivePath.replace('/', '\\');
                    const QString name = drivePath.endsWith('\\')
                                             ? drivePath.left(drivePath.size() - 1)
                                             : drivePath;
                    QTreeWidgetItem* driveItem = new QTreeWidgetItem(tree);
                    driveItem->setText(0, name);
                    driveItem->setData(0, RolePath, drivePath);
                    driveItem->setData(0, RoleConnIdx, i);
                    driveItem->setData(0, RoleLoaded, false);
                    driveItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                    driveItem->setCheckState(0, Qt::Unchecked);
                    driveItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                }
            }
        }

        QObject::connect(tree, &QTreeWidget::itemExpanded, &dlg, [&](QTreeWidgetItem* item) {
            loadTreeChildren(item);
        });
        if (tree->topLevelItemCount() > 0) {
            tree->expandItem(tree->topLevelItem(0));
            loadTreeChildren(tree->topLevelItem(0));
        }
    }

    if (dirTabs.isEmpty()) {
        QLabel* noConnLabel = new QLabel(
            trk(QStringLiteral("t_advdir_tabs_empty"),
                QStringLiteral("No hay conexiones activas para seleccionar directorios."),
                QStringLiteral("There are no active connections to select directories."),
                QStringLiteral("没有可用于目录选择的活动连接。")),
            dirsGroup);
        noConnLabel->setWordWrap(true);
        dirsLay->addWidget(noConnLabel);
    }
    if (tabIndexByConn.contains(ctx.connIdx)) {
        dirTabsWidget->setCurrentIndex(tabIndexByConn.value(ctx.connIdx));
    }

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(trk(QStringLiteral("t_advdir_auto020"), QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        trk(QStringLiteral("t_advdir_auto021"), QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")),
        QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    bool accepted = false;
    CreateDatasetOptions opt;
    QVector<QPair<int, QString>> selectedSources;
    QObject::connect(createBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString path = pathEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto022"), QStringLiteral("Debe indicar el path del dataset."),
                                     QStringLiteral("Dataset path is required."),
                                     QStringLiteral("必须指定数据集路径。")));
            return;
        }
        // El punto de montaje decide si esta acción puede funcionar siquiera: la copia
        // escribe en él. Se exige aquí, con el diálogo abierto y el dato a mano, en vez
        // de dejar que falle al aplicarse.
        const QString mp = propValue(QStringLiteral("mountpoint")).trimmed();
        if (mp.isEmpty() || !mp.startsWith(QLatin1Char('/'))) {
            QMessageBox::warning(
                &dlg, QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_advdir_mp_required001"),
                    QStringLiteral("Indique el punto de montaje (mountpoint) del dataset, con "
                                   "ruta absoluta.\n\nEs donde se copiarán los directorios: sin "
                                   "él el dataset se crea pero no hay dónde escribir."),
                    QStringLiteral("Set the dataset mountpoint, as an absolute path.\n\nIt is "
                                   "where the directories get copied: without it the dataset is "
                                   "created but there is nowhere to write."),
                    QStringLiteral("请填写数据集的挂载点，使用绝对路径。\n\n目录会复制到该位置："
                                   "没有它，数据集虽然会创建，但没有可写入的地方。")));
            return;
        }

        QHash<int, QStringList> checkedByConn;
        std::function<void(QTreeWidgetItem*, bool)> collectChecked = [&](QTreeWidgetItem* item, bool parentChecked) {
            if (!item) {
                return;
            }
            const bool checked = item->checkState(0) == Qt::Checked;
            const bool chooseThis = checked && !parentChecked;
            if (chooseThis) {
                const QString p = item->data(0, RolePath).toString().trimmed();
                const int conn = item->data(0, RoleConnIdx).toInt();
                if (!p.isEmpty()) {
                    checkedByConn[conn].push_back(p);
                }
            }
            for (int i = 0; i < item->childCount(); ++i) {
                collectChecked(item->child(i), parentChecked || checked);
            }
        };
        for (const DirTab& tab : std::as_const(dirTabs)) {
            if (!tab.tree) {
                continue;
            }
            for (int i = 0; i < tab.tree->topLevelItemCount(); ++i) {
                collectChecked(tab.tree->topLevelItem(i), false);
            }
        }
        if (checkedByConn.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_tabs_required"),
                                     QStringLiteral("Debe marcar al menos un directorio origen."),
                                     QStringLiteral("You must check at least one source directory."),
                                     QStringLiteral("请至少勾选一个源目录。")));
            return;
        }
        const QList<int> keys = checkedByConn.keys();
        for (int connKey : keys) {
            QStringList values = checkedByConn.value(connKey);
            std::sort(values.begin(), values.end(), [](const QString& a, const QString& b) {
                return a.size() < b.size();
            });
            QStringList reduced;
            const bool isWinConn = isWindowsConnection(connKey);
            for (const QString& val : std::as_const(values)) {
                bool covered = false;
                for (const QString& kept : std::as_const(reduced)) {
                    if (isAncestorPath(kept, val, isWinConn)) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    reduced.push_back(val);
                }
            }
            checkedByConn[connKey] = reduced;
        }

        selectedSources.clear();
        const QList<int> orderedConnKeys = checkedByConn.keys();
        for (int connKey : orderedConnKeys) {
            const QStringList values = checkedByConn.value(connKey);
            for (const QString& value : values) {
                selectedSources.push_back(qMakePair(connKey, value));
            }
        }
        if (selectedSources.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_tabs_required"),
                                     QStringLiteral("Debe marcar al menos un directorio origen."),
                                     QStringLiteral("You must check at least one source directory."),
                                     QStringLiteral("请至少勾选一个源目录。")));
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
        opt.encryptionPassphrase = needsPromptPassphrase ? encPassEdit->text() : QString();
        accepted = true;
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted || !accepted) {
        return;
    }

    const bool deleteSourceDir = deleteSourceDirChk->isChecked();
    // Solo Windows: en Unix la creación va por RPC y la frase no toca ninguna orden.
    // La variante de shell para Unix —`printf '%s\\n%s\\n' 'FRASE' 'FRASE' | zfs create`—
    // se ha borrado, no desactivado: dejarla ahí era invitar a volver a usarla.
    const QString createCmd = buildZfsCreateCmd(opt);
    const QString createCmdWithPassphrase = [&]() {
        if (opt.encryptionPassphrase.isEmpty() || !isWindowsConnection(ctx.connIdx)) {
            return createCmd;
        }
        return QStringLiteral(
                   "$pp=%1; "
                   "$payload=$pp + \"`n\" + $pp + \"`n\"; "
                   "$bytes=[System.Text.Encoding]::UTF8.GetBytes($payload); "
                   "$ms=New-Object System.IO.MemoryStream(,$bytes); "
                   "$sr=New-Object System.IO.StreamReader($ms,[System.Text.Encoding]::UTF8); "
                   "$old=[Console]::In; [Console]::SetIn($sr); "
                   "try { %2 } finally { [Console]::SetIn($old); $sr.Dispose(); $ms.Dispose(); }")
            .arg(psSingle(opt.encryptionPassphrase), createCmd);
    }();

    QHash<int, ConnectionProfile> execProfiles;
    auto profileFor = [&](int connIdx) -> ConnectionProfile {
        if (execProfiles.contains(connIdx)) {
            return execProfiles.value(connIdx);
        }
        ConnectionProfile p = m_profiles.value(connIdx);
        if (isLocalConnection(p) && !isWindowsConnection(p)) {
            p.useSudo = true;
            if (!ensureLocalSudoCredentials(p)) {
                return ConnectionProfile{};
            }
        }
        execProfiles.insert(connIdx, p);
        return p;
    };

    ConnectionProfile dstProfile = profileFor(ctx.connIdx);
    if (dstProfile.name.trimmed().isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Desde Dir cancelado: faltan credenciales sudo locales"));
        return;
    }

    // Dónde cae cada origen DENTRO del dataset.
    //
    // Antes se usaba la ruta absoluta sin la barra inicial, así que copiar
    // /Users/linarese/.chirp reconstruía «Users/linarese/.chirp» entero dentro del
    // dataset. Eso replica la jerarquía de la máquina de origen, que no es lo que se
    // pide al elegir un directorio: el dataset ES el directorio.
    //
    // La regla ahora:
    //   - un solo directorio de origen  -> su CONTENIDO va a la raíz del dataset;
    //   - varios                        -> cada uno a un subdirectorio con su nombre;
    //   - si dos comparten nombre       -> se le antepone el de su conexión.
    //
    // Se registra el mapa completo antes de empezar: es una copia entre máquinas y
    // adivinar dónde acabó cada cosa no debería hacer falta.
    QHash<QString, QString> destSubdirByKey;
    {
        const auto sourceKey = [](int connIdx, const QString& path) {
            return QStringLiteral("%1|%2").arg(connIdx).arg(path.trimmed());
        };
        const auto baseNameOf = [&](int connIdx, const QString& path) {
            QString p = path.trimmed();
            const bool win = isWindowsConnection(connIdx);
            if (win) {
                p.replace('\\', '/');
            }
            while (p.endsWith(QLatin1Char('/'))) {
                p.chop(1);
            }
            const int slash = p.lastIndexOf(QLatin1Char('/'));
            const QString base = (slash >= 0) ? p.mid(slash + 1) : p;
            return base.trimmed();
        };
        QHash<QString, int> baseCount;
        for (const auto& e : std::as_const(selectedSources)) {
            if (!e.second.trimmed().isEmpty()) {
                ++baseCount[baseNameOf(e.first, e.second)];
            }
        }
        const bool single = (selectedSources.size() == 1);
        for (const auto& e : std::as_const(selectedSources)) {
            if (e.second.trimmed().isEmpty()) {
                continue;
            }
            QString sub;
            if (!single) {
                const QString base = baseNameOf(e.first, e.second);
                const QString connName = (e.first >= 0 && e.first < m_profiles.size())
                                             ? (m_profiles.at(e.first).name.trimmed().isEmpty()
                                                    ? m_profiles.at(e.first).id.trimmed()
                                                    : m_profiles.at(e.first).name.trimmed())
                                             : QString();
                sub = (baseCount.value(base) > 1 && !connName.isEmpty())
                          ? QStringLiteral("%1-%2").arg(connName, base)
                          : base;
            }
            destSubdirByKey.insert(sourceKey(e.first, e.second), sub);
            appLog(QStringLiteral("NORMAL"),
                   QStringLiteral("[FROMDIR] %1 -> %2")
                       .arg(e.second.trimmed(),
                            sub.isEmpty() ? QStringLiteral("(raíz del dataset)") : sub));
        }
    }
    const auto destSubdirFor = [&](int connIdx, const QString& path) {
        return destSubdirByKey.value(QStringLiteral("%1|%2").arg(connIdx).arg(path.trimmed()));
    };

    QStringList steps;
    // Paso previo por RPC, no por shell: la frase de cifrado iba incrustada en la orden
    // (`printf '%s\\n%s\\n' 'FRASE' 'FRASE' | zfs create ...`), así que quedaba en argv
    // —visible en `ps` en las dos máquinas—, en la vista previa y en el registro. Por el
    // RPC viaja dentro de la carga cifrada por mTLS y el daemon se la da a `zfs` por una
    // tubería. Se rellena más abajo, junto al resto del cambio pendiente.
    QStringList createArgv;
    QString createSecret;
    if (!isWindowsConnection(ctx.connIdx)) {
        const QStringList zfsArgs = buildZfsCreateArgv(opt);
        QJsonArray arr;
        for (const QString& a : zfsArgs) {
            arr.append(a);
        }
        const QByteArray argvJson = QJsonDocument(arr).toJson(QJsonDocument::Compact);
        createArgv = QStringList{QStringLiteral("--mutate-zfs-create"),
                                 QString::fromLatin1(argvJson.toBase64())};
        createSecret = opt.encryptionPassphrase;
        // Montarlo sí puede ir por shell: no lleva secretos.
        steps.push_back(sshExecFromLocal(
            dstProfile,
            withSudo(dstProfile,
                     QStringLiteral("DATASET=%1; "
                                    "zfs set canmount=on \"$DATASET\" >/dev/null 2>&1 || true; "
                                    "zfs mount \"$DATASET\" >/dev/null 2>&1 || true")
                         .arg(shSingleQuote(opt.datasetPath)))));
    } else {
        const QString dstSetup = QStringLiteral(
                                     "$ErrorActionPreference='Stop'; "
                                     "$dataset=%1; "
                                     "%2; "
                                     "zfs set canmount=on $dataset 2>$null | Out-Null; "
                                     "zfs mount $dataset 2>$null | Out-Null")
                                     .arg(psSingle(opt.datasetPath), createCmdWithPassphrase);
        steps.push_back(sshExecFromLocal(dstProfile, withSudo(dstProfile, dstSetup)));
    }

    QVector<QPair<int, QString>> deleteQueue = selectedSources;
    std::sort(deleteQueue.begin(), deleteQueue.end(), [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
        return a.second.size() > b.second.size();
    });

    for (const auto& srcEntry : std::as_const(selectedSources)) {
        const int srcConnIdx = srcEntry.first;
        const QString srcPath = srcEntry.second.trimmed();
        if (srcPath.isEmpty()) {
            continue;
        }
        ConnectionProfile srcProfile = profileFor(srcConnIdx);
        if (srcProfile.name.trimmed().isEmpty()) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Desde Dir cancelado: faltan credenciales sudo locales"));
            return;
        }

        const QString rel = destSubdirFor(srcConnIdx, srcPath);
        const QString srcTarCmd = mwhelpers::buildTarSourceCommand(isWindowsConnection(srcConnIdx),
                                                                   srcPath,
                                                                   mwhelpers::StreamCodec::None);
        const QString srcSeg = sshExecFromLocal(srcProfile, withSudo(srcProfile, srcTarCmd));

        QString dstRecvCmd;
        if (!isWindowsConnection(ctx.connIdx)) {
            // Typed RPC instead of an inline shell script: the agent mounts the
            // dataset, resolves the mountpoint, creates the subdirectory and runs tar
            // itself with execvp. withSudoStreamInput() keeps stdin wired to the
            // incoming tar stream.
            dstRecvCmd = daemonpayload::unixBinPath() + QStringLiteral(" --mutate-advanced-fromdir %1 %2")
                             .arg(shSingleQuote(opt.datasetPath), shSingleQuote(rel));
        } else {
            const QString relWin = rel;
            dstRecvCmd = QStringLiteral(
                             "$ErrorActionPreference='Stop'; "
                             "$dataset=%1; "
                             "zfs set canmount=on $dataset 2>$null | Out-Null; "
                             "zfs mount $dataset 2>$null | Out-Null; "
                             "$mp=''; "
                             "foreach ($line in @(zfs mount 2>$null)) { "
                             "  if ($line -match '^\\s*(\\S+)\\s+(.+)$' -and $Matches[1] -eq $dataset) { $mp=$Matches[2].Trim(); break; } "
                             "} "
                             "if ([string]::IsNullOrWhiteSpace($mp)) { throw 'could not resolve effective mountpoint'; } "
                             "$rel=%2; "
                             "$dst = [string]::IsNullOrWhiteSpace($rel) ? $mp : (Join-Path $mp $rel.Replace('/','\\\\')); "
                             "if (!(Test-Path -LiteralPath $dst)) { New-Item -ItemType Directory -Force -Path $dst | Out-Null; } "
                             "Write-Output ('[FROMDIR] tar recv -> ' + $dst); "
                             "tar -xpf - -C $dst")
                             .arg(psSingle(opt.datasetPath), psSingle(relWin));
        }
        const QString dstSeg = sshExecFromLocal(dstProfile, withSudoStreamInput(dstProfile, dstRecvCmd));
        steps.push_back(mwhelpers::buildPipedTransferCommand(srcSeg, dstSeg));
    }

    if (deleteSourceDir) {
        for (const auto& srcEntry : std::as_const(deleteQueue)) {
            const int srcConnIdx = srcEntry.first;
            const QString srcPath = srcEntry.second.trimmed();
            if (srcPath.isEmpty()) {
                continue;
            }
            ConnectionProfile srcProfile = profileFor(srcConnIdx);
            if (srcProfile.name.trimmed().isEmpty()) {
                appLog(QStringLiteral("INFO"), QStringLiteral("Desde Dir cancelado: faltan credenciales sudo locales"));
                return;
            }
            QString delCmd;
            if (!isWindowsConnection(srcConnIdx)) {
                delCmd = QStringLiteral("rm -rf %1").arg(shSingleQuote(srcPath));
            } else {
                delCmd = QStringLiteral(
                             "if (Test-Path -LiteralPath %1) { Remove-Item -LiteralPath %1 -Recurse -Force }")
                             .arg(psSingle(srcPath));
            }
            steps.push_back(sshExecFromLocal(srcProfile, withSudo(srcProfile, delCmd)));
        }
    }

    if (steps.isEmpty()) {
        return;
    }
    steps.push_back(QStringLiteral("echo '[FROMDIR] done'"));
    const QString localCmd = steps.join(QStringLiteral(" && "));
    // La etiqueta nombraba solo el DESTINO, así que un cambio pendiente que lee de otra
    // máquina se leía como si fuera todo local: «Local::fc16  Desde Dir Local::fc16/x».
    // En una operación que copia entre equipos, de dónde salen los datos es justo lo que
    // hay que poder comprobar antes de aplicar.
    QMap<int, int> dirsBySourceConn;  // conexión de origen -> nº de directorios
    for (const auto& srcEntry : std::as_const(selectedSources)) {
        if (!srcEntry.second.trimmed().isEmpty()) {
            ++dirsBySourceConn[srcEntry.first];
        }
    }
    QStringList originParts;
    for (auto it = dirsBySourceConn.cbegin(); it != dirsBySourceConn.cend(); ++it) {
        const int srcIdx = it.key();
        const QString srcName = (srcIdx >= 0 && srcIdx < m_profiles.size())
                                    ? (m_profiles.at(srcIdx).name.trimmed().isEmpty()
                                           ? m_profiles.at(srcIdx).id.trimmed()
                                           : m_profiles.at(srcIdx).name.trimmed())
                                    : QStringLiteral("?");
        originParts << QStringLiteral("%1 (%2)").arg(srcName).arg(it.value());
    }
    const QString displayLabel = QStringLiteral("Desde Dir %1 -> %2::%3")
                                     .arg(originParts.join(QStringLiteral(" + ")),
                                          dstProfile.name, opt.datasetPath);
    DatasetSelectionContext srcCtx;
    srcCtx.valid = false;
    QString errorText;
    PendingShellActionDraft draft{
        pendingTransferScopeLabel(srcCtx, ctx),
        displayLabel,
        localCmd,
        0,
        true,
        srcCtx,
        ctx,
        PendingShellActionDraft::RefreshScope::TargetOnly};
    if (!createSecret.isEmpty()
        && (ctx.connIdx < 0 || ctx.connIdx >= m_states.size()
            || !m_states[ctx.connIdx].daemonActive)) {
        // Sin daemon, runAgentCommand cae a SSH y la frase volvería a viajar en argv.
        // Mejor no dejar hacerlo que hacerlo de forma insegura sin decirlo.
        QMessageBox::warning(
            this, QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_fromdir_enc_needs_daemon_001"),
                QStringLiteral("Crear un dataset cifrado requiere el agente activo en la "
                               "conexión de destino: es lo que permite enviar la frase de "
                               "cifrado por el canal seguro y no en la línea de órdenes."),
                QStringLiteral("Creating an encrypted dataset requires the agent running on "
                               "the destination connection: that is what allows sending the "
                               "passphrase over the secure channel instead of the command line."),
                QStringLiteral("创建加密数据集需要目标连接上的代理处于运行状态：只有这样才能通过"
                               "安全通道发送密码，而不是放在命令行中。")));
        return;
    }
    draft.rpcConnIdx = createArgv.isEmpty() ? -1 : ctx.connIdx;
    draft.rpcArgv = createArgv;
    draft.rpcSecret = createSecret;
    if (!queuePendingShellAction(draft, &errorText)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), errorText);
        return;
    }
    appLog(QStringLiteral("INFO"),
           QStringLiteral("[fromdir] encolado con paso previo RPC: conn=%1 argv=%2 cifrado=%3")
               .arg(draft.rpcConnIdx)
               .arg(draft.rpcArgv.size())
               .arg(draft.rpcSecret.isEmpty() ? QStringLiteral("no") : QStringLiteral("sí")));
    appLog(QStringLiteral("NORMAL"),
           QStringLiteral("Cambio pendiente añadido: %1  %2")
               .arg(pendingTransferScopeLabel(srcCtx, ctx), displayLabel));
    updateApplyPropsButtonState();
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
    if (!requireFeature(curr.connIdx, zfsmgr::caps::Feature::DirToDir)) {
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
    const ConnectionProfile profile = m_profiles[ctx.connIdx];
    // Única exclusión por plataforma que sobrevive al barrido, y con motivo: el daemon
    // de Windows no conoce --mutate-advanced-todir (responde "unknown command",
    // comprobado por RPC). La tabla de capacidades ya rechaza la acción antes de llegar
    // aquí; esto solo evita elegir un camino que no existe.
    const bool daemonReadApiOk =
        !isWindowsConnection(profile)
        && ctx.connIdx >= 0
        && ctx.connIdx < m_states.size()
        && m_states[ctx.connIdx].daemonInstalled
        && m_states[ctx.connIdx].daemonActive
        && m_states[ctx.connIdx].daemonApiVersion.trimmed() == agentversion::expectedApiVersion().trimmed();
    QString cmd;
    bool allowWindowsScript = false;
    if (!isWin) {
        if (!isWindowsConnection(profile)) {
            Q_UNUSED(daemonReadApiOk);
            // El directorio lo elige el usuario: como argv no puede truncarse aunque
            // contenga ';', '&' o '|', que es lo que rompía esta acción.
            const QStringList argv{QStringLiteral("--mutate-advanced-todir"), ds, localDir,
                                   deleteSourceDataset ? QStringLiteral("1") : QStringLiteral("0")};
            cmd = mwhelpers::agentShellCommand(profile, argv);
            executeDatasetAction(QStringLiteral("conncontent"),
                                 // Identificador interno, NO texto de interfaz: aguas abajo
                                 // se compara con "Hacia Dir" para decidir si la acción se
                                 // envía como trabajo del daemon y si publica progreso. Al
                                 // pasar aquí el texto traducido, en inglés valía "To Dir" y
                                 // las dos cosas se apagaban solas. Desglosar y Ensamblar ya
                                 // pasan el literal por este mismo motivo.
                                 QStringLiteral("Hacia Dir"),
                                 ctx,
                                 cmd,
                                 0,
                                 allowWindowsScript,
                                 {},
                                 true,
                                 {},
                                 argv);
            return;
        }
        cmd = QStringLiteral(
                  "set -e; "
                  "DATASET=%1; "
                  "DST_DIR=%2; "
                  "RSYNC_OPTS='-aHWS'; "
                  "RSYNC_PROGRESS='--info=progress2'; "
                  "rsync --help 2>/dev/null | grep -q -- '--info' || RSYNC_PROGRESS='--progress'; "
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
                  "echo \"[TODIR] rsync start\"; "
                  "rsync $RSYNC_OPTS $RSYNC_PROGRESS \"$TMP_MP\"/ \"$TMP_OUT\"/; "
                  "echo \"[TODIR] rsync done\"; "
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
                         QStringLiteral("Hacia Dir"),  // identificador interno; ver arriba
                         ctx,
                         cmd,
                         0,
                         allowWindowsScript);
}
