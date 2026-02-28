#include "mainwindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTabBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QStackedWidget>
#include <QSet>
#include <algorithm>
#include <QVBoxLayout>
#include <QWidget>
#include <QComboBox>
#include <QTextStream>
#include <QApplication>
#include <QClipboard>
#include <QMetaObject>
#include <QRegularExpression>
#include <QFontMetrics>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSettings>
#include <functional>

#include <QtConcurrent/QtConcurrent>

namespace {

QString tsNow() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

QString oneLine(const QString& v) {
    QString x = v.simplified();
    return x.left(220);
}

QString parseOpenZfsVersionText(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    const QString lower = text.toLower();
    const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("\\bzfs(?:-kmod)?[-\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
        QRegularExpression(QStringLiteral("\\bopenzfs(?:[-\\s]+version)?[:\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
    };
    for (const QRegularExpression& rx : patterns) {
        const QRegularExpressionMatch m = rx.match(lower);
        if (m.hasMatch()) {
            const QString ver = m.captured(1);
            const int major = ver.section('.', 0, 0).toInt();
            if (major <= 10) {
                return ver;
            }
        }
    }
    return QString();
}

bool isUserProperty(const QString& prop) {
    return prop.contains(':');
}

bool isDatasetPropertyEditable(const QString& propName, const QString& datasetType, const QString& source, const QString& readonly) {
    const QString prop = propName.trimmed().toLower();
    const QString dsType = datasetType.trimmed().toLower();
    const QString src = source.trimmed();
    const QString ro = readonly.trimmed().toLower();
    if (prop.isEmpty()) {
        return false;
    }
    if (ro == QStringLiteral("true") || ro == QStringLiteral("on") || ro == QStringLiteral("yes") || ro == QStringLiteral("1")) {
        return false;
    }
    if (src == QStringLiteral("-")) {
        return false;
    }
    if (isUserProperty(prop)) {
        return true;
    }

    static const QSet<QString> common = {
        QStringLiteral("atime"), QStringLiteral("relatime"), QStringLiteral("readonly"), QStringLiteral("compression"),
        QStringLiteral("checksum"), QStringLiteral("sync"), QStringLiteral("logbias"), QStringLiteral("primarycache"),
        QStringLiteral("secondarycache"), QStringLiteral("dedup"), QStringLiteral("copies"), QStringLiteral("acltype"),
        QStringLiteral("aclinherit"), QStringLiteral("xattr"), QStringLiteral("normalization"),
        QStringLiteral("casesensitivity"), QStringLiteral("utf8only"), QStringLiteral("keylocation"), QStringLiteral("comment")
    };
    static const QSet<QString> fs = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("mountpoint"), QStringLiteral("canmount"), QStringLiteral("recordsize"), QStringLiteral("quota"),
            QStringLiteral("reservation"), QStringLiteral("refquota"), QStringLiteral("refreservation"),
            QStringLiteral("snapdir"), QStringLiteral("exec"), QStringLiteral("setuid"), QStringLiteral("devices")
        });
        return s;
    }();
    static const QSet<QString> vol = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("volsize"), QStringLiteral("volblocksize"), QStringLiteral("reservation"),
            QStringLiteral("refreservation"), QStringLiteral("snapdev"), QStringLiteral("volmode")
        });
        return s;
    }();

    if (dsType == QStringLiteral("filesystem")) {
        return fs.contains(prop);
    }
    if (dsType == QStringLiteral("volume")) {
        return vol.contains(prop);
    }
    if (dsType == QStringLiteral("snapshot")) {
        return false;
    }
    return fs.contains(prop) || vol.contains(prop);
}

bool isMountedValueTrue(const QString& value) {
    const QString v = value.trimmed().toLower();
    return v == QStringLiteral("yes") || v == QStringLiteral("on") || v == QStringLiteral("true") || v == QStringLiteral("1");
}

QString parentDatasetName(const QString& dataset) {
    const int slash = dataset.lastIndexOf('/');
    if (slash <= 0) {
        return QString();
    }
    return dataset.left(slash);
}

QString shSingleQuote(const QString& s) {
    QString out = s;
    out.replace('\'', "'\"'\"'");
    return QStringLiteral("'") + out + QStringLiteral("'");
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

QString formatCommandPreview(const QString& input) {
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

    QString pretty = body;
    pretty.replace(QStringLiteral(" && "), QStringLiteral(" &&\n  "));
    pretty.replace(QStringLiteral(" || "), QStringLiteral(" ||\n  "));
    pretty.replace(QStringLiteral(" | "), QStringLiteral(" |\n  "));
    pretty.replace(QStringLiteral("; "), QStringLiteral(";\n"));

    if (!header.isEmpty()) {
        return header + QStringLiteral("\n  ") + pretty;
    }
    return pretty;
}

} // namespace

MainWindow::MainWindow(const QString& masterPassword, const QString& language, QWidget* parent)
    : QMainWindow(parent)
    , m_store(QStringLiteral("ZFSMgr")) {
    m_language = language.trimmed().toLower();
    if (m_language.isEmpty()) {
        m_language = QStringLiteral("es");
    }
    loadUiSettings();
    if (!language.trimmed().isEmpty()) {
        m_language = language.trimmed().toLower();
        saveUiSettings();
    }
    m_store.setMasterPassword(masterPassword);
    initLogPersistence();
    buildUi();
    loadConnections();
    QTimer::singleShot(0, this, [this]() {
        refreshAllConnections();
    });
}

QString MainWindow::tr3(const QString& es, const QString& en, const QString& zh) const {
    if (m_language == QStringLiteral("en")) return en;
    if (m_language == QStringLiteral("zh")) return zh;
    return es;
}

void MainWindow::loadUiSettings() {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("app"));
    const QString lang = ini.value(QStringLiteral("language"), m_language).toString().trimmed().toLower();
    if (!lang.isEmpty()) {
        m_language = lang;
    }
    m_actionConfirmEnabled = ini.value(QStringLiteral("confirm_actions"), true).toBool();
    ini.endGroup();
}

void MainWindow::saveUiSettings() const {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("app"));
    ini.setValue(QStringLiteral("language"), m_language);
    ini.setValue(QStringLiteral("confirm_actions"), m_actionConfirmEnabled);
    ini.endGroup();
    ini.sync();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("ZFSMgr (C++/Qt)"));
    resize(1200, 736);
    setMinimumSize(1120, 736);
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #f3f7fb; color: #14212b; }"
        "QTabWidget::pane { border: 1px solid #b8c7d6; background: #f8fbff; }"
        "QTabBar { background: #f3f7fb; }"
        "QTabBar::scroller { background: #f3f7fb; }"
        "QTabBar QToolButton { background: #f3f7fb; border: 1px solid #b8c7d6; color: #14212b; }"
        "QTabBar::tab { padding: 3px 10px; min-height: 18px; background: #e6edf4; border: 1px solid #b8c7d6; border-bottom: none; }"
        "QTabBar::tab:selected { font-weight: 700; min-height: 24px; background: #cfe5ff; color: #0b2f4f; border: 1px solid #6ea6dd; }"
        "QTabBar::tab:!selected { margin-top: 4px; background: #e6edf4; }"
        "QGroupBox { margin-top: 12px; border: 1px solid #b8c7d6; border-radius: 4px; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 8px; padding: 0 4px 0 4px; }"
        "QPushButton { background: #e8eff5; border: 1px solid #9db0c4; border-radius: 4px; padding: 3px 8px; }"
        "QPushButton:hover { background: #d6e6f2; }"
        "QPushButton:pressed { background: #c4d8e8; }"
        "QPushButton:disabled { background: #edf1f5; color: #8c99a6; border: 1px solid #c8d2dc; }"
        "QMenu { background: #ffffff; border: 1px solid #9db0c4; padding: 3px; }"
        "QMenu::item { padding: 4px 14px; color: #102233; }"
        "QMenu::item:selected { background: #cfe5ff; color: #0b2f4f; }"
        "QMenu::item:disabled { color: #8f9aa5; background: #f4f6f8; }"
        "QListWidget, QTreeWidget, QTableWidget, QPlainTextEdit, QTextEdit, QComboBox { background: #ffffff; color: #102233; }"
        "QComboBox QAbstractItemView { background: #ffffff; color: #102233; }"
        "QScrollBar:vertical { width: 8px; }"
        "QScrollBar:horizontal { height: 8px; }"
        "QTreeWidget::item:selected, QTableWidget::item:selected, QListWidget::item:selected {"
        "  background: #dcecff; color: #0d2438; font-weight: 600; }"
        "QHeaderView::section { background: #eaf1f7; border: 1px solid #c5d3e0; padding: 2px 4px; }"));

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* topArea = new QWidget(central);
    auto* topLayout = new QHBoxLayout(topArea);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(6);

    auto* leftPane = new QWidget(topArea);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
    m_leftTabs = new QTabWidget(leftPane);
    m_leftTabs->setDocumentMode(false);
    m_leftTabs->setTabPosition(QTabWidget::North);
    // Anchura fija basada en el texto real de botones para evitar solapes en macOS.
    const QFontMetrics fm(font());
    const int btnTextWidth = qMax(
        fm.horizontalAdvance(tr3(QStringLiteral("Refrescar todo"), QStringLiteral("Refresh all"), QStringLiteral("全部刷新"))),
        fm.horizontalAdvance(tr3(QStringLiteral("Configuración"), QStringLiteral("Configuration"), QStringLiteral("配置"))));
    const int leftBaseWidth = qMax(340, btnTextWidth + 190);
    const int leftFixedWidth = qMax(220, static_cast<int>(leftBaseWidth * 0.85 * 1.15 * 1.10 * 1.25));
    leftPane->setMinimumWidth(leftFixedWidth);
    leftPane->setMaximumWidth(leftFixedWidth);

    auto* connectionsTab = new QWidget(m_leftTabs);
    auto* connLayout = new QVBoxLayout(connectionsTab);
    connLayout->setContentsMargins(4, 4, 4, 4);
    connLayout->setSpacing(4);
    auto* connButtonsBox = new QGroupBox(
        tr3(QStringLiteral("Conexiones"), QStringLiteral("Connections"), QStringLiteral("连接")), connectionsTab);
    auto* connButtonsBoxLayout = new QVBoxLayout(connButtonsBox);
    connButtonsBoxLayout->setContentsMargins(8, 20, 8, 8);
    auto* connButtons = new QHBoxLayout();
    connButtons->setSpacing(8);
    m_btnNew = new QPushButton(tr3(QStringLiteral("Nueva"), QStringLiteral("New"), QStringLiteral("新建")), connectionsTab);
    m_btnRefreshAll = new QPushButton(tr3(QStringLiteral("Refrescar todo"), QStringLiteral("Refresh all"), QStringLiteral("全部刷新")), connectionsTab);
    m_btnConfig = new QPushButton(tr3(QStringLiteral("Configuración"), QStringLiteral("Configuration"), QStringLiteral("配置")), connectionsTab);
    m_btnNew->setMinimumHeight(34);
    m_btnRefreshAll->setMinimumHeight(34);
    m_btnConfig->setMinimumHeight(34);
    m_btnNew->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnRefreshAll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConfig->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConfig->setVisible(true);
    connButtons->addWidget(m_btnNew);
    connButtons->addWidget(m_btnRefreshAll);
    connButtons->addWidget(m_btnConfig);
    connButtonsBoxLayout->addLayout(connButtons);
    connLayout->addWidget(connButtonsBox, 0);

    auto* connListBox = new QGroupBox(
        tr3(QStringLiteral("Listado"), QStringLiteral("List"), QStringLiteral("列表")), connectionsTab);
    auto* connListBoxLayout = new QVBoxLayout(connListBox);
    connListBoxLayout->setContentsMargins(8, 20, 8, 8);
    m_connectionsList = new QListWidget(connListBox);
    m_connectionsList->setAlternatingRowColors(true);
    m_connectionsList->setUniformItemSizes(true);
    m_connectionsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_connectionsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connListBoxLayout->addWidget(m_connectionsList, 1);
    connLayout->addWidget(connListBox, 1);
    connectionsTab->setLayout(connLayout);

    auto* datasetsTab = new QWidget(m_leftTabs);
    auto* dsLeftTabLayout = new QVBoxLayout(datasetsTab);
    dsLeftTabLayout->setContentsMargins(4, 4, 4, 4);
    dsLeftTabLayout->setSpacing(4);
    auto* transferBox = new QGroupBox(tr3(QStringLiteral("Origen-->Destino"), QStringLiteral("Source-->Target"), QStringLiteral("源-->目标")), datasetsTab);
    auto* transferLayout = new QVBoxLayout(transferBox);
    m_transferOriginLabel = new QLabel(QStringLiteral("Origen: Dataset (seleccione)"), transferBox);
    m_transferDestLabel = new QLabel(QStringLiteral("Destino: Dataset (seleccione)"), transferBox);
    m_transferOriginLabel->setWordWrap(true);
    m_transferDestLabel->setWordWrap(true);
    m_transferOriginLabel->setMinimumHeight(34);
    m_transferDestLabel->setMinimumHeight(34);
    m_transferOriginLabel->hide();
    m_transferDestLabel->hide();
    m_btnCopy = new QPushButton(tr3(QStringLiteral("Copiar"), QStringLiteral("Copy"), QStringLiteral("复制")), transferBox);
    m_btnLevel = new QPushButton(tr3(QStringLiteral("Nivelar"), QStringLiteral("Level"), QStringLiteral("同步快照")), transferBox);
    m_btnSync = new QPushButton(tr3(QStringLiteral("Sincronizar"), QStringLiteral("Sync"), QStringLiteral("同步文件")), transferBox);
    m_btnCopy->setToolTip(
        tr3(QStringLiteral("Envía un snapshot desde Origen a Destino mediante send/recv.\n"
                           "Requiere: snapshot seleccionado en Origen y dataset seleccionado en Destino."),
            QStringLiteral("Send one snapshot from Source to Target using send/recv.\n"
                           "Requires: snapshot selected in Source and dataset selected in Target."),
            QStringLiteral("通过 send/recv 将源端快照发送到目标端。\n"
                           "条件：源端选择快照，目标端选择数据集。")));
    m_btnLevel->setToolTip(
        tr3(QStringLiteral("Genera/aplica envío diferencial para igualar Origen->Destino.\n"
                           "Requiere: dataset o snapshot seleccionado en Origen y dataset en Destino."),
            QStringLiteral("Build/apply differential transfer to level Source->Target.\n"
                           "Requires: dataset or snapshot selected in Source and dataset in Target."),
            QStringLiteral("生成/应用差异传输以对齐源端到目标端。\n"
                           "条件：源端选择数据集或快照，目标端选择数据集。")));
    m_btnSync->setToolTip(
        tr3(QStringLiteral("Sincroniza contenido de dataset Origen a Destino con rsync.\n"
                           "Requiere: dataset seleccionado (no snapshot) en Origen y Destino."),
            QStringLiteral("Sync dataset contents from Source to Target with rsync.\n"
                           "Requires: dataset selected (not snapshot) in Source and Target."),
            QStringLiteral("使用 rsync 同步源端到目标端的数据集内容。\n"
                           "条件：源端和目标端都选择数据集（非快照）。")));
    m_btnCopy->setEnabled(false);
    m_btnLevel->setEnabled(false);
    m_btnSync->setEnabled(false);
    auto* transferButtonsRow = new QHBoxLayout();
    transferButtonsRow->setSpacing(8);
    transferButtonsRow->addWidget(m_btnCopy);
    transferButtonsRow->addWidget(m_btnLevel);
    transferButtonsRow->addWidget(m_btnSync);
    transferLayout->addLayout(transferButtonsRow);
    dsLeftTabLayout->addWidget(transferBox);
    auto* datasetsInfoTabs = new QTabWidget(datasetsTab);
    datasetsInfoTabs->setDocumentMode(false);
    auto* mountedLeftTab = new QWidget(datasetsInfoTabs);
    auto* mountedLeftLayout = new QVBoxLayout(mountedLeftTab);
    m_mountedDatasetsTableLeft = new QTableWidget(mountedLeftTab);
    m_mountedDatasetsTableLeft->setColumnCount(2);
    m_mountedDatasetsTableLeft->setHorizontalHeaderLabels(
        {tr3(QStringLiteral("Dataset"), QStringLiteral("Dataset"), QStringLiteral("数据集")),
         QStringLiteral("mountpoint")});
    m_mountedDatasetsTableLeft->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_mountedDatasetsTableLeft->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_mountedDatasetsTableLeft->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mountedDatasetsTableLeft->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mountedDatasetsTableLeft->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mountedDatasetsTableLeft->verticalHeader()->setVisible(false);
    m_mountedDatasetsTableLeft->verticalHeader()->setDefaultSectionSize(22);
    m_mountedDatasetsTableLeft->setWordWrap(false);
    m_mountedDatasetsTableLeft->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_mountedDatasetsTableLeft->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    {
        QFont f = m_mountedDatasetsTableLeft->font();
        f.setPointSize(qMax(6, f.pointSize() - 2));
        m_mountedDatasetsTableLeft->setFont(f);
        QFont hf = f;
        hf.setPointSize(f.pointSize());
        hf.setBold(false);
        m_mountedDatasetsTableLeft->horizontalHeader()->setFont(hf);
    }
    m_mountedDatasetsTableLeft->setStyleSheet(
        QStringLiteral("QScrollBar:vertical{width:8px;} "
                       "QScrollBar:horizontal{height:8px;}"));
    m_mountedDatasetsTableLeft->setColumnWidth(0, 180);
    m_mountedDatasetsTableLeft->setColumnWidth(1, 220);
    mountedLeftLayout->addWidget(m_mountedDatasetsTableLeft, 1);
    auto* propsLeftTab = new QWidget(datasetsInfoTabs);
    auto* propsLeftLayout = new QVBoxLayout(propsLeftTab);
    auto* propsLeftBox = new QGroupBox(
        tr3(QStringLiteral("Propiedades del dataset"),
            QStringLiteral("Dataset properties"),
            QStringLiteral("数据集属性")),
        propsLeftTab);
    auto* propsLeftBoxLayout = new QVBoxLayout(propsLeftBox);
    propsLeftBoxLayout->setContentsMargins(8, 20, 8, 8);
    m_datasetPropsTable = new QTableWidget(propsLeftBox);
    m_datasetPropsTable->setColumnCount(3);
    m_datasetPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Inherit")});
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_datasetPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_datasetPropsTable->verticalHeader()->setVisible(false);
    m_datasetPropsTable->verticalHeader()->setDefaultSectionSize(22);
    m_datasetPropsTable->setFont(m_mountedDatasetsTableLeft->font());
    {
        QFont hf = m_datasetPropsTable->font();
        hf.setBold(false);
        m_datasetPropsTable->horizontalHeader()->setFont(hf);
    }
    m_btnApplyDatasetProps = new QPushButton(
        tr3(QStringLiteral("Aplicar cambios"), QStringLiteral("Apply changes"), QStringLiteral("应用更改")),
        propsLeftBox);
    m_btnApplyDatasetProps->setEnabled(false);
    propsLeftBoxLayout->addWidget(m_datasetPropsTable, 1);
    propsLeftBoxLayout->addWidget(m_btnApplyDatasetProps, 0, Qt::AlignRight);
    propsLeftLayout->addWidget(propsLeftBox, 1);
    datasetsInfoTabs->addTab(
        propsLeftTab,
        tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")));
    datasetsInfoTabs->addTab(
        mountedLeftTab,
        tr3(QStringLiteral("Montados"), QStringLiteral("Mounted"), QStringLiteral("已挂载")));
    dsLeftTabLayout->addWidget(datasetsInfoTabs, 1);
    datasetsTab->setLayout(dsLeftTabLayout);

    auto* advancedTab = new QWidget(m_leftTabs);
    auto* advLeftTabLayout = new QVBoxLayout(advancedTab);
    advLeftTabLayout->setContentsMargins(4, 4, 4, 4);
    advLeftTabLayout->setSpacing(4);
    auto* commandsBox = new QGroupBox(tr3(QStringLiteral("Comandos"), QStringLiteral("Commands"), QStringLiteral("命令")), advancedTab);
    auto* commandsLayout = new QVBoxLayout(commandsBox);
    commandsLayout->setSpacing(10);
    m_btnAdvancedBreakdown = new QPushButton(tr3(QStringLiteral("Desglosar"), QStringLiteral("Break down"), QStringLiteral("拆分")), commandsBox);
    m_btnAdvancedAssemble = new QPushButton(tr3(QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("组装")), commandsBox);
    m_btnAdvancedBreakdown->setToolTip(
        tr3(QStringLiteral("Construye datasets a partir de directorios. "
                           "Requiere dataset y descendientes montados. "
                           "Permite seleccionar directorios a desglosar. "
                           "No se ejecuta si hay conflictos de mountpoint."),
            QStringLiteral("Builds datasets from directories. "
                           "Requires dataset and descendants mounted. "
                           "Lets you select directories to split. "
                           "Will not run if mountpoint conflicts exist."),
            QStringLiteral("从目录构建数据集。"
                           "要求数据集及其后代已挂载。"
                           "可选择要拆分的目录。"
                           "若存在挂载点冲突则不会执行。")));
    m_btnAdvancedAssemble->setToolTip(
        tr3(QStringLiteral("Convierte datasets en directorios. "
                           "Requiere dataset y descendientes montados. "
                           "Permite seleccionar subdatasets a ensamblar. "
                           "zfs destroy solo se ejecuta si rsync finaliza OK."),
            QStringLiteral("Converts datasets into directories. "
                           "Requires dataset and descendants mounted. "
                           "Lets you select child datasets to assemble. "
                           "zfs destroy runs only if rsync succeeds."),
            QStringLiteral("将数据集转换为目录。"
                           "要求数据集及其后代已挂载。"
                           "可选择要组装的子数据集。"
                           "仅当 rsync 成功时才执行 zfs destroy。")));
    const int transferBtnH = m_btnCopy ? m_btnCopy->sizeHint().height() : m_btnAdvancedBreakdown->sizeHint().height();
    m_btnAdvancedBreakdown->setFixedHeight(transferBtnH);
    m_btnAdvancedAssemble->setFixedHeight(transferBtnH);
    m_btnAdvancedBreakdown->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedAssemble->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedBreakdown->setEnabled(false);
    m_btnAdvancedAssemble->setEnabled(false);
    auto* commandsButtonsRow = new QHBoxLayout();
    commandsButtonsRow->setSpacing(8);
    commandsButtonsRow->addWidget(m_btnAdvancedBreakdown);
    commandsButtonsRow->addWidget(m_btnAdvancedAssemble);
    commandsLayout->addLayout(commandsButtonsRow);
    auto* advancedInfoTabs = new QTabWidget(advancedTab);
    advancedInfoTabs->setDocumentMode(false);
    auto* mountedAdvTab = new QWidget(advancedInfoTabs);
    auto* mountedAdvLayout = new QVBoxLayout(mountedAdvTab);
    m_mountedDatasetsTableAdv = new QTableWidget(mountedAdvTab);
    m_mountedDatasetsTableAdv->setColumnCount(2);
    m_mountedDatasetsTableAdv->setHorizontalHeaderLabels(
        {tr3(QStringLiteral("Dataset"), QStringLiteral("Dataset"), QStringLiteral("数据集")),
         QStringLiteral("mountpoint")});
    m_mountedDatasetsTableAdv->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_mountedDatasetsTableAdv->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_mountedDatasetsTableAdv->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mountedDatasetsTableAdv->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mountedDatasetsTableAdv->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mountedDatasetsTableAdv->verticalHeader()->setVisible(false);
    m_mountedDatasetsTableAdv->verticalHeader()->setDefaultSectionSize(22);
    m_mountedDatasetsTableAdv->setWordWrap(false);
    m_mountedDatasetsTableAdv->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_mountedDatasetsTableAdv->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    {
        QFont f = m_mountedDatasetsTableAdv->font();
        f.setPointSize(qMax(6, f.pointSize() - 2));
        m_mountedDatasetsTableAdv->setFont(f);
        QFont hf = f;
        hf.setPointSize(f.pointSize());
        hf.setBold(false);
        m_mountedDatasetsTableAdv->horizontalHeader()->setFont(hf);
    }
    m_mountedDatasetsTableAdv->setStyleSheet(
        QStringLiteral("QScrollBar:vertical{width:8px;} "
                       "QScrollBar:horizontal{height:8px;}"));
    m_mountedDatasetsTableAdv->setColumnWidth(0, 180);
    m_mountedDatasetsTableAdv->setColumnWidth(1, 220);
    mountedAdvLayout->addWidget(m_mountedDatasetsTableAdv, 1);
    auto* propsAdvTab = new QWidget(advancedInfoTabs);
    auto* propsAdvLayout = new QVBoxLayout(propsAdvTab);
    auto* propsAdvBox = new QGroupBox(
        tr3(QStringLiteral("Propiedades del dataset"),
            QStringLiteral("Dataset properties"),
            QStringLiteral("数据集属性")),
        propsAdvTab);
    auto* propsAdvBoxLayout = new QVBoxLayout(propsAdvBox);
    propsAdvBoxLayout->setContentsMargins(8, 20, 8, 8);
    m_advPropsTable = new QTableWidget(propsAdvBox);
    m_advPropsTable->setColumnCount(3);
    m_advPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Inherit")});
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_advPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_advPropsTable->verticalHeader()->setVisible(false);
    m_advPropsTable->verticalHeader()->setDefaultSectionSize(22);
    m_advPropsTable->setFont(m_mountedDatasetsTableAdv->font());
    {
        QFont hf = m_advPropsTable->font();
        hf.setBold(false);
        m_advPropsTable->horizontalHeader()->setFont(hf);
    }
    m_btnApplyAdvancedProps = new QPushButton(
        tr3(QStringLiteral("Aplicar cambios"), QStringLiteral("Apply changes"), QStringLiteral("应用更改")),
        propsAdvBox);
    m_btnApplyAdvancedProps->setEnabled(false);
    propsAdvBoxLayout->addWidget(m_advPropsTable, 1);
    propsAdvBoxLayout->addWidget(m_btnApplyAdvancedProps, 0, Qt::AlignRight);
    propsAdvLayout->addWidget(propsAdvBox, 1);
    advancedInfoTabs->addTab(
        propsAdvTab,
        tr3(QStringLiteral("Propiedades"), QStringLiteral("Properties"), QStringLiteral("属性")));
    advancedInfoTabs->addTab(
        mountedAdvTab,
        tr3(QStringLiteral("Montados"), QStringLiteral("Mounted"), QStringLiteral("已挂载")));
    advLeftTabLayout->setSpacing(8);
    advLeftTabLayout->addWidget(commandsBox);
    advLeftTabLayout->addWidget(advancedInfoTabs, 1);
    advancedTab->setLayout(advLeftTabLayout);

    m_leftTabs->addTab(connectionsTab, tr3(QStringLiteral("Conexiones"), QStringLiteral("Connections"), QStringLiteral("连接")));
    m_leftTabs->addTab(datasetsTab, tr3(QStringLiteral("Datasets"), QStringLiteral("Datasets"), QStringLiteral("数据集")));
    m_leftTabs->addTab(advancedTab, tr3(QStringLiteral("Avanzado"), QStringLiteral("Advanced"), QStringLiteral("高级")));
    leftLayout->addWidget(m_leftTabs, 1);

    auto* rightPane = new QWidget(topArea);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    m_rightStack = new QStackedWidget(rightPane);

    auto* rightConnectionsPage = new QWidget(m_rightStack);
    auto* rightConnectionsLayout = new QVBoxLayout(rightConnectionsPage);
    rightConnectionsLayout->setContentsMargins(0, 0, 0, 0);
    rightConnectionsLayout->setSpacing(4);
    m_rightTabs = new QTabWidget(rightConnectionsPage);
    m_rightTabs->setDocumentMode(false);

    auto* importedTab = new QWidget(m_rightTabs);
    auto* importedLayout = new QVBoxLayout(importedTab);
    m_importedPoolsTable = new QTableWidget(importedTab);
    m_importedPoolsTable->setColumnCount(3);
    m_importedPoolsTable->setHorizontalHeaderLabels({QStringLiteral("Conexión"), QStringLiteral("Pool"), QStringLiteral("Acción")});
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importedPoolsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_importedPoolsTable->setContextMenuPolicy(Qt::NoContextMenu);
    m_importedPoolsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_importedPoolsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_importedPoolsTable->verticalHeader()->setVisible(false);
    {
        QFont f = m_importedPoolsTable->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_importedPoolsTable->setFont(f);
    }
    m_importedPoolsTable->verticalHeader()->setDefaultSectionSize(22);
    m_importedPoolsTable->setStyleSheet(QStringLiteral("QTableWidget::item{padding:1px 3px;}"));
    importedLayout->addWidget(m_importedPoolsTable, 1);

    auto* importableTab = new QWidget(m_rightTabs);
    auto* importableLayout = new QVBoxLayout(importableTab);
    m_importablePoolsTable = new QTableWidget(importableTab);
    m_importablePoolsTable->setColumnCount(5);
    m_importablePoolsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Acción"), QStringLiteral("Conexión"), QStringLiteral("Pool"), QStringLiteral("Estado"), QStringLiteral("Motivo")});
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_importablePoolsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_importablePoolsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_importablePoolsTable->setContextMenuPolicy(Qt::NoContextMenu);
    m_importablePoolsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_importablePoolsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_importablePoolsTable->verticalHeader()->setVisible(false);
    {
        QFont f = m_importablePoolsTable->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_importablePoolsTable->setFont(f);
    }
    m_importablePoolsTable->verticalHeader()->setDefaultSectionSize(22);
    m_importablePoolsTable->setStyleSheet(QStringLiteral("QTableWidget::item{padding:1px 3px;}"));
    importableLayout->addWidget(m_importablePoolsTable, 1);

    m_rightTabs->addTab(importedTab, tr3(QStringLiteral("Pools importados"), QStringLiteral("Imported pools"), QStringLiteral("已导入池")));
    m_rightTabs->addTab(importableTab, tr3(QStringLiteral("Pools importables"), QStringLiteral("Importable pools"), QStringLiteral("可导入池")));

    m_poolDetailTabs = new QTabWidget(rightConnectionsPage);
    m_poolDetailTabs->setDocumentMode(false);
    auto* propsPoolTab = new QWidget(m_poolDetailTabs);
    auto* propsPoolLayout = new QVBoxLayout(propsPoolTab);
    m_poolPropsTable = new QTableWidget(propsPoolTab);
    m_poolPropsTable->setColumnCount(3);
    m_poolPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Origen")});
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_poolPropsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_poolPropsTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_poolPropsTable->verticalHeader()->setVisible(false);
    m_poolPropsTable->verticalHeader()->setDefaultSectionSize(22);
    propsPoolLayout->addWidget(m_poolPropsTable, 1);

    auto* statusPoolTab = new QWidget(m_poolDetailTabs);
    auto* statusPoolLayout = new QVBoxLayout(statusPoolTab);
    auto* statusBody = new QHBoxLayout();
    statusBody->setContentsMargins(0, 0, 0, 0);
    statusBody->setSpacing(6);
    auto* statusActions = new QVBoxLayout();
    statusActions->setContentsMargins(0, 0, 0, 0);
    statusActions->setSpacing(4);
    m_poolStatusRefreshBtn = new QPushButton(QStringLiteral("Actualizar"), statusPoolTab);
    statusActions->addWidget(m_poolStatusRefreshBtn, 0, Qt::AlignTop);
    statusActions->addStretch(1);
    m_poolStatusText = new QPlainTextEdit(statusPoolTab);
    m_poolStatusText->setReadOnly(true);
    {
        QFont mono = m_poolStatusText->font();
        mono.setFamily(QStringLiteral("Monospace"));
        mono.setPointSize(8);
        m_poolStatusText->setFont(mono);
    }
    statusBody->addLayout(statusActions, 0);
    statusBody->addWidget(m_poolStatusText, 1);
    statusPoolLayout->addLayout(statusBody, 1);

    m_poolDetailTabs->addTab(propsPoolTab, tr3(QStringLiteral("Propiedades del pool"), QStringLiteral("Pool properties"), QStringLiteral("存储池属性")));
    m_poolDetailTabs->addTab(statusPoolTab, tr3(QStringLiteral("Estado"), QStringLiteral("Status"), QStringLiteral("状态")));

    auto* connDetailSplit = new QSplitter(Qt::Vertical, rightConnectionsPage);
    connDetailSplit->setChildrenCollapsible(false);
    connDetailSplit->setHandleWidth(1);
    connDetailSplit->addWidget(m_rightTabs);
    connDetailSplit->addWidget(m_poolDetailTabs);
    connDetailSplit->setStretchFactor(0, 55);
    connDetailSplit->setStretchFactor(1, 45);
    rightConnectionsLayout->setSpacing(0);
    rightConnectionsLayout->addWidget(connDetailSplit, 1);

    auto* rightDatasetsPage = new QWidget(m_rightStack);
    auto* rightDatasetsLayout = new QVBoxLayout(rightDatasetsPage);
    rightDatasetsLayout->setContentsMargins(0, 0, 0, 0);
    rightDatasetsLayout->setSpacing(4);
    auto* dsLeft = new QWidget(rightDatasetsPage);
    auto* dsLeftLayout = new QVBoxLayout(dsLeft);
    dsLeftLayout->setContentsMargins(0, 0, 0, 0);
    dsLeftLayout->setSpacing(4);

    auto* originPane = new QWidget(dsLeft);
    auto* originLayout = new QVBoxLayout(originPane);
    originLayout->setContentsMargins(0, 0, 0, 0);
    originLayout->setSpacing(4);
    auto* originTop = new QHBoxLayout();
    originTop->setContentsMargins(0, 0, 0, 0);
    originTop->setSpacing(6);
    auto* originLabel = new QLabel(tr3(QStringLiteral("Origen"), QStringLiteral("Source"), QStringLiteral("源")), originPane);
    m_originPoolCombo = new QComboBox(originPane);
    m_originPoolCombo->setMinimumContentsLength(8);
    m_originPoolCombo->setMaximumWidth(140);
    m_originPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_originTree = new QTreeWidget(originPane);
    m_originTree->setColumnCount(2);
    m_originTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot")});
    m_originTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_originTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_originTree->header()->setStretchLastSection(false);
    m_originTree->setColumnWidth(0, 280);
    m_originTree->setColumnWidth(1, 98);
    m_originTree->setUniformRowHeights(true);
    {
        QFont f = m_originTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_originTree->setFont(f);
    }
    m_originTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
    m_originSelectionLabel = new QLabel(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")), originPane);
    m_originSelectionLabel->setWordWrap(true);
    m_originSelectionLabel->setMinimumHeight(36);
    originTop->addWidget(originLabel, 0);
    originTop->addWidget(m_originPoolCombo, 0);
    originTop->addWidget(m_originSelectionLabel, 1);
    originLayout->addLayout(originTop);
    originLayout->addWidget(m_originTree, 1);

    auto* destPane = new QWidget(dsLeft);
    auto* destLayout = new QVBoxLayout(destPane);
    destLayout->setContentsMargins(0, 0, 0, 0);
    destLayout->setSpacing(4);
    auto* destTop = new QHBoxLayout();
    destTop->setContentsMargins(0, 0, 0, 0);
    destTop->setSpacing(6);
    auto* destLabel = new QLabel(tr3(QStringLiteral("Destino"), QStringLiteral("Target"), QStringLiteral("目标")), destPane);
    m_destPoolCombo = new QComboBox(destPane);
    m_destPoolCombo->setMinimumContentsLength(8);
    m_destPoolCombo->setMaximumWidth(140);
    m_destPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_destTree = new QTreeWidget(destPane);
    m_destTree->setColumnCount(2);
    m_destTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot")});
    m_destTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_destTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_destTree->header()->setStretchLastSection(false);
    m_destTree->setColumnWidth(0, 280);
    m_destTree->setColumnWidth(1, 98);
    m_destTree->setUniformRowHeights(true);
    {
        QFont f = m_destTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_destTree->setFont(f);
    }
    m_destTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
    m_destSelectionLabel = new QLabel(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")), destPane);
    m_destSelectionLabel->setWordWrap(true);
    m_destSelectionLabel->setMinimumHeight(36);
    destTop->addWidget(destLabel, 0);
    destTop->addWidget(m_destPoolCombo, 0);
    destTop->addWidget(m_destSelectionLabel, 1);
    destLayout->addLayout(destTop);
    destLayout->addWidget(m_destTree, 1);

    dsLeftLayout->addWidget(originPane, 1);
    dsLeftLayout->addWidget(destPane, 1);
    rightDatasetsLayout->addWidget(dsLeft, 1);

    auto* rightAdvancedPage = new QWidget(m_rightStack);
    auto* rightAdvancedLayout = new QVBoxLayout(rightAdvancedPage);
    rightAdvancedLayout->setContentsMargins(0, 0, 0, 0);
    rightAdvancedLayout->setSpacing(4);
    auto* advLeft = new QWidget(rightAdvancedPage);
    auto* advLeftLayout = new QVBoxLayout(advLeft);
    m_advPoolCombo = new QComboBox(rightAdvancedPage);
    m_advPoolCombo->setMinimumContentsLength(6);
    m_advPoolCombo->setMaximumWidth(110);
    m_advPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_advTree = new QTreeWidget(rightAdvancedPage);
    m_advTree->setColumnCount(2);
    m_advTree->setHeaderLabels({QStringLiteral("Dataset"), QStringLiteral("Snapshot")});
    m_advTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_advTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_advTree->header()->setStretchLastSection(false);
    m_advTree->setColumnWidth(0, 280);
    m_advTree->setColumnWidth(1, 98);
    m_advTree->setUniformRowHeights(true);
    {
        QFont f = m_advTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_advTree->setFont(f);
    }
    m_advTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
    m_advSelectionLabel = new QLabel(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")), rightAdvancedPage);
    m_advSelectionLabel->setWordWrap(true);
    m_advSelectionLabel->setMinimumHeight(36);
    auto* advTop = new QHBoxLayout();
    advTop->addWidget(m_advPoolCombo, 0);
    advTop->addWidget(m_advSelectionLabel, 1);
    advLeftLayout->addLayout(advTop);
    advLeftLayout->addWidget(m_advTree, 1);
    rightAdvancedLayout->addWidget(advLeft, 1);

    m_rightStack->addWidget(rightConnectionsPage);
    m_rightStack->addWidget(rightDatasetsPage);
    m_rightStack->addWidget(rightAdvancedPage);
    rightLayout->addWidget(m_rightStack, 1);

    topLayout->addWidget(leftPane, 0);
    topLayout->addWidget(rightPane, 1);
    root->addWidget(topArea, 81);

    auto* logBox = new QGroupBox(tr3(QStringLiteral("Log combinado"), QStringLiteral("Combined log"), QStringLiteral("组合日志")), central);
    auto* logLayout = new QVBoxLayout(logBox);
    logLayout->setContentsMargins(6, 6, 6, 6);
    logLayout->setSpacing(4);
    auto* logBody = new QHBoxLayout();

    auto* leftInfo = new QWidget(logBox);
    auto* leftInfoLayout = new QVBoxLayout(leftInfo);
    leftInfoLayout->setContentsMargins(0, 0, 0, 0);
    leftInfoLayout->setSpacing(4);
    auto* statusTitle = new QLabel(tr3(QStringLiteral("Estado"), QStringLiteral("Status"), QStringLiteral("状态")), leftInfo);
    QFont smallTitle = statusTitle->font();
    smallTitle.setBold(true);
    smallTitle.setPointSize(qMax(6, smallTitle.pointSize() - 1));
    statusTitle->setFont(smallTitle);
    m_statusText = new QTextEdit(leftInfo);
    m_statusText->setReadOnly(true);
    m_statusText->setAcceptRichText(false);
    m_statusText->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_statusText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_statusText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    {
        QFont f = m_statusText->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_statusText->setFont(f);
    }
    auto* detailTitle = new QLabel(tr3(QStringLiteral("Detalle"), QStringLiteral("Detail"), QStringLiteral("详情")), leftInfo);
    detailTitle->setFont(smallTitle);
    m_lastDetailText = new QTextEdit(leftInfo);
    m_lastDetailText->setReadOnly(true);
    m_lastDetailText->setAcceptRichText(false);
    m_lastDetailText->setLineWrapMode(QTextEdit::WidgetWidth);
    m_lastDetailText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    {
        QFont f = m_lastDetailText->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_lastDetailText->setFont(f);
    }
    statusTitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    detailTitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    statusTitle->setMinimumWidth(48);
    detailTitle->setMinimumWidth(48);
    statusTitle->setMaximumWidth(64);
    detailTitle->setMaximumWidth(64);
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(6);
    statusRow->addWidget(statusTitle, 0);
    statusRow->addWidget(m_statusText, 1);
    auto* detailRow = new QHBoxLayout();
    detailRow->setContentsMargins(0, 0, 0, 0);
    detailRow->setSpacing(6);
    detailRow->addWidget(detailTitle, 0);
    detailRow->addWidget(m_lastDetailText, 1);
    leftInfoLayout->addLayout(statusRow, 1);
    leftInfoLayout->addLayout(detailRow, 1);

    auto* rightLogs = new QWidget(logBox);
    auto* rightLogsLayout = new QVBoxLayout(rightLogs);
    rightLogsLayout->setContentsMargins(0, 0, 0, 0);
    rightLogsLayout->setSpacing(4);
    auto* rightLogsBody = new QHBoxLayout();
    rightLogsBody->setContentsMargins(0, 0, 0, 0);
    rightLogsBody->setSpacing(6);
    m_logsTabs = new QTabWidget(rightLogs);
    m_logsTabs->setDocumentMode(false);
    m_logsTabs->setStyleSheet(
        QStringLiteral("QTabBar::tab { padding: 1px 8px; min-height: 10px; }"
                       "QTabBar::tab:selected { min-height: 12px; }"
                       "QTabBar::tab:!selected { margin-top: 2px; }"));
    auto* appTab = new QWidget(m_logsTabs);
    auto* appTabLayout = new QVBoxLayout(appTab);
    m_logView = new QPlainTextEdit(appTab);
    m_logView->setReadOnly(true);
    QFont mono = m_logView->font();
    mono.setFamily(QStringLiteral("Monospace"));
    mono.setPointSize(8);
    m_logView->setFont(mono);
    appTabLayout->addWidget(m_logView, 1);
    m_logsTabs->addTab(appTab, tr3(QStringLiteral("Aplicación"), QStringLiteral("Application"), QStringLiteral("应用")));
    rightLogsBody->addWidget(m_logsTabs, 1);

    auto* controlsPane = new QWidget(rightLogs);
    controlsPane->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    controlsPane->setMinimumWidth(72);
    controlsPane->setMaximumWidth(72);
    auto* logControls = new QVBoxLayout(controlsPane);
    logControls->setContentsMargins(0, 0, 0, 0);
    logControls->setSpacing(4);
    m_logLevelCombo = new QComboBox(rightLogs);
    m_logLevelCombo->addItems({QStringLiteral("normal"), QStringLiteral("info"), QStringLiteral("debug")});
    m_logLevelCombo->setCurrentText(QStringLiteral("normal"));
    m_logMaxLinesCombo = new QComboBox(rightLogs);
    m_logMaxLinesCombo->addItems({QStringLiteral("100"), QStringLiteral("200"), QStringLiteral("500"), QStringLiteral("1000")});
    m_logMaxLinesCombo->setCurrentText(QStringLiteral("500"));
    m_logClearBtn = new QPushButton(tr3(QStringLiteral("Limpiar"), QStringLiteral("Clear"), QStringLiteral("清空")), rightLogs);
    m_logCopyBtn = new QPushButton(tr3(QStringLiteral("Copiar"), QStringLiteral("Copy"), QStringLiteral("复制")), rightLogs);
    m_logCancelBtn = new QPushButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), rightLogs);
    m_logCancelBtn->setVisible(false);
    m_logLevelCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_logMaxLinesCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_logClearBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_logCopyBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_logCancelBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    {
        QFont cf = m_logLevelCombo->font();
        cf.setPointSize(qMax(6, cf.pointSize() - 1));
        m_logLevelCombo->setFont(cf);
        m_logMaxLinesCombo->setFont(cf);
        m_logClearBtn->setFont(cf);
        m_logCopyBtn->setFont(cf);
        m_logCancelBtn->setFont(cf);
    }
    m_logLevelCombo->setFixedWidth(66);
    m_logMaxLinesCombo->setFixedWidth(66);
    m_logClearBtn->setFixedWidth(66);
    m_logCopyBtn->setFixedWidth(66);
    m_logCancelBtn->setFixedWidth(66);
    logControls->addWidget(m_logLevelCombo, 0);
    logControls->addWidget(m_logMaxLinesCombo, 0);
    logControls->addWidget(m_logClearBtn, 0);
    logControls->addWidget(m_logCopyBtn, 0);
    logControls->addWidget(m_logCancelBtn, 0);
    logControls->addStretch(1);
    rightLogsBody->addWidget(controlsPane, 0);
    rightLogsLayout->addLayout(rightLogsBody, 1);

    logBody->addWidget(leftInfo, 1);
    logBody->addWidget(rightLogs, 2);
    logLayout->addLayout(logBody, 1);
    root->addWidget(logBox, 19);

    setCentralWidget(central);

    connect(m_btnRefreshAll, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Refrescar todo (botón)"));
        refreshAllConnections();
    });
    connect(m_btnNew, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Nueva conexión (botón)"));
        createConnection();
    });
    connect(m_btnConfig, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Configuración (botón)"));
        openConfigurationDialog();
    });
    connect(m_connectionsList, &QListWidget::itemSelectionChanged, this, [this]() { onConnectionSelectionChanged(); });
    m_connectionsList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_connectionsList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onConnectionListContextMenuRequested(pos);
    });
    connect(m_leftTabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx >= 0 && idx < m_rightStack->count()) {
            m_rightStack->setCurrentIndex(idx);
        }
        if (idx == 0) {
            populateAllPoolsTables();
        } else if (idx == 2) {
            onAdvancedPoolChanged();
        }
    });
    connect(m_importedPoolsTable, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (col == 2) {
            logUiAction(QStringLiteral("Exportar pool (tabla)"));
            exportPoolFromRow(row);
        }
        refreshSelectedPoolDetails();
    });
    connect(m_importedPoolsTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        refreshSelectedPoolDetails();
    });
    connect(m_poolStatusRefreshBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Actualizar estado de pool (botón)"));
        if (m_importedPoolsTable && !m_importedPoolsTable->selectedItems().isEmpty()) {
            refreshSelectedPoolDetails();
        }
    });
    connect(m_importablePoolsTable, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (col == 0) {
            logUiAction(QStringLiteral("Importar pool (tabla)"));
            importPoolFromRow(row);
        }
    });
    connect(m_originPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onOriginPoolChanged(); });
    connect(m_destPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onDestPoolChanged(); });
    connect(m_advPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onAdvancedPoolChanged(); });
    connect(m_originTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onOriginTreeSelectionChanged(); });
    connect(m_destTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onDestTreeSelectionChanged(); });
    connect(m_originTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        onOriginTreeItemDoubleClicked(item, col);
    });
    connect(m_destTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        onDestTreeItemDoubleClicked(item, col);
    });
    m_originTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_destTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_advTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_originTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onOriginTreeContextMenuRequested(pos);
    });
    connect(m_destTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onDestTreeContextMenuRequested(pos);
    });
    connect(m_advTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showDatasetContextMenu(QStringLiteral("advanced"), m_advTree, pos);
    });
    connect(m_datasetPropsTable, &QTableWidget::cellChanged, this, [this](int row, int col) {
        onDatasetPropsCellChanged(row, col);
    });
    connect(m_btnApplyDatasetProps, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Aplicar propiedades dataset (botón)"));
        applyDatasetPropertyChanges();
    });
    connect(m_advPropsTable, &QTableWidget::cellChanged, this, [this](int row, int col) {
        onAdvancedPropsCellChanged(row, col);
    });
    connect(m_btnApplyAdvancedProps, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Aplicar propiedades avanzadas (botón)"));
        applyAdvancedDatasetPropertyChanges();
    });
    connect(m_btnCopy, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Copiar snapshot (botón)"));
        actionCopySnapshot();
    });
    connect(m_btnLevel, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Nivelar snapshot (botón)"));
        actionLevelSnapshot();
    });
    connect(m_btnSync, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Sincronizar datasets (botón)"));
        actionSyncDatasets();
    });
    connect(m_logClearBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Limpiar log (botón)"));
        clearAppLog();
    });
    connect(m_logCopyBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Copiar log (botón)"));
        copyAppLogToClipboard();
    });
    connect(m_logMaxLinesCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        trimLogWidget(m_logView);
    });
    connect(m_advTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        const auto selected = m_advTree->selectedItems();
        if (selected.isEmpty()) {
            m_advSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
            refreshDatasetProperties(QStringLiteral("advanced"));
            updateTransferButtonsState();
            return;
        }
        auto* it = selected.first();
        const QString ds = it->data(0, Qt::UserRole).toString();
        const QString snap = it->data(1, Qt::UserRole).toString();
        if (!ds.isEmpty() && !snap.isEmpty()) {
            m_advSelectionLabel->setText(QStringLiteral("%1@%2").arg(ds, snap));
        } else if (!ds.isEmpty()) {
            m_advSelectionLabel->setText(ds);
        } else {
            m_advSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        }
        refreshDatasetProperties(QStringLiteral("advanced"));
        updateTransferButtonsState();
    });
    connect(m_advTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        Q_UNUSED(item);
        Q_UNUSED(col);
    });
    connect(m_btnAdvancedBreakdown, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Desglosar (botón)"));
        actionAdvancedBreakdown();
    });
    connect(m_btnAdvancedAssemble, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Ensamblar (botón)"));
        actionAdvancedAssemble();
    });
}

void MainWindow::loadConnections() {
    const LoadResult loaded = m_store.loadConnections();
    m_profiles = loaded.profiles;
    m_states.clear();
    m_states.resize(m_profiles.size());

    rebuildConnectionList();
    updateStatus(tr3(QStringLiteral("Estado: %1 conexiones cargadas"),
                     QStringLiteral("Status: %1 connections loaded"),
                     QStringLiteral("状态：已加载 %1 个连接"))
                     .arg(m_profiles.size()));
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }
    rebuildDatasetPoolSelectors();
    syncConnectionLogTabs();
}

void MainWindow::rebuildConnectionList() {
    m_connectionsList->clear();
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        const auto& s = m_states[i];

        const QString line1 = QStringLiteral("%1/%2").arg(p.name, p.connType);
        QString zfsTxt = s.zfsVersion.trimmed();
        if (zfsTxt.isEmpty()) {
            zfsTxt = QStringLiteral("?");
        }
        QString statusTag;
        QColor rowColor("#14212b");
        const QString st = s.status.trimmed().toUpper();
        if (st == QStringLiteral("OK")) {
            statusTag = QStringLiteral("[OK] ");
            rowColor = QColor("#1f7a1f");
        } else if (!st.isEmpty()) {
            statusTag = QStringLiteral("[Error] ");
            rowColor = QColor("#a12a2a");
        }
        QString line = QStringLiteral("%1%2 | %3 | ZFS v%4").arg(statusTag, line1, p.osType, zfsTxt);

        auto* item = new QListWidgetItem(line, m_connectionsList);
        item->setData(Qt::UserRole, i);
        item->setForeground(QBrush(rowColor));
        item->setToolTip(QStringLiteral("Host: %1\nPort: %2\nEstado: %3\nDetalle: %4")
                             .arg(p.host)
                             .arg(p.port)
                             .arg(s.status)
                             .arg(s.detail));
    }
}

void MainWindow::rebuildDatasetPoolSelectors() {
    m_originPoolCombo->blockSignals(true);
    m_destPoolCombo->blockSignals(true);
    m_advPoolCombo->blockSignals(true);

    const QString originPrev = m_originPoolCombo->currentData().toString();
    const QString destPrev = m_destPoolCombo->currentData().toString();
    const QString advPrev = m_advPoolCombo->currentData().toString();

    m_originPoolCombo->clear();
    m_destPoolCombo->clear();
    m_advPoolCombo->clear();

    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& p : st.importedPools) {
            if (p.pool.isEmpty() || p.pool == QStringLiteral("Sin pools")) {
                continue;
            }
            const QString token = QStringLiteral("%1::%2").arg(i).arg(p.pool);
            const QString label = QStringLiteral("%1::%2").arg(m_profiles[i].name, p.pool);
            m_originPoolCombo->addItem(label, token);
            m_destPoolCombo->addItem(label, token);
            m_advPoolCombo->addItem(label, token);
        }
    }

    auto restoreCurrent = [](QComboBox* combo, const QString& token) {
        if (combo->count() <= 0) {
            return;
        }
        int idx = combo->findData(token);
        if (idx < 0) {
            idx = 0;
        }
        combo->setCurrentIndex(idx);
    };
    restoreCurrent(m_originPoolCombo, originPrev);
    restoreCurrent(m_destPoolCombo, destPrev);
    restoreCurrent(m_advPoolCombo, advPrev);

    m_originPoolCombo->blockSignals(false);
    m_destPoolCombo->blockSignals(false);
    m_advPoolCombo->blockSignals(false);
    onOriginPoolChanged();
    onDestPoolChanged();
    onAdvancedPoolChanged();
}

void MainWindow::refreshAllConnections() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: refresh bloqueado"));
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refrescar todas las conexiones"));
    if (m_profiles.isEmpty()) {
        rebuildConnectionList();
        rebuildDatasetPoolSelectors();
        populateAllPoolsTables();
        updateStatus(tr3(QStringLiteral("Estado: refresco finalizado"),
                         QStringLiteral("Status: refresh finished"),
                         QStringLiteral("状态：刷新完成")));
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = m_profiles.size();
    m_refreshTotal = m_profiles.size();
    updateStatus(tr3(QStringLiteral("Estado: refrescando 0/%1"),
                     QStringLiteral("Status: refreshing 0/%1"),
                     QStringLiteral("状态：刷新中 0/%1"))
                     .arg(m_refreshTotal));

    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile profile = m_profiles[i];
        (void)QtConcurrent::run([this, generation, i, profile]() {
            const ConnectionRuntimeState state = refreshConnection(profile);
            QMetaObject::invokeMethod(this, [this, generation, i, state]() {
                onAsyncRefreshResult(generation, i, state);
            }, Qt::QueuedConnection);
        });
    }
}

void MainWindow::refreshSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: refresh bloqueado"));
        return;
    }
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const int idx = selected.first()->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = 1;
    m_refreshTotal = 1;
    updateStatus(tr3(QStringLiteral("Estado: refrescando 0/1"),
                     QStringLiteral("Status: refreshing 0/1"),
                     QStringLiteral("状态：刷新中 0/1")));
    const ConnectionProfile profile = m_profiles[idx];
    (void)QtConcurrent::run([this, generation, idx, profile]() {
        const ConnectionRuntimeState state = refreshConnection(profile);
        QMetaObject::invokeMethod(this, [this, generation, idx, state]() {
            onAsyncRefreshResult(generation, idx, state);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onAsyncRefreshResult(int generation, int idx, const ConnectionRuntimeState& state) {
    if (generation != m_refreshGeneration) {
        return;
    }
    if (idx < 0 || idx >= m_states.size()) {
        return;
    }
    int selectedIdx = -1;
    const auto selected = m_connectionsList ? m_connectionsList->selectedItems() : QList<QListWidgetItem*>{};
    if (!selected.isEmpty()) {
        selectedIdx = selected.first()->data(Qt::UserRole).toInt();
    }
    m_states[idx] = state;
    rebuildConnectionList();
    if (selectedIdx >= 0 && selectedIdx < m_connectionsList->count()) {
        m_connectionsList->setCurrentRow(selectedIdx);
    }
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
    if (m_refreshPending > 0) {
        --m_refreshPending;
    }
    const int done = qMax(0, m_refreshTotal - m_refreshPending);
    updateStatus(tr3(QStringLiteral("Estado: refrescando %1/%2"),
                     QStringLiteral("Status: refreshing %1/%2"),
                     QStringLiteral("状态：刷新中 %1/%2"))
                     .arg(done)
                     .arg(qMax(1, m_refreshTotal)));
    if (m_refreshPending == 0) {
        onAsyncRefreshDone(generation);
    }
}

void MainWindow::onAsyncRefreshDone(int generation) {
    if (generation != m_refreshGeneration) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refresco paralelo finalizado"));
    updateStatus(tr3(QStringLiteral("Estado: refresco finalizado"),
                     QStringLiteral("Status: refresh finished"),
                     QStringLiteral("状态：刷新完成")));
}

void MainWindow::createConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: nueva conexión bloqueada"));
        return;
    }
    ConnectionDialog dlg(this);
    ConnectionProfile p;
    p.connType = QStringLiteral("SSH");
    p.transport = QStringLiteral("SSH");
    p.osType = QStringLiteral("Linux");
    p.port = 22;
    dlg.setProfile(p);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    QString err;
    if (!m_store.upsertConnection(dlg.profile(), err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudo crear conexión:\n%1").arg(err));
        return;
    }
    loadConnections();
    refreshAllConnections();
}

void MainWindow::editConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: edición bloqueada"));
        return;
    }
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const int idx = selected.first()->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    ConnectionDialog dlg(this);
    dlg.setProfile(m_profiles[idx]);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile edited = dlg.profile();
    edited.id = m_profiles[idx].id;
    QString err;
    if (!m_store.upsertConnection(edited, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudo actualizar conexión:\n%1").arg(err));
        return;
    }
    loadConnections();
    refreshAllConnections();
}

void MainWindow::deleteConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: borrado bloqueado"));
        return;
    }
    const auto selected = m_connectionsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const int idx = selected.first()->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        QStringLiteral("Borrar conexión"),
        QStringLiteral("¿Borrar conexión \"%1\"?").arg(m_profiles[idx].name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    QString err;
    if (!m_store.deleteConnectionById(m_profiles[idx].id, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudo borrar conexión:\n%1").arg(err));
        return;
    }
    loadConnections();
    refreshAllConnections();
}

void MainWindow::onConnectionSelectionChanged() {
    if (m_leftTabs->currentIndex() == 0) {
        populateAllPoolsTables();
    }
}

void MainWindow::onConnectionListContextMenuRequested(const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    QListWidgetItem* item = m_connectionsList->itemAt(pos);
    if (item) {
        m_connectionsList->setCurrentItem(item);
    }
    const bool hasSel = (m_connectionsList->currentItem() != nullptr);

    QMenu menu(this);
    QAction* refreshAct = menu.addAction(QStringLiteral("Refrescar"));
    menu.addSeparator();
    QAction* editAct = menu.addAction(QStringLiteral("Editar"));
    QAction* deleteAct = menu.addAction(QStringLiteral("Borrar"));
    refreshAct->setEnabled(hasSel);
    editAct->setEnabled(hasSel);
    deleteAct->setEnabled(hasSel);

    QAction* picked = menu.exec(m_connectionsList->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == refreshAct) {
        logUiAction(QStringLiteral("Refrescar conexión (menú)"));
        refreshSelectedConnection();
    } else if (picked == editAct) {
        logUiAction(QStringLiteral("Editar conexión (menú)"));
        editConnection();
    } else if (picked == deleteAct) {
        logUiAction(QStringLiteral("Borrar conexión (menú)"));
        deleteConnection();
    }
}

void MainWindow::onImportedPoolsContextMenuRequested(const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    const QModelIndex idx = m_importedPoolsTable->indexAt(pos);
    if (!idx.isValid()) {
        return;
    }
    const int row = idx.row();
    m_importedPoolsTable->selectRow(row);
    QMenu menu(this);
    QAction* exportAct = menu.addAction(QStringLiteral("Exportar"));
    QAction* picked = menu.exec(m_importedPoolsTable->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == exportAct) {
        logUiAction(QStringLiteral("Exportar pool (menú)"));
        exportPoolFromRow(row);
    }
}

void MainWindow::onImportablePoolsContextMenuRequested(const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    const QModelIndex idx = m_importablePoolsTable->indexAt(pos);
    if (!idx.isValid()) {
        return;
    }
    const int row = idx.row();
    m_importablePoolsTable->selectRow(row);
    QMenu menu(this);
    QAction* importAct = menu.addAction(QStringLiteral("Importar"));
    const QString state = m_importablePoolsTable->item(row, 3) ? m_importablePoolsTable->item(row, 3)->text().trimmed().toUpper() : QString();
    importAct->setEnabled(state == QStringLiteral("ONLINE") || state == QStringLiteral("ACTIVE"));
    QAction* picked = menu.exec(m_importablePoolsTable->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == importAct) {
        logUiAction(QStringLiteral("Importar pool (menú)"));
        importPoolFromRow(row);
    }
}

void MainWindow::onOriginPoolChanged() {
    m_originSelectedDataset.clear();
    m_originSelectedSnapshot.clear();
    const QString token = m_originPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_originTree->clear();
        m_originSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_originTree, connIdx, poolName, QStringLiteral("origin"));
    refreshDatasetProperties(QStringLiteral("origin"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onDestPoolChanged() {
    m_destSelectedDataset.clear();
    m_destSelectedSnapshot.clear();
    const QString token = m_destPoolCombo->currentData().toString();
    if (token.isEmpty()) {
        m_destTree->clear();
        m_destSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        return;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_destTree, connIdx, poolName, QStringLiteral("dest"));
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::onAdvancedPoolChanged() {
    const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        if (m_advTree) {
            m_advTree->clear();
        }
        if (m_advSelectionLabel) {
            m_advSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        }
        refreshDatasetProperties(QStringLiteral("advanced"));
        updateTransferButtonsState();
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    populateDatasetTree(m_advTree, connIdx, poolName, QStringLiteral("origin"));
    if (m_advSelectionLabel) {
        m_advSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
    }
    refreshDatasetProperties(QStringLiteral("advanced"));
    updateTransferButtonsState();
}

void MainWindow::onOriginTreeSelectionChanged() {
    const auto selected = m_originTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("origin"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("origin"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onDestTreeSelectionChanged() {
    const auto selected = m_destTree->selectedItems();
    if (selected.isEmpty()) {
        setSelectedDataset(QStringLiteral("dest"), QString(), QString());
        return;
    }
    auto* it = selected.first();
    setSelectedDataset(QStringLiteral("dest"), it->data(0, Qt::UserRole).toString(), it->data(1, Qt::UserRole).toString());
}

void MainWindow::onOriginTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    Q_UNUSED(item);
    Q_UNUSED(col);
}

void MainWindow::onDestTreeItemDoubleClicked(QTreeWidgetItem* item, int col) {
    Q_UNUSED(item);
    Q_UNUSED(col);
}

void MainWindow::onOriginTreeContextMenuRequested(const QPoint& pos) {
    showDatasetContextMenu(QStringLiteral("origin"), m_originTree, pos);
}

void MainWindow::onDestTreeContextMenuRequested(const QPoint& pos) {
    showDatasetContextMenu(QStringLiteral("dest"), m_destTree, pos);
}

bool MainWindow::runSsh(const ConnectionProfile& p, const QString& remoteCmd, int timeoutMs, QString& out, QString& err, int& rc) {
    out.clear();
    err.clear();
    rc = -1;

    QStringList args;
    args << "-o" << "BatchMode=yes";
    args << "-o" << "ConnectTimeout=10";
    args << "-o" << "LogLevel=ERROR";
    args << "-o" << "StrictHostKeyChecking=no";
    args << "-o" << "UserKnownHostsFile=/dev/null";
    if (p.port > 0) {
        args << "-p" << QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        args << "-i" << p.keyPath;
    }
    args << QStringLiteral("%1@%2").arg(p.username, p.host);
    args << remoteCmd;

    const QString cmdLine = QStringLiteral("%1@%2:%3 $ %4")
                                .arg(p.username, p.host)
                                .arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22"))
                                .arg(remoteCmd);
    appLog(QStringLiteral("INFO"), cmdLine);
    appendConnectionLog(p.id, cmdLine);

    QProcess proc;
    proc.start(QStringLiteral("ssh"), args);
    if (!proc.waitForStarted(4000)) {
        err = QStringLiteral("No se pudo iniciar ssh");
        appendConnectionLog(p.id, err);
        return false;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        err = QStringLiteral("Timeout");
        appendConnectionLog(p.id, err);
        return false;
    }

    rc = proc.exitCode();
    out = QString::fromUtf8(proc.readAllStandardOutput());
    err = QString::fromUtf8(proc.readAllStandardError());
    if (!out.trimmed().isEmpty()) {
        appendConnectionLog(p.id, oneLine(out));
    }
    if (!err.trimmed().isEmpty()) {
        appendConnectionLog(p.id, oneLine(err));
    }
    return true;
}

QString MainWindow::withSudo(const ConnectionProfile& p, const QString& cmd) const {
    if (!p.useSudo) {
        return cmd;
    }
    if (!p.password.isEmpty()) {
        return QStringLiteral("printf '%s\\n' %1 | sudo -S -p '' sh -lc %2")
            .arg(shSingleQuote(p.password), shSingleQuote(cmd));
    }
    return QStringLiteral("sudo -n ") + cmd;
}

bool MainWindow::getDatasetProperty(int connIdx, const QString& dataset, const QString& prop, QString& valueOut) {
    valueOut.clear();
    if (connIdx < 0 || connIdx >= m_profiles.size() || dataset.isEmpty() || prop.isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd =
        QStringLiteral("zfs get -H -o value %1 %2").arg(shSingleQuote(prop), shSingleQuote(dataset));
    cmd = withSudo(p, cmd);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 15000, out, err, rc) || rc != 0) {
        return false;
    }
    valueOut = out.trimmed();
    return true;
}

QString MainWindow::datasetCacheKey(int connIdx, const QString& poolName) const {
    return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
}

bool MainWindow::ensureDatasetsLoaded(int connIdx, const QString& poolName) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return false;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    PoolDatasetCache& cache = m_poolDatasetCache[key];
    if (cache.loaded) {
        return true;
    }

    const ConnectionProfile& p = m_profiles[connIdx];
    QString cmd = QStringLiteral(
        "zfs list -H -p -t filesystem,volume,snapshot "
        "-o name,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r %1")
                      .arg(poolName);
    cmd = withSudo(p, cmd);

    QString out;
    QString err;
    int rc = -1;
    appLog(QStringLiteral("INFO"), QStringLiteral("Loading datasets %1::%2").arg(p.name, poolName));
    if (!runSsh(p, cmd, 35000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("WARN"), QStringLiteral("Failed datasets %1::%2 -> %3")
                                        .arg(p.name, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        return false;
    }

    QMap<QString, QVector<QPair<QString, QString>>> snapshotMetaByDataset;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList f = line.split('\t');
        if (f.size() < 9) {
            continue;
        }
        const QString name = f[0].trimmed();
        if (name.isEmpty()) {
            continue;
        }
        DatasetRecord rec{name, f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]};
        if (name.contains('@')) {
            const QString ds = name.section('@', 0, 0);
            const QString snap = name.section('@', 1);
            snapshotMetaByDataset[ds].push_back(qMakePair(rec.creation, snap));
        } else {
            cache.datasets.push_back(rec);
            cache.recordByName[name] = rec;
        }
    }
    for (auto it = snapshotMetaByDataset.begin(); it != snapshotMetaByDataset.end(); ++it) {
        auto rows = it.value();
        std::sort(rows.begin(), rows.end(), [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
            bool aOk = false;
            bool bOk = false;
            const qlonglong av = a.first.toLongLong(&aOk);
            const qlonglong bv = b.first.toLongLong(&bOk);
            if (aOk && bOk && av != bv) {
                return av > bv; // más nuevo primero
            }
            if (a.first != b.first) {
                return a.first > b.first; // fallback textual desc
            }
            return a.second > b.second; // fallback por nombre desc
        });
        QStringList sortedSnaps;
        sortedSnaps.reserve(rows.size());
        for (const auto& row : rows) {
            sortedSnaps.push_back(row.second);
        }
        cache.snapshotsByDataset.insert(it.key(), sortedSnaps);
    }
    cache.loaded = true;
    appLog(QStringLiteral("DEBUG"), QStringLiteral("Datasets loaded %1::%2 (%3)")
                                     .arg(p.name)
                                     .arg(poolName)
                                     .arg(cache.datasets.size()));
    return true;
}

void MainWindow::populateDatasetTree(QTreeWidget* tree, int connIdx, const QString& poolName, const QString& side) {
    tree->clear();
    if (!ensureDatasetsLoaded(connIdx, poolName)) {
        return;
    }
    const QString key = datasetCacheKey(connIdx, poolName);
    const PoolDatasetCache& cache = m_poolDatasetCache[key];

    QMap<QString, QTreeWidgetItem*> byName;
    for (const DatasetRecord& rec : cache.datasets) {
        auto* item = new QTreeWidgetItem();
        const QString displayName = rec.name.contains('/')
                                        ? rec.name.section('/', -1, -1)
                                        : rec.name;
        item->setText(0, displayName);
        const QStringList snaps = cache.snapshotsByDataset.value(rec.name);
        item->setText(1, snaps.isEmpty() ? QString() : QStringLiteral("(ninguno)"));
        item->setData(1, Qt::UserRole, QString());
        item->setData(0, Qt::UserRole, rec.name);
        item->setData(2, Qt::UserRole, snaps);
        byName.insert(rec.name, item);
    }

    for (const DatasetRecord& rec : cache.datasets) {
        QTreeWidgetItem* item = byName.value(rec.name, nullptr);
        if (!item) {
            continue;
        }
        const QString parent = parentDatasetName(rec.name);
        QTreeWidgetItem* parentItem = byName.value(parent, nullptr);
        if (parentItem) {
            parentItem->addChild(item);
        } else {
            tree->addTopLevelItem(item);
        }
    }
    tree->expandToDepth(0);

    // Dropdown embebido en celda Snapshot, sin seleccionar ninguno al inicio.
    std::function<void(QTreeWidgetItem*)> attachCombos = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QStringList snaps = n->data(2, Qt::UserRole).toStringList();
        if (!snaps.isEmpty()) {
            QStringList options;
            options << QStringLiteral("(ninguno)");
            options += snaps;
            auto* combo = new QComboBox(tree);
            combo->addItems(options);
            combo->setCurrentIndex(0);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
            combo->setMinimumHeight(22);
            combo->setMaximumHeight(22);
            combo->setFont(tree->font());
            combo->setStyleSheet(QStringLiteral("QComboBox{padding:0 2px; margin:0px;}"));
            tree->setItemWidget(n, 1, combo);
            QObject::connect(combo, &QComboBox::currentTextChanged, tree, [this, tree, n, side](const QString& txt) {
                onSnapshotComboChanged(tree, n, side, txt);
            });
        } else {
            tree->setItemWidget(n, 1, nullptr);
            n->setText(1, QString());
            n->setData(1, Qt::UserRole, QString());
        }
        for (int i = 0; i < n->childCount(); ++i) {
            attachCombos(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        attachCombos(tree->topLevelItem(i));
    }

    if (side == QStringLiteral("origin")) {
        m_originSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
    } else {
        m_destSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
    }
}

void MainWindow::clearOtherSnapshotSelections(QTreeWidget* tree, QTreeWidgetItem* keepItem) {
    std::function<void(QTreeWidgetItem*)> clearRec = [&](QTreeWidgetItem* n) {
        if (!n || n == keepItem) {
            return;
        }
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(n, 1))) {
            QSignalBlocker b(cb);
            cb->setCurrentIndex(0);
        }
        n->setData(1, Qt::UserRole, QString());
        for (int i = 0; i < n->childCount(); ++i) {
            clearRec(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        clearRec(tree->topLevelItem(i));
    }
}

void MainWindow::onSnapshotComboChanged(QTreeWidget* tree, QTreeWidgetItem* item, const QString& side, const QString& chosen) {
    if (!tree || !item) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString();
    const QString snap = (chosen == QStringLiteral("(ninguno)")) ? QString() : chosen.trimmed();
    if (!snap.isEmpty()) {
        clearOtherSnapshotSelections(tree, item);
    }
    item->setData(1, Qt::UserRole, snap);
    tree->setCurrentItem(item);
    setSelectedDataset(side, ds, snap);
}

void MainWindow::refreshDatasetProperties(const QString& side) {
    QString dataset;
    if (side == QStringLiteral("origin")) {
        dataset = m_originSelectedDataset;
    } else if (side == QStringLiteral("dest")) {
        dataset = m_destSelectedDataset;
    } else {
        const auto selected = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
        if (!selected.isEmpty()) {
            dataset = selected.first()->data(0, Qt::UserRole).toString();
        }
    }
    QTableWidget* table = (side == QStringLiteral("advanced")) ? m_advPropsTable : m_datasetPropsTable;
    if (!table) {
        return;
    }
    if (dataset.isEmpty()) {
        table->setRowCount(0);
        if (side == QStringLiteral("advanced")) {
            m_advPropsDataset.clear();
            m_advPropsOriginalValues.clear();
            m_advPropsOriginalInherit.clear();
            m_advPropsDirty = false;
        } else {
            m_propsDataset.clear();
            m_propsSide = side;
            m_propsOriginalValues.clear();
            m_propsOriginalInherit.clear();
            m_propsDirty = false;
        }
        updateApplyPropsButtonState();
        return;
    }

    QString token;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo->currentData().toString();
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo->currentData().toString();
    } else {
        token = m_advPoolCombo->currentData().toString();
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const QString key = datasetCacheKey(connIdx, poolName);
    const auto it = m_poolDatasetCache.constFind(key);
    if (it == m_poolDatasetCache.constEnd()) {
        return;
    }
    const PoolDatasetCache& cache = it.value();
    const auto recIt = cache.recordByName.constFind(dataset);
    if (recIt == cache.recordByName.constEnd()) {
        return;
    }
    const DatasetRecord& rec = recIt.value();
    const ConnectionProfile& p = m_profiles[connIdx];

    QString datasetType = dataset.contains('@') ? QStringLiteral("snapshot") : QStringLiteral("filesystem");
    {
        QString tOut, tErr;
        int tRc = -1;
        const QString typeCmd = withSudo(
            p,
            QStringLiteral("zfs get -H -o value type %1").arg(shSingleQuote(dataset)));
        if (runSsh(p, typeCmd, 12000, tOut, tErr, tRc) && tRc == 0) {
            const QString t = tOut.trimmed().toLower();
            if (!t.isEmpty()) {
                datasetType = t;
            }
        }
    }

    struct PropRow {
        QString prop;
        QString value;
        QString source;
        QString readonly;
    };
    QVector<PropRow> rows;
    rows.push_back({QStringLiteral("dataset"), rec.name, QString(), QStringLiteral("true")});
    const QString mountedRaw = rec.mounted.trimmed().toLower();
    const bool mountedYes = (mountedRaw == QStringLiteral("yes")
                             || mountedRaw == QStringLiteral("on")
                             || mountedRaw == QStringLiteral("true")
                             || mountedRaw == QStringLiteral("1"));
    rows.push_back({QStringLiteral("estado"), mountedYes ? QStringLiteral("Montado") : QStringLiteral("Desmontado"), QString(), QStringLiteral("true")});

    QString out;
    QString err;
    int rc = -1;
    QString propsCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o property,value,source,readonly all %1").arg(shSingleQuote(dataset)));
    if (!runSsh(p, propsCmd, 20000, out, err, rc) || rc != 0) {
        propsCmd = withSudo(
            p,
            QStringLiteral("zfs get -H -o property,value,source all %1").arg(shSingleQuote(dataset)));
        out.clear();
        err.clear();
        rc = -1;
        runSsh(p, propsCmd, 20000, out, err, rc);
    }
    if (rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            QString prop, val, source, ro;
            const QStringList parts = raw.split('\t');
            if (parts.size() >= 4) {
                prop = parts[0].trimmed();
                val = parts[1].trimmed();
                source = parts[2].trimmed();
                ro = parts[3].trimmed().toLower();
            } else if (parts.size() >= 3) {
                prop = parts[0].trimmed();
                val = parts[1].trimmed();
                source = parts[2].trimmed();
                ro.clear();
            } else {
                const QStringList sp = raw.simplified().split(' ');
                if (sp.size() < 3) {
                    continue;
                }
                prop = sp[0].trimmed();
                val = sp[1].trimmed();
                source = sp[2].trimmed();
                ro = (sp.size() > 3) ? sp[3].trimmed().toLower() : QString();
            }
            if (!isDatasetPropertyEditable(prop, datasetType, source, ro)) {
                continue;
            }
            rows.push_back({prop, val, source, ro});
        }
    }

    m_loadingPropsTable = true;
    table->setRowCount(0);
    if (side == QStringLiteral("advanced")) {
        m_advPropsOriginalValues.clear();
        m_advPropsOriginalInherit.clear();
        m_advPropsDataset = rec.name;
    } else {
        m_propsOriginalValues.clear();
        m_propsOriginalInherit.clear();
        m_propsSide = side;
        m_propsDataset = rec.name;
    }
    const QSet<QString> inheritableProps = {QStringLiteral("mountpoint"), QStringLiteral("canmount")};
    for (const PropRow& row : rows) {
        const int r = table->rowCount();
        table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(row.prop));
        auto* v = new QTableWidgetItem(row.value);
        if (row.prop == QStringLiteral("dataset") || row.prop == QStringLiteral("estado")) {
            v->setFlags(v->flags() & ~Qt::ItemIsEditable);
        }
        table->setItem(r, 1, v);
        auto* inh = new QTableWidgetItem();
        if (inheritableProps.contains(row.prop)) {
            inh->setFlags((inh->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) & ~Qt::ItemIsEditable);
            inh->setCheckState(Qt::Unchecked);
        } else {
            inh->setFlags(Qt::ItemIsEnabled);
            inh->setText(QStringLiteral("-"));
        }
        table->setItem(r, 2, inh);
        if (side == QStringLiteral("advanced")) {
            m_advPropsOriginalValues[row.prop] = row.value;
            m_advPropsOriginalInherit[row.prop] = false;
        } else {
            m_propsOriginalValues[row.prop] = row.value;
            m_propsOriginalInherit[row.prop] = false;
        }
    }
    if (side == QStringLiteral("advanced")) {
        m_advPropsDirty = false;
    } else {
        m_propsDirty = false;
    }
    m_loadingPropsTable = false;
    updateApplyPropsButtonState();
}

void MainWindow::setSelectedDataset(const QString& side, const QString& datasetName, const QString& snapshotName) {
    if (side == QStringLiteral("origin")) {
        m_originSelectedDataset = datasetName;
        m_originSelectedSnapshot = snapshotName;
        if (datasetName.isEmpty()) {
            m_originSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
        } else if (snapshotName.isEmpty()) {
            m_originSelectionLabel->setText(datasetName);
        } else {
            m_originSelectionLabel->setText(QStringLiteral("%1@%2").arg(datasetName, snapshotName));
        }
        refreshDatasetProperties(QStringLiteral("origin"));
        refreshTransferSelectionLabels();
        updateTransferButtonsState();
        return;
    }
    m_destSelectedDataset = datasetName;
    m_destSelectedSnapshot = snapshotName;
    if (datasetName.isEmpty()) {
        m_destSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）")));
    } else if (snapshotName.isEmpty()) {
        m_destSelectionLabel->setText(datasetName);
    } else {
        m_destSelectionLabel->setText(QStringLiteral("%1@%2").arg(datasetName, snapshotName));
    }
    refreshDatasetProperties(QStringLiteral("dest"));
    refreshTransferSelectionLabels();
    updateTransferButtonsState();
}

void MainWindow::refreshTransferSelectionLabels() {
    QString originText;
    if (!m_originSelectedDataset.isEmpty()) {
        if (!m_originSelectedSnapshot.isEmpty()) {
            originText = QStringLiteral("%1@%2").arg(m_originSelectedDataset, m_originSelectedSnapshot);
        } else {
            originText = m_originSelectedDataset;
        }
    } else {
        originText = tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）"));
    }
    if (m_transferOriginLabel) {
        m_transferOriginLabel->setText(originText);
    }
    if (m_originSelectionLabel) {
        m_originSelectionLabel->setText(originText);
    }

    QString destText;
    if (!m_destSelectedDataset.isEmpty()) {
        if (!m_destSelectedSnapshot.isEmpty()) {
            destText = QStringLiteral("%1@%2").arg(m_destSelectedDataset, m_destSelectedSnapshot);
        } else {
            destText = m_destSelectedDataset;
        }
    } else {
        destText = tr3(QStringLiteral("(sin selección)"), QStringLiteral("(no selection)"), QStringLiteral("（未选择）"));
    }
    if (m_transferDestLabel) {
        m_transferDestLabel->setText(destText);
    }
    if (m_destSelectionLabel) {
        m_destSelectionLabel->setText(destText);
    }
}

void MainWindow::updateTransferButtonsState() {
    if (actionsLocked()) {
        if (m_btnCopy) m_btnCopy->setEnabled(false);
        if (m_btnLevel) m_btnLevel->setEnabled(false);
        if (m_btnSync) m_btnSync->setEnabled(false);
        if (m_btnAdvancedBreakdown) m_btnAdvancedBreakdown->setEnabled(false);
        if (m_btnAdvancedAssemble) m_btnAdvancedAssemble->setEnabled(false);
        return;
    }
    const bool srcDs = !m_originSelectedDataset.isEmpty();
    const bool srcSnap = !m_originSelectedSnapshot.isEmpty();
    const bool dstDs = !m_destSelectedDataset.isEmpty();
    const bool dstSnap = !m_destSelectedSnapshot.isEmpty();
    m_btnCopy->setEnabled(srcDs && srcSnap && dstDs && !dstSnap);
    m_btnLevel->setEnabled(srcDs && dstDs && !dstSnap);
    m_btnSync->setEnabled(srcDs && !srcSnap && dstDs && !dstSnap);
    const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("advanced"));
    bool advDatasetOnly = actx.valid && !actx.datasetName.isEmpty() && actx.snapshotName.isEmpty();
    if (advDatasetOnly) {
        const QString key = datasetCacheKey(actx.connIdx, actx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt == m_poolDatasetCache.constEnd() || !cacheIt->loaded) {
            advDatasetOnly = false;
        } else {
            bool allMounted = true;
            const QString base = actx.datasetName;
            const QString pref = base + QStringLiteral("/");
            for (auto it = cacheIt->recordByName.constBegin(); it != cacheIt->recordByName.constEnd(); ++it) {
                const QString& ds = it.key();
                if (ds != base && !ds.startsWith(pref)) {
                    continue;
                }
                if (!isMountedValueTrue(it.value().mounted)) {
                    allMounted = false;
                    break;
                }
            }
            advDatasetOnly = allMounted;
        }
    }
    if (m_btnAdvancedBreakdown) {
        m_btnAdvancedBreakdown->setEnabled(advDatasetOnly);
    }
    if (m_btnAdvancedAssemble) {
        m_btnAdvancedAssemble->setEnabled(advDatasetOnly);
    }
}

bool MainWindow::runLocalCommand(const QString& displayLabel, const QString& command, int timeoutMs) {
    if (!confirmActionExecution(displayLabel, {QStringLiteral("[local]\n%1").arg(command)})) {
        return false;
    }
    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1").arg(displayLabel));
    appLog(QStringLiteral("INFO"), QStringLiteral("$ %1").arg(command));
    QProcess proc;
    proc.start(QStringLiteral("sh"), QStringList{QStringLiteral("-lc"), command});
    if (!proc.waitForStarted(4000)) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("No se pudo iniciar comando local"));
        setActionsLocked(false);
        return false;
    }
    if (timeoutMs > 0) {
        if (!proc.waitForFinished(timeoutMs)) {
            proc.kill();
            proc.waitForFinished(1000);
            appLog(QStringLiteral("NORMAL"), QStringLiteral("Timeout en comando local"));
            setActionsLocked(false);
            return false;
        }
    } else {
        proc.waitForFinished(-1);
    }
    const int rc = proc.exitCode();
    const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    if (!out.isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(err));
    }
    if (rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizó con error %1").arg(rc));
        setActionsLocked(false);
        return false;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Comando finalizado correctamente"));
    setActionsLocked(false);
    return true;
}

void MainWindow::actionCopySnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const QString srcSnap = src.datasetName + QStringLiteral("@") + src.snapshotName;
    const QString recvTarget = dst.datasetName + QStringLiteral("/") + src.datasetName.section('/', -1);

    QString srcSsh = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (sp.port > 0) {
        srcSsh += QStringLiteral(" -p ") + QString::number(sp.port);
    }
    if (!sp.keyPath.isEmpty()) {
        srcSsh += QStringLiteral(" -i ") + shSingleQuote(sp.keyPath);
    }
    srcSsh += QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    QString dstSsh = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (dp.port > 0) {
        dstSsh += QStringLiteral(" -p ") + QString::number(dp.port);
    }
    if (!dp.keyPath.isEmpty()) {
        dstSsh += QStringLiteral(" -i ") + shSingleQuote(dp.keyPath);
    }
    dstSsh += QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    QString sendCmd = withSudo(sp, QStringLiteral("zfs send -wLec %1").arg(shSingleQuote(srcSnap)));
    QString recvCmd = withSudo(dp, QStringLiteral("zfs recv -F %1").arg(shSingleQuote(recvTarget)));

    const QString pipeline =
        srcSsh + QStringLiteral(" ") + shSingleQuote(sendCmd)
        + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab) || cat) | ")
        + dstSsh + QStringLiteral(" ") + shSingleQuote(recvCmd);

    if (runLocalCommand(QStringLiteral("Copiar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionLevelSnapshot() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.snapshotName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];
    const QString srcSnap = src.datasetName + QStringLiteral("@") + src.snapshotName;
    const QString recvTarget = dst.datasetName;

    QString srcSsh = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (sp.port > 0) {
        srcSsh += QStringLiteral(" -p ") + QString::number(sp.port);
    }
    if (!sp.keyPath.isEmpty()) {
        srcSsh += QStringLiteral(" -i ") + shSingleQuote(sp.keyPath);
    }
    srcSsh += QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    QString dstSsh = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (dp.port > 0) {
        dstSsh += QStringLiteral(" -p ") + QString::number(dp.port);
    }
    if (!dp.keyPath.isEmpty()) {
        dstSsh += QStringLiteral(" -i ") + shSingleQuote(dp.keyPath);
    }
    dstSsh += QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    QString sendCmd = withSudo(sp, QStringLiteral("zfs send -wLecR %1").arg(shSingleQuote(srcSnap)));
    QString recvCmd = withSudo(dp, QStringLiteral("zfs recv -F %1").arg(shSingleQuote(recvTarget)));

    const QString pipeline =
        srcSsh + QStringLiteral(" ") + shSingleQuote(sendCmd)
        + QStringLiteral(" | ((command -v pv >/dev/null 2>&1 && pv -trab) || cat) | ")
        + dstSsh + QStringLiteral(" ") + shSingleQuote(recvCmd);

    if (runLocalCommand(QStringLiteral("Nivelar snapshot %1 -> %2").arg(srcSnap, recvTarget), pipeline, 0)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionSyncDatasets() {
    const DatasetSelectionContext src = currentDatasetSelection(QStringLiteral("origin"));
    const DatasetSelectionContext dst = currentDatasetSelection(QStringLiteral("dest"));
    if (!src.valid || !dst.valid || src.datasetName.isEmpty() || dst.datasetName.isEmpty()) {
        return;
    }
    const ConnectionProfile& sp = m_profiles[src.connIdx];
    const ConnectionProfile& dp = m_profiles[dst.connIdx];

    QString srcMp;
    QString dstMp;
    QString srcMounted;
    QString dstMounted;
    if (!getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mountpoint"), srcMp)
        || !getDatasetProperty(src.connIdx, src.datasetName, QStringLiteral("mounted"), srcMounted)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mountpoint"), dstMp)
        || !getDatasetProperty(dst.connIdx, dst.datasetName, QStringLiteral("mounted"), dstMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("No se pudieron leer mountpoints para sincronizar."));
        return;
    }
    if (srcMounted != QStringLiteral("yes") || dstMounted != QStringLiteral("yes")) {
        QMessageBox::warning(this,
                             QStringLiteral("ZFSMgr"),
                             QStringLiteral("Origen y destino deben estar montados para sincronizar.\nOrigen=%1 Destino=%2").arg(srcMounted, dstMounted));
        return;
    }

    QString srcSsh = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (sp.port > 0) {
        srcSsh += QStringLiteral(" -p ") + QString::number(sp.port);
    }
    if (!sp.keyPath.isEmpty()) {
        srcSsh += QStringLiteral(" -i ") + shSingleQuote(sp.keyPath);
    }
    srcSsh += QStringLiteral(" ") + shSingleQuote(sp.username + QStringLiteral("@") + sp.host);
    QString dstSsh = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null");
    if (dp.port > 0) {
        dstSsh += QStringLiteral(" -p ") + QString::number(dp.port);
    }
    if (!dp.keyPath.isEmpty()) {
        dstSsh += QStringLiteral(" -i ") + shSingleQuote(dp.keyPath);
    }
    dstSsh += QStringLiteral(" ") + shSingleQuote(dp.username + QStringLiteral("@") + dp.host);

    QString remoteRsync =
        QStringLiteral("rsync -aHAWXS --delete --info=progress2 -e ")
        + shSingleQuote(QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null")
                        + (dp.port > 0 ? QStringLiteral(" -p ") + QString::number(dp.port) : QString())
                        + (!dp.keyPath.isEmpty() ? QStringLiteral(" -i ") + dp.keyPath : QString()))
        + QStringLiteral(" %1/ %2:%3/")
              .arg(shSingleQuote(srcMp),
                   shSingleQuote(dp.username + QStringLiteral("@") + dp.host),
                   shSingleQuote(dstMp));
    remoteRsync = withSudo(sp, remoteRsync);
    const QString command = srcSsh + QStringLiteral(" ") + shSingleQuote(remoteRsync);
    if (runLocalCommand(QStringLiteral("Sincronizar %1 -> %2").arg(src.datasetName, dst.datasetName), command, 0)) {
        invalidateDatasetCacheForPool(dst.connIdx, dst.poolName);
        reloadDatasetSide(QStringLiteral("dest"));
    }
}

void MainWindow::actionAdvancedBreakdown() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset en Avanzado."));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const ConnectionProfile& p = m_profiles[connIdx];
    QString mountOut;
    QString mountErr;
    int mountRc = -1;
    const QString mountCheckCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o name,value mounted -r %1").arg(shSingleQuote(ds)));
    if (!runSsh(p, mountCheckCmd, 25000, mountOut, mountErr, mountRc) || mountRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    QStringList unmounted;
    for (const QString& ln : mountOut.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = ln.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString name = parts[0].trimmed();
        const QString mountedVal = parts[1].trimmed();
        if (name.isEmpty() || name.contains('@')) {
            continue; // ignorar snapshots
        }
        if (!isMountedValueTrue(mountedVal)) {
            unmounted << name;
        }
    }
    if (!unmounted.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Desglosar requiere dataset y descendientes montados.\nNo montados:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("Break down requires dataset and descendants mounted.\nNot mounted:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("拆分要求数据集及其所有后代已挂载。\n未挂载：\n%1")
                    .arg(unmounted.join('\n'))));
        return;
    }

    QString listOut;
    QString listErr;
    int listRc = -1;
    const QString listCmd = withSudo(
        p,
        QStringLiteral("set -e; DATASET=%1; MP=$(zfs get -H -o value mountpoint \"$DATASET\"); "
                       "[ \"$MP\" = \"none\" ] && exit 2; "
                       "[ -d \"$MP\" ] || exit 0; "
                       "find \"$MP\" -mindepth 1 -maxdepth 1 -type d -printf '%%f\\n' | sort -u")
            .arg(shSingleQuote(ds)));
    if (!runSsh(p, listCmd, 25000, listOut, listErr, listRc) || listRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudieron listar directorios para desglosar."),
                                 QStringLiteral("Could not list directories for breakdown."),
                                 QStringLiteral("无法列出可拆分目录。")));
        return;
    }
    QStringList dirs = listOut.split('\n', Qt::SkipEmptyParts);
    for (QString& d : dirs) {
        d = d.trimmed();
    }
    dirs.removeAll(QString());
    if (dirs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No hay directorios para desglosar en el dataset seleccionado."),
                                     QStringLiteral("No directories available to break down in selected dataset."),
                                     QStringLiteral("所选数据集中没有可拆分目录。")));
        return;
    }
    QStringList selectedDirs;
    if (!selectItemsDialog(
            tr3(QStringLiteral("Desglosar: seleccionar directorios"),
                QStringLiteral("Break down: select directories"),
                QStringLiteral("拆分：选择目录")),
            tr3(QStringLiteral("Seleccione los directorios que desea desglosar en subdatasets."),
                QStringLiteral("Select directories to split into subdatasets."),
                QStringLiteral("请选择要拆分为子数据集的目录。")),
            dirs,
            selectedDirs)) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Desglosar cancelado o sin selección."));
        return;
    }

    QStringList selectedQuoted;
    selectedQuoted.reserve(selectedDirs.size());
    for (const QString& d : selectedDirs) {
        selectedQuoted << shSingleQuote(d);
    }
    const QString selectedList = selectedQuoted.join(' ');

    const QString cmd =
        QStringLiteral("set -e; DATASET=%1; MP=$(zfs get -H -o value mountpoint \"$DATASET\"); "
                       "[ \"$MP\" = \"none\" ] && { echo \"mountpoint=none\"; exit 2; }; "
                       "SELECTED_DIRS=(%2); is_selected_dir(){ for s in \"${SELECTED_DIRS[@]}\"; do [ \"$s\" = \"$1\" ] && return 0; done; return 1; }; "
                       "for d in \"$MP\"/*; do [ -d \"$d\" ] || continue; bn=$(basename \"$d\"); is_selected_dir \"$bn\" || continue; "
                       "zfs list -H -o name \"$DATASET/$bn\" >/dev/null 2>&1 || "
                       "{ zfs create \"$DATASET/$bn\"; rsync -aHAWXS --remove-source-files \"$d\"/ \"$MP/$bn\"/; }; done")
            .arg(shSingleQuote(ds), selectedList);
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Desglosar"), ctx, cmd, 0)) {
        m_advSelectionLabel->setText(ds);
    }
}

void MainWindow::actionAdvancedAssemble() {
    const auto selected = m_advTree->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset en Avanzado."));
        return;
    }
    const QString ds = selected.first()->data(0, Qt::UserRole).toString();
    if (ds.isEmpty()) {
        return;
    }
    const QString token = m_advPoolCombo->currentData().toString();
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    DatasetSelectionContext ctx;
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = poolName;
    ctx.datasetName = ds;
    ctx.snapshotName.clear();

    const ConnectionProfile& p = m_profiles[connIdx];
    QString mountOut;
    QString mountErr;
    int mountRc = -1;
    const QString mountCheckCmd = withSudo(
        p,
        QStringLiteral("zfs get -H -o name,value mounted -r %1").arg(shSingleQuote(ds)));
    if (!runSsh(p, mountCheckCmd, 25000, mountOut, mountErr, mountRc) || mountRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar el estado de montaje del dataset."),
                                 QStringLiteral("Could not verify dataset mount state."),
                                 QStringLiteral("无法检查数据集挂载状态。")));
        return;
    }
    QStringList unmounted;
    for (const QString& ln : mountOut.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = ln.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString name = parts[0].trimmed();
        const QString mountedVal = parts[1].trimmed();
        if (name.isEmpty() || name.contains('@')) {
            continue; // ignorar snapshots
        }
        if (!isMountedValueTrue(mountedVal)) {
            unmounted << name;
        }
    }
    if (!unmounted.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Ensamblar requiere dataset y descendientes montados.\nNo montados:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("Assemble requires dataset and descendants mounted.\nNot mounted:\n%1")
                    .arg(unmounted.join('\n')),
                QStringLiteral("组装要求数据集及其所有后代已挂载。\n未挂载：\n%1")
                    .arg(unmounted.join('\n'))));
        return;
    }

    QString listOut;
    QString listErr;
    int listRc = -1;
    const QString listCmd = withSudo(
        p,
        QStringLiteral("zfs list -H -o name -r %1 | tail -n +2")
            .arg(shSingleQuote(ds)));
    if (!runSsh(p, listCmd, 25000, listOut, listErr, listRc) || listRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudieron listar subdatasets para ensamblar."),
                                 QStringLiteral("Could not list child datasets for assemble."),
                                 QStringLiteral("无法列出可组装子数据集。")));
        return;
    }
    QStringList children = listOut.split('\n', Qt::SkipEmptyParts);
    for (QString& c : children) {
        c = c.trimmed();
    }
    children.removeAll(QString());
    if (children.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"),
                                 tr3(QStringLiteral("No hay subdatasets para ensamblar."),
                                     QStringLiteral("No child datasets available to assemble."),
                                     QStringLiteral("没有可组装的子数据集。")));
        return;
    }
    QStringList selectedChildren;
    if (!selectItemsDialog(
            tr3(QStringLiteral("Ensamblar: seleccionar subdatasets"),
                QStringLiteral("Assemble: select child datasets"),
                QStringLiteral("组装：选择子数据集")),
            tr3(QStringLiteral("Seleccione los subdatasets que desea ensamblar en el dataset padre."),
                QStringLiteral("Select child datasets to assemble into parent dataset."),
                QStringLiteral("请选择要组装回父数据集的子数据集。")),
            children,
            selectedChildren)) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Ensamblar cancelado o sin selección."));
        return;
    }
    QStringList selectedQuoted;
    selectedQuoted.reserve(selectedChildren.size());
    for (const QString& c : selectedChildren) {
        selectedQuoted << shSingleQuote(c);
    }
    const QString selectedList = selectedQuoted.join(' ');

    const QString cmd =
        QStringLiteral("set -e; DATASET=%1; MP=$(zfs get -H -o value mountpoint \"$DATASET\"); "
                       "[ \"$MP\" = \"none\" ] && { echo \"mountpoint=none\"; exit 2; }; "
                       "SELECTED_CHILDREN=(%2); "
                       "for child in \"${SELECTED_CHILDREN[@]}\"; do bn=${child##*/}; "
                       "CMP=$(zfs get -H -o value mountpoint \"$child\"); [ \"$CMP\" = \"none\" ] && continue; "
                       "mkdir -p \"$MP/$bn\"; rsync -aHAWXS \"$CMP\"/ \"$MP/$bn\"/ && zfs destroy -r \"$child\"; done")
            .arg(shSingleQuote(ds), selectedList);
    if (executeDatasetAction(QStringLiteral("origin"), QStringLiteral("Ensamblar"), ctx, cmd, 0)) {
        m_advSelectionLabel->setText(ds);
    }
}

void MainWindow::onDatasetPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || (col != 1 && col != 2)) {
        return;
    }
    QTableWidgetItem* pk = m_datasetPropsTable->item(row, 0);
    QTableWidgetItem* pv = m_datasetPropsTable->item(row, 1);
    QTableWidgetItem* pi = m_datasetPropsTable->item(row, 2);
    if (!pk || !pv || !pi) {
        return;
    }
    Q_UNUSED(pk);
    Q_UNUSED(pv);
    Q_UNUSED(pi);
    m_propsDirty = false;
    for (int r = 0; r < m_datasetPropsTable->rowCount(); ++r) {
        QTableWidgetItem* rk = m_datasetPropsTable->item(r, 0);
        QTableWidgetItem* rv = m_datasetPropsTable->item(r, 1);
        QTableWidgetItem* ri = m_datasetPropsTable->item(r, 2);
        if (!rk || !rv || !ri) {
            continue;
        }
        const QString key = rk->text().trimmed();
        const bool inh = (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
        if (inh != m_propsOriginalInherit.value(key, false)
            || rv->text() != m_propsOriginalValues.value(key)) {
            m_propsDirty = true;
            break;
        }
    }
    updateApplyPropsButtonState();
}

void MainWindow::onAdvancedPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || (col != 1 && col != 2) || !m_advPropsTable) {
        return;
    }
    Q_UNUSED(row);
    m_advPropsDirty = false;
    for (int r = 0; r < m_advPropsTable->rowCount(); ++r) {
        QTableWidgetItem* rk = m_advPropsTable->item(r, 0);
        QTableWidgetItem* rv = m_advPropsTable->item(r, 1);
        QTableWidgetItem* ri = m_advPropsTable->item(r, 2);
        if (!rk || !rv || !ri) {
            continue;
        }
        const QString key = rk->text().trimmed();
        const bool inh = (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
        if (inh != m_advPropsOriginalInherit.value(key, false)
            || rv->text() != m_advPropsOriginalValues.value(key)) {
            m_advPropsDirty = true;
            break;
        }
    }
    updateApplyPropsButtonState();
}

void MainWindow::applyDatasetPropertyChanges() {
    if (actionsLocked()) {
        return;
    }
    if (!m_propsDirty || m_propsDataset.isEmpty() || m_propsSide.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    if (!ctx.valid || ctx.datasetName != m_propsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset activo para aplicar cambios."));
        return;
    }

    QStringList subcmds;
    for (int r = 0; r < m_datasetPropsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = m_datasetPropsTable->item(r, 0);
        QTableWidgetItem* pv = m_datasetPropsTable->item(r, 1);
        QTableWidgetItem* pi = m_datasetPropsTable->item(r, 2);
        if (!pk || !pv || !pi) {
            continue;
        }
        const QString prop = pk->text().trimmed();
        if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado")) {
            continue;
        }
        const bool inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
        if (inheritChecked) {
            subcmds << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(prop), shSingleQuote(ctx.datasetName));
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_propsOriginalValues.value(prop).trimmed();
        if (now == old) {
            continue;
        }
        const QString assign = prop + QStringLiteral("=") + now;
        subcmds << QStringLiteral("zfs set %1 %2").arg(shSingleQuote(assign), shSingleQuote(ctx.datasetName));
    }
    if (subcmds.isEmpty()) {
        m_propsDirty = false;
        updateApplyPropsButtonState();
        return;
    }

    const QString cmd = QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
    if (executeDatasetAction(m_propsSide, QStringLiteral("Aplicar propiedades"), ctx, cmd, 60000)) {
        m_propsDirty = false;
        updateApplyPropsButtonState();
    }
}

void MainWindow::applyAdvancedDatasetPropertyChanges() {
    if (actionsLocked()) {
        return;
    }
    if (!m_advPropsDirty || m_advPropsDataset.isEmpty() || !m_advPropsTable) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("advanced"));
    if (!ctx.valid || ctx.datasetName != m_advPropsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Seleccione un dataset activo para aplicar cambios."));
        return;
    }

    QStringList subcmds;
    for (int r = 0; r < m_advPropsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = m_advPropsTable->item(r, 0);
        QTableWidgetItem* pv = m_advPropsTable->item(r, 1);
        QTableWidgetItem* pi = m_advPropsTable->item(r, 2);
        if (!pk || !pv || !pi) {
            continue;
        }
        const QString prop = pk->text().trimmed();
        if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado")) {
            continue;
        }
        const bool inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
        if (inheritChecked) {
            subcmds << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(prop), shSingleQuote(ctx.datasetName));
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_advPropsOriginalValues.value(prop).trimmed();
        if (now == old) {
            continue;
        }
        const QString assign = prop + QStringLiteral("=") + now;
        subcmds << QStringLiteral("zfs set %1 %2").arg(shSingleQuote(assign), shSingleQuote(ctx.datasetName));
    }
    if (subcmds.isEmpty()) {
        m_advPropsDirty = false;
        updateApplyPropsButtonState();
        return;
    }

    const QString cmd = QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
    if (executeDatasetAction(QStringLiteral("advanced"), QStringLiteral("Aplicar propiedades"), ctx, cmd, 60000)) {
        m_advPropsDirty = false;
        updateApplyPropsButtonState();
    }
}

void MainWindow::updateApplyPropsButtonState() {
    const DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    const bool eligible = ctx.valid && ctx.snapshotName.isEmpty() && (ctx.datasetName == m_propsDataset);
    auto hasEffectiveChanges = [](QTableWidget* table,
                                  const QMap<QString, QString>& originals,
                                  const QMap<QString, bool>& originalInherit) -> bool {
        if (!table) {
            return false;
        }
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem* pk = table->item(r, 0);
            QTableWidgetItem* pv = table->item(r, 1);
            QTableWidgetItem* pi = table->item(r, 2);
            if (!pk || !pv || !pi) {
                continue;
            }
            const QString prop = pk->text().trimmed();
            if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado")) {
                continue;
            }
            const bool inh = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
            const QString now = pv->text();
            if (inh != originalInherit.value(prop, false) || now != originals.value(prop)) {
                return true;
            }
        }
        return false;
    };
    const bool hasChanges = hasEffectiveChanges(m_datasetPropsTable, m_propsOriginalValues, m_propsOriginalInherit);
    m_btnApplyDatasetProps->setEnabled(m_propsDirty && eligible && hasChanges);
    const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("advanced"));
    const bool aok = actx.valid && actx.snapshotName.isEmpty() && (actx.datasetName == m_advPropsDataset);
    if (m_btnApplyAdvancedProps) {
        const bool advHasChanges =
            hasEffectiveChanges(m_advPropsTable, m_advPropsOriginalValues, m_advPropsOriginalInherit);
        m_btnApplyAdvancedProps->setEnabled(m_advPropsDirty && aok && advHasChanges);
    }
}

void MainWindow::initLogPersistence() {
    const QString dir = m_store.configDir();
    if (dir.isEmpty()) {
        return;
    }
    m_appLogPath = dir + "/application.log";
    rotateLogIfNeeded();
}

void MainWindow::rotateLogIfNeeded() {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    constexpr qint64 maxBytes = 2 * 1024 * 1024;
    constexpr int backups = 5;

    QFileInfo fi(m_appLogPath);
    if (!fi.exists() || fi.size() < maxBytes) {
        return;
    }

    for (int i = backups; i >= 1; --i) {
        const QString src = (i == 1) ? m_appLogPath : (m_appLogPath + "." + QString::number(i - 1));
        const QString dst = m_appLogPath + "." + QString::number(i);
        if (QFile::exists(dst)) {
            QFile::remove(dst);
        }
        if (QFile::exists(src)) {
            QFile::rename(src, dst);
        }
    }
}

void MainWindow::appendLogToFile(const QString& line) {
    if (m_appLogPath.isEmpty()) {
        return;
    }
    rotateLogIfNeeded();
    QFile f(m_appLogPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << maskSecrets(line) << '\n';
    ts.flush();
}

void MainWindow::clearAppLog() {
    m_logView->clear();
    for (auto it = m_connectionLogViews.begin(); it != m_connectionLogViews.end(); ++it) {
        if (it.value()) {
            it.value()->clear();
        }
    }
    if (m_lastDetailText) {
        m_lastDetailText->clear();
    }
    if (!m_appLogPath.isEmpty()) {
        QFile f(m_appLogPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.close();
        }
    }
}

void MainWindow::copyAppLogToClipboard() {
    QClipboard* cb = QApplication::clipboard();
    if (!cb) {
        return;
    }
    QString text = QStringLiteral("[Aplicación]\n") + m_logView->toPlainText();
    for (auto it = m_connectionLogViews.constBegin(); it != m_connectionLogViews.constEnd(); ++it) {
        const QString connId = it.key();
        const QPlainTextEdit* view = it.value();
        QString connName = connId;
        for (const auto& p : m_profiles) {
            if (p.id == connId) {
                connName = p.name;
                break;
            }
        }
        text += QStringLiteral("\n\n[%1]\n%2").arg(connName, view ? view->toPlainText() : QString());
    }
    cb->setText(text);
}

int MainWindow::findConnectionIndexByName(const QString& name) const {
    const QString key = name.trimmed();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].name.compare(key, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

void MainWindow::refreshConnectionByIndex(int idx) {
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    m_states[idx] = refreshConnection(m_profiles[idx]);
    rebuildConnectionList();
    rebuildDatasetPoolSelectors();
    populateAllPoolsTables();
}

void MainWindow::exportPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    if (!connItem || !poolItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        QStringLiteral("Exportar pool"),
        QStringLiteral("¿Exportar pool %1 en %2?").arg(poolName, connName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, QStringLiteral("zpool export %1").arg(shSingleQuote(poolName)));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Exportar"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio exportar %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error exportando %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("Exportar falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin exportar %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
}

void MainWindow::importPoolFromRow(int row) {
    if (actionsLocked()) {
        return;
    }
    QTableWidgetItem* connItem = m_importablePoolsTable->item(row, 1);
    QTableWidgetItem* poolItem = m_importablePoolsTable->item(row, 2);
    QTableWidgetItem* stateItem = m_importablePoolsTable->item(row, 3);
    if (!connItem || !poolItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    const QString poolState = stateItem ? stateItem->text().trimmed().toUpper() : QString();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    if (poolState != QStringLiteral("ONLINE")) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Importar omitido %1::%2 (state=%3)")
                                      .arg(connName, poolName, poolState.isEmpty() ? QStringLiteral("UNKNOWN") : poolState));
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0) {
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Importar pool: %1").arg(poolName));
    dlg.setModal(true);
    auto* lay = new QVBoxLayout(&dlg);

    auto* flagsBox = new QGroupBox(QStringLiteral("Flags"), &dlg);
    auto* flagsLay = new QGridLayout(flagsBox);
    QCheckBox* forceCb = new QCheckBox(QStringLiteral("-f force"), flagsBox);
    QCheckBox* missingLogCb = new QCheckBox(QStringLiteral("-m missing log"), flagsBox);
    QCheckBox* noMountCb = new QCheckBox(QStringLiteral("-N do not mount"), flagsBox);
    QCheckBox* rewindCb = new QCheckBox(QStringLiteral("-F rewind"), flagsBox);
    QCheckBox* dryRunCb = new QCheckBox(QStringLiteral("-n dry run"), flagsBox);
    QCheckBox* destroyedCb = new QCheckBox(QStringLiteral("-D destroyed"), flagsBox);
    QCheckBox* extremeCb = new QCheckBox(QStringLiteral("-X extreme rewind"), flagsBox);
    QCheckBox* loadKeysCb = new QCheckBox(QStringLiteral("-l load keys"), flagsBox);
    flagsLay->addWidget(forceCb, 0, 0);
    flagsLay->addWidget(missingLogCb, 0, 1);
    flagsLay->addWidget(noMountCb, 1, 0);
    flagsLay->addWidget(rewindCb, 1, 1);
    flagsLay->addWidget(dryRunCb, 2, 0);
    flagsLay->addWidget(destroyedCb, 2, 1);
    flagsLay->addWidget(extremeCb, 3, 0);
    flagsLay->addWidget(loadKeysCb, 3, 1);
    lay->addWidget(flagsBox);

    auto* fieldsBox = new QGroupBox(QStringLiteral("Valores"), &dlg);
    auto* form = new QFormLayout(fieldsBox);
    QLineEdit* cachefileEd = new QLineEdit(fieldsBox);
    QLineEdit* altrootEd = new QLineEdit(fieldsBox);
    QLineEdit* dirsEd = new QLineEdit(fieldsBox);
    QLineEdit* mntoptsEd = new QLineEdit(fieldsBox);
    QLineEdit* propsEd = new QLineEdit(fieldsBox);
    QLineEdit* txgEd = new QLineEdit(fieldsBox);
    QLineEdit* newNameEd = new QLineEdit(fieldsBox);
    QLineEdit* extraEd = new QLineEdit(fieldsBox);
    form->addRow(QStringLiteral("cachefile"), cachefileEd);
    form->addRow(QStringLiteral("altroot"), altrootEd);
    form->addRow(QStringLiteral("directories (, )"), dirsEd);
    form->addRow(QStringLiteral("mntopts"), mntoptsEd);
    form->addRow(QStringLiteral("properties (, )"), propsEd);
    form->addRow(QStringLiteral("txg"), txgEd);
    form->addRow(QStringLiteral("new name"), newNameEd);
    form->addRow(QStringLiteral("extra args"), extraEd);
    lay->addWidget(fieldsBox);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    QStringList parts;
    parts << QStringLiteral("zpool import");
    if (forceCb->isChecked()) {
        parts << QStringLiteral("-f");
    }
    if (missingLogCb->isChecked()) {
        parts << QStringLiteral("-m");
    }
    if (noMountCb->isChecked()) {
        parts << QStringLiteral("-N");
    }
    if (rewindCb->isChecked()) {
        parts << QStringLiteral("-F");
    }
    if (dryRunCb->isChecked()) {
        parts << QStringLiteral("-n");
    }
    if (destroyedCb->isChecked()) {
        parts << QStringLiteral("-D");
    }
    if (extremeCb->isChecked()) {
        parts << QStringLiteral("-X");
    }
    if (loadKeysCb->isChecked()) {
        parts << QStringLiteral("-l");
    }
    if (!cachefileEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-c") << shSingleQuote(cachefileEd->text().trimmed());
    }
    if (!altrootEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-R") << shSingleQuote(altrootEd->text().trimmed());
    }
    for (const QString& d : dirsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString dd = d.trimmed();
        if (!dd.isEmpty()) {
            parts << QStringLiteral("-d") << shSingleQuote(dd);
        }
    }
    if (!mntoptsEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-o") << shSingleQuote(mntoptsEd->text().trimmed());
    }
    for (const QString& pval : propsEd->text().split(',', Qt::SkipEmptyParts)) {
        const QString pp = pval.trimmed();
        if (!pp.isEmpty()) {
            parts << QStringLiteral("-o") << shSingleQuote(pp);
        }
    }
    if (!txgEd->text().trimmed().isEmpty()) {
        parts << QStringLiteral("-T") << shSingleQuote(txgEd->text().trimmed());
    }
    parts << shSingleQuote(poolName);
    if (!newNameEd->text().trimmed().isEmpty()) {
        parts << shSingleQuote(newNameEd->text().trimmed());
    }
    if (!extraEd->text().trimmed().isEmpty()) {
        parts << extraEd->text().trimmed();
    }

    const ConnectionProfile& p = m_profiles[idx];
    const QString cmd = withSudo(p, parts.join(' '));
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, cmd));
    if (!confirmActionExecution(QStringLiteral("Importar"), {preview})) {
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio importar %1::%2").arg(connName, poolName));
    setActionsLocked(true);
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, cmd, 45000, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Error importando %1::%2 -> %3")
                                       .arg(connName, poolName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("Importar falló:\n%1").arg(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return;
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin importar %1::%2").arg(connName, poolName));
    setActionsLocked(false);
    refreshConnectionByIndex(idx);
}

MainWindow::DatasetSelectionContext MainWindow::currentDatasetSelection(const QString& side) const {
    DatasetSelectionContext ctx;
    QString token;
    QString ds;
    QString snap;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo->currentData().toString();
        ds = m_originSelectedDataset;
        snap = m_originSelectedSnapshot;
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo->currentData().toString();
        ds = m_destSelectedDataset;
        snap = m_destSelectedSnapshot;
    } else {
        token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
        if (m_advTree) {
            const auto selected = m_advTree->selectedItems();
            if (!selected.isEmpty()) {
                ds = selected.first()->data(0, Qt::UserRole).toString();
                snap = selected.first()->data(1, Qt::UserRole).toString();
            }
        }
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        return ctx;
    }
    const int connIdx = token.left(sep).toInt();
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return ctx;
    }
    const QString pool = token.mid(sep + 2);
    if (ds.isEmpty()) {
        return ctx;
    }
    ctx.valid = true;
    ctx.connIdx = connIdx;
    ctx.poolName = pool;
    ctx.datasetName = ds;
    ctx.snapshotName = snap;
    return ctx;
}

void MainWindow::showDatasetContextMenu(const QString& side, QTreeWidget* tree, const QPoint& pos) {
    if (actionsLocked()) {
        return;
    }
    QTreeWidgetItem* item = tree->itemAt(pos);
    if (!item) {
        return;
    }
    tree->setCurrentItem(item);
    if (side == QStringLiteral("origin")) {
        onOriginTreeSelectionChanged();
    } else if (side == QStringLiteral("dest")) {
        onDestTreeSelectionChanged();
    } else {
        refreshDatasetProperties(QStringLiteral("advanced"));
        if (m_advSelectionLabel) {
            const QString ds = item->data(0, Qt::UserRole).toString();
            const QString snap = item->data(1, Qt::UserRole).toString();
            if (ds.isEmpty()) {
                m_advSelectionLabel->setText(tr3(QStringLiteral("(sin selección)"),
                                                 QStringLiteral("(no selection)"),
                                                 QStringLiteral("（未选择）")));
            } else if (snap.isEmpty()) {
                m_advSelectionLabel->setText(ds);
            } else {
                m_advSelectionLabel->setText(QStringLiteral("%1@%2").arg(ds, snap));
            }
        }
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid) {
        return;
    }

    QMenu menu(this);
    QAction* mountAct = menu.addAction(QStringLiteral("Montar"));
    QAction* mountWithChildrenAct = menu.addAction(QStringLiteral("Montar con todos los hijos"));
    QAction* umountAct = menu.addAction(QStringLiteral("Desmontar"));
    menu.addSeparator();
    QAction* createAct = menu.addAction(QStringLiteral("Crear hijo"));
    QAction* deleteAct = menu.addAction(QStringLiteral("Borrar"));

    if (!ctx.snapshotName.isEmpty()) {
        mountAct->setEnabled(false);
        mountWithChildrenAct->setEnabled(false);
        umountAct->setEnabled(false);
        createAct->setEnabled(false);
    } else {
        bool knownMounted = false;
        bool isMounted = false;
        const QString key = datasetCacheKey(ctx.connIdx, ctx.poolName);
        const auto cacheIt = m_poolDatasetCache.constFind(key);
        if (cacheIt != m_poolDatasetCache.constEnd()) {
            const auto recIt = cacheIt->recordByName.constFind(ctx.datasetName);
            if (recIt != cacheIt->recordByName.constEnd()) {
                const QString m = recIt->mounted.trimmed().toLower();
                if (m == QStringLiteral("yes") || m == QStringLiteral("on") || m == QStringLiteral("true") || m == QStringLiteral("1")) {
                    knownMounted = true;
                    isMounted = true;
                } else if (m == QStringLiteral("no") || m == QStringLiteral("off") || m == QStringLiteral("false") || m == QStringLiteral("0")) {
                    knownMounted = true;
                    isMounted = false;
                }
            }
        }
        if (knownMounted) {
            mountAct->setEnabled(!isMounted);
            umountAct->setEnabled(isMounted);
        }
    }

    QAction* picked = menu.exec(tree->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == mountAct) {
        logUiAction(QStringLiteral("Montar dataset (menú)"));
        actionMountDataset(side);
    } else if (picked == mountWithChildrenAct) {
        logUiAction(QStringLiteral("Montar dataset con hijos (menú)"));
        actionMountDatasetWithChildren(side);
    } else if (picked == umountAct) {
        logUiAction(QStringLiteral("Desmontar dataset (menú)"));
        actionUmountDataset(side);
    } else if (picked == createAct) {
        logUiAction(QStringLiteral("Crear hijo dataset (menú)"));
        actionCreateChildDataset(side);
    } else if (picked == deleteAct) {
        logUiAction(QStringLiteral("Borrar dataset/snapshot (menú)"));
        actionDeleteDatasetOrSnapshot(side);
    }
}

bool MainWindow::executeDatasetAction(const QString& side, const QString& actionName, const DatasetSelectionContext& ctx, const QString& cmd, int timeoutMs) {
    if (!ctx.valid) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString remoteCmd = withSudo(p, cmd);
    const QString preview = QStringLiteral("[%1]\n%2")
                                .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                .arg(buildSshPreviewCommand(p, remoteCmd));
    if (!confirmActionExecution(actionName, {preview})) {
        return false;
    }
    setActionsLocked(true);
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 %2::%3").arg(actionName, p.name, ctx.datasetName));
    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, remoteCmd, timeoutMs, out, err, rc) || rc != 0) {
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Error en %1: %2")
                   .arg(actionName, oneLine(err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err)));
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"), QStringLiteral("%1 falló:\n%2").arg(actionName, err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err));
        setActionsLocked(false);
        return false;
    }
    if (!out.trimmed().isEmpty()) {
        appLog(QStringLiteral("INFO"), oneLine(out));
    }
    appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 finalizado").arg(actionName));
    invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
    reloadDatasetSide(side);
    if (actionName == QStringLiteral("Montar")
        || actionName == QStringLiteral("Montar con todos los hijos")
        || actionName == QStringLiteral("Desmontar")) {
        refreshConnectionByIndex(ctx.connIdx);
    }
    setActionsLocked(false);
    return true;
}

void MainWindow::invalidateDatasetCacheForPool(int connIdx, const QString& poolName) {
    m_poolDatasetCache.remove(datasetCacheKey(connIdx, poolName));
}

void MainWindow::reloadDatasetSide(const QString& side) {
    if (side == QStringLiteral("origin")) {
        onOriginPoolChanged();
    } else if (side == QStringLiteral("dest")) {
        onDestPoolChanged();
    } else {
        const QString token = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep > 0) {
            const int connIdx = token.left(sep).toInt();
            const QString poolName = token.mid(sep + 2);
            populateDatasetTree(m_advTree, connIdx, poolName, QStringLiteral("origin"));
            refreshDatasetProperties(QStringLiteral("advanced"));
        }
    }
}

void MainWindow::actionMountDataset(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    if (!ensureParentMountedBeforeMount(ctx, side)) {
        return;
    }
    if (!ensureNoMountpointConflictsBeforeMount(ctx, false)) {
        return;
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    const QString cmd = QStringLiteral("zfs mount %1").arg(dsQ);
    executeDatasetAction(side, QStringLiteral("Montar"), ctx, cmd);
}

void MainWindow::actionMountDatasetWithChildren(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    if (!ensureParentMountedBeforeMount(ctx, side)) {
        return;
    }
    if (!ensureNoMountpointConflictsBeforeMount(ctx, true)) {
        return;
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    const QString cmd = QStringLiteral(
                            "set -e; DATASET=%1; "
                            "zfs list -H -o name -r \"$DATASET\" | "
                            "while IFS= read -r child; do "
                            "  [ -n \"$child\" ] || continue; "
                            "  mounted=$(zfs get -H -o value mounted \"$child\" 2>/dev/null || true); "
                            "  case \"$mounted\" in yes|on|true|1) : ;; *) zfs mount \"$child\" ;; esac; "
                            "done")
                            .arg(dsQ);
    executeDatasetAction(side, QStringLiteral("Montar con todos los hijos"), ctx, cmd);
}

bool MainWindow::ensureParentMountedBeforeMount(const DatasetSelectionContext& ctx, const QString& side) {
    Q_UNUSED(side);
    if (!ctx.valid || ctx.datasetName.isEmpty()) {
        return false;
    }
    const QString parent = parentDatasetName(ctx.datasetName);
    if (parent.isEmpty()) {
        return true;
    }

    QString parentMountpoint;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("mountpoint"), parentMountpoint)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar mountpoint del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mountpoint %1").arg(parent),
                                 QStringLiteral("无法检查父数据集 mountpoint：%1").arg(parent)));
        return false;
    }
    const QString mp = parentMountpoint.trimmed().toLower();
    if (mp.isEmpty() || mp == QStringLiteral("none")) {
        return true;
    }

    QString parentMounted;
    if (!getDatasetProperty(ctx.connIdx, parent, QStringLiteral("mounted"), parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar estado mounted del padre %1").arg(parent),
                                 QStringLiteral("Could not verify parent mounted state %1").arg(parent),
                                 QStringLiteral("无法检查父数据集挂载状态：%1").arg(parent)));
        return false;
    }
    if (!isMountedValueTrue(parentMounted)) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("El dataset padre %1 no está montado, móntelo antes por favor").arg(parent),
                                 QStringLiteral("Parent dataset %1 is not mounted, mount it first").arg(parent),
                                 QStringLiteral("父数据集 %1 未挂载，请先挂载").arg(parent)));
        return false;
    }
    return true;
}

bool MainWindow::ensureNoMountpointConflictsBeforeMount(const DatasetSelectionContext& ctx, bool includeDescendants) {
    if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size() || ctx.datasetName.isEmpty()) {
        return false;
    }
    const ConnectionProfile& p = m_profiles[ctx.connIdx];

    QString targetsOut;
    QString targetsErr;
    int targetsRc = -1;
    const QString targetsCmd = includeDescendants
                                   ? withSudo(p, QStringLiteral("zfs get -H -o name,value mountpoint -r %1")
                                                     .arg(shSingleQuote(ctx.datasetName)))
                                   : withSudo(p, QStringLiteral("zfs get -H -o name,value mountpoint %1")
                                                     .arg(shSingleQuote(ctx.datasetName)));
    if (!runSsh(p, targetsCmd, 20000, targetsOut, targetsErr, targetsRc) || targetsRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo comprobar conflictos de mountpoint."),
                                 QStringLiteral("Could not validate mountpoint conflicts."),
                                 QStringLiteral("无法检查挂载点冲突。")));
        return false;
    }

    QMap<QString, QString> targetMpByDs;
    QMap<QString, QStringList> targetDsByMp;
    for (const QString& ln : targetsOut.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = ln.split('\t');
        if (parts.size() < 2) {
            continue;
        }
        const QString ds = parts[0].trimmed();
        const QString mp = parts[1].trimmed();
        const QString mpl = mp.toLower();
        if (ds.isEmpty() || mp.isEmpty() || mpl == QStringLiteral("none") || mpl == QStringLiteral("-")) {
            continue;
        }
        targetMpByDs[ds] = mp;
        targetDsByMp[mp].push_back(ds);
    }

    for (auto it = targetDsByMp.constBegin(); it != targetDsByMp.constEnd(); ++it) {
        const QStringList dsList = it.value();
        if (dsList.size() > 1) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                tr3(QStringLiteral("Conflicto de mountpoint dentro de la selección.\nMountpoint: %1\nDatasets:\n%2")
                        .arg(it.key(), dsList.join('\n')),
                    QStringLiteral("Mountpoint conflict inside selection.\nMountpoint: %1\nDatasets:\n%2")
                        .arg(it.key(), dsList.join('\n')),
                    QStringLiteral("所选项内部存在挂载点冲突。\n挂载点：%1\n数据集：\n%2")
                        .arg(it.key(), dsList.join('\n'))));
            return false;
        }
    }

    QString mountedOut;
    QString mountedErr;
    int mountedRc = -1;
    const QString mountedCmd = withSudo(p, QStringLiteral("zfs mount"));
    if (!runSsh(p, mountedCmd, 20000, mountedOut, mountedErr, mountedRc) || mountedRc != 0) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             tr3(QStringLiteral("No se pudo leer datasets montados."),
                                 QStringLiteral("Could not read mounted datasets."),
                                 QStringLiteral("无法读取已挂载数据集。")));
        return false;
    }

    QMap<QString, QStringList> mountedByMp;
    for (const QString& ln : mountedOut.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = ln.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const int sp = trimmed.indexOf(' ');
        if (sp <= 0) {
            continue;
        }
        const QString ds = trimmed.left(sp).trimmed();
        const QString mp = trimmed.mid(sp + 1).trimmed();
        if (ds.isEmpty() || mp.isEmpty()) {
            continue;
        }
        mountedByMp[mp].push_back(ds);
    }

    for (auto it = targetMpByDs.constBegin(); it != targetMpByDs.constEnd(); ++it) {
        const QString targetDs = it.key();
        const QString mp = it.value();
        const QStringList mountedDs = mountedByMp.value(mp);
        for (const QString& dsMounted : mountedDs) {
            if (dsMounted != targetDs) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("ZFSMgr"),
                    tr3(QStringLiteral("No se permite montar más de un dataset en el mismo directorio.\nMountpoint: %1\nMontado: %2\nSolicitado: %3")
                            .arg(mp, dsMounted, targetDs),
                        QStringLiteral("Only one mounted dataset per directory is allowed.\nMountpoint: %1\nMounted: %2\nRequested: %3")
                            .arg(mp, dsMounted, targetDs),
                        QStringLiteral("同一目录不允许挂载多个数据集。\n挂载点：%1\n已挂载：%2\n请求：%3")
                            .arg(mp, dsMounted, targetDs)));
                return false;
            }
        }
    }
    return true;
}

void MainWindow::actionUmountDataset(const QString& side) {
    if (actionsLocked()) {
        return;
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(side);
    if (!ctx.valid || !ctx.snapshotName.isEmpty()) {
        return;
    }
    const QString dsQ = shSingleQuote(ctx.datasetName);
    const QString hasChildrenCmd = QStringLiteral("zfs mount | awk '{print $1}' | grep -E '^%1/' -q").arg(ctx.datasetName);

    QString out;
    QString err;
    int rc = -1;
    const ConnectionProfile& p = m_profiles[ctx.connIdx];
    QString checkCmd = withSudo(p, hasChildrenCmd);
    const bool ran = runSsh(p, checkCmd, 12000, out, err, rc);
    bool hasChildrenMounted = ran && rc == 0;
    QString cmd;
    if (hasChildrenMounted) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("ZFSMgr"),
            QStringLiteral("Hay hijos montados bajo %1.\n¿Desmontar recursivamente?").arg(ctx.datasetName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            appLog(QStringLiteral("INFO"), QStringLiteral("Desmontar abortado por usuario"));
            return;
        }
        cmd = QStringLiteral(
            "zfs mount | awk '{print $1}' | grep -E '^%1(/|$)' | awk '{print length, $0}' | sort -rn | cut -d' ' -f2- | "
            "while IFS= read -r ds; do [ -n \"$ds\" ] && zfs umount \"$ds\"; done")
                  .arg(ctx.datasetName);
    } else {
        cmd = QStringLiteral("zfs umount %1").arg(dsQ);
    }
    executeDatasetAction(side, QStringLiteral("Desmontar"), ctx, cmd);
}

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

        QString curPath = pathEdit->text().trimmed();
        if (isSnapshot) {
            if (!curPath.contains('@')) {
                QString base = ctx.datasetName;
                if (base.isEmpty()) {
                    base = curPath.section('@', 0, 0);
                }
                if (!base.isEmpty()) {
                    pathEdit->setText(base + QStringLiteral("@snap"));
                }
            }
        } else if (curPath.contains('@')) {
            pathEdit->setText(curPath.section('@', 0, 0));
        }
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
        QStringLiteral("Confirmar borrado"),
        QStringLiteral("Se va a borrar:\n%1\n¿Continuar?").arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm1 != QMessageBox::Yes) {
        return;
    }
    const auto confirm2 = QMessageBox::question(
        this,
        QStringLiteral("Confirmar borrado (2/2)"),
        QStringLiteral("Confirmación final de borrado:\n%1").arg(target),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm2 != QMessageBox::Yes) {
        return;
    }

    bool recursive = false;
    if (ctx.snapshotName.isEmpty()) {
        const auto askRec = QMessageBox::question(
            this,
            QStringLiteral("Borrado recursivo"),
            QStringLiteral("¿Borrar recursivamente datasets/snapshots hijos?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        recursive = (askRec == QMessageBox::Yes);
    }
    QString cmd;
    if (ctx.snapshotName.isEmpty()) {
        cmd = recursive ? QStringLiteral("zfs destroy -r %1").arg(shSingleQuote(target))
                        : QStringLiteral("zfs destroy %1").arg(shSingleQuote(target));
    } else {
        cmd = QStringLiteral("zfs destroy %1").arg(shSingleQuote(target));
    }
    executeDatasetAction(side, QStringLiteral("Borrar"), ctx, cmd, 90000);
}

MainWindow::ConnectionRuntimeState MainWindow::refreshConnection(const ConnectionProfile& p) {
    ConnectionRuntimeState state;
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Inicio refresh: %1 [%2]").arg(p.name, p.connType));

    if (p.connType.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Tipo de conexión no soportado aún en cppqt");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }
    if (p.host.isEmpty() || p.username.isEmpty()) {
        state.status = QStringLiteral("ERROR");
        state.detail = QStringLiteral("Host/usuario no definido");
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }

    QString out;
    QString err;
    int rc = -1;
    if (!runSsh(p, QStringLiteral("uname -a"), 12000, out, err, rc) || rc != 0) {
        state.status = QStringLiteral("ERROR");
        state.detail = oneLine(err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }
    state.status = QStringLiteral("OK");
    state.detail = oneLine(out);
    state.zfsVersion.clear();

    out.clear();
    err.clear();
    rc = -1;
    const QString zfsVersionCmd = withSudo(
        p,
        QStringLiteral("(command -v zfs >/dev/null 2>&1 && zfs version) || "
                       "([ -x /usr/local/zfs/bin/zfs ] && /usr/local/zfs/bin/zfs version) || "
                       "([ -x /sbin/zfs ] && /sbin/zfs version)"));
    if (runSsh(p, zfsVersionCmd, 12000, out, err, rc) && rc == 0) {
        state.zfsVersion = parseOpenZfsVersionText(out + QStringLiteral("\n") + err);
    }

    QString zpoolListCmd = withSudo(p, QStringLiteral("zpool list -H -p -o name,size,alloc,free,cap,dedupratio"));

    out.clear();
    err.clear();
    rc = -1;
    if (runSsh(p, zpoolListCmd, 18000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString poolName = line.section('\t', 0, 0).trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            state.importedPools.push_back(PoolImported{p.name, poolName, QStringLiteral("Exportar")});
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zpool list -> %2").arg(p.name, oneLine(err)));
    }

    auto parseImportableStructured = [&](const QString& text) -> QVector<PoolImportable> {
        QVector<PoolImportable> rows;
        const QRegularExpression poolNameRx(QStringLiteral("^[A-Za-z0-9_.:-]+$"));
        QString currentPool;
        QString currentState;
        QString currentReason;
        bool collectingStatus = false;
        auto flushCurrent = [&]() {
            if (currentPool.isEmpty()) {
                return;
            }
            if (!poolNameRx.match(currentPool).hasMatch()) {
                currentPool.clear();
                currentState.clear();
                currentReason.clear();
                collectingStatus = false;
                return;
            }
            // Evita falsos positivos: un bloque válido debe tener al menos state o status.
            if (currentState.isEmpty() && currentReason.isEmpty()) {
                currentPool.clear();
                collectingStatus = false;
                return;
            }
            rows.push_back(PoolImportable{
                p.name,
                currentPool,
                currentState.isEmpty() ? QStringLiteral("UNKNOWN") : currentState,
                currentReason,
                QStringLiteral("Importar"),
            });
            currentPool.clear();
            currentState.clear();
            currentReason.clear();
            collectingStatus = false;
        };
        const QStringList lines = text.split('\n');
        for (QString line : lines) {
            line = line.trimmed();
            if (line.startsWith(QStringLiteral("pool: "))) {
                flushCurrent();
                currentPool = line.mid(QStringLiteral("pool: ").size()).trimmed();
                continue;
            }
            if (currentPool.isEmpty()) {
                continue;
            }
            if (line.startsWith(QStringLiteral("state: "))) {
                currentState = line.mid(QStringLiteral("state: ").size()).trimmed();
                collectingStatus = false;
                continue;
            }
            if (line.startsWith(QStringLiteral("status: "))) {
                currentReason = line.mid(QStringLiteral("status: ").size()).trimmed();
                collectingStatus = true;
                continue;
            }
            if (collectingStatus) {
                if (line.startsWith(QStringLiteral("action:")) || line.startsWith(QStringLiteral("see:")) || line.startsWith(QStringLiteral("config:"))) {
                    collectingStatus = false;
                } else if (!line.isEmpty()) {
                    currentReason = (currentReason + QStringLiteral(" ") + line).trimmed();
                    continue;
                }
            }
            if (line.startsWith(QStringLiteral("cannot import"))) {
                if (!currentReason.isEmpty()) {
                    currentReason += QStringLiteral(" ");
                }
                currentReason += line;
            }
        }
        flushCurrent();
        return rows;
    };

    const QStringList importProbeArgs = {
        QStringLiteral("zpool import"),
        QStringLiteral("zpool import -s"),
    };
    bool importablesFound = false;
    for (const QString& probe : importProbeArgs) {
        out.clear();
        err.clear();
        rc = -1;
        const QString cmd = withSudo(p, probe);
        if (!runSsh(p, cmd, 18000, out, err, rc)) {
            continue;
        }
        const QString merged = out + QStringLiteral("\n") + err;
        QVector<PoolImportable> parsed = parseImportableStructured(merged);
        if (!parsed.isEmpty()) {
            state.importablePools = parsed;
            importablesFound = true;
            break;
        }
        if (!err.isEmpty()) {
            appLog(QStringLiteral("INFO"), QStringLiteral("%1: %2 -> %3").arg(p.name, probe, oneLine(err)));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    const QString mountedCmd = withSudo(p, QStringLiteral("zfs mount"));
    if (runSsh(p, mountedCmd, 18000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString ln = raw.trimmed();
            if (ln.isEmpty()) {
                continue;
            }
            const int sp = ln.indexOf(' ');
            if (sp <= 0) {
                continue;
            }
            const QString ds = ln.left(sp).trimmed();
            const QString mp = ln.mid(sp + 1).trimmed();
            if (!ds.isEmpty() && !mp.isEmpty()) {
                state.mountedDatasets.push_back(qMakePair(ds, mp));
            }
        }
    } else if (!err.isEmpty()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("%1: zfs mount -> %2").arg(p.name, oneLine(err)));
    }

    appLog(QStringLiteral("NORMAL"), QStringLiteral("Fin refresh: %1 -> OK (%2)").arg(p.name, state.detail));
    return state;
}

void MainWindow::populateAllPoolsTables() {
    m_importedPoolsTable->setRowCount(0);
    m_importablePoolsTable->setRowCount(0);
    for (int i = 0; i < m_states.size(); ++i) {
        const auto& st = m_states[i];
        for (const PoolImported& pool : st.importedPools) {
            const int row = m_importedPoolsTable->rowCount();
            m_importedPoolsTable->insertRow(row);
            m_importedPoolsTable->setItem(row, 0, new QTableWidgetItem(pool.connection));
            m_importedPoolsTable->setItem(row, 1, new QTableWidgetItem(pool.pool));
            auto* act = new QTableWidgetItem(pool.action);
            act->setForeground(QBrush(QColor("#1f5f8b")));
            m_importedPoolsTable->setItem(row, 2, act);
        }
        for (const PoolImportable& pool : st.importablePools) {
            const int row = m_importablePoolsTable->rowCount();
            m_importablePoolsTable->insertRow(row);
            m_importablePoolsTable->setItem(row, 1, new QTableWidgetItem(pool.connection));
            m_importablePoolsTable->setItem(row, 2, new QTableWidgetItem(pool.pool));
            auto* state = new QTableWidgetItem(pool.state);
            const QString up = pool.state.trimmed().toUpper();
            state->setForeground(QBrush((up == QStringLiteral("ONLINE")) ? QColor("#1f7a1f") : QColor("#a12a2a")));
            m_importablePoolsTable->setItem(row, 3, state);
            m_importablePoolsTable->setItem(row, 4, new QTableWidgetItem(pool.reason));
            const QString action = (up == QStringLiteral("ONLINE")) ? pool.action : QString();
            auto* act = new QTableWidgetItem(action);
            if (!action.isEmpty()) {
                act->setForeground(QBrush(QColor("#1f5f8b")));
            }
            m_importablePoolsTable->setItem(row, 0, act);
        }
    }
    refreshSelectedPoolDetails();
    populateMountedDatasetsTables();
}

void MainWindow::populateMountedDatasetsTables() {
    auto fill = [this](QTableWidget* table) {
        if (!table) {
            return;
        }
        table->setRowCount(0);
        QMap<QString, int> mountpointCountByConn;
        struct RowData {
            QString conn;
            QString dataset;
            QString mountpoint;
        };
        QVector<RowData> allRows;
        for (int i = 0; i < m_states.size() && i < m_profiles.size(); ++i) {
            const QString connName = m_profiles[i].name;
            const auto& rows = m_states[i].mountedDatasets;
            for (const auto& pair : rows) {
                allRows.push_back({connName, pair.first, pair.second});
                mountpointCountByConn[connName + QStringLiteral("::") + pair.second] += 1;
            }
        }
        for (const RowData& row : allRows) {
            const int r = table->rowCount();
            table->insertRow(r);
            auto* dsItem = new QTableWidgetItem(QStringLiteral("%1::%2").arg(row.conn, row.dataset));
            auto* mpItem = new QTableWidgetItem(row.mountpoint);
            const bool duplicated = mountpointCountByConn.value(row.conn + QStringLiteral("::") + row.mountpoint, 0) > 1;
            if (duplicated) {
                const QColor redWarn(QStringLiteral("#b22a2a"));
                dsItem->setForeground(QBrush(redWarn));
                mpItem->setForeground(QBrush(redWarn));
            }
            table->setItem(r, 0, dsItem);
            table->setItem(r, 1, mpItem);
        }
    };
    fill(m_mountedDatasetsTableLeft);
    fill(m_mountedDatasetsTableAdv);
}

void MainWindow::refreshSelectedPoolDetails() {
    if (!m_poolPropsTable || !m_poolStatusText || !m_importedPoolsTable) {
        return;
    }
    m_poolPropsTable->setRowCount(0);
    m_poolStatusText->clear();

    const auto sel = m_importedPoolsTable->selectedItems();
    if (sel.isEmpty()) {
        return;
    }
    const int row = sel.first()->row();
    QTableWidgetItem* connItem = m_importedPoolsTable->item(row, 0);
    QTableWidgetItem* poolItem = m_importedPoolsTable->item(row, 1);
    if (!connItem || !poolItem) {
        return;
    }
    const QString connName = connItem->text().trimmed();
    const QString poolName = poolItem->text().trimmed();
    if (poolName.isEmpty() || poolName == QStringLiteral("Sin pools")) {
        return;
    }
    const int idx = findConnectionIndexByName(connName);
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];

    QString out;
    QString err;
    int rc = -1;
    const QString propsCmd = withSudo(
        p, QStringLiteral("zpool get -H -o property,value,source all %1").arg(shSingleQuote(poolName)));
    if (runSsh(p, propsCmd, 20000, out, err, rc) && rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList parts = line.split('\t');
            if (parts.size() < 3) {
                continue;
            }
            const int r = m_poolPropsTable->rowCount();
            m_poolPropsTable->insertRow(r);
            m_poolPropsTable->setItem(r, 0, new QTableWidgetItem(parts[0].trimmed()));
            m_poolPropsTable->setItem(r, 1, new QTableWidgetItem(parts[1].trimmed()));
            m_poolPropsTable->setItem(r, 2, new QTableWidgetItem(parts[2].trimmed()));
        }
    }

    out.clear();
    err.clear();
    rc = -1;
    const QString stCmd = withSudo(
        p, QStringLiteral("zpool status -v %1").arg(shSingleQuote(poolName)));
    if (runSsh(p, stCmd, 20000, out, err, rc) && rc == 0) {
        m_poolStatusText->setPlainText(out.trimmed());
    } else {
        m_poolStatusText->setPlainText(err.trimmed());
    }
}

void MainWindow::updateStatus(const QString& text) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, text]() {
            updateStatus(text);
        }, Qt::QueuedConnection);
        return;
    }
    if (m_statusText) {
        m_statusText->setPlainText(maskSecrets(text));
    }
}

bool MainWindow::actionsLocked() const {
    return m_actionsLocked;
}

QString MainWindow::buildSshPreviewCommand(const ConnectionProfile& p, const QString& remoteCmd) const {
    QStringList parts;
    parts << QStringLiteral("ssh");
    parts << QStringLiteral("-o BatchMode=yes");
    parts << QStringLiteral("-o ConnectTimeout=10");
    parts << QStringLiteral("-o LogLevel=ERROR");
    parts << QStringLiteral("-o StrictHostKeyChecking=no");
    parts << QStringLiteral("-o UserKnownHostsFile=/dev/null");
    if (p.port > 0) {
        parts << QStringLiteral("-p %1").arg(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        parts << QStringLiteral("-i %1").arg(shSingleQuote(p.keyPath));
    }
    parts << QStringLiteral("%1@%2").arg(p.username, p.host);
    parts << shSingleQuote(remoteCmd);
    return parts.join(' ');
}

bool MainWindow::confirmActionExecution(const QString& actionName, const QStringList& commands) {
    if (!m_actionConfirmEnabled) {
        return true;
    }
    if (commands.isEmpty()) {
        return true;
    }
    QDialog dlg(this);
    dlg.setModal(true);
    dlg.resize(980, 520);
    dlg.setWindowTitle(tr3(QStringLiteral("Confirmar ejecución"), QStringLiteral("Confirm execution"), QStringLiteral("确认执行")));

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    QLabel* intro = new QLabel(
        tr3(QStringLiteral("Se van a ejecutar estos comandos para la acción: %1")
                .arg(actionName),
            QStringLiteral("These commands will be executed for action: %1")
                .arg(actionName),
            QStringLiteral("将为该操作执行以下命令：%1")
                .arg(actionName)),
        &dlg);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QStringList rendered;
    rendered.reserve(commands.size());
    for (const QString& cmd : commands) {
        rendered.push_back(formatCommandPreview(cmd));
    }

    QPlainTextEdit* txt = new QPlainTextEdit(&dlg);
    txt->setReadOnly(true);
    txt->setPlainText(rendered.join(QStringLiteral("\n\n")));
    root->addWidget(txt, 1);

    QDialogButtonBox* box = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = box->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* okBtn = box->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(box);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    const bool accepted = (dlg.exec() == QDialog::Accepted);
    if (!accepted) {
        appLog(QStringLiteral("INFO"), tr3(QStringLiteral("Acción cancelada por el usuario: %1").arg(actionName),
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
    QPushButton* allBtn = new QPushButton(tr3(QStringLiteral("Seleccionar todo"), QStringLiteral("Select all"), QStringLiteral("全选")), &dlg);
    QPushButton* noneBtn = new QPushButton(tr3(QStringLiteral("Deseleccionar todo"), QStringLiteral("Clear all"), QStringLiteral("全不选")), &dlg);
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
    QPushButton* cancelBtn = box->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* okBtn = box->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
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
    dlg.setWindowTitle(tr3(QStringLiteral("Configuración"), QStringLiteral("Configuration"), QStringLiteral("配置")));
    dlg.resize(460, 190);

    QVBoxLayout* root = new QVBoxLayout(&dlg);
    QFormLayout* form = new QFormLayout();

    QComboBox* langCombo = new QComboBox(&dlg);
    langCombo->addItem(QStringLiteral("Español"), QStringLiteral("es"));
    langCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    langCombo->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    int idx = langCombo->findData(m_language);
    langCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    form->addRow(tr3(QStringLiteral("Idioma"), QStringLiteral("Language"), QStringLiteral("语言")), langCombo);

    QCheckBox* confirmChk = new QCheckBox(
        tr3(QStringLiteral("Mostrar confirmación antes de ejecutar acciones"),
            QStringLiteral("Show confirmation before executing actions"),
            QStringLiteral("执行操作前显示确认")),
        &dlg);
    confirmChk->setChecked(m_actionConfirmEnabled);
    form->addRow(QString(), confirmChk);

    root->addLayout(form);
    root->addStretch(1);

    QDialogButtonBox* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(tr3(QStringLiteral("Cancelar"), QStringLiteral("Cancel"), QStringLiteral("取消")), QDialogButtonBox::RejectRole);
    QPushButton* okBtn = buttons->addButton(tr3(QStringLiteral("Aceptar"), QStringLiteral("Accept"), QStringLiteral("确认")), QDialogButtonBox::AcceptRole);
    root->addWidget(buttons);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString newLang = langCombo->currentData().toString().trimmed().toLower();
    const bool newConfirm = confirmChk->isChecked();
    const bool langChanged = (newLang != m_language);
    m_language = newLang.isEmpty() ? QStringLiteral("es") : newLang;
    m_actionConfirmEnabled = newConfirm;
    saveUiSettings();
    appLog(QStringLiteral("INFO"),
           QStringLiteral("Configuración actualizada: idioma=%1, confirmación=%2")
               .arg(m_language, m_actionConfirmEnabled ? QStringLiteral("on") : QStringLiteral("off")));
    if (langChanged) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            tr3(QStringLiteral("Idioma guardado. Se aplicará completamente al reiniciar la aplicación."),
                QStringLiteral("Language saved. It will be fully applied after restarting the application."),
                QStringLiteral("语言已保存。重启应用后将完全生效。")));
    }
}

void MainWindow::setActionsLocked(bool locked) {
    m_actionsLocked = locked;
    if (m_btnNew) m_btnNew->setEnabled(!locked);
    if (m_btnRefreshAll) m_btnRefreshAll->setEnabled(!locked);
    if (m_btnConfig) m_btnConfig->setEnabled(!locked);
    if (m_poolStatusRefreshBtn) m_poolStatusRefreshBtn->setEnabled(!locked);
    if (m_btnApplyDatasetProps) m_btnApplyDatasetProps->setEnabled(!locked && m_btnApplyDatasetProps->isEnabled());
    if (m_btnApplyAdvancedProps) m_btnApplyAdvancedProps->setEnabled(!locked && m_btnApplyAdvancedProps->isEnabled());
    if (locked) {
        if (m_btnCopy) m_btnCopy->setEnabled(false);
        if (m_btnLevel) m_btnLevel->setEnabled(false);
        if (m_btnSync) m_btnSync->setEnabled(false);
    } else {
        updateTransferButtonsState();
        updateApplyPropsButtonState();
    }
}

QString MainWindow::maskSecrets(const QString& text) const {
    if (text.isEmpty()) {
        return text;
    }
    QString out = text;
    out.replace(
        QRegularExpression(QStringLiteral("printf\\s+'%s\\\\n'\\s+.+?\\s+\\|\\s+sudo\\s+-S\\s+-p\\s+''")),
        QStringLiteral("printf '%s\\n' [secret] | sudo -S -p ''"));
    out.replace(
        QRegularExpression(QStringLiteral("(?i)(password\\s*[:=]\\s*)\\S+")),
        QStringLiteral("\\1[secret]"));
    for (const ConnectionProfile& p : m_profiles) {
        if (!p.password.isEmpty()) {
            out.replace(p.password, QStringLiteral("[secret]"));
        }
    }
    return out;
}

void MainWindow::logUiAction(const QString& action) {
    appLog(QStringLiteral("INFO"), QStringLiteral("UI action: %1").arg(action));
}

void MainWindow::appLog(const QString& level, const QString& msg) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, level, msg]() {
            appLog(level, msg);
        }, Qt::QueuedConnection);
        return;
    }
    const QString line = QStringLiteral("[%1] [%2] %3").arg(tsNow(), level, maskSecrets(msg));
    const QString current = m_logLevelCombo ? m_logLevelCombo->currentText().toLower() : QStringLiteral("normal");
    auto rank = [](const QString& l) -> int {
        const QString x = l.toLower();
        if (x == QStringLiteral("debug")) {
            return 2;
        }
        if (x == QStringLiteral("info")) {
            return 1;
        }
        return 0;
    };
    const QString lvl = level.toLower();
    const bool always = (lvl == QStringLiteral("warn") || lvl == QStringLiteral("error"));
    if (always || rank(lvl) <= rank(current)) {
        m_logView->appendPlainText(line);
        trimLogWidget(m_logView);
    }
    if (m_lastDetailText) {
        m_lastDetailText->setPlainText(line);
    }
    appendLogToFile(line);
}

int MainWindow::maxLogLines() const {
    bool ok = false;
    const int v = m_logMaxLinesCombo ? m_logMaxLinesCombo->currentText().toInt(&ok) : 500;
    if (!ok || v <= 0) {
        return 500;
    }
    return v;
}

void MainWindow::trimLogWidget(QPlainTextEdit* widget) {
    if (!widget) {
        return;
    }
    QTextDocument* doc = widget->document();
    if (!doc) {
        return;
    }
    const int limit = maxLogLines();
    while (doc->blockCount() > limit) {
        QTextCursor c(doc);
        c.movePosition(QTextCursor::Start);
        c.select(QTextCursor::LineUnderCursor);
        c.removeSelectedText();
        c.deleteChar();
    }
}

void MainWindow::syncConnectionLogTabs() {
    if (!m_logsTabs) {
        return;
    }
    QSet<QString> wanted;
    for (const auto& p : m_profiles) {
        wanted.insert(p.id);
        if (m_connectionLogViews.contains(p.id)) {
            continue;
        }
        auto* tab = new QWidget(m_logsTabs);
        auto* lay = new QVBoxLayout(tab);
        auto* view = new QPlainTextEdit(tab);
        view->setReadOnly(true);
        QFont mono = view->font();
        mono.setFamily(QStringLiteral("Monospace"));
        mono.setPointSize(8);
        view->setFont(mono);
        lay->addWidget(view, 1);
        m_logsTabs->addTab(tab, p.name);
        m_connectionLogViews.insert(p.id, view);
    }

    for (auto it = m_connectionLogViews.begin(); it != m_connectionLogViews.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        QWidget* tab = it.value() ? it.value()->parentWidget() : nullptr;
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->removeTab(idx);
        }
        if (tab) {
            tab->deleteLater();
        }
        it = m_connectionLogViews.erase(it);
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        QWidget* tab = m_connectionLogViews.value(m_profiles[i].id)
                           ? m_connectionLogViews.value(m_profiles[i].id)->parentWidget()
                           : nullptr;
        const int idx = tab ? m_logsTabs->indexOf(tab) : -1;
        if (idx >= 0) {
            m_logsTabs->setTabText(idx, m_profiles[i].name);
        }
    }
}

void MainWindow::appendConnectionLog(const QString& connId, const QString& line) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, connId, line]() {
            appendConnectionLog(connId, line);
        }, Qt::QueuedConnection);
        return;
    }
    QPlainTextEdit* view = m_connectionLogViews.value(connId, nullptr);
    if (!view) {
        return;
    }
    view->appendPlainText(QStringLiteral("[%1] %2").arg(tsNow(), maskSecrets(line)));
    trimLogWidget(view);
}
