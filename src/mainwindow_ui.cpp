#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <algorithm>
#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QResizeEvent>
#include <QStyleFactory>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextBlock>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QPainter>

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.9.9rc1"
#endif

namespace {
constexpr int kIsPoolRootRole = Qt::UserRole + 12;
constexpr int kConnPropRowRole = Qt::UserRole + 13;
constexpr int kConnPropRowKindRole = Qt::UserRole + 16; // 1=name, 2=value
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kConnPropGroupNodeRole = Qt::UserRole + 17;
constexpr int kConnPropGroupNameRole = Qt::UserRole + 18;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kConnSnapshotHoldsNodeRole = Qt::UserRole + 21;
constexpr int kConnSnapshotHoldItemRole = Qt::UserRole + 22;
constexpr int kConnSnapshotHoldTagRole = Qt::UserRole + 23;
constexpr int kConnSnapshotHoldTimestampRole = Qt::UserRole + 24;
constexpr int kConnPermissionsNodeRole = Qt::UserRole + 25;
constexpr int kConnPermissionsKindRole = Qt::UserRole + 26;
constexpr int kConnPermissionsScopeRole = Qt::UserRole + 27;
constexpr int kConnPermissionsTargetTypeRole = Qt::UserRole + 28;
constexpr int kConnPermissionsTargetNameRole = Qt::UserRole + 29;
constexpr int kConnPermissionsEntryNameRole = Qt::UserRole + 30;
constexpr int kConnPermissionsPendingRole = Qt::UserRole + 31;
constexpr int kConnInlineCellUsedRole = Qt::UserRole + 32;
constexpr char kPoolBlockInfoKey[] = "__pool_block_info__";

QString pendingChangeLastQuotedArg(const QString& text) {
    const int lastQuote = text.lastIndexOf(QLatin1Char('\''));
    if (lastQuote <= 0) {
        return QString();
    }
    const int prevQuote = text.lastIndexOf(QLatin1Char('\''), lastQuote - 1);
    if (prevQuote < 0 || prevQuote >= lastQuote) {
        return QString();
    }
    return text.mid(prevQuote + 1, lastQuote - prevQuote - 1).trimmed();
}

class ConnContentPropBorderDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (!painter || !index.isValid() || index.column() < 4) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        const QColor vBorder = option.palette.color(QPalette::Mid).darker(118);
        const QColor hBorder = option.palette.color(QPalette::Mid).darker(108);
        const QRect r = option.rect;
        const bool isPropRow = index.sibling(index.row(), 0).data(kConnPropRowRole).toBool();
        if (isPropRow) {
            const int kind = index.sibling(index.row(), 0).data(kConnPropRowKindRole).toInt();
            const bool used = index.data(kConnInlineCellUsedRole).toBool();
            if (kind == 1 || kind == 2) {
                if (!used) {
                    painter->restore();
                    return;
                }
                painter->fillRect(QRect(r.left(), r.top(), 1, r.height()), vBorder);
                painter->fillRect(QRect(r.right(), r.top(), 1, r.height()), vBorder);
                if (kind == 1) {
                    painter->fillRect(QRect(r.left(), r.top(), r.width(), 1), hBorder);
                } else {
                    painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), hBorder);
                }
            }
            painter->restore();
            return;
        }

        if (index.row() > 0) {
            const QModelIndex prev = index.sibling(index.row() - 1, 0);
            if (prev.isValid()
                && prev.data(kConnPropRowRole).toBool()
                && prev.data(kConnPropRowKindRole).toInt() == 2
                && prev.sibling(prev.row(), index.column()).data(kConnInlineCellUsedRole).toBool()) {
                painter->fillRect(QRect(r.left(), r.top(), r.width(), 1), hBorder);
            }
        }
        painter->restore();
    }
};

class TooltipPushButton final : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    void enterEvent(QEnterEvent* event) override {
        QPushButton::enterEvent(event);
        const QString text = toolTip().trimmed();
        if (!text.isEmpty()) {
            QToolTip::showText(mapToGlobal(rect().bottomLeft()), text, this, rect());
        }
    }

    void leaveEvent(QEvent* event) override {
        QPushButton::leaveEvent(event);
        QToolTip::hideText();
    }
};

class CenteredCheckDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!painter || !index.isValid()
            || !(index.flags() & Qt::ItemIsUserCheckable)) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem viewOpt(option);
        initStyleOption(&viewOpt, index);
        const QWidget* widget = viewOpt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();

        // Draw base item without text; this column is check-only.
        const QString savedText = viewOpt.text;
        viewOpt.text.clear();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &viewOpt, painter, widget);
        viewOpt.text = savedText;

        QStyleOptionButton cbOpt;
        cbOpt.state = QStyle::State_None;
        if (index.flags() & Qt::ItemIsEnabled) {
            cbOpt.state |= QStyle::State_Enabled;
        }
        cbOpt.state |= (index.data(Qt::CheckStateRole).toInt() == Qt::Checked)
                           ? QStyle::State_On
                           : QStyle::State_Off;

        const QRect indicator = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &viewOpt, widget);
        const QPoint centeredPos(
            viewOpt.rect.x() + (viewOpt.rect.width() - indicator.width()) / 2,
            viewOpt.rect.y() + (viewOpt.rect.height() - indicator.height()) / 2);
        cbOpt.rect = QRect(centeredPos, indicator.size());

        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &cbOpt, painter, widget);

        if (option.state & QStyle::State_HasFocus) {
            QStyleOptionFocusRect focusOpt;
            focusOpt.QStyleOption::operator=(option);
            focusOpt.rect = option.rect.adjusted(1, 1, -1, -1);
            focusOpt.state |= QStyle::State_KeyboardFocusChange;
            focusOpt.backgroundColor = option.palette.color(QPalette::Base);
            style->drawPrimitive(QStyle::PE_FrameFocusRect, &focusOpt, painter, widget);
        }
    }

    bool editorEvent(QEvent* event,
                     QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        Q_UNUSED(option);
        if (!event || !model || !index.isValid() || !(index.flags() & Qt::ItemIsUserCheckable)
            || !(index.flags() & Qt::ItemIsEnabled)) {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        const auto toggleDeferred = [&]() {
            const QPersistentModelIndex pidx(index);
            QPointer<QAbstractItemModel> modelPtr(model);
            QTimer::singleShot(0, [pidx, modelPtr]() {
                if (!modelPtr || !pidx.isValid()) {
                    return;
                }
                const Qt::CheckState cur = static_cast<Qt::CheckState>(pidx.data(Qt::CheckStateRole).toInt());
                const Qt::CheckState next = (cur == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
                modelPtr->setData(pidx, next, Qt::CheckStateRole);
            });
            return true;
        };

        switch (event->type()) {
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && option.rect.contains(me->position().toPoint())) {
                return toggleDeferred();
            }
            break;
        }
        case QEvent::KeyPress: {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Space || ke->key() == Qt::Key_Select) {
                return toggleDeferred();
            }
            break;
        }
        default:
            break;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};

class ManagePropsListWidget final : public QListWidget {
public:
    explicit ManagePropsListWidget(QWidget* parent = nullptr)
        : QListWidget(parent) {}

    void setManagedColumnCount(int cols) {
        m_managedColumnCount = qMax(1, cols);
        updateManagedGrid();
    }

    void setPinnedCount(int count) {
        m_pinnedCount = qMax(0, count);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QListWidget::resizeEvent(event);
        updateManagedGrid();
    }

    void adoptCheckStateFromNeighbors(QListWidgetItem* item) {
        if (!item) {
            return;
        }
        const int idx = row(item);
        if (idx < 0 || idx < m_pinnedCount) {
            return;
        }
        bool shouldCheck = false;
        if (idx > 0) {
            if (QListWidgetItem* prev = this->item(idx - 1); prev && prev->checkState() == Qt::Checked) {
                shouldCheck = true;
            }
        }
        if (!shouldCheck && idx + 1 < count()) {
            if (QListWidgetItem* next = this->item(idx + 1); next && next->checkState() == Qt::Checked) {
                shouldCheck = true;
            }
        }
        if (shouldCheck && item->checkState() != Qt::Checked) {
            item->setCheckState(Qt::Checked);
        }
    }

    void paintEvent(QPaintEvent* event) override {
        QListWidget::paintEvent(event);
        if (m_indicatorRow < 0 || m_indicatorRow >= count()) {
            return;
        }
        QListWidgetItem* item = this->item(m_indicatorRow);
        if (!item) {
            return;
        }
        const QRect rect = visualItemRect(item).adjusted(1, 1, -1, -1);
        if (!rect.isValid()) {
            return;
        }
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(rect, QColor(220, 38, 38, 90));
        QPen pen(QColor(185, 28, 28));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.drawRect(rect);
    }

    int insertionRowForPos(const QPoint& pos) const {
        int to = count();
        if (QListWidgetItem* target = itemAt(pos)) {
            to = row(target);
            const QRect rect = visualItemRect(target);
            const bool afterTarget =
                (pos.y() > rect.center().y())
                || (qAbs(pos.y() - rect.center().y()) <= rect.height() / 3
                    && pos.x() > rect.center().x());
            if (afterTarget) {
                ++to;
            }
        }
        return qBound(m_pinnedCount, to, count());
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event && event->source() == this) {
            m_dragItem = currentItem();
            if (m_dragItem && row(m_dragItem) < m_pinnedCount) {
                m_dragItem = nullptr;
                event->ignore();
                return;
            }
            m_indicatorRow = m_dragItem ? row(m_dragItem) : -1;
            viewport()->update();
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        QListWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event && event->source() == this && m_dragItem) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint pos = event->position().toPoint();
#else
            const QPoint pos = event->pos();
#endif
            const int from = row(m_dragItem);
            int to = insertionRowForPos(pos);
            if (from >= 0) {
                if (to > from) {
                    --to;
                }
                to = qBound(0, to, count());
                if (to != from) {
                    QListWidgetItem* moved = takeItem(from);
                    if (moved) {
                        insertItem(to, moved);
                        m_dragItem = moved;
                        adoptCheckStateFromNeighbors(m_dragItem);
                        setCurrentItem(m_dragItem);
                        scrollToItem(m_dragItem);
                    }
                }
                m_indicatorRow = row(m_dragItem);
                viewport()->update();
            }
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        QListWidget::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        m_indicatorRow = -1;
        viewport()->update();
        QListWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        if (!event || event->source() != this) {
            m_dragItem = nullptr;
            m_indicatorRow = -1;
            viewport()->update();
            QListWidget::dropEvent(event);
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
        m_dragItem = nullptr;
        m_indicatorRow = -1;
        viewport()->update();
    }

private:
    void updateManagedGrid() {
        const int cols = qMax(1, m_managedColumnCount);
        const int spacing = this->spacing();
        const int viewportWidth = qMax(320, viewport()->width());
        const int cellWidth = qMax(120, (viewportWidth - ((cols - 1) * spacing)) / cols);
        setGridSize(QSize(cellWidth, 30));
        setIconSize(QSize(0, 0));
    }

    QListWidgetItem* m_dragItem{nullptr};
    int m_indicatorRow{-1};
    int m_managedColumnCount{1};
    int m_pinnedCount{0};
};
}

void MainWindow::activatePendingChangeAtCursor() {
    if (!m_pendingChangesView || !m_pendingChangesView->hasFocus() || m_pendingChangeActivationInProgress) {
        return;
    }
    const QScopedValueRollback<bool> guard(m_pendingChangeActivationInProgress, true);
    const QString line = m_pendingChangesView->textCursor().block().text().trimmed();
    if (line.isEmpty()) {
        return;
    }
    focusPendingChangeLine(line);
}

bool MainWindow::focusPendingChangeLine(const QString& line) {
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const int cmdSep = trimmed.indexOf(QStringLiteral("  "));
    if (cmdSep <= 0) {
        return false;
    }
    const QString prefix = trimmed.left(cmdSep).trimmed();
    const QString cmd = trimmed.mid(cmdSep + 2).trimmed();
    const int prefixSep = prefix.lastIndexOf(QStringLiteral("::"));
    if (prefixSep <= 0) {
        return false;
    }
    const QString connLabel = prefix.left(prefixSep).trimmed();
    const QString poolName = prefix.mid(prefixSep + 2).trimmed();
    if (connLabel.isEmpty() || poolName.isEmpty() || cmd.isEmpty()) {
        return false;
    }

    int connIdx = -1;
    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile& p = m_profiles.at(i);
        if (p.name.compare(connLabel, Qt::CaseInsensitive) == 0
            || p.id.compare(connLabel, Qt::CaseInsensitive) == 0) {
            connIdx = i;
            break;
        }
    }
    if (connIdx < 0) {
        return false;
    }

    auto visiblePoolRoot = [&](QTreeWidget* tree) -> QTreeWidgetItem* {
        if (!tree) {
            return nullptr;
        }
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (!item || !item->data(0, kIsPoolRootRole).toBool()) {
                continue;
            }
            if (item->data(0, kConnIdxRole).toInt() == connIdx
                && item->data(0, kPoolNameRole).toString().trimmed().compare(poolName, Qt::CaseInsensitive) == 0) {
                return item;
            }
        }
        return nullptr;
    };

    QTreeWidgetItem* originPoolRoot = visiblePoolRoot(m_connContentTree);
    QTreeWidgetItem* destPoolRoot = visiblePoolRoot(m_bottomConnContentTree);
    QTreeWidget* targetTree = nullptr;
    // Si el pool está visible en ambos árboles, priorizar siempre Origen.
    if (originPoolRoot) {
        targetTree = m_connContentTree;
    } else if (destPoolRoot) {
        targetTree = m_bottomConnContentTree;
    }
    if (!targetTree) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_pending_focus_pool_not_visible_001"),
                QStringLiteral("Debes seleccionar en Origen o Destino la conexión/pool %1::%2 para poder situar el foco."),
                QStringLiteral("You must select connection/pool %1::%2 in Source or Target before focusing this change."),
                QStringLiteral("必须先在来源或目标中选择连接/存储池 %1::%2，才能定位到此更改。"))
                .arg(connLabel, poolName));
        return false;
    }

    const QString datasetName = pendingChangeLastQuotedArg(cmd);
    if (datasetName.isEmpty()) {
        return false;
    }
    auto findDatasetNode = [&](QTreeWidgetItem* node, auto&& self) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed().compare(datasetName, Qt::CaseInsensitive) == 0) {
            return node;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (QTreeWidgetItem* found = self(node->child(i), self)) {
                return found;
            }
        }
        return nullptr;
    };
    QTreeWidgetItem* datasetItem = nullptr;
    for (int i = 0; i < targetTree->topLevelItemCount() && !datasetItem; ++i) {
        datasetItem = findDatasetNode(targetTree->topLevelItem(i), findDatasetNode);
    }
    if (!datasetItem) {
        return false;
    }
    const QString prevToken = m_connContentToken;
    QTreeWidget* const prevTree = m_connContentTree;
    m_connContentTree = targetTree;
    m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);

    QSignalBlocker treeBlocker(targetTree);
    std::unique_ptr<QSignalBlocker> selectionBlocker;
    if (targetTree->selectionModel()) {
        selectionBlocker = std::make_unique<QSignalBlocker>(targetTree->selectionModel());
    }
    for (QTreeWidgetItem* parent = datasetItem->parent(); parent; parent = parent->parent()) {
        parent->setExpanded(true);
    }
    targetTree->setCurrentItem(datasetItem);
    refreshDatasetProperties(QStringLiteral("conncontent"));
    syncConnContentPropertyColumns();
    datasetItem->setExpanded(true);
    targetTree->scrollToItem(datasetItem, QAbstractItemView::PositionAtCenter);

    auto findChild = [](QTreeWidgetItem* parent, auto&& pred) -> QTreeWidgetItem* {
        if (!parent) {
            return nullptr;
        }
        for (int i = 0; i < parent->childCount(); ++i) {
            QTreeWidgetItem* child = parent->child(i);
            if (child && pred(child)) {
                return child;
            }
        }
        return nullptr;
    };
    auto findPropRow = [&](QTreeWidgetItem* parent, const QString& propName, auto&& self) -> QTreeWidgetItem* {
        if (!parent) {
            return nullptr;
        }
        for (int i = 0; i < parent->childCount(); ++i) {
            QTreeWidgetItem* child = parent->child(i);
            if (!child) {
                continue;
            }
            for (int col = 0; col < targetTree->columnCount(); ++col) {
                if (child->text(col).trimmed().compare(propName, Qt::CaseInsensitive) == 0) {
                    return child;
                }
            }
            if (QTreeWidgetItem* nested = self(child, propName, self)) {
                return nested;
            }
        }
        return nullptr;
    };

    if (cmd.startsWith(QStringLiteral("zfs allow "), Qt::CaseInsensitive)
        || cmd.startsWith(QStringLiteral("zfs unallow "), Qt::CaseInsensitive)) {
        if (QTreeWidgetItem* permissionsNode = findChild(datasetItem, [](QTreeWidgetItem* child) {
                return child->data(0, kConnPermissionsNodeRole).toBool()
                       && child->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root");
            })) {
            if (permissionsNode->childCount() == 0) {
                populateDatasetPermissionsNode(targetTree, datasetItem, false);
            }
            permissionsNode->setExpanded(true);
            targetTree->setCurrentItem(permissionsNode);
            targetTree->scrollToItem(permissionsNode, QAbstractItemView::PositionAtCenter);
        }
        m_connContentTree = prevTree;
        m_connContentToken = prevToken;
        targetTree->setFocus();
        return true;
    }

    QString propName;
    if (cmd.startsWith(QStringLiteral("zfs set "), Qt::CaseInsensitive)) {
        const QString prefixCmd = cmd.left(cmd.lastIndexOf(QLatin1Char('\'')));
        const QString firstArg = pendingChangeLastQuotedArg(prefixCmd);
        const int eq = firstArg.indexOf(QLatin1Char('='));
        if (eq > 0) {
            propName = firstArg.left(eq).trimmed();
        }
    } else if (cmd.startsWith(QStringLiteral("zfs inherit "), Qt::CaseInsensitive)) {
        const QRegularExpression rx(QStringLiteral("^zfs\\s+inherit\\s+'([^']+)'"),
                                    QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = rx.match(cmd);
        if (m.hasMatch()) {
            propName = m.captured(1).trimmed();
        }
    } else if (cmd.startsWith(QStringLiteral("zfs mount "), Qt::CaseInsensitive)
               || cmd.startsWith(QStringLiteral("zfs umount "), Qt::CaseInsensitive)
               || cmd.startsWith(QStringLiteral("zfs unmount "), Qt::CaseInsensitive)) {
        propName = QStringLiteral("Montado");
    }

    if (QTreeWidgetItem* propsNode = findChild(datasetItem, [this](QTreeWidgetItem* child) {
            return child->data(0, kConnPropGroupNodeRole).toBool()
                   && child->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                   && child->text(0).trimmed() == trk(QStringLiteral("t_props_lbl_001"),
                                                      QStringLiteral("Propiedades"),
                                                      QStringLiteral("Properties"),
                                                      QStringLiteral("属性"));
        })) {
        propsNode->setExpanded(true);
        if (!propName.isEmpty()) {
            if (QTreeWidgetItem* propRow = findPropRow(propsNode, propName, findPropRow)) {
                targetTree->setCurrentItem(propRow);
                targetTree->scrollToItem(propRow, QAbstractItemView::PositionAtCenter);
            } else {
                targetTree->setCurrentItem(propsNode);
                targetTree->scrollToItem(propsNode, QAbstractItemView::PositionAtCenter);
            }
        } else {
            targetTree->setCurrentItem(propsNode);
            targetTree->scrollToItem(propsNode, QAbstractItemView::PositionAtCenter);
        }
    }

    m_connContentTree = prevTree;
    m_connContentToken = prevToken;
    targetTree->setFocus();
    return true;
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
        "#zfsmgrLogTabs QTabBar::tab { padding: 1px 8px; min-height: 14px; }"
        "#zfsmgrLogTabs QTabBar::tab:selected { margin-bottom: -1px; }"
        "#zfsmgrDetailContainer { border: 0px; background: transparent; margin-top: 0px; }"
        "#zfsmgrDetailContainer > QWidget { border: 0px; background: transparent; }"
        "#zfsmgrDetailContainer QTabBar { background: transparent; }"
        "#zfsmgrSubtabContentFrame { border: 0px; background: transparent; margin-top: 0px; }"));
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

    auto applyPropColumnsSetting = [this](int cols) {
        const int bounded = qBound(5, cols, 10);
        if (bounded == m_connPropColumnsSetting) {
            return;
        }
        m_connPropColumnsSetting = bounded;
        saveUiSettings();
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Columnas de propiedades: %1").arg(m_connPropColumnsSetting));

        auto refreshOneConnContentTree = [this](QTreeWidget* tree) {
            if (!tree) {
                return;
            }
            const bool isBottomTree = (tree == m_bottomConnContentTree);
            const int connIdx = isBottomTree ? m_bottomDetailConnIdx : m_topDetailConnIdx;
            if (isBottomTree) {
                saveBottomTreeStateForConnection(connIdx);
            } else {
                saveTopTreeStateForConnection(connIdx);
            }
            QTreeWidget* prevTree = m_connContentTree;
            m_connContentTree = tree;
            QTreeWidgetItem* cur = tree->currentItem();
            QTreeWidgetItem* owner = cur;
            while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
                   && !owner->data(0, kIsPoolRootRole).toBool()) {
                owner = owner->parent();
            }
            const bool poolMode = owner && owner->data(0, kIsPoolRootRole).toBool();
            if (poolMode) {
                syncConnContentPoolColumns();
            } else {
                syncConnContentPropertyColumns();
            }
            m_connContentTree = prevTree;
            if (isBottomTree) {
                restoreBottomTreeStateForConnection(connIdx);
            } else {
                restoreTopTreeStateForConnection(connIdx);
            }
        };
        refreshOneConnContentTree(m_connContentTree);
        refreshOneConnContentTree(m_bottomConnContentTree);
    };

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
            QStringLiteral("Ayuda")));
    QAction* quickManualAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_quick_001"),
            QStringLiteral("Manual rápido")));
    connect(quickManualAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("manual_rapido"),
                      trk(QStringLiteral("t_help_quick_001"),
                          QStringLiteral("Manual rápido")));
    });

    QMenu* actionsHelpMenu = helpMenu->addMenu(
        trk(QStringLiteral("t_help_actions_001"),
            QStringLiteral("Acciones")));
    struct HelpTopicItem {
        QString id;
        QString key;
        QString es;
    };
    const QVector<HelpTopicItem> helpActions = {
        {QStringLiteral("accion_copiar"), QStringLiteral("t_copy_001"), QStringLiteral("Copiar")},
        {QStringLiteral("accion_clonar"), QStringLiteral("t_clone_btn_001"), QStringLiteral("Clonar")},
        {QStringLiteral("accion_diff"), QStringLiteral("t_diff_btn_001"), QStringLiteral("Diff")},
        {QStringLiteral("accion_sincronizar"), QStringLiteral("t_sync_btn_001"), QStringLiteral("Sincronizar")},
        {QStringLiteral("accion_nivelar"), QStringLiteral("t_level_btn_001"), QStringLiteral("Nivelar")},
        {QStringLiteral("accion_desglosar"), QStringLiteral("t_breakdown_btn1"), QStringLiteral("Desglosar")},
        {QStringLiteral("accion_ensamblar"), QStringLiteral("t_assemble_btn1"), QStringLiteral("Ensamblar")},
        {QStringLiteral("accion_desde_dir"), QStringLiteral("t_from_dir_btn1"), QStringLiteral("Desde Dir")},
        {QStringLiteral("accion_hacia_dir"), QStringLiteral("t_to_dir_btn_001"), QStringLiteral("Hacia Dir")}
    };
    for (const HelpTopicItem& item : helpActions) {
        QAction* act = actionsHelpMenu->addAction(trk(item.key, item.es));
        connect(act, &QAction::triggered, this, [this, item]() {
            openHelpTopic(item.id, trk(item.key, item.es));
        });
    }

    QAction* ctxMenusAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_ctx_001"),
            QStringLiteral("Menús contextuales")));
    connect(ctxMenusAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("menus_contextuales"),
                      trk(QStringLiteral("t_help_ctx_001"),
                          QStringLiteral("Menús contextuales")));
    });

    QAction* navigationAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_short_001"),
            QStringLiteral("Navegación y estados")));
    connect(navigationAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("atajos_estados"),
                      trk(QStringLiteral("t_help_short_001"),
                          QStringLiteral("Navegación y estados")));
    });

    QAction* inlinePropsAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_inline_props_001"),
            QStringLiteral("Propiedades inline y columnas")));
    connect(inlinePropsAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("propiedades_inline_columnas"),
                      trk(QStringLiteral("t_help_inline_props_001"),
                          QStringLiteral("Propiedades inline y columnas")));
    });

    QAction* windowsConnAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_windows_conn_001"),
            QStringLiteral("Conexiones Windows")));
    connect(windowsConnAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("conexiones_windows"),
                      trk(QStringLiteral("t_help_windows_conn_001"),
                          QStringLiteral("Conexiones Windows")));
    });

    QAction* appLogHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_applog_001"),
            QStringLiteral("Logs de aplicación")));
    connect(appLogHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("logs_aplicacion"),
                      trk(QStringLiteral("t_help_applog_001"),
                          QStringLiteral("Logs de aplicación")));
    });

    QAction* cfgFilesHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_cfg_001"),
            QStringLiteral("Configuración, columnas y archivos INI")));
    connect(cfgFilesHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("configuracion_archivos"),
                      trk(QStringLiteral("t_help_cfg_001"),
                          QStringLiteral("Configuración, columnas y archivos INI")));
    });

    QAction* aboutAct = helpMenu->addAction(
        trk(QStringLiteral("t_about_001"),
            QStringLiteral("Acerca de")));
    connect(aboutAct, &QAction::triggered, this, [this]() {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_about_msg_001"),
                QStringLiteral("ZFSMgr\nGestor ZFS multiplataforma.\nAutor: Eladio Linares\nLicencia: GNU")));
    });

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 2, 8, 8);
    root->setSpacing(6);

    auto* topArea = new QWidget(central);
    auto* topLayout = new QVBoxLayout(topArea);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);
    auto* topSplit = new QSplitter(Qt::Horizontal, topArea);
    topSplit->setChildrenCollapsible(true);
    topSplit->setHandleWidth(4);

    auto* leftPane = new QWidget(topSplit);
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
    const int leftFixedWidth = qMax(220, static_cast<int>(leftBaseWidth * 0.69 * 1.08));
    leftPane->setMinimumWidth(0);

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
    m_connectionsTable->setColumnCount(2);
    m_connectionsTable->setHorizontalHeaderLabels({
        trk(QStringLiteral("t_conn_col_show_01"),
            QStringLiteral("Mostrar"),
            QStringLiteral("Show"),
            QStringLiteral("显示")),
        trk(QStringLiteral("t_connections_001"),
            QStringLiteral("Conexión"),
            QStringLiteral("Connection"),
            QStringLiteral("连接"))
    });
    m_connectionsTable->horizontalHeader()->setVisible(true);
    m_connectionsTable->verticalHeader()->setVisible(false);
    m_connectionsTable->setAlternatingRowColors(false);
    m_connectionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_connectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_connectionsTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_connectionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_connectionsTable->setColumnWidth(0, 96);
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
    m_btnApplyConnContentProps = new TooltipPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        connActionRightBox);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    m_btnConnCopy = new QPushButton(
        trk(QStringLiteral("t_copy_001"),
            QStringLiteral("Copiar"),
            QStringLiteral("Copy"),
            QStringLiteral("复制")),
        connActionRightBox);
    m_btnConnClone = new QPushButton(
        trk(QStringLiteral("t_clone_btn_001"),
            QStringLiteral("Clonar")),
        connActionRightBox);
    m_btnConnDiff = new QPushButton(
        trk(QStringLiteral("t_diff_btn_001"),
            QStringLiteral("Diff")),
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
    m_btnConnClone->setToolTip(
        trk(QStringLiteral("t_tt_clone_001"),
            QStringLiteral("Clona un snapshot sobre un dataset destino en la misma conexión y el mismo pool.\n"
                           "Requiere: snapshot seleccionado en Origen y dataset seleccionado en Destino.")));
    m_btnConnDiff->setToolTip(
        trk(QStringLiteral("t_tt_diff_001"),
            QStringLiteral("Compara un snapshot de Origen con su dataset padre actual o con otro snapshot del mismo dataset,\n"
                           "si ambos están en la misma conexión y el mismo pool.")));
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
                           "Requiere: dataset seleccionado (no snapshot) en Origen y Destino, y ambos datasets montados."),
            QStringLiteral("Sync dataset contents from Source to Target with rsync.\n"
                           "Requires: dataset selected (not snapshot) in Source and Target, and both datasets mounted."),
            QStringLiteral("使用 rsync 同步源端到目标端的数据集内容。\n"
                           "条件：源端和目标端都选择数据集（非快照），且两端数据集均已挂载。")));
    m_btnApplyConnContentProps->setMinimumHeight(stdLeftBtnH);
    m_btnConnCopy->setMinimumHeight(stdLeftBtnH);
    m_btnConnClone->setMinimumHeight(stdLeftBtnH);
    m_btnConnDiff->setMinimumHeight(stdLeftBtnH);
    m_btnConnLevel->setMinimumHeight(stdLeftBtnH);
    m_btnConnSync->setMinimumHeight(stdLeftBtnH);
    m_btnApplyConnContentProps->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnConnCopy->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnClone->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnDiff->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnLevel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnSync->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* connRightBtns = new QGridLayout();
    connRightBtns->setContentsMargins(0, 0, 0, 0);
    connRightBtns->setHorizontalSpacing(6);
    connRightBtns->setVerticalSpacing(6);
    connRightBtns->setColumnStretch(0, 1);
    connRightBtns->setColumnStretch(1, 1);
    connRightBtns->addWidget(m_btnConnSync, 0, 0);
    connRightBtns->addWidget(m_btnConnCopy, 0, 1);
    connRightBtns->addWidget(m_btnConnClone, 1, 0);
    connRightBtns->addWidget(m_btnConnLevel, 1, 1);
    connRightBtns->addWidget(m_btnConnDiff, 2, 0, 1, 2);
    connActionRightLayout->addLayout(connRightBtns);
    connActionsLayout->addWidget(connActionRightBox, 1);
    connLayout->addWidget(m_connActionsBox, 0);
    connectionsTab->setLayout(connLayout);

    // Legacy left "Datasets" tab removed from UI.
    // Legacy "advanced" layer removed from visible UI.
    const int actionsBoxHeight = qMax(130, m_connActionsBox ? m_connActionsBox->sizeHint().height() : 130);
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setFixedHeight(actionsBoxHeight);
    }
    if (m_connActionsBox) {
        m_connActionsBox->setMinimumHeight(actionsBoxHeight);
    }
    m_btnAdvancedBreakdown = nullptr;
    m_btnAdvancedAssemble = nullptr;
    m_btnAdvancedFromDir = nullptr;
    m_btnAdvancedToDir = nullptr;

    m_leftTabs->hide();
    leftLayout->addWidget(connectionsTab, 1);

    auto* rightPane = new QWidget(topSplit);
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
    m_connContentTree->setHeaderLabels({QStringLiteral("Origen:")
                                            + trk(QStringLiteral("t_dataset_001"),
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
    m_connContentTree->setColumnHidden(1, true);
    m_connContentTree->setColumnHidden(2, true);
    m_connContentTree->setColumnHidden(3, true);
    m_connContentTree->setUniformRowHeights(false);
    m_connContentTree->setRootIsDecorated(true);
    m_connContentTree->setItemsExpandable(true);
    m_connContentTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_connContentTree->verticalScrollBar()->setSingleStep(12);
    {
        QFont f = m_connContentTree->font();
        f.setPointSize(qMax(6, f.pointSize() - 1));
        m_connContentTree->setFont(f);
    }
    m_connContentTree->setStyleSheet(QStringLiteral(
        "QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"
        "QTreeWidget::indicator { width: 8px; height: 8px; margin: 2px; }"));
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

    m_rightStack->addWidget(rightConnectionsPage);
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
    m_bottomConnContentTree->setHeaderLabels({QStringLiteral("Destino:")
                                                   + trk(QStringLiteral("t_dataset_001"),
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
    m_bottomConnContentTree->header()->setStretchLastSection(false);
    m_bottomConnContentTree->setColumnWidth(0, 230);
    m_bottomConnContentTree->setColumnWidth(1, 90);
    m_bottomConnContentTree->setColumnWidth(2, 72);
    m_bottomConnContentTree->setColumnWidth(3, 170);
    m_bottomConnContentTree->setColumnHidden(1, true);
    m_bottomConnContentTree->setColumnHidden(2, true);
    m_bottomConnContentTree->setColumnHidden(3, true);
    m_bottomConnContentTree->setUniformRowHeights(false);
    m_bottomConnContentTree->setRootIsDecorated(true);
    m_bottomConnContentTree->setItemsExpandable(true);
    m_bottomConnContentTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_bottomConnContentTree->verticalScrollBar()->setSingleStep(12);
    m_bottomConnContentTree->setStyleSheet(QStringLiteral(
        "QTreeWidget::item { height: 22px; padding: 0px; margin: 0px; }"
        "QTreeWidget::indicator { width: 8px; height: 8px; margin: 2px; }"));
    m_bottomConnContentTree->setItemDelegate(new ConnContentPropBorderDelegate(m_bottomConnContentTree));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_bottomConnContentTree->setStyle(fusion);
    }
#endif
    // Mantener esquema de columnas idéntico en ambos árboles (superior/inferior)
    // incluso cuando uno de ellos esté vacío.
    {
        QTreeWidget* prevTree = m_connContentTree;
        m_connContentTree = prevTree;
        syncConnContentPropertyColumns();
        m_connContentTree = m_bottomConnContentTree;
        syncConnContentPropertyColumns();
        m_connContentTree = prevTree;
    }
    auto installTreeHeaderContextMenu = [this, applyPropColumnsSetting](QTreeWidget* tree) {
        if (!tree || !tree->header()) {
            return;
        }
        QHeaderView* header = tree->header();
        header->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(header, &QWidget::customContextMenuRequested, this, [this, tree, header, applyPropColumnsSetting](const QPoint& pos) {
            if (!tree || !header) {
                return;
            }
            const int logicalIndex = header->logicalIndexAt(pos);
            if (logicalIndex < 0) {
                return;
            }
            QMenu menu(this);
            QAction* aResizeThis = menu.addAction(
                trk(QStringLiteral("t_ctx_resize_col_001"),
                    QStringLiteral("Ajustar tamaño de esta columna")));
            QAction* aResizeAll = menu.addAction(
                trk(QStringLiteral("t_ctx_resize_allcol_001"),
                    QStringLiteral("Ajustar tamaño de todas las columnas")));
            menu.addSeparator();
            QMenu* propColsMenu = menu.addMenu(
                trk(QStringLiteral("t_prop_cols_menu001"),
                    QStringLiteral("Columnas de propiedades")));
            auto* propColsGroup = new QActionGroup(&menu);
            propColsGroup->setExclusive(true);
            for (int cols = 5; cols <= 10; ++cols) {
                QAction* act = propColsMenu->addAction(QString::number(cols));
                act->setCheckable(true);
                act->setData(cols);
                if (cols == qBound(5, m_connPropColumnsSetting, 10)) {
                    act->setChecked(true);
                }
                propColsGroup->addAction(act);
            }
            QAction* picked = menu.exec(header->mapToGlobal(pos));
            if (!picked) {
                return;
            }
            auto resizeOne = [tree](int col) {
                if (!tree || col < 0 || col >= tree->columnCount() || tree->isColumnHidden(col)) {
                    return;
                }
                tree->resizeColumnToContents(col);
            };
            if (picked == aResizeThis) {
                resizeOne(logicalIndex);
            } else if (picked == aResizeAll) {
                for (int col = 0; col < tree->columnCount(); ++col) {
                    resizeOne(col);
                }
            } else if (propColsGroup->actions().contains(picked)) {
                bool ok = false;
                const int cols = picked->data().toInt(&ok);
                if (ok) {
                    applyPropColumnsSetting(cols);
                }
            }
        });
    };
    installTreeHeaderContextMenu(m_connContentTree);
    installTreeHeaderContextMenu(m_bottomConnContentTree);
    auto* bottomConnOverlayHost = new QWidget(bottomConnBox);
    auto* bottomConnOverlayLayout = new QStackedLayout(bottomConnOverlayHost);
    bottomConnOverlayLayout->setStackingMode(QStackedLayout::StackAll);
    bottomConnOverlayLayout->setContentsMargins(0, 0, 0, 0);
    bottomConnOverlayLayout->addWidget(m_bottomConnContentTree);
    auto* bottomConnOverlay = new QWidget(bottomConnOverlayHost);
    bottomConnOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    bottomConnOverlay->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* bottomConnOverlayButtons = new QVBoxLayout(bottomConnOverlay);
    bottomConnOverlayButtons->setContentsMargins(0, 0, 10, 10);
    bottomConnOverlayButtons->addStretch(1);
    auto* bottomConnOverlayRow = new QHBoxLayout();
    bottomConnOverlayRow->setContentsMargins(0, 0, 0, 0);
    bottomConnOverlayRow->addStretch(1);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    bottomConnOverlayRow->addWidget(m_btnApplyConnContentProps, 0, Qt::AlignRight | Qt::AlignBottom);
    bottomConnOverlayButtons->addLayout(bottomConnOverlayRow);
    bottomConnOverlayLayout->addWidget(bottomConnOverlay);
    bottomConnOverlay->show();
    bottomConnOverlay->raise();
    m_btnApplyConnContentProps->show();
    m_btnApplyConnContentProps->raise();
    bottomConnLayout->addWidget(bottomConnOverlayHost, 1);
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

    topSplit->addWidget(leftPane);
    topSplit->addWidget(rightPane);
    topSplit->setStretchFactor(0, 0);
    topSplit->setStretchFactor(1, 1);
    topSplit->setSizes({leftFixedWidth, qMax(leftFixedWidth * 2, width() - leftFixedWidth)});
    topLayout->addWidget(topSplit, 1);

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
    auto* appTabs = new QTabWidget(rightLogs);
    appTabs->setObjectName(QStringLiteral("zfsmgrLogTabs"));
    auto* appLogTab = new QWidget(appTabs);
    auto* appLogLayout = new QVBoxLayout(appLogTab);
    appLogLayout->setContentsMargins(6, 6, 6, 6);
    appLogLayout->setSpacing(4);
    m_logView = new QPlainTextEdit(appLogTab);
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_logView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(8);
    m_logView->setFont(mono);
    appLogLayout->addWidget(m_logView, 1);
    appTabs->addTab(appLogTab,
                    trk(QStringLiteral("t_app_tab_001"),
                        QStringLiteral("Aplicación"),
                        QStringLiteral("Application"),
                        QStringLiteral("应用")));

    auto* pendingChangesTab = new QWidget(appTabs);
    auto* pendingChangesLayout = new QVBoxLayout(pendingChangesTab);
    pendingChangesLayout->setContentsMargins(6, 6, 6, 6);
    pendingChangesLayout->setSpacing(4);
    m_pendingChangesView = new QPlainTextEdit(pendingChangesTab);
    m_pendingChangesView->setReadOnly(true);
    m_pendingChangesView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_pendingChangesView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pendingChangesView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pendingChangesView->setFont(mono);
    connect(m_pendingChangesView, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        activatePendingChangeAtCursor();
    });
    pendingChangesLayout->addWidget(m_pendingChangesView, 1);
    appTabs->addTab(pendingChangesTab,
                    trk(QStringLiteral("t_pending_changes_tab001"),
                        QStringLiteral("Cambios pendientes"),
                        QStringLiteral("Pending changes"),
                        QStringLiteral("待处理更改")));

    loadPersistedAppLogToView();
    rightLogsBody->addWidget(appTabs, 1);
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
        if (col == 0) {
            return;
        }
        QTableWidgetItem* it = m_connectionsTable->item(row, 1);
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
    auto rebuildInlineConnTree = [this](QTreeWidget* tree, const QString& token) {
        if (!tree || token.trimmed().isEmpty()) {
            return;
        }
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep <= 0) {
            return;
        }
        bool ok = false;
        const int connIdx = token.left(sep).toInt(&ok);
        const QString poolName = token.mid(sep + 2).trimmed();
        if (!ok || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
            return;
        }
        const QString prevToken = m_connContentToken;
        QTreeWidget* prevTree = m_connContentTree;
        m_connContentTree = tree;
        m_connContentToken = token;
        saveConnContentTreeState(token);
        populateDatasetTree(tree, connIdx, poolName, QStringLiteral("conncontent"), true);
        restoreConnContentTreeState(token);
        m_connContentTree = prevTree;
        m_connContentToken = prevToken;
    };
    auto applyInlineSectionVisibility = [this,
                                         rebuildInlineConnTree,
                                         refreshInlinePropsVisualBottom]() {
        auto tokenFromTree = [this](QTreeWidget* tree) -> QString {
            if (!tree) {
                return QString();
            }
            QTreeWidgetItem* owner = tree->currentItem();
            if (!owner && tree->topLevelItemCount() > 0) {
                owner = tree->topLevelItem(0);
            }
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
        saveUiSettings();
        if (m_connContentTree) {
            const QString token = tokenFromTree(m_connContentTree);
            if (!token.isEmpty()) {
                rebuildInlineConnTree(m_connContentTree, token);
                const QString prevToken = m_connContentToken;
                QTreeWidget* prevTree = m_connContentTree;
                m_connContentToken = token;
                QTreeWidgetItem* sel = m_connContentTree->currentItem();
                auto isInfoNodeOrInside = [](QTreeWidgetItem* n) -> bool {
                    for (QTreeWidgetItem* p = n; p; p = p->parent()) {
                        if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                            return true;
                        }
                    }
                    return false;
                };
                const bool isPoolContext =
                    sel && (sel->data(0, kIsPoolRootRole).toBool() || isInfoNodeOrInside(sel));
                if (isPoolContext) {
                    syncConnContentPoolColumns();
                } else {
                    syncConnContentPropertyColumns();
                }
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
            }
        }
        if (m_bottomConnContentTree) {
            const QString token = tokenFromTree(m_bottomConnContentTree);
            if (!token.isEmpty()) {
                rebuildInlineConnTree(m_bottomConnContentTree, token);
                refreshInlinePropsVisualBottom(m_bottomConnContentTree);
            }
        }
    };
    auto manageInlinePropsVisualization = [this](QTreeWidget* tree, QTreeWidgetItem* rawItem, bool poolContext) {
        if (!tree || !rawItem) {
            return;
        }
        QTreeWidgetItem* item = rawItem;
        if (item->data(0, kConnPropRowRole).toBool() && item->parent()) {
            item = item->parent();
        }
        const QString clickedGroupName = item->data(0, kConnPropGroupNameRole).toString().trimmed();
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
        const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
        auto normalizeList = [](const QStringList& in) {
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
        auto displayedPropsFromNode = [tree, normalizeList](QTreeWidgetItem* parentNode) {
            QStringList out;
            if (!parentNode) {
                return out;
            }
            for (int i = 0; i < parentNode->childCount(); ++i) {
                QTreeWidgetItem* row = parentNode->child(i);
                if (!row || !row->data(0, kConnPropRowRole).toBool()
                    || row->data(0, kConnPropRowKindRole).toInt() != 1) {
                    continue;
                }
                for (int col = 4; col < tree->columnCount(); ++col) {
                    QString key = row->data(col, kConnPropKeyRole).toString().trimmed();
                    if (key.isEmpty()) {
                        key = row->text(col).trimmed();
                    }
                    if (!key.isEmpty()) {
                        out.push_back(key);
                    }
                }
            }
            return normalizeList(out);
        };

        enum class ManagePropsScope {
            Pool,
            Dataset,
            Snapshot,
        };
        QStringList allProps;
        QStringList currentVisible;
        QVector<InlinePropGroupConfig> currentGroups;
        ManagePropsScope scope = poolContext ? ManagePropsScope::Pool : ManagePropsScope::Dataset;
        const QString fixedSnapshotProp = QStringLiteral("snapshot");
        if (poolContext) {
            currentGroups = m_poolInlinePropGroups;
            const QString cacheKey = poolDetailsCacheKey(connIdx, poolName);
            const auto pit = m_poolDetailsCache.constFind(cacheKey);
            if (pit != m_poolDetailsCache.cend() && pit->loaded) {
                for (const QStringList& row : pit->propsRows) {
                    if (row.isEmpty()) {
                        continue;
                    }
                    const QString prop = row[0].trimmed();
                    if (prop.isEmpty() || prop.startsWith(QStringLiteral("feature@"), Qt::CaseInsensitive)) {
                        continue;
                    }
                    allProps.push_back(prop);
                }
            }
            allProps = normalizeList(allProps);
            QTreeWidgetItem* infoNode = item;
            while (infoNode && infoNode->data(0, kConnPropKeyRole).toString() != QString::fromLatin1(kPoolBlockInfoKey)) {
                infoNode = infoNode->parent();
            }
            currentVisible = displayedPropsFromNode(infoNode);
            if (!m_poolInlinePropsOrder.isEmpty()) {
                currentVisible.clear();
                for (const QString& p : m_poolInlinePropsOrder) {
                    for (const QString& have : allProps) {
                        if (p.compare(have, Qt::CaseInsensitive) == 0) {
                            currentVisible.push_back(have);
                            break;
                        }
                    }
                }
            }
        } else {
            const QString ds = owner->data(0, Qt::UserRole).toString().trimmed();
            if (ds.isEmpty()) {
                return;
            }
            const QString snap = owner->data(1, Qt::UserRole).toString().trimmed();
            if (!snap.isEmpty()) {
                scope = ManagePropsScope::Snapshot;
                currentGroups = m_snapshotInlinePropGroups;
            } else {
                currentGroups = m_datasetInlinePropGroups;
            }
            const QString objectName = snap.isEmpty() ? ds : QStringLiteral("%1@%2").arg(ds, snap);
            const QString key = QStringLiteral("%1|%2").arg(token.trimmed().toLower(),
                                                            objectName.trimmed().toLower());
            const auto vit = m_connContentPropValuesByObject.constFind(key);
            if (vit != m_connContentPropValuesByObject.cend()) {
                allProps = vit.value().keys();
            }
            allProps = normalizeList(allProps);
            currentVisible = displayedPropsFromNode(owner);
            const QStringList& savedOrder =
                (scope == ManagePropsScope::Snapshot) ? m_snapshotInlinePropsOrder : m_datasetInlinePropsOrder;
            if (!savedOrder.isEmpty()) {
                currentVisible.clear();
                for (const QString& p : savedOrder) {
                    for (const QString& have : allProps) {
                        if (p.compare(have, Qt::CaseInsensitive) == 0) {
                            currentVisible.push_back(have);
                            break;
                        }
                    }
                }
            }
            if (allProps.isEmpty()) {
                allProps = currentVisible;
            }
        }
        allProps = normalizeList(allProps);
        currentVisible = normalizeList(currentVisible);
        if (allProps.isEmpty()) {
            return;
        }
        bool snapshotFixedPresent = false;
        if (scope == ManagePropsScope::Snapshot) {
            QString canonicalSnapshot;
            for (const QString& p : allProps) {
                if (p.compare(fixedSnapshotProp, Qt::CaseInsensitive) == 0) {
                    canonicalSnapshot = p;
                    break;
                }
            }
            snapshotFixedPresent = !canonicalSnapshot.isEmpty();
            if (snapshotFixedPresent) {
                allProps.removeAll(canonicalSnapshot);
                allProps.prepend(canonicalSnapshot);
                currentVisible.removeAll(canonicalSnapshot);
                currentVisible.prepend(canonicalSnapshot);
                for (InlinePropGroupConfig& cfg : currentGroups) {
                    cfg.props.removeAll(canonicalSnapshot);
                }
            }
        }
        if (currentVisible.isEmpty()) {
            currentVisible = allProps;
        }

        QStringList orderedAll = currentVisible;
        for (const QString& p : allProps) {
            bool exists = false;
            for (const QString& q : orderedAll) {
                if (q.compare(p, Qt::CaseInsensitive) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                orderedAll.push_back(p);
            }
        }

        const int propCols = qBound(5, m_connPropColumnsSetting, 10);
        const int managePropsCellWidth = 120;
        const int managePropsSpacing = 6;
        const int managePropsDialogWidth =
            140 + (propCols * managePropsCellWidth) + ((propCols - 1) * managePropsSpacing);
        QDialog dlg(this);
        dlg.setModal(true);
        dlg.resize(managePropsDialogWidth, 520);
        dlg.setMinimumWidth(managePropsDialogWidth);
        dlg.setWindowTitle(
            trk(QStringLiteral("t_manage_props_vis001"),
                QStringLiteral("Gestionar visualización de propiedades")));
        auto* root = new QVBoxLayout(&dlg);
        auto* hint = new QLabel(
            trk(QStringLiteral("t_manage_props_hint001"),
                QStringLiteral("Marca qué propiedades quieres mostrar y reordénalas arrastrando y soltando.")),
            &dlg);
        hint->setWordWrap(true);
        root->addWidget(hint);
        auto* tabs = new QTabWidget(&dlg);
        root->addWidget(tabs, 1);
        struct ManageGroupTab {
            QString name;
            ManagePropsListWidget* list{nullptr};
            bool isMain{false};
        };
        QVector<ManageGroupTab> groupTabs;
        auto configureList = [this, propCols, managePropsSpacing, scope, snapshotFixedPresent, fixedSnapshotProp](ManagePropsListWidget* list,
                                                                                                                    const QStringList& orderedProps,
                                                                                                                    const QStringList& visible,
                                                                                                                    bool isMain) {
            if (!list) {
                return;
            }
            list->setSelectionMode(QAbstractItemView::SingleSelection);
            list->setViewMode(QListView::IconMode);
            list->setFlow(QListView::LeftToRight);
            list->setWrapping(true);
            list->setResizeMode(QListView::Adjust);
            list->setMovement(QListView::Static);
            list->setAcceptDrops(true);
            list->setDragEnabled(true);
            list->viewport()->setAcceptDrops(true);
            list->setDropIndicatorShown(true);
            list->setDragDropOverwriteMode(false);
            list->setDragDropMode(QAbstractItemView::InternalMove);
            list->setDefaultDropAction(Qt::CopyAction);
            list->setSpacing(managePropsSpacing);
            list->setUniformItemSizes(true);
            list->setManagedColumnCount(propCols);
            list->setPinnedCount((scope == ManagePropsScope::Snapshot && snapshotFixedPresent && isMain) ? 1 : 0);
            list->setMinimumHeight(220);
            for (const QString& p : orderedProps) {
                auto* it = new QListWidgetItem(p, list);
                bool checked = false;
                for (const QString& v : visible) {
                    if (v.compare(p, Qt::CaseInsensitive) == 0) {
                        checked = true;
                        break;
                    }
                }
                Qt::ItemFlags flags = it->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled;
                const bool fixedSnapshot =
                    (scope == ManagePropsScope::Snapshot && isMain
                     && p.compare(fixedSnapshotProp, Qt::CaseInsensitive) == 0);
                if (!fixedSnapshot) {
                    flags |= Qt::ItemIsDragEnabled;
                }
                it->setFlags(flags);
                it->setCheckState((fixedSnapshot || checked) ? Qt::Checked : Qt::Unchecked);
                if (fixedSnapshot) {
                    it->setToolTip(trk(QStringLiteral("t_snapshot_fixed_prop001"),
                                       QStringLiteral("La propiedad snapshot queda fija en la primera posición.")));
                }
                it->setTextAlignment(Qt::AlignCenter);
                it->setSizeHint(QSize(140, 28));
            }
        };
        auto attachUncheckedReorder = [&dlg](ManagePropsListWidget* list) {
            auto* reorderingUncheckedItems = new bool(false);
            QObject::connect(list, &QListWidget::itemChanged, &dlg,
                             [list, reorderingUncheckedItems](QListWidgetItem* item) {
                if (!list || !item || *reorderingUncheckedItems || item->checkState() != Qt::Unchecked) {
                    return;
                }
                const int row = list->row(item);
                if (row < 0 || row == list->count() - 1) {
                    return;
                }
                *reorderingUncheckedItems = true;
                QListWidgetItem* moved = list->takeItem(row);
                if (moved) {
                    list->addItem(moved);
                    list->setCurrentItem(moved);
                }
                *reorderingUncheckedItems = false;
            });
        };
        auto addGroupTab = [&](const QString& name, const QStringList& visible, bool isMain) {
            auto* list = new ManagePropsListWidget(tabs);
            QStringList normalizedVisible = normalizeList(visible);
            QStringList orderedProps = orderedAll;
            if (scope == ManagePropsScope::Snapshot && !isMain) {
                QString canonicalSnapshot;
                for (const QString& p : orderedProps) {
                    if (p.compare(fixedSnapshotProp, Qt::CaseInsensitive) == 0) {
                        canonicalSnapshot = p;
                        break;
                    }
                }
                if (!canonicalSnapshot.isEmpty()) {
                    orderedProps.removeAll(canonicalSnapshot);
                    normalizedVisible.removeAll(canonicalSnapshot);
                }
            }
            if (scope == ManagePropsScope::Snapshot && isMain && snapshotFixedPresent) {
                QString canonicalSnapshot;
                for (const QString& p : orderedProps) {
                    if (p.compare(fixedSnapshotProp, Qt::CaseInsensitive) == 0) {
                        canonicalSnapshot = p;
                        break;
                    }
                }
                if (!canonicalSnapshot.isEmpty()) {
                    normalizedVisible.removeAll(canonicalSnapshot);
                    normalizedVisible.prepend(canonicalSnapshot);
                }
            }
            configureList(list, orderedProps, normalizedVisible, isMain);
            attachUncheckedReorder(list);
            tabs->addTab(list, name);
            groupTabs.push_back({name, list, isMain});
        };
        addGroupTab(trk(QStringLiteral("t_main_group_001"),
                        QStringLiteral("Principal")),
                    currentVisible,
                    true);
        for (const InlinePropGroupConfig& cfg : currentGroups) {
            addGroupTab(cfg.name, normalizeList(cfg.props), false);
        }
        if (!clickedGroupName.isEmpty()) {
            for (int i = 0; i < groupTabs.size(); ++i) {
                if (groupTabs[i].name.compare(clickedGroupName, Qt::CaseInsensitive) == 0) {
                    tabs->setCurrentIndex(i);
                    break;
                }
            }
        }
        auto* rowBtns = new QHBoxLayout();
        auto* btnNewGroup = new QPushButton(
            trk(QStringLiteral("t_new_group_001"),
                QStringLiteral("Nuevo group")),
            &dlg);
        auto* btnDeleteGroup = new QPushButton(
            trk(QStringLiteral("t_delete_group_001"),
                QStringLiteral("Borrar grupo")),
            &dlg);
        auto* btnAll = new QPushButton(
            trk(QStringLiteral("t_all_001"), QStringLiteral("Todo")), &dlg);
        auto* btnNone = new QPushButton(
            trk(QStringLiteral("t_none_001"), QStringLiteral("Ninguno")), &dlg);
        rowBtns->addWidget(btnNewGroup);
        rowBtns->addWidget(btnDeleteGroup);
        rowBtns->addWidget(btnAll);
        rowBtns->addWidget(btnNone);
        rowBtns->addStretch(1);
        root->addLayout(rowBtns);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        root->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        auto currentList = [tabs, &groupTabs]() -> ManagePropsListWidget* {
            const int idx = tabs ? tabs->currentIndex() : -1;
            return (idx >= 0 && idx < groupTabs.size()) ? groupTabs[idx].list : nullptr;
        };
        connect(btnNewGroup, &QPushButton::clicked, &dlg, [this, &dlg, tabs, &groupTabs, &addGroupTab]() {
            bool ok = false;
            const QString name = QInputDialog::getText(
                &dlg,
                trk(QStringLiteral("t_new_group_001"),
                    QStringLiteral("Nuevo group")),
                trk(QStringLiteral("t_group_name_001"),
                    QStringLiteral("Nombre del group")),
                QLineEdit::Normal,
                QString(),
                &ok).trimmed();
            if (!ok || name.isEmpty()) {
                return;
            }
            for (const ManageGroupTab& tab : groupTabs) {
                if (tab.name.compare(name, Qt::CaseInsensitive) == 0) {
                    QMessageBox::warning(
                        &dlg,
                        trk(QStringLiteral("t_new_group_001"),
                            QStringLiteral("Nuevo group")),
                        trk(QStringLiteral("t_group_exists_001"),
                            QStringLiteral("Ya existe un group con ese nombre.")));
                    return;
                }
            }
            addGroupTab(name, QStringList(), false);
            tabs->setCurrentIndex(groupTabs.size() - 1);
        });
        auto updateDeleteGroupButton = [tabs, &groupTabs, btnDeleteGroup]() {
            const int idx = tabs ? tabs->currentIndex() : -1;
            const bool canDelete = idx > 0 && idx < groupTabs.size() && !groupTabs[idx].isMain;
            btnDeleteGroup->setEnabled(canDelete);
        };
        connect(tabs, &QTabWidget::currentChanged, &dlg, [updateDeleteGroupButton](int) {
            updateDeleteGroupButton();
        });
        connect(btnDeleteGroup, &QPushButton::clicked, &dlg, [tabs, &groupTabs, updateDeleteGroupButton]() {
            if (!tabs) {
                return;
            }
            const int idx = tabs->currentIndex();
            if (idx <= 0 || idx >= groupTabs.size() || groupTabs[idx].isMain) {
                return;
            }
            QWidget* page = tabs->widget(idx);
            tabs->removeTab(idx);
            if (page) {
                page->deleteLater();
            }
            groupTabs.removeAt(idx);
            tabs->setCurrentIndex(qMax(0, idx - 1));
            updateDeleteGroupButton();
        });
        connect(btnAll, &QPushButton::clicked, &dlg, [currentList]() {
            ManagePropsListWidget* list = currentList();
            if (!list) {
                return;
            }
            for (int i = 0; i < list->count(); ++i) {
                if (QListWidgetItem* it = list->item(i)) {
                    it->setCheckState(Qt::Checked);
                }
            }
        });
        connect(btnNone, &QPushButton::clicked, &dlg, [currentList]() {
            ManagePropsListWidget* list = currentList();
            if (!list) {
                return;
            }
            for (int i = 0; i < list->count(); ++i) {
                if (QListWidgetItem* it = list->item(i)) {
                    if (!(it->flags() & Qt::ItemIsDragEnabled) && i == 0) {
                        it->setCheckState(Qt::Checked);
                        continue;
                    }
                    it->setCheckState(Qt::Unchecked);
                }
            }
        });
        updateDeleteGroupButton();
        if (QPushButton* okButton = bb->button(QDialogButtonBox::Ok)) {
            connect(okButton, &QPushButton::clicked, &dlg, [this, &dlg, &groupTabs]() {
                if (groupTabs.isEmpty() || !groupTabs[0].list) {
                    dlg.reject();
                    return;
                }
                int checkedCount = 0;
                for (int i = 0; i < groupTabs[0].list->count(); ++i) {
                    if (QListWidgetItem* it = groupTabs[0].list->item(i); it && it->checkState() == Qt::Checked) {
                        ++checkedCount;
                    }
                }
                if (checkedCount == 0) {
                    QMessageBox::warning(
                        &dlg,
                        trk(QStringLiteral("t_manage_props_vis001"),
                            QStringLiteral("Gestionar visualización de propiedades")),
                        trk(QStringLiteral("t_manage_props_need_one001"),
                            QStringLiteral("Debes seleccionar al menos una propiedad.")));
                    return;
                }
                dlg.accept();
            });
        }
        if (dlg.exec() != QDialog::Accepted) {
            return;
        }
        auto selectedFromList = [normalizeList, scope, snapshotFixedPresent, fixedSnapshotProp](QListWidget* list, bool isMain) {
            QStringList selected;
            if (!list) {
                return selected;
            }
            for (int i = 0; i < list->count(); ++i) {
                QListWidgetItem* it = list->item(i);
                if (!it || it->checkState() != Qt::Checked) {
                    continue;
                }
                selected.push_back(it->text().trimmed());
            }
            selected = normalizeList(selected);
            if (scope == ManagePropsScope::Snapshot) {
                QString canonicalSnapshot;
                for (const QString& p : selected) {
                    if (p.compare(fixedSnapshotProp, Qt::CaseInsensitive) == 0) {
                        canonicalSnapshot = p;
                        break;
                    }
                }
                if (isMain && snapshotFixedPresent) {
                    if (canonicalSnapshot.isEmpty()) {
                        canonicalSnapshot = fixedSnapshotProp;
                    }
                    selected.removeAll(canonicalSnapshot);
                    selected.prepend(canonicalSnapshot);
                } else if (!canonicalSnapshot.isEmpty()) {
                    selected.removeAll(canonicalSnapshot);
                }
            }
            return selected;
        };
        if (scope == ManagePropsScope::Pool) {
            m_poolInlinePropsOrder = selectedFromList(groupTabs.isEmpty() ? nullptr : groupTabs[0].list, true);
            QVector<InlinePropGroupConfig> groups;
            for (int i = 1; i < groupTabs.size(); ++i) {
                InlinePropGroupConfig cfg;
                cfg.name = groupTabs[i].name.trimmed();
                cfg.props = selectedFromList(groupTabs[i].list, false);
                groups.push_back(cfg);
            }
            m_poolInlinePropGroups = groups;
        } else if (scope == ManagePropsScope::Snapshot) {
            m_snapshotInlinePropsOrder = selectedFromList(groupTabs.isEmpty() ? nullptr : groupTabs[0].list, true);
            QVector<InlinePropGroupConfig> groups;
            for (int i = 1; i < groupTabs.size(); ++i) {
                InlinePropGroupConfig cfg;
                cfg.name = groupTabs[i].name.trimmed();
                cfg.props = selectedFromList(groupTabs[i].list, false);
                groups.push_back(cfg);
            }
            m_snapshotInlinePropGroups = groups;
        } else {
            m_datasetInlinePropsOrder = selectedFromList(groupTabs.isEmpty() ? nullptr : groupTabs[0].list, true);
            QVector<InlinePropGroupConfig> groups;
            for (int i = 1; i < groupTabs.size(); ++i) {
                InlinePropGroupConfig cfg;
                cfg.name = groupTabs[i].name.trimmed();
                cfg.props = selectedFromList(groupTabs[i].list, false);
                groups.push_back(cfg);
            }
            m_datasetInlinePropGroups = groups;
        }
        saveUiSettings();

        const QString prevToken = m_connContentToken;
        QTreeWidget* prevTree = m_connContentTree;
        const bool isBottomTree = (tree == m_bottomConnContentTree);
        if (isBottomTree) {
            saveBottomTreeStateForConnection(connIdx);
        } else {
            saveTopTreeStateForConnection(connIdx);
        }
        m_connContentTree = tree;
        m_connContentToken = token;
        if (poolContext) {
            syncConnContentPoolColumns();
        } else {
            syncConnContentPropertyColumns();
        }
        m_connContentTree = prevTree;
        m_connContentToken = prevToken;
        if (isBottomTree) {
            restoreBottomTreeStateForConnection(connIdx);
        } else {
            restoreTopTreeStateForConnection(connIdx);
        }
    };
    auto executeDatasetActionWithStdin =
        [this](const QString& side,
               const QString& actionName,
               const DatasetSelectionContext& ctx,
               const QString& cmd,
               const QByteArray& stdinPayload,
               int timeoutMs = 45000) {
            if (!ctx.valid || ctx.connIdx < 0 || ctx.connIdx >= m_profiles.size()) {
                return false;
            }
            ConnectionProfile profile = m_profiles[ctx.connIdx];
            if (!ensureLocalSudoCredentials(profile)) {
                appLog(QStringLiteral("INFO"), QStringLiteral("%1 cancelada: faltan credenciales sudo locales").arg(actionName));
                return false;
            }
            const QString remoteCmd = withSudo(profile, cmd);
            const QString preview = QStringLiteral("[%1]\n%2")
                                        .arg(QStringLiteral("%1@%2:%3")
                                                 .arg(profile.username, profile.host)
                                                 .arg(profile.port > 0 ? QString::number(profile.port) : QStringLiteral("22")))
                                        .arg(buildSshPreviewCommand(profile, remoteCmd));
            if (!confirmActionExecution(actionName, {preview})) {
                return false;
            }
            setActionsLocked(true);
                appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 %2::%3").arg(actionName, profile.name, ctx.datasetName));
                updateStatus(QStringLiteral("%1 %2::%3").arg(actionName, profile.name, ctx.datasetName));
            QString out;
            QString err;
            int rc = -1;
                const bool ok = runSsh(profile,
                                   remoteCmd,
                                   timeoutMs,
                                   out,
                                   err,
                                   rc,
                                   {},
                                   {},
                                   {},
                                   WindowsCommandMode::Auto,
                                   stdinPayload);
                if (!ok || rc != 0) {
                    const QString failureDetail = err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err;
                    appLog(QStringLiteral("NORMAL"),
                       QStringLiteral("Error en %1: %2").arg(actionName, failureDetail.simplified()));
                QMessageBox::critical(this,
                                      QStringLiteral("ZFSMgr"),
                                      trk(QStringLiteral("t_action_fail001"),
                                          QStringLiteral("%1 falló:\n%2"))
                                          .arg(actionName, failureDetail));
                setActionsLocked(false);
                return false;
            }
            if (!out.trimmed().isEmpty()) {
                appLog(QStringLiteral("INFO"), out.simplified());
            }
            appLog(QStringLiteral("NORMAL"), QStringLiteral("%1 finalizado").arg(actionName));
            updateStatus(QStringLiteral("%1 finalizado %2::%3").arg(actionName, profile.name, ctx.datasetName));
            invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
            reloadDatasetSide(side);
            setActionsLocked(false);
            return true;
        };
    auto promptNewPassphrase = [this](const QString& title, QString& passphraseOut) -> bool {
        QDialog dlg(this);
        dlg.setModal(true);
        dlg.setWindowTitle(title);
        auto* layout = new QFormLayout(&dlg);
        auto* pass1 = new QLineEdit(&dlg);
        auto* pass2 = new QLineEdit(&dlg);
        pass1->setEchoMode(QLineEdit::Password);
        pass2->setEchoMode(QLineEdit::Password);
        layout->addRow(QStringLiteral("Nueva clave"), pass1);
        layout->addRow(QStringLiteral("Repita la clave"), pass2);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        const QString p1 = pass1->text();
        const QString p2 = pass2->text();
        if (p1.isEmpty() || p1 != p2) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Las claves no coinciden."));
            return false;
        }
        passphraseOut = p1;
        return true;
    };
    auto createSnapshotHold = [this](QTreeWidget* tree, QTreeWidgetItem* rawItem) {
        if (!tree || !rawItem) {
            return;
        }
        QTreeWidgetItem* item = rawItem;
        while (item && item->data(0, Qt::UserRole).toString().isEmpty()
               && !item->data(0, kIsPoolRootRole).toBool()) {
            item = item->parent();
        }
        if (!item) {
            return;
        }
        const QString datasetName = item->data(0, Qt::UserRole).toString().trimmed();
        const QString snapshotName = item->data(1, Qt::UserRole).toString().trimmed();
        const int connIdx = item->data(0, kConnIdxRole).toInt();
        const QString poolName = item->data(0, kPoolNameRole).toString().trimmed();
        if (datasetName.isEmpty() || snapshotName.isEmpty() || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
            return;
        }

        bool ok = false;
        const QString holdName = QInputDialog::getText(
            this,
            trk(QStringLiteral("t_new_hold_title001"),
                QStringLiteral("Nuevo Hold")),
            trk(QStringLiteral("t_new_hold_prompt001"),
                QStringLiteral("Nombre del hold")),
            QLineEdit::Normal,
            QString(),
            &ok).trimmed();
        if (!ok || holdName.isEmpty()) {
            return;
        }

        DatasetSelectionContext ctx;
        ctx.valid = true;
        ctx.connIdx = connIdx;
        ctx.poolName = poolName;
        ctx.datasetName = datasetName;
        ctx.snapshotName = snapshotName;
        const QString objectName = QStringLiteral("%1@%2").arg(datasetName, snapshotName);
        auto shQuote = [](QString s) {
            s.replace('\'', QStringLiteral("'\"'\"'"));
            return QStringLiteral("'%1'").arg(s);
        };
        const QString cmd = QStringLiteral("zfs hold %1 %2")
                                .arg(shQuote(holdName),
                                     shQuote(objectName));
        if (!executeDatasetAction(QStringLiteral("conncontent"),
                                  QStringLiteral("Crear hold"),
                                  ctx,
                                  cmd,
                                  45000,
                                  isWindowsConnection(connIdx))) {
            return;
        }

        const QString prevToken = m_connContentToken;
        QTreeWidget* prevTree = m_connContentTree;
        const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
        m_connContentTree = tree;
        m_connContentToken = token;
        saveConnContentTreeState(token);
        populateDatasetTree(tree, connIdx, poolName, QStringLiteral("conncontent"), true);
        auto findInTree = [](QTreeWidget* tw, const QString& ds) -> QTreeWidgetItem* {
            if (!tw || ds.isEmpty()) {
                return nullptr;
            }
            std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
                if (!n) {
                    return nullptr;
                }
                if (n->data(0, Qt::UserRole).toString().trimmed() == ds) {
                    return n;
                }
                for (int i = 0; i < n->childCount(); ++i) {
                    if (QTreeWidgetItem* f = rec(n->child(i))) {
                        return f;
                    }
                }
                return nullptr;
            };
            for (int i = 0; i < tw->topLevelItemCount(); ++i) {
                if (QTreeWidgetItem* f = rec(tw->topLevelItem(i))) {
                    return f;
                }
            }
            return nullptr;
        };
        if (QTreeWidgetItem* restored = findInTree(tree, datasetName)) {
            restored->setData(1, Qt::UserRole, snapshotName);
            if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(restored, 1))) {
                const int idx = cb->findText(snapshotName);
                if (idx > 0) {
                    const QSignalBlocker blocker(cb);
                    cb->setCurrentIndex(idx);
                }
            }
            tree->setCurrentItem(restored);
            refreshDatasetProperties(QStringLiteral("conncontent"));
            syncConnContentPropertyColumns();
        }
        m_connContentTree = prevTree;
        m_connContentToken = prevToken;
    };
    auto releaseSnapshotHold = [this](QTreeWidget* tree, QTreeWidgetItem* rawItem) {
        if (!tree || !rawItem) {
            return;
        }
        QTreeWidgetItem* holdItem = rawItem;
        if (!holdItem->data(0, kConnSnapshotHoldItemRole).toBool()) {
            holdItem = holdItem->parent();
        }
        while (holdItem && !holdItem->data(0, kConnSnapshotHoldItemRole).toBool()) {
            holdItem = holdItem->parent();
        }
        if (!holdItem) {
            return;
        }
        QTreeWidgetItem* snapshotItem = holdItem;
        while (snapshotItem && snapshotItem->data(0, Qt::UserRole).toString().isEmpty()
               && !snapshotItem->data(0, kIsPoolRootRole).toBool()) {
            snapshotItem = snapshotItem->parent();
        }
        if (!snapshotItem) {
            return;
        }
        const QString holdName = holdItem->data(0, kConnSnapshotHoldTagRole).toString().trimmed();
        const QString datasetName = snapshotItem->data(0, Qt::UserRole).toString().trimmed();
        const QString snapshotName = snapshotItem->data(1, Qt::UserRole).toString().trimmed();
        const int connIdx = snapshotItem->data(0, kConnIdxRole).toInt();
        const QString poolName = snapshotItem->data(0, kPoolNameRole).toString().trimmed();
        if (holdName.isEmpty() || datasetName.isEmpty() || snapshotName.isEmpty()
            || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
            return;
        }
        const auto confirm = QMessageBox::question(
            this,
            trk(QStringLiteral("t_release_hold_title001"),
                QStringLiteral("Release")),
            trk(QStringLiteral("t_release_hold_confirm001"),
                QStringLiteral("¿Liberar hold \"%1\" del snapshot \"%2@%3\"?")
                    .arg(holdName, datasetName, snapshotName)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirm != QMessageBox::Yes) {
            return;
        }
        DatasetSelectionContext ctx;
        ctx.valid = true;
        ctx.connIdx = connIdx;
        ctx.poolName = poolName;
        ctx.datasetName = datasetName;
        ctx.snapshotName = snapshotName;
        auto shQuote = [](QString s) {
            s.replace('\'', QStringLiteral("'\"'\"'"));
            return QStringLiteral("'%1'").arg(s);
        };
        const QString objectName = QStringLiteral("%1@%2").arg(datasetName, snapshotName);
        const QString cmd = QStringLiteral("zfs release %1 %2")
                                .arg(shQuote(holdName), shQuote(objectName));
        if (!executeDatasetAction(QStringLiteral("conncontent"),
                                  QStringLiteral("Release hold"),
                                  ctx,
                                  cmd,
                                  45000,
                                  isWindowsConnection(connIdx))) {
            return;
        }

        const QString prevToken = m_connContentToken;
        QTreeWidget* prevTree = m_connContentTree;
        const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
        m_connContentTree = tree;
        m_connContentToken = token;
        saveConnContentTreeState(token);
        populateDatasetTree(tree, connIdx, poolName, QStringLiteral("conncontent"), true);
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
            if (!n) {
                return nullptr;
            }
            if (n->data(0, Qt::UserRole).toString().trimmed() == datasetName) {
                return n;
            }
            for (int i = 0; i < n->childCount(); ++i) {
                if (QTreeWidgetItem* f = rec(n->child(i))) {
                    return f;
                }
            }
            return nullptr;
        };
        QTreeWidgetItem* restored = nullptr;
        for (int i = 0; i < tree->topLevelItemCount() && !restored; ++i) {
            restored = rec(tree->topLevelItem(i));
        }
        if (restored) {
            if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(restored, 1))) {
                const int idx = cb->findText(snapshotName);
                if (idx > 0) {
                    const QSignalBlocker blocker(cb);
                    cb->setCurrentIndex(idx);
                }
            }
            tree->setCurrentItem(restored);
            refreshDatasetProperties(QStringLiteral("conncontent"));
            syncConnContentPropertyColumns();
        }
        m_connContentTree = prevTree;
        m_connContentToken = prevToken;
    };
    auto permissionOwnerItem = [](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        QTreeWidgetItem* owner = item;
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, kIsPoolRootRole).toBool()) {
            owner = owner->parent();
        }
        return owner;
    };
    auto permissionNodeItem = [](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        QTreeWidgetItem* node = item;
        while (node && !node->data(0, kConnPermissionsNodeRole).toBool()) {
            node = node->parent();
        }
        return node;
    };
    auto refreshPermissionsTreeNode = [this, permissionOwnerItem](QTreeWidget* tree, QTreeWidgetItem* rawItem, bool forceReload) {
        if (!tree || !rawItem) {
            return;
        }
        QTreeWidgetItem* owner = permissionOwnerItem(rawItem);
        if (!owner) {
            return;
        }
        populateDatasetPermissionsNode(tree, owner, forceReload);
    };
    auto permissionTokensForDataset = [this](const DatasetSelectionContext& ctx) {
        return availableDelegablePermissions(ctx.datasetName, ctx.connIdx, ctx.poolName);
    };
    auto scopeFlagsForPermission = [](const QString& scope) {
        const QString s = scope.trimmed().toLower();
        if (s == QStringLiteral("local")) {
            return QStringLiteral("-l ");
        }
        if (s == QStringLiteral("descendant")) {
            return QStringLiteral("-d ");
        }
        return QString();
    };
    auto targetFlagsForPermission = [](const QString& targetType, const QString& targetName) {
        const QString tt = targetType.trimmed().toLower();
        if (tt == QStringLiteral("user")) {
            return QStringLiteral("-u %1 ").arg(mwhelpers::shSingleQuote(targetName));
        }
        if (tt == QStringLiteral("group")) {
            return QStringLiteral("-g %1 ").arg(mwhelpers::shSingleQuote(targetName));
        }
        return QStringLiteral("-e ");
    };
    auto promptPermissionGrant = [this, permissionTokensForDataset](const DatasetSelectionContext& ctx,
                                                                   const QString& title,
                                                                   const QString& initialTargetType,
                                                                   const QString& initialTargetName,
                                                                   const QString& initialScope,
                                                                   const QStringList& initialTokens,
                                                                   bool allowTokenSelection,
                                                                   QString& targetTypeOut,
                                                                   QString& targetNameOut,
                                                                   QString& scopeOut,
                                                                   QStringList& tokensOut) {
        QDialog dlg(this);
        dlg.setModal(true);
        dlg.setWindowTitle(title);
        auto* layout = new QVBoxLayout(&dlg);
        auto* form = new QFormLayout();
        auto* targetType = new QComboBox(&dlg);
        targetType->addItem(QStringLiteral("Usuario"), QStringLiteral("user"));
        targetType->addItem(QStringLiteral("Grupo"), QStringLiteral("group"));
        targetType->addItem(QStringLiteral("Everyone"), QStringLiteral("everyone"));
        auto* targetName = new QComboBox(&dlg);
        targetName->setEditable(true);
        auto* scope = new QComboBox(&dlg);
        scope->addItem(QStringLiteral("Local"), QStringLiteral("local"));
        scope->addItem(QStringLiteral("Descendiente"), QStringLiteral("descendant"));
        scope->addItem(QStringLiteral("Local + descendiente"), QStringLiteral("local_descendant"));
        const auto cacheIt = m_datasetPermissionsCache.constFind(
            datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName));
        auto reloadTargetNames = [this, cacheIt, targetType, targetName]() {
            targetName->clear();
            if (cacheIt == m_datasetPermissionsCache.cend()) {
                return;
            }
            const QString kind = targetType->currentData().toString();
            const QStringList names =
                (kind == QStringLiteral("group")) ? cacheIt->systemGroups : cacheIt->systemUsers;
            targetName->addItems(names);
            targetName->setEnabled(kind != QStringLiteral("everyone"));
        };
        QObject::connect(targetType, &QComboBox::currentIndexChanged, &dlg, reloadTargetNames);
        reloadTargetNames();
        if (!initialTargetType.isEmpty()) {
            const int typeIdx = targetType->findData(initialTargetType);
            if (typeIdx >= 0) {
                targetType->setCurrentIndex(typeIdx);
                reloadTargetNames();
            }
        }
        if (!initialTargetName.isEmpty()) {
            targetName->setCurrentText(initialTargetName);
        }
        if (!initialScope.isEmpty()) {
            const int scopeIdx = scope->findData(initialScope);
            if (scopeIdx >= 0) {
                scope->setCurrentIndex(scopeIdx);
            }
        }
        form->addRow(QStringLiteral("Destino"), targetType);
        form->addRow(QStringLiteral("Nombre"), targetName);
        form->addRow(QStringLiteral("Ámbito"), scope);
        layout->addLayout(form);
        QStringList selectedTokens = initialTokens;
        if (allowTokenSelection) {
            auto* pickTokens = new QPushButton(QStringLiteral("Elegir permisos"), &dlg);
            auto* chosenLabel = new QLabel(selectedTokens.isEmpty() ? QStringLiteral("Ninguno seleccionado")
                                                                    : selectedTokens.join(QStringLiteral(", ")),
                                           &dlg);
            QObject::connect(pickTokens, &QPushButton::clicked, &dlg, [&, chosenLabel]() {
                QStringList picked = selectedTokens;
                if (!selectItemsDialog(QStringLiteral("Permisos delegados"),
                                       QStringLiteral("Seleccione permisos o @sets para la nueva delegación."),
                                       permissionTokensForDataset(ctx),
                                       picked)) {
                    return;
                }
                selectedTokens = picked;
                chosenLabel->setText(selectedTokens.isEmpty() ? QStringLiteral("Ninguno seleccionado")
                                                              : selectedTokens.join(QStringLiteral(", ")));
            });
            layout->addWidget(pickTokens);
            layout->addWidget(chosenLabel);
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        targetTypeOut = targetType->currentData().toString();
        targetNameOut = targetName->currentText().trimmed();
        scopeOut = scope->currentData().toString();
        tokensOut = selectedTokens;
        if (allowTokenSelection && tokensOut.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Debe seleccionar al menos un permiso o set."));
            return false;
        }
        if (targetTypeOut != QStringLiteral("everyone") && targetNameOut.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Debe indicar el usuario o grupo."));
            return false;
        }
        return true;
    };
    auto promptCreatePermissions = [this, permissionTokensForDataset](const DatasetSelectionContext& ctx,
                                                                      const QString& title,
                                                                      const QStringList& initialTokens,
                                                                      QStringList& tokensOut) {
        QStringList picked = initialTokens;
        if (!selectItemsDialog(title,
                               QStringLiteral("Seleccione permisos o @sets para los nuevos descendientes."),
                               permissionTokensForDataset(ctx),
                               picked)) {
            return false;
        }
        if (picked.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Debe seleccionar al menos un permiso o set."));
            return false;
        }
        tokensOut = picked;
        return true;
    };
    auto promptPermissionSet = [this, permissionTokensForDataset](const DatasetSelectionContext& ctx,
                                                                  const QString& title,
                                                                  const QString& initialSetName,
                                                                  const QStringList& initialTokens,
                                                                  bool allowEmpty,
                                                                  QString& setNameOut,
                                                                  QStringList& tokensOut) {
        bool ok = false;
        QString setName = QInputDialog::getText(
            this,
            title,
            QStringLiteral("Nombre del set"),
            QLineEdit::Normal,
            initialSetName,
            &ok).trimmed();
        if (!ok || setName.isEmpty()) {
            return false;
        }
        if (!setName.startsWith(QLatin1Char('@'))) {
            setName.prepend(QLatin1Char('@'));
        }
        QStringList picked = initialTokens;
        QStringList available = permissionTokensForDataset(ctx);
        if (!initialSetName.trimmed().isEmpty()) {
            for (int i = available.size() - 1; i >= 0; --i) {
                if (available.at(i).compare(initialSetName.trimmed(), Qt::CaseInsensitive) == 0) {
                    available.removeAt(i);
                }
            }
        }
        if (!selectItemsDialog(title,
                               QStringLiteral("Seleccione permisos o @sets para el nuevo set."),
                               available,
                               picked)) {
            return false;
        }
        if (picked.isEmpty() && !allowEmpty) {
            QMessageBox::warning(this, QStringLiteral("ZFSMgr"), QStringLiteral("Debe seleccionar al menos un permiso o set."));
            return false;
        }
        setNameOut = setName;
        tokensOut = picked;
        return true;
    };
    auto permissionTokensFromNode = [](QTreeWidgetItem* permNode) {
        QStringList tokens;
        if (!permNode) {
            return tokens;
        }
        QTreeWidgetItem* owner = permNode;
        const QString kind = permNode->data(0, kConnPermissionsKindRole).toString();
        if ((kind == QStringLiteral("grant_perm") || kind == QStringLiteral("set_perm")) && permNode->parent()) {
            owner = permNode->parent();
        }
        for (int i = 0; i < owner->childCount(); ++i) {
            QTreeWidgetItem* child = owner->child(i);
            if (!child) {
                continue;
            }
            if (child->data(0, kConnPropRowRole).toBool()) {
                if (child->data(0, kConnPropRowKindRole).toInt() != 2) {
                    continue;
                }
                QTreeWidget* tree = owner->treeWidget();
                if (!tree) {
                    continue;
                }
                for (int col = 4; col < tree->columnCount(); ++col) {
                    const QString token = child->data(col, kConnPermissionsEntryNameRole).toString().trimmed();
                    if (token.isEmpty()) {
                        continue;
                    }
                    QWidget* host = tree->itemWidget(child, col);
                    QCheckBox* cb = host ? host->findChild<QCheckBox*>() : nullptr;
                    if (!cb || !cb->isChecked() || tokens.contains(token, Qt::CaseInsensitive)) {
                        continue;
                    }
                    tokens.push_back(token);
                }
                continue;
            }
            if ((child->flags() & Qt::ItemIsUserCheckable) && child->checkState(0) != Qt::Checked) {
                continue;
            }
            const QString token = child->text(0).trimmed();
            if (!token.isEmpty() && !tokens.contains(token, Qt::CaseInsensitive)) {
                tokens.push_back(token);
            }
        }
        tokens.sort(Qt::CaseInsensitive);
        return tokens;
    };
    auto executePermissionCommand = [this, refreshPermissionsTreeNode](QTreeWidget* tree,
                                                                       QTreeWidgetItem* rawItem,
                                                                       const QString& actionName,
                                                                       const QString& cmd) {
        QTreeWidgetItem* owner = rawItem;
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, kIsPoolRootRole).toBool()) {
            owner = owner->parent();
        }
        if (!owner) {
            return false;
        }
        const QString datasetName = owner->data(0, Qt::UserRole).toString().trimmed();
        const int connIdx = owner->data(0, kConnIdxRole).toInt();
        const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
        DatasetSelectionContext ctx;
        ctx.valid = true;
        ctx.connIdx = connIdx;
        ctx.poolName = poolName;
        ctx.datasetName = datasetName;
        if (ctx.connIdx < 0 || ctx.poolName.isEmpty() || ctx.datasetName.isEmpty()) {
            return false;
        }
        auto stableId = [](QTreeWidgetItem* node) {
            if (!node) {
                return QString();
            }
            if (node->data(0, kConnPermissionsNodeRole).toBool()) {
                return QStringLiteral("perm|%1|%2|%3")
                    .arg(node->data(0, kConnPermissionsKindRole).toString(),
                         node->data(0, kConnPermissionsEntryNameRole).toString().trimmed(),
                         node->text(0).trimmed());
            }
            if (node->data(0, kConnPropGroupNodeRole).toBool()) {
                return QStringLiteral("group|%1|%2")
                    .arg(node->data(0, kConnPropGroupNameRole).toString().trimmed(),
                         node->text(0).trimmed());
            }
            if (node->data(0, kConnSnapshotHoldsNodeRole).toBool()) {
                return QStringLiteral("holds|%1").arg(node->text(0).trimmed());
            }
            if (node->data(0, kConnSnapshotHoldItemRole).toBool()) {
                return QStringLiteral("hold|%1").arg(node->data(0, kConnSnapshotHoldTagRole).toString().trimmed());
            }
            return QStringLiteral("text|%1").arg(node->text(0).trimmed());
        };
        auto collectExpandedPaths = [&](QTreeWidgetItem* datasetNode) {
            QStringList paths;
            QSet<QString> seen;
            std::function<void(QTreeWidgetItem*, QStringList)> rec = [&](QTreeWidgetItem* node, QStringList parts) {
                if (!node) {
                    return;
                }
                if (node != datasetNode) {
                    const QString id = stableId(node);
                    if (!id.isEmpty()) {
                        parts.push_back(id);
                        if (node->isExpanded()) {
                            const QString path = parts.join(QStringLiteral("/"));
                            if (!seen.contains(path)) {
                                seen.insert(path);
                                paths.push_back(path);
                            }
                        }
                    }
                }
                for (int i = 0; i < node->childCount(); ++i) {
                    rec(node->child(i), parts);
                }
            };
            rec(datasetNode, {});
            return paths;
        };
        auto restoreExpandedPaths = [&](QTreeWidgetItem* datasetNode, const QStringList& paths) {
            if (!datasetNode || paths.isEmpty()) {
                return;
            }
            const QSet<QString> wanted(paths.cbegin(), paths.cend());
            std::function<void(QTreeWidgetItem*, QStringList)> rec = [&](QTreeWidgetItem* node, QStringList parts) {
                if (!node) {
                    return;
                }
                if (node != datasetNode) {
                    const QString id = stableId(node);
                    if (!id.isEmpty()) {
                        parts.push_back(id);
                        if (wanted.contains(parts.join(QStringLiteral("/")))) {
                            node->setExpanded(true);
                        }
                    }
                }
                for (int i = 0; i < node->childCount(); ++i) {
                    rec(node->child(i), parts);
                }
            };
            rec(datasetNode, {});
        };
        const bool ownerExpanded = owner->isExpanded();
        const QStringList ownerExpandedPaths = collectExpandedPaths(owner);
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("executePermissionCommand before action=%1 dataset=%2 ownerExpanded=%3 ownerPaths=%4")
                   .arg(actionName,
                        datasetName,
                        ownerExpanded ? QStringLiteral("1") : QStringLiteral("0"),
                        ownerExpandedPaths.join(QStringLiteral(" || "))));
        if (!executeDatasetAction(QStringLiteral("conncontent"), actionName, ctx, cmd, 60000, false)) {
            return false;
        }
        auto findDatasetItem = [&](auto&& self, QTreeWidgetItem* node) -> QTreeWidgetItem* {
            if (!node) {
                return nullptr;
            }
            if (node->data(0, Qt::UserRole).toString().trimmed() == datasetName
                && node->data(0, kConnIdxRole).toInt() == connIdx
                && node->data(0, kPoolNameRole).toString().trimmed() == poolName) {
                return node;
            }
            for (int i = 0; i < node->childCount(); ++i) {
                if (QTreeWidgetItem* found = self(self, node->child(i))) {
                    return found;
                }
            }
            return nullptr;
        };
        QTreeWidgetItem* refreshedOwner = nullptr;
        if (tree) {
            for (int i = 0; i < tree->topLevelItemCount() && !refreshedOwner; ++i) {
                refreshedOwner = findDatasetItem(findDatasetItem, tree->topLevelItem(i));
            }
        }
        if (refreshedOwner) {
            const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            const QString prevToken = m_connContentToken;
            QTreeWidget* prevTree = m_connContentTree;
            m_connContentTree = tree;
            m_connContentToken = token;
            refreshPermissionsTreeNode(tree, refreshedOwner, true);
            refreshedOwner->setExpanded(ownerExpanded);
            restoreExpandedPaths(refreshedOwner, ownerExpandedPaths);
            appLog(QStringLiteral("DEBUG"),
                   QStringLiteral("executePermissionCommand after local-restore action=%1 dataset=%2 ownerExpanded=%3")
                       .arg(actionName,
                            datasetName,
                            refreshedOwner->isExpanded() ? QStringLiteral("1") : QStringLiteral("0")));
            restoreConnContentTreeState(token);
            m_connContentTree = prevTree;
            m_connContentToken = prevToken;
            QPointer<QTreeWidget> safeTree(tree);
            QTimer::singleShot(0, this, [this, safeTree, connIdx, poolName, datasetName, token]() {
                if (!safeTree) {
                    return;
                }
                QTreeWidget* prevTree = m_connContentTree;
                const QString prevToken = m_connContentToken;
                m_connContentTree = safeTree;
                m_connContentToken = token;
                std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
                    if (!node) {
                        return nullptr;
                    }
                    if (node->data(0, Qt::UserRole).toString().trimmed() == datasetName
                        && node->data(0, kConnIdxRole).toInt() == connIdx
                        && node->data(0, kPoolNameRole).toString().trimmed() == poolName) {
                        return node;
                    }
                    for (int i = 0; i < node->childCount(); ++i) {
                        if (QTreeWidgetItem* found = rec(node->child(i))) {
                            return found;
                        }
                    }
                    return nullptr;
                };
                QTreeWidgetItem* ownerNode = nullptr;
                for (int i = 0; i < safeTree->topLevelItemCount() && !ownerNode; ++i) {
                    ownerNode = rec(safeTree->topLevelItem(i));
                }
                if (ownerNode) {
                    populateDatasetPermissionsNode(safeTree, ownerNode, false);
                    restoreConnContentTreeState(token);
                }
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
            });
        }
        return true;
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
        struct EditField {
            int row{-1};
            int sourceOrder{-1};
            QString label;
            QString propKey;
            QLineEdit* line{nullptr};
            QComboBox* combo{nullptr};
            QCheckBox* inherit{nullptr};
            QWidget* rowWidget{nullptr};
        };
        QVector<EditField> fields;
        QDialog dlg(this);
        dlg.setWindowTitle(
            trk(QStringLiteral("t_edit_ds_t_001"), QStringLiteral("Editar dataset"), QStringLiteral("Edit dataset"), QStringLiteral("编辑数据集"))
            + QStringLiteral(": ") + ctx.datasetName);
        dlg.setModal(true);
        dlg.resize(860, 520);
        auto* root = new QVBoxLayout(&dlg);
        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(6);
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
            rowL->setSpacing(6);
            EditField ef;
            ef.row = r;
            ef.sourceOrder = fields.size();
            ef.label = pk->text();
            ef.propKey = prop;
            ef.rowWidget = rowW;
            if (QComboBox* srcCb = qobject_cast<QComboBox*>(m_connContentPropsTable->cellWidget(r, 1))) {
                auto* cb = new QComboBox(rowW);
                for (int i = 0; i < srcCb->count(); ++i) cb->addItem(srcCb->itemText(i));
                cb->setCurrentText(srcCb->currentText());
                cb->setEnabled(valueEditable);
                ef.combo = cb;
                rowL->addWidget(cb, 0);
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
            rowL->addStretch(1);
            fields.push_back(ef);
        }
        auto fieldPriority = [](const EditField& ef) -> int {
            const QString key = ef.propKey.trimmed().toLower();
            if (key == QStringLiteral("dataset") || key == QStringLiteral("name") || key == QStringLiteral("nombre")) {
                return 0;
            }
            if (key == QStringLiteral("mountpoint")) {
                return 1;
            }
            return 100 + ef.sourceOrder;
        };
        std::stable_sort(fields.begin(), fields.end(), [fieldPriority](const EditField& a, const EditField& b) {
            return fieldPriority(a) < fieldPriority(b);
        });

        QFontMetrics fm(dlg.font());
        int maxLabelWidth = 0;
        int maxEditorText = fm.horizontalAdvance(ctx.datasetName);
        for (const EditField& ef : fields) {
            maxLabelWidth = qMax(maxLabelWidth, fm.horizontalAdvance(ef.label));
            if (ef.combo) {
                maxEditorText = qMax(maxEditorText, fm.horizontalAdvance(ef.combo->currentText()));
                for (int i = 0; i < ef.combo->count(); ++i) {
                    maxEditorText = qMax(maxEditorText, fm.horizontalAdvance(ef.combo->itemText(i)));
                }
            } else if (ef.line) {
                maxEditorText = qMax(maxEditorText, fm.horizontalAdvance(ef.line->text()));
            }
        }
        const int editorWidth = qBound(240, maxEditorText + 40, 560);
        const int labelWidth = qBound(90, maxLabelWidth + 10, 260);
        const int columns = 2;
        for (int i = 0; i < fields.size(); ++i) {
            const int r = i / columns;
            const int block = i % columns;
            const int labelCol = block * 2;
            const int valueCol = labelCol + 1;
            auto* lbl = new QLabel(fields[i].label, &dlg);
            lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            lbl->setMinimumWidth(labelWidth);
            lbl->setMaximumWidth(labelWidth);
            lbl->setStyleSheet(QStringLiteral("QLabel { background: #eef4f8; color: #102233; padding: 2px 6px; }"));
            if (!fields[i].rowWidget) {
                continue;
            }
            if (fields[i].line) {
                fields[i].line->setMinimumWidth(editorWidth);
                fields[i].line->setMaximumWidth(editorWidth);
            }
            if (fields[i].combo) {
                fields[i].combo->setMinimumWidth(editorWidth);
                fields[i].combo->setMaximumWidth(editorWidth);
            }
            grid->addWidget(lbl, r, labelCol);
            grid->addWidget(fields[i].rowWidget, r, valueCol);
            grid->setColumnStretch(labelCol, 0);
            grid->setColumnStretch(valueCol, 1);
        }
        root->addLayout(grid, 1);
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
        connect(m_bottomConnContentTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
            if (!m_bottomConnContentTree || !item) {
                return;
            }
            const bool isLazyPropsNode =
                item->data(0, kConnPropGroupNodeRole).toBool()
                && item->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                && item->text(0).trimmed() == trk(QStringLiteral("t_props_lbl_001"),
                                                  QStringLiteral("Propiedades"),
                                                  QStringLiteral("Properties"),
                                                  QStringLiteral("属性"));
            if (isLazyPropsNode) {
                QTimer::singleShot(0, this, [this, tree = m_bottomConnContentTree, item]() {
                    if (!tree || !item) {
                        return;
                    }
                    QTreeWidget* prevTree = m_connContentTree;
                    const QString prevToken = m_connContentToken;
                    QTreeWidgetItem* owner = item->parent();
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
                    m_connContentTree = tree;
                    m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
                    if (item->childCount() == 0) {
                        refreshDatasetProperties(QStringLiteral("conncontent"));
                    }
                    item->setExpanded(true);
                    QTimer::singleShot(0, tree, [item]() {
                        if (item) {
                            item->setExpanded(true);
                        }
                    });
                    m_connContentTree = prevTree;
                    m_connContentToken = prevToken;
                });
                return;
            }
            if (!item->data(0, kConnPermissionsNodeRole).toBool()
                || item->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("root")) {
                return;
            }
            QTreeWidgetItem* owner = item->parent();
            if (!owner) {
                return;
            }
            const bool wasEmpty = (item->childCount() == 0);
            QTimer::singleShot(0, this, [this, tree = m_bottomConnContentTree, owner, item, wasEmpty]() {
                if (!tree || !owner || !item) {
                    return;
                }
                if (wasEmpty && item->childCount() == 0) {
                    populateDatasetPermissionsNode(tree, owner, false);
                }
                if (wasEmpty) {
                    item->setExpanded(true);
                } else {
                    item->setExpanded(!item->isExpanded());
                }
            });
        });
        connect(m_bottomConnContentTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
            if (!m_bottomConnContentTree || m_syncingConnContentColumns || m_rebuildingBottomConnContentTree) {
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
            const bool isGroupNode = sel && sel->data(0, kConnPropGroupNodeRole).toBool();
            const bool isHoldItem = sel && sel->data(0, kConnSnapshotHoldItemRole).toBool();
            const bool isPermissionsNode = sel && sel->data(0, kConnPermissionsNodeRole).toBool();
            const bool isPoolContext =
                sel && (sel->data(0, kIsPoolRootRole).toBool() || isInfoNodeOrInside(sel));
            const bool isLazyPropsNode =
                sel && isGroupNode && !isPoolContext
                && sel->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                && sel->text(0).trimmed() == trk(QStringLiteral("t_props_lbl_001"),
                                                 QStringLiteral("Propiedades"),
                                                 QStringLiteral("Properties"),
                                                 QStringLiteral("属性"))
                && sel->childCount() == 0;
            const bool isLazyPermissionsNode =
                sel && isPermissionsNode
                && sel->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
                && sel->childCount() == 0;
            if ((isPropRow || isGroupNode || isHoldItem || isPermissionsNode)
                && !isPoolContext && !isLazyPropsNode && !isLazyPermissionsNode) {
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
                if (isLazyPermissionsNode) {
                    populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                    if (sel) {
                        sel->setExpanded(true);
                    }
                } else {
                    refreshDatasetProperties(QStringLiteral("conncontent"));
                    if (isLazyPropsNode && sel) {
                        sel->setExpanded(true);
                        QTimer::singleShot(0, m_bottomConnContentTree, [sel]() {
                            if (sel) {
                                sel->setExpanded(true);
                            }
                        });
                    }
                    syncConnContentPropertyColumns();
                }
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
        connect(m_bottomConnContentTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
            if (!m_bottomConnContentTree || !item) {
                return;
            }
            resizeTreeColumnsToVisibleContent(m_bottomConnContentTree);
            if (!item->data(0, kConnPermissionsNodeRole).toBool()) {
                return;
            }
            if (item->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root") && item->childCount() == 0) {
                if (QTreeWidgetItem* owner = item->parent()) {
                    populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                    resizeTreeColumnsToVisibleContent(m_bottomConnContentTree);
                }
            }
        });
        m_bottomConnContentTree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_bottomConnContentTree, &QWidget::customContextMenuRequested, this,
                [this, applyInlineSectionVisibility, manageInlinePropsVisualization, createSnapshotHold, releaseSnapshotHold,
                 executeDatasetActionWithStdin, promptNewPassphrase, permissionNodeItem,
                 permissionOwnerItem, refreshPermissionsTreeNode, promptPermissionGrant,
                 promptPermissionSet, permissionTokensFromNode,
                 executePermissionCommand, scopeFlagsForPermission, targetFlagsForPermission](const QPoint& pos) {
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
                item = m_bottomConnContentTree->currentItem();
                if (!item) {
                    return;
                }
            }

            if (QTreeWidgetItem* permNode = permissionNodeItem(item)) {
                QTreeWidgetItem* owner = permissionOwnerItem(item);
                if (!owner) {
                    return;
                }
                const QString prevToken = m_connContentToken;
                QTreeWidget* prevTree = m_connContentTree;
                m_connContentTree = m_bottomConnContentTree;
                m_connContentToken = QStringLiteral("%1::%2")
                                         .arg(owner->data(0, kConnIdxRole).toInt())
                                         .arg(owner->data(0, kPoolNameRole).toString().trimmed());
                const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (!ctx.valid || !ctx.snapshotName.isEmpty() || isWindowsConnection(ctx.connIdx)) {
                    m_connContentTree = prevTree;
                    m_connContentToken = prevToken;
                    return;
                }
                if (permNode->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
                    && permNode->childCount() == 0) {
                    populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                }
                QMenu permMenu(this);
                const QString kind = permNode->data(0, kConnPermissionsKindRole).toString();
                QAction* aRefreshPerms = permMenu.addAction(QStringLiteral("Refrescar permisos"));
                QAction* aNewGrant = permMenu.addAction(QStringLiteral("Nueva delegación"));
                QAction* aNewSet = permMenu.addAction(QStringLiteral("Nuevo set de permisos"));
                QAction* aEditGrant = nullptr;
                QAction* aDeleteGrant = nullptr;
                QAction* aRenameSet = nullptr;
                QAction* aDeleteSet = nullptr;
                if (kind == QStringLiteral("grant") || kind == QStringLiteral("grant_perm")) {
                    permMenu.addSeparator();
                    aEditGrant = permMenu.addAction(QStringLiteral("Editar delegación"));
                    aDeleteGrant = permMenu.addAction(QStringLiteral("Eliminar delegación"));
                } else if (kind == QStringLiteral("set") || kind == QStringLiteral("set_perm")) {
                    permMenu.addSeparator();
                    aRenameSet = permMenu.addAction(QStringLiteral("Renombrar conjunto de permisos"));
                    aDeleteSet = permMenu.addAction(QStringLiteral("Eliminar set"));
                }
                QAction* picked = permMenu.exec(m_bottomConnContentTree->viewport()->mapToGlobal(pos));
                if (picked == aRefreshPerms) {
                    refreshPermissionsTreeNode(m_bottomConnContentTree, item, true);
                } else if (picked == aNewGrant) {
                    QString targetType;
                    QString targetName;
                    QString scope;
                    QStringList tokens;
                    if (promptPermissionGrant(ctx,
                                              QStringLiteral("Nueva delegación"),
                                              QString(),
                                              QString(),
                                              QString(),
                                              {},
                                              false,
                                              targetType,
                                              targetName,
                                              scope,
                                              tokens)) {
                        const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                        auto it = m_datasetPermissionsCache.find(cacheKey);
                        if (it != m_datasetPermissionsCache.end()) {
                            bool exists = false;
                            auto appendPending = [&](QVector<DatasetPermissionGrant>& grants) {
                                for (const DatasetPermissionGrant& g : grants) {
                                    if (g.scope == scope && g.targetType == targetType && g.targetName == targetName) {
                                        exists = true;
                                        break;
                                    }
                                }
                                if (!exists) {
                                    DatasetPermissionGrant g;
                                    g.scope = scope;
                                    g.targetType = targetType;
                                    g.targetName = targetName;
                                    g.pending = true;
                                    grants.push_back(g);
                                    it->dirty = true;
                                    exists = true;
                                }
                            };
                            if (scope == QStringLiteral("local")) {
                                appendPending(it->localGrants);
                            } else if (scope == QStringLiteral("descendant")) {
                                appendPending(it->descendantGrants);
                            } else {
                                appendPending(it->localDescendantGrants);
                            }
                            populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                            updateApplyPropsButtonState();
                            for (int i = 0; i < owner->childCount(); ++i) {
                                QTreeWidgetItem* child = owner->child(i);
                                if (!child || !child->data(0, kConnPermissionsNodeRole).toBool()
                                    || child->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("root")) {
                                    continue;
                                }
                                child->setExpanded(true);
                                for (int j = 0; j < child->childCount(); ++j) {
                                    QTreeWidgetItem* sec = child->child(j);
                                    if (!sec || sec->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("grants_root")) {
                                        continue;
                                    }
                                    sec->setExpanded(true);
                                    for (int k = 0; k < sec->childCount(); ++k) {
                                        QTreeWidgetItem* grantNode = sec->child(k);
                                        if (!grantNode) {
                                            continue;
                                        }
                                        if (grantNode->data(0, kConnPermissionsScopeRole).toString() == scope
                                            && grantNode->data(0, kConnPermissionsTargetTypeRole).toString() == targetType
                                            && grantNode->data(0, kConnPermissionsTargetNameRole).toString() == targetName) {
                                            grantNode->setExpanded(true);
                                            m_bottomConnContentTree->setCurrentItem(grantNode);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (picked == aEditGrant) {
                    QTreeWidgetItem* grantNode = permNode;
                    if (kind == QStringLiteral("grant_perm") && permNode->parent()) {
                        grantNode = permNode->parent();
                    }
                    QString targetType = grantNode->data(0, kConnPermissionsTargetTypeRole).toString();
                    QString targetName = grantNode->data(0, kConnPermissionsTargetNameRole).toString();
                    QString scope = grantNode->data(0, kConnPermissionsScopeRole).toString();
                    QStringList tokens = permissionTokensFromNode(grantNode);
                    QString newTargetType;
                    QString newTargetName;
                    QString newScope;
                    QStringList newTokens;
                    if (promptPermissionGrant(ctx,
                                              QStringLiteral("Editar delegación"),
                                              targetType,
                                              targetName,
                                              scope,
                                              tokens,
                                              false,
                                              newTargetType,
                                              newTargetName,
                                              newScope,
                                              newTokens)) {
                        const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                        auto it = m_datasetPermissionsCache.find(cacheKey);
                        if (it != m_datasetPermissionsCache.end()) {
                            auto updateGrant = [&](QVector<DatasetPermissionGrant>& grants) {
                                for (DatasetPermissionGrant& g : grants) {
                                    if (g.scope == scope && g.targetType == targetType && g.targetName == targetName) {
                                        g.scope = newScope;
                                        g.targetType = newTargetType;
                                        g.targetName = newTargetName;
                                        it->dirty = true;
                                        return true;
                                    }
                                }
                                return false;
                            };
                            if (updateGrant(it->localGrants) || updateGrant(it->descendantGrants)
                                || updateGrant(it->localDescendantGrants)) {
                                populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                                updateApplyPropsButtonState();
                            }
                        }
                    }
                } else if (picked == aNewSet) {
                    QString setName;
                    QStringList tokens;
                    if (promptPermissionSet(ctx,
                                            QStringLiteral("Nuevo set de permisos"),
                                            QString(),
                                            {},
                                            false,
                                            setName,
                                            tokens)) {
                        const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                        auto it = m_datasetPermissionsCache.find(cacheKey);
                        if (it != m_datasetPermissionsCache.end()) {
                            bool exists = false;
                            for (const DatasetPermissionSet& s : it->permissionSets) {
                                if (s.name.compare(setName, Qt::CaseInsensitive) == 0) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists) {
                                DatasetPermissionSet s;
                                s.name = setName;
                                s.permissions = tokens;
                                it->permissionSets.push_back(s);
                                it->dirty = true;
                                populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                                updateApplyPropsButtonState();
                            }
                        }
                    }
                } else if (picked == aRenameSet) {
                    QTreeWidgetItem* setNode = permNode;
                    if (kind == QStringLiteral("set_perm") && permNode->parent()) {
                        setNode = permNode->parent();
                    }
                    const QString oldSetName = setNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                    const QStringList tokens = permissionTokensFromNode(setNode);
                    bool ok = false;
                    QString newSetName = QInputDialog::getText(
                        this,
                        QStringLiteral("Renombrar conjunto de permisos"),
                        QStringLiteral("Nuevo nombre"),
                        QLineEdit::Normal,
                        oldSetName,
                        &ok).trimmed();
                    if (ok && !newSetName.isEmpty()) {
                        if (!newSetName.startsWith(QLatin1Char('@'))) {
                            newSetName.prepend(QLatin1Char('@'));
                        }
                        const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                        auto it = m_datasetPermissionsCache.find(cacheKey);
                        if (it != m_datasetPermissionsCache.end()) {
                            for (DatasetPermissionSet& s : it->permissionSets) {
                                if (s.name.compare(oldSetName, Qt::CaseInsensitive) == 0) {
                                    s.name = newSetName;
                                    it->dirty = true;
                                    break;
                                }
                            }
                            populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                            updateApplyPropsButtonState();
                        }
                    }
                } else if (picked == aDeleteGrant) {
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it != m_datasetPermissionsCache.end()) {
                        auto removeGrant = [&](QVector<DatasetPermissionGrant>& grants) {
                            for (int i = grants.size() - 1; i >= 0; --i) {
                                const DatasetPermissionGrant& g = grants.at(i);
                                if (g.scope == permNode->data(0, kConnPermissionsScopeRole).toString()
                                    && g.targetType == permNode->data(0, kConnPermissionsTargetTypeRole).toString()
                                    && g.targetName == permNode->data(0, kConnPermissionsTargetNameRole).toString()) {
                                    grants.removeAt(i);
                                    it->dirty = true;
                                }
                            }
                        };
                        removeGrant(it->localGrants);
                        removeGrant(it->descendantGrants);
                        removeGrant(it->localDescendantGrants);
                        populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                        updateApplyPropsButtonState();
                    }
                } else if (picked == aDeleteSet) {
                    QString setName = permNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                    if (kind == QStringLiteral("set_perm") && permNode->parent()) {
                        setName = permNode->parent()->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                    }
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it != m_datasetPermissionsCache.end()) {
                        for (int i = it->permissionSets.size() - 1; i >= 0; --i) {
                            if (it->permissionSets.at(i).name.compare(setName, Qt::CaseInsensitive) == 0) {
                                it->permissionSets.removeAt(i);
                                it->dirty = true;
                            }
                        }
                        populateDatasetPermissionsNode(m_bottomConnContentTree, owner, false);
                        updateApplyPropsButtonState();
                    }
                }
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                return;
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
            auto deleteLabelForItem = [this](int itemConnIdx, const QString& itemPoolName, QTreeWidgetItem* targetItem) {
                QTreeWidgetItem* owner = targetItem;
                while (owner) {
                    const QString ownerDatasetName = owner->data(0, Qt::UserRole).toString().trimmed();
                    const QString ownerSnapshotName = owner->data(1, Qt::UserRole).toString().trimmed();
                    if (!ownerDatasetName.isEmpty() || !ownerSnapshotName.isEmpty()
                        || owner->data(0, kIsPoolRootRole).toBool()) {
                        break;
                    }
                    owner = owner->parent();
                }
                const QString snapshotName = owner ? owner->data(1, Qt::UserRole).toString().trimmed() : QString();
                const QString datasetName = owner ? owner->data(0, Qt::UserRole).toString().trimmed() : QString();
                if (!snapshotName.isEmpty() && !datasetName.isEmpty()) {
                    return QStringLiteral("%1 %2@%3").arg(
                        trk(QStringLiteral("t_ctx_delete_snapshot001"),
                            QStringLiteral("Borrar Snapshot")),
                        datasetName,
                        snapshotName);
                }
                if (!datasetName.isEmpty()) {
                    const auto it = m_poolDatasetCache.constFind(datasetCacheKey(itemConnIdx, itemPoolName));
                    if (it != m_poolDatasetCache.cend()) {
                        const auto recIt = it->recordByName.constFind(datasetName);
                        if (recIt != it->recordByName.cend()) {
                            const DatasetRecord& rec = recIt.value();
                            if (rec.mounted.trimmed() == QStringLiteral("-")
                                && rec.mountpoint.trimmed() == QStringLiteral("-")) {
                                return QStringLiteral("%1 %2").arg(
                                    trk(QStringLiteral("t_ctx_delete_zvol001"),
                                        QStringLiteral("Borrar ZVol")),
                                    datasetName);
                            }
                        }
                    }
                    return QStringLiteral("%1 %2").arg(
                        trk(QStringLiteral("t_ctx_delete_dataset001"),
                            QStringLiteral("Borrar Dataset")),
                        datasetName);
                }
                return trk(QStringLiteral("t_ctx_delete_dataset001"),
                           QStringLiteral("Borrar Dataset"));
            };
            const bool isPoolRoot = item->data(0, kIsPoolRootRole).toBool();
            auto isInfoNodeOrInside = [](QTreeWidgetItem* n) -> bool {
                for (QTreeWidgetItem* p = n; p; p = p->parent()) {
                    if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                        return true;
                    }
                }
                return false;
            };
            const bool isPoolInfoContext = isInfoNodeOrInside(item);
            const bool isSnapshotHoldContext = item->data(0, kConnSnapshotHoldItemRole).toBool()
                                               || (item->parent() && item->parent()->data(0, kConnSnapshotHoldItemRole).toBool());
            QTreeWidgetItem* holdContextItem = item;
            if (holdContextItem && !holdContextItem->data(0, kConnSnapshotHoldItemRole).toBool()) {
                holdContextItem = holdContextItem->parent();
            }
            const QString holdContextName =
                holdContextItem ? holdContextItem->data(0, kConnSnapshotHoldTagRole).toString().trimmed() : QString();
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
                const bool canHistory = canExport;
                const bool canSync = canExport;
                const bool canScrub = canExport;
                const bool canTrim = canExport;
                const bool canInitialize = canExport;
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
                QAction* aHistory = menu.addAction(
                    trk(QStringLiteral("t_pool_history_t1"),
                        QStringLiteral("Historial")));
                QAction* aSync = menu.addAction(QStringLiteral("Sync"));
                QAction* aScrub = menu.addAction(QStringLiteral("Scrub"));
                QAction* aTrim = menu.addAction(QStringLiteral("Trim"));
                QAction* aInitialize = menu.addAction(QStringLiteral("Initialize"));
                QAction* aDestroy = menu.addAction(QStringLiteral("Destroy"));
                menu.addSeparator();
                QAction* aShowPoolInfo = menu.addAction(QStringLiteral("Mostrar Información del pool"));
                aShowPoolInfo->setCheckable(true);
                aShowPoolInfo->setChecked(m_showPoolInfoNode);
                aUpdate->setEnabled(canRefresh);
                aImport->setEnabled(canImport);
                aExport->setEnabled(canExport);
                aHistory->setEnabled(canHistory);
                aSync->setEnabled(canSync);
                aScrub->setEnabled(canScrub);
                aTrim->setEnabled(canTrim);
                aInitialize->setEnabled(canInitialize);
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
                } else if (picked == aHistory && canHistory && poolRow >= 0) {
                    showPoolHistoryFromRow(poolRow);
                } else if (picked == aSync && canSync && poolRow >= 0) {
                    syncPoolFromRow(poolRow);
                } else if (picked == aScrub && canScrub && poolRow >= 0) {
                    scrubPoolFromRow(poolRow);
                } else if (picked == aTrim && canTrim && poolRow >= 0) {
                    trimPoolFromRow(poolRow);
                } else if (picked == aInitialize && canInitialize && poolRow >= 0) {
                    initializePoolFromRow(poolRow);
                } else if (picked == aDestroy && canDestroy && poolRow >= 0) {
                    destroyPoolFromRow(poolRow);
                } else if (picked == aShowPoolInfo) {
                    m_showPoolInfoNode = aShowPoolInfo->isChecked();
                    applyInlineSectionVisibility();
                }
                return;
            }
            if (isPoolInfoContext) {
                QAction* aManage = menu.addAction(
                    trk(QStringLiteral("t_manage_props_vis001"),
                        QStringLiteral("Gestionar visualización de propiedades")));
                QAction* aShowPoolInfo = menu.addAction(QStringLiteral("Mostrar Información del pool"));
                aShowPoolInfo->setCheckable(true);
                aShowPoolInfo->setChecked(m_showPoolInfoNode);
                QAction* aShowInlineProps = menu.addAction(QStringLiteral("Mostrar propiedades en línea"));
                aShowInlineProps->setCheckable(true);
                aShowInlineProps->setChecked(m_showInlinePropertyNodes);
                QAction* aShowInlinePerms = menu.addAction(QStringLiteral("Mostrar Permisos en línea"));
                aShowInlinePerms->setCheckable(true);
                aShowInlinePerms->setChecked(m_showInlinePermissionsNodes);
                QAction* picked = menu.exec(m_bottomConnContentTree->viewport()->mapToGlobal(pos));
                if (picked == aManage) {
                    manageInlinePropsVisualization(m_bottomConnContentTree, item, true);
                } else if (picked == aShowPoolInfo) {
                    m_showPoolInfoNode = aShowPoolInfo->isChecked();
                    applyInlineSectionVisibility();
                } else if (picked == aShowInlineProps) {
                    m_showInlinePropertyNodes = aShowInlineProps->isChecked();
                    applyInlineSectionVisibility();
                } else if (picked == aShowInlinePerms) {
                    m_showInlinePermissionsNodes = aShowInlinePerms->isChecked();
                    applyInlineSectionVisibility();
                }
                return;
            }

            QAction* aManage = menu.addAction(
                trk(QStringLiteral("t_manage_props_vis001"),
                    QStringLiteral("Gestionar visualización de propiedades")));
            QAction* aShowInlineProps = menu.addAction(QStringLiteral("Mostrar propiedades en línea"));
            aShowInlineProps->setCheckable(true);
            aShowInlineProps->setChecked(m_showInlinePropertyNodes);
            QAction* aShowInlinePerms = menu.addAction(QStringLiteral("Mostrar Permisos en línea"));
            aShowInlinePerms->setCheckable(true);
            aShowInlinePerms->setChecked(m_showInlinePermissionsNodes);
            menu.addSeparator();
            QAction* aCreate = menu.addAction(
                trk(QStringLiteral("t_ctx_create_dsv001"),
                    QStringLiteral("Crear dataset/snapshot/vol"),
                    QStringLiteral("Create dataset/snapshot/vol"),
                    QStringLiteral("创建 dataset/snapshot/vol")));
            QAction* aDelete = menu.addAction(
                deleteLabelForItem(connIdx, poolName, item));
            QMenu* mEncryption = menu.addMenu(QStringLiteral("Encriptación"));
            QAction* aLoadKey = mEncryption->addAction(QStringLiteral("Load key"));
            QAction* aUnloadKey = mEncryption->addAction(QStringLiteral("Unload key"));
            QAction* aChangeKey = mEncryption->addAction(QStringLiteral("Change key"));
            menu.addSeparator();
            QMenu* mSelectSnapshot = menu.addMenu(
                trk(QStringLiteral("t_ctx_sel_snap001"),
                    QStringLiteral("Seleccionar snapshot"),
                    QStringLiteral("Select snapshot"),
                    QStringLiteral("选择快照")));
            QMap<QAction*, QString> snapshotActions;
            const QString itemDatasetPath = item->data(0, Qt::UserRole).toString().trimmed();
            {
                const QStringList snaps = item->data(1, Qt::UserRole + 1).toStringList();
                const QString currentSnap = item->data(1, Qt::UserRole).toString().trimmed();
                QAction* noneAct = mSelectSnapshot->addAction(QStringLiteral("(ninguno)"));
                noneAct->setCheckable(true);
                noneAct->setChecked(currentSnap.isEmpty());
                snapshotActions.insert(noneAct, QString());
                if (!snaps.isEmpty()) {
                    mSelectSnapshot->addSeparator();
                }
                for (const QString& s : snaps) {
                    const QString snapName = s.trimmed();
                    if (snapName.isEmpty()) {
                        continue;
                    }
                    QAction* sa = mSelectSnapshot->addAction(snapName);
                    sa->setCheckable(true);
                    sa->setChecked(snapName == currentSnap);
                    snapshotActions.insert(sa, snapName);
                }
            }
            mSelectSnapshot->setEnabled(!actionsLocked() && !snapshotActions.isEmpty());
            QAction* aRollback = menu.addAction(QStringLiteral("Rollback"));
            QAction* aNewHold = menu.addAction(
                trk(QStringLiteral("t_new_hold_title001"),
                    QStringLiteral("Nuevo Hold")));
            QAction* aReleaseHold = menu.addAction(
                holdContextName.isEmpty()
                    ? trk(QStringLiteral("t_release_hold_title001"),
                          QStringLiteral("Release"))
                    : QStringLiteral("%1 %2").arg(
                          trk(QStringLiteral("t_release_hold_title001"),
                              QStringLiteral("Release")),
                          holdContextName));
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
            auto datasetPropFromCache = [this](const DatasetSelectionContext& c, const QString& prop) -> QString {
                if (!c.valid || c.datasetName.isEmpty() || !c.snapshotName.isEmpty()) {
                    return QString();
                }
                const auto itCache = m_datasetPropsCache.constFind(
                    datasetPropsCacheKey(c.connIdx, c.poolName, c.datasetName));
                if (itCache == m_datasetPropsCache.cend() || !itCache->loaded) {
                    return QString();
                }
                for (const DatasetPropCacheRow& row : itCache->rows) {
                    if (row.prop.compare(prop, Qt::CaseInsensitive) == 0) {
                        return row.value.trimmed();
                    }
                }
                return QString();
            };
            const QString encryptionRoot = datasetPropFromCache(ctx, QStringLiteral("encryptionroot"));
            const QString keyStatus = datasetPropFromCache(ctx, QStringLiteral("keystatus")).toLower();
            const QString keyLocation = datasetPropFromCache(ctx, QStringLiteral("keylocation")).toLower();
            const QString keyFormat = datasetPropFromCache(ctx, QStringLiteral("keyformat")).toLower();
            const bool isEncryptionRoot =
                hasConnSel && !hasConnSnap
                && !encryptionRoot.isEmpty()
                && encryptionRoot.compare(ctx.datasetName, Qt::CaseInsensitive) == 0;
            const bool keyLoaded = (keyStatus == QStringLiteral("available"));
            aLoadKey->setEnabled(isEncryptionRoot && !keyLoaded);
            aUnloadKey->setEnabled(isEncryptionRoot && keyLoaded);
            aChangeKey->setEnabled(isEncryptionRoot && keyLoaded);
            mEncryption->setEnabled(isEncryptionRoot);
            aManage->setEnabled(hasConnSel);
            aRollback->setEnabled(!actionsLocked() && hasConnSnap);
            aCreate->setEnabled(!actionsLocked() && hasConnSel && !hasConnSnap);
            aNewHold->setEnabled(!actionsLocked() && hasConnSnap);
            aReleaseHold->setEnabled(!actionsLocked() && hasConnSnap && isSnapshotHoldContext);
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
            if (snapshotActions.contains(picked)) {
                const QString snapName = snapshotActions.value(picked);
                QTreeWidgetItem* targetItem = item;
                if (!itemDatasetPath.isEmpty()) {
                    auto findInTree = [](QTreeWidget* tw, const QString& ds) -> QTreeWidgetItem* {
                        if (!tw || ds.isEmpty()) {
                            return nullptr;
                        }
                        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
                            if (!n) {
                                return nullptr;
                            }
                            if (n->data(0, Qt::UserRole).toString().trimmed() == ds) {
                                return n;
                            }
                            for (int i = 0; i < n->childCount(); ++i) {
                                if (QTreeWidgetItem* f = rec(n->child(i))) {
                                    return f;
                                }
                            }
                            return nullptr;
                        };
                        for (int i = 0; i < tw->topLevelItemCount(); ++i) {
                            if (QTreeWidgetItem* f = rec(tw->topLevelItem(i))) {
                                return f;
                            }
                        }
                        return nullptr;
                    };
                    if (QTreeWidgetItem* found = findInTree(m_bottomConnContentTree, itemDatasetPath)) {
                        targetItem = found;
                    }
                }
                if (!targetItem) {
                    m_connContentTree = prevTree;
                    m_connContentToken = prevToken;
                    return;
                }
                onSnapshotComboChanged(
                    m_bottomConnContentTree,
                    targetItem,
                    QStringLiteral("conncontent"),
                    snapName.isEmpty() ? QStringLiteral("(ninguno)") : snapName);
                const DatasetSelectionContext sctx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (sctx.valid && !sctx.datasetName.isEmpty()) {
                    setConnectionDestinationSelection(sctx);
                } else {
                    setConnectionDestinationSelection(DatasetSelectionContext{});
                }
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                return;
            }
            if (picked == aManage) {
                manageInlinePropsVisualization(m_bottomConnContentTree, item, false);
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                return;
            }
            if (picked == aShowInlineProps) {
                m_showInlinePropertyNodes = aShowInlineProps->isChecked();
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                applyInlineSectionVisibility();
                return;
            }
            if (picked == aShowInlinePerms) {
                m_showInlinePermissionsNodes = aShowInlinePerms->isChecked();
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                applyInlineSectionVisibility();
                return;
            }
            if (picked == aNewHold) {
                createSnapshotHold(m_bottomConnContentTree, item);
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
                return;
            }
            if (picked == aReleaseHold) {
                releaseSnapshotHold(m_bottomConnContentTree, item);
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
            } else if (picked == aLoadKey || picked == aUnloadKey || picked == aChangeKey) {
                auto shQuote = [](QString s) {
                    s.replace('\'', QStringLiteral("'\"'\"'"));
                    return QStringLiteral("'%1'").arg(s);
                };
                QString actionName;
                QString cmd;
                QByteArray stdinPayload;
                if (picked == aLoadKey) {
                    actionName = QStringLiteral("Load key");
                    cmd = QStringLiteral("zfs load-key %1").arg(shQuote(ctx.datasetName));
                } else if (picked == aUnloadKey) {
                    actionName = QStringLiteral("Unload key");
                    cmd = QStringLiteral("zfs unload-key %1").arg(shQuote(ctx.datasetName));
                } else {
                    actionName = QStringLiteral("Change key");
                    cmd = QStringLiteral("zfs change-key -o keylocation=prompt%1 %2")
                              .arg(keyFormat == QStringLiteral("passphrase")
                                       ? QStringLiteral(" -o keyformat=passphrase")
                                       : QString(),
                                   shQuote(ctx.datasetName));
                }
                if (picked == aChangeKey) {
                    QString newPassphrase;
                    if (!promptNewPassphrase(actionName, newPassphrase)) {
                        m_connContentTree = prevTree;
                        m_connContentToken = prevToken;
                        return;
                    }
                    stdinPayload = (newPassphrase + QStringLiteral("\n") + newPassphrase + QStringLiteral("\n")).toUtf8();
                    executeDatasetActionWithStdin(QStringLiteral("conncontent"), actionName, ctx, cmd, stdinPayload, 90000);
                } else if (picked == aLoadKey && keyLocation == QStringLiteral("prompt")) {
                    bool ok = false;
                    const QString passphrase = QInputDialog::getText(
                        this,
                        actionName,
                        QStringLiteral("Clave"),
                        QLineEdit::Password,
                        QString(),
                        &ok);
                    if (!ok || passphrase.isEmpty()) {
                        m_connContentTree = prevTree;
                        m_connContentToken = prevToken;
                        return;
                    }
                    stdinPayload = (passphrase + QStringLiteral("\n")).toUtf8();
                    executeDatasetActionWithStdin(QStringLiteral("conncontent"), actionName, ctx, cmd, stdinPayload, 90000);
                } else {
                    executeDatasetAction(QStringLiteral("conncontent"), actionName, ctx, cmd, 90000);
                }
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
        int connIdx = -1;
        int rowForMenu = -1;
        if (idxAt.isValid() && idxAt.row() >= 0) {
            rowForMenu = idxAt.row();
            m_connectionsTable->setCurrentCell(idxAt.row(), 1);
        } else {
            rowForMenu = m_connectionsTable->currentRow();
        }
        if (rowForMenu >= 0 && rowForMenu < m_connectionsTable->rowCount()) {
            QTableWidgetItem* it = m_connectionsTable->item(rowForMenu, 1);
            if (it) {
                bool ok = false;
                const int idx = it->data(Qt::UserRole).toInt(&ok);
                if (ok) {
                    connIdx = idx;
                }
            }
        }
        const bool hasConn = (connIdx >= 0 && connIdx < m_profiles.size());
        const bool isDisconnected = hasConn && isConnectionDisconnected(connIdx);
        const bool canRefresh = hasConn && !isDisconnected && !actionsLocked();
        const bool canEditDelete = hasConn && !actionsLocked() && !isLocalConnection(connIdx)
            && !isConnectionRedirectedToLocal(connIdx);
        const bool hasWindowsUnixLayerReady =
            hasConn
            && connIdx < m_states.size()
            && isWindowsConnection(connIdx)
            && m_states[connIdx].unixFromMsysOrMingw
            && m_states[connIdx].missingUnixCommands.isEmpty()
            && !m_states[connIdx].detectedUnixCommands.isEmpty();
        const bool canInstallMsys =
            hasConn && !actionsLocked() && !isDisconnected && isWindowsConnection(connIdx) && !hasWindowsUnixLayerReady;

        QMenu menu(this);
        QAction* aConnect = menu.addAction(
            trk(QStringLiteral("t_connect_ctx_001"),
                QStringLiteral("Conectar"),
                QStringLiteral("Connect"),
                QStringLiteral("连接")));
        QAction* aDisconnect = menu.addAction(
            trk(QStringLiteral("t_disconnect_ctx001"),
                QStringLiteral("Desconectar"),
                QStringLiteral("Disconnect"),
                QStringLiteral("断开连接")));
        QAction* aInstallMsys = menu.addAction(
            trk(QStringLiteral("t_install_msys_ctx001"),
                QStringLiteral("Instalar MSYS2"),
                QStringLiteral("Install MSYS2"),
                QStringLiteral("安装 MSYS2")));
        menu.addSeparator();
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
        aConnect->setEnabled(!actionsLocked() && hasConn && isDisconnected);
        aDisconnect->setEnabled(!actionsLocked() && hasConn && !isDisconnected);
        aInstallMsys->setEnabled(canInstallMsys);
        aRefresh->setEnabled(canRefresh);
        aEdit->setEnabled(canEditDelete);
        aDelete->setEnabled(canEditDelete);
        aRefreshAll->setEnabled(!actionsLocked());
        aNewConn->setEnabled(!actionsLocked());
        aNewPool->setEnabled(!actionsLocked() && hasConn && !isDisconnected);

        QAction* chosen = menu.exec(m_connectionsTable->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if (chosen == aConnect && hasConn) {
            logUiAction(QStringLiteral("Conectar conexión (menú conexiones)"));
            setConnectionDisconnected(connIdx, false);
            appLog(QStringLiteral("NORMAL"), QStringLiteral("Conexión marcada como conectada: %1").arg(m_profiles[connIdx].name));
            rebuildConnectionsTable();
            populateAllPoolsTables();
            refreshConnectionByIndex(connIdx);
        } else if (chosen == aDisconnect && hasConn) {
            setConnectionDisconnected(connIdx, true);
            appLog(QStringLiteral("NORMAL"), QStringLiteral("Conexión marcada como desconectada: %1").arg(m_profiles[connIdx].name));
            rebuildConnectionsTable();
            populateAllPoolsTables();
        } else if (chosen == aRefresh) {
            logUiAction(QStringLiteral("Refrescar conexión (menú conexiones)"));
            refreshSelectedConnection();
        } else if (chosen == aEdit) {
            logUiAction(QStringLiteral("Editar conexión (menú conexiones)"));
            editConnection();
        } else if (chosen == aDelete) {
            logUiAction(QStringLiteral("Borrar conexión (menú conexiones)"));
            deleteConnection();
        } else if (chosen == aInstallMsys) {
            logUiAction(QStringLiteral("Instalar MSYS2 (menÃº conexiones)"));
            installMsysForSelectedConnection();
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
            int sourceOrder{-1};
            QString label;
            QString propKey;
            QLineEdit* line{nullptr};
            QComboBox* combo{nullptr};
            QCheckBox* inherit{nullptr};
            QWidget* rowWidget{nullptr};
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
        dlg.resize(860, 520);
        auto* root = new QVBoxLayout(&dlg);
        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(6);

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
            rowL->setSpacing(6);
            EditField ef;
            ef.row = r;
            ef.sourceOrder = fields.size();
            ef.label = pk->text();
            ef.propKey = prop;
            ef.rowWidget = rowW;

            if (QComboBox* srcCb = qobject_cast<QComboBox*>(m_connContentPropsTable->cellWidget(r, 1))) {
                auto* cb = new QComboBox(rowW);
                for (int i = 0; i < srcCb->count(); ++i) {
                    cb->addItem(srcCb->itemText(i));
                }
                cb->setCurrentText(srcCb->currentText());
                cb->setEnabled(valueEditable);
                ef.combo = cb;
                rowL->addWidget(cb, 0);
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
            rowL->addStretch(1);

            fields.push_back(ef);
        }

        auto fieldPriority = [](const EditField& ef) -> int {
            const QString key = ef.propKey.trimmed().toLower();
            if (key == QStringLiteral("dataset") || key == QStringLiteral("name") || key == QStringLiteral("nombre")) {
                return 0;
            }
            if (key == QStringLiteral("mountpoint")) {
                return 1;
            }
            return 100 + ef.sourceOrder;
        };
        std::stable_sort(fields.begin(), fields.end(), [fieldPriority](const EditField& a, const EditField& b) {
            return fieldPriority(a) < fieldPriority(b);
        });

        QFontMetrics fm(dlg.font());
        int maxLabelWidth = 0;
        int maxEditorText = fm.horizontalAdvance(ctx.datasetName);
        for (const EditField& ef : fields) {
            maxLabelWidth = qMax(maxLabelWidth, fm.horizontalAdvance(ef.label));
            if (ef.combo) {
                maxEditorText = qMax(maxEditorText, fm.horizontalAdvance(ef.combo->currentText()));
                for (int i = 0; i < ef.combo->count(); ++i) {
                    maxEditorText = qMax(maxEditorText, fm.horizontalAdvance(ef.combo->itemText(i)));
                }
            } else if (ef.line) {
                maxEditorText = qMax(maxEditorText, fm.horizontalAdvance(ef.line->text()));
            }
        }
        const int editorWidth = qBound(240, maxEditorText + 40, 560);
        const int labelWidth = qBound(90, maxLabelWidth + 10, 260);
        const int columns = 2;
        for (int i = 0; i < fields.size(); ++i) {
            const int r = i / columns;
            const int block = i % columns;
            const int labelCol = block * 2;
            const int valueCol = labelCol + 1;
            auto* lbl = new QLabel(fields[i].label, &dlg);
            lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            lbl->setMinimumWidth(labelWidth);
            lbl->setMaximumWidth(labelWidth);
            lbl->setStyleSheet(QStringLiteral("QLabel { background: #eef4f8; color: #102233; padding: 2px 6px; }"));
            if (!fields[i].rowWidget) {
                continue;
            }
            if (fields[i].line) {
                fields[i].line->setMinimumWidth(editorWidth);
                fields[i].line->setMaximumWidth(editorWidth);
            }
            if (fields[i].combo) {
                fields[i].combo->setMinimumWidth(editorWidth);
                fields[i].combo->setMaximumWidth(editorWidth);
            }
            grid->addWidget(lbl, r, labelCol);
            grid->addWidget(fields[i].rowWidget, r, valueCol);
            grid->setColumnStretch(labelCol, 0);
            grid->setColumnStretch(valueCol, 1);
        }
        root->addLayout(grid, 1);
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
        connect(m_connContentTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
            if (!m_connContentTree || !item) {
                return;
            }
            const bool isLazyPropsNode =
                item->data(0, kConnPropGroupNodeRole).toBool()
                && item->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                && item->text(0).trimmed() == trk(QStringLiteral("t_props_lbl_001"),
                                                  QStringLiteral("Propiedades"),
                                                  QStringLiteral("Properties"),
                                                  QStringLiteral("属性"));
            if (isLazyPropsNode) {
                QTimer::singleShot(0, this, [this, tree = m_connContentTree, item]() {
                    if (!tree || !item) {
                        return;
                    }
                    QTreeWidgetItem* owner = item->parent();
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
                    m_connContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
                    if (item->childCount() == 0) {
                        refreshDatasetProperties(QStringLiteral("conncontent"));
                    }
                    item->setExpanded(true);
                    QTimer::singleShot(0, tree, [item]() {
                        if (item) {
                            item->setExpanded(true);
                        }
                    });
                    m_connContentToken = prevToken;
                });
                return;
            }
            if (!item->data(0, kConnPermissionsNodeRole).toBool()
                || item->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("root")) {
                return;
            }
            QTreeWidgetItem* owner = item->parent();
            if (!owner) {
                return;
            }
            const bool wasEmpty = (item->childCount() == 0);
            QTimer::singleShot(0, this, [this, tree = m_connContentTree, owner, item, wasEmpty]() {
                if (!tree || !owner || !item) {
                    return;
                }
                if (wasEmpty && item->childCount() == 0) {
                    populateDatasetPermissionsNode(tree, owner, false);
                }
                if (wasEmpty) {
                    item->setExpanded(true);
                } else {
                    item->setExpanded(!item->isExpanded());
                }
            });
        });
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
            const bool isGroupNode = sel && sel->data(0, kConnPropGroupNodeRole).toBool();
            const bool isHoldItem = sel && sel->data(0, kConnSnapshotHoldItemRole).toBool();
            const bool isPermissionsNode = sel && sel->data(0, kConnPermissionsNodeRole).toBool();
            const bool isPoolContext =
                sel && (sel->data(0, kIsPoolRootRole).toBool() || isInfoNodeOrInside(sel));
            const bool isLazyPropsNode =
                sel && isGroupNode && !isPoolContext
                && sel->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                && sel->text(0).trimmed() == trk(QStringLiteral("t_props_lbl_001"),
                                                 QStringLiteral("Propiedades"),
                                                 QStringLiteral("Properties"),
                                                 QStringLiteral("属性"))
                && sel->childCount() == 0;
            const bool isLazyPermissionsNode =
                sel && isPermissionsNode
                && sel->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
                && sel->childCount() == 0;
            if ((isPropRow || isGroupNode || isHoldItem || isPermissionsNode)
                && !isPoolContext && !isLazyPropsNode && !isLazyPermissionsNode) {
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
                if (isLazyPermissionsNode) {
                    populateDatasetPermissionsNode(m_connContentTree, owner, false);
                    if (sel) {
                        sel->setExpanded(true);
                    }
                } else {
                    refreshDatasetProperties(QStringLiteral("conncontent"));
                    if (isLazyPropsNode && sel) {
                        sel->setExpanded(true);
                        QTimer::singleShot(0, m_connContentTree, [sel]() {
                            if (sel) {
                                sel->setExpanded(true);
                            }
                        });
                    }
                    syncConnContentPropertyColumns();
                }
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
        connect(m_connContentTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
            if (!m_connContentTree || !item) {
                return;
            }
            resizeTreeColumnsToVisibleContent(m_connContentTree);
            if (!item->data(0, kConnPermissionsNodeRole).toBool()) {
                return;
            }
            if (item->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root") && item->childCount() == 0) {
                if (QTreeWidgetItem* owner = item->parent()) {
                    populateDatasetPermissionsNode(m_connContentTree, owner, false);
                    resizeTreeColumnsToVisibleContent(m_connContentTree);
                }
            }
        });
        connect(m_connContentTree, &QWidget::customContextMenuRequested, this,
                [this, applyInlineSectionVisibility, manageInlinePropsVisualization, createSnapshotHold, releaseSnapshotHold,
                 executeDatasetActionWithStdin, promptNewPassphrase, permissionNodeItem,
                 permissionOwnerItem, refreshPermissionsTreeNode, promptPermissionGrant,
                 promptPermissionSet, permissionTokensFromNode,
                 executePermissionCommand, scopeFlagsForPermission, targetFlagsForPermission](const QPoint& pos) {
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
                item = m_connContentTree->currentItem();
                if (!item) {
                    return;
                }
            }
            updateConnectionActionsState();

            if (QTreeWidgetItem* permNode = permissionNodeItem(item)) {
                QTreeWidgetItem* owner = permissionOwnerItem(item);
                if (!owner) {
                    return;
                }
                const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (!ctx.valid || !ctx.snapshotName.isEmpty() || isWindowsConnection(ctx.connIdx)) {
                    return;
                }
                if (permNode->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
                    && permNode->childCount() == 0) {
                    populateDatasetPermissionsNode(m_connContentTree, owner, false);
                }
                QMenu permMenu(this);
                const QString kind = permNode->data(0, kConnPermissionsKindRole).toString();
                QAction* aRefreshPerms = permMenu.addAction(QStringLiteral("Refrescar permisos"));
                QAction* aNewGrant = permMenu.addAction(QStringLiteral("Nueva delegación"));
                QAction* aNewSet = permMenu.addAction(QStringLiteral("Nuevo set de permisos"));
                QAction* aEditGrant = nullptr;
                QAction* aDeleteGrant = nullptr;
                QAction* aRenameSet = nullptr;
                QAction* aDeleteSet = nullptr;
                if (kind == QStringLiteral("grant") || kind == QStringLiteral("grant_perm")) {
                    permMenu.addSeparator();
                    aEditGrant = permMenu.addAction(QStringLiteral("Editar delegación"));
                    aDeleteGrant = permMenu.addAction(QStringLiteral("Eliminar delegación"));
                } else if (kind == QStringLiteral("set") || kind == QStringLiteral("set_perm")) {
                    permMenu.addSeparator();
                    aRenameSet = permMenu.addAction(QStringLiteral("Renombrar conjunto de permisos"));
                    aDeleteSet = permMenu.addAction(QStringLiteral("Eliminar set"));
                }
                QAction* picked = permMenu.exec(m_connContentTree->viewport()->mapToGlobal(pos));
                if (!picked) {
                    return;
                }
                if (picked == aRefreshPerms) {
                    refreshPermissionsTreeNode(m_connContentTree, item, true);
                    return;
                }
                if (picked == aNewGrant) {
                    QString targetType;
                    QString targetName;
                    QString scope;
                    QStringList tokens;
                    if (!promptPermissionGrant(ctx,
                                               QStringLiteral("Nueva delegación"),
                                               QString(),
                                               QString(),
                                               QString(),
                                               {},
                                               false,
                                               targetType,
                                               targetName,
                                               scope,
                                               tokens)) {
                        return;
                    }
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it == m_datasetPermissionsCache.end()) {
                        return;
                    }
                    bool exists = false;
                    auto appendPending = [&](QVector<DatasetPermissionGrant>& grants) {
                        for (const DatasetPermissionGrant& g : grants) {
                            if (g.scope == scope && g.targetType == targetType && g.targetName == targetName) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            DatasetPermissionGrant g;
                            g.scope = scope;
                            g.targetType = targetType;
                            g.targetName = targetName;
                            g.pending = true;
                            grants.push_back(g);
                            it->dirty = true;
                            exists = true;
                        }
                    };
                    if (scope == QStringLiteral("local")) {
                        appendPending(it->localGrants);
                    } else if (scope == QStringLiteral("descendant")) {
                        appendPending(it->descendantGrants);
                    } else {
                        appendPending(it->localDescendantGrants);
                    }
                    populateDatasetPermissionsNode(m_connContentTree, owner, false);
                    updateApplyPropsButtonState();
                    for (int i = 0; i < owner->childCount(); ++i) {
                        QTreeWidgetItem* child = owner->child(i);
                        if (!child || !child->data(0, kConnPermissionsNodeRole).toBool()
                            || child->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("root")) {
                            continue;
                        }
                        child->setExpanded(true);
                        for (int j = 0; j < child->childCount(); ++j) {
                            QTreeWidgetItem* sec = child->child(j);
                            if (!sec || sec->data(0, kConnPermissionsKindRole).toString() != QStringLiteral("grants_root")) {
                                continue;
                            }
                            sec->setExpanded(true);
                            for (int k = 0; k < sec->childCount(); ++k) {
                                QTreeWidgetItem* grantNode = sec->child(k);
                                if (!grantNode) {
                                    continue;
                                }
                                if (grantNode->data(0, kConnPermissionsScopeRole).toString() == scope
                                    && grantNode->data(0, kConnPermissionsTargetTypeRole).toString() == targetType
                                    && grantNode->data(0, kConnPermissionsTargetNameRole).toString() == targetName) {
                                    grantNode->setExpanded(true);
                                    m_connContentTree->setCurrentItem(grantNode);
                                    break;
                                }
                            }
                        }
                    }
                    return;
                }
                if (picked == aEditGrant) {
                    QTreeWidgetItem* grantNode = permNode;
                    if (kind == QStringLiteral("grant_perm") && permNode->parent()) {
                        grantNode = permNode->parent();
                    }
                    QString targetType = grantNode->data(0, kConnPermissionsTargetTypeRole).toString();
                    QString targetName = grantNode->data(0, kConnPermissionsTargetNameRole).toString();
                    QString scope = grantNode->data(0, kConnPermissionsScopeRole).toString();
                    QStringList tokens = permissionTokensFromNode(grantNode);
                    QString newTargetType;
                    QString newTargetName;
                    QString newScope;
                    QStringList newTokens;
                    if (!promptPermissionGrant(ctx,
                                               QStringLiteral("Editar delegación"),
                                               targetType,
                                               targetName,
                                               scope,
                                               tokens,
                                               false,
                                               newTargetType,
                                               newTargetName,
                                               newScope,
                                               newTokens)) {
                        return;
                    }
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it != m_datasetPermissionsCache.end()) {
                        auto updateGrant = [&](QVector<DatasetPermissionGrant>& grants) {
                            for (DatasetPermissionGrant& g : grants) {
                                if (g.scope == scope && g.targetType == targetType && g.targetName == targetName) {
                                    g.scope = newScope;
                                    g.targetType = newTargetType;
                                    g.targetName = newTargetName;
                                    it->dirty = true;
                                    return true;
                                }
                            }
                            return false;
                        };
                        if (updateGrant(it->localGrants) || updateGrant(it->descendantGrants)
                            || updateGrant(it->localDescendantGrants)) {
                            populateDatasetPermissionsNode(m_connContentTree, owner, false);
                            updateApplyPropsButtonState();
                        }
                    }
                    return;
                }
                if (picked == aNewSet) {
                    QString setName;
                    QStringList tokens;
                    if (!promptPermissionSet(ctx,
                                             QStringLiteral("Nuevo set de permisos"),
                                             QString(),
                                             {},
                                             false,
                                             setName,
                                             tokens)) {
                        return;
                    }
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it != m_datasetPermissionsCache.end()) {
                        bool exists = false;
                        for (const DatasetPermissionSet& s : it->permissionSets) {
                            if (s.name.compare(setName, Qt::CaseInsensitive) == 0) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            DatasetPermissionSet s;
                            s.name = setName;
                            s.permissions = tokens;
                            it->permissionSets.push_back(s);
                            it->dirty = true;
                            populateDatasetPermissionsNode(m_connContentTree, owner, false);
                            updateApplyPropsButtonState();
                        }
                    }
                    return;
                }
                if (picked == aRenameSet) {
                    QTreeWidgetItem* setNode = permNode;
                    if (kind == QStringLiteral("set_perm") && permNode->parent()) {
                        setNode = permNode->parent();
                    }
                    const QString oldSetName = setNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                    const QStringList tokens = permissionTokensFromNode(setNode);
                    bool ok = false;
                    QString newSetName = QInputDialog::getText(
                        this,
                        QStringLiteral("Renombrar conjunto de permisos"),
                        QStringLiteral("Nuevo nombre"),
                        QLineEdit::Normal,
                        oldSetName,
                        &ok).trimmed();
                    if (!ok || newSetName.isEmpty()) {
                        return;
                    }
                    if (!newSetName.startsWith(QLatin1Char('@'))) {
                        newSetName.prepend(QLatin1Char('@'));
                    }
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it != m_datasetPermissionsCache.end()) {
                        for (DatasetPermissionSet& s : it->permissionSets) {
                            if (s.name.compare(oldSetName, Qt::CaseInsensitive) == 0) {
                                s.name = newSetName;
                                it->dirty = true;
                                break;
                            }
                        }
                        populateDatasetPermissionsNode(m_connContentTree, owner, false);
                        updateApplyPropsButtonState();
                    }
                    return;
                }
                if (picked == aDeleteGrant) {
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it != m_datasetPermissionsCache.end()) {
                        auto removeGrant = [&](QVector<DatasetPermissionGrant>& grants) {
                            for (int i = grants.size() - 1; i >= 0; --i) {
                                const DatasetPermissionGrant& g = grants.at(i);
                                if (g.scope == permNode->data(0, kConnPermissionsScopeRole).toString()
                                    && g.targetType == permNode->data(0, kConnPermissionsTargetTypeRole).toString()
                                    && g.targetName == permNode->data(0, kConnPermissionsTargetNameRole).toString()) {
                                    grants.removeAt(i);
                                    it->dirty = true;
                                }
                            }
                        };
                        removeGrant(it->localGrants);
                        removeGrant(it->descendantGrants);
                        removeGrant(it->localDescendantGrants);
                        populateDatasetPermissionsNode(m_connContentTree, owner, false);
                        updateApplyPropsButtonState();
                    }
                    return;
                }
                if (picked == aDeleteSet) {
                    QString setName = permNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                    if (kind == QStringLiteral("set_perm") && permNode->parent()) {
                        setName = permNode->parent()->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                    }
                    const QString cacheKey = datasetPermissionsCacheKey(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    auto it = m_datasetPermissionsCache.find(cacheKey);
                    if (it != m_datasetPermissionsCache.end()) {
                        for (int i = it->permissionSets.size() - 1; i >= 0; --i) {
                            if (it->permissionSets.at(i).name.compare(setName, Qt::CaseInsensitive) == 0) {
                                it->permissionSets.removeAt(i);
                                it->dirty = true;
                            }
                        }
                        populateDatasetPermissionsNode(m_connContentTree, owner, false);
                        updateApplyPropsButtonState();
                    }
                    return;
                }
                return;
            }

            QMenu menu(this);
            auto deleteLabelForItem = [this](QTreeWidgetItem* targetItem) {
                const DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("conncontent"));
                QTreeWidgetItem* owner = targetItem;
                while (owner) {
                    const QString ownerDatasetName = owner->data(0, Qt::UserRole).toString().trimmed();
                    const QString ownerSnapshotName = owner->data(1, Qt::UserRole).toString().trimmed();
                    if (!ownerDatasetName.isEmpty() || !ownerSnapshotName.isEmpty()
                        || owner->data(0, kIsPoolRootRole).toBool()) {
                        break;
                    }
                    owner = owner->parent();
                }
                const QString snapshotName = owner ? owner->data(1, Qt::UserRole).toString().trimmed() : QString();
                const QString datasetName = owner ? owner->data(0, Qt::UserRole).toString().trimmed() : QString();
                if (!snapshotName.isEmpty() && !datasetName.isEmpty()) {
                    return QStringLiteral("%1 %2@%3").arg(
                        trk(QStringLiteral("t_ctx_delete_snapshot001"),
                            QStringLiteral("Borrar Snapshot")),
                        datasetName,
                        snapshotName);
                }
                if (ctx.valid && !datasetName.isEmpty()) {
                    const auto it = m_poolDatasetCache.constFind(datasetCacheKey(ctx.connIdx, ctx.poolName));
                    if (it != m_poolDatasetCache.cend()) {
                        const auto recIt = it->recordByName.constFind(datasetName);
                        if (recIt != it->recordByName.cend()) {
                            const DatasetRecord& rec = recIt.value();
                            if (rec.mounted.trimmed() == QStringLiteral("-")
                                && rec.mountpoint.trimmed() == QStringLiteral("-")) {
                                return QStringLiteral("%1 %2").arg(
                                    trk(QStringLiteral("t_ctx_delete_zvol001"),
                                        QStringLiteral("Borrar ZVol")),
                                    datasetName);
                            }
                        }
                    }
                    return QStringLiteral("%1 %2").arg(
                        trk(QStringLiteral("t_ctx_delete_dataset001"),
                            QStringLiteral("Borrar Dataset")),
                        datasetName);
                }
                return trk(QStringLiteral("t_ctx_delete_dataset001"),
                           QStringLiteral("Borrar Dataset"));
            };
            const bool isPoolRoot = item->data(0, kIsPoolRootRole).toBool();
            auto isInfoNodeOrInside = [](QTreeWidgetItem* n) -> bool {
                for (QTreeWidgetItem* p = n; p; p = p->parent()) {
                    if (p->data(0, kConnPropKeyRole).toString() == QString::fromLatin1(kPoolBlockInfoKey)) {
                        return true;
                    }
                }
                return false;
            };
            const bool isPoolInfoContext = isInfoNodeOrInside(item);
            const bool isSnapshotHoldContext = item->data(0, kConnSnapshotHoldItemRole).toBool()
                                               || (item->parent() && item->parent()->data(0, kConnSnapshotHoldItemRole).toBool());
            QTreeWidgetItem* holdContextItem = item;
            if (holdContextItem && !holdContextItem->data(0, kConnSnapshotHoldItemRole).toBool()) {
                holdContextItem = holdContextItem->parent();
            }
            const QString holdContextName =
                holdContextItem ? holdContextItem->data(0, kConnSnapshotHoldTagRole).toString().trimmed() : QString();
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
                const bool canHistory = canExport;
                const bool canSync = canExport;
                const bool canScrub = canExport;
                const bool canTrim = canExport;
                const bool canInitialize = canExport;
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
                QAction* aHistory = menu.addAction(
                    trk(QStringLiteral("t_pool_history_t1"),
                        QStringLiteral("Historial")));
                QAction* aSync = menu.addAction(QStringLiteral("Sync"));
                QAction* aScrub = menu.addAction(QStringLiteral("Scrub"));
                QAction* aTrim = menu.addAction(QStringLiteral("Trim"));
                QAction* aInitialize = menu.addAction(QStringLiteral("Initialize"));
                QAction* aDestroy = menu.addAction(QStringLiteral("Destroy"));
                menu.addSeparator();
                QAction* aShowPoolInfo = menu.addAction(QStringLiteral("Mostrar Información del pool"));
                aShowPoolInfo->setCheckable(true);
                aShowPoolInfo->setChecked(m_showPoolInfoNode);
                aUpdate->setEnabled(canRefresh);
                aImport->setEnabled(canImport);
                aExport->setEnabled(canExport);
                aHistory->setEnabled(canHistory);
                aSync->setEnabled(canSync);
                aScrub->setEnabled(canScrub);
                aTrim->setEnabled(canTrim);
                aInitialize->setEnabled(canInitialize);
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
                } else if (picked == aHistory && canHistory && poolRow >= 0) {
                    showPoolHistoryFromRow(poolRow);
                } else if (picked == aSync && canSync && poolRow >= 0) {
                    syncPoolFromRow(poolRow);
                } else if (picked == aScrub && canScrub && poolRow >= 0) {
                    scrubPoolFromRow(poolRow);
                } else if (picked == aTrim && canTrim && poolRow >= 0) {
                    trimPoolFromRow(poolRow);
                } else if (picked == aInitialize && canInitialize && poolRow >= 0) {
                    initializePoolFromRow(poolRow);
                } else if (picked == aDestroy && canDestroy && poolRow >= 0) {
                    destroyPoolFromRow(poolRow);
                } else if (picked == aShowPoolInfo) {
                    m_showPoolInfoNode = aShowPoolInfo->isChecked();
                    applyInlineSectionVisibility();
                }
                return;
            }
            if (isPoolInfoContext) {
                QAction* aManage = menu.addAction(
                    trk(QStringLiteral("t_manage_props_vis001"),
                        QStringLiteral("Gestionar visualización de propiedades")));
                QAction* aShowPoolInfo = menu.addAction(QStringLiteral("Mostrar Información del pool"));
                aShowPoolInfo->setCheckable(true);
                aShowPoolInfo->setChecked(m_showPoolInfoNode);
                QAction* aShowInlineProps = menu.addAction(QStringLiteral("Mostrar propiedades en línea"));
                aShowInlineProps->setCheckable(true);
                aShowInlineProps->setChecked(m_showInlinePropertyNodes);
                QAction* aShowInlinePerms = menu.addAction(QStringLiteral("Mostrar Permisos en línea"));
                aShowInlinePerms->setCheckable(true);
                aShowInlinePerms->setChecked(m_showInlinePermissionsNodes);
                QAction* picked = menu.exec(m_connContentTree->viewport()->mapToGlobal(pos));
                if (picked == aManage) {
                    manageInlinePropsVisualization(m_connContentTree, item, true);
                } else if (picked == aShowPoolInfo) {
                    m_showPoolInfoNode = aShowPoolInfo->isChecked();
                    applyInlineSectionVisibility();
                } else if (picked == aShowInlineProps) {
                    m_showInlinePropertyNodes = aShowInlineProps->isChecked();
                    applyInlineSectionVisibility();
                } else if (picked == aShowInlinePerms) {
                    m_showInlinePermissionsNodes = aShowInlinePerms->isChecked();
                    applyInlineSectionVisibility();
                }
                return;
            }

            QAction* aManage = menu.addAction(
                trk(QStringLiteral("t_manage_props_vis001"),
                    QStringLiteral("Gestionar visualización de propiedades")));
            QAction* aShowInlineProps = menu.addAction(QStringLiteral("Mostrar propiedades en línea"));
            aShowInlineProps->setCheckable(true);
            aShowInlineProps->setChecked(m_showInlinePropertyNodes);
            QAction* aShowInlinePerms = menu.addAction(QStringLiteral("Mostrar Permisos en línea"));
            aShowInlinePerms->setCheckable(true);
            aShowInlinePerms->setChecked(m_showInlinePermissionsNodes);
            menu.addSeparator();
            QAction* aCreate = menu.addAction(
                trk(QStringLiteral("t_ctx_create_dsv001"),
                    QStringLiteral("Crear dataset/snapshot/vol"),
                    QStringLiteral("Create dataset/snapshot/vol"),
                    QStringLiteral("创建 dataset/snapshot/vol")));
            QAction* aDelete = menu.addAction(
                deleteLabelForItem(item));
            QMenu* mEncryption = menu.addMenu(QStringLiteral("Encriptación"));
            QAction* aLoadKey = mEncryption->addAction(QStringLiteral("Load key"));
            QAction* aUnloadKey = mEncryption->addAction(QStringLiteral("Unload key"));
            QAction* aChangeKey = mEncryption->addAction(QStringLiteral("Change key"));
            menu.addSeparator();
            QMenu* mSelectSnapshot = menu.addMenu(
                trk(QStringLiteral("t_ctx_sel_snap001"),
                    QStringLiteral("Seleccionar snapshot"),
                    QStringLiteral("Select snapshot"),
                    QStringLiteral("选择快照")));
            QMap<QAction*, QString> snapshotActions;
            const QString itemDatasetPath = item->data(0, Qt::UserRole).toString().trimmed();
            {
                const QStringList snaps = item->data(1, Qt::UserRole + 1).toStringList();
                const QString currentSnap = item->data(1, Qt::UserRole).toString().trimmed();
                QAction* noneAct = mSelectSnapshot->addAction(QStringLiteral("(ninguno)"));
                noneAct->setCheckable(true);
                noneAct->setChecked(currentSnap.isEmpty());
                snapshotActions.insert(noneAct, QString());
                if (!snaps.isEmpty()) {
                    mSelectSnapshot->addSeparator();
                }
                for (const QString& s : snaps) {
                    const QString snapName = s.trimmed();
                    if (snapName.isEmpty()) {
                        continue;
                    }
                    QAction* sa = mSelectSnapshot->addAction(snapName);
                    sa->setCheckable(true);
                    sa->setChecked(snapName == currentSnap);
                    snapshotActions.insert(sa, snapName);
                }
            }
            mSelectSnapshot->setEnabled(!actionsLocked() && !snapshotActions.isEmpty());
            QAction* aRollback = menu.addAction(QStringLiteral("Rollback"));
            QAction* aNewHold = menu.addAction(
                trk(QStringLiteral("t_new_hold_title001"),
                    QStringLiteral("Nuevo Hold")));
            QAction* aReleaseHold = menu.addAction(
                holdContextName.isEmpty()
                    ? trk(QStringLiteral("t_release_hold_title001"),
                          QStringLiteral("Release"))
                    : QStringLiteral("%1 %2").arg(
                          trk(QStringLiteral("t_release_hold_title001"),
                              QStringLiteral("Release")),
                          holdContextName));
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
            auto datasetPropFromCache = [this](const DatasetSelectionContext& c, const QString& prop) -> QString {
                if (!c.valid || c.datasetName.isEmpty() || !c.snapshotName.isEmpty()) {
                    return QString();
                }
                const auto itCache = m_datasetPropsCache.constFind(
                    datasetPropsCacheKey(c.connIdx, c.poolName, c.datasetName));
                if (itCache == m_datasetPropsCache.cend() || !itCache->loaded) {
                    return QString();
                }
                for (const DatasetPropCacheRow& row : itCache->rows) {
                    if (row.prop.compare(prop, Qt::CaseInsensitive) == 0) {
                        return row.value.trimmed();
                    }
                }
                return QString();
            };
            const QString encryptionRoot = datasetPropFromCache(ctx, QStringLiteral("encryptionroot"));
            const QString keyStatus = datasetPropFromCache(ctx, QStringLiteral("keystatus")).toLower();
            const QString keyLocation = datasetPropFromCache(ctx, QStringLiteral("keylocation")).toLower();
            const QString keyFormat = datasetPropFromCache(ctx, QStringLiteral("keyformat")).toLower();
            const bool isEncryptionRoot =
                hasConnSel && !hasConnSnap
                && !encryptionRoot.isEmpty()
                && encryptionRoot.compare(ctx.datasetName, Qt::CaseInsensitive) == 0;
            const bool keyLoaded = (keyStatus == QStringLiteral("available"));
            aLoadKey->setEnabled(isEncryptionRoot && !keyLoaded);
            aUnloadKey->setEnabled(isEncryptionRoot && keyLoaded);
            aChangeKey->setEnabled(isEncryptionRoot && keyLoaded);
            mEncryption->setEnabled(isEncryptionRoot);
            aManage->setEnabled(hasConnSel);
            aRollback->setEnabled(!actionsLocked() && hasConnSnap);
            aCreate->setEnabled(!actionsLocked() && hasConnSel && !hasConnSnap);
            aNewHold->setEnabled(!actionsLocked() && hasConnSnap);
            aReleaseHold->setEnabled(!actionsLocked() && hasConnSnap && isSnapshotHoldContext);
            aDelete->setEnabled(!actionsLocked() && hasConnSel);
            aBreakdown->setEnabled(m_btnConnBreakdown && m_btnConnBreakdown->isEnabled());
            aAssemble->setEnabled(m_btnConnAssemble && m_btnConnAssemble->isEnabled());
            aFromDir->setEnabled(m_btnConnFromDir && m_btnConnFromDir->isEnabled());
            aToDir->setEnabled(m_btnConnToDir && m_btnConnToDir->isEnabled());

            QAction* picked = menu.exec(m_connContentTree->viewport()->mapToGlobal(pos));
            if (!picked) {
                return;
            }
            if (snapshotActions.contains(picked)) {
                const QString snapName = snapshotActions.value(picked);
                QTreeWidgetItem* targetItem = item;
                if (!itemDatasetPath.isEmpty()) {
                    auto findInTree = [](QTreeWidget* tw, const QString& ds) -> QTreeWidgetItem* {
                        if (!tw || ds.isEmpty()) {
                            return nullptr;
                        }
                        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
                            if (!n) {
                                return nullptr;
                            }
                            if (n->data(0, Qt::UserRole).toString().trimmed() == ds) {
                                return n;
                            }
                            for (int i = 0; i < n->childCount(); ++i) {
                                if (QTreeWidgetItem* f = rec(n->child(i))) {
                                    return f;
                                }
                            }
                            return nullptr;
                        };
                        for (int i = 0; i < tw->topLevelItemCount(); ++i) {
                            if (QTreeWidgetItem* f = rec(tw->topLevelItem(i))) {
                                return f;
                            }
                        }
                        return nullptr;
                    };
                    if (QTreeWidgetItem* found = findInTree(m_connContentTree, itemDatasetPath)) {
                        targetItem = found;
                    }
                }
                if (!targetItem) {
                    return;
                }
                onSnapshotComboChanged(
                    m_connContentTree,
                    targetItem,
                    QStringLiteral("conncontent"),
                    snapName.isEmpty() ? QStringLiteral("(ninguno)") : snapName);
                const DatasetSelectionContext sctx = currentDatasetSelection(QStringLiteral("conncontent"));
                if (sctx.valid && !sctx.datasetName.isEmpty()) {
                    setConnectionOriginSelection(sctx);
                } else {
                    setConnectionOriginSelection(DatasetSelectionContext{});
                }
                return;
            }
            if (picked == aManage) {
                manageInlinePropsVisualization(m_connContentTree, item, false);
                return;
            }
            if (picked == aShowInlineProps) {
                m_showInlinePropertyNodes = aShowInlineProps->isChecked();
                applyInlineSectionVisibility();
                return;
            }
            if (picked == aShowInlinePerms) {
                m_showInlinePermissionsNodes = aShowInlinePerms->isChecked();
                applyInlineSectionVisibility();
                return;
            }
            if (picked == aNewHold) {
                createSnapshotHold(m_connContentTree, item);
                return;
            }
            if (picked == aReleaseHold) {
                releaseSnapshotHold(m_connContentTree, item);
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
            } else if (picked == aLoadKey || picked == aUnloadKey || picked == aChangeKey) {
                auto shQuote = [](QString s) {
                    s.replace('\'', QStringLiteral("'\"'\"'"));
                    return QStringLiteral("'%1'").arg(s);
                };
                QString actionName;
                QString cmd;
                QByteArray stdinPayload;
                if (picked == aLoadKey) {
                    actionName = QStringLiteral("Load key");
                    cmd = QStringLiteral("zfs load-key %1").arg(shQuote(ctx.datasetName));
                } else if (picked == aUnloadKey) {
                    actionName = QStringLiteral("Unload key");
                    cmd = QStringLiteral("zfs unload-key %1").arg(shQuote(ctx.datasetName));
                } else {
                    actionName = QStringLiteral("Change key");
                    cmd = QStringLiteral("zfs change-key -o keylocation=prompt%1 %2")
                              .arg(keyFormat == QStringLiteral("passphrase")
                                       ? QStringLiteral(" -o keyformat=passphrase")
                                       : QString(),
                                   shQuote(ctx.datasetName));
                }
                if (picked == aChangeKey) {
                    QString newPassphrase;
                    if (!promptNewPassphrase(actionName, newPassphrase)) {
                        return;
                    }
                    stdinPayload = (newPassphrase + QStringLiteral("\n") + newPassphrase + QStringLiteral("\n")).toUtf8();
                    executeDatasetActionWithStdin(QStringLiteral("conncontent"), actionName, ctx, cmd, stdinPayload, 90000);
                } else if (picked == aLoadKey && keyLocation == QStringLiteral("prompt")) {
                    bool ok = false;
                    const QString passphrase = QInputDialog::getText(
                        this,
                        actionName,
                        QStringLiteral("Clave"),
                        QLineEdit::Password,
                        QString(),
                        &ok);
                    if (!ok || passphrase.isEmpty()) {
                        return;
                    }
                    stdinPayload = (passphrase + QStringLiteral("\n")).toUtf8();
                    executeDatasetActionWithStdin(QStringLiteral("conncontent"), actionName, ctx, cmd, stdinPayload, 90000);
                } else {
                    executeDatasetAction(QStringLiteral("conncontent"), actionName, ctx, cmd, 90000);
                }
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
    connect(m_btnConnClone, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnClone) {
                logUiAction(QStringLiteral("Cancelar Clonar (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnClone;
        m_activeConnActionName = trk(QStringLiteral("t_clone_btn_001"), QStringLiteral("Clonar"));
        logUiAction(QStringLiteral("Clonar snapshot (botón Conexiones)"));
        executeConnectionTransferAction(QStringLiteral("clone"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnDiff, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            if (m_activeConnActionBtn == m_btnConnDiff) {
                logUiAction(QStringLiteral("Cancelar Diff (botón Conexiones)"));
                requestCancelRunningAction();
            }
            return;
        }
        m_activeConnActionBtn = m_btnConnDiff;
        m_activeConnActionName = trk(QStringLiteral("t_diff_btn_001"), QStringLiteral("Diff"));
        logUiAction(QStringLiteral("Diff snapshot (botón Conexiones)"));
        executeConnectionTransferAction(QStringLiteral("diff"));
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
