#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "peticiones.h"
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
// muestra «… (N)», que es cuando tiene algo pendiente. Ninguna otra lleva paréntesis.
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

void MainWindow::splitAndRootConnContent(Qt::Orientation orientation, bool insertBefore,
                                          int connIdx, const QString& poolName,
                                          const QString& rootDataset, QTreeWidget* sourceTree) {
    if (!m_connContentPage || connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return;
    }
    const bool isConnectionLevel = poolName.trimmed().isEmpty();
    if (!isConnectionLevel && rootDataset.trimmed().isEmpty()) {
        return;
    }
    const ConnectionProfile p = m_conns.profiles.at(connIdx);
    const QString connName = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
    const QString trimmedRoot = rootDataset.trimmed();
    const QString trimmedPool = poolName.trimmed();
    const QString displayRoot = isConnectionLevel
        ? connName
        : ((trimmedRoot.compare(trimmedPool, Qt::CaseInsensitive) == 0)
               ? QStringLiteral("%1::%2").arg(connName, trimmedPool)
               : QStringLiteral("%1::%2").arg(connName, trimmedRoot));

    auto* delegate = new MainWindowConnectionDatasetTreeDelegate(this, this);
    ConnectionDatasetTreeWidget::Config config;
    config.treeName = QStringLiteral("splitDatasetTree_%1").arg(m_splitTrees.size());
    config.primaryColumnTitle = m_topDatasetTreeWidget
        ? m_topDatasetTreeWidget->config().primaryColumnTitle
        : QStringLiteral("Dataset");
    config.role = ConnectionDatasetTreePane::Role::Top;
    config.groupPoolsByConnectionRoots = isConnectionLevel;
    auto* splitWidget = new ConnectionDatasetTreeWidget(config, delegate, nullptr);
    auto* splitTree = splitWidget->tree();
    if (splitTree) {
        splitTree->setProperty("zfsmgr.isSplitTree", true);
        // Stable key for this split tree: persists the user-expanded state independently.
        const QString splitKey = connStableIdForIndex(connIdx)
                                 + QStringLiteral("|") + trimmedPool
                                 + QStringLiteral("|") + trimmedRoot;
        splitTree->setProperty("zfsmgr.splitTreeKey", splitKey);
        splitTree->setItemDelegate(new ConnContentPropBorderDelegate(splitTree));
        installConnContentTreeHeaderContextMenu(splitTree);
        // El panel nace con las 4 columnas base que fija el constructor del panel, y sin
        // esto nadie le añadía nunca las de propiedades: el split salía sin columnas, el
        // menú de la cabecera decía «8» —porque ese número sale del ajuste global, no del
        // árbol— y las propiedades de fichero no se escribían, porque se escriben de la
        // columna 4 en adelante y no había ninguna. Se sincroniza ANTES de poblarlo para
        // que las filas nazcan ya con sus celdas.
        splitTree->setProperty("propColumnsSetting", propColumnCountForTree(nullptr));
        syncConnContentPropertyColumnsFor(splitTree, connContentTokenForTree(splitTree));
        if (isConnectionLevel) {
            appendSplitDatasetTreeForConnection(splitTree, connIdx);
        } else {
            appendSplitDatasetTree(splitTree, connIdx, trimmedPool, trimmedRoot, displayRoot);
        }
        applyUserExpandedState(splitTree);
    }

    // Find the widget that was right-clicked (the panel to split)
    QWidget* sourceWidget = nullptr;
    if (sourceTree) {
        if (m_topDatasetTreeWidget && m_topDatasetTreeWidget->tree() == sourceTree) {
            sourceWidget = m_topDatasetTreeWidget;
        } else {
            for (const SplitTreeEntry& entry : std::as_const(m_splitTrees)) {
                if (entry.treeWidget && entry.treeWidget->tree() == sourceTree) {
                    sourceWidget = entry.treeWidget;
                    break;
                }
            }
        }
    }
    if (!sourceWidget) {
        sourceWidget = m_topDatasetTreeWidget;
    }

    // Replace sourceWidget's position with a new splitter.
    // insertBefore=false: [sourceWidget, splitWidget] (Right/Below)
    // insertBefore=true:  [splitWidget, sourceWidget] (Left/Above)
    auto* newSplitter = new QSplitter(orientation, nullptr);
    QSplitter* parentSplitter = qobject_cast<QSplitter*>(sourceWidget->parentWidget());

    if (parentSplitter) {
        // sourceWidget is inside another splitter — insert newSplitter at the same slot.
        // Save parent sizes first so inserting the inner splitter doesn't disturb sibling panels.
        const QList<int> parentSizes = parentSplitter->sizes();
        const int idx = parentSplitter->indexOf(sourceWidget);
        parentSplitter->insertWidget(idx, newSplitter);
        if (insertBefore) {
            newSplitter->addWidget(splitWidget);
            newSplitter->addWidget(sourceWidget);
        } else {
            newSplitter->addWidget(sourceWidget);
            newSplitter->addWidget(splitWidget);
        }
        parentSplitter->setSizes(parentSizes);
    } else {
        // sourceWidget is directly in the layout
        auto* layout = qobject_cast<QVBoxLayout*>(m_connContentPage->layout());
        if (layout) {
            const int idx = layout->indexOf(sourceWidget);
            const int stretch = (idx >= 0) ? 1 : 0;
            if (idx >= 0) {
                layout->removeWidget(sourceWidget);
            }
            newSplitter->setParent(m_connContentPage);
            layout->insertWidget(qMax(0, idx), newSplitter, stretch);
            if (insertBefore) {
                newSplitter->addWidget(splitWidget);
                newSplitter->addWidget(sourceWidget);
            } else {
                newSplitter->addWidget(sourceWidget);
                newSplitter->addWidget(splitWidget);
            }
            if (!m_connContentTreeSplitter) {
                m_connContentTreeSplitter = newSplitter;
            }
        }
    }

    // Split the available space equally between source and new panel.
    QPointer<QSplitter> splitterPtr(newSplitter);
    QTimer::singleShot(0, this, [splitterPtr]() {
        if (!splitterPtr || splitterPtr->count() != 2) {
            return;
        }
        const int total = (splitterPtr->orientation() == Qt::Horizontal)
                              ? splitterPtr->width()
                              : splitterPtr->height();
        if (total > 0) {
            const int half = total / 2;
            splitterPtr->setSizes({half, half});
        }
    });

    SplitTreeEntry entry;
    entry.connIdx = connIdx;
    entry.poolName = trimmedPool;  // empty for connection-level splits
    entry.rootDataset = trimmedRoot;
    entry.displayRoot = displayRoot;
    entry.treeWidget = splitWidget;
    entry.delegate = delegate;
    m_splitTrees.push_back(entry);
    saveUiSettings();
}

void MainWindow::closeSplitTree(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    for (int i = 0; i < m_splitTrees.size(); ++i) {
        const SplitTreeEntry& entry = m_splitTrees.at(i);
        if (entry.treeWidget && entry.treeWidget->tree() == tree) {
            ConnectionDatasetTreeWidget* widget = entry.treeWidget;
            m_splitTrees.removeAt(i);

            if (widget) {
                QSplitter* parentSplitter = qobject_cast<QSplitter*>(widget->parentWidget());
                widget->setParent(nullptr);

                // If the parent splitter now has a single child, unwrap it
                if (parentSplitter && parentSplitter->count() == 1) {
                    QWidget* remaining = parentSplitter->widget(0);
                    QSplitter* grandSplitter =
                        qobject_cast<QSplitter*>(parentSplitter->parentWidget());

                    if (grandSplitter) {
                        // Replace slot in grandparent splitter
                        const int idx = grandSplitter->indexOf(parentSplitter);
                        grandSplitter->insertWidget(idx, remaining);
                    } else {
                        // Parent is directly in the layout
                        auto* layout = qobject_cast<QVBoxLayout*>(
                            m_connContentPage ? m_connContentPage->layout() : nullptr);
                        if (layout) {
                            const int idx = layout->indexOf(parentSplitter);
                            layout->removeWidget(parentSplitter);
                            remaining->setParent(m_connContentPage);
                            layout->insertWidget(qMax(0, idx), remaining, 1);
                        }
                    }
                    if (m_connContentTreeSplitter == parentSplitter) {
                        m_connContentTreeSplitter =
                            qobject_cast<QSplitter*>(remaining);
                    }
                    parentSplitter->deleteLater();
                }

                // If no split trees remain, put main tree back directly in layout
                if (m_splitTrees.isEmpty() && m_topDatasetTreeWidget) {
                    QSplitter* rootSplitter =
                        qobject_cast<QSplitter*>(m_topDatasetTreeWidget->parentWidget());
                    if (rootSplitter) {
                        auto* layout = qobject_cast<QVBoxLayout*>(
                            m_connContentPage ? m_connContentPage->layout() : nullptr);
                        if (layout) {
                            // Walk up to find the splitter directly in the layout
                            QWidget* topLevel = rootSplitter;
                            while (qobject_cast<QSplitter*>(topLevel->parentWidget())) {
                                topLevel = topLevel->parentWidget();
                            }
                            const int idx = layout->indexOf(topLevel);
                            layout->removeWidget(topLevel);
                            m_topDatasetTreeWidget->setParent(m_connContentPage);
                            layout->insertWidget(qMax(0, idx), m_topDatasetTreeWidget, 1);
                            topLevel->deleteLater();
                        }
                        m_connContentTreeSplitter = nullptr;
                    }
                }

                widget->deleteLater();
            }
            saveUiSettings();
            return;
        }
    }
}

void MainWindow::rebuildAllSplitTrees() {
    for (const SplitTreeEntry& entry : std::as_const(m_splitTrees)) {
        if (!entry.treeWidget) {
            continue;
        }
        QTreeWidget* t = entry.treeWidget->tree();
        if (!t) {
            continue;
        }
        {
            const QSignalBlocker blocker(t);
            t->clear();
            if (entry.poolName.trimmed().isEmpty()) {
                appendSplitDatasetTreeForConnection(t, entry.connIdx);
            } else {
                appendSplitDatasetTree(t, entry.connIdx, entry.poolName, entry.rootDataset, entry.displayRoot);
            }
            applyUserExpandedState(t);
        }
    }
}

QString MainWindow::serializeSplitTreeLayoutState() const {
    if (!m_connContentPage || !m_topDatasetTreeWidget) {
        return QString();
    }
    auto findSplitEntryForWidget = [this](const ConnectionDatasetTreeWidget* widget) -> const SplitTreeEntry* {
        if (!widget) {
            return nullptr;
        }
        for (const SplitTreeEntry& e : m_splitTrees) {
            if (e.treeWidget == widget) {
                return &e;
            }
        }
        return nullptr;
    };
    std::function<QJsonObject(QWidget*)> encodeNode = [&](QWidget* widget) -> QJsonObject {
        QJsonObject out;
        if (!widget) {
            return out;
        }
        if (widget == m_topDatasetTreeWidget) {
            out.insert(QStringLiteral("kind"), QStringLiteral("main"));
            return out;
        }
        if (auto* splitter = qobject_cast<QSplitter*>(widget)) {
            out.insert(QStringLiteral("kind"), QStringLiteral("split"));
            out.insert(QStringLiteral("orientation"),
                       splitter->orientation() == Qt::Horizontal ? QStringLiteral("h")
                                                                 : QStringLiteral("v"));
            QJsonArray sizes;
            for (int s : splitter->sizes()) {
                sizes.push_back(s);
            }
            out.insert(QStringLiteral("sizes"), sizes);
            QJsonArray children;
            for (int i = 0; i < splitter->count(); ++i) {
                if (QWidget* child = splitter->widget(i)) {
                    children.push_back(encodeNode(child));
                }
            }
            out.insert(QStringLiteral("children"), children);
            return out;
        }
        if (auto* splitWidget = qobject_cast<ConnectionDatasetTreeWidget*>(widget)) {
            const SplitTreeEntry* entry = findSplitEntryForWidget(splitWidget);
            if (!entry) {
                return out;
            }
            out.insert(QStringLiteral("kind"), QStringLiteral("splitTree"));
            out.insert(QStringLiteral("conn"), connectionPersistKey(entry->connIdx));
            out.insert(QStringLiteral("pool"), entry->poolName.trimmed());
            out.insert(QStringLiteral("dataset"), entry->rootDataset.trimmed());
            return out;
        }
        return out;
    };

    auto* layout = qobject_cast<QVBoxLayout*>(m_connContentPage->layout());
    if (!layout || layout->count() <= 0) {
        return QString();
    }
    QWidget* rootWidget = nullptr;
    for (int i = 0; i < layout->count(); ++i) {
        if (QLayoutItem* item = layout->itemAt(i)) {
            if (QWidget* w = item->widget()) {
                rootWidget = w;
                break;
            }
        }
    }
    if (!rootWidget) {
        return QString();
    }
    const QJsonObject root = encodeNode(rootWidget);
    if (root.isEmpty()) {
        return QString();
    }
    QJsonObject doc;
    doc.insert(QStringLiteral("version"), 1);
    doc.insert(QStringLiteral("root"), root);
    return QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
}

void MainWindow::restoreSplitTreeLayoutFromState(const QString& state) {
    if (!m_connContentPage || !m_topDatasetTreeWidget || state.trimmed().isEmpty()) {
        return;
    }
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(state.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }
    const QJsonObject rootObj = doc.object().value(QStringLiteral("root")).toObject();
    if (rootObj.isEmpty()) {
        return;
    }

    auto connIdxByPersistKey = [this](const QString& key) -> int {
        const QString wanted = key.trimmed().toLower();
        if (wanted.isEmpty()) {
            return -1;
        }
        for (int i = 0; i < m_conns.profiles.size(); ++i) {
            const QString k = connectionPersistKey(i).trimmed().toLower();
            if (!k.isEmpty() && k == wanted) {
                return i;
            }
        }
        return -1;
    };

    auto* layout = qobject_cast<QVBoxLayout*>(m_connContentPage->layout());
    if (!layout) {
        return;
    }

    for (const SplitTreeEntry& e : std::as_const(m_splitTrees)) {
        if (e.treeWidget) {
            e.treeWidget->setParent(nullptr);
            e.treeWidget->deleteLater();
        }
    }
    m_splitTrees.clear();
    m_connContentTreeSplitter = nullptr;

    QWidget* existingRoot = nullptr;
    for (int i = 0; i < layout->count(); ++i) {
        if (QLayoutItem* item = layout->itemAt(i)) {
            if (QWidget* w = item->widget()) {
                existingRoot = w;
                break;
            }
        }
    }
    if (existingRoot && existingRoot != m_topDatasetTreeWidget) {
        layout->removeWidget(existingRoot);
        existingRoot->deleteLater();
    }
    if (m_topDatasetTreeWidget->parentWidget() != m_connContentPage) {
        m_topDatasetTreeWidget->setParent(m_connContentPage);
    }
    if (layout->indexOf(m_topDatasetTreeWidget) < 0) {
        layout->insertWidget(0, m_topDatasetTreeWidget, 1);
    }

    std::function<QWidget*(const QJsonObject&)> buildNode = [&](const QJsonObject& node) -> QWidget* {
        const QString kind = node.value(QStringLiteral("kind")).toString().trimmed();
        if (kind == QStringLiteral("main")) {
            return m_topDatasetTreeWidget;
        }
        if (kind == QStringLiteral("splitTree")) {
            const int connIdx = connIdxByPersistKey(node.value(QStringLiteral("conn")).toString());
            const QString pool = node.value(QStringLiteral("pool")).toString().trimmed();
            const QString dataset = node.value(QStringLiteral("dataset")).toString().trimmed();
            if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
                return nullptr;
            }
            const ConnectionProfile p = m_conns.profiles.at(connIdx);
            const QString connName = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
            const bool isConnectionLevel = pool.isEmpty();
            const QString displayRoot = isConnectionLevel
                ? connName
                : ((dataset.compare(pool, Qt::CaseInsensitive) == 0)
                       ? QStringLiteral("%1::%2").arg(connName, pool)
                       : QStringLiteral("%1::%2").arg(connName, dataset));

            auto* delegate = new MainWindowConnectionDatasetTreeDelegate(this, this);
            ConnectionDatasetTreeWidget::Config config;
            config.treeName = QStringLiteral("splitDatasetTree_%1").arg(m_splitTrees.size());
            config.primaryColumnTitle = m_topDatasetTreeWidget
                ? m_topDatasetTreeWidget->config().primaryColumnTitle
                : QStringLiteral("Dataset");
            config.role = ConnectionDatasetTreePane::Role::Top;
            config.groupPoolsByConnectionRoots = isConnectionLevel;
            auto* splitWidget = new ConnectionDatasetTreeWidget(config, delegate, nullptr);
            if (QTreeWidget* splitTree = splitWidget->tree()) {
                splitTree->setProperty("zfsmgr.isSplitTree", true);
                splitTree->setItemDelegate(new ConnContentPropBorderDelegate(splitTree));
                installConnContentTreeHeaderContextMenu(splitTree);
                if (isConnectionLevel) {
                    appendSplitDatasetTreeForConnection(splitTree, connIdx);
                } else {
                    appendSplitDatasetTree(splitTree, connIdx, pool, dataset, displayRoot);
                }
            }
            SplitTreeEntry entry;
            entry.connIdx = connIdx;
            entry.poolName = pool;
            entry.rootDataset = dataset;
            entry.displayRoot = displayRoot;
            entry.treeWidget = splitWidget;
            entry.delegate = delegate;
            m_splitTrees.push_back(entry);
            return splitWidget;
        }
        if (kind == QStringLiteral("split")) {
            auto* splitter = new QSplitter(
                node.value(QStringLiteral("orientation")).toString() == QStringLiteral("h")
                    ? Qt::Horizontal
                    : Qt::Vertical,
                nullptr);
            const QJsonArray children = node.value(QStringLiteral("children")).toArray();
            for (const QJsonValue& childVal : children) {
                if (!childVal.isObject()) {
                    continue;
                }
                QWidget* childWidget = buildNode(childVal.toObject());
                if (childWidget) {
                    splitter->addWidget(childWidget);
                }
            }
            const QJsonArray sizesArr = node.value(QStringLiteral("sizes")).toArray();
            if (sizesArr.size() == splitter->count()) {
                QList<int> sizes;
                sizes.reserve(sizesArr.size());
                for (const QJsonValue& v : sizesArr) {
                    sizes.push_back(v.toInt());
                }
                splitter->setSizes(sizes);
            }
            return splitter;
        }
        return nullptr;
    };

    QWidget* rebuiltRoot = buildNode(rootObj);
    if (!rebuiltRoot || rebuiltRoot == m_topDatasetTreeWidget) {
        m_connContentTreeSplitter = nullptr;
        return;
    }
    if (layout->indexOf(m_topDatasetTreeWidget) >= 0) {
        layout->removeWidget(m_topDatasetTreeWidget);
    }
    rebuiltRoot->setParent(m_connContentPage);
    layout->insertWidget(0, rebuiltRoot, 1);
    m_connContentTreeSplitter = qobject_cast<QSplitter*>(rebuiltRoot);
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
        m_conns.store.setLanguage(m_language);
        saveUiSettings();
        appLog(QStringLiteral("INFO"), QStringLiteral("Idioma cambiado a %1").arg(m_language));
        applyLanguageLive();
    });

    m_connectivityMatrixAction = appMenu->addAction(
        trk(QStringLiteral("t_connectivity_menu_001"),
            QStringLiteral("Comprobar conectividad"),
            QStringLiteral("Check connectivity"),
            QStringLiteral("检查连通性")));
    connect(m_connectivityMatrixAction, &QAction::triggered, this, [this]() {
        logUiAction(QStringLiteral("Comprobar conectividad (menú)"));
        openConnectivityMatrixDialog();
    });

    m_confirmActionsMenuAction = nullptr;


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
            QStringLiteral("Acciones"),
            QStringLiteral("Actions"),
            QStringLiteral("操作")));
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

    // El rótulo decía «archivos INI». Ese formato se retiró hace tiempo —la configuración
    // es config.json y trust-store.json— y el TEMA ya estaba corregido, pero la entrada de
    // menú que lo abre seguía nombrando un fichero que no existe.
    QAction* cfgFilesHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_cfg_002"),
            QStringLiteral("Configuración y archivos"),
            QStringLiteral("Configuration and files"),
            QStringLiteral("配置与文件")));
    connect(cfgFilesHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("configuracion_archivos"),
                      trk(QStringLiteral("t_help_cfg_002"),
                          QStringLiteral("Configuración y archivos"),
                          QStringLiteral("Configuration and files"),
                          QStringLiteral("配置与文件")));
    });

    QAction* cliHelpAct = helpMenu->addAction(
        trk(QStringLiteral("t_help_cli_001"),
            QStringLiteral("Línea de órdenes"),
            QStringLiteral("Command line"),
            QStringLiteral("命令行")));
    connect(cliHelpAct, &QAction::triggered, this, [this]() {
        openHelpTopic(QStringLiteral("linea_de_ordenes"),
                      trk(QStringLiteral("t_help_cli_001"),
                          QStringLiteral("Línea de órdenes"),
                          QStringLiteral("Command line"),
                          QStringLiteral("命令行")));
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
            QStringLiteral("Source:(empty)"),
            QStringLiteral("源：（空）")),
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
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        connectionsTab);
    m_btnDiscardPendingChanges = new QPushButton(
        trk(QStringLiteral("t_discard_changes_001"),
            QStringLiteral("Vaciar lista"),
            QStringLiteral("Empty list"),
            QStringLiteral("清空列表")),
        connectionsTab);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    m_btnApplyConnContentProps->setFont(baseUiFont);
    m_btnDiscardPendingChanges->setFont(baseUiFont);
    m_btnApplyConnContentProps->setMinimumHeight(stdLeftBtnH);
    m_btnDiscardPendingChanges->setMinimumHeight(stdLeftBtnH);
    m_btnApplyConnContentProps->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnDiscardPendingChanges->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // Aquí vivía una rejilla 3x2 con Sincronizar, Copiar, Clonar, Mover, Nivelar y Diff.
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
    m_btnApplyConnContentProps->setParent(pendingChangesBox);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_btnDiscardPendingChanges->setParent(pendingChangesBox);
    pendingButtonsCol->addWidget(m_btnApplyConnContentProps, 0, Qt::AlignLeft | Qt::AlignTop);
    pendingButtonsCol->addWidget(m_btnDiscardPendingChanges, 0, Qt::AlignLeft | Qt::AlignTop);
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
    // Los botones Aplicar y Deshacer de la columna izquierda SE QUEDAN: siguen sirviendo a
    // las propiedades y los permisos, que se editan en el árbol y sí se aplican en lote. No
    // hay ninguna tabla junto a la que ponerlos —`m_connContentPropsTable` nunca llegó a
    // asignarse, es un miembro muerto—, así que este panel sigue siendo su sitio.
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
    poolPropsPageLayout->addWidget(m_poolPropsTable, 1);
    auto* poolStatusBox = new QGroupBox(
        trk(QStringLiteral("t_pool_status_box_001"),
            QStringLiteral("Estado del pool"),
            QStringLiteral("Pool status"),
            QStringLiteral("存储池状态")),
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
    delete m_topConnContentDelegate;
    m_topConnContentDelegate = new MainWindowConnectionDatasetTreeDelegate(this, this);
    ConnectionDatasetTreeWidget::Config topTreeConfig;
    topTreeConfig.treeName = QStringLiteral("originDatasetTreeWidget");
    topTreeConfig.primaryColumnTitle = trk(QStringLiteral("t_unified_dataset_col001"),
                                           QStringLiteral("Conexión/Pool/Dataset"),
                                           QStringLiteral("Connection/Pool/Dataset"),
                                           QStringLiteral("连接/存储池/数据集"));
    topTreeConfig.role = ConnectionDatasetTreePane::Role::Unified;
    topTreeConfig.groupPoolsByConnectionRoots = true;
    m_topDatasetTreeWidget = new ConnectionDatasetTreeWidget(topTreeConfig, m_topConnContentDelegate, m_connContentPage);
    m_topDatasetPane = m_topDatasetTreeWidget->pane();
    m_connContentTree = m_topDatasetTreeWidget->tree();
    m_connContentTree->setItemDelegate(new ConnContentPropBorderDelegate(m_connContentTree));
    // Las acciones se exponen por menú contextual del árbol.
    connContentLayout->addWidget(m_topDatasetTreeWidget, 1);
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
    m_bottomDatasetTreeWidget = nullptr;
    m_bottomConnContentTree = nullptr;
    // Mantener esquema de columnas idéntico en ambos árboles (superior/inferior)
    // incluso cuando uno de ellos esté vacío.
    syncConnContentPropertyColumnsFor(m_connContentTree, connContentTokenForTree(m_connContentTree));
    installConnContentTreeHeaderContextMenu(m_connContentTree);

    m_logsTabs = new QTabWidget(central);
    m_logsTabs->setObjectName(QStringLiteral("zfsmgrLogTabs"));

    auto* settingsTab = new QWidget(m_logsTabs);
    auto* settingsLayout = new QVBoxLayout(settingsTab);
    settingsLayout->setContentsMargins(8, 8, 8, 8);
    settingsLayout->setSpacing(8);

    auto* logsSettingsBox = new QGroupBox(
        trk(QStringLiteral("t_logs_menu_001"),
            QStringLiteral("Logs"),
            QStringLiteral("Logs"),
            QStringLiteral("日志")),
        settingsTab);
    auto* logsSettingsLayout = new QFormLayout(logsSettingsBox);
    logsSettingsLayout->setContentsMargins(8, 8, 8, 8);
    logsSettingsLayout->setSpacing(6);
    logsSettingsLayout->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);

    auto* logLevelCombo = new QComboBox(logsSettingsBox);
    logLevelCombo->addItem(QStringLiteral("normal"), QStringLiteral("normal"));
    logLevelCombo->addItem(QStringLiteral("info"), QStringLiteral("info"));
    logLevelCombo->addItem(QStringLiteral("debug"), QStringLiteral("debug"));
    {
        const int idx = qMax(0, logLevelCombo->findData(m_logLevelSetting));
        logLevelCombo->setCurrentIndex(idx);
    }
    logLevelCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    logLevelCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    logLevelCombo->setMaximumWidth(180);
    connect(logLevelCombo, &QComboBox::currentIndexChanged, this, [this, logLevelCombo](int) {
        const QString level = logLevelCombo->currentData().toString().trimmed().toLower();
        if (level == QStringLiteral("normal")
            || level == QStringLiteral("info")
            || level == QStringLiteral("debug")) {
            m_logLevelSetting = level;
            saveUiSettings();
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
        }
    });

    auto* logLinesCombo = new QComboBox(logsSettingsBox);
    for (int lines : {100, 200, 500, 1000}) {
        logLinesCombo->addItem(QString::number(lines), lines);
    }
    {
        int idx = logLinesCombo->findData(m_logMaxLinesSetting);
        if (idx < 0) {
            idx = logLinesCombo->findData(500);
        }
        logLinesCombo->setCurrentIndex(qMax(0, idx));
    }
    logLinesCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    logLinesCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    logLinesCombo->setMaximumWidth(180);
    connect(logLinesCombo, &QComboBox::currentIndexChanged, this, [this, logLinesCombo](int) {
        const int lines = logLinesCombo->currentData().toInt();
        if (lines == 100 || lines == 200 || lines == 500 || lines == 1000) {
            m_logMaxLinesSetting = lines;
            trimLogWidget(m_logView);
            saveUiSettings();
        }
    });

    auto* logSizeCombo = new QComboBox(logsSettingsBox);
    QList<int> sizesMb = {5, 10, 20, 50, 100, 200, 500, 1024};
    if (!sizesMb.contains(m_logMaxSizeMb)) {
        sizesMb.push_back(qBound(1, m_logMaxSizeMb, 1024));
        std::sort(sizesMb.begin(), sizesMb.end());
        sizesMb.erase(std::unique(sizesMb.begin(), sizesMb.end()), sizesMb.end());
    }
    for (int mb : sizesMb) {
        logSizeCombo->addItem(QStringLiteral("%1 MB").arg(mb), mb);
    }
    {
        int idx = logSizeCombo->findData(m_logMaxSizeMb);
        if (idx < 0) {
            idx = logSizeCombo->findData(10);
        }
        logSizeCombo->setCurrentIndex(qMax(0, idx));
    }
    logSizeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    logSizeCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    logSizeCombo->setMaximumWidth(180);
    connect(logSizeCombo, &QComboBox::currentIndexChanged, this, [this, logSizeCombo](int) {
        const int mb = qBound(1, logSizeCombo->currentData().toInt(), 1024);
        if (mb == m_logMaxSizeMb) {
            return;
        }
        m_logMaxSizeMb = mb;
        saveUiSettings();
        rotateLogIfNeeded();
        appLog(QStringLiteral("INFO"), QStringLiteral("Tamaño máximo de log rotativo: %1 MB").arg(m_logMaxSizeMb));
    });

    auto* logsActionsRow = new QWidget(logsSettingsBox);
    auto* logsActionsLayout = new QHBoxLayout(logsActionsRow);
    logsActionsLayout->setContentsMargins(0, 0, 0, 0);
    logsActionsLayout->setSpacing(6);
    auto* clearLogsBtn = new QPushButton(
        trk(QStringLiteral("t_clear_001"),
            QStringLiteral("Limpiar"),
            QStringLiteral("Clear"),
            QStringLiteral("清空")),
        logsActionsRow);
    auto* copyLogsBtn = new QPushButton(
        trk(QStringLiteral("t_copy_001"),
            QStringLiteral("Copiar"),
            QStringLiteral("Copy"),
            QStringLiteral("复制")),
        logsActionsRow);
    logsActionsLayout->addWidget(clearLogsBtn, 0);
    logsActionsLayout->addWidget(copyLogsBtn, 0);
    logsActionsLayout->addStretch(1);
    connect(clearLogsBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Limpiar log (ajustes)"));
        clearAppLog();
    });
    connect(copyLogsBtn, &QPushButton::clicked, this, [this]() {
        logUiAction(QStringLiteral("Copiar log (ajustes)"));
        copyAppLogToClipboard();
    });
    auto* confirmActionsCb = new QCheckBox(
        trk(QStringLiteral("t_show_confirm_001"),
            QStringLiteral("Mostrar confirmación antes de ejecutar acciones"),
            QStringLiteral("Show confirmation before executing actions"),
            QStringLiteral("执行操作前显示确认")),
        logsSettingsBox);
    confirmActionsCb->setChecked(m_actionConfirmEnabled);
    connect(confirmActionsCb, &QCheckBox::toggled, this, [this](bool checked) {
        m_actionConfirmEnabled = checked;
        saveUiSettings();
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Confirmación de acciones: %1").arg(checked ? QStringLiteral("on")
                                                                          : QStringLiteral("off")));
    });

    auto* combosRow = new QWidget(logsSettingsBox);
    auto* combosLayout = new QHBoxLayout(combosRow);
    combosLayout->setContentsMargins(0, 0, 0, 0);
    combosLayout->setSpacing(10);
    auto* levelLabel = new QLabel(
        trk(QStringLiteral("t_log_level_001"),
            QStringLiteral("Nivel de log"),
            QStringLiteral("Log level"),
            QStringLiteral("日志级别")),
        combosRow);
    auto* linesLabel = new QLabel(
        trk(QStringLiteral("t_log_lines_001"),
            QStringLiteral("Número de líneas"),
            QStringLiteral("Number of lines"),
            QStringLiteral("行数")),
        combosRow);
    auto* sizeLabel = new QLabel(
        trk(QStringLiteral("t_log_max_rot_001"),
            QStringLiteral("Tamaño máximo log rotativo"),
            QStringLiteral("Max rotating log size"),
            QStringLiteral("滚动日志最大大小")),
        combosRow);
    combosLayout->addWidget(levelLabel, 0);
    combosLayout->addWidget(logLevelCombo, 0);
    combosLayout->addWidget(linesLabel, 0);
    combosLayout->addWidget(logLinesCombo, 0);
    combosLayout->addWidget(sizeLabel, 0);
    combosLayout->addWidget(logSizeCombo, 0);
    combosLayout->addStretch(1);
    logsSettingsLayout->addRow(combosRow);
    logsSettingsLayout->addRow(confirmActionsCb);
    logsSettingsLayout->addRow(logsActionsRow);
    settingsLayout->addWidget(logsSettingsBox, 0);
    settingsLayout->addStretch(1);

    auto* combinedLogTab = new QWidget(m_logsTabs);
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
                                       QStringLiteral("Status"),
                                       QStringLiteral("状态")),
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
                                   QStringLiteral("Loading..."),
                                   QStringLiteral("加载中...")));
    statusLayout->addWidget(statusLabel, 0);
    statusLayout->addWidget(m_statusText, 1);

    auto* detailWrap = new QWidget(stateProgressRow);
    auto* detailLayout = new QHBoxLayout(detailWrap);
    detailLayout->setContentsMargins(0, 1, 0, 0);
    detailLayout->setSpacing(6);
    auto* detailLabel = new QLabel(trk(QStringLiteral("t_detail_lbl001"),
                                       QStringLiteral("Progreso"),
                                       QStringLiteral("Progress"),
                                       QStringLiteral("进度")),
                                   detailWrap);
    m_lastDetailText = new QTextEdit(detailWrap);
    m_lastDetailText->setFont(combinedLogFont);
    m_lastDetailText->setReadOnly(true);
    m_lastDetailText->setAcceptRichText(false);
    m_lastDetailText->setLineWrapMode(QTextEdit::NoWrap);
    m_lastDetailText->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lastDetailText->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_lastDetailText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    m_lastDetailText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_lastDetailText->setFixedHeight(22);
    detailLayout->addWidget(detailLabel, 0);
    detailLayout->addWidget(m_lastDetailText, 1);
    // El origen comparte fila con Estado y Progreso en vez de ocupar una tira propia:
    // eran tres franjas apiladas encima del árbol y ahora es una. Va el último y sin
    // estirar, que es lo que menos ancho roba a los otros dos.
    stateProgressLayout->addWidget(m_connOriginSelectionLabel, 1);
    stateProgressLayout->addWidget(statusWrap, 1);
    stateProgressLayout->addWidget(detailWrap, 3);

    auto* appLogBox = new QGroupBox(trk(QStringLiteral("t_app_tab_001"),
                                        QStringLiteral("Aplicación"),
                                        QStringLiteral("Application"),
                                        QStringLiteral("应用")),
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
    auto* topBottomPane = new QWidget(topArea);
    auto* topBottomLayout = new QVBoxLayout(topBottomPane);
    topBottomLayout->setContentsMargins(0, 0, 0, 0);
    topBottomLayout->setSpacing(2);
    topBottomLayout->addWidget(stateProgressRow, 0);
    // `leftPane` NO se añade: contiene la pestaña de Conexiones, que se quedó sin un solo
    // widget cuando la lista de cambios pendientes se mudó a las pestañas de abajo —los
    // árboles viven en m_connContentPage, en el panel de arriba—. Con estiramiento 1
    // primero, y con 0 después, seguía siendo un widget vacío al que el divisor le daba
    // su alto guardado: eso era la banda enorme y vacía. Se oculta en vez de borrarse
    // porque sigue siendo el padre de widgets que ya se reubicaron.
    leftPane->hide();
    const int defaultBottomInfoMinHeight = stateProgressRow->sizeHint().height() + 2;
    topBottomPane->setMinimumHeight(defaultBottomInfoMinHeight);

    // UN SOLO divisor horizontal, y la banda de Origen/Estado/Progreso baja con las
    // pestañas.
    //
    // Antes había dos: uno entre el árbol y la banda, y otro entre la banda y las
    // pestañas. El de arriba no se podía arrastrar —la banda tiene techo de una fila, así
    // que no había nada que repartir— y quedaba como un asa muerta en medio de la
    // ventana. Ahora la banda va pegada a las pestañas, en su mismo panel, y el único
    // divisor separa el árbol de todo lo demás, que es el reparto que de verdad se toca.
    topBottomPane->setMaximumHeight(stateProgressRow->sizeHint().height() + 8);
    topLayout->addWidget(m_rightStack, 1);
    loadPersistedAppLogToView();

    m_pendingChangesTab = pendingChangesBox;
    // La pestaña ya no es «Cambios pendientes»: no hay nada pendiente. Enseña lo que está
    // corriendo ahora mismo, que es lo que uno quiere tener a la vista mientras trabaja.
    m_logsTabs->addTab(pendingChangesBox,
                       trk(QStringLiteral("t_jobs_tab_001"),
                           QStringLiteral("Transferencias"),
                           QStringLiteral("Transfers"),
                           QStringLiteral("传输")));
    m_logsTabs->addTab(settingsTab,
                       trk(QStringLiteral("t_settings_tab_001"),
                           QStringLiteral("Ajustes"),
                           QStringLiteral("Settings"),
                           QStringLiteral("设置")));
    m_logsTabs->addTab(combinedLogTab,
                       trk(QStringLiteral("t_combined_log001"),
                           QStringLiteral("Log combinado"),
                           QStringLiteral("Combined log"),
                           QStringLiteral("组合日志")));

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
                                                QStringLiteral("Cancel selected"),
                                                QStringLiteral("取消所选")), m_pendingChangesTab);
        auto* refreshBtn = new QPushButton(trk(QStringLiteral("t_jobs_refresh001"),
                                                QStringLiteral("Refrescar"),
                                                QStringLiteral("Refresh"),
                                                QStringLiteral("刷新")), m_pendingChangesTab);
        if (m_pendingButtonsCol) {
            m_pendingButtonsCol->insertWidget(2, refreshBtn, 0, Qt::AlignLeft | Qt::AlignTop);
            m_pendingButtonsCol->insertWidget(3, cancelBtn, 0, Qt::AlignLeft | Qt::AlignTop);
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
                zfsmgr::commands::peticiones::cancelaTrabajo(jobId.toStdString()));
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

    // Por nombre, no por índice: al meter «Cambios pendientes» delante, el 1 dejó de ser
    // el log combinado.
    m_logsTabs->setCurrentIndex(qMax(0, m_logsTabs->indexOf(combinedLogTab)));

    auto* bottomTabsPane = new QWidget(central);
    auto* bottomTabsLayout = new QVBoxLayout(bottomTabsPane);
    bottomTabsLayout->setContentsMargins(0, 0, 0, 0);
    bottomTabsLayout->setSpacing(6);
    if (m_logsTabs->tabBar()) {
        // El estilo va SOLO en la barra de pestañas, no en toda la aplicación.
        m_logsTabs->tabBar()->setStyle(new CountedTabStyle(m_logsTabs->tabBar()->style()));
    }
    // La banda primero y sin estirar; las pestañas se quedan con el resto.
    bottomTabsLayout->addWidget(topBottomPane, 0);
    bottomTabsLayout->addWidget(m_logsTabs, 1);

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
    // La altura de la banda sale del CONTENIDO y de nada más.
    //
    // Antes se tomaba del divisor que la contenía —el alto que tuviera al guardarse— y se
    // fijaba como mínimo, así que el alto de ayer era el suelo de hoy y no bajaba nunca.
    // Ese divisor ya no existe: la banda va en el panel de las pestañas con estiramiento
    // cero, o sea que ocupa exactamente lo que necesita.
    topBottomPane->setMinimumHeight(qMax(1, topBottomPane->sizeHint().height()));

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
                                                  "the copy saved on disk"),
                                   QStringLiteral("  •  列表中的 %1 项操作，包括保存在磁盘上的副本"))
                                   .arg(queuedActions);
                }
                if (propertyDrafts > 0) {
                    bullets << trk(QStringLiteral("t_empty_list_item_props001"),
                                   QStringLiteral("  •  cambios de propiedades sin aplicar en "
                                                  "%1 objeto(s)"),
                                   QStringLiteral("  •  unapplied property changes on %1 object(s)"),
                                   QStringLiteral("  •  %1 个对象上尚未应用的属性更改"))
                                   .arg(propertyDrafts);
                }
                if (permissionDrafts > 0) {
                    bullets << trk(QStringLiteral("t_empty_list_item_perms001"),
                                   QStringLiteral("  •  cambios de permisos sin aplicar en "
                                                  "%1 dataset(s)"),
                                   QStringLiteral("  •  unapplied permission changes on %1 dataset(s)"),
                                   QStringLiteral("  •  %1 个数据集上尚未应用的权限更改"))
                                   .arg(permissionDrafts);
                }
                const auto choice = QMessageBox::question(
                    this,
                    trk(QStringLiteral("t_empty_list_title001"),
                        QStringLiteral("Vaciar la lista de cambios pendientes"),
                        QStringLiteral("Empty the pending changes list"),
                        QStringLiteral("清空待应用更改列表")),
                    trk(QStringLiteral("t_empty_list_body001"),
                        QStringLiteral("Se va a descartar:\n\n%1\n\nNo afecta a lo ya "
                                       "ejecutado, y no se puede deshacer.\n\n¿Vaciar la lista?"),
                        QStringLiteral("The following will be discarded:\n\n%1\n\nThis does not "
                                       "affect what has already run, and cannot be undone.\n\n"
                                       "Empty the list?"),
                        QStringLiteral("将丢弃以下内容：\n\n%1\n\n这不会影响已执行的操作，且无法"
                                       "撤销。\n\n是否清空列表？"))
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
