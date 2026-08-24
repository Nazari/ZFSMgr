#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "requests.h"
#include "mainwindow_connectiondatasettreedelegate.h"
#include "mainwindow_ui_logic.h"

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
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QCoreApplication>
#include <QListView>
#include <QListWidget>
#include <QPixmap>
#include <QTimer>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QPainterPath>
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
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOptionTab>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextBlock>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QPainter>

#ifndef ZFSMGR_APP_VERSION
#define ZFSMGR_APP_VERSION "0.10.0rc1"
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
constexpr int kConnPoolAutoSnapshotsNodeRole = Qt::UserRole + 34;
constexpr int kConnPoolAutoSnapshotsDatasetRole = Qt::UserRole + 35;
constexpr int kConnStatePartRole = Qt::UserRole + 44;
constexpr int kIsSplitRootRole = Qt::UserRole + 50;
constexpr char kPoolBlockInfoKey[] = "__pool_block_info__";




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

void paintConnectionSelectionOverlay(QPainter* painter,
                                     const QStyleOptionViewItem& option,
                                     const QModelIndex& index) {
    if (!painter || !index.isValid() || !(option.state & QStyle::State_Selected)) {
        return;
    }
    const QRect r = option.rect.adjusted(0, 0, -1, -1);
    const QColor overlay(58, 124, 210, 28);
    const QColor border(58, 124, 210, 170);
    painter->save();
    painter->fillRect(r, overlay);
    painter->setPen(border);
    painter->drawLine(r.topLeft(), r.topRight());
    painter->drawLine(r.bottomLeft(), r.bottomRight());
    if (index.column() == 0) {
        painter->drawLine(r.topLeft(), r.bottomLeft());
    }
    if (index.model() && index.column() == index.model()->columnCount() - 1) {
        painter->drawLine(r.topRight(), r.bottomRight());
    }
    painter->restore();
}

class ConnectionRowTextDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!painter || !index.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem viewOpt(option);
        initStyleOption(&viewOpt, index);
        const bool selected = viewOpt.state & QStyle::State_Selected;
        viewOpt.state &= ~QStyle::State_Selected;
        QStyledItemDelegate::paint(painter, viewOpt, index);
        if (selected) {
            paintConnectionSelectionOverlay(painter, option, index);
        }
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
        const bool selected = viewOpt.state & QStyle::State_Selected;
        viewOpt.state &= ~QStyle::State_Selected;
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
        if (selected) {
            paintConnectionSelectionOverlay(painter, option, index);
        }

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
            const Qt::CheckState cur = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
            const Qt::CheckState next = (cur == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            model->setData(index, next, Qt::CheckStateRole);
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

class LightCenteredCheckDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!painter || !index.isValid() || !(index.flags() & Qt::ItemIsUserCheckable)) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem viewOpt(option);
        initStyleOption(&viewOpt, index);
        const bool selected = viewOpt.state & QStyle::State_Selected;
        viewOpt.state &= ~QStyle::State_Selected;
        const QWidget* widget = viewOpt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();

        const QString savedText = viewOpt.text;
        viewOpt.text.clear();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &viewOpt, painter, widget);
        viewOpt.text = savedText;

        const bool enabled = index.flags() & Qt::ItemIsEnabled;
        const bool checked = (index.data(Qt::CheckStateRole).toInt() == Qt::Checked);
        const int boxSize = qMax(12, qMin(option.rect.width() - 8, option.rect.height() - 8));
        const QRect boxRect(option.rect.x() + (option.rect.width() - boxSize) / 2,
                            option.rect.y() + (option.rect.height() - boxSize) / 2,
                            boxSize,
                            boxSize);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        const QColor border = enabled ? QColor(125, 146, 166) : QColor(176, 186, 196);
        const QColor fill = enabled ? QColor(255, 255, 255) : QColor(243, 245, 247);
        painter->setPen(border);
        painter->setBrush(fill);
        painter->drawRect(boxRect.adjusted(0, 0, -1, -1));

        if (checked) {
            QPen tickPen(enabled ? QColor(33, 92, 151) : QColor(132, 149, 166), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter->setPen(tickPen);
            QPainterPath path;
            path.moveTo(boxRect.left() + boxRect.width() * 0.20, boxRect.top() + boxRect.height() * 0.55);
            path.lineTo(boxRect.left() + boxRect.width() * 0.42, boxRect.top() + boxRect.height() * 0.76);
            path.lineTo(boxRect.left() + boxRect.width() * 0.78, boxRect.top() + boxRect.height() * 0.26);
            painter->drawPath(path);
        }
        painter->restore();
        if (selected) {
            paintConnectionSelectionOverlay(painter, option, index);
        }

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
        CenteredCheckDelegate helper(parent());
        return helper.editorEvent(event, model, option, index);
    }
};

class ManagePropsCheckBelowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(index);
        const int width = qMax(140, option.fontMetrics.horizontalAdvance(QStringLiteral("secondarycache")) + 20);
        return QSize(width, 56);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!painter || !index.isValid()) {
            return;
        }

        QStyleOptionViewItem viewOpt(option);
        initStyleOption(&viewOpt, index);
        const QWidget* widget = viewOpt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();

        const QString label = index.data(Qt::UserRole + 1).toString().trimmed().isEmpty()
                                  ? index.data(Qt::DisplayRole).toString()
                                  : index.data(Qt::UserRole + 1).toString().trimmed();
        const bool enabled = index.flags() & Qt::ItemIsEnabled;
        const bool checked = (index.data(Qt::CheckStateRole).toInt() == Qt::Checked);

        QStyleOptionViewItem panelOpt(viewOpt);
        panelOpt.text.clear();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &panelOpt, painter, widget);

        painter->save();
        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect.adjusted(2, 2, -2, -2), QColor(223, 237, 250));
        }
        painter->setPen(enabled ? QColor(16, 34, 51) : QColor(128, 138, 148));
        const QRect textRect = option.rect.adjusted(6, 4, -6, -22);
        painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, label);
        painter->restore();

        QStyleOptionButton cbOpt;
        cbOpt.state = QStyle::State_None;
        if (enabled) {
            cbOpt.state |= QStyle::State_Enabled;
        }
        cbOpt.state |= checked ? QStyle::State_On : QStyle::State_Off;
        const QRect checkRect(option.rect.center().x() - 7, option.rect.bottom() - 20, 15, 15);
        cbOpt.rect = checkRect;
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
        if (!event || !model || !index.isValid() || !(index.flags() & Qt::ItemIsUserCheckable)
            || !(index.flags() & Qt::ItemIsEnabled)) {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        const auto toggleDeferred = [&]() {
            const Qt::CheckState cur = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
            const Qt::CheckState next = (cur == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            model->setData(index, next, Qt::CheckStateRole);
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
        setGridSize(QSize(cellWidth, 56));
        setIconSize(QSize(0, 0));
    }

    QListWidgetItem* m_dragItem{nullptr};
    int m_indicatorRow{-1};
    int m_managedColumnCount{1};
    int m_pinnedCount{0};
};
}

static QPixmap makePendingStatusPixmap(MainWindow::PendingItemStatus status, int frame) {
    constexpr int sz = 14;
    QPixmap px(sz, sz);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    switch (status) {
    case MainWindow::PendingItemStatus::Pending: {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(160, 160, 160));
        p.drawEllipse(QRectF(3.0, 3.0, 8.0, 8.0));
        break;
    }
    case MainWindow::PendingItemStatus::Running: {
        const qreal start = static_cast<qreal>((frame % 8) * 45) * 16.0;
        p.setPen(QPen(QColor(50, 130, 220), 2.5, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(2.0, 2.0, 10.0, 10.0), static_cast<int>(start), 270 * 16);
        break;
    }
    case MainWindow::PendingItemStatus::Success: {
        p.setPen(QPen(QColor(40, 160, 40), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPolygonF check;
        check << QPointF(2.0, 7.0) << QPointF(5.0, 11.0) << QPointF(12.0, 3.0);
        p.drawPolyline(check);
        break;
    }
    case MainWindow::PendingItemStatus::Failed: {
        p.setPen(QPen(QColor(200, 50, 50), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3.0, 3.0), QPointF(11.0, 11.0));
        p.drawLine(QPointF(11.0, 3.0), QPointF(3.0, 11.0));
        break;
    }
    }
    return px;
}

namespace {

// Pone en negrita la pestaña que lleva contador.
//
// Qt no ofrece tipografía por pestaña: QTabBar aplica una sola a todas. La vía limpia es
// interceptar el dibujado de la etiqueta y cambiar la fuente solo para esa, que es lo que
// hace este estilo. La alternativa —poner en negrita toda la barra— destacaría también
// las pestañas que no tienen nada pendiente, o sea justo lo contrario de lo que se pide.
//
// Se reconoce por el propio contador: una pestaña está en negrita exactamente cuando
// muestra «… (N)», que es cuando tiene algo pendiente. None_ otra lleva paréntesis.
class CountedTabStyle final : public QProxyStyle {
public:
    explicit CountedTabStyle(QStyle* base) : QProxyStyle(base) {}

    void drawControl(ControlElement element,
                     const QStyleOption* option,
                     QPainter* painter,
                     const QWidget* widget) const override {
        if (element == CE_TabBarTabLabel && painter) {
            if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option)) {
                const QString text = tab->text.trimmed();
                if (text.endsWith(QLatin1Char(')')) && text.contains(QStringLiteral(" ("))) {
                    QFont bold = painter->font();
                    bold.setBold(true);
                    painter->save();
                    painter->setFont(bold);
                    QProxyStyle::drawControl(element, option, painter, widget);
                    painter->restore();
                    return;
                }
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

}  // namespace

void MainWindow::updatePendingChangesList() {
    // Ya no hay nada pendiente que pintar, y esta lista es AHORA la de trabajos.
    //
    // Dejarla como estaba no era inofensivo: vaciaba la lista y la repoblaba desde un modelo
    // que ya nadie alimenta, así que cada llamada —y hay siete— habría borrado los trabajos
    // en marcha de la vista. Se corta aquí, en un solo sitio, en vez de perseguir los siete
    // llamantes: lo que queda del modelo de pendientes se retirará entero después, y
    // entonces esta función desaparece con él.
    return;
}

void MainWindow::startPendingApplyAnimation() {
    m_pendingApplyInProgress = true;
    // El estado por fila se pintaba en la lista de cambios, que ya no existe. La animación
    // se queda porque sigue diciendo algo cierto: que hay una tanda aplicándose.
    if (!m_pendingSpinnerTimer) {
        m_pendingSpinnerTimer = new QTimer(this);
        m_pendingSpinnerTimer->setInterval(100);
        connect(m_pendingSpinnerTimer, &QTimer::timeout, this, [this]() {
            ++m_pendingSpinnerFrame;
            updatePendingChangesList();
        });
    }
    m_pendingSpinnerFrame = 0;
    m_pendingSpinnerTimer->start();
    updatePendingChangesList();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 30);
}

void MainWindow::finishPendingApplyAnimation() {
    if (!m_pendingApplyInProgress) {
        return;
    }
    m_pendingApplyInProgress = false;
    if (m_pendingSpinnerTimer) {
        m_pendingSpinnerTimer->stop();
    }
}

int MainWindow::propColumnCountForTree(const QTreeWidget* tree) const {
    if (!tree) {
        return qBound(4, m_connPropColumnsSetting, 16);
    }
    const QVariant prop = tree->property("propColumnsSetting");
    return prop.isValid() ? qBound(4, prop.toInt(), 16) : qBound(4, m_connPropColumnsSetting, 16);
}

void MainWindow::installConnContentTreeHeaderContextMenu(QTreeWidget* tree) {
    if (!tree || !tree->header()) {
        return;
    }
    QHeaderView* header = tree->header();
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this, [this, tree, header](const QPoint& pos) {
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
        const int currentCols = propColumnCountForTree(tree);
        for (int cols = 4; cols <= 16; cols += 2) {
            QAction* act = propColsMenu->addAction(QString::number(cols));
            act->setCheckable(true);
            act->setData(cols);
            if (cols == currentCols) {
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
            if (!ok) {
                return;
            }
            int bounded = qBound(4, cols, 16);
            if ((bounded % 2) != 0) ++bounded;
            if (bounded == currentCols) return;
            tree->setProperty("propColumnsSetting", bounded);
            if (tree == m_connContentTree) {
                m_connPropColumnsSetting = bounded;
                saveUiSettings();
            }
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Columnas de propiedades: %1").arg(bounded));
            const QString token = connContentTokenForTree(tree);
            syncConnContentPropertyColumnsFor(tree, token);
            syncConnContentPoolColumnsFor(tree, token);
            resizeTreeColumnsToVisibleContent(tree);
        }
    });
}

// ── Los dos árboles fijos ────────────────────────────────────────────────────────
//
// Cada uno lleva encima dos desplegables, conexión y pool, y enseña dentro lo que
// cuelga de esa pareja. Antes había un solo árbol con TODAS las conexiones dentro,
// cada una como nodo raíz: para llegar a un dataset había que desplegar la conexión,
// desplegar el pool y bajar; y para transferir entre dos máquinas, tener las dos
// desplegadas a la vez en la misma columna.

void MainWindow::buildDatasetPane(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const bool isOrigin = (paneIdx == 0);

    // La caja del árbol: su cabecera de desplegables y el árbol. El detalle y el log son
    // hermanos suyos en OTRAS filas del partidor, no hijos: es lo que permite que el
    // divisor sea uno solo para los dos paneles.
    pane.treeBox = new QWidget();
    auto* holder = pane.treeBox;
    auto* holderLayout = new QVBoxLayout(holder);
    holderLayout->setContentsMargins(0, 0, 0, 0);
    holderLayout->setSpacing(3);

    auto* head = new QWidget(holder);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(2, 0, 2, 0);
    headLayout->setSpacing(4);
    auto* title = new QLabel(isOrigin
                                 ? trk(QStringLiteral("t_pane_origin_001"),
                                       QStringLiteral("Origen"),
                                       QStringLiteral("Source"))
                                 : trk(QStringLiteral("t_pane_dest_001"),
                                       QStringLiteral("Destino"),
                                       QStringLiteral("Destination")),
                             head);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    pane.connCombo = new QComboBox(head);
    pane.connCombo->setObjectName(isOrigin ? QStringLiteral("originConnCombo")
                                           : QStringLiteral("destinationConnCombo"));
    pane.poolCombo = new QComboBox(head);
    pane.poolCombo->setObjectName(isOrigin ? QStringLiteral("originPoolCombo")
                                           : QStringLiteral("destinationPoolCombo"));
    // Que no crezcan con el nombre más largo: los dos desplegables comparten fila y sin
    // esto una conexión de nombre largo se lleva todo el ancho y deja el pool sin sitio.
    pane.connCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    pane.poolCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    pane.connCombo->setMinimumWidth(120);
    pane.poolCombo->setMinimumWidth(120);
    headLayout->addWidget(title, 0);
    headLayout->addWidget(pane.connCombo, 1);
    headLayout->addWidget(pane.poolCombo, 1);

    delete pane.delegate;
    pane.delegate = new MainWindowConnectionDatasetTreeDelegate(this, this);
    ConnectionDatasetTreeWidget::Config cfg;
    cfg.treeName = isOrigin ? QStringLiteral("originDatasetTreeWidget")
                            : QStringLiteral("destinationDatasetTreeWidget");
    cfg.primaryColumnTitle = trk(QStringLiteral("t_pane_dataset_col001"),
                                 QStringLiteral("Pool/Dataset"),
                                 QStringLiteral("Pool/Dataset"));
    cfg.role = isOrigin ? ConnectionDatasetTreePane::Role::Top
                        : ConnectionDatasetTreePane::Role::Bottom;
    // Sin raíces de conexión: la conexión ya la dice el desplegable de encima, y
    // repetirla dentro costaba un nivel de despliegue en cada rama.
    cfg.groupPoolsByConnectionRoots = false;
    pane.treeWidget = new ConnectionDatasetTreeWidget(cfg, pane.delegate, holder);
    if (QTreeWidget* tree = pane.treeWidget->tree()) {
        tree->setItemDelegate(new ConnContentPropBorderDelegate(tree));
        // Cada panel guarda su despliegue por separado: los dos pueden estar sobre la
        // misma conexión, y sin esto el de la derecha heredaría lo abierto en el izquierdo.
        tree->setProperty("zfsmgr.isSplitTree", true);
        installConnContentTreeHeaderContextMenu(tree);
    }
    holderLayout->addWidget(head, 0);
    holderLayout->addWidget(pane.treeWidget, 1);
    pane.detailBox = buildPaneDetail(paneIdx, nullptr);
    pane.logBox = buildPaneLog(paneIdx, nullptr);

    if (ConnectionDatasetTreePane* treePane = pane.treeWidget->pane()) {
        connect(treePane, &ConnectionDatasetTreePane::selectionChanged, this,
                [this, paneIdx]() {
                    // Marcar otra cosa en el árbol descarta el snapshot que hubiera
                    // marcado en la pestaña: es de otro dataset.
                    m_datasetPanes[paneIdx].snapshotSel.clear();
                    m_datasetPanes[paneIdx].detailForceConnection = false;
                    updatePaneDetail(paneIdx);
                });
    }

    connect(pane.connCombo, &QComboBox::currentIndexChanged, this,
            [this, paneIdx](int) { onPaneConnectionChosen(paneIdx); });
    connect(pane.poolCombo, &QComboBox::currentIndexChanged, this,
            [this, paneIdx](int) { onPanePoolChosen(paneIdx); });
}


// El log de la conexión del panel, debajo de su detalle.
//
// Antes los logs de TODAS las conexiones estaban en pestañas al pie de la ventana, junto
// al log combinado. Mirar el de una máquina era buscar su pestaña; y con dos paneles
// abiertos sobre dos máquinas, ir y volver entre dos pestañas.
//
// Los dos visores NO son dueños de su texto: enseñan el documento del visor que sí lo es
// —el que `syncConnectionLogTabs()` crea por conexión y al que escribe
// `appendConnectionLog()`—. `QPlainTextEdit::setDocument()` permite que dos vistas
// compartan documento, que es lo que hace falta para que la misma conexión pueda estar
// elegida en los dos paneles a la vez sin duplicar el texto ni perder líneas.
QWidget* MainWindow::buildPaneLog(int paneIdx, QWidget* parent) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    const bool isOrigin = (paneIdx == 0);
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    pane.logTabs = new QTabWidget(box);
    pane.logTabs->setDocumentMode(true);
    if (pane.logTabs->tabBar()) {
        pane.logTabs->tabBar()->setExpanding(false);
    }

    const auto makeView = [this](QWidget* owner) {
        auto* view = new QPlainTextEdit(owner);
        view->setReadOnly(true);
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        if (m_logView) {
            view->setFont(m_logView->font());
        }
        return view;
    };

    pane.logTerminalView = makeView(pane.logTabs);
    pane.logTerminalView->setObjectName(isOrigin ? QStringLiteral("originConnLogView")
                                                 : QStringLiteral("destinationConnLogView"));
    // «Log», no «Terminal». Se llamaba así porque enseñaba lo que salía por el terminal
    // de la sesión SSH; pero lo que uno busca ahí es el log de esa conexión, y con el
    // nombre de la máquina al lado el rótulo lo dice entero.
    pane.logTabs->addTab(pane.logTerminalView,
                         trk(QStringLiteral("t_pane_log_tab_001"),
                             QStringLiteral("Log"),
                             QStringLiteral("Log")));

    auto* daemonPage = new QWidget(pane.logTabs);
    auto* daemonLayout = new QVBoxLayout(daemonPage);
    daemonLayout->setContentsMargins(0, 0, 0, 0);
    daemonLayout->setSpacing(2);
    auto* btnRow = new QWidget(daemonPage);
    auto* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(4, 2, 4, 2);
    auto* heartbeatBtn = new QPushButton(QStringLiteral("Heartbeat"), btnRow);
    connect(heartbeatBtn, &QPushButton::clicked, this, [this, paneIdx]() {
        const DatasetPane& p = m_datasetPanes[paneIdx];
        if (p.connIdx >= 0 && p.connIdx < m_conns.profiles.size()) {
            runDaemonHeartbeat(m_conns.profiles.at(p.connIdx).id);
        }
    });
    btnLayout->addWidget(heartbeatBtn, 0);
    btnLayout->addStretch(1);
    pane.logDaemonView = makeView(daemonPage);
    daemonLayout->addWidget(btnRow, 0);
    daemonLayout->addWidget(pane.logDaemonView, 1);
    pane.logTabs->addTab(daemonPage, QStringLiteral("Daemon"));

    layout->addWidget(pane.logTabs, 1);
    return box;
}

void MainWindow::updatePaneLog(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (!pane.logTerminalView || !pane.logDaemonView) {
        return;
    }
    const QString connId = (pane.connIdx >= 0 && pane.connIdx < m_conns.profiles.size())
                               ? m_conns.profiles.at(pane.connIdx).id
                               : QString();
    const auto attach = [](QPlainTextEdit* view, QPlainTextEdit* owner) {
        if (!view) {
            return;
        }
        // Sin dueño —conexión desconectada, o log aún sin crear— un documento propio y
        // vacío. Dejarle el anterior enseñaría el log de otra máquina bajo este panel, que
        // es peor que no enseñar nada.
        QTextDocument* doc = owner ? owner->document() : nullptr;
        if (view->document() == doc) {
            return;
        }
        view->setDocument(doc ? doc : new QTextDocument(view));
    };
    attach(pane.logTerminalView, connId.isEmpty() ? nullptr
                                                  : m_connectionLogViews.value(connId, nullptr));
    attach(pane.logDaemonView, connId.isEmpty() ? nullptr
                                                : m_connectionGsaLogViews.value(connId, nullptr));
    if (pane.logTabs) {
        const QString name = (pane.connIdx >= 0 && pane.connIdx < m_conns.profiles.size())
                                 ? m_conns.profiles.at(pane.connIdx).name
                                 : QString();
        const QString logLabel = trk(QStringLiteral("t_pane_log_tab_001"),
                                     QStringLiteral("Log"),
                                     QStringLiteral("Log"));
        pane.logTabs->setTabText(0, name.isEmpty()
                                        ? logLabel
                                        : QStringLiteral("%1 · %2").arg(logLabel, name));
    }
}

// Rellena los dos desplegables sin perder lo que hubiera elegido el usuario.
//
// Se llama en cada refresco, así que la elección tiene que sobrevivir a que cambie la
// lista: se busca por identificador de conexión y por nombre de pool, no por posición.
// Indexar por posición es justo lo que hacía que al borrar una conexión el panel se
// quedara enseñando la siguiente sin avisar.
void MainWindow::refillDatasetPaneCombos() {
    for (int paneIdx = 0; paneIdx < 2; ++paneIdx) {
        DatasetPane& pane = m_datasetPanes[paneIdx];
        if (!pane.connCombo || !pane.poolCombo) {
            continue;
        }
        QScopedValueRollback<bool> guard(pane.refilling, true);

        const QString wantedConnId = (pane.connIdx >= 0 && pane.connIdx < m_conns.profiles.size())
                                         ? m_conns.profiles[pane.connIdx].id.trimmed()
                                         : QString();
        pane.connCombo->clear();
        for (int i = 0; i < m_conns.profiles.size(); ++i) {
            const ConnectionProfile& p = m_conns.profiles.at(i);
            const QString name = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
            pane.connCombo->addItem(name, p.id.trimmed());
        }
        int connRow = wantedConnId.isEmpty() ? -1 : pane.connCombo->findData(wantedConnId);
        if (connRow < 0 && pane.connCombo->count() > 0) {
            connRow = 0;
        }
        pane.connCombo->setCurrentIndex(connRow);
        pane.connIdx = (connRow >= 0 && connRow < m_conns.profiles.size()) ? connRow : -1;

        const QString wantedPool = pane.poolName.trimmed();
        pane.poolCombo->clear();
        pane.poolCombo->addItem(trk(QStringLiteral("t_pane_all_pools_001"),
                                    QStringLiteral("(todos los pools)"),
                                    QStringLiteral("(all pools)")),
                                QString());
        if (pane.connIdx >= 0 && pane.connIdx < m_conns.states.size()) {
            const ConnectionRuntimeState& st = m_conns.states.at(pane.connIdx);
            QSet<QString> seen;
            for (const PoolImported& pool : st.importedPools) {
                const QString poolName = pool.pool.trimmed();
                if (poolName.isEmpty() || seen.contains(poolName.toLower())) {
                    continue;
                }
                seen.insert(poolName.toLower());
                pane.poolCombo->addItem(poolName, poolName);
            }
            for (const PoolImportable& pool : st.importablePools) {
                const QString poolName = pool.pool.trimmed();
                if (poolName.isEmpty() || seen.contains(poolName.toLower())) {
                    continue;
                }
                seen.insert(poolName.toLower());
                pane.poolCombo->addItem(
                    QStringLiteral("%1 [%2]").arg(poolName,
                                                  trk(QStringLiteral("t_importable_tag_001"),
                                                      QStringLiteral("Importable"),
                                                      QStringLiteral("Importable"))),
                    poolName);
            }
        }
        int poolRow = wantedPool.isEmpty() ? 0 : pane.poolCombo->findData(wantedPool);
        if (poolRow < 0) {
            poolRow = 0;
        }
        pane.poolCombo->setCurrentIndex(poolRow);
        pane.poolName = pane.poolCombo->itemData(poolRow).toString().trimmed();
    }
}

void MainWindow::onPaneConnectionChosen(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (pane.refilling || !pane.connCombo) {
        return;
    }
    const int row = pane.connCombo->currentIndex();
    pane.connIdx = (row >= 0 && row < m_conns.profiles.size()) ? row : -1;
    // Al cambiar de conexión el pool anterior no tiene por qué existir en la nueva: se
    // vuelve a «todos» y que el relleno decida.
    pane.poolName.clear();
    // Los dos paneles marcan cuál es la conexión de trabajo, no solo el de origen: es
    // sobre ella sobre la que actúa el menú «Conexiones», y sus rótulos la nombran.
    if (pane.connIdx >= 0) {
        m_topDetailConnIdx = pane.connIdx;
    }
    refillDatasetPaneCombos();
    rebuildDatasetPane(paneIdx);
}

bool MainWindow::isPoolImportableForConnection(int connIdx, const QString& poolName) const {
    if (connIdx < 0 || connIdx >= m_conns.states.size() || poolName.trimmed().isEmpty()) {
        return false;
    }
    for (const PoolImportable& pool : m_conns.states.at(connIdx).importablePools) {
        if (pool.pool.trimmed().compare(poolName.trimmed(), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

// El diálogo de importar se abre por nombre de pool, no por fila de la tabla de pools:
// desde el desplegable no hay fila. La tabla sigue siendo la fuente —lleva el guid y la
// acción, que es lo que decide si el pool se puede importar— y aquí solo se busca en ella.
void MainWindow::importPoolByName(int connIdx, const QString& poolName) {
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || poolName.trimmed().isEmpty()) {
        return;
    }
    const QString connName = m_conns.profiles.at(connIdx).name;
    for (int row = 0; row < m_conns.poolListEntries.size(); ++row) {
        const auto& pe = m_conns.poolListEntries.at(row);
        if (pe.connection == connName
            && pe.pool.trimmed().compare(poolName.trimmed(), Qt::CaseInsensitive) == 0) {
            importPoolFromRow(row);
            return;
        }
    }
    appLog(QStringLiteral("INFO"),
           QStringLiteral("Importar %1::%2 sin fila en la tabla de pools").arg(connName, poolName));
}

void MainWindow::onPanePoolChosen(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (pane.refilling || !pane.poolCombo) {
        return;
    }
    pane.poolName = pane.poolCombo->currentData().toString().trimmed();
    rebuildDatasetPane(paneIdx);
    // Elegir un pool que está sin importar es pedir importarlo: un pool importable no
    // tiene datasets que enseñar, así que quedarse mirando un árbol vacío no sería una
    // respuesta. Se abre el diálogo con sus opciones —punto de montaje alternativo,
    // solo lectura, forzar, renombrar— que es el mismo de la tabla de pools.
    if (isPoolImportableForConnection(pane.connIdx, pane.poolName)) {
        importPoolByName(pane.connIdx, pane.poolName);
    }
}

void MainWindow::rebuildDatasetPane(int paneIdx) {
    DatasetPane& pane = m_datasetPanes[paneIdx];
    if (!pane.treeWidget) {
        return;
    }
    populatePaneTree(pane.treeWidget->tree(), pane.connIdx, pane.poolName);
    updatePaneDetail(paneIdx);
}

void MainWindow::rebuildDatasetPanes() {
    refillDatasetPaneCombos();
    for (int paneIdx = 0; paneIdx < 2; ++paneIdx) {
        rebuildDatasetPane(paneIdx);
        updatePaneLog(paneIdx);
    }
}

void MainWindow::populatePaneTree(QTreeWidget* tree, int connIdx, const QString& poolName) {
    if (!tree) {
        return;
    }
    const QSignalBlocker blocker(tree);
    const QString stateToken = connContentTokenForTree(tree);
    tree->clear();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size() || connIdx >= m_conns.states.size()) {
        syncConnContentPropertyColumnsFor(tree, stateToken);
        return;
    }
    const QString pool = poolName.trimmed();
    if (pool.isEmpty()) {
        populateConnectionPoolsIntoTree(tree, connIdx, m_conns.states.at(connIdx));
    } else {
        appendPaneTreeRootedAtPool(tree, connIdx, pool, pool, pool);
    }
    if (tree->topLevelItemCount() == 0) {
        auto* empty = new QTreeWidgetItem();
        empty->setText(0, isConnectionDisconnected(connIdx)
                              ? trk(QStringLiteral("t_pane_disconnected_001"),
                                    QStringLiteral("Conexión desconectada"),
                                    QStringLiteral("Connection disconnected"))
                              : trk(QStringLiteral("t_no_pools_001"),
                                    QStringLiteral("Sin Pools"),
                                    QStringLiteral("No Pools")));
        QFont f = empty->font(0);
        f.setItalic(true);
        empty->setFont(0, f);
        empty->setFlags((empty->flags() & ~Qt::ItemIsSelectable) & ~Qt::ItemIsEnabled);
        tree->addTopLevelItem(empty);
    }
    if (!stateToken.isEmpty()) {
        restoreConnContentTreeStateFor(tree, stateToken);
    }
    applyUserExpandedState(tree);
    applyDebugNodeIdsToTree(tree);
    syncConnContentPropertyColumnsFor(tree, connContentTokenForTree(tree));
}

bool MainWindow::focusPendingChangeLine(const QString& line) {
    // Enfocaba una fila de la lista de pendientes. Ya no hay lista de pendientes: esta lista
    // enseña los trabajos en marcha, que no se enfocan por «línea de cambio». Se deja el
    // punto de entrada porque lo llaman desde varios sitios al terminar una acción, y no
    // hace nada.
    Q_UNUSED(line);
    // Devuelve false: no hay ninguna fila que enfocar. Sin este `return` la función tenía
    // comportamiento indefinido —el cruce de MinGW lo avisó y el de Linux no—.
    return false;
}

void MainWindow::setShowInlinePropertyNodesForTree(QTreeWidget* tree, bool visible) {
    Q_UNUSED(tree);
    Q_UNUSED(visible);
}

void MainWindow::setShowInlinePermissionsNodesForTree(QTreeWidget* tree, bool visible) {
    Q_UNUSED(tree);
    Q_UNUSED(visible);
}

void MainWindow::setShowInlineGsaNodeForTree(QTreeWidget* tree, bool visible) {
    Q_UNUSED(tree);
    Q_UNUSED(visible);
}

void MainWindow::setShowPoolInfoNodeForTree(const QTreeWidget* tree, bool visible) {
    Q_UNUSED(tree);
    Q_UNUSED(visible);
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("ZFSMgr [%1]").arg(QStringLiteral(ZFSMGR_APP_VERSION)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/ZFSMgr-512.png")));
    resize(1200, 736);
    setMinimumSize(560, 368);
    const QFont baseUiFont = QApplication::font();
    const int baseUiPointSize = qMax(6, baseUiFont.pointSize());
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
        "QMenu { background: #ffffff; border: 1px solid #9db0c4; padding: 3px; font-family: \"%1\"; font-size: %2pt; }"
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
        "QHeaderView::section { background: #eaf1f7; border: 1px solid #c5d3e0; padding: 1px 3px; }")
        .arg(baseUiFont.family(), QString::number(baseUiPointSize)));
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
            QStringLiteral("Menu")));
    // Sin separador delante: al salir de aquí Idioma y Comprobar conectividad, «Salir» se
    // quedó sola y la barra encabezaba el menú, que es un renglón vacío.
    m_menuExitAction = appMenu->addAction(
        trk(QStringLiteral("t_menu_exit_001"),
            QStringLiteral("Salir"),
            QStringLiteral("Exit")));
    m_menuExitAction->setEnabled(!actionsLocked());
    connect(m_menuExitAction, &QAction::triggered, this, [this]() {
        if (actionsLocked()) {
            return;
        }
        close();
    });

    // ── Conexiones ───────────────────────────────────────────────────────────
    //
    // Gestionar conexiones estaba en el menú contextual del nodo raíz de conexión del
    // árbol. Ese nodo desapareció al pasar la conexión al desplegable de cada panel, así
    // que se quedó sin puerta; y la barra es además su sitio, porque crear una conexión o
    // refrescarlas todas no es una acción SOBRE algo marcado en el árbol.
    QMenu* connectionsMenu = menuBar()->addMenu(
        trk(QStringLiteral("t_menu_connections_001"),
            QStringLiteral("Conexiones"),
            QStringLiteral("Connections")));
    connectionsMenu->setObjectName(QStringLiteral("connectionsMenu"));
    // Se rellena al abrirlo, no aquí: los rótulos llevan dentro el nombre de la conexión
    // sobre la que actúan, y lo que se puede hacer con ella depende de su estado.
    connect(connectionsMenu, &QMenu::aboutToShow, this,
            [this, connectionsMenu]() { fillConnectionsMenu(connectionsMenu); });
    fillConnectionsMenu(connectionsMenu);

    // ── Ajustes ──────────────────────────────────────────────────────────────
    //
    // Esto era una PESTAÑA en el panel de abajo, entre «Transferencias» y «Log combinado».
    // No pinta nada allí: las pestañas de abajo enseñan lo que está pasando —trabajos en
    // marcha, registro— y esto no enseña nada, se toca una vez y se olvida. Ocupaba una
    // pestaña permanente para tres desplegables y una casilla.
    //
    // Las listas cerradas pasan a submenús con marca de selección, que es la forma que Qt
    // da a «una de estas»: se ve el valor puesto sin abrir nada, y elegir cuesta un clic
    // menos que un desplegable.
    QMenu* settingsMenu = menuBar()->addMenu(
        trk(QStringLiteral("t_settings_tab_001"),
            QStringLiteral("Ajustes"),
            QStringLiteral("Settings")));

    // Idioma, lo primero de Ajustes. Estuvo suelto en la barra por el mismo argumento que
    // ahora lo trae aquí —quien abre la aplicación en un idioma que no es el suyo tiene
    // que poder cambiarlo sin leer nada— y resulta que ese argumento se cumple igual: los
    // menús de Qt se abren al pasar por encima, así que llegar hasta aquí cuesta el mismo
    // gesto que antes y la barra se queda con una entrada menos.
    QMenu* languageMenu = settingsMenu->addMenu(
        trk(QStringLiteral("t_lang_menu_001"),
            QStringLiteral("Idioma"),
            QStringLiteral("Language")));
    auto* langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);
    QAction* langEs = languageMenu->addAction(QStringLiteral("Español"));
    QAction* langEn = languageMenu->addAction(QStringLiteral("English"));
    langEs->setCheckable(true);
    langEn->setCheckable(true);
    langEs->setData(QStringLiteral("es"));
    langEn->setData(QStringLiteral("en"));
    langGroup->addAction(langEs);
    langGroup->addAction(langEn);
    const QString langNorm = m_language.trimmed().toLower();
    if (langNorm == QStringLiteral("en")) {
        langEn->setChecked(true);
    } else {
        // Cualquier otra cosa cae en castellano, y eso INCLUYE «zh»: hay configuraciones
        // guardadas con el chino puesto, y sin esta rama el menú se quedaría sin ninguna
        // marca —ningún idioma elegido— hasta que alguien tocara el selector.
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
        m_conns.store.setLanguage(m_language);
        saveUiSettings();
        appLog(QStringLiteral("INFO"), QStringLiteral("Idioma cambiado a %1").arg(m_language));
        applyLanguageLive();
    });

    QMenu* logsMenu = settingsMenu->addMenu(
        trk(QStringLiteral("t_logs_menu_001"),
            QStringLiteral("Logs"),
            QStringLiteral("Logs")));

    // Nivel de log.
    QMenu* logLevelMenu = logsMenu->addMenu(
        trk(QStringLiteral("t_log_level_001"),
            QStringLiteral("Nivel de log"),
            QStringLiteral("Log level")));
    auto* logLevelGroup = new QActionGroup(this);
    logLevelGroup->setExclusive(true);
    for (const QString& level : {QStringLiteral("normal"), QStringLiteral("info"),
                                 QStringLiteral("debug")}) {
        QAction* act = logLevelMenu->addAction(level);
        act->setCheckable(true);
        act->setData(level);
        act->setChecked(level == m_logLevelSetting);
        logLevelGroup->addAction(act);
    }
    connect(logLevelGroup, &QActionGroup::triggered, this, [this](QAction* act) {
        if (!act) {
            return;
        }
        const QString level = act->data().toString().trimmed().toLower();
        if (level != QStringLiteral("normal") && level != QStringLiteral("info")
            && level != QStringLiteral("debug")) {
            return;
        }
        m_logLevelSetting = level;
        saveUiSettings();
        // Los identificadores de nodo solo se pintan en «debug», así que al cambiar de
        // nivel hay que repasar TODOS los árboles y no solo el que está a la vista.
        if (m_connContentTree) {
            applyDebugNodeIdsToTree(m_connContentTree);
        }
        const auto panes = findChildren<ConnectionDatasetTreePane*>();
        for (ConnectionDatasetTreePane* pane : panes) {
            if (!pane) {
                continue;
            }
            if (QTreeWidget* tree = pane->tree()) {
                if (tree == m_connContentTree) {
                    continue;
                }
                applyDebugNodeIdsToTree(tree);
            }
        }
    });

    // Cuántas líneas se conservan en la vista.
    QMenu* logLinesMenu = logsMenu->addMenu(
        trk(QStringLiteral("t_log_lines_001"),
            QStringLiteral("Número de líneas"),
            QStringLiteral("Number of lines")));
    auto* logLinesGroup = new QActionGroup(this);
    logLinesGroup->setExclusive(true);
    for (int lines : {100, 200, 500, 1000}) {
        QAction* act = logLinesMenu->addAction(QString::number(lines));
        act->setCheckable(true);
        act->setData(lines);
        act->setChecked(lines == m_logMaxLinesSetting);
        logLinesGroup->addAction(act);
    }
    connect(logLinesGroup, &QActionGroup::triggered, this, [this](QAction* act) {
        if (!act) {
            return;
        }
        const int lines = act->data().toInt();
        if (lines != 100 && lines != 200 && lines != 500 && lines != 1000) {
            return;
        }
        m_logMaxLinesSetting = lines;
        trimLogWidget(m_logView);
        saveUiSettings();
    });

    // Tamaño al que rota el fichero de log.
    QMenu* logSizeMenu = logsMenu->addMenu(
        trk(QStringLiteral("t_log_max_rot_001"),
            QStringLiteral("Tamaño máximo log rotativo"),
            QStringLiteral("Max rotating log size")));
    auto* logSizeGroup = new QActionGroup(this);
    logSizeGroup->setExclusive(true);
    {
        // El valor guardado se añade a la lista si no es uno de los de siempre: puede venir
        // de un config.json escrito a mano, y no ofrecerlo dejaría el menú sin ninguna
        // marca, como si no hubiera nada elegido.
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
            act->setChecked(mb == m_logMaxSizeMb);
            logSizeGroup->addAction(act);
        }
    }
    connect(logSizeGroup, &QActionGroup::triggered, this, [this](QAction* act) {
        if (!act) {
            return;
        }
        const int mb = qBound(1, act->data().toInt(), 1024);
        if (mb == m_logMaxSizeMb) {
            return;
        }
        m_logMaxSizeMb = mb;
        saveUiSettings();
        rotateLogIfNeeded();
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Tamaño máximo de log rotativo: %1 MB").arg(m_logMaxSizeMb));
    });

    logsMenu->addSeparator();
    QAction* clearLogsAct = logsMenu->addAction(
        trk(QStringLiteral("t_clear_001"),
            QStringLiteral("Limpiar"),
            QStringLiteral("Clear")));
    connect(clearLogsAct, &QAction::triggered, this, [this]() {
        logUiAction(QStringLiteral("Limpiar log (ajustes)"));
        clearAppLog();
    });
    QAction* copyLogsAct = logsMenu->addAction(
        trk(QStringLiteral("t_copy_001"),
            QStringLiteral("Copiar"),
            QStringLiteral("Copy")));
    connect(copyLogsAct, &QAction::triggered, this, [this]() {
        logUiAction(QStringLiteral("Copiar log (ajustes)"));
        copyAppLogToClipboard();
    });

    // La casilla, colgada de `m_confirmActionsMenuAction`.
    //
    // Ese miembro existía y se ponía a nullptr, y `mainwindow_dialogs.cpp` lo consulta para
    // mantenerla al día cuando la confirmación se cambia desde otro sitio. Con la casilla
    // en una pestaña, esa sincronización no llegaba a nadie.
    m_confirmActionsMenuAction = settingsMenu->addAction(
        trk(QStringLiteral("t_show_confirm_001"),
            QStringLiteral("Mostrar confirmación antes de ejecutar acciones"),
            QStringLiteral("Show confirmation before executing actions")));
    m_confirmActionsMenuAction->setCheckable(true);
    m_confirmActionsMenuAction->setChecked(m_actionConfirmEnabled);
    connect(m_confirmActionsMenuAction, &QAction::toggled, this, [this](bool checked) {
        if (checked == m_actionConfirmEnabled) {
            return;
        }
        m_actionConfirmEnabled = checked;
        saveUiSettings();
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Confirmación de acciones: %1").arg(checked ? QStringLiteral("on")
                                                                          : QStringLiteral("off")));
    });

    QMenu* helpMenu = menuBar()->addMenu(
        trk(QStringLiteral("t_help_menu_001"),
            QStringLiteral("Ayuda")));

    // Comprobar conectividad ABRE el menú y va separada por una barra. No es un tema de
    // ayuda —hace algo, habla con las máquinas— y mezclarla con los temas la convertía en
    // uno más de la lista.
    m_connectivityMatrixAction = helpMenu->addAction(
        trk(QStringLiteral("t_connectivity_menu_001"),
            QStringLiteral("Comprobar conectividad"),
            QStringLiteral("Check connectivity")));
    connect(m_connectivityMatrixAction, &QAction::triggered, this, [this]() {
        logUiAction(QStringLiteral("Comprobar conectividad (menú)"));
        openConnectivityMatrixDialog();
    });
    helpMenu->addSeparator();

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
            QStringLiteral("Acciones"),
            QStringLiteral("Actions")));
    struct HelpTopicItem {
        QString id;
        QString key;
        QString es;
    };
    const QVector<HelpTopicItem> helpActions = {
        // **Clave propia, no `t_copy_001`.** Esa la comparte el botón «Send» del registro, que
        // copia al portapapeles y sigue llamándose así. Con una sola clave, traducir la acción
        // como «Enviar» habría renombrado también aquel botón.
        {QStringLiteral("accion_enviar"), QStringLiteral("t_send_action_001"),
         QStringLiteral("Enviar")},
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

    // El rótulo decía «archivos INI». Ese formato se retiró hace tiempo —la configuración
    // es config.json y trust-store.json— y el TEMA ya estaba corregido, pero la entrada de
    // menú que lo abre seguía nombrando un fichero que no existe.
    QAction* cfgFilesHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_cfg_002"),
            QStringLiteral("Configuración y archivos"),
            QStringLiteral("Configuration and files")));
    connect(cfgFilesHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("configuracion_archivos"),
                      trk(QStringLiteral("t_help_cfg_002"),
                          QStringLiteral("Configuración y archivos"),
                          QStringLiteral("Configuration and files")));
    });

    QAction* cliHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_cli_001"),
            QStringLiteral("Línea de órdenes"),
            QStringLiteral("Command line")));
    connect(cliHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("linea_de_ordenes"),
                      trk(QStringLiteral("t_help_cli_001"),
                          QStringLiteral("Línea de órdenes"),
                          QStringLiteral("Command line")));
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
    topLayout->setSpacing(6);
    m_topMainSplit = nullptr;
    m_rightMainSplit = nullptr;

    auto* leftPane = new QWidget(topArea);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
    leftPane->setMinimumWidth(0);

    auto* connectionsTab = new QWidget(leftPane);
    auto* connLayout = new QVBoxLayout(connectionsTab);
    connLayout->setContentsMargins(2, 2, 2, 2);
    connLayout->setSpacing(2);
    const int stdLeftBtnH = 34;
    m_poolMgmtBox = nullptr;

    updateConnectivityMatrixButtonState();

    // La caja «Acciones» ya no existe. Contenía la rejilla de seis botones de
    // origen+destino, que pasaron al menú contextual del destino; con ellos fuera se
    // quedaba vacía —la etiqueta del origen siempre estuvo en su propia fila, no aquí—,
    // ocupando ancho y una altura mínima a la que además se ajustaba la caja de gestión
    // de pools.

    m_connOriginSelectionLabel = new QLabel(
        trk(QStringLiteral("t_conn_origin_sel1"),
            QStringLiteral("Origen:(vacío)"),
            QStringLiteral("Source:(empty)")),
        connectionsTab);
    // Sin ajuste de línea ni altura mínima: es una sola línea, y con wordWrap una ruta
    // larga hacía crecer la banda de en medio a costa del árbol. Si no cabe, se elide y
    // el texto completo queda en el tooltip.
    m_connOriginSelectionLabel->setWordWrap(false);
    m_connOriginSelectionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Comparte fila con Estado y Progreso, así que no puede reclamar la anchura de una
    // ruta larga ni quedarse en cero. Anchura mínima para que siempre se lea algo, y el
    // texto se acorta con puntos suspensivos al pintarlo; entero, en el tooltip.
    m_connOriginSelectionLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_connOriginSelectionLabel->setMinimumWidth(140);
    m_connOriginSelectionLabel->setFont(baseUiFont);
    m_btnApplyConnContentProps = new TooltipPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes")),
        connectionsTab);
    m_btnDiscardPendingChanges = new QPushButton(
        // «Vaciar lista» era su nombre cuando descartaba una lista de acciones
        // encoladas. Esa lista ya no existe: lo que descarta son ediciones a medias de
        // propiedades y permisos, así que se llama por lo que hace.
        trk(QStringLiteral("t_discard_changes_001"),
            QStringLiteral("Descartar cambios"),
            QStringLiteral("Discard changes")),
        connectionsTab);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    m_btnApplyConnContentProps->setFont(baseUiFont);
    m_btnDiscardPendingChanges->setFont(baseUiFont);
    m_btnApplyConnContentProps->setMinimumHeight(stdLeftBtnH);
    m_btnDiscardPendingChanges->setMinimumHeight(stdLeftBtnH);
    m_btnApplyConnContentProps->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnDiscardPendingChanges->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // Aquí vivía una rejilla 3x2 con Sincronizar, Enviar, Clonar, Mover, Nivelar y Diff.
    // Las seis pasaron al menú contextual del destino, así que la caja se queda solo con
    // la etiqueta del origen y pierde la altura mínima de tres filas de botones: ese es
    // el espacio que se recupera.

    // Panel sin marco ni título: pasa a ser la primera pestaña de abajo, y ahí el nombre
    // lo pone la propia pestaña. Sacarlo de la pestaña de Conexiones devuelve toda esa
    // altura al árbol, que es lo que se mira el 90% del tiempo.
    auto* pendingChangesBox = new QWidget(connectionsTab);
    auto* pendingChangesLayout = new QVBoxLayout(pendingChangesBox);
    pendingChangesLayout->setContentsMargins(6, 6, 6, 6);
    pendingChangesLayout->setSpacing(4);
    auto* pendingChangesBody = new QHBoxLayout();
    pendingChangesBody->setContentsMargins(0, 0, 0, 0);
    pendingChangesBody->setSpacing(6);
    auto* pendingButtonsCol = new QVBoxLayout();
    pendingButtonsCol->setContentsMargins(0, 0, 0, 0);
    pendingButtonsCol->setSpacing(4);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_pendingButtonsCol = pendingButtonsCol;
    pendingButtonsCol->addStretch(1);
    pendingChangesBody->addLayout(pendingButtonsCol, 0);
    // Esta lista era la de cambios pendientes. Ahora enseña los TRABAJOS en marcha.
    //
    // No es un reaprovechamiento oportunista del hueco: es que la lista de pendientes ya no
    // tiene nada que enseñar —las acciones se ejecutan al pulsarlas— y lo que sí necesita un
    // sitio fijo a la vista es lo que está corriendo ahora mismo en los daemons. Antes eso
    // vivía en una pestaña aparte, «Transferencias», que se retira: dos listas para lo mismo
    // en pestañas distintas era el reparto anterior, no una decisión.
    //
    // Aplicar y Deshacer ya no están en esta columna: se fueron a la banda de Estado y
    // Progreso, encima de los árboles, que es donde se edita. Aquí quedan Refrescar y
    // Cancelar, que sí son de los trabajos.
    m_pendingChangesList = new QListWidget(pendingChangesBox);
    // Un solo widget con dos nombres, a propósito y por poco tiempo: el código que pinta los
    // trabajos escribe en `m_jobsListWidget` y está probado; el que coloca y dimensiona este
    // panel escribe en `m_pendingChangesList`. Apuntando los dos al mismo sitio, los
    // trabajos aparecen aquí sin tocar ninguna de las dos partes. Queda por unificar el
    // nombre cuando se retire lo que resta del modelo de pendientes.
    m_jobsListWidget = m_pendingChangesList;
    m_pendingChangesList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pendingChangesList->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pendingChangesList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pendingChangesList->setMinimumHeight(0);
    m_pendingChangesList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_pendingChangesList->setIconSize(QSize(14, 14));
    m_pendingChangesList->setSpacing(1);
    m_pendingChangesList->setContextMenuPolicy(Qt::CustomContextMenu);
    pendingChangesBody->addWidget(m_pendingChangesList, 1);
    pendingChangesLayout->addLayout(pendingChangesBody, 1);
    pendingChangesBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    pendingChangesBox->setMinimumHeight(0);
    pendingChangesBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connectionsTab->setLayout(connLayout);

    // Legacy left "Datasets" tab removed from UI.
    // Legacy "advanced" layer removed from visible UI.
    // La caja de gestión de pools se fijaba a la altura de la de acciones. Sin aquella,
    // se dimensiona por su propio contenido: forzarla a una altura ajena era lo que la
    // descuadraba al cambiar lo de al lado.


    leftLayout->addWidget(connectionsTab, 1);

    m_rightStack = new QStackedWidget(topArea);

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
    auto* detailContainer = new QFrame(m_poolDetailTabs);
    detailContainer->setObjectName(QStringLiteral("zfsmgrDetailContainer"));
    detailContainer->setFrameShape(QFrame::NoFrame);
    detailContainer->setFrameShadow(QFrame::Plain);
    detailContainer->setLineWidth(0);
    auto* detailContainerLayout = new QVBoxLayout(detailContainer);
    detailContainerLayout->setContentsMargins(0, 0, 0, 0);
    detailContainerLayout->setSpacing(0);
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
                                                     QStringLiteral("Property")),
                                                 trk(QStringLiteral("t_value_col_001"),
                                                     QStringLiteral("Valor"),
                                                     QStringLiteral("Value")),
                                                 trk(QStringLiteral("t_origin_col001"),
                                                     QStringLiteral("Origen"),
                                                     QStringLiteral("Source"))});
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_poolPropsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_poolPropsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_poolPropsTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_poolPropsTable->verticalHeader()->setVisible(false);
    m_poolPropsTable->verticalHeader()->setDefaultSectionSize(22);
    enableSortableHeader(m_poolPropsTable);
    auto* poolPropBtns = new QHBoxLayout();
    poolPropBtns->setContentsMargins(0, 0, 0, 0);
    poolPropBtns->setSpacing(6);
    m_poolStatusRefreshBtn = new QPushButton(trk(QStringLiteral("t_refresh_btn001"),
                                                 QStringLiteral("Actualizar"),
                                                 QStringLiteral("Refresh")),
                                             m_connPoolPropsPage);
    m_poolStatusImportBtn = new QPushButton(trk(QStringLiteral("t_import_btn001"),
                                                QStringLiteral("Importar"),
                                                QStringLiteral("Import")),
                                            m_connPoolPropsPage);
    m_poolStatusExportBtn = new QPushButton(trk(QStringLiteral("t_export_btn001"),
                                                QStringLiteral("Exportar"),
                                                QStringLiteral("Export")),
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
    poolPropsPageLayout->addWidget(m_poolPropsTable, 1);
    auto* poolStatusBox = new QGroupBox(
        trk(QStringLiteral("t_pool_status_box_001"),
            QStringLiteral("Estado del pool"),
            QStringLiteral("Pool status")),
        m_connPoolPropsPage);
    auto* poolStatusBoxLayout = new QVBoxLayout(poolStatusBox);
    poolStatusBoxLayout->setContentsMargins(6, 8, 6, 6);
    m_poolStatusText = new QPlainTextEdit(poolStatusBox);
    m_poolStatusText->setReadOnly(true);
    m_poolStatusText->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_poolStatusText->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    poolStatusBoxLayout->addWidget(m_poolStatusText, 1);
    poolPropsPageLayout->addWidget(poolStatusBox, 1);
    m_connPropsStack->addWidget(m_connPoolPropsPage);

    m_connContentPage = new QWidget(m_connPropsStack);
    auto* connContentLayout = new QVBoxLayout(m_connContentPage);
    connContentLayout->setContentsMargins(0, 0, 0, 0);
    connContentLayout->setSpacing(4);
    buildDatasetPane(0);
    buildDatasetPane(1);
    // Tres filas y UN divisor por frontera, compartido por los dos paneles: árboles,
    // detalles y logs. Cada fila reparte su ancho al 50% con un layout, no con un
    // partidor: entre el panel izquierdo y el derecho no hay nada que arrastrar, y así el
    // reparto horizontal no puede quedarse descuadrado.
    //
    // Antes cada panel llevaba su propio partidor vertical, de modo que subir el detalle
    // de la izquierda dejaba el de la derecha donde estaba y las dos columnas dejaban de
    // leerse en paralelo, que es justo para lo que sirven dos paneles.
    const auto makeRow = [](QWidget* left, QWidget* right) {
        auto* row = new QWidget();
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        layout->addWidget(left, 1);
        layout->addWidget(right, 1);
        return row;
    };
    m_paneRowsSplit = new QSplitter(Qt::Vertical, m_connContentPage);
    m_paneRowsSplit->setObjectName(QStringLiteral("zfsmgrPaneRowsSplit"));
    m_paneRowsSplit->setChildrenCollapsible(false);
    m_paneRowsSplit->setHandleWidth(4);
    m_paneRowsSplit->addWidget(makeRow(m_datasetPanes[0].treeBox, m_datasetPanes[1].treeBox));
    m_paneRowsSplit->addWidget(makeRow(m_datasetPanes[0].detailBox, m_datasetPanes[1].detailBox));
    m_paneRowsSplit->addWidget(makeRow(m_datasetPanes[0].logBox, m_datasetPanes[1].logBox));
    m_paneRowsSplit->setStretchFactor(0, 5);
    m_paneRowsSplit->setStretchFactor(1, 4);
    m_paneRowsSplit->setStretchFactor(2, 2);
    // Los nombres de siempre siguen apuntando a los mismos dos árboles: origen es el
    // panel izquierdo y destino el derecho. El resto del código pide «el árbol de
    // origen» y «el de destino» por estos miembros, y así no tiene que enterarse de
    // que ahora vienen de un panel con desplegables.
    m_topConnContentDelegate = m_datasetPanes[0].delegate;
    m_topDatasetTreeWidget = m_datasetPanes[0].treeWidget;
    m_topDatasetPane = m_topDatasetTreeWidget->pane();
    m_connContentTree = m_topDatasetTreeWidget->tree();
    m_bottomDatasetTreeWidget = m_datasetPanes[1].treeWidget;
    m_bottomConnContentTree = m_bottomDatasetTreeWidget->tree();
    // Las acciones se exponen por menú contextual del árbol.
    connContentLayout->addWidget(m_paneRowsSplit, 1);
    m_btnApplyConnContentProps->setEnabled(false);
    if (m_btnDiscardPendingChanges) m_btnDiscardPendingChanges->setEnabled(false);
    m_connPropsStack->addWidget(m_connContentPage);
    m_connPropsStack->setCurrentWidget(m_connPoolPropsPage);
    propsPoolLayout->addWidget(m_connPropsStack, 1);
    detailContainerLayout->addWidget(m_connPropsGroup, 1);
    poolDetailLayout->addWidget(detailContainer, 1);
    entityFrameLayout->addWidget(m_poolDetailTabs, 1);
    rightConnectionsLayout->setSpacing(0);
    rightConnectionsLayout->addWidget(entityFrame, 1);

    m_rightStack->addWidget(rightConnectionsPage);
    // Mismo esquema de columnas en los dos árboles aunque uno esté vacío.
    syncConnContentPropertyColumnsFor(m_connContentTree, connContentTokenForTree(m_connContentTree));
    syncConnContentPropertyColumnsFor(m_bottomConnContentTree,
                                      connContentTokenForTree(m_bottomConnContentTree));

    // Una CAJA con su título dentro, no una pestaña.
    //
    // Abajo queda una sola cosa —las transferencias—, y una barra de pestañas con una
    // pestaña sola gasta una fila entera para no ofrecer ninguna elección. El título de
    // un QGroupBox va dentro del borde y ese alto se lo queda la lista.
    m_transfersBox = new QGroupBox(central);
    m_transfersBox->setObjectName(QStringLiteral("zfsmgrTransfersBox"));

    // Aquí se construía la pestaña «Ajustes»: un QGroupBox «Logs» con tres desplegables,
    // la casilla de confirmación y los botones de Limpiar y Copiar. Todo eso vive ahora en
    // el menú «Ajustes» de la barra —ver más arriba, junto a los demás menús—, porque no
    // era información que mirar mientras se trabaja, que es para lo que sirven estas
    // pestañas de abajo.


    auto* combinedLogTab = new QWidget(central);
    auto* logLayout = new QVBoxLayout(combinedLogTab);
    logLayout->setContentsMargins(6, 6, 6, 6);
    logLayout->setSpacing(4);

    QFont combinedLogFont = baseUiFont;

    auto* stateProgressRow = new QWidget(topArea);
    auto* stateProgressLayout = new QHBoxLayout(stateProgressRow);
    stateProgressLayout->setContentsMargins(0, 0, 0, 0);
    stateProgressLayout->setSpacing(4);
    auto* statusWrap = new QWidget(stateProgressRow);
    auto* statusLayout = new QHBoxLayout(statusWrap);
    statusLayout->setContentsMargins(0, 1, 0, 0);
    statusLayout->setSpacing(6);
    auto* statusLabel = new QLabel(trk(QStringLiteral("t_status_col_001"),
                                       QStringLiteral("Estado"),
                                       QStringLiteral("Status")),
                                   statusWrap);
    m_statusText = new QTextEdit(statusWrap);
    m_statusText->setFont(combinedLogFont);
    m_statusText->setReadOnly(true);
    m_statusText->setAcceptRichText(false);
    m_statusText->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_statusText->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_statusText->setLineWrapMode(QTextEdit::NoWrap);
    m_statusText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    m_statusText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_statusText->setFixedHeight(22);
    m_statusText->setPlainText(trk(QStringLiteral("t_status_loading_001"),
                                   QStringLiteral("Loading..."),
                                   QStringLiteral("Loading...")));
    statusLayout->addWidget(statusLabel, 0);
    statusLayout->addWidget(m_statusText, 1);

    auto* detailWrap = new QWidget(stateProgressRow);
    auto* detailLayout = new QHBoxLayout(detailWrap);
    detailLayout->setContentsMargins(0, 1, 0, 0);
    detailLayout->setSpacing(6);
    auto* detailLabel = new QLabel(trk(QStringLiteral("t_detail_lbl001"),
                                       QStringLiteral("Progreso"),
                                       QStringLiteral("Progress")),
                                   detailWrap);
    m_lastDetailText = new QTextEdit(detailWrap);
    m_lastDetailText->setFont(combinedLogFont);
    m_lastDetailText->setReadOnly(true);
    m_lastDetailText->setAcceptRichText(false);
    m_lastDetailText->setLineWrapMode(QTextEdit::NoWrap);
    m_lastDetailText->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Sin barra horizontal tampoco. El campo mide 22 px de alto y la barra se comía la
    // mitad, dejando el texto cortado por abajo para poder desplazar una línea que ya
    // está entera en el tooltip y en el log. Lo que no cabe, no cabe.
    m_lastDetailText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lastDetailText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    m_lastDetailText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_lastDetailText->setFixedHeight(22);
    detailLayout->addWidget(detailLabel, 0);
    detailLayout->addWidget(m_lastDetailText, 1);
    // Sin el rótulo de «Origen»: decía qué había marcado como origen cuando origen y
    // destino se elegían dentro del mismo árbol. Con un panel para cada papel, lo dice el
    // propio panel —su desplegable arriba y su selección dentro— y el rótulo repetía a
    // media línea lo que ya se ve entero.
    stateProgressLayout->addWidget(statusWrap, 1);
    stateProgressLayout->addWidget(detailWrap, 3);
    // Aplicar y Deshacer, aquí y no en cada panel.
    //
    // Estaban en `leftPane`, que está oculto desde que su pestaña se quedó sin contenido:
    // o sea que no se veían. Con la edición de propiedades en las columnas C1...C10 del
    // árbol eso ya era un agujero —se podía cambiar un valor y no había con qué
    // aplicarlo—; con la tabla del detalle se nota a la primera.
    //
    // Van en la banda de abajo y no dentro de cada panel porque el borrador es UNO: el
    // mismo `m_propsToken`/`m_propsDataset` para los dos árboles. Dos parejas de botones
    // dirían que hay dos lotes de cambios, y no los hay.
    stateProgressLayout->addWidget(m_btnApplyConnContentProps, 0);
    stateProgressLayout->addWidget(m_btnDiscardPendingChanges, 0);

    auto* appLogBox = new QGroupBox(trk(QStringLiteral("t_app_tab_001"),
                                        QStringLiteral("Aplicación"),
                                        QStringLiteral("Application")),
                                    combinedLogTab);
    auto* appLogLayout = new QVBoxLayout(appLogBox);
    appLogLayout->setContentsMargins(6, 6, 6, 6);
    appLogLayout->setSpacing(4);
    m_logView = new QPlainTextEdit(appLogBox);
    m_logView->setObjectName(QStringLiteral("applicationLogView"));
    m_logView->setFont(combinedLogFont);
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_logView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    appLogLayout->addWidget(m_logView, 1);
    stateProgressRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    appLogBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    logLayout->addWidget(appLogBox, 1);

    leftPane->setMinimumHeight(0);
    leftPane->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    leftPane->setMaximumWidth(QWIDGETSIZE_MAX);
    // `leftPane` NO se añade: contiene la pestaña de Conexiones, que se quedó sin un solo
    // widget cuando la lista de cambios pendientes se mudó a las pestañas de abajo —los
    // árboles viven en m_connContentPage, en el panel de arriba—. Con estiramiento 1
    // primero, y con 0 después, seguía siendo un widget vacío al que el divisor le daba
    // su alto guardado: eso era la banda enorme y vacía. Se oculta en vez de borrarse
    // porque sigue siendo el padre de widgets que ya se reubicaron.
    leftPane->hide();

    // Estado y Progreso, ENCIMA de los árboles.
    //
    // Estaban al pie, pegados a las pestañas de log. Ahí abajo cuentan lo que está
    // pasando arriba, y quedaban a un palmo del sitio donde se mira: se pulsaba una
    // acción en el árbol y la respuesta aparecía al otro extremo de la ventana. Van sin
    // estirar, así que ocupan una línea y ni un píxel más.
    topLayout->addWidget(stateProgressRow, 0);
    topLayout->addWidget(m_rightStack, 1);
    loadPersistedAppLogToView();

    m_pendingChangesTab = pendingChangesBox;
    // La pestaña ya no es «Cambios pendientes»: no hay nada pendiente. Enseña lo que está
    // corriendo ahora mismo, que es lo que uno quiere tener a la vista mientras trabaja.
    m_transfersBox->setTitle(trk(QStringLiteral("t_jobs_tab_001"),
                                 QStringLiteral("Transferencias"),
                                 QStringLiteral("Transfers")));
    auto* transfersLayout = new QVBoxLayout(m_transfersBox);
    transfersLayout->setContentsMargins(6, 2, 6, 4);
    transfersLayout->setSpacing(0);
    transfersLayout->addWidget(pendingChangesBox, 1);
    // Sin pestaña de «Log combinado». Enseñaba el log de la aplicación mezclado con el de
    // todas las conexiones; los de cada conexión están ahora bajo su panel, y el de la
    // aplicación sigue escribiéndose en disco y se copia desde Ajustes ▸ Logs. El widget
    // se construye igual porque es a donde escribe `appLog()`, pero no se enseña.
    combinedLogTab->hide();

    // ── Transfer Jobs tab ──────────────────────────────────────────────────
    {
        // Ya no se crea una pestaña propia: la lista vive en el panel de abajo, junto a
        // Aplicar y Deshacer. Aquí solo quedan sus DOS BOTONES, que se cuelgan de la misma
        // columna.
        //
        // El widget suelto que había antes no era inofensivo: al quitarle la pestaña seguía
        // construyéndose, y sus botones aparecían flotando sobre la barra de pestañas,
        // tapándola. Se vio en la captura de la interfaz, no leyendo el código.
        m_jobsTab = m_pendingChangesTab;
        auto* cancelBtn  = new QPushButton(trk(QStringLiteral("t_jobs_cancel_sel001"),
                                                QStringLiteral("Cancelar seleccionado"),
                                                QStringLiteral("Cancel selected")), m_pendingChangesTab);
        auto* refreshBtn = new QPushButton(trk(QStringLiteral("t_jobs_refresh001"),
                                                QStringLiteral("Refrescar"),
                                                QStringLiteral("Refresh")), m_pendingChangesTab);
        // Los dos del mismo ancho, el del más largo. Uno debajo del otro y con anchos
        // distintos, la columna quedaba en escalera. El ancho sale de `sizeHint()` y no de
        // un número: el rótulo cambia con el idioma, y «Cancel selected» no mide lo mismo
        // que «Cancelar seleccionado».
        const int btnWidth = qMax(refreshBtn->sizeHint().width(), cancelBtn->sizeHint().width());
        refreshBtn->setMinimumWidth(btnWidth);
        cancelBtn->setMinimumWidth(btnWidth);
        if (m_pendingButtonsCol) {
            // En 0 y 1, ARRIBA del estirador. Iban en 2 y 3 porque delante estaban Aplicar
            // y Deshacer; al mudarse esos dos, los índices dejaron a los botones detrás
            // del estirador y se pegaban al fondo de la caja, con el hueco encima.
            m_pendingButtonsCol->insertWidget(0, refreshBtn, 0, Qt::AlignLeft | Qt::AlignTop);
            m_pendingButtonsCol->insertWidget(1, cancelBtn, 0, Qt::AlignLeft | Qt::AlignTop);
        }

        connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::pollDaemonJobs);
        connect(cancelBtn, &QPushButton::clicked, this, [this]() {
            if (!m_jobsListWidget) return;
            auto* item = m_jobsListWidget->currentItem();
            if (!item) return;
            const QString jobId   = item->data(Qt::UserRole).toString();
            const int srcConnIdx  = item->data(Qt::UserRole + 1).toInt();
            if (jobId.isEmpty() || srcConnIdx < 0 || srcConnIdx >= m_conns.profiles.size()) return;
            const ConnectionProfile sp = m_conns.profiles[srcConnIdx];
            QStringList args;
            args << mwhelpers::argvQt(
                zfsmgr::commands::requests::cancelJob(jobId.toStdString()));
            QString out, err;
            int rc = -1;
            tryRunRemoteAgentRpcViaTunnel(sp, args, 5000, out, err, rc);
            if (rc == 0) {
                for (ActiveDaemonJob& j : m_activeDaemonJobs) {
                    if (j.jobId == jobId) { j.state = QStringLiteral("cancelled"); break; }
                }
                updateJobsListWidget();
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("Job %1 cancelado por el usuario").arg(jobId));
            }
        });

        m_jobPollTimer = new QTimer(this);
        m_jobPollTimer->setSingleShot(false);
        m_jobPollTimer->setInterval(2500);
        connect(m_jobPollTimer, &QTimer::timeout, this, &MainWindow::pollDaemonJobs);
    }

    auto* bottomTabsPane = new QWidget(central);
    auto* bottomTabsLayout = new QVBoxLayout(bottomTabsPane);
    bottomTabsLayout->setContentsMargins(0, 0, 0, 0);
    bottomTabsLayout->setSpacing(0);
    bottomTabsLayout->addWidget(m_transfersBox, 1);

    m_verticalMainSplit = new QSplitter(Qt::Vertical, central);
    m_verticalMainSplit->setChildrenCollapsible(true);
    m_verticalMainSplit->setHandleWidth(4);
    topArea->setMinimumHeight(0);
    bottomTabsPane->setMinimumHeight(0);
    m_verticalMainSplit->addWidget(topArea);
    m_verticalMainSplit->addWidget(bottomTabsPane);
    m_verticalMainSplit->setStretchFactor(0, 81);
    m_verticalMainSplit->setStretchFactor(1, 19);
    m_verticalMainSplit->setSizes({810, 190});
    root->addWidget(m_verticalMainSplit, 1);

    setCentralWidget(central);
    if (!m_mainWindowGeometryState.isEmpty()) {
        restoreGeometry(m_mainWindowGeometryState);
    }
    if (m_topMainSplit && !m_topMainSplitState.isEmpty()) {
        m_topMainSplit->restoreState(m_topMainSplitState);
    }
    if (m_rightMainSplit && !m_rightMainSplitState.isEmpty()) {
        m_rightMainSplit->restoreState(m_rightMainSplitState);
    }
    if (m_verticalMainSplit && !m_verticalMainSplitState.isEmpty()) {
        m_verticalMainSplit->restoreState(m_verticalMainSplitState);
    }
    if (m_verticalMainSplit) {
        m_verticalMainSplit->setOrientation(Qt::Vertical);
    }

    // Y el REPARTO del divisor, que es lo que de verdad dejaba el hueco.
    //
    // Medido: el divisor asignaba [314, 220] mientras el panel de la banda medía 31 px
    // por su altura máxima. Los 189 de diferencia no los ocupaba nadie: eran hueco muerto
    // DENTRO de la asignación, así que daba igual cuánto se encogiera el contenido.
    // Los 220 venían del setSizes inicial y del estado guardado en config.json, y se
    // reponían en cada arranque.
    //
    // Va después de restoreState a propósito: antes lo pisaría el estado restaurado. A
    // partir de aquí el reparto correcto se guarda al cerrar y ya se sostiene solo.

    int minLogsHeight = bottomTabsPane->sizeHint().height();
    if (m_verticalMainSplit) {
        const QList<int> mainSizes = m_verticalMainSplit->sizes();
        if (mainSizes.size() >= 2 && mainSizes.at(1) > 0) {
            minLogsHeight = qMax(minLogsHeight, mainSizes.at(1));
        }
    }
    minLogsHeight = qMax(1, minLogsHeight / 2);

    if (m_verticalMainSplit) {
        connect(m_verticalMainSplit, &QSplitter::splitterMoved, this, [this, minLogsHeight](int, int) {
            if (!m_verticalMainSplit || m_verticalMainSplit->property("_enforcingMinLogs").toBool()) {
                return;
            }
            const QList<int> sizes = m_verticalMainSplit->sizes();
            if (sizes.size() < 2) {
                return;
            }
            const int upper = sizes.at(0);
            const int lower = sizes.at(1);
            if (lower == 0 || lower >= minLogsHeight) {
                return;
            }
            const int total = upper + lower;
            if (total <= minLogsHeight) {
                return;
            }
            m_verticalMainSplit->setProperty("_enforcingMinLogs", true);
            m_verticalMainSplit->setSizes({total - minLogsHeight, minLogsHeight});
            m_verticalMainSplit->setProperty("_enforcingMinLogs", false);
        });
    }


    struct PermissionMenuActions {
        QAction* refreshPerms{nullptr};
        QAction* newGrant{nullptr};
        QAction* newSet{nullptr};
        QAction* editGrant{nullptr};
        QAction* deleteGrant{nullptr};
        QAction* renameSet{nullptr};
        QAction* deleteSet{nullptr};
    };


    if (m_connContentTree) {
        connect(m_connContentTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) {});
    }
    if (m_btnApplyConnContentProps) {
        connect(m_btnApplyConnContentProps, &QPushButton::clicked, this, [this]() {
            logUiAction(QStringLiteral("Aplicar cambios (botón flotante)"));
            applyDatasetPropertyChanges();
        });
    }
    if (m_btnDiscardPendingChanges) {
        connect(m_btnDiscardPendingChanges, &QPushButton::clicked, this, [this]() {
            logUiAction(QStringLiteral("Deshacer cambios (panel pendientes)"));
            // Confirmación siempre que haya algo que perder, enumerando QUÉ se lleva.
            //
            // El botón descarta ediciones a medias, y se dice CUÁLES antes de hacerlo.
            //
            // Antes vaciaba además la cola de acciones —que podía llevar días guardada en
            // disco—, y por eso el aviso enumeraba tres cosas. Ya no hay cola: quedan los
            // borradores de propiedades y de permisos, que siguen mereciendo el aviso porque
            // se pierden sin vuelta atrás.
            const int queuedActions = 0;
            const int propertyDrafts = pendingConnContentPropertyDraftsFromModel().size();
            const int permissionDrafts = dirtyDatasetPermissionsEntriesFromModel().size();
            if (queuedActions > 0 || propertyDrafts > 0 || permissionDrafts > 0) {
                QStringList bullets;
                if (queuedActions > 0) {
                    bullets << trk(QStringLiteral("t_empty_list_item_actions001"),
                                   QStringLiteral("  •  %1 acción(es) de la lista, incluida su "
                                                  "copia guardada en disco"),
                                   QStringLiteral("  •  %1 action(s) from the list, including "
                                                  "the copy saved on disk"))
                                   .arg(queuedActions);
                }
                if (propertyDrafts > 0) {
                    bullets << trk(QStringLiteral("t_empty_list_item_props001"),
                                   QStringLiteral("  •  cambios de propiedades sin aplicar en "
                                                  "%1 objeto(s)"),
                                   QStringLiteral("  •  unapplied property changes on %1 object(s)"))
                                   .arg(propertyDrafts);
                }
                if (permissionDrafts > 0) {
                    bullets << trk(QStringLiteral("t_empty_list_item_perms001"),
                                   QStringLiteral("  •  cambios de permisos sin aplicar en "
                                                  "%1 dataset(s)"),
                                   QStringLiteral("  •  unapplied permission changes on %1 dataset(s)"))
                                   .arg(permissionDrafts);
                }
                const auto choice = QMessageBox::question(
                    this,
                    trk(QStringLiteral("t_empty_list_title001"),
                        QStringLiteral("Descartar los cambios sin aplicar"),
                        QStringLiteral("Discard the unapplied changes")),
                    trk(QStringLiteral("t_empty_list_body001"),
                        QStringLiteral("Se va a descartar:\n\n%1\n\nNo afecta a lo ya "
                                       "ejecutado, y no se puede deshacer.\n\n¿Descartarlos?"),
                        QStringLiteral("The following will be discarded:\n\n%1\n\nThis does not "
                                       "affect what has already run, and cannot be undone.\n\n"
                                       "Discard them?"))
                        .arg(bullets.join(QStringLiteral("\n"))),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (choice != QMessageBox::Yes) {
                    appLog(QStringLiteral("INFO"),
                           QStringLiteral("[pendientes] vaciado cancelado: %1 acciones intactas")
                               .arg(queuedActions));
                    return;
                }
                appLog(QStringLiteral("NORMAL"),
                       QStringLiteral("[pendientes] lista vaciada a petición del usuario: "
                                      "%1 acciones, %2 borradores de propiedades, %3 de permisos")
                           .arg(queuedActions).arg(propertyDrafts).arg(permissionDrafts));
            }
            discardAllDraftEdits();
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
    updateConnectionActionsState();
}
