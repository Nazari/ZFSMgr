#include "mainwindow.h"

#include <algorithm>
#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.1.0"
#endif

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("ZFSMgr [%1]").arg(QStringLiteral(ZFSMGR_APP_VERSION)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
    resize(1200, 736);
    setMinimumSize(1120, 736);
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #f3f7fb; color: #14212b; }"
        "QTabWidget::pane { border: 1px solid #b8c7d6; border-radius: 6px; background: #f8fbff; margin-top: -1px; }"
        "QTabWidget::tab-bar { alignment: left; }"
        "QTabBar { background: #f3f7fb; }"
        "QTabBar::scroller { background: #f3f7fb; }"
        "QTabBar QToolButton { background: #f3f7fb; border: 1px solid #b8c7d6; color: #14212b; }"
        "QTabBar::tab { padding: 4px 12px; min-height: 20px; background: #e6edf4; border: 1px solid #b8c7d6; border-bottom: 1px solid #b8c7d6; border-top-left-radius: 6px; border-top-right-radius: 6px; }"
        "QTabBar::tab:selected { font-weight: 700; background: #f8fbff; color: #0b2f4f; border: 1px solid #6ea6dd; border-bottom: 1px solid #f8fbff; }"
        "QTabBar::tab:!selected { margin-top: 2px; background: #e6edf4; }"
        "QGroupBox { margin-top: 12px; border: 1px solid #b8c7d6; border-radius: 4px; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 8px; padding: 0 4px 0 4px; background: #f3f7fb; }"
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
        if (!act || !m_logLevelCombo) {
            return;
        }
        m_logLevelCombo->setCurrentText(act->data().toString());
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
        if (!act || !m_logMaxLinesCombo) {
            return;
        }
        m_logMaxLinesCombo->setCurrentText(QString::number(act->data().toInt()));
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
        {QStringLiteral("accion_ensamblar"), QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar"), QStringLiteral("Assemble"), QStringLiteral("合并")}
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
    const int leftFixedWidth = qMax(220, static_cast<int>(leftBaseWidth * 0.85 * 1.15 * 1.10 * 1.25 * 0.80 * 1.20));
    leftPane->setMinimumWidth(leftFixedWidth);
    leftPane->setMaximumWidth(leftFixedWidth);

    auto* connectionsTab = new QWidget(leftPane);
    auto* connLayout = new QVBoxLayout(connectionsTab);
    connLayout->setContentsMargins(2, 2, 2, 2);
    connLayout->setSpacing(2);
    const int stdLeftBtnH = 34;
    const int connBtnMinW = qMax(
        fm.horizontalAdvance(trk(QStringLiteral("t_refrescar__7f8af2"),
                                 QStringLiteral("Refrescar todo"),
                                 QStringLiteral("Refresh all"),
                                 QStringLiteral("全部刷新"))) + 28,
        fm.horizontalAdvance(trk(QStringLiteral("t_new_btn_001"),
                                 QStringLiteral("Nueva"),
                                 QStringLiteral("New"),
                                 QStringLiteral("新建"))) + 28);

    m_poolMgmtBox = new QGroupBox(
        trk(QStringLiteral("t_pool_mgmt_empty1"),
            QStringLiteral("Gestión de Pools de [vacío]"),
            QStringLiteral("Pool Management of [empty]"),
            QStringLiteral("[空] 的池管理")),
        connectionsTab);
    auto* poolMgmtLayout = new QVBoxLayout(m_poolMgmtBox);
    poolMgmtLayout->setContentsMargins(8, 20, 8, 8);
    auto* poolMgmtButtons = new QHBoxLayout();
    poolMgmtButtons->setSpacing(8);
    m_btnPoolNew =
        new QPushButton(trk(QStringLiteral("t_new_btn_002"),
                            QStringLiteral("Nuevo"),
                            QStringLiteral("New"),
                            QStringLiteral("新建")),
                        m_poolMgmtBox);
    m_btnPoolNew->setMinimumHeight(stdLeftBtnH);
    m_btnPoolNew->setMinimumWidth(connBtnMinW);
    m_btnPoolNew->setMaximumWidth(connBtnMinW);
    m_btnPoolNew->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnPoolNew->setEnabled(false);
    poolMgmtButtons->addWidget(m_btnPoolNew);
    poolMgmtButtons->addStretch(1);
    poolMgmtLayout->addLayout(poolMgmtButtons);
    connLayout->addWidget(m_poolMgmtBox, 0);
    m_poolMgmtBox->setVisible(false);

    auto* connListBox = new QGroupBox(
        trk(QStringLiteral("t_list_001"),
            QStringLiteral("Conexiones"),
            QStringLiteral("Connections"),
            QStringLiteral("连接")),
        connectionsTab);
    auto* connListBoxLayout = new QVBoxLayout(connListBox);
    connListBoxLayout->setContentsMargins(6, 8, 6, 6);
    m_connectionsList = new QTreeWidget(connListBox);
    m_connectionsList->setHeaderHidden(true);
    m_connectionsList->setColumnCount(1);
    m_connectionsList->setRootIsDecorated(true);
    m_connectionsList->setItemsExpandable(true);
    m_connectionsList->setExpandsOnDoubleClick(true);
    m_connectionsList->setAlternatingRowColors(true);
    m_connectionsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_connectionsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_connectionsList->setStyle(fusion);
    }
#endif
    connListBoxLayout->addWidget(m_connectionsList, 1);
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

    auto* connActionLeftBox = new QWidget(m_connActionsBox);
    auto* connActionLeftLayout = new QVBoxLayout(connActionLeftBox);
    connActionLeftLayout->setContentsMargins(6, 2, 6, 4);
    connActionLeftLayout->setSpacing(4);
    m_connLeftSelectionLabel = new QLabel(
        trk(QStringLiteral("t_empty_sel_001"),
            QStringLiteral("(vacío)"),
            QStringLiteral("(empty)"),
            QStringLiteral("（空）")),
        connActionLeftBox);
    m_connLeftSelectionLabel->setWordWrap(true);
    m_connLeftSelectionLabel->setMinimumHeight(34);
    connActionLeftLayout->addWidget(m_connLeftSelectionLabel);
    m_btnConnBreakdown = new QPushButton(
        trk(QStringLiteral("t_breakdown_btn1"),
            QStringLiteral("Desglosar"),
            QStringLiteral("Break down"),
            QStringLiteral("拆分")),
        connActionLeftBox);
    m_btnConnAssemble = new QPushButton(
        trk(QStringLiteral("t_assemble_btn1"),
            QStringLiteral("Ensamblar"),
            QStringLiteral("Assemble"),
            QStringLiteral("组装")),
        connActionLeftBox);
    m_btnConnFromDir = new QPushButton(
        trk(QStringLiteral("t_from_dir_btn1"),
            QStringLiteral("Desde Dir"),
            QStringLiteral("From Dir"),
            QStringLiteral("来自目录")),
        connActionLeftBox);
    m_btnConnToDir = new QPushButton(
        trk(QStringLiteral("t_to_dir_btn_001"),
            QStringLiteral("Hacia Dir"),
            QStringLiteral("To Dir"),
            QStringLiteral("到目录")),
        connActionLeftBox);
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
    auto* connLeftBtns = new QGridLayout();
    connLeftBtns->setContentsMargins(0, 0, 0, 0);
    connLeftBtns->setHorizontalSpacing(6);
    connLeftBtns->setVerticalSpacing(6);
    connLeftBtns->setColumnStretch(0, 1);
    connLeftBtns->setColumnStretch(1, 1);
    connLeftBtns->addWidget(m_btnConnBreakdown, 0, 0);
    connLeftBtns->addWidget(m_btnConnAssemble, 0, 1);
    connLeftBtns->addWidget(m_btnConnFromDir, 1, 0);
    connLeftBtns->addWidget(m_btnConnToDir, 1, 1);
    connActionLeftLayout->addLayout(connLeftBtns);

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
    m_connOriginSelectionLabel->setMinimumHeight(34);
    connActionRightLayout->addWidget(m_connOriginSelectionLabel);
    m_btnConnReset = new QPushButton(
        trk(QStringLiteral("t_reset_btn_001"),
            QStringLiteral("Reset"),
            QStringLiteral("Reset"),
            QStringLiteral("重置")),
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
    m_btnConnReset->setToolTip(
        trk(QStringLiteral("t_tt_reset_001"),
            QStringLiteral("Limpia la selección de Origen."),
            QStringLiteral("Clear the Source selection."),
            QStringLiteral("清空源端选择。")));
    m_btnConnReset->setMinimumHeight(stdLeftBtnH);
    m_btnConnCopy->setMinimumHeight(stdLeftBtnH);
    m_btnConnLevel->setMinimumHeight(stdLeftBtnH);
    m_btnConnSync->setMinimumHeight(stdLeftBtnH);
    m_btnConnReset->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnCopy->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnLevel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnSync->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* connRightBtns = new QGridLayout();
    connRightBtns->setContentsMargins(0, 0, 0, 0);
    connRightBtns->setHorizontalSpacing(6);
    connRightBtns->setVerticalSpacing(6);
    connRightBtns->setColumnStretch(0, 1);
    connRightBtns->setColumnStretch(1, 1);
    connRightBtns->addWidget(m_btnConnReset, 0, 0);
    connRightBtns->addWidget(m_btnConnSync, 0, 1);
    connRightBtns->addWidget(m_btnConnCopy, 1, 0);
    connRightBtns->addWidget(m_btnConnLevel, 1, 1);
    connActionRightLayout->addLayout(connRightBtns);
    connActionsLayout->addWidget(connActionRightBox, 1);
    connActionsLayout->addWidget(connActionLeftBox, 1);
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

    auto* advancedTab = new QWidget(m_leftTabs);
    auto* advLeftTabLayout = new QVBoxLayout(advancedTab);
    advLeftTabLayout->setContentsMargins(4, 4, 4, 4);
    advLeftTabLayout->setSpacing(4);
    m_advCommandsBox = new QGroupBox(trk(QStringLiteral("t_action_on_sel01"),
                                         QStringLiteral("Acción sobre [vacío]"),
                                         QStringLiteral("Action on [empty]"),
                                         QStringLiteral("对 [空] 的操作")),
                                     advancedTab);
    auto* commandsLayout = new QVBoxLayout(m_advCommandsBox);
    commandsLayout->setSpacing(10);
    m_btnAdvancedBreakdown = new QPushButton(trk(QStringLiteral("t_breakdown_btn1"),
                                                 QStringLiteral("Desglosar"),
                                                 QStringLiteral("Break down"),
                                                 QStringLiteral("拆分")),
                                             m_advCommandsBox);
    m_btnAdvancedAssemble = new QPushButton(trk(QStringLiteral("t_assemble_btn1"),
                                                QStringLiteral("Ensamblar"),
                                                QStringLiteral("Assemble"),
                                                QStringLiteral("组装")),
                                            m_advCommandsBox);
    m_btnAdvancedFromDir = new QPushButton(trk(QStringLiteral("t_from_dir_btn1"),
                                               QStringLiteral("Desde Dir"),
                                               QStringLiteral("From Dir"),
                                               QStringLiteral("来自目录")),
                                           m_advCommandsBox);
    m_btnAdvancedToDir = new QPushButton(trk(QStringLiteral("t_to_dir_btn_001"),
                                             QStringLiteral("Hacia Dir"),
                                             QStringLiteral("To Dir"),
                                             QStringLiteral("到目录")),
                                         m_advCommandsBox);
    m_btnAdvancedBreakdown->setToolTip(
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
    m_btnAdvancedAssemble->setToolTip(
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
    m_btnAdvancedFromDir->setToolTip(
        trk(QStringLiteral("t_tt_fromdir001"), QStringLiteral("Crea un dataset hijo usando un directorio local como mountpoint.\n"
                           "Requiere dataset seleccionado en Avanzado."),
            QStringLiteral("Create a child dataset using a local directory as mountpoint.\n"
                           "Requires a dataset selected in Advanced."),
            QStringLiteral("使用本地目录作为挂载点创建子数据集。\n"
                           "需要在高级页选择一个数据集。")));
    m_btnAdvancedToDir->setToolTip(
        trk(QStringLiteral("t_tt_todir_001"), QStringLiteral("Hace lo contrario de Desde Dir: copia el contenido del dataset a un directorio local\n"
                           "y elimina el dataset al finalizar correctamente."),
            QStringLiteral("Inverse of From Dir: copy dataset content to a local directory\n"
                           "and remove dataset when finished successfully."),
            QStringLiteral("与“来自目录”相反：将数据集内容复制到本地目录，\n"
                           "成功后删除该数据集。")));
    m_btnAdvancedBreakdown->setMinimumHeight(stdLeftBtnH);
    m_btnAdvancedAssemble->setMinimumHeight(stdLeftBtnH);
    m_btnAdvancedFromDir->setMinimumHeight(stdLeftBtnH);
    m_btnAdvancedToDir->setMinimumHeight(stdLeftBtnH);
    m_btnAdvancedBreakdown->setMinimumWidth(0);
    m_btnAdvancedAssemble->setMinimumWidth(0);
    m_btnAdvancedFromDir->setMinimumWidth(0);
    m_btnAdvancedToDir->setMinimumWidth(0);
    m_btnAdvancedBreakdown->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedAssemble->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedFromDir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedToDir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnAdvancedBreakdown->setEnabled(false);
    m_btnAdvancedAssemble->setEnabled(false);
    m_btnAdvancedFromDir->setEnabled(false);
    m_btnAdvancedToDir->setEnabled(false);
    auto* commandsButtonsGrid = new QGridLayout();
    commandsButtonsGrid->setContentsMargins(0, 0, 0, 0);
    commandsButtonsGrid->setHorizontalSpacing(6);
    commandsButtonsGrid->setVerticalSpacing(6);
    commandsButtonsGrid->setColumnStretch(0, 1);
    commandsButtonsGrid->setColumnStretch(1, 1);
    commandsButtonsGrid->addWidget(m_btnAdvancedBreakdown, 0, 0);
    commandsButtonsGrid->addWidget(m_btnAdvancedAssemble, 0, 1);
    commandsButtonsGrid->addWidget(m_btnAdvancedFromDir, 1, 0);
    commandsButtonsGrid->addWidget(m_btnAdvancedToDir, 1, 1);
    commandsLayout->addLayout(commandsButtonsGrid);
    // Igualar altura de las cajas de acciones de panel izquierdo:
    // Conexiones, Datasets (Origen-->Destino) y Avanzado (Comandos).
    const int actionsBoxHeight = qMax(130, m_connActionsBox ? m_connActionsBox->sizeHint().height() : 130);
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setFixedHeight(actionsBoxHeight);
    }
    if (m_connActionsBox) {
        m_connActionsBox->setMinimumHeight(actionsBoxHeight);
    }
    m_transferBox->setFixedHeight(actionsBoxHeight);
    m_advCommandsBox->setFixedHeight(actionsBoxHeight);

    auto* advancedInfoTabs = new QTabWidget(advancedTab);
    advancedInfoTabs->setDocumentMode(false);
    auto* mountedAdvTab = new QWidget(advancedInfoTabs);
    auto* mountedAdvLayout = new QVBoxLayout(mountedAdvTab);
    m_mountedDatasetsTableAdv = new QTableWidget(mountedAdvTab);
    m_mountedDatasetsTableAdv->setColumnCount(2);
    m_mountedDatasetsTableAdv->setHorizontalHeaderLabels(
        {trk(QStringLiteral("t_dataset_001"),
             QStringLiteral("Dataset"),
             QStringLiteral("Dataset"),
             QStringLiteral("数据集")),
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
    enableSortableHeader(m_mountedDatasetsTableAdv);
    mountedAdvLayout->addWidget(m_mountedDatasetsTableAdv, 1);
    auto* propsAdvTab = new QWidget(advancedInfoTabs);
    auto* propsAdvLayout = new QVBoxLayout(propsAdvTab);
    propsAdvLayout->setContentsMargins(0, 0, 0, 0);
    propsAdvLayout->setSpacing(4);
    m_advPropsTable = new QTableWidget(propsAdvTab);
    m_advPropsTable->setColumnCount(3);
    m_advPropsTable->setHorizontalHeaderLabels({QStringLiteral("Propiedad"), QStringLiteral("Valor"), QStringLiteral("Inherit")});
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_advPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_advPropsTable->horizontalHeader()->setStretchLastSection(false);
    m_advPropsTable->setColumnWidth(0, 200);
    m_advPropsTable->setColumnWidth(1, 210);
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_advPropsTable->setStyle(fusion);
    }
    m_advPropsTable->setStyleSheet(QStringLiteral(
        "QTableWidget::indicator:unchecked { border: 2px solid #1f4f76; background: #ffffff; border-radius: 3px; }"
        "QTableWidget::indicator:checked { border: 2px solid #1f4f76; background: #d9ecff; border-radius: 3px; }"));
    const int advInheritColWidth = qMax(36, static_cast<int>((m_advPropsTable->fontMetrics().horizontalAdvance(QStringLiteral("Inherit")) / 2 + 12) * 1.10));
    m_advPropsTable->setColumnWidth(2, advInheritColWidth);
#else
    m_advPropsTable->setColumnWidth(2, 90);
#endif
    m_advPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_advPropsTable->verticalHeader()->setVisible(false);
    m_advPropsTable->verticalHeader()->setDefaultSectionSize(22);
    m_advPropsTable->setProperty("pinned_rows", 5);
    m_advPropsTable->setFont(m_mountedDatasetsTableAdv->font());
    {
        QFont hf = m_advPropsTable->font();
        hf.setBold(false);
        m_advPropsTable->horizontalHeader()->setFont(hf);
    }
    m_btnApplyAdvancedProps = new QPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        propsAdvTab);
    m_btnApplyAdvancedProps->setEnabled(false);
    enableSortableHeader(m_advPropsTable);
    propsAdvLayout->addWidget(m_advPropsTable, 1);
    propsAdvLayout->addWidget(m_btnApplyAdvancedProps, 0, Qt::AlignRight);
    advancedInfoTabs->addTab(
        propsAdvTab,
        trk(QStringLiteral("t_props_tab_001"),
            QStringLiteral("Propiedades"),
            QStringLiteral("Properties"),
            QStringLiteral("属性")));
    advancedInfoTabs->addTab(
        mountedAdvTab,
        trk(QStringLiteral("t_mounted_tab_001"),
            QStringLiteral("Montados"),
            QStringLiteral("Mounted"),
            QStringLiteral("已挂载")));
    advLeftTabLayout->setSpacing(8);
    advLeftTabLayout->addWidget(m_advCommandsBox);
    advLeftTabLayout->addWidget(advancedInfoTabs, 1);
    advancedTab->setLayout(advLeftTabLayout);

    m_leftTabs->addTab(datasetsTab, trk(QStringLiteral("t_datasets_tab_001"),
                                        QStringLiteral("Datasets"),
                                        QStringLiteral("Datasets"),
                                        QStringLiteral("数据集")));
    m_leftTabs->addTab(advancedTab, trk(QStringLiteral("t_advanced_tab_001"),
                                        QStringLiteral("Avanzado"),
                                        QStringLiteral("Advanced"),
                                        QStringLiteral("高级")));
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

    // Backend-only pool table (hidden). Data/actions are driven from connection tree nodes.
    m_importedPoolsTable = new QTableWidget(rightConnectionsPage);
    m_importedPoolsTable->setColumnCount(5);
    m_importedPoolsTable->setHorizontalHeaderLabels(
        {trk(QStringLiteral("t_conexi_n_d70cf0"),
             QStringLiteral("Conexión"),
             QStringLiteral("Connection"),
             QStringLiteral("连接")),
         trk(QStringLiteral("t_pool_col_001"),
             QStringLiteral("Pool"),
             QStringLiteral("Pool"),
             QStringLiteral("池")),
         trk(QStringLiteral("t_status_col_001"),
             QStringLiteral("Estado"),
             QStringLiteral("Status"),
             QStringLiteral("状态")),
         trk(QStringLiteral("t_imported_col01"),
             QStringLiteral("Importado"),
             QStringLiteral("Imported"),
             QStringLiteral("已导入")),
         trk(QStringLiteral("t_reason_col_001"),
             QStringLiteral("Motivo"),
             QStringLiteral("Reason"),
             QStringLiteral("原因"))});
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_importedPoolsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_importedPoolsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_importedPoolsTable->setContextMenuPolicy(Qt::CustomContextMenu);
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
    enableSortableHeader(m_importedPoolsTable);
    m_importedPoolsTable->setVisible(false);

    m_poolDetailTabs = new QWidget(rightConnectionsPage);
    auto* poolDetailLayout = new QVBoxLayout(m_poolDetailTabs);
    poolDetailLayout->setContentsMargins(0, 0, 0, 0);
    poolDetailLayout->setSpacing(4);
    m_connPropsGroup = new QGroupBox(trk(QStringLiteral("t_pool_props001"),
                                         QStringLiteral("Propiedades"),
                                         QStringLiteral("Properties"),
                                         QStringLiteral("属性")),
                                     m_poolDetailTabs);
    auto* propsPoolLayout = new QVBoxLayout(m_connPropsGroup);
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
    poolPropsPageLayout->addWidget(m_poolPropsTable, 1);
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
    poolPropsPageLayout->addLayout(connPropBtns);

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
    m_connContentTree->header()->setStretchLastSection(false);
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
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_connContentTree->setStyle(fusion);
    }
#endif
    connContentLayout->addWidget(m_connContentTree, 1);
    m_connPropsStack->addWidget(m_connContentPage);
    m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
    propsPoolLayout->addWidget(m_connPropsStack, 1);

    m_connBottomGroup = new QGroupBox(trk(QStringLiteral("t_status_col_001"),
                                          QStringLiteral("Estado"),
                                          QStringLiteral("Status"),
                                          QStringLiteral("状态")),
                                      m_poolDetailTabs);
    auto* statusPoolLayout = new QVBoxLayout(m_connBottomGroup);
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
    const int connInheritColWidth = qMax(36, static_cast<int>((m_connContentPropsTable->fontMetrics().horizontalAdvance(QStringLiteral("Inherit")) / 2 + 12) * 1.10));
    m_connContentPropsTable->setColumnWidth(2, connInheritColWidth);
    m_connContentPropsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    m_connContentPropsTable->verticalHeader()->setVisible(false);
    m_connContentPropsTable->verticalHeader()->setDefaultSectionSize(22);
    enableSortableHeader(m_connContentPropsTable);
    connDsPropsLayout->addWidget(m_connContentPropsTable, 1);
    m_btnApplyConnContentProps = new QPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        m_connDatasetPropsPage);
    m_btnApplyConnContentProps->setEnabled(false);
    connDsPropsLayout->addWidget(m_btnApplyConnContentProps, 0, Qt::AlignLeft);
    m_connBottomStack->addWidget(m_connDatasetPropsPage);
    m_connBottomStack->setCurrentWidget(m_connStatusPage);
    statusPoolLayout->addWidget(m_connBottomStack, 1);
    auto* connDetailSplit = new QSplitter(Qt::Vertical, rightConnectionsPage);
    connDetailSplit->setChildrenCollapsible(false);
    connDetailSplit->setHandleWidth(1);
    connDetailSplit->addWidget(m_connPropsGroup);
    connDetailSplit->addWidget(m_connBottomGroup);
    connDetailSplit->setStretchFactor(0, 55);
    connDetailSplit->setStretchFactor(1, 45);
    poolDetailLayout->addWidget(connDetailSplit, 1);
    rightConnectionsLayout->setSpacing(0);
    rightConnectionsLayout->addWidget(m_poolDetailTabs, 1);

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
    m_advTree->setColumnCount(4);
    m_advTree->setHeaderLabels({trk(QStringLiteral("t_dataset_001"),
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
    m_advTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_advTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_advTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_advTree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_advTree->header()->setStretchLastSection(false);
    m_advTree->setColumnWidth(0, 250);
    m_advTree->setColumnWidth(1, 90);
    m_advTree->setColumnWidth(2, 72);
    m_advTree->setColumnWidth(3, 180);
    m_advTree->setUniformRowHeights(true);
    m_advTree->setRootIsDecorated(true);
    m_advTree->setItemsExpandable(true);
    {
        QFont f = m_advTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_advTree->setFont(f);
    }
    m_advTree->setStyleSheet(QStringLiteral("QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_advTree->setStyle(fusion);
    }
#endif
    m_advSelectionLabel = new QLabel(trk(QStringLiteral("t_no_sel_001"),
                                         QStringLiteral("(sin selección)"),
                                         QStringLiteral("(no selection)"),
                                         QStringLiteral("（未选择）")),
                                     rightAdvancedPage);
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
        const QFontMetrics fm(m_statusText->font());
        const int h = fm.lineSpacing() * 2 + 10;
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
    m_logsTabs = new QTabWidget(rightLogs);
    m_logsTabs->setDocumentMode(false);
    m_logsTabs->setStyleSheet(
        QStringLiteral("QTabWidget::tab-bar { alignment: left; }"
                       "QTabWidget::pane { margin-top: -1px; }"
                       "QTabBar::tab { padding: 2px 8px; min-height: 16px; }"
                       "QTabBar::tab:!selected { margin-top: 1px; }"));
    auto* appTab = new QWidget(m_logsTabs);
    auto* appTabLayout = new QVBoxLayout(appTab);
    m_logView = new QPlainTextEdit(appTab);
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_logView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(8);
    m_logView->setFont(mono);
    appTabLayout->addWidget(m_logView, 1);
    m_logsTabs->addTab(appTab, trk(QStringLiteral("t_app_tab_001"),
                                   QStringLiteral("Aplicación"),
                                   QStringLiteral("Application"),
                                   QStringLiteral("应用")));

    auto* controlsPane = new QWidget(rightLogs);
    controlsPane->setVisible(false);
    m_logLevelCombo = new QComboBox(rightLogs);
    m_logLevelCombo->addItems({QStringLiteral("normal"), QStringLiteral("info"), QStringLiteral("debug")});
    m_logLevelCombo->setCurrentText(m_logLevelSetting);
    m_logMaxLinesCombo = new QComboBox(rightLogs);
    m_logMaxLinesCombo->addItems({QStringLiteral("100"), QStringLiteral("200"), QStringLiteral("500"), QStringLiteral("1000")});
    m_logMaxLinesCombo->setCurrentText(QString::number(m_logMaxLinesSetting));
    m_logClearBtn = new QPushButton(trk(QStringLiteral("t_clear_001"),
                                        QStringLiteral("Limpiar"),
                                        QStringLiteral("Clear"),
                                        QStringLiteral("清空")),
                                    rightLogs);
    m_logCopyBtn = new QPushButton(trk(QStringLiteral("t_copy_001"),
                                       QStringLiteral("Copiar"),
                                       QStringLiteral("Copy"),
                                       QStringLiteral("复制")),
                                   rightLogs);
    m_logLevelCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_logMaxLinesCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_logClearBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_logCopyBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    {
        QFont cf = m_logLevelCombo->font();
        cf.setPointSize(qMax(6, cf.pointSize() - 1));
        m_logLevelCombo->setFont(cf);
        m_logMaxLinesCombo->setFont(cf);
        m_logClearBtn->setFont(cf);
        m_logCopyBtn->setFont(cf);
    }
    int ctrlW = 56;
    m_logLevelCombo->setFixedWidth(ctrlW);
    m_logMaxLinesCombo->setFixedWidth(ctrlW);
    m_logClearBtn->setFixedWidth(ctrlW);
    m_logCopyBtn->setFixedWidth(ctrlW);
    m_logLevelCombo->hide();
    m_logMaxLinesCombo->hide();
    m_logClearBtn->hide();
    m_logCopyBtn->hide();
    rightLogsBody->addWidget(controlsPane, 0);
    rightLogsBody->addWidget(m_logsTabs, 1);
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

    connect(m_btnPoolNew, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Nuevo pool (botón)"));
        createPoolForSelectedConnection();
    });
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
    connect(m_connectionsList, &QTreeWidget::itemSelectionChanged, this, [this]() { onConnectionSelectionChanged(); });
    m_connectionsList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_connectionsList, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onConnectionListContextMenuRequested(pos);
    });
    if (m_connContentTree) {
        connect(m_connContentTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
            refreshDatasetProperties(QStringLiteral("conncontent"));
            updateConnectionDetailTitlesForCurrentSelection();
            updateConnectionActionsState();
        });
        connect(m_connContentTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
            Q_UNUSED(item);
            Q_UNUSED(col);
        });
        m_connContentTree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_connContentTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            showDatasetContextMenu(QStringLiteral("conncontent"), m_connContentTree, pos);
        });
        connect(m_connContentTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
            onDatasetTreeItemChanged(m_connContentTree, item, col, QStringLiteral("conncontent"));
            updateConnectionActionsState();
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
    connect(m_importedPoolsTable, &QTableWidget::cellClicked, this, [this](int row, int col) {
        Q_UNUSED(row);
        Q_UNUSED(col);
        onPoolsSelectionChanged();
    });
    connect(m_importedPoolsTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        onPoolsSelectionChanged();
    });
    connect(m_importedPoolsTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        onPoolsListContextMenuRequested(pos);
    });
    connect(m_poolStatusRefreshBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Actualizar estado de pool (botón)"));
        if (m_importedPoolsTable && !m_importedPoolsTable->selectedItems().isEmpty()) {
            refreshSelectedPoolDetails();
        }
    });
    connect(m_poolStatusImportBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Importar pool (botón Estado)"));
        importPoolFromRow(sel.first()->row());
    });
    connect(m_poolStatusExportBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Exportar pool (botón Estado)"));
        exportPoolFromRow(sel.first()->row());
    });
    connect(m_poolStatusScrubBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Scrub pool (botón Estado)"));
        scrubPoolFromRow(sel.first()->row());
    });
    connect(m_poolStatusDestroyBtn, &QPushButton::clicked, this, [this]() {
        if (!m_importedPoolsTable) {
            return;
        }
        const auto sel = m_importedPoolsTable->selectedItems();
        if (sel.isEmpty()) {
            return;
        }
        logUiAction(QStringLiteral("Destroy pool (botón Estado)"));
        destroyPoolFromRow(sel.first()->row());
    });
    connect(m_originPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onOriginPoolChanged(); });
    connect(m_destPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onDestPoolChanged(); });
    connect(m_advPoolCombo, &QComboBox::currentIndexChanged, this, [this]() { onAdvancedPoolChanged(); });
    connect(m_originTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onOriginTreeSelectionChanged(); });
    connect(m_destTree, &QTreeWidget::itemSelectionChanged, this, [this]() { onDestTreeSelectionChanged(); });
    connect(m_originTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        onDatasetTreeItemChanged(m_originTree, item, col, QStringLiteral("origin"));
    });
    connect(m_destTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        onDatasetTreeItemChanged(m_destTree, item, col, QStringLiteral("dest"));
    });
    connect(m_advTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        onDatasetTreeItemChanged(m_advTree, item, col, QStringLiteral("advanced"));
    });
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
    connect(m_btnConnReset, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            return;
        }
        logUiAction(QStringLiteral("Reset origen (botón Conexiones)"));
        setConnectionOriginSelection(DatasetSelectionContext{});
        setConnectionDestinationSelection(DatasetSelectionContext{});
        refreshTransferSelectionLabels();
        updateConnectionActionsState();
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
        saveUiSettings();
    });
    connect(m_logLevelCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        saveUiSettings();
    });
    connect(m_advTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        const auto selected = m_advTree->selectedItems();
        if (selected.isEmpty()) {
            updateAdvancedSelectionUi(QString(), QString());
            refreshDatasetProperties(QStringLiteral("advanced"));
            updateTransferButtonsState();
            return;
        }
        auto* it = selected.first();
        const QString ds = it->data(0, Qt::UserRole).toString();
        const QString snap = it->data(1, Qt::UserRole).toString();
        updateAdvancedSelectionUi(ds, snap);
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
    connect(m_btnAdvancedFromDir, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Desde Dir (botón)"));
        actionAdvancedCreateFromDir();
    });
    connect(m_btnAdvancedToDir, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Hacia Dir (botón)"));
        actionAdvancedToDir();
    });
    updateConnectionActionsState();
}
