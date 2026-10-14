#include "mainwindow.h"
#include "agentversion.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "advanced.h"
#include "transfer.h"
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
                                     QStringLiteral("Select a dataset in Advanced.")));
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
                                     QStringLiteral("You must select a dataset (not a snapshot).")));
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
                           QStringLiteral("Create dataset from directory")));
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

    QLabel* pathLabel = new QLabel(trk(QStringLiteral("t_advdir_auto004"), QStringLiteral("Path"), QStringLiteral("Path")), formWidget);
    QLineEdit* pathEdit = new QLineEdit(formWidget);
    pathEdit->setText(ds + QStringLiteral("/new_dataset"));
    form->addWidget(pathLabel, row, 0);
    form->addWidget(pathEdit, row, 1, 1, 3);
    row++;

    QLabel* typeLabel = new QLabel(trk(QStringLiteral("t_advdir_auto005"), QStringLiteral("Tipo"), QStringLiteral("Type")), formWidget);
    QComboBox* typeCombo = new QComboBox(formWidget);
    typeCombo->addItem(QStringLiteral("filesystem"), QStringLiteral("filesystem"));
    typeCombo->setCurrentIndex(0);
    typeCombo->setEnabled(false);
    form->addWidget(typeLabel, row, 0);
    form->addWidget(typeCombo, row, 1);
    row++;

    QLabel* blocksizeLabel = new QLabel(trk(QStringLiteral("t_advdir_auto016"), QStringLiteral("Blocksize"), QStringLiteral("Blocksize")), formWidget);
    QLineEdit* blocksizeEdit = new QLineEdit(formWidget);
    form->addWidget(blocksizeLabel, row, 0);
    form->addWidget(blocksizeEdit, row, 1);
    row++;

    QWidget* optsWidget = new QWidget(formWidget);
    QHBoxLayout* optsLay = new QHBoxLayout(optsWidget);
    optsLay->setContentsMargins(0, 0, 0, 0);
    optsLay->setSpacing(12);
    QCheckBox* parentsChk = new QCheckBox(trk(QStringLiteral("t_advdir_auto017"), QStringLiteral("Crear padres (-p)"), QStringLiteral("Create parents (-p)")), optsWidget);
    parentsChk->setChecked(true);
    optsLay->addWidget(parentsChk);
    optsLay->addStretch(1);
    form->addWidget(optsWidget, row, 0, 1, 4);
    row++;

    QLabel* extraLabel = new QLabel(trk(QStringLiteral("t_advdir_auto018"), QStringLiteral("Argumentos extra"), QStringLiteral("Extra args")), formWidget);
    QLineEdit* extraEdit = new QLineEdit(formWidget);
    form->addWidget(extraLabel, row, 0);
    form->addWidget(extraEdit, row, 1, 1, 3);
    row++;

    QLabel* encPassLabel = new QLabel(trk(QStringLiteral("t_create_ds_encpass_001"),
                                          QStringLiteral("Passphrase cifrado"),
                                          QStringLiteral("Encryption passphrase")),
                                      formWidget);
    QLineEdit* encPassEdit = new QLineEdit(formWidget);
    encPassEdit->setEchoMode(QLineEdit::Password);
    form->addWidget(encPassLabel, row, 0);
    form->addWidget(encPassEdit, row, 1, 1, 3);
    row++;

    QLabel* encPass2Label = new QLabel(trk(QStringLiteral("t_create_ds_encpass_002"),
                                           QStringLiteral("Repetir passphrase"),
                                           QStringLiteral("Repeat passphrase")),
                                       formWidget);
    QLineEdit* encPass2Edit = new QLineEdit(formWidget);
    encPass2Edit->setEchoMode(QLineEdit::Password);
    form->addWidget(encPass2Label, row, 0);
    form->addWidget(encPass2Edit, row, 1, 1, 3);
    row++;

    root->addWidget(formWidget);

    QGroupBox* propsGroup = new QGroupBox(trk(QStringLiteral("t_advdir_auto019"), QStringLiteral("Propiedades"), QStringLiteral("Properties")), &dlg);
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
        // Desplegable y editable: los valores válidos son un conjunto cerrado de
        // esquemas —prompt, file://, http://, https://— pero file:// y las URL llevan
        // detrás una ruta que hay que escribir. Escrito a mano se acierta a la tercera:
        // en una prueba real salió "invalid keylocation" antes de dar con la forma.
        {QStringLiteral("keylocation"), QStringLiteral("combo"),
         {QString(), QStringLiteral("prompt"), QStringLiteral("file:///etc/zfs/key"),
          QStringLiteral("https://"), QStringLiteral("http://")}},
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
                                               "somewhere for the copy to have a destination.")));
        }
        propsGrid->addWidget(lbl, r, cBase);
        PropEditor editor;
        editor.name = spec.name;
        if (spec.kind == QStringLiteral("combo")) {
            QComboBox* cb = new QComboBox(propsContainer);
            cb->addItems(spec.values);
            if (spec.name == QStringLiteral("compression")
                || spec.name == QStringLiteral("keylocation")) {
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
            // Vacío cuenta como "prompt": es lo que hace ZFS por omisión cuando el
            // formato de clave es una frase. Exigirlo explícito escondía los campos de
            // la contraseña justo en la configuración más natural —cifrado + passphrase
            // y keylocation sin tocar—, así que no había dónde escribirla.
            && (keylocation.isEmpty() || keylocation == QStringLiteral("prompt"));
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
        if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
            errorMsg = QStringLiteral("invalid connection");
            return false;
        }
        // The shell's `ls zfsm://<conn>#path/<dir>`, keeping the directories. On Windows the
        // root lists the drives («C:»), which the tree writes as «C:\».
        const bool isWinRemote = isWindowsConnection(connIdx);
        QJsonObject r;
        QString err;
        if (!cliReadJson(connIdx, QStringLiteral("ls %1").arg(pathUrl(connIdx, basePath)), r, &err,
                         25000)
            || !err.isEmpty()) {
            errorMsg = err;
            return false;
        }
        children.clear();
        for (const QJsonValue& v : r.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("type")).toString() != QStringLiteral("directory")) {
                continue;
            }
            QString nombre = o.value(QStringLiteral("name")).toString().trimmed();
            if (nombre.isEmpty()) {
                continue;
            }
            if (isWinRemote && basePath.trimmed().isEmpty()
                && !nombre.endsWith(QLatin1Char('\\'))) {
                nombre += QLatin1Char('\\');
            }
            children.push_back(nombre);
        }
        children.sort(Qt::CaseInsensitive);
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
                        .arg(m_conns.profiles.value(connIdx).name, err),
                    QStringLiteral("Could not list directories for %1:\n%2")
                        .arg(m_conns.profiles.value(connIdx).name, err)
                        .arg(m_conns.profiles.value(connIdx).name, err)));
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
            // None_ de las dos hace falta aquí: la recogida ya se queda con el marcado
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
            QStringLiteral("Source directories by connection")),
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
                           "subdirectory named after it.")),
        dirsGroup);
    dirsHint->setWordWrap(true);
    dirsLay->addWidget(dirsHint);

    QTabWidget* dirTabsWidget = new QTabWidget(dirsGroup);
    dirsLay->addWidget(dirTabsWidget, 1);
    root->addWidget(dirsGroup, 1);

    for (int i = 0; i < m_conns.profiles.size(); ++i) {
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
        dirTabsWidget->addTab(page, m_conns.profiles[i].name);

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
                QStringLiteral("There are no active connections to select directories.")),
            dirsGroup);
        noConnLabel->setWordWrap(true);
        dirsLay->addWidget(noConnLabel);
    }
    if (tabIndexByConn.contains(ctx.connIdx)) {
        dirTabsWidget->setCurrentIndex(tabIndexByConn.value(ctx.connIdx));
    }

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(trk(QStringLiteral("t_advdir_auto020"), QStringLiteral("Cancelar"), QStringLiteral("Cancel")), QDialogButtonBox::RejectRole);
    QPushButton* createBtn = buttons->addButton(
        trk(QStringLiteral("t_advdir_auto021"), QStringLiteral("Crear"), QStringLiteral("Create")),
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
                                     QStringLiteral("Dataset path is required.")));
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
                                   "created but there is nowhere to write.")));
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
                                     QStringLiteral("You must check at least one source directory.")));
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
                                     QStringLiteral("You must check at least one source directory.")));
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
            // Vacío cuenta como "prompt": es lo que hace ZFS por omisión cuando el
            // formato de clave es una frase. Exigirlo explícito escondía los campos de
            // la contraseña justo en la configuración más natural —cifrado + passphrase
            // y keylocation sin tocar—, así que no había dónde escribirla.
            && (keylocation.isEmpty() || keylocation == QStringLiteral("prompt"));
        if (needsPromptPassphrase) {
            if (encPassEdit->text().isEmpty() || encPass2Edit->text().isEmpty()) {
                QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_create_ds_encpass_req_001"),
                                         QStringLiteral("Debe indicar y repetir la passphrase de cifrado."),
                                         QStringLiteral("You must enter and repeat the encryption passphrase.")));
                return;
            }
            if (encPassEdit->text() != encPass2Edit->text()) {
                QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_create_ds_encpass_req_002"),
                                         QStringLiteral("Las passphrases de cifrado no coinciden."),
                                         QStringLiteral("Encryption passphrases do not match.")));
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

    // First the dataset, with the shell's `create` on its parent; a passphrase it asks for is
    // the one typed in the dialog.
    QStringList crear{QStringLiteral("create"), cliQuote(opt.datasetPath)};
    if (opt.parents) {
        crear << QStringLiteral("-p");
    }
    if (!opt.blocksize.isEmpty()) {
        crear << QStringLiteral("-b") << cliQuote(opt.blocksize);
    }
    for (const QString& prop : std::as_const(opt.properties)) {
        crear << cliQuote(prop);
    }
    if (!opt.extraArgs.isEmpty()) {
        crear << opt.extraArgs;
    }
    crear << QStringLiteral("--on")
          << zfsmUrl(ctx.connIdx, ctx.datasetName.isEmpty() ? ctx.poolName : ctx.datasetName);
    m_cliPresetSecret = opt.encryptionPassphrase;
    const int rcCrear = runCliAndWait(crear.join(QLatin1Char(' ')), QStringLiteral("text")).rc;
    m_cliPresetSecret.reset();
    if (rcCrear != 0) {
        return;
    }
    // Then each directory into it with the shell's `fromdir`: one alone fills the root,
    // several go each into a subdirectory —the rule of commands::advanced—.
    std::vector<zfsmgr::commands::advanced::FromDirSource> origenes;
    QVector<QPair<int, QString>> conRuta;
    for (const auto& e : std::as_const(selectedSources)) {
        if (e.second.trimmed().isEmpty() || e.first < 0 || e.first >= m_conns.profiles.size()) {
            continue;
        }
        origenes.push_back({e.second.trimmed().toStdString(), cliConnName(e.first).toStdString(),
                            isWindowsConnection(e.first)});
        conRuta.push_back(e);
    }
    const std::vector<std::string> subs = zfsmgr::commands::advanced::destinationSubdirs(origenes);
    const QString destino = zfsmUrl(ctx.connIdx, opt.datasetPath);
    for (int n = 0; n < conRuta.size(); ++n) {
        QString linea = QStringLiteral("fromdir %1 --from %2")
                            .arg(cliQuote(conRuta[n].second.trimmed()), zfsmUrl(conRuta[n].first));
        const QString sub = n < static_cast<int>(subs.size())
                                ? QString::fromStdString(subs[static_cast<std::size_t>(n)])
                                : QString();
        if (!sub.isEmpty()) {
            linea += QStringLiteral(" --subdir ") + cliQuote(sub);
        }
        linea += QStringLiteral(" --on ") + destino;
        runCli(linea, [this, ctx](const CliService::Result& r) {
            if (r.rc == 0 && m_refreshAfterCli) {
                refreshAllJobsAsync();
                refreshAfterCli(ctx);
            }
        });
    }
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
                                     QStringLiteral("Select a dataset in Advanced.")));
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
                                     QStringLiteral("You must select a dataset (not a snapshot).")));
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
                           QStringLiteral("Export dataset to directory")));
    dlg.setModal(true);
    dlg.resize(720, 180);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QLabel* intro = new QLabel(
        trk(QStringLiteral("t_advdir_auto028"), QStringLiteral("Se copiará el contenido de %1 a un directorio local. Puede elegir si destruir el dataset fuente.")
                .arg(ds),
            QStringLiteral("Contents of %1 will be copied to a local directory. You can choose whether to destroy the source dataset.")
                .arg(ds)
                .arg(ds)),
        &dlg);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QHBoxLayout* dirRow = new QHBoxLayout();
    QLabel* dirLabel = new QLabel(trk(QStringLiteral("t_advdir_auto029"), QStringLiteral("Directorio local"),
                                      QStringLiteral("Local directory")),
                                  &dlg);
    QLineEdit* dirEdit = new QLineEdit(&dlg);
    QPushButton* browseBtn = new QPushButton(
        trk(QStringLiteral("t_advdir_auto030"), QStringLiteral("Seleccionar..."), QStringLiteral("Select...")),
        &dlg);
    dirRow->addWidget(dirLabel, 0);
    dirRow->addWidget(dirEdit, 1);
    dirRow->addWidget(browseBtn, 0);
    root->addLayout(dirRow);

    QCheckBox* deleteSourceDatasetChk = new QCheckBox(
        trk(QStringLiteral("t_advdir_del_ds001"),
            QStringLiteral("Borrar dataset fuente tras copiar"),
            QStringLiteral("Delete source dataset after copy")),
        &dlg);
    root->addWidget(deleteSourceDatasetChk);

    // Se propone el punto de montaje actual del dataset: convertirlo en un directorio
    // EN SU MISMO SITIO es el caso principal —de hecho el daemon lo reubica a un
    // mountpoint temporal precisamente para que el destino pueda ser esa ruta—.
    //
    // Y con ese destino, "borrar el dataset" viene marcado: si no se borra, los mismos
    // datos quedan dos veces, una en el directorio nuevo y otra en el dataset que
    // sobrevive desmontado. Para cualquier otro destino se desmarca sola, porque
    // entonces es una copia a otro sitio y borrar el origen es otra decisión.
    // Re-edición: la semilla trae el directorio elegido y si se pidió borrar el origen.
    bool seededFromEdit = false;

    QString currentMp;
    if (getDatasetProperty(ctx.connIdx, ds, QStringLiteral("mountpoint"), currentMp)) {
        currentMp = currentMp.trimmed();
        if (currentMp.startsWith(QLatin1Char('/')) && dirEdit->text().trimmed().isEmpty()) {
            dirEdit->setText(currentMp);
        } else {
            currentMp.clear();
        }
    } else {
        currentMp.clear();
    }
    if (!seededFromEdit) {
        deleteSourceDatasetChk->setChecked(!currentMp.isEmpty());
    }
    QObject::connect(dirEdit, &QLineEdit::textChanged, &dlg,
                     [deleteSourceDatasetChk, currentMp](const QString& text) {
        if (currentMp.isEmpty()) {
            return;
        }
        deleteSourceDatasetChk->setChecked(text.trimmed() == currentMp);
    });

    QObject::connect(browseBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString picked = QFileDialog::getExistingDirectory(
            &dlg,
            trk(QStringLiteral("t_advdir_auto031"), QStringLiteral("Seleccionar directorio local"),
                QStringLiteral("Select local directory")),
            dirEdit->text().trimmed());
        if (!picked.trimmed().isEmpty()) {
            dirEdit->setText(picked);
        }
    });

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(trk(QStringLiteral("t_advdir_auto032"), QStringLiteral("Cancelar"), QStringLiteral("Cancel")), QDialogButtonBox::RejectRole);
    QPushButton* acceptBtn = buttons->addButton(trk(QStringLiteral("t_advdir_auto033"), QStringLiteral("Aceptar"), QStringLiteral("Accept")), QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(acceptBtn, &QPushButton::clicked, &dlg, [&]() {
        if (dirEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_advdir_auto034"), QStringLiteral("Debe seleccionar un directorio local."),
                                     QStringLiteral("You must select a local directory.")));
            return;
        }
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    // The shell's `todir`, as a job; it asks before destroying the source.
    QString linea = QStringLiteral("todir %1").arg(cliQuote(dirEdit->text().trimmed()));
    if (deleteSourceDatasetChk->isChecked()) {
        linea += QStringLiteral(" --delete-source");
    }
    linea += QStringLiteral(" --on ") + zfsmUrl(ctx.connIdx, ds);
    runCli(linea, [this, ctx](const CliService::Result& r) {
        if (r.rc == 0 && m_refreshAfterCli) {
            refreshAllJobsAsync();
            refreshAfterCli(ctx);
        }
    });
}
