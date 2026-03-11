#include "mainwindow.h"

#include <algorithm>
#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QStyledItemDelegate>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QPainter>

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.9.5"
#endif

namespace {
constexpr int kIsPoolRootRole = Qt::UserRole + 12;
constexpr int kConnPropRowRole = Qt::UserRole + 13;
constexpr int kConnPropRowKindRole = Qt::UserRole + 16; // 1=name, 2=value
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr char kPoolBlockInfoKey[] = "__pool_block_info__";

class ConnContentPropBorderDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (!painter || !index.isValid() || index.column() < 4) {
            return;
        }
        if (!index.sibling(index.row(), 0).data(kConnPropRowRole).toBool()) {
            return;
        }
        const int kind = index.sibling(index.row(), 0).data(kConnPropRowKindRole).toInt();
        if (kind != 1 && kind != 2) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        QPen pen(option.palette.color(QPalette::Mid));
        pen.setWidth(1);
        painter->setPen(pen);
        const QRect r = option.rect.adjusted(0, 0, -1, -1);
        painter->drawLine(r.topLeft(), r.bottomLeft());
        painter->drawLine(r.topRight(), r.bottomRight());
        if (kind == 1) {
            painter->drawLine(r.topLeft(), r.topRight());
        } else {
            painter->drawLine(r.bottomLeft(), r.bottomRight());
        }
        painter->restore();
    }
};
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("ZFSMgr [%1]").arg(QStringLiteral(ZFSMGR_APP_VERSION)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
    resize(1200, 736);
    setMinimumSize(1120, 736);
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #f3f7fb; color: #14212b; }"
        "QTabWidget::pane { border: 1px solid #b8c7d6; border-radius: 0px; background: #f8fbff; top: -1px; }"
        "QTabWidget::tab-bar { alignment: left; }"
        "QTabBar { background: #f3f7fb; }"
        "QTabBar::scroller { background: #f3f7fb; }"
        "QTabBar QToolButton { background: #f3f7fb; border: 1px solid #b8c7d6; color: #14212b; }"
        "QTabBar::tab { padding: 4px 12px; min-height: 20px; background: #e6edf4; border: 1px solid #b8c7d6; border-bottom: 1px solid #b8c7d6; border-top-left-radius: 0px; border-top-right-radius: 0px; margin-right: 1px; }"
        "QTabBar::tab:selected { font-weight: 700; background: #f8fbff; color: #0b2f4f; border: 1px solid #6ea6dd; border-bottom-color: #f8fbff; margin-bottom: -1px; }"
        "QTabBar::tab:!selected { margin-top: 1px; background: #e6edf4; }"
        "QGroupBox { margin-top: 10px; border: 1px solid #b8c7d6; border-radius: 0px; padding-top: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 8px; padding: 0 3px 0 3px; background: #f3f7fb; color: #14212b; }"
        "QPushButton { background: #e8eff5; border: 1px solid #9db0c4; border-radius: 4px; padding: 3px 8px; }"
        "QPushButton:hover { background: #d6e6f2; }"
        "QPushButton:pressed { background: #c4d8e8; }"
        "QPushButton:disabled { background: #edf1f5; color: #8c99a6; border: 1px solid #c8d2dc; }"
        "QMenu { background: #ffffff; border: 1px solid #9db0c4; padding: 3px; }"
        "QMenu::item { padding: 4px 14px; color: #102233; }"
        "QMenu::item:selected { background: #cfe5ff; color: #0b2f4f; }"
        "QMenu::item:disabled { color: #8f9aa5; background: #f4f6f8; }"
        "QListWidget, QTableWidget, QTreeWidget { background: #ffffff; color: #102233; }"
        "QPlainTextEdit, QTextEdit, QComboBox, QLineEdit { background: #ffffff; color: #102233; }"
        "QLineEdit { border: 1px solid #9db0c4; border-radius: 3px; padding: 2px 4px; }"
        "QLineEdit:disabled { background: #edf1f5; color: #8c99a6; border: 1px solid #c8d2dc; }"
        "QComboBox QAbstractItemView { background: #ffffff; color: #102233; }"
        "QScrollBar:vertical { width: 8px; }"
        "QScrollBar:horizontal { height: 8px; }"
        "QTreeWidget::item:selected, QTableWidget::item:selected, QListWidget::item:selected {"
        "  background: #dcecff; color: #0d2438; font-weight: 600; }"
        "QHeaderView::section { background: #eaf1f7; border: 1px solid #c5d3e0; padding: 1px 3px; font-size: 85%; }"));
    setStyleSheet(styleSheet() + QStringLiteral(
        "#zfsmgrEntityFrame { border: 0px; background: transparent; }"
        "#zfsmgrEntityFrame > QWidget { border: 0px; background: transparent; }"
        "#zfsmgrEntityTabs::tab, #zfsmgrPoolViewTabs::tab { border-bottom: 1px solid #b8c7d6; }"
        "#zfsmgrEntityTabs::tab:selected, #zfsmgrPoolViewTabs::tab:selected { border-bottom: 0px; margin-bottom: -1px; padding-bottom: 1px; }"
        "#zfsmgrDetailContainer { border: 0px; background: transparent; margin-top: 0px; }"
        "#zfsmgrDetailContainer > QWidget { border: 0px; background: transparent; }"
        "#zfsmgrDetailContainer QTabBar { background: transparent; }"
        "#zfsmgrSubtabContentFrame { border: 0px; background: transparent; margin-top: 0px; }"));
#ifdef Q_OS_MAC
    setStyleSheet(styleSheet() + QStringLiteral(
        "QTreeView::indicator:unchecked, QTableView::indicator:unchecked, QCheckBox::indicator:unchecked {"
        " border: 2px solid #5b7289; background: #ffffff; }"
        "QTreeView::indicator:checked, QTableView::indicator:checked, QCheckBox::indicator:checked {"
        " border: 2px solid #2f5f8c; background: #e7f1fb; }"));
#endif

    QMenu* appMenu = menuBar()->addMenu(
        trk(QStringLiteral("t_menu_main_001"),
            QStringLiteral("Menú"),
            QStringLiteral("Menu"),
            QStringLiteral("菜单")));
    QMenu* languageMenu = appMenu->addMenu(
        trk(QStringLiteral("t_lang_menu_001"),
            QStringLiteral("Idioma"),
            QStringLiteral("Language"),
            QStringLiteral("语言")));
    auto* langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);
    QAction* langEs = languageMenu->addAction(QStringLiteral("Español"));
    QAction* langEn = languageMenu->addAction(QStringLiteral("English"));
    QAction* langZh = languageMenu->addAction(QStringLiteral("中文"));
    langEs->setCheckable(true);
    langEn->setCheckable(true);
    langZh->setCheckable(true);
    langEs->setData(QStringLiteral("es"));
    langEn->setData(QStringLiteral("en"));
    langZh->setData(QStringLiteral("zh"));
    langGroup->addAction(langEs);
    langGroup->addAction(langEn);
    langGroup->addAction(langZh);
    const QString langNorm = m_language.trimmed().toLower();
    if (langNorm == QStringLiteral("en")) {
        langEn->setChecked(true);
    } else if (langNorm == QStringLiteral("zh")) {
        langZh->setChecked(true);
    } else {
        langEs->setChecked(true);
    }
    connect(langGroup, &QActionGroup::triggered, this, [this](QAction* act) {
        if (!act) {
            return;
        }
        const QString newLang = act->data().toString().trimmed().toLower();
        if (newLang.isEmpty() || newLang == m_language) {
            return;
        }
        m_language = newLang;
        m_store.setLanguage(m_language);
        saveUiSettings();
        appLog(QStringLiteral("INFO"), QStringLiteral("Idioma cambiado a %1").arg(m_language));
        applyLanguageLive();
    });

    QAction* confirmAct = appMenu->addAction(
        trk(QStringLiteral("t_show_confirm_001"),
            QStringLiteral("Mostrar confirmación antes de ejecutar acciones"),
            QStringLiteral("Show confirmation before executing actions"),
            QStringLiteral("执行操作前显示确认")));
    confirmAct->setCheckable(true);
    confirmAct->setChecked(m_actionConfirmEnabled);
    connect(confirmAct, &QAction::toggled, this, [this](bool checked) {
        m_actionConfirmEnabled = checked;
        saveUiSettings();
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Confirmación de acciones: %1").arg(checked ? QStringLiteral("on")
                                                                          : QStringLiteral("off")));
    });

    QMenu* logsMenu = appMenu->addMenu(
        trk(QStringLiteral("t_logs_menu_001"),
            QStringLiteral("Logs"),
            QStringLiteral("Logs"),
            QStringLiteral("日志")));
    QMenu* logLevelMenu = logsMenu->addMenu(
        trk(QStringLiteral("t_log_level_001"),
            QStringLiteral("Nivel de log"),
            QStringLiteral("Log level"),
            QStringLiteral("日志级别")));
    auto* logLevelGroup = new QActionGroup(this);
    logLevelGroup->setExclusive(true);
    for (const QString& lv : {QStringLiteral("normal"), QStringLiteral("info"), QStringLiteral("debug")}) {
        QAction* act = logLevelMenu->addAction(lv);
        act->setCheckable(true);
        act->setData(lv);
        if (lv == m_logLevelSetting) {
            act->setChecked(true);
        }
        logLevelGroup->addAction(act);
    }
    connect(logLevelGroup, &QActionGroup::triggered, this, [this](QAction* act) {
        if (!act) {
            return;
        }
        m_logLevelSetting = act->data().toString().trimmed().toLower();
        if (m_logLevelSetting != QStringLiteral("normal")
            && m_logLevelSetting != QStringLiteral("info")
            && m_logLevelSetting != QStringLiteral("debug")) {
            m_logLevelSetting = QStringLiteral("normal");
        }
        saveUiSettings();
    });

    QMenu* logLinesMenu = logsMenu->addMenu(
        trk(QStringLiteral("t_log_lines_001"),
            QStringLiteral("Número de líneas"),
            QStringLiteral("Number of lines"),
            QStringLiteral("行数")));
    auto* logLinesGroup = new QActionGroup(this);
    logLinesGroup->setExclusive(true);
    for (int lines : {100, 200, 500, 1000}) {
        QAction* act = logLinesMenu->addAction(QString::number(lines));
        act->setCheckable(true);
        act->setData(lines);
        if (lines == m_logMaxLinesSetting) {
            act->setChecked(true);
        }
        logLinesGroup->addAction(act);
    }
    connect(logLinesGroup, &QActionGroup::triggered, this, [this](QAction* act) {
        if (!act) {
            return;
        }
        const int lines = act->data().toInt();
        if (lines == 100 || lines == 200 || lines == 500 || lines == 1000) {
            m_logMaxLinesSetting = lines;
        }
        trimLogWidget(m_logView);
        saveUiSettings();
    });

    QAction* clearLogsAct = logsMenu->addAction(
        trk(QStringLiteral("t_clear_001"),
            QStringLiteral("Limpiar"),
            QStringLiteral("Clear"),
            QStringLiteral("清空")));
    connect(clearLogsAct, &QAction::triggered, this, [this]() {
        logUiAction(QStringLiteral("Limpiar log (menú)"));
        clearAppLog();
    });
    QAction* copyLogsAct = logsMenu->addAction(
        trk(QStringLiteral("t_copy_001"),
            QStringLiteral("Copiar"),
            QStringLiteral("Copy"),
            QStringLiteral("复制")));
    connect(copyLogsAct, &QAction::triggered, this, [this]() {
        logUiAction(QStringLiteral("Copiar log (menú)"));
        copyAppLogToClipboard();
    });
    logsMenu->addSeparator();

    QMenu* logSizeMenu = logsMenu->addMenu(
        trk(QStringLiteral("t_log_max_rot_001"),
            QStringLiteral("Tamaño máximo log rotativo"),
            QStringLiteral("Max rotating log size"),
            QStringLiteral("滚动日志最大大小")));
    auto* logSizeGroup = new QActionGroup(this);
    logSizeGroup->setExclusive(true);
    QList<int> sizesMb = {5, 10, 20, 50, 100, 200, 500, 1024};
    if (!sizesMb.contains(m_logMaxSizeMb)) {
        sizesMb.push_back(qBound(1, m_logMaxSizeMb, 1024));
        std::sort(sizesMb.begin(), sizesMb.end());
        sizesMb.erase(std::unique(sizesMb.begin(), sizesMb.end()), sizesMb.end());
    }
    for (int mb : sizesMb) {
        QAction* act = logSizeMenu->addAction(QStringLiteral("%1 MB").arg(mb));
        act->setCheckable(true);
        act->setData(mb);
        if (mb == m_logMaxSizeMb) {
            act->setChecked(true);
        }
        logSizeGroup->addAction(act);
    }
    connect(logSizeGroup, &QActionGroup::triggered, this, [this](QAction* act) {
        if (!act) {
            return;
        }
        bool ok = false;
        const int mb = act->data().toInt(&ok);
        if (!ok) {
            return;
        }
        const int bounded = qBound(1, mb, 1024);
        if (bounded == m_logMaxSizeMb) {
            return;
        }
        m_logMaxSizeMb = bounded;
        saveUiSettings();
        rotateLogIfNeeded();
        appLog(QStringLiteral("INFO"), QStringLiteral("Tamaño máximo de log rotativo: %1 MB").arg(m_logMaxSizeMb));
    });

    appMenu->addSeparator();
    m_menuExitAction = appMenu->addAction(
        trk(QStringLiteral("t_menu_exit_001"),
            QStringLiteral("Salir"),
            QStringLiteral("Exit"),
            QStringLiteral("退出")));
    m_menuExitAction->setEnabled(!actionsLocked());
    connect(m_menuExitAction, &QAction::triggered, this, [this]() {
        if (actionsLocked()) {
            return;
        }
        close();
    });

    QMenu* helpMenu = menuBar()->addMenu(
        trk(QStringLiteral("t_help_menu_001"),
            QStringLiteral("Ayuda"),
            QStringLiteral("Help"),
            QStringLiteral("帮助")));
    QAction* quickManualAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_quick_001"),
            QStringLiteral("Manual rápido"),
            QStringLiteral("Quick manual"),
            QStringLiteral("快速手册")));
    connect(quickManualAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("manual_rapido"),
                      trk(QStringLiteral("t_help_quick_001"),
                          QStringLiteral("Manual rápido"),
                          QStringLiteral("Quick manual"),
                          QStringLiteral("快速手册")));
    });

    QMenu* actionsHelpMenu = helpMenu->addMenu(
        trk(QStringLiteral("t_help_actions_001"),
            QStringLiteral("Acciones"),
            QStringLiteral("Actions"),
            QStringLiteral("操作")));
    struct HelpTopicItem {
        QString id;
        QString key;
        QString es;
        QString en;
        QString zh;
    };
    const QVector<HelpTopicItem> helpActions = {
        {QStringLiteral("accion_copiar"), QStringLiteral("t_copy_001"), QStringLiteral("Copiar"), QStringLiteral("Copy"), QStringLiteral("复制")},
        {QStringLiteral("accion_sincronizar"), QStringLiteral("t_sync_btn_001"), QStringLiteral("Sincronizar"), QStringLiteral("Sync"), QStringLiteral("同步")},
        {QStringLiteral("accion_nivelar"), QStringLiteral("t_level_btn_001"), QStringLiteral("Nivelar"), QStringLiteral("Level"), QStringLiteral("对齐")},
        {QStringLiteral("accion_desglosar"), QStringLiteral("t_breakdown_btn1"), QStringLiteral("Desglosar"), QStringLiteral("Breakdown"), QStringLiteral("拆分")},
        {QStringLiteral("accion_ensamblar"), QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("合并")},
        {QStringLiteral("accion_desde_dir"), QStringLiteral("t_from_dir_btn1"), QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")},
        {QStringLiteral("accion_hacia_dir"), QStringLiteral("t_to_dir_btn_001"), QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")}
    };
    for (const HelpTopicItem& item : helpActions) {
        QAction* act = actionsHelpMenu->addAction(trk(item.key, item.es, item.en, item.zh));
        connect(act, &QAction::triggered, this, [this, item]() {
            openHelpTopic(item.id, trk(item.key, item.es, item.en, item.zh));
        });
    }

    QAction* ctxMenusAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_ctx_001"),
            QStringLiteral("Menús contextuales"),
            QStringLiteral("Context menus"),
            QStringLiteral("上下文菜单")));
    connect(ctxMenusAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("menus_contextuales"),
                      trk(QStringLiteral("t_help_ctx_001"),
                          QStringLiteral("Menús contextuales"),
                          QStringLiteral("Context menus"),
                          QStringLiteral("上下文菜单")));
    });

    QAction* shortcutsAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_short_001"),
            QStringLiteral("Atajos y estados"),
            QStringLiteral("Shortcuts and states"),
            QStringLiteral("快捷键与状态")));
    connect(shortcutsAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("atajos_estados"),
                      trk(QStringLiteral("t_help_short_001"),
                          QStringLiteral("Atajos y estados"),
                          QStringLiteral("Shortcuts and states"),
                          QStringLiteral("快捷键与状态")));
    });

    QAction* appLogHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_applog_001"),
            QStringLiteral("Logs de aplicación"),
            QStringLiteral("Application logs"),
            QStringLiteral("应用日志")));
    connect(appLogHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("logs_aplicacion"),
                      trk(QStringLiteral("t_help_applog_001"),
                          QStringLiteral("Logs de aplicación"),
                          QStringLiteral("Application logs"),
                          QStringLiteral("应用日志")));
    });

    QAction* cfgFilesHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_cfg_001"),
            QStringLiteral("Configuración y archivos INI"),
            QStringLiteral("Configuration and INI files"),
            QStringLiteral("配置与 INI 文件")));
    connect(cfgFilesHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("configuracion_archivos"),
                      trk(QStringLiteral("t_help_cfg_001"),
                          QStringLiteral("Configuración y archivos INI"),
                          QStringLiteral("Configuration and INI files"),
                          QStringLiteral("配置与 INI 文件")));
    });

    QAction* aboutAct = helpMenu->addAction(
        trk(QStringLiteral("t_about_001"),
            QStringLiteral("Acerca de"),
            QStringLiteral("About"),
            QStringLiteral("关于")));
    connect(aboutAct, &QAction::triggered, this, [this]() {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_about_msg_001"),
                QStringLiteral("ZFSMgr\nGestor ZFS multiplataforma.\nAutor: Eladio Linares\nLicencia: GNU"),
                QStringLiteral("ZFSMgr\nCross-platform ZFS manager.\nAuthor: Eladio Linares\nLicense: GNU"),
                QStringLiteral("ZFSMgr\n跨平台 ZFS 管理器。\n作者：Eladio Linares\n许可：GNU")));
    });

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 2, 8, 8);
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
        fm.horizontalAdvance(trk(QStringLiteral("t_refrescar__7f8af2"),
                                 QStringLiteral("Refrescar todo"),
                                 QStringLiteral("Refresh all"),
                                 QStringLiteral("全部刷新"))),
        fm.horizontalAdvance(trk(QStringLiteral("t_new_btn_001"),
                                 QStringLiteral("Nueva"),
                                 QStringLiteral("New"),
                                 QStringLiteral("新建"))));
    const int leftBaseWidth = qMax(340, btnTextWidth + 190);
    const int leftFixedWidth = qMax(220, static_cast<int>(leftBaseWidth * 0.69 * 1.20));
    leftPane->setMinimumWidth(leftFixedWidth);
    leftPane->setMaximumWidth(leftFixedWidth);

    auto* connectionsTab = new QWidget(leftPane);
    auto* connLayout = new QVBoxLayout(connectionsTab);
    connLayout->setContentsMargins(2, 2, 2, 2);
    connLayout->setSpacing(2);
    const int stdLeftBtnH = 34;
    m_poolMgmtBox = nullptr;

    auto* connListBox = new QGroupBox(
        trk(QStringLiteral("t_list_001"),
            QStringLiteral("Conexiones"),
            QStringLiteral("Connections"),
            QStringLiteral("连接")),
        connectionsTab);
    auto* connListBoxLayout = new QVBoxLayout(connListBox);
    connListBoxLayout->setContentsMargins(6, 8, 6, 6);
    m_connectionsTable = new QTableWidget(connListBox);
    m_connectionsTable->setColumnCount(3);
    m_connectionsTable->setHorizontalHeaderLabels({
        QStringLiteral("Origen"),
        QStringLiteral("Destino"),
        trk(QStringLiteral("t_connections_001"),
            QStringLiteral("Conexión"),
            QStringLiteral("Connection"),
            QStringLiteral("连接"))
    });
    m_connectionsTable->horizontalHeader()->setVisible(true);
    m_connectionsTable->verticalHeader()->setVisible(false);
    m_connectionsTable->setAlternatingRowColors(true);
    m_connectionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_connectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_connectionsTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_connectionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_connectionsTable->setStyle(fusion);
    }
#endif
    connListBoxLayout->addWidget(m_connectionsTable, 1);
    connLayout->addWidget(connListBox, 1);

    m_connActionsBox = new QGroupBox(
        trk(QStringLiteral("t_actions_box_001"),
            QStringLiteral("Acciones"),
            QStringLiteral("Actions"),
            QStringLiteral("操作")),
        connectionsTab);
    auto* connActionsLayout = new QHBoxLayout(m_connActionsBox);
    connActionsLayout->setContentsMargins(6, 8, 6, 6);
    connActionsLayout->setSpacing(8);

    m_btnConnBreakdown = new QPushButton(
        trk(QStringLiteral("t_breakdown_btn1"),
            QStringLiteral("Desglosar"),
            QStringLiteral("Break down"),
            QStringLiteral("拆分")),
        m_connActionsBox);
    m_btnConnAssemble = new QPushButton(
        trk(QStringLiteral("t_assemble_btn1"),
            QStringLiteral("Ensamblar"),
            QStringLiteral("Assemble"),
            QStringLiteral("组装")),
        m_connActionsBox);
    m_btnConnFromDir = new QPushButton(
        trk(QStringLiteral("t_from_dir_btn1"),
            QStringLiteral("Desde Dir"),
            QStringLiteral("From Dir"),
            QStringLiteral("来自目录")),
        m_connActionsBox);
    m_btnConnToDir = new QPushButton(
        trk(QStringLiteral("t_to_dir_btn_001"),
            QStringLiteral("Hacia Dir"),
            QStringLiteral("To Dir"),
            QStringLiteral("到目录")),
        m_connActionsBox);
    m_btnConnBreakdown->setToolTip(
        trk(QStringLiteral("t_tt_breakdown001"), QStringLiteral("Construye datasets a partir de directorios. "
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
    m_btnConnAssemble->setToolTip(
        trk(QStringLiteral("t_tt_assemble001"), QStringLiteral("Convierte datasets en directorios. "
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
    m_btnConnFromDir->setToolTip(
        trk(QStringLiteral("t_tt_fromdir001"), QStringLiteral("Crea un dataset hijo usando un directorio local como mountpoint.\n"
                           "Requiere dataset seleccionado en Avanzado."),
            QStringLiteral("Create a child dataset using a local directory as mountpoint.\n"
                           "Requires a dataset selected in Advanced."),
            QStringLiteral("使用本地目录作为挂载点创建子数据集。\n"
                           "需要在高级页选择一个数据集。")));
    m_btnConnToDir->setToolTip(
        trk(QStringLiteral("t_tt_todir_001"), QStringLiteral("Hace lo contrario de Desde Dir: copia el contenido del dataset a un directorio local\n"
                           "y elimina el dataset al finalizar correctamente."),
            QStringLiteral("Inverse of From Dir: copy dataset content to a local directory\n"
                           "and remove dataset when finished successfully."),
            QStringLiteral("与“来自目录”相反：将数据集内容复制到本地目录，\n"
                           "成功后删除该数据集。")));
    m_btnConnBreakdown->setMinimumHeight(stdLeftBtnH);
    m_btnConnAssemble->setMinimumHeight(stdLeftBtnH);
    m_btnConnFromDir->setMinimumHeight(stdLeftBtnH);
    m_btnConnToDir->setMinimumHeight(stdLeftBtnH);
    m_btnConnBreakdown->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnAssemble->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnFromDir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnToDir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* connActionRightBox = new QWidget(m_connActionsBox);
    auto* connActionRightLayout = new QVBoxLayout(connActionRightBox);
    connActionRightLayout->setContentsMargins(6, 2, 6, 4);
    connActionRightLayout->setSpacing(4);
    m_connOriginSelectionLabel = new QLabel(
        trk(QStringLiteral("t_conn_origin_sel1"),
            QStringLiteral("Origen:(vacío)"),
            QStringLiteral("Source:(empty)"),
            QStringLiteral("源：（空）")),
        connActionRightBox);
    m_connOriginSelectionLabel->setWordWrap(true);
    m_connOriginSelectionLabel->setMinimumHeight(20);
    connActionRightLayout->addWidget(m_connOriginSelectionLabel);
    m_connDestSelectionLabel = new QLabel(
        trk(QStringLiteral("t_conn_dest_sel01"),
            QStringLiteral("Destino:(vacío)"),
            QStringLiteral("Target:(empty)"),
            QStringLiteral("目标：（空）")),
        connActionRightBox);
    m_connDestSelectionLabel->setWordWrap(true);
    m_connDestSelectionLabel->setMinimumHeight(20);
    connActionRightLayout->addWidget(m_connDestSelectionLabel);
    m_btnApplyConnContentProps = new QPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        connActionRightBox);
    m_btnConnCopy = new QPushButton(
        trk(QStringLiteral("t_copy_001"),
            QStringLiteral("Copiar"),
            QStringLiteral("Copy"),
            QStringLiteral("复制")),
        connActionRightBox);
    m_btnConnLevel = new QPushButton(
        trk(QStringLiteral("t_level_btn_001"),
            QStringLiteral("Nivelar"),
            QStringLiteral("Level"),
            QStringLiteral("同步快照")),
        connActionRightBox);
    m_btnConnSync = new QPushButton(
        trk(QStringLiteral("t_sync_btn_001"),
            QStringLiteral("Sincronizar"),
            QStringLiteral("Sync"),
            QStringLiteral("同步文件")),
        connActionRightBox);
    m_btnConnCopy->setToolTip(
        trk(QStringLiteral("t_tt_copy_001"),
            QStringLiteral("Envía un snapshot desde Origen a Destino mediante send/recv.\n"
                           "Requiere: snapshot seleccionado en Origen y dataset seleccionado en Destino."),
            QStringLiteral("Send one snapshot from Source to Target using send/recv.\n"
                           "Requires: snapshot selected in Source and dataset selected in Target."),
            QStringLiteral("通过 send/recv 将源端快照发送到目标端。\n"
                           "条件：源端选择快照，目标端选择数据集。")));
    m_btnConnLevel->setToolTip(
        trk(QStringLiteral("t_tt_level_001"),
            QStringLiteral("Genera/aplica envío diferencial para igualar Origen->Destino.\n"
                           "Requiere: dataset o snapshot seleccionado en Origen y dataset en Destino."),
            QStringLiteral("Build/apply differential transfer to level Source->Target.\n"
                           "Requires: dataset or snapshot selected in Source and dataset in Target."),
            QStringLiteral("生成/应用差异传输以对齐源端到目标端。\n"
                           "条件：源端选择数据集或快照，目标端选择数据集。")));
    m_btnConnSync->setToolTip(
        trk(QStringLiteral("t_tt_sync_001"),
            QStringLiteral("Sincroniza contenido de dataset Origen a Destino con rsync.\n"
                           "Requiere: dataset seleccionado (no snapshot) en Origen y Destino."),
            QStringLiteral("Sync dataset contents from Source to Target with rsync.\n"
                           "Requires: dataset selected (not snapshot) in Source and Target."),
            QStringLiteral("使用 rsync 同步源端到目标端的数据集内容。\n"
                           "条件：源端和目标端都选择数据集（非快照）。")));
    m_btnApplyConnContentProps->setMinimumHeight(stdLeftBtnH);
    m_btnConnCopy->setMinimumHeight(stdLeftBtnH);
    m_btnConnLevel->setMinimumHeight(stdLeftBtnH);
    m_btnConnSync->setMinimumHeight(stdLeftBtnH);
    m_btnApplyConnContentProps->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnCopy->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnLevel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnSync->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* connRightBtns = new QGridLayout();
    connRightBtns->setContentsMargins(0, 0, 0, 0);
    connRightBtns->setHorizontalSpacing(6);
    connRightBtns->setVerticalSpacing(6);
    connRightBtns->setColumnStretch(0, 1);
    connRightBtns->setColumnStretch(1, 1);
    connRightBtns->addWidget(m_btnApplyConnContentProps, 0, 0);
    connRightBtns->addWidget(m_btnConnSync, 0, 1);
    connRightBtns->addWidget(m_btnConnCopy, 1, 0);
    connRightBtns->addWidget(m_btnConnLevel, 1, 1);
    connActionRightLayout->addLayout(connRightBtns);
    connActionsLayout->addWidget(connActionRightBox, 1);
    connLayout->addWidget(m_connActionsBox, 0);
    connectionsTab->setLayout(connLayout);

    auto* datasetsTab = new QWidget(m_leftTabs);
    auto* dsLeftTabLayout = new QVBoxLayout(datasetsTab);
    dsLeftTabLayout->setContentsMargins(4, 4, 4, 4);
    dsLeftTabLayout->setSpacing(4);
    m_transferBox = new QGroupBox(trk(QStringLiteral("t_action_from_to1"),
                                      QStringLiteral("Acción desde [vacío] hacia [vacío]"),
                                      QStringLiteral("Action from [empty] to [empty]"),
                                      QStringLiteral("从 [空] 到 [空] 的操作")),
                                  datasetsTab);
    auto* transferLayout = new QVBoxLayout(m_transferBox);
    m_transferOriginLabel = new QLabel(trk(QStringLiteral("t_origin_sel_001"),
                                           QStringLiteral("Origen: Dataset (seleccione)"),
                                           QStringLiteral("Source: Dataset (select)"),
                                           QStringLiteral("源：数据集（请选择）")),
                                       m_transferBox);
    m_transferDestLabel = new QLabel(trk(QStringLiteral("t_target_sel_001"),
                                         QStringLiteral("Destino: Dataset (seleccione)"),
                                         QStringLiteral("Target: Dataset (select)"),
                                         QStringLiteral("目标：数据集（请选择）")),
                                     m_transferBox);
    m_transferOriginLabel->setWordWrap(true);
    m_transferDestLabel->setWordWrap(true);
    m_transferOriginLabel->setMinimumHeight(34);
    m_transferDestLabel->setMinimumHeight(34);
    m_transferOriginLabel->hide();
    m_transferDestLabel->hide();
    m_btnCopy = new QPushButton(trk(QStringLiteral("t_copy_001"),
                                    QStringLiteral("Copiar"),
                                    QStringLiteral("Copy"),
                                    QStringLiteral("复制")),
                                m_transferBox);
    m_btnLevel = new QPushButton(trk(QStringLiteral("t_level_btn_001"),
                                     QStringLiteral("Nivelar"),
                                     QStringLiteral("Level"),
                                     QStringLiteral("同步快照")),
                                 m_transferBox);
    m_btnSync = new QPushButton(trk(QStringLiteral("t_sync_btn_001"),
                                    QStringLiteral("Sincronizar"),
                                    QStringLiteral("Sync"),
                                    QStringLiteral("同步文件")),
                                m_transferBox);
    m_btnCopy->setMinimumHeight(stdLeftBtnH);
    m_btnLevel->setMinimumHeight(stdLeftBtnH);
    m_btnSync->setMinimumHeight(stdLeftBtnH);
    m_btnCopy->setMinimumWidth(0);
    m_btnLevel->setMinimumWidth(0);
    m_btnSync->setMinimumWidth(0);
    m_btnCopy->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnLevel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnSync->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnCopy->setToolTip(
        trk(QStringLiteral("t_tt_copy_001"),
            QStringLiteral("Envía un snapshot desde Origen a Destino mediante send/recv.\n"
                           "Requiere: snapshot seleccionado en Origen y dataset seleccionado en Destino."),
            QStringLiteral("Send one snapshot from Source to Target using send/recv.\n"
                           "Requires: snapshot selected in Source and dataset selected in Target."),
            QStringLiteral("通过 send/recv 将源端快照发送到目标端。\n"
                           "条件：源端选择快照，目标端选择数据集。")));
    m_btnLevel->setToolTip(
        trk(QStringLiteral("t_tt_level_001"),
            QStringLiteral("Genera/aplica envío diferencial para igualar Origen->Destino.\n"
                           "Requiere: dataset o snapshot seleccionado en Origen y dataset en Destino."),
            QStringLiteral("Build/apply differential transfer to level Source->Target.\n"
                           "Requires: dataset or snapshot selected in Source and dataset in Target."),
            QStringLiteral("生成/应用差异传输以对齐源端到目标端。\n"
                           "条件：源端选择数据集或快照，目标端选择数据集。")));
    m_btnSync->setToolTip(
        trk(QStringLiteral("t_tt_sync_001"),
            QStringLiteral("Sincroniza contenido de dataset Origen a Destino con rsync.\n"
                           "Requiere: dataset seleccionado (no snapshot) en Origen y Destino."),
            QStringLiteral("Sync dataset contents from Source to Target with rsync.\n"
                           "Requires: dataset selected (not snapshot) in Source and Target."),
            QStringLiteral("使用 rsync 同步源端到目标端的数据集内容。\n"
                           "条件：源端和目标端都选择数据集（非快照）。")));
    m_btnCopy->setEnabled(false);
    m_btnLevel->setEnabled(false);
    m_btnSync->setEnabled(false);
    auto* transferButtonsGrid = new QGridLayout();
    transferButtonsGrid->setContentsMargins(0, 0, 0, 0);
    transferButtonsGrid->setHorizontalSpacing(6);
    transferButtonsGrid->setVerticalSpacing(6);
    transferButtonsGrid->setColumnStretch(0, 1);
    transferButtonsGrid->setColumnStretch(1, 1);
    transferButtonsGrid->addWidget(m_btnCopy, 0, 0);
    transferButtonsGrid->addWidget(m_btnLevel, 0, 1);
    transferButtonsGrid->addWidget(m_btnSync, 1, 0, 1, 2);
    transferLayout->addLayout(transferButtonsGrid);
    dsLeftTabLayout->addWidget(m_transferBox);
    auto* datasetsInfoTabs = new QTabWidget(datasetsTab);
    datasetsInfoTabs->setDocumentMode(false);
    auto* mountedLeftTab = new QWidget(datasetsInfoTabs);
    auto* mountedLeftLayout = new QVBoxLayout(mountedLeftTab);
    m_mountedDatasetsTableLeft = new QTableWidget(mountedLeftTab);
    m_mountedDatasetsTableLeft->setColumnCount(2);
    m_mountedDatasetsTableLeft->setHorizontalHeaderLabels(
        {trk(QStringLiteral("t_dataset_001"),
             QStringLiteral("Dataset"),
             QStringLiteral("Dataset"),
             QStringLiteral("数据集")),
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
    enableSortableHeader(m_mountedDatasetsTableLeft);
    mountedLeftLayout->addWidget(m_mountedDatasetsTableLeft, 1);
    auto* propsLeftTab = new QWidget(datasetsInfoTabs);
    auto* propsLeftLayout = new QVBoxLayout(propsLeftTab);
    propsLeftLayout->setContentsMargins(0, 0, 0, 0);
    propsLeftLayout->setSpacing(4);
    m_datasetPropsTable = new QTableWidget(propsLeftTab);
    m_datasetPropsTable->setColumnCount(3);
    m_datasetPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Inherit")});
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_datasetPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_datasetPropsTable->horizontalHeader()->setStretchLastSection(false);
    m_datasetPropsTable->setColumnWidth(0, 200);
    m_datasetPropsTable->setColumnWidth(1, 210);
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_datasetPropsTable->setStyle(fusion);
    }
    m_datasetPropsTable->setStyleSheet(QStringLiteral(
        "QTableWidget::indicator:unchecked { border: 2px solid #1f4f76; background: #ffffff; border-radius: 3px; }"
        "QTableWidget::indicator:checked { border: 2px solid #1f4f76; background: #d9ecff; border-radius: 3px; }"));
    const int inheritColWidth = qMax(36, static_cast<int>((m_datasetPropsTable->fontMetrics().horizontalAdvance(QStringLiteral("Inherit")) / 2 + 12) * 1.10));
    m_datasetPropsTable->setColumnWidth(2, inheritColWidth);
#else
    m_datasetPropsTable->setColumnWidth(2, 90);
#endif
    m_datasetPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_datasetPropsTable->verticalHeader()->setVisible(false);
    m_datasetPropsTable->verticalHeader()->setDefaultSectionSize(22);
    m_datasetPropsTable->setProperty("pinned_rows", 5);
    m_datasetPropsTable->setFont(m_mountedDatasetsTableLeft->font());
    {
        QFont hf = m_datasetPropsTable->font();
        hf.setBold(false);
        m_datasetPropsTable->horizontalHeader()->setFont(hf);
    }
    m_btnApplyDatasetProps = new QPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        propsLeftTab);
    m_btnApplyDatasetProps->setEnabled(false);
    enableSortableHeader(m_datasetPropsTable);
    propsLeftLayout->addWidget(m_datasetPropsTable, 1);
    propsLeftLayout->addWidget(m_btnApplyDatasetProps, 0, Qt::AlignRight);
    datasetsInfoTabs->addTab(
        propsLeftTab,
        trk(QStringLiteral("t_props_tab_001"),
            QStringLiteral("Propiedades"),
            QStringLiteral("Properties"),
            QStringLiteral("属性")));
    datasetsInfoTabs->addTab(
        mountedLeftTab,
        trk(QStringLiteral("t_mounted_tab_001"),
            QStringLiteral("Montados"),
            QStringLiteral("Mounted"),
            QStringLiteral("已挂载")));
    dsLeftTabLayout->addWidget(datasetsInfoTabs, 1);
    datasetsTab->setLayout(dsLeftTabLayout);

    // Legacy "advanced" layer removed from visible UI.
    const int actionsBoxHeight = qMax(130, m_connActionsBox ? m_connActionsBox->sizeHint().height() : 130);
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setFixedHeight(actionsBoxHeight);
    }
    if (m_connActionsBox) {
        m_connActionsBox->setMinimumHeight(actionsBoxHeight);
    }
    m_transferBox->setFixedHeight(actionsBoxHeight);
    m_btnAdvancedBreakdown = nullptr;
    m_btnAdvancedAssemble = nullptr;
    m_btnAdvancedFromDir = nullptr;
    m_btnAdvancedToDir = nullptr;
    m_mountedDatasetsTableAdv = nullptr;

    m_leftTabs->addTab(datasetsTab, trk(QStringLiteral("t_datasets_tab_001"),
                                        QStringLiteral("Datasets"),
                                        QStringLiteral("Datasets"),
                                        QStringLiteral("数据集")));
    // Legacy left tabs are hidden in current UX.
    m_leftTabs->hide();
    leftLayout->addWidget(connectionsTab, 1);

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

    auto* entityFrame = new QFrame(rightConnectionsPage);
    entityFrame->setObjectName(QStringLiteral("zfsmgrEntityFrame"));
    entityFrame->setFrameShape(QFrame::NoFrame);
    auto* entityFrameLayout = new QVBoxLayout(entityFrame);
    entityFrameLayout->setContentsMargins(0, 0, 0, 0);
    entityFrameLayout->setSpacing(0);
    m_poolDetailTabs = new QWidget(entityFrame);
    m_poolDetailTabs->setObjectName(QStringLiteral("zfsmgrPoolDetailTabs"));
    auto* poolDetailLayout = new QVBoxLayout(m_poolDetailTabs);
    poolDetailLayout->setContentsMargins(3, 3, 3, 3);
    poolDetailLayout->setSpacing(0);
    m_connectionEntityTabs = new QTabBar(m_poolDetailTabs);
    m_connectionEntityTabs->setObjectName(QStringLiteral("zfsmgrEntityTabs"));
    m_connectionEntityTabs->setExpanding(false);
    m_connectionEntityTabs->setDrawBase(false);
    m_connectionEntityTabs->setUsesScrollButtons(true);
    m_connectionEntityTabs->setVisible(false);
    poolDetailLayout->addWidget(m_connectionEntityTabs, 0);
    auto* detailContainer = new QFrame(m_poolDetailTabs);
    detailContainer->setObjectName(QStringLiteral("zfsmgrDetailContainer"));
    detailContainer->setFrameShape(QFrame::NoFrame);
    detailContainer->setFrameShadow(QFrame::Plain);
    detailContainer->setLineWidth(0);
    auto* detailContainerLayout = new QVBoxLayout(detailContainer);
    detailContainerLayout->setContentsMargins(0, 0, 0, 0);
    detailContainerLayout->setSpacing(0);
    m_poolViewTabBar = nullptr;
    m_connPropsGroup = new QWidget(m_poolDetailTabs);
    auto* propsPoolLayout = new QVBoxLayout(m_connPropsGroup);
    propsPoolLayout->setContentsMargins(0, 0, 0, 0);
    propsPoolLayout->setSpacing(4);
    m_connPropsStack = new QStackedWidget(m_connPropsGroup);
    m_connPoolPropsPage = new QWidget(m_connPropsStack);
    auto* poolPropsPageLayout = new QVBoxLayout(m_connPoolPropsPage);
    poolPropsPageLayout->setContentsMargins(0, 0, 0, 0);
    m_poolPropsTable = new QTableWidget(m_connPoolPropsPage);
    m_poolPropsTable->setColumnCount(3);
    m_poolPropsTable->setHorizontalHeaderLabels({trk(QStringLiteral("t_prop_col_001"),
                                                     QStringLiteral("Propiedad"),
                                                     QStringLiteral("Property"),
                                                     QStringLiteral("属性")),
                                                 trk(QStringLiteral("t_value_col_001"),
                                                     QStringLiteral("Valor"),
                                                     QStringLiteral("Value"),
                                                     QStringLiteral("值")),
                                                 trk(QStringLiteral("t_origin_col001"),
                                                     QStringLiteral("Origen"),
                                                     QStringLiteral("Source"),
                                                     QStringLiteral("来源"))});
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_poolPropsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_poolPropsTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_poolPropsTable->verticalHeader()->setVisible(false);
    m_poolPropsTable->verticalHeader()->setDefaultSectionSize(22);
    enableSortableHeader(m_poolPropsTable);
    auto* connPropBtns = new QHBoxLayout();
    connPropBtns->setContentsMargins(0, 0, 0, 0);
    connPropBtns->setSpacing(6);
    m_connPropsRefreshBtn = new QPushButton(
        trk(QStringLiteral("t_refresh_conn_ctx001"),
            QStringLiteral("Refrescar"),
            QStringLiteral("Refresh"),
            QStringLiteral("刷新")),
        m_connPoolPropsPage);
    m_connPropsEditBtn = new QPushButton(
        trk(QStringLiteral("t_edit_conn_ctx001"),
            QStringLiteral("Editar"),
            QStringLiteral("Edit"),
            QStringLiteral("编辑")),
        m_connPoolPropsPage);
    m_connPropsDeleteBtn = new QPushButton(
        trk(QStringLiteral("t_del_conn_ctx001"),
            QStringLiteral("Borrar"),
            QStringLiteral("Delete"),
            QStringLiteral("删除")),
        m_connPoolPropsPage);
    m_connPropsRefreshBtn->setEnabled(false);
    m_connPropsEditBtn->setEnabled(false);
    m_connPropsDeleteBtn->setEnabled(false);
    connPropBtns->addWidget(m_connPropsRefreshBtn, 0);
    connPropBtns->addWidget(m_connPropsEditBtn, 0);
    connPropBtns->addWidget(m_connPropsDeleteBtn, 0);
    connPropBtns->addStretch(1);
    auto* poolPropBtns = new QHBoxLayout();
    poolPropBtns->setContentsMargins(0, 0, 0, 0);
    poolPropBtns->setSpacing(6);
    m_poolStatusRefreshBtn = new QPushButton(trk(QStringLiteral("t_refresh_btn001"),
                                                 QStringLiteral("Actualizar"),
                                                 QStringLiteral("Refresh"),
                                                 QStringLiteral("刷新")),
                                             m_connPoolPropsPage);
    m_poolStatusImportBtn = new QPushButton(trk(QStringLiteral("t_import_btn001"),
                                                QStringLiteral("Importar"),
                                                QStringLiteral("Import"),
                                                QStringLiteral("导入")),
                                            m_connPoolPropsPage);
    m_poolStatusExportBtn = new QPushButton(trk(QStringLiteral("t_export_btn001"),
                                                QStringLiteral("Exportar"),
                                                QStringLiteral("Export"),
                                                QStringLiteral("导出")),
                                            m_connPoolPropsPage);
    m_poolStatusScrubBtn = new QPushButton(QStringLiteral("Scrub"), m_connPoolPropsPage);
    m_poolStatusDestroyBtn = new QPushButton(QStringLiteral("Destroy"), m_connPoolPropsPage);
    m_poolStatusDestroyBtn->setStyleSheet(
        QStringLiteral("QPushButton:enabled { color: #b00020; font-weight: 700; }"
                       "QPushButton:disabled { color: palette(buttonText); font-weight: 400; }"));
    m_poolStatusRefreshBtn->setEnabled(false);
    m_poolStatusImportBtn->setEnabled(false);
    m_poolStatusExportBtn->setEnabled(false);
    m_poolStatusScrubBtn->setEnabled(false);
    m_poolStatusDestroyBtn->setEnabled(false);
    poolPropBtns->addWidget(m_poolStatusRefreshBtn, 0);
    poolPropBtns->addWidget(m_poolStatusImportBtn, 0);
    poolPropBtns->addWidget(m_poolStatusExportBtn, 0);
    poolPropBtns->addWidget(m_poolStatusScrubBtn, 0);
    poolPropBtns->addWidget(m_poolStatusDestroyBtn, 0);
    poolPropBtns->addStretch(1);
    poolPropsPageLayout->addLayout(poolPropBtns);
    poolPropsPageLayout->addLayout(connPropBtns);
    poolPropsPageLayout->addWidget(m_poolPropsTable, 1);
    m_connPropsStack->addWidget(m_connPoolPropsPage);

    m_connContentPage = new QWidget(m_connPropsStack);
    auto* connContentLayout = new QVBoxLayout(m_connContentPage);
    connContentLayout->setContentsMargins(0, 0, 0, 0);
    connContentLayout->setSpacing(4);
    m_connContentTree = new QTreeWidget(m_connContentPage);
    m_connContentTree->setColumnCount(4);
    m_connContentTree->setHeaderLabels({trk(QStringLiteral("t_dataset_001"),
                                            QStringLiteral("Dataset"),
                                            QStringLiteral("Dataset"),
                                            QStringLiteral("数据集")),
                                        trk(QStringLiteral("t_snapshot_col01"),
                                            QStringLiteral("Snapshot"),
                                            QStringLiteral("Snapshot"),
                                            QStringLiteral("快照")),
                                        trk(QStringLiteral("t_montado_a97484"),
                                            QStringLiteral("Montado"),
                                            QStringLiteral("Mounted"),
                                            QStringLiteral("已挂载")),
                                        trk(QStringLiteral("t_mountpoint_001"),
                                            QStringLiteral("Mountpoint"),
                                            QStringLiteral("Mountpoint"),
                                            QStringLiteral("挂载点"))});
    m_connContentTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_connContentTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_connContentTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_connContentTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_connContentTree->header()->setStretchLastSection(true);
    m_connContentTree->setColumnWidth(0, 250);
    m_connContentTree->setColumnWidth(1, 90);
    m_connContentTree->setColumnWidth(2, 72);
    m_connContentTree->setColumnWidth(3, 180);
    m_connContentTree->setUniformRowHeights(true);
    m_connContentTree->setRootIsDecorated(true);
    m_connContentTree->setItemsExpandable(true);
    {
        QFont f = m_connContentTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_connContentTree->setFont(f);
    }
    m_connContentTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
    m_connContentTree->setItemDelegate(new ConnContentPropBorderDelegate(m_connContentTree));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_connContentTree->setStyle(fusion);
    }
#endif
    // Las acciones se exponen por menú contextual del árbol.
    if (m_btnConnBreakdown) m_btnConnBreakdown->setVisible(false);
    if (m_btnConnAssemble) m_btnConnAssemble->setVisible(false);
    if (m_btnConnFromDir) m_btnConnFromDir->setVisible(false);
    if (m_btnConnToDir) m_btnConnToDir->setVisible(false);
    connContentLayout->addWidget(m_connContentTree, 1);
    m_btnApplyConnContentProps->setEnabled(false);
    m_connPropsStack->addWidget(m_connContentPage);
    m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
    propsPoolLayout->addWidget(m_connPropsStack, 1);

    m_connBottomGroup = new QWidget(m_poolDetailTabs);
    auto* statusPoolLayout = new QVBoxLayout(m_connBottomGroup);
    statusPoolLayout->setContentsMargins(0, 0, 0, 0);
    statusPoolLayout->setSpacing(4);
    m_connBottomStack = new QStackedWidget(m_connBottomGroup);
    m_connStatusPage = new QWidget(m_connBottomStack);
    auto* statusPageLayout = new QVBoxLayout(m_connStatusPage);
    statusPageLayout->setContentsMargins(0, 0, 0, 0);
    m_poolStatusText = new QPlainTextEdit(m_connStatusPage);
    m_poolStatusText->setReadOnly(true);
    m_poolStatusText->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_poolStatusText->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    {
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSize(9);
        m_poolStatusText->setFont(mono);
    }
    statusPageLayout->addWidget(m_poolStatusText, 1);
    m_connBottomStack->addWidget(m_connStatusPage);

    m_connDatasetPropsPage = new QWidget(m_connBottomStack);
    auto* connDsPropsLayout = new QVBoxLayout(m_connDatasetPropsPage);
    connDsPropsLayout->setContentsMargins(0, 0, 0, 0);
    connDsPropsLayout->setSpacing(4);
    m_connContentPropsTable = new QTableWidget(m_connDatasetPropsPage);
    m_connContentPropsTable->setColumnCount(3);
    m_connContentPropsTable->setHorizontalHeaderLabels({trk(QStringLiteral("t_prop_col_001"),
                                                             QStringLiteral("Propiedad"),
                                                             QStringLiteral("Property"),
                                                             QStringLiteral("属性")),
                                                        trk(QStringLiteral("t_value_col_001"),
                                                            QStringLiteral("Valor"),
                                                            QStringLiteral("Value"),
                                                            QStringLiteral("值")),
                                                        trk(QStringLiteral("t_inherit_col001"),
                                                            QStringLiteral("Inherit"),
                                                            QStringLiteral("Inherit"),
                                                            QStringLiteral("继承"))});
    m_connContentPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_connContentPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_connContentPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_connContentPropsTable->horizontalHeader()->setStretchLastSection(false);
    m_connContentPropsTable->setColumnWidth(0, 200);
    m_connContentPropsTable->setColumnWidth(1, 210);
    const int inheritTextPx = m_connContentPropsTable->fontMetrics().horizontalAdvance(QStringLiteral("Inherit"));
    const int connInheritColWidth = qMax(36, static_cast<int>(((static_cast<double>(inheritTextPx) / 2.0) + 12.0) * 1.10));
    m_connContentPropsTable->setColumnWidth(2, connInheritColWidth);
    m_connContentPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_connContentPropsTable->verticalHeader()->setVisible(false);
    m_connContentPropsTable->verticalHeader()->setDefaultSectionSize(22);
    enableSortableHeader(m_connContentPropsTable);
    connDsPropsLayout->addWidget(m_connContentPropsTable, 1);
    m_connBottomStack->addWidget(m_connDatasetPropsPage);
    m_connBottomStack->setCurrentWidget(m_connStatusPage);
    statusPoolLayout->addWidget(m_connBottomStack, 1);
    auto* subtabContentFrame = new QFrame(detailContainer);
    subtabContentFrame->setObjectName(QStringLiteral("zfsmgrSubtabContentFrame"));
    subtabContentFrame->setFrameShape(QFrame::NoFrame);
    subtabContentFrame->setFrameShadow(QFrame::Plain);
    subtabContentFrame->setLineWidth(0);
    auto* subtabContentLayout = new QVBoxLayout(subtabContentFrame);
    subtabContentLayout->setContentsMargins(0, 0, 0, 0);
    subtabContentLayout->setSpacing(0);
    m_connDetailSplit = new QSplitter(Qt::Vertical, subtabContentFrame);
    m_connDetailSplit->setChildrenCollapsible(false);
    m_connDetailSplit->setHandleWidth(1);
    m_connDetailSplit->addWidget(m_connPropsGroup);
    m_connDetailSplit->addWidget(m_connBottomGroup);
    m_connDetailSplit->setStretchFactor(0, 55);
    m_connDetailSplit->setStretchFactor(1, 45);
    subtabContentLayout->addWidget(m_connDetailSplit, 1);
    detailContainerLayout->addWidget(subtabContentFrame, 1);
    poolDetailLayout->addWidget(detailContainer, 1);
    entityFrameLayout->addWidget(m_poolDetailTabs, 1);
    rightConnectionsLayout->setSpacing(0);
    rightConnectionsLayout->addWidget(entityFrame, 1);

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
    auto* originLabel = new QLabel(trk(QStringLiteral("t_origin_lbl_001"),
                                       QStringLiteral("Origen"),
                                       QStringLiteral("Source"),
                                       QStringLiteral("源")),
                                   originPane);
    m_originPoolCombo = new QComboBox(originPane);
    m_originPoolCombo->setMinimumContentsLength(8);
    m_originPoolCombo->setMaximumWidth(140);
    m_originPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_originTree = new QTreeWidget(originPane);
    m_originTree->setColumnCount(4);
    m_originTree->setHeaderLabels({trk(QStringLiteral("t_dataset_001"),
                                       QStringLiteral("Dataset"),
                                       QStringLiteral("Dataset"),
                                       QStringLiteral("数据集")),
                                   trk(QStringLiteral("t_snapshot_col01"),
                                       QStringLiteral("Snapshot"),
                                       QStringLiteral("Snapshot"),
                                       QStringLiteral("快照")),
                                   trk(QStringLiteral("t_montado_a97484"),
                                       QStringLiteral("Montado"),
                                       QStringLiteral("Mounted"),
                                       QStringLiteral("已挂载")),
                                   trk(QStringLiteral("t_mountpoint_001"),
                                       QStringLiteral("Mountpoint"),
                                       QStringLiteral("Mountpoint"),
                                       QStringLiteral("挂载点"))});
    m_originTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_originTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_originTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_originTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_originTree->header()->setStretchLastSection(false);
    m_originTree->setColumnWidth(0, 250);
    m_originTree->setColumnWidth(1, 90);
    m_originTree->setColumnWidth(2, 72);
    m_originTree->setColumnWidth(3, 180);
    m_originTree->setUniformRowHeights(true);
    m_originTree->setRootIsDecorated(true);
    m_originTree->setItemsExpandable(true);
    {
        QFont f = m_originTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_originTree->setFont(f);
    }
    m_originTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_originTree->setStyle(fusion);
    }
#endif
    m_originSelectionLabel = new QLabel(trk(QStringLiteral("t_no_sel_001"),
                                            QStringLiteral("(sin selección)"),
                                            QStringLiteral("(no selection)"),
                                            QStringLiteral("（未选择）")),
                                        originPane);
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
    auto* destLabel = new QLabel(trk(QStringLiteral("t_target_lbl001"),
                                     QStringLiteral("Destino"),
                                     QStringLiteral("Target"),
                                     QStringLiteral("目标")),
                                 destPane);
    m_destPoolCombo = new QComboBox(destPane);
    m_destPoolCombo->setMinimumContentsLength(8);
    m_destPoolCombo->setMaximumWidth(140);
    m_destPoolCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_destTree = new QTreeWidget(destPane);
    m_destTree->setColumnCount(4);
    m_destTree->setHeaderLabels({trk(QStringLiteral("t_dataset_001"),
                                     QStringLiteral("Dataset"),
                                     QStringLiteral("Dataset"),
                                     QStringLiteral("数据集")),
                                 trk(QStringLiteral("t_snapshot_col01"),
                                     QStringLiteral("Snapshot"),
                                     QStringLiteral("Snapshot"),
                                     QStringLiteral("快照")),
                                 trk(QStringLiteral("t_montado_a97484"),
                                     QStringLiteral("Montado"),
                                     QStringLiteral("Mounted"),
                                     QStringLiteral("已挂载")),
                                 trk(QStringLiteral("t_mountpoint_001"),
                                     QStringLiteral("Mountpoint"),
                                     QStringLiteral("Mountpoint"),
                                     QStringLiteral("挂载点"))});
    m_destTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_destTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_destTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_destTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_destTree->header()->setStretchLastSection(false);
    m_destTree->setColumnWidth(0, 250);
    m_destTree->setColumnWidth(1, 90);
    m_destTree->setColumnWidth(2, 72);
    m_destTree->setColumnWidth(3, 180);
    m_destTree->setUniformRowHeights(true);
    m_destTree->setRootIsDecorated(true);
    m_destTree->setItemsExpandable(true);
    {
        QFont f = m_destTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_destTree->setFont(f);
    }
    m_destTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_destTree->setStyle(fusion);
    }
#endif
    m_destSelectionLabel = new QLabel(trk(QStringLiteral("t_no_sel_001"),
                                          QStringLiteral("(sin selección)"),
                                          QStringLiteral("(no selection)"),
                                          QStringLiteral("（未选择）")),
                                      destPane);
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

    m_rightStack->addWidget(rightConnectionsPage);
    m_rightStack->addWidget(rightDatasetsPage);
    auto* rightSplit = new QSplitter(Qt::Vertical, rightPane);
    rightSplit->setChildrenCollapsible(false);
    rightSplit->setHandleWidth(4);
    rightSplit->addWidget(m_rightStack);
    auto* bottomConnBox = new QWidget(rightSplit);
    auto* bottomConnLayout = new QVBoxLayout(bottomConnBox);
    bottomConnLayout->setContentsMargins(6, 6, 6, 6);
    m_bottomConnectionEntityTabs = new QTabBar(bottomConnBox);
    m_bottomConnectionEntityTabs->setObjectName(QStringLiteral("zfsmgrEntityTabsBottom"));
    m_bottomConnectionEntityTabs->setExpanding(false);
    m_bottomConnectionEntityTabs->setDrawBase(false);
    m_bottomConnectionEntityTabs->setUsesScrollButtons(true);
    m_bottomConnectionEntityTabs->setVisible(false);
    bottomConnLayout->addWidget(m_bottomConnectionEntityTabs, 0);
    m_bottomConnContentTree = new QTreeWidget(bottomConnBox);
    m_bottomConnContentTree->setColumnCount(4);
    m_bottomConnContentTree->setHeaderLabels({trk(QStringLiteral("t_dataset_001"),
                                                   QStringLiteral("Dataset"),
                                                   QStringLiteral("Dataset"),
                                                   QStringLiteral("数据集")),
                                               trk(QStringLiteral("t_snapshot_col01"),
                                                   QStringLiteral("Snapshot"),
                                                   QStringLiteral("Snapshot"),
                                                   QStringLiteral("快照")),
                                               trk(QStringLiteral("t_montado_a97484"),
                                                   QStringLiteral("Montado"),
                                                   QStringLiteral("Mounted"),
                                                   QStringLiteral("已挂载")),
                                               trk(QStringLiteral("t_mountpoint_001"),
                                                   QStringLiteral("Mountpoint"),
                                                   QStringLiteral("Mountpoint"),
                                                   QStringLiteral("挂载点"))});
    m_bottomConnContentTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_bottomConnContentTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_bottomConnContentTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_bottomConnContentTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_bottomConnContentTree->header()->setStretchLastSection(true);
    m_bottomConnContentTree->setColumnWidth(0, 230);
    m_bottomConnContentTree->setColumnWidth(1, 90);
    m_bottomConnContentTree->setColumnWidth(2, 72);
    m_bottomConnContentTree->setColumnWidth(3, 170);
    m_bottomConnContentTree->setUniformRowHeights(true);
    m_bottomConnContentTree->setRootIsDecorated(true);
    m_bottomConnContentTree->setItemsExpandable(true);
    m_bottomConnContentTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
    m_bottomConnContentTree->setItemDelegate(new ConnContentPropBorderDelegate(m_bottomConnContentTree));
    bottomConnLayout->addWidget(m_bottomConnContentTree, 1);
    rightSplit->setStretchFactor(0, 1);
    rightSplit->setStretchFactor(1, 1);
    rightSplit->setSizes({500, 500});
    auto equalizeTreeHeights = [this, rightSplit, rightConnectionsPage, bottomConnBox]() {
        QTreeWidget* topTree = nullptr;
        if (m_connContentPage) {
            topTree = m_connContentPage->findChild<QTreeWidget*>();
        }
        if (!rightSplit || !topTree || !m_bottomConnContentTree
            || !rightConnectionsPage || !bottomConnBox) {
            return;
        }
        const int total = rightSplit->size().height() > 0
                              ? rightSplit->size().height()
                              : rightSplit->sizes().value(0, 0) + rightSplit->sizes().value(1, 0);
        if (total <= 0) {
            return;
        }
        const int topTreeH = topTree->viewport() ? topTree->viewport()->height() : topTree->height();
        const int bottomTreeH = m_bottomConnContentTree->viewport() ? m_bottomConnContentTree->viewport()->height()
                                                                     : m_bottomConnContentTree->height();
        if (topTreeH <= 0 || bottomTreeH <= 0) {
            int topH = total / 2;
            topH = qBound(120, topH, total - 120);
            rightSplit->setSizes({topH, total - topH});
            return;
        }
        const int delta = topTreeH - bottomTreeH;
        if (qAbs(delta) <= 1) {
            return;
        }
        QList<int> sizes = rightSplit->sizes();
        if (sizes.size() < 2) {
            int topH = total / 2;
            topH = qBound(120, topH, total - 120);
            rightSplit->setSizes({topH, total - topH});
            return;
        }
        int topH = sizes[0] - (delta / 2);
        topH = qBound(120, topH, total - 120);
        rightSplit->setSizes({topH, total - topH});
    };
    QTimer::singleShot(0, this, equalizeTreeHeights);
    QTimer::singleShot(80, this, equalizeTreeHeights);
    QTimer::singleShot(180, this, equalizeTreeHeights);
    QTimer::singleShot(320, this, equalizeTreeHeights);
    QTimer::singleShot(520, this, equalizeTreeHeights);
    QTimer::singleShot(900, this, equalizeTreeHeights);
    rightLayout->addWidget(rightSplit, 1);

    topLayout->addWidget(leftPane, 0);
    topLayout->addWidget(rightPane, 1);

    auto* logBox = new QGroupBox(trk(QStringLiteral("t_combined_log001"),
                                     QStringLiteral("Log combinado"),
                                     QStringLiteral("Combined log"),
                                     QStringLiteral("组合日志")),
                                 central);
    auto* logLayout = new QVBoxLayout(logBox);
    logLayout->setContentsMargins(6, 6, 6, 6);
    logLayout->setSpacing(4);
    auto* logBody = new QHBoxLayout();

    auto* leftInfo = new QWidget(logBox);
    auto* leftInfoLayout = new QVBoxLayout(leftInfo);
    leftInfoLayout->setContentsMargins(0, 0, 0, 0);
    leftInfoLayout->setSpacing(4);
    auto* statusTitle = new QLabel(trk(QStringLiteral("t_status_col_001"),
                                       QStringLiteral("Estado"),
                                       QStringLiteral("Status"),
                                       QStringLiteral("状态")),
                                   leftInfo);
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
    {
        const QFontMetrics statusFm(m_statusText->font());
        const int h = statusFm.lineSpacing() * 2 + 10;
        m_statusText->setFixedHeight(h);
        m_statusText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    auto* detailTitle = new QLabel(trk(QStringLiteral("t_detail_lbl001"),
                                       QStringLiteral("Progreso"),
                                       QStringLiteral("Progress"),
                                       QStringLiteral("进度")),
                                   leftInfo);
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
    statusTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    detailTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    leftInfoLayout->addWidget(statusTitle, 0);
    leftInfoLayout->addWidget(m_statusText, 0);
    leftInfoLayout->addWidget(detailTitle, 0);
    m_lastDetailText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    leftInfoLayout->addWidget(m_lastDetailText, 1);

    auto* rightLogs = new QWidget(logBox);
    auto* rightLogsLayout = new QVBoxLayout(rightLogs);
    rightLogsLayout->setContentsMargins(0, 0, 0, 0);
    rightLogsLayout->setSpacing(4);
    auto* rightLogsBody = new QHBoxLayout();
    rightLogsBody->setContentsMargins(0, 0, 0, 0);
    rightLogsBody->setSpacing(6);
    auto* appLogBox = new QGroupBox(
        trk(QStringLiteral("t_app_tab_001"),
            QStringLiteral("Aplicación"),
            QStringLiteral("Application"),
            QStringLiteral("应用")),
        rightLogs);
    auto* appLogLayout = new QVBoxLayout(appLogBox);
    appLogLayout->setContentsMargins(6, 8, 6, 6);
    appLogLayout->setSpacing(4);
    m_logView = new QPlainTextEdit(appLogBox);
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_logView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(8);
    m_logView->setFont(mono);
    appLogLayout->addWidget(m_logView, 1);

    loadPersistedAppLogToView();
    rightLogsBody->addWidget(appLogBox, 1);
    rightLogsLayout->addLayout(rightLogsBody, 1);

    logBody->addWidget(leftInfo, 1);
    logBody->addWidget(rightLogs, 3);
    logLayout->addLayout(logBody, 1);

    auto* verticalMainSplit = new QSplitter(Qt::Vertical, central);
    verticalMainSplit->setChildrenCollapsible(true);
    verticalMainSplit->setHandleWidth(4);
    verticalMainSplit->addWidget(topArea);
    verticalMainSplit->addWidget(logBox);
    verticalMainSplit->setStretchFactor(0, 81);
    verticalMainSplit->setStretchFactor(1, 19);
    verticalMainSplit->setSizes({810, 190});
    root->addWidget(verticalMainSplit, 1);

    setCentralWidget(central);

    if (m_connPropsRefreshBtn) {
        connect(m_connPropsRefreshBtn, &QPushButton::clicked, this, [this]() {
            logUiAction(QStringLiteral("Refrescar conexión (botón propiedades)"));
            refreshSelectedConnection();
        });
    }
    if (m_connPropsEditBtn) {
        connect(m_connPropsEditBtn, &QPushButton::clicked, this, [this]() {
            logUiAction(QStringLiteral("Editar conexión (botón propiedades)"));
            editConnection();
        });
    }
    if (m_connPropsDeleteBtn) {
        connect(m_connPropsDeleteBtn, &QPushButton::clicked, this, [this]() {
            logUiAction(QStringLiteral("Borrar conexión (botón propiedades)"));
            deleteConnection();
        });
    }
    connect(m_connectionsTable, &QTableWidget::currentCellChanged, this,
            [this](int, int, int, int) { onConnectionSelectionChanged(); });
    connect(m_connectionsTable, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (!m_connectionsTable || row < 0) {
            return;
        }
        if (col == 0 || col == 1) {
            return;
        }
        QTableWidgetItem* it = m_connectionsTable->item(row, 2);
        if (!it) {
            return;
        }
        bool ok = false;
        const int connIdx = it->data(Qt::UserRole).toInt(&ok);
        if (!ok || connIdx < 0 || connIdx >= m_profiles.size()) {
            return;
        }
        m_userSelectedConnectionKey = m_profiles[connIdx].id.trimmed().toLower();
        if (m_userSelectedConnectionKey.isEmpty()) {
            m_userSelectedConnectionKey = m_profiles[connIdx].name.trimmed().toLower();
        }
    });
    connect(m_connectionsTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!item || m_syncConnSelectorChecks || !m_connectionsTable) {
            return;
        }
        const int col = item->column();
        if (col != 0 && col != 1) {
            return;
        }
        bool ok = false;
        const int connIdx = item->data(Qt::UserRole).toInt(&ok);
        if (!ok || connIdx < 0 || connIdx >= m_profiles.size()) {
            return;
        }
        const bool checked = item->checkState() == Qt::Checked;
        m_syncConnSelectorChecks = true;
        if (col == 0) {
            if (checked) {
                m_topDetailConnIdx = connIdx;
            } else if (m_topDetailConnIdx == connIdx) {
                m_topDetailConnIdx = -1;
            }
            for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
                QTableWidgetItem* it = m_connectionsTable->item(r, 0);
                if (!it) {
                    continue;
                }
                const int idx = it->data(Qt::UserRole).toInt();
                it->setCheckState((idx == m_topDetailConnIdx) ? Qt::Checked : Qt::Unchecked);
            }
        } else {
            if (checked) {
                m_bottomDetailConnIdx = connIdx;
            } else if (m_bottomDetailConnIdx == connIdx) {
                m_bottomDetailConnIdx = -1;
            }
            for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
                QTableWidgetItem* it = m_connectionsTable->item(r, 1);
                if (!it) {
                    continue;
                }
                const int idx = it->data(Qt::UserRole).toInt();
                it->setCheckState((idx == m_bottomDetailConnIdx) ? Qt::Checked : Qt::Unchecked);
            }
        }
        m_syncConnSelectorChecks = false;
        if (col == 0 && checked) {
            for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
                QTableWidgetItem* it = m_connectionsTable->item(r, 2);
                if (!it) {
                    continue;
                }
                const int idx = it->data(Qt::UserRole).toInt();
                if (idx == connIdx) {
                    m_connectionsTable->setCurrentCell(r, 2);
                    break;
                }
            }
        }
        updateSecondaryConnectionDetail();
    });
    if (m_connectionEntityTabs) {
        connect(m_connectionEntityTabs, &QTabBar::currentChanged, this, [this](int idx) {
            onConnectionEntityTabChanged(idx);
        });
    }
    if (m_bottomConnectionEntityTabs) {
        connect(m_bottomConnectionEntityTabs, &QTabBar::currentChanged, this, [this](int idx) {
            if (!m_bottomConnectionEntityTabs || !m_bottomConnContentTree) {
                return;
            }
            if (idx < 0 || idx >= m_bottomConnectionEntityTabs->count()) {
                m_bottomConnContentTree->clear();
                return;
            }
            const QString key = m_bottomConnectionEntityTabs->tabData(idx).toString();
            const QStringList parts = key.split(':');
            if (parts.size() < 3 || parts.first() != QStringLiteral("pool")) {
                m_bottomConnContentTree->clear();
                return;
            }
            bool ok = false;
            const int connIdx = parts.value(1).toInt(&ok);
            const QString poolName = parts.value(2).trimmed();
            if (!ok || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
                m_bottomConnContentTree->clear();
                return;
            }
            saveBottomConnectionNavState(connIdx);
            const QString prevToken = m_connContentToken;
            QTreeWidget* prevTree = m_connContentTree;
            m_connContentTree = m_bottomConnContentTree;
            m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            populateDatasetTree(m_bottomConnContentTree, connIdx, poolName, QStringLiteral("conncontent"), true);
            syncConnContentPoolColumns();
            if (m_bottomConnContentTree->topLevelItemCount() > 0) {
                m_bottomConnContentTree->setCurrentItem(m_bottomConnContentTree->topLevelItem(0));
            }
            m_connContentTree = prevTree;
            m_connContentToken = prevToken;
        });
    }
    auto connTokenFromTreeSelectionBottom = [this](QTreeWidget* tree) -> QString {
        if (!tree) {
            return QString();
        }
        QTreeWidgetItem* owner = tree->currentItem();
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, kIsPoolRootRole).toBool()) {
            owner = owner->parent();
        }
        if (!owner) {
            return QString();
        }
        const int connIdx = owner->data(0, kConnIdxRole).toInt();
        const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
        if (connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
            return QString();
        }
        return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    };
    auto refreshInlinePropsVisualBottom = [this, connTokenFromTreeSelectionBottom](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        {
            QSignalBlocker blocker(tree);
            QTreeWidgetItem* cur = tree->currentItem();
            if (cur && cur->data(0, kConnPropRowRole).toBool() && cur->parent()) {
                tree->setCurrentItem(cur->parent());
            }
        }
        const QString token = connTokenFromTreeSelectionBottom(tree);
        if (token.isEmpty()) {
            return;
        }
        const QString prevToken = m_connContentToken;
        QTreeWidget* prevTree = m_connContentTree;
        m_connContentTree = tree;
        m_connContentToken = token;
        {
            QSignalBlocker blocker(tree);
            syncConnContentPropertyColumns();
        }
        m_connContentTree = prevTree;
        m_connContentToken = prevToken;
    };
    auto openEditDatasetDialogBottom = [this](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
        if (!ctx.valid || ctx.datasetName.trimmed().isEmpty() || !ctx.snapshotName.trimmed().isEmpty()) {
            return;
        }
        refreshDatasetProperties(QStringLiteral("conncontent"));
        if (!m_connContentPropsTable || m_connContentPropsTable->rowCount() <= 0) {
            return;
        }
        struct EditField { int row{-1}; QLineEdit* line{nullptr}; QComboBox* combo{nullptr}; QCheckBox* inherit{nullptr}; };
        QVector<EditField> fields;
        QDialog dlg(this);
        dlg.setWindowTitle(
            trk(QStringLiteral("t_edit_ds_t_001"), QStringLiteral("Editar dataset"), QStringLiteral("Edit dataset"), QStringLiteral("编辑数据集"))
            + QStringLiteral(": ") + ctx.datasetName);
        dlg.setModal(true);
        dlg.resize(760, 520);
        auto* root = new QVBoxLayout(&dlg);
        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
            QTableWidgetItem* pk = m_connContentPropsTable->item(r, 0);
            QTableWidgetItem* pv = m_connContentPropsTable->item(r, 1);
            QTableWidgetItem* pi = m_connContentPropsTable->item(r, 2);
            if (!pk || !pv || !pi) continue;
            const QString prop = pk->data(Qt::UserRole + 777).toString().trimmed().isEmpty()
                                     ? pk->text().trimmed()
                                     : pk->data(Qt::UserRole + 777).toString().trimmed();
            const bool valueEditable = (pv->flags() & Qt::ItemIsEditable) || (m_connContentPropsTable->cellWidget(r, 1) != nullptr);
            const bool inheritEditable = (pi->flags() & Qt::ItemIsUserCheckable);
            if (!valueEditable && !inheritEditable) continue;
            auto* rowW = new QWidget(&dlg);
            auto* rowL = new QHBoxLayout(rowW);
            rowL->setContentsMargins(0, 0, 0, 0);
            rowL->setSpacing(8);
            EditField ef; ef.row = r;
            if (QComboBox* srcCb = qobject_cast<QComboBox*>(m_connContentPropsTable->cellWidget(r, 1))) {
                auto* cb = new QComboBox(rowW);
                for (int i = 0; i < srcCb->count(); ++i) cb->addItem(srcCb->itemText(i));
                cb->setCurrentText(srcCb->currentText());
                cb->setEnabled(valueEditable);
                ef.combo = cb;
                rowL->addWidget(cb, 1);
            } else {
                auto* le = new QLineEdit(rowW);
                le->setText(pv->text());
                le->setEnabled(valueEditable);
                le->selectAll();
                ef.line = le;
                rowL->addWidget(le, 1);
            }
            if (inheritEditable) {
                auto* inh = new QCheckBox(trk(QStringLiteral("t_inherit_col001"), QStringLiteral("Inherit"), QStringLiteral("Inherit"), QStringLiteral("继承")), rowW);
                inh->setChecked(pi->checkState() == Qt::Checked);
                ef.inherit = inh;
                rowL->addWidget(inh, 0);
            }
            fields.push_back(ef);
            form->addRow(pk->text(), rowW);
        }
        root->addLayout(form, 1);
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        root->addWidget(btns);
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        for (const EditField& ef : fields) {
            if (ef.row < 0 || ef.row >= m_connContentPropsTable->rowCount()) continue;
            QTableWidgetItem* pv = m_connContentPropsTable->item(ef.row, 1);
            QTableWidgetItem* pi = m_connContentPropsTable->item(ef.row, 2);
            if (pv) {
                const QString v = ef.combo ? ef.combo->currentText() : (ef.line ? ef.line->text() : pv->text());
                pv->setText(v);
                if (QComboBox* srcCb = qobject_cast<QComboBox*>(m_connContentPropsTable->cellWidget(ef.row, 1))) srcCb->setCurrentText(v);
                onDatasetPropsCellChanged(ef.row, 1);
            }
            if (pi && ef.inherit && (pi->flags() & Qt::ItemIsUserCheckable)) {
                pi->setCheckState(ef.inherit->isChecked() ? Qt::Checked : Qt::Unchecked);
                onDatasetPropsCellChanged(ef.row, 2);
            }
        }
        if (m_btnApplyConnContentProps && m_btnApplyConnContentProps->isEnabled()) applyDatasetPropertyChanges();
    };

    if (m_bottomConnContentTree) {
        connect(m_bottomConnContentTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
            if (!m_bottomConnContentTree || m_syncingConnContentColumns) {
                return;
            }
            QTreeWidgetItem* sel = m_bottomConnContentTree->currentItem();
            if (!sel) {
                return;
            }
            QTreeWidgetItem* owner = sel;
            while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
                   && !owner->data(0, kIsPoolRootRole).toBool()) {
                owner = owner->parent();
            }
            if (!owner) {
                return;
            }
            const int connIdx = owner->data(0, kConnIdxRole).toInt();
            const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
            if (connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
                return;
            }
            auto isInfoNodeOrInside = [](QTreeWidgetItem* n) -> bool {
                for (QTreeWidgetItem* p = n; p; p = p->parent()) {
                    if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                        return true;
                    }
                }
                return false;
            };
            const bool isPropRow = sel && sel->data(0, kConnPropRowRole).toBool();
            const bool isPoolContext =
                sel && (sel->data(0, kIsPoolRootRole).toBool() || isInfoNodeOrInside(sel));
            if (isPropRow && !isPoolContext) {
                // No reconstruir propiedades al seleccionar una fila de propiedades de dataset;
                // si no, el combo se destruye al abrirse.
                return;
            }

            const QString prevToken = m_connContentToken;
            QTreeWidget* prevTree = m_connContentTree;
            m_connContentTree = m_bottomConnContentTree;
            m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            if (isPoolContext) {
                if (sel && sel->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                    sel->setExpanded(true);
                }
                syncConnContentPoolColumns();
                setConnectionDestinationSelection(DatasetSelectionContext{});
            } else {
                refreshDatasetProperties(QStringLiteral("conncontent"));
                syncConnContentPropertyColumns();
                const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (actx.valid && !actx.datasetName.isEmpty()) {
                    setConnectionDestinationSelection(actx);
                } else {
                    setConnectionDestinationSelection(DatasetSelectionContext{});
                }
            }
            m_connContentTree = prevTree;
            m_connContentToken = prevToken;
        });
        connect(m_bottomConnContentTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
            if (!m_bottomConnContentTree || !item) {
                return;
            }
            QTreeWidgetItem* owner = item;
            while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
                   && !owner->data(0, kIsPoolRootRole).toBool()) {
                owner = owner->parent();
            }
            if (!owner) {
                return;
            }
            const int connIdx = owner->data(0, kConnIdxRole).toInt();
            const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
            if (connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
                return;
            }
            const QString prevToken = m_connContentToken;
            QTreeWidget* prevTree = m_connContentTree;
            m_connContentTree = m_bottomConnContentTree;
            m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            onDatasetTreeItemChanged(m_bottomConnContentTree, item, col, QStringLiteral("conncontent"));
            m_connContentTree = prevTree;
            m_connContentToken = prevToken;
        });
        m_bottomConnContentTree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_bottomConnContentTree, &QWidget::customContextMenuRequested, this,
                [this, refreshInlinePropsVisualBottom, openEditDatasetDialogBottom](const QPoint& pos) {
            if (!m_bottomConnContentTree) {
                return;
            }
            QTreeWidgetItem* item = m_bottomConnContentTree->itemAt(pos);
            if (!item) {
                return;
            }
            const bool isPropRow = item->data(0, kConnPropRowRole).toBool();
            if (isPropRow && item->parent()) {
                item = item->parent();
            }
            if (m_bottomConnContentTree->currentItem() != item) {
                m_bottomConnContentTree->setCurrentItem(item);
            }

            int connIdx = item->data(0, kConnIdxRole).toInt();
            QString poolName = item->data(0, kPoolNameRole).toString().trimmed();
            if ((connIdx < 0 || poolName.isEmpty()) && m_bottomConnectionEntityTabs) {
                const int tabIdx = m_bottomConnectionEntityTabs->currentIndex();
                if (tabIdx >= 0) {
                    const QString key = m_bottomConnectionEntityTabs->tabData(tabIdx).toString();
                    const QStringList parts = key.split(':');
                    if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
                        bool ok = false;
                        const int idx = parts.value(1).toInt(&ok);
                        if (ok) {
                            connIdx = idx;
                            poolName = parts.value(2).trimmed();
                        }
                    }
                }
            }

            // No tocar la selección de la tabla de conexiones desde este menú:
            // puede reconstruir vistas mientras el menú usa punteros del árbol inferior.
            updateConnectionActionsState();

            QMenu menu(this);
            const bool isPoolRoot = item->data(0, kIsPoolRootRole).toBool();
            if (isPoolRoot) {
                int poolRow = -1;
                if (connIdx >= 0 && connIdx < m_profiles.size() && !poolName.isEmpty()) {
                    poolRow = findPoolRow(m_profiles[connIdx].name.trimmed(), poolName);
                }
                const QString poolAction =
                    (poolRow >= 0 && poolRow < m_poolListEntries.size())
                        ? m_poolListEntries[poolRow].action.trimmed()
                        : QString();
                const bool canImport = (poolAction.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0);
                const bool canExport = (poolAction.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0);
                const bool canScrub = canExport;
                const bool canDestroy = canExport;
                const bool canRefresh = (poolRow >= 0);
                QAction* aUpdate = menu.addAction(
                    trk(QStringLiteral("t_refresh_btn001"),
                        QStringLiteral("Actualizar"),
                        QStringLiteral("Refresh"),
                        QStringLiteral("刷新")));
                QAction* aImport = menu.addAction(
                    trk(QStringLiteral("t_import_btn001"),
                        QStringLiteral("Importar"),
                        QStringLiteral("Import"),
                        QStringLiteral("导入")));
                QAction* aExport = menu.addAction(
                    trk(QStringLiteral("t_export_btn001"),
                        QStringLiteral("Exportar"),
                        QStringLiteral("Export"),
                        QStringLiteral("导出")));
                QAction* aScrub = menu.addAction(QStringLiteral("Scrub"));
                QAction* aDestroy = menu.addAction(QStringLiteral("Destroy"));
                aUpdate->setEnabled(canRefresh);
                aImport->setEnabled(canImport);
                aExport->setEnabled(canExport);
                aScrub->setEnabled(canScrub);
                aDestroy->setEnabled(canDestroy);
                QAction* picked = menu.exec(m_bottomConnContentTree->viewport()->mapToGlobal(pos));
                if (!picked) {
                    return;
                }
                if (picked == aUpdate && canRefresh) {
                    refreshSelectedPoolDetails(true, true);
                } else if (picked == aImport && canImport && poolRow >= 0) {
                    importPoolFromRow(poolRow);
                } else if (picked == aExport && canExport && poolRow >= 0) {
                    exportPoolFromRow(poolRow);
                } else if (picked == aScrub && canScrub && poolRow >= 0) {
                    scrubPoolFromRow(poolRow);
                } else if (picked == aDestroy && canDestroy && poolRow >= 0) {
                    destroyPoolFromRow(poolRow);
                }
                return;
            }

            QAction* aShowInline = menu.addAction(
                trk(QStringLiteral("t_show_inline_001"),
                    QStringLiteral("Mostrar propiedades en línea"),
                    QStringLiteral("Show inline properties"),
                    QStringLiteral("显示内联属性")));
            aShowInline->setCheckable(true);
            aShowInline->setChecked(m_showInlineDatasetProps);
            QAction* aEdit = menu.addAction(
                trk(QStringLiteral("t_edit_528f79"),
                    QStringLiteral("Editar"),
                    QStringLiteral("Edit"),
                    QStringLiteral("编辑")));
            menu.addSeparator();
            QAction* aRollback = menu.addAction(QStringLiteral("Rollback"));
            QAction* aCreate = menu.addAction(
                trk(QStringLiteral("t_create_ch_001"), QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")));
            QAction* aDelete = menu.addAction(
                trk(QStringLiteral("t_delete_menu002"), QStringLiteral("Borrar"), QStringLiteral("Delete"), QStringLiteral("删除")));
            menu.addSeparator();
            QAction* aBreakdown = menu.addAction(
                trk(QStringLiteral("t_breakdown_btn1"), QStringLiteral("Desglosar"), QStringLiteral("Break down"), QStringLiteral("拆分")));
            QAction* aAssemble = menu.addAction(
                trk(QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("组装")));
            QAction* aFromDir = menu.addAction(
                trk(QStringLiteral("t_from_dir_btn1"), QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")));
            QAction* aToDir = menu.addAction(
                trk(QStringLiteral("t_to_dir_btn_001"), QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")));

            const QString prevToken = m_connContentToken;
            QTreeWidget* prevTree = m_connContentTree;
            m_connContentTree = m_bottomConnContentTree;
            m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
            const bool hasConnSel = ctx.valid && !ctx.datasetName.isEmpty();
            const bool hasConnSnap = hasConnSel && !ctx.snapshotName.isEmpty();
            aEdit->setEnabled(!actionsLocked() && hasConnSel && !hasConnSnap);
            aRollback->setEnabled(!actionsLocked() && hasConnSnap);
            aCreate->setEnabled(!actionsLocked() && hasConnSel && !hasConnSnap);
            aDelete->setEnabled(!actionsLocked() && hasConnSel);
            aBreakdown->setEnabled(m_btnConnBreakdown && m_btnConnBreakdown->isEnabled());
            aAssemble->setEnabled(m_btnConnAssemble && m_btnConnAssemble->isEnabled());
            aFromDir->setEnabled(m_btnConnFromDir && m_btnConnFromDir->isEnabled());
            aToDir->setEnabled(m_btnConnToDir && m_btnConnToDir->isEnabled());

            QAction* picked = menu.exec(m_bottomConnContentTree->viewport()->mapToGlobal(pos));
            if (!picked) {
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                return;
            }
            if (picked == aShowInline) {
                m_showInlineDatasetProps = aShowInline->isChecked();
                saveUiSettings();
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                refreshInlinePropsVisualBottom(prevTree);
                refreshInlinePropsVisualBottom(m_bottomConnContentTree);
                return;
            }
            if (picked == aEdit) {
                openEditDatasetDialogBottom(m_bottomConnContentTree);
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                return;
            }
            if (picked == aRollback) {
                const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (!actx.valid || actx.snapshotName.isEmpty()) {
                    m_connContentTree = prevTree;
                    m_connContentToken = prevToken;
                    return;
                }
                const QString snapObj = QStringLiteral("%1@%2").arg(actx.datasetName, actx.snapshotName);
                const auto confirm = QMessageBox::question(
                    this,
                    QStringLiteral("Rollback"),
                    QStringLiteral("¿Confirmar rollback de snapshot?\n%1").arg(snapObj),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (confirm != QMessageBox::Yes) {
                    m_connContentTree = prevTree;
                    m_connContentToken = prevToken;
                    return;
                }
                logUiAction(QStringLiteral("Rollback snapshot (menú Contenido inferior)"));
                QString q = snapObj;
                q.replace('\'', "'\"'\"'");
                const QString cmd = QStringLiteral("zfs rollback '%1'").arg(q);
                executeDatasetAction(QStringLiteral("conncontent"), QStringLiteral("Rollback"), actx, cmd, 90000);
            } else if (picked == aCreate) {
                logUiAction(QStringLiteral("Crear hijo dataset (menú Contenido inferior)"));
                actionCreateChildDataset(QStringLiteral("conncontent"));
            } else if (picked == aDelete) {
                logUiAction(QStringLiteral("Borrar dataset/snapshot (menú Contenido inferior)"));
                actionDeleteDatasetOrSnapshot(QStringLiteral("conncontent"));
            } else if (picked == aBreakdown && m_btnConnBreakdown) {
                m_btnConnBreakdown->click();
            } else if (picked == aAssemble && m_btnConnAssemble) {
                m_btnConnAssemble->click();
            } else if (picked == aFromDir && m_btnConnFromDir) {
                m_btnConnFromDir->click();
            } else if (picked == aToDir && m_btnConnToDir) {
                m_btnConnToDir->click();
            }
            m_connContentTree = prevTree;
            m_connContentToken = prevToken;
        });
    }
    m_connectionsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_connectionsTable, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!m_connectionsTable) {
            return;
        }
        const QModelIndex idxAt = m_connectionsTable->indexAt(pos);
        if (idxAt.isValid() && idxAt.row() >= 0) {
            m_connectionsTable->setCurrentCell(idxAt.row(), 2);
        }
        int connIdx = -1;
        const int curRow = m_connectionsTable->currentRow();
        if (curRow >= 0 && curRow < m_connectionsTable->rowCount()) {
            QTableWidgetItem* it = m_connectionsTable->item(curRow, 2);
            if (it) {
                bool ok = false;
                const int idx = it->data(Qt::UserRole).toInt(&ok);
                if (ok) {
                    connIdx = idx;
                }
            }
        }
        const bool hasConn = (connIdx >= 0 && connIdx < m_profiles.size());
        const bool canRefresh = hasConn && !actionsLocked();
        const bool canEditDelete = canRefresh && !isLocalConnection(connIdx) && !isConnectionRedirectedToLocal(connIdx);

        QMenu menu(this);
        QAction* aRefresh = menu.addAction(
            trk(QStringLiteral("t_refresh_conn_ctx001"),
                QStringLiteral("Refrescar"),
                QStringLiteral("Refresh"),
                QStringLiteral("刷新")));
        QAction* aEdit = menu.addAction(
            trk(QStringLiteral("t_edit_conn_ctx001"),
                QStringLiteral("Editar"),
                QStringLiteral("Edit"),
                QStringLiteral("编辑")));
        QAction* aDelete = menu.addAction(
            trk(QStringLiteral("t_del_conn_ctx001"),
                QStringLiteral("Borrar"),
                QStringLiteral("Delete"),
                QStringLiteral("删除")));
        menu.addSeparator();
        QAction* aRefreshAll = menu.addAction(
            trk(QStringLiteral("t_refresh_all_001"),
                QStringLiteral("Refrescar todas las conexiones"),
                QStringLiteral("Refresh all connections"),
                QStringLiteral("刷新所有连接")));
        QAction* aNewConn = menu.addAction(
            trk(QStringLiteral("t_new_conn_ctx001"),
                QStringLiteral("Nueva Conexión"),
                QStringLiteral("New Connection"),
                QStringLiteral("新建连接")));
        QAction* aNewPool = menu.addAction(
            trk(QStringLiteral("t_new_pool_ctx_001"),
                QStringLiteral("Nuevo Pool"),
                QStringLiteral("New Pool"),
                QStringLiteral("新建存储池")));
        aRefresh->setEnabled(canRefresh);
        aEdit->setEnabled(canEditDelete);
        aDelete->setEnabled(canEditDelete);
        aRefreshAll->setEnabled(!actionsLocked());
        aNewConn->setEnabled(!actionsLocked());
        aNewPool->setEnabled(!actionsLocked() && hasConn);

        QAction* chosen = menu.exec(m_connectionsTable->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if (chosen == aRefresh) {
            logUiAction(QStringLiteral("Refrescar conexión (menú conexiones)"));
            refreshSelectedConnection();
        } else if (chosen == aEdit) {
            logUiAction(QStringLiteral("Editar conexión (menú conexiones)"));
            editConnection();
        } else if (chosen == aDelete) {
            logUiAction(QStringLiteral("Borrar conexión (menú conexiones)"));
            deleteConnection();
        } else if (chosen == aRefreshAll) {
            logUiAction(QStringLiteral("Refrescar todas las conexiones (menú conexiones)"));
            refreshAllConnections();
        } else if (chosen == aNewConn) {
            logUiAction(QStringLiteral("Nueva conexión (menú conexiones)"));
            createConnection();
        } else if (chosen == aNewPool) {
            logUiAction(QStringLiteral("Nuevo pool (menú conexiones)"));
            createPoolForSelectedConnection();
        }
    });
    if (m_poolViewTabBar) {
        m_connSplitSizesProps = m_connDetailSplit ? m_connDetailSplit->sizes() : QList<int>{};
        m_connSplitSizesContent = m_connSplitSizesProps;
        m_connSplitActiveTab = m_poolViewTabBar->currentIndex();
        connect(m_poolViewTabBar, &QTabBar::currentChanged, this, [this](int idx) {
            if (!m_connPropsStack || !m_connBottomStack) {
                return;
            }
            if (m_connDetailSplit) {
                const QList<int> cur = m_connDetailSplit->sizes();
                if (m_connSplitActiveTab == 0) {
                    m_connSplitSizesProps = cur;
                } else {
                    m_connSplitSizesContent = cur;
                }
            }
            if (idx == 1) {
                if (m_connContentPage) m_connPropsStack->setCurrentWidget(m_connContentPage);
                if (m_connDatasetPropsPage) m_connBottomStack->setCurrentWidget(m_connDatasetPropsPage);
            } else {
                if (m_connPoolPropsPage) m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
                if (m_connStatusPage) m_connBottomStack->setCurrentWidget(m_connStatusPage);
            }
            if (m_connDetailSplit) {
                const QList<int> next = (idx == 1) ? m_connSplitSizesContent : m_connSplitSizesProps;
                if (!next.isEmpty()) {
                    m_connDetailSplit->setSizes(next);
                }
            }
            m_connSplitActiveTab = idx;
            const int connIdx = selectedConnectionIndexForPoolManagement();
            if (connIdx >= 0) {
                saveConnectionNavState(connIdx);
            }
        });
    }
    auto connTokenFromTreeSelection = [this](QTreeWidget* tree) -> QString {
        if (!tree) {
            return QString();
        }
        QTreeWidgetItem* owner = tree->currentItem();
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, kIsPoolRootRole).toBool()) {
            owner = owner->parent();
        }
        if (!owner) {
            return QString();
        }
        const int connIdx = owner->data(0, kConnIdxRole).toInt();
        const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
        if (connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
            return QString();
        }
        return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    };
    auto refreshInlinePropsVisual = [this, connTokenFromTreeSelection](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        const QString token = connTokenFromTreeSelection(tree);
        if (token.isEmpty()) {
            return;
        }
        const QString prevToken = m_connContentToken;
        QTreeWidget* prevTree = m_connContentTree;
        m_connContentTree = tree;
        m_connContentToken = token;
        auto isInfoNodeOrInside = [](QTreeWidgetItem* n) -> bool {
            for (QTreeWidgetItem* p = n; p; p = p->parent()) {
                if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                    return true;
                }
            }
            return false;
        };
        QTreeWidgetItem* sel = tree->currentItem();
        const bool isPoolContext =
            sel && (sel->data(0, kIsPoolRootRole).toBool() || isInfoNodeOrInside(sel));
        if (isPoolContext) {
            syncConnContentPoolColumns();
        } else {
            syncConnContentPropertyColumns();
        }
        m_connContentTree = prevTree;
        m_connContentToken = prevToken;
    };
    auto openEditDatasetDialog = [this](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
        if (!ctx.valid || ctx.datasetName.trimmed().isEmpty() || !ctx.snapshotName.trimmed().isEmpty()) {
            QMessageBox::warning(
                this,
                trk(QStringLiteral("t_edit_ds_t_001"),
                    QStringLiteral("Editar dataset"),
                    QStringLiteral("Edit dataset"),
                    QStringLiteral("编辑数据集")),
                trk(QStringLiteral("t_edit_ds_m_001"),
                    QStringLiteral("Seleccione un dataset (no snapshot) para editar."),
                    QStringLiteral("Select a dataset (not a snapshot) to edit."),
                    QStringLiteral("请选择一个数据集（不是快照）进行编辑。")));
            return;
        }

        refreshDatasetProperties(QStringLiteral("conncontent"));
        if (!m_connContentPropsTable || m_connContentPropsTable->rowCount() <= 0) {
            return;
        }

        struct EditField {
            int row{-1};
            QLineEdit* line{nullptr};
            QComboBox* combo{nullptr};
            QCheckBox* inherit{nullptr};
        };
        QVector<EditField> fields;

        QDialog dlg(this);
        dlg.setWindowTitle(
            trk(QStringLiteral("t_edit_ds_t_001"),
                QStringLiteral("Editar dataset"),
                QStringLiteral("Edit dataset"),
                QStringLiteral("编辑数据集"))
            + QStringLiteral(": ") + ctx.datasetName);
        dlg.setModal(true);
        dlg.resize(760, 520);
        auto* root = new QVBoxLayout(&dlg);
        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
            QTableWidgetItem* pk = m_connContentPropsTable->item(r, 0);
            QTableWidgetItem* pv = m_connContentPropsTable->item(r, 1);
            QTableWidgetItem* pi = m_connContentPropsTable->item(r, 2);
            if (!pk || !pv || !pi) {
                continue;
            }
            const QString prop = pk->data(Qt::UserRole + 777).toString().trimmed().isEmpty()
                                     ? pk->text().trimmed()
                                     : pk->data(Qt::UserRole + 777).toString().trimmed();
            const bool valueEditable = (pv->flags() & Qt::ItemIsEditable) || (m_connContentPropsTable->cellWidget(r, 1) != nullptr);
            const bool inheritEditable = (pi->flags() & Qt::ItemIsUserCheckable);
            if (!valueEditable && !inheritEditable) {
                continue;
            }

            auto* rowW = new QWidget(&dlg);
            auto* rowL = new QHBoxLayout(rowW);
            rowL->setContentsMargins(0, 0, 0, 0);
            rowL->setSpacing(8);
            EditField ef;
            ef.row = r;

            if (QComboBox* srcCb = qobject_cast<QComboBox*>(m_connContentPropsTable->cellWidget(r, 1))) {
                auto* cb = new QComboBox(rowW);
                for (int i = 0; i < srcCb->count(); ++i) {
                    cb->addItem(srcCb->itemText(i));
                }
                cb->setCurrentText(srcCb->currentText());
                cb->setEnabled(valueEditable);
                ef.combo = cb;
                rowL->addWidget(cb, 1);
            } else {
                auto* le = new QLineEdit(rowW);
                le->setText(pv->text());
                le->setEnabled(valueEditable);
                le->selectAll();
                ef.line = le;
                rowL->addWidget(le, 1);
            }

            if (inheritEditable) {
                auto* inh = new QCheckBox(
                    trk(QStringLiteral("t_inherit_col001"),
                        QStringLiteral("Inherit"),
                        QStringLiteral("Inherit"),
                        QStringLiteral("继承")),
                    rowW);
                inh->setChecked(pi->checkState() == Qt::Checked);
                ef.inherit = inh;
                rowL->addWidget(inh, 0);
            }

            fields.push_back(ef);
            form->addRow(pk->text(), rowW);
        }

        root->addLayout(form, 1);
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        root->addWidget(btns);
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() != QDialog::Accepted) {
            return;
        }

        for (const EditField& ef : fields) {
            if (ef.row < 0 || ef.row >= m_connContentPropsTable->rowCount()) {
                continue;
            }
            QTableWidgetItem* pv = m_connContentPropsTable->item(ef.row, 1);
            QTableWidgetItem* pi = m_connContentPropsTable->item(ef.row, 2);
            if (pv) {
                const QString v = ef.combo ? ef.combo->currentText() : (ef.line ? ef.line->text() : pv->text());
                pv->setText(v);
                if (QComboBox* srcCb = qobject_cast<QComboBox*>(m_connContentPropsTable->cellWidget(ef.row, 1))) {
                    srcCb->setCurrentText(v);
                }
                onDatasetPropsCellChanged(ef.row, 1);
            }
            if (pi && ef.inherit && (pi->flags() & Qt::ItemIsUserCheckable)) {
                pi->setCheckState(ef.inherit->isChecked() ? Qt::Checked : Qt::Unchecked);
                onDatasetPropsCellChanged(ef.row, 2);
            }
        }

        if (m_btnApplyConnContentProps && m_btnApplyConnContentProps->isEnabled()) {
            applyDatasetPropertyChanges();
        }
    };

    if (m_connContentTree) {
        connect(m_connContentTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
            if (m_syncingConnContentColumns) {
                return;
            }
            QTreeWidgetItem* sel = m_connContentTree ? m_connContentTree->currentItem() : nullptr;
            auto isInfoNodeOrInside = [](QTreeWidgetItem* n) -> bool {
                for (QTreeWidgetItem* p = n; p; p = p->parent()) {
                    if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                        return true;
                    }
                }
                return false;
            };
            const bool isPropRow = sel && sel->data(0, kConnPropRowRole).toBool();
            const bool isPoolContext =
                sel && (sel->data(0, kIsPoolRootRole).toBool() || isInfoNodeOrInside(sel));
            if (isPropRow && !isPoolContext) {
                updateConnectionDetailTitlesForCurrentSelection();
                updateConnectionActionsState();
                return;
            }
            QTreeWidgetItem* owner = sel;
            while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
                   && !owner->data(0, kIsPoolRootRole).toBool()) {
                owner = owner->parent();
            }
            if (owner) {
                const int connIdx = owner->data(0, kConnIdxRole).toInt();
                const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
                if (connIdx >= 0 && connIdx < m_profiles.size() && !poolName.isEmpty()) {
                    m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
                }
            }
            if (isPoolContext) {
                if (sel && sel->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                    sel->setExpanded(true);
                }
                refreshSelectedPoolDetails(false, true);
                syncConnContentPoolColumns();
                setConnectionOriginSelection(DatasetSelectionContext{});
            } else {
                refreshDatasetProperties(QStringLiteral("conncontent"));
                syncConnContentPropertyColumns();
                const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (actx.valid && !actx.datasetName.isEmpty()) {
                    setConnectionOriginSelection(actx);
                } else {
                    setConnectionOriginSelection(DatasetSelectionContext{});
                }
            }
            updateConnectionDetailTitlesForCurrentSelection();
            updateConnectionActionsState();
        });
        connect(m_connContentTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) {});
        m_connContentTree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_connContentTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
            onDatasetTreeItemChanged(m_connContentTree, item, col, QStringLiteral("conncontent"));
            updateConnectionActionsState();
        });
        connect(m_connContentTree, &QWidget::customContextMenuRequested, this,
                [this, refreshInlinePropsVisual, openEditDatasetDialog](const QPoint& pos) {
            if (!m_connContentTree) {
                return;
            }
            QTreeWidgetItem* item = m_connContentTree->itemAt(pos);
            if (!item) {
                return;
            }
            const bool isPropRow = item->data(0, kConnPropRowRole).toBool();
            if (isPropRow && item->parent()) {
                item = item->parent();
            }
            if (m_connContentTree->currentItem() != item) {
                m_connContentTree->setCurrentItem(item);
            }
            updateConnectionActionsState();

            QMenu menu(this);
            const bool isPoolRoot = item->data(0, kIsPoolRootRole).toBool();
            if (isPoolRoot) {
                const int connIdx = item->data(0, kConnIdxRole).toInt();
                const QString poolName = item->data(0, kPoolNameRole).toString().trimmed();
                int poolRow = -1;
                if (connIdx >= 0 && connIdx < m_profiles.size() && !poolName.isEmpty()) {
                    poolRow = findPoolRow(m_profiles[connIdx].name.trimmed(), poolName);
                }
                const QString poolAction =
                    (poolRow >= 0 && poolRow < m_poolListEntries.size())
                        ? m_poolListEntries[poolRow].action.trimmed()
                        : QString();
                const bool canImport = (poolAction.compare(QStringLiteral("Importar"), Qt::CaseInsensitive) == 0);
                const bool canExport = (poolAction.compare(QStringLiteral("Exportar"), Qt::CaseInsensitive) == 0);
                const bool canScrub = canExport;
                const bool canDestroy = canExport;
                const bool canRefresh = (poolRow >= 0);

                QAction* aUpdate = menu.addAction(
                    trk(QStringLiteral("t_refresh_btn001"),
                        QStringLiteral("Actualizar"),
                        QStringLiteral("Refresh"),
                        QStringLiteral("刷新")));
                QAction* aImport = menu.addAction(
                    trk(QStringLiteral("t_import_btn001"),
                        QStringLiteral("Importar"),
                        QStringLiteral("Import"),
                        QStringLiteral("导入")));
                QAction* aExport = menu.addAction(
                    trk(QStringLiteral("t_export_btn001"),
                        QStringLiteral("Exportar"),
                        QStringLiteral("Export"),
                        QStringLiteral("导出")));
                QAction* aScrub = menu.addAction(QStringLiteral("Scrub"));
                QAction* aDestroy = menu.addAction(QStringLiteral("Destroy"));
                aUpdate->setEnabled(canRefresh);
                aImport->setEnabled(canImport);
                aExport->setEnabled(canExport);
                aScrub->setEnabled(canScrub);
                aDestroy->setEnabled(canDestroy);
                QAction* picked = menu.exec(m_connContentTree->viewport()->mapToGlobal(pos));
                if (!picked) {
                    return;
                }
                if (picked == aUpdate && canRefresh) {
                    refreshSelectedPoolDetails(true, true);
                } else if (picked == aImport && canImport && poolRow >= 0) {
                    importPoolFromRow(poolRow);
                } else if (picked == aExport && canExport && poolRow >= 0) {
                    exportPoolFromRow(poolRow);
                } else if (picked == aScrub && canScrub && poolRow >= 0) {
                    scrubPoolFromRow(poolRow);
                } else if (picked == aDestroy && canDestroy && poolRow >= 0) {
                    destroyPoolFromRow(poolRow);
                }
                return;
            }

            QAction* aShowInline = menu.addAction(
                trk(QStringLiteral("t_show_inline_001"),
                    QStringLiteral("Mostrar propiedades en línea"),
                    QStringLiteral("Show inline properties"),
                    QStringLiteral("显示内联属性")));
            aShowInline->setCheckable(true);
            aShowInline->setChecked(m_showInlineDatasetProps);
            QAction* aEdit = menu.addAction(
                trk(QStringLiteral("t_edit_528f79"),
                    QStringLiteral("Editar"),
                    QStringLiteral("Edit"),
                    QStringLiteral("编辑")));
            menu.addSeparator();
            QAction* aRollback = menu.addAction(QStringLiteral("Rollback"));
            QAction* aCreate = menu.addAction(
                trk(QStringLiteral("t_create_ch_001"), QStringLiteral("Crear"), QStringLiteral("Create"), QStringLiteral("创建")));
            QAction* aDelete = menu.addAction(
                trk(QStringLiteral("t_delete_menu002"), QStringLiteral("Borrar"), QStringLiteral("Delete"), QStringLiteral("删除")));
            menu.addSeparator();
            QAction* aBreakdown = menu.addAction(
                trk(QStringLiteral("t_breakdown_btn1"), QStringLiteral("Desglosar"), QStringLiteral("Break down"), QStringLiteral("拆分")));
            QAction* aAssemble = menu.addAction(
                trk(QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("组装")));
            QAction* aFromDir = menu.addAction(
                trk(QStringLiteral("t_from_dir_btn1"), QStringLiteral("Desde Dir"), QStringLiteral("From Dir"), QStringLiteral("来自目录")));
            QAction* aToDir = menu.addAction(
                trk(QStringLiteral("t_to_dir_btn_001"), QStringLiteral("Hacia Dir"), QStringLiteral("To Dir"), QStringLiteral("到目录")));

            const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
            const bool hasConnSel = ctx.valid && !ctx.datasetName.isEmpty();
            const bool hasConnSnap = hasConnSel && !ctx.snapshotName.isEmpty();
            aEdit->setEnabled(!actionsLocked() && hasConnSel && !hasConnSnap);
            aRollback->setEnabled(!actionsLocked() && hasConnSnap);
            aCreate->setEnabled(!actionsLocked() && hasConnSel && !hasConnSnap);
            aDelete->setEnabled(!actionsLocked() && hasConnSel);
            aBreakdown->setEnabled(m_btnConnBreakdown && m_btnConnBreakdown->isEnabled());
            aAssemble->setEnabled(m_btnConnAssemble && m_btnConnAssemble->isEnabled());
            aFromDir->setEnabled(m_btnConnFromDir && m_btnConnFromDir->isEnabled());
            aToDir->setEnabled(m_btnConnToDir && m_btnConnToDir->isEnabled());

            QAction* picked = menu.exec(m_connContentTree->viewport()->mapToGlobal(pos));
            if (!picked) {
                return;
            }
            if (picked == aShowInline) {
                m_showInlineDatasetProps = aShowInline->isChecked();
                saveUiSettings();
                refreshInlinePropsVisual(m_connContentTree);
                refreshInlinePropsVisual(m_bottomConnContentTree);
                return;
            }
            if (picked == aEdit) {
                openEditDatasetDialog(m_connContentTree);
                return;
            }
            if (picked == aRollback) {
                const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (!actx.valid || actx.snapshotName.isEmpty()) {
                    return;
                }
                const QString snapObj = QStringLiteral("%1@%2").arg(actx.datasetName, actx.snapshotName);
                const auto confirm = QMessageBox::question(
                    this,
                    QStringLiteral("Rollback"),
                    QStringLiteral("¿Confirmar rollback de snapshot?\n%1").arg(snapObj),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (confirm != QMessageBox::Yes) {
                    return;
                }
                logUiAction(QStringLiteral("Rollback snapshot (menú Contenido)"));
                QString q = snapObj;
                q.replace('\'', "'\"'\"'");
                const QString cmd = QStringLiteral("zfs rollback '%1'").arg(q);
                executeDatasetAction(QStringLiteral("conncontent"), QStringLiteral("Rollback"), actx, cmd, 90000);
            } else if (picked == aCreate) {
                logUiAction(QStringLiteral("Crear hijo dataset (menú Contenido)"));
                actionCreateChildDataset(QStringLiteral("conncontent"));
            } else if (picked == aDelete) {
                logUiAction(QStringLiteral("Borrar dataset/snapshot (menú Contenido)"));
                actionDeleteDatasetOrSnapshot(QStringLiteral("conncontent"));
            } else if (picked == aBreakdown && m_btnConnBreakdown) {
                m_btnConnBreakdown->click();
            } else if (picked == aAssemble && m_btnConnAssemble) {
                m_btnConnAssemble->click();
            } else if (picked == aFromDir && m_btnConnFromDir) {
                m_btnConnFromDir->click();
            } else if (picked == aToDir && m_btnConnToDir) {
                m_btnConnToDir->click();
            }
        });
    }
    if (m_connContentPropsTable) {
        connect(m_connContentPropsTable, &QTableWidget::cellChanged, this, [this](int row, int col) {
            onDatasetPropsCellChanged(row, col);
        });
    }
    if (m_btnApplyConnContentProps) {
        connect(m_btnApplyConnContentProps, &QPushButton::clicked, this, [this]() {
            applyDatasetPropertyChanges();
        });
    }
    m_rightStack->setCurrentIndex(0);
    connect(m_poolStatusRefreshBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Actualizar estado de pool (botón)"));
        if (selectedPoolRowFromTabs() >= 0) {
            refreshSelectedPoolDetails(true, true);
        }
    });
    connect(m_poolStatusImportBtn, &QPushButton::clicked, this, [this]() {
        const int row = selectedPoolRowFromTabs();
        if (row < 0) return;
        logUiAction(QStringLiteral("Importar pool (botón Estado)"));
        importPoolFromRow(row);
    });
    connect(m_poolStatusExportBtn, &QPushButton::clicked, this, [this]() {
        const int row = selectedPoolRowFromTabs();
        if (row < 0) return;
        logUiAction(QStringLiteral("Exportar pool (botón Estado)"));
        exportPoolFromRow(row);
    });
    connect(m_poolStatusScrubBtn, &QPushButton::clicked, this, [this]() {
        const int row = selectedPoolRowFromTabs();
        if (row < 0) return;
        logUiAction(QStringLiteral("Scrub pool (botón Estado)"));
        scrubPoolFromRow(row);
    });
    connect(m_poolStatusDestroyBtn, &QPushButton::clicked, this, [this]() {
        const int row = selectedPoolRowFromTabs();
        if (row < 0) return;
        logUiAction(QStringLiteral("Destroy pool (botón Estado)"));
        destroyPoolFromRow(row);
    });
    // Datasets/Avanzado legacy pages are hidden in the current UI and intentionally
    // have no live signal wiring. Active workflow runs through "conncontent".
    connect(m_btnConnBreakdown, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnBreakdown) {
                logUiAction(QStringLiteral("Cancelar Desglosar (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnBreakdown;
        m_activeConnActionName = trk(QStringLiteral("t_breakdown_btn1"), QStringLiteral("Desglosar"),
                                     QStringLiteral("Break down"), QStringLiteral("拆分"));
        logUiAction(QStringLiteral("Desglosar (botón Conexiones)"));
        executeConnectionAdvancedAction(QStringLiteral("breakdown"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnAssemble, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnAssemble) {
                logUiAction(QStringLiteral("Cancelar Ensamblar (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnAssemble;
        m_activeConnActionName = trk(QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar"),
                                     QStringLiteral("Assemble"), QStringLiteral("组装"));
        logUiAction(QStringLiteral("Ensamblar (botón Conexiones)"));
        executeConnectionAdvancedAction(QStringLiteral("assemble"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnFromDir, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnFromDir) {
                logUiAction(QStringLiteral("Cancelar Desde Dir (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnFromDir;
        m_activeConnActionName = trk(QStringLiteral("t_from_dir_btn1"), QStringLiteral("Desde Dir"),
                                     QStringLiteral("From Dir"), QStringLiteral("来自目录"));
        logUiAction(QStringLiteral("Desde Dir (botón Conexiones)"));
        executeConnectionAdvancedAction(QStringLiteral("fromdir"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnToDir, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnToDir) {
                logUiAction(QStringLiteral("Cancelar Hacia Dir (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnToDir;
        m_activeConnActionName = trk(QStringLiteral("t_to_dir_btn_001"), QStringLiteral("Hacia Dir"),
                                     QStringLiteral("To Dir"), QStringLiteral("到目录"));
        logUiAction(QStringLiteral("Hacia Dir (botón Conexiones)"));
        executeConnectionAdvancedAction(QStringLiteral("todir"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnCopy, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnCopy) {
                logUiAction(QStringLiteral("Cancelar Copiar (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnCopy;
        m_activeConnActionName = trk(QStringLiteral("t_copy_001"), QStringLiteral("Copiar"),
                                     QStringLiteral("Copy"), QStringLiteral("复制"));
        logUiAction(QStringLiteral("Copiar snapshot (botón Conexiones)"));
        executeConnectionTransferAction(QStringLiteral("copy"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnLevel, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnLevel) {
                logUiAction(QStringLiteral("Cancelar Nivelar (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnLevel;
        m_activeConnActionName = trk(QStringLiteral("t_level_btn_001"), QStringLiteral("Nivelar"),
                                     QStringLiteral("Level"), QStringLiteral("同步快照"));
        logUiAction(QStringLiteral("Nivelar snapshot (botón Conexiones)"));
        executeConnectionTransferAction(QStringLiteral("level"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnSync, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnSync) {
                logUiAction(QStringLiteral("Cancelar Sincronizar (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnSync;
        m_activeConnActionName = trk(QStringLiteral("t_sync_btn_001"), QStringLiteral("Sincronizar"),
                                     QStringLiteral("Sync"), QStringLiteral("同步文件"));
        logUiAction(QStringLiteral("Sincronizar datasets (botón Conexiones)"));
        executeConnectionTransferAction(QStringLiteral("sync"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    updateConnectionActionsState();
}
