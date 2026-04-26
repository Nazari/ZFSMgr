#include "mainwindow.h"
#include "mainwindow_helpers.h"
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

QString pendingChangeFirstQuotedArg(const QString& text) {
    const int firstQuote = text.indexOf(QLatin1Char('\''));
    if (firstQuote < 0) {
        return QString();
    }
    const int nextQuote = text.indexOf(QLatin1Char('\''), firstQuote + 1);
    if (nextQuote <= firstQuote) {
        return QString();
    }
    return text.mid(firstQuote + 1, nextQuote - firstQuote - 1).trimmed();
}

QString datasetLeafNameUi(const QString& datasetName) {
    const QString trimmed = datasetName.trimmed();
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return (slash >= 0) ? trimmed.mid(slash + 1) : trimmed;
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

void MainWindow::updatePendingChangesList() {
    if (!m_pendingChangesList) {
        return;
    }
    const QStringList currentLines = pendingConnContentApplyDisplayLines();
    const QSet<QString> currentSet(currentLines.cbegin(), currentLines.cend());

    // Append newly-appearing items to the end of the persistent ordered list.
    // If a line had a terminal status (success/failed) and appears again as pending,
    // treat it as a new queue item instead of reusing previous visual state/order.
    for (const QString& line : currentLines) {
        const PendingItemStatus prevStatus = m_pendingItemStatus.value(line, PendingItemStatus::Pending);
        const bool wasTerminal = (prevStatus == PendingItemStatus::Success
                                  || prevStatus == PendingItemStatus::Failed);
        if (wasTerminal) {
            m_pendingOrderedDisplayLines.removeAll(line);
            m_pendingItemStatus.remove(line);
        }
        if (!m_pendingOrderedDisplayLines.contains(line)) {
            m_pendingOrderedDisplayLines.append(line);
        }
    }
    // Drop stale terminal-history rows that are no longer pending.
    // This keeps the list aligned with the real pending model and avoids
    // showing old "green tick" rows when an action is reverted to no-op.
    for (int i = m_pendingOrderedDisplayLines.size() - 1; i >= 0; --i) {
        const QString line = m_pendingOrderedDisplayLines.at(i);
        if (currentSet.contains(line)) {
            continue;
        }
        const PendingItemStatus status = m_pendingItemStatus.value(line, PendingItemStatus::Pending);
        if (status == PendingItemStatus::Running) {
            continue;
        }
        m_pendingOrderedDisplayLines.removeAt(i);
        m_pendingItemStatus.remove(line);
    }

    const QSignalBlocker blocker(m_pendingChangesList);
    m_pendingChangesList->clear();

    if (m_pendingOrderedDisplayLines.isEmpty()) {
        auto* item = new QListWidgetItem(m_pendingChangesList);
        item->setText(trk(QStringLiteral("t_apply_changes_tt_001"),
                          QStringLiteral("Sin cambios pendientes."),
                          QStringLiteral("No pending changes."),
                          QStringLiteral("没有待应用的更改。")));
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(QColor(160, 160, 160));
        return;
    }

    for (const QString& line : m_pendingOrderedDisplayLines) {
        PendingItemStatus status;
        if (currentSet.contains(line)) {
            status = m_pendingItemStatus.value(line, PendingItemStatus::Pending);
        } else {
            // Item no longer in model: show recorded result (Success/Failed)
            status = m_pendingItemStatus.value(line, PendingItemStatus::Success);
        }
        auto* item = new QListWidgetItem(m_pendingChangesList);
        item->setText(line);
        item->setIcon(QIcon(makePendingStatusPixmap(status, m_pendingSpinnerFrame)));
    }
}

void MainWindow::startPendingApplyAnimation() {
    m_pendingApplyInProgress = true;
    const QStringList lines = pendingConnContentApplyDisplayLines();
    for (const QString& line : lines) {
        m_pendingItemStatus[line] = PendingItemStatus::Pending;
    }
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
    const QStringList remainingLines = pendingConnContentApplyDisplayLines();
    const QSet<QString> remaining(remainingLines.cbegin(), remainingLines.cend());
    for (auto it = m_pendingItemStatus.begin(); it != m_pendingItemStatus.end(); ++it) {
        if (it.value() != PendingItemStatus::Running) {
            continue;
        }
        it.value() = remaining.contains(it.key()) ? PendingItemStatus::Failed : PendingItemStatus::Success;
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
    if (!m_connContentPage || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const bool isConnectionLevel = poolName.trimmed().isEmpty();
    if (!isConnectionLevel && rootDataset.trimmed().isEmpty()) {
        return;
    }
    const ConnectionProfile& p = m_profiles.at(connIdx);
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
    config.visualOptions = m_topDatasetTreeWidget
        ? m_topDatasetTreeWidget->visualOptions()
        : ConnectionDatasetTreePane::VisualOptions{};
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
        for (int i = 0; i < m_profiles.size(); ++i) {
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
            if (connIdx < 0 || connIdx >= m_profiles.size()) {
                return nullptr;
            }
            const ConnectionProfile& p = m_profiles.at(connIdx);
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
            config.visualOptions = m_topDatasetTreeWidget
                ? m_topDatasetTreeWidget->visualOptions()
                : ConnectionDatasetTreePane::VisualOptions{};
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

void MainWindow::activatePendingChangeAtCursor() {
    if (!m_pendingChangesList || !m_pendingChangesList->hasFocus() || m_pendingChangeActivationInProgress) {
        return;
    }
    const QScopedValueRollback<bool> guard(m_pendingChangeActivationInProgress, true);
    QListWidgetItem* current = m_pendingChangesList->currentItem();
    const QString line = current ? current->text().trimmed() : QString();
    if (line.isEmpty()) {
        return;
    }
    focusPendingChangeLine(line);
}

bool MainWindow::focusPendingChangeLine(const QString& line) {
    PendingChange change;
    if (!findPendingChangeByDisplayLine(line, &change)) {
        return false;
    }
    if (change.connIdx < 0 || change.connIdx >= m_profiles.size() || change.poolName.trimmed().isEmpty()) {
        return false;
    }
    const int connIdx = change.connIdx;
    const QString poolName = change.poolName.trimmed();
    const ConnectionProfile& p = m_profiles.at(connIdx);
    const QString connLabel = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();

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
    QTreeWidget* targetTree = originPoolRoot ? m_connContentTree : nullptr;
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

    const QString objectName = change.objectName.trimmed();
    if (objectName.isEmpty()) {
        return false;
    }
    const int objectAt = objectName.indexOf(QLatin1Char('@'));
    const QString datasetName = (objectAt > 0) ? objectName.left(objectAt).trimmed() : objectName.trimmed();
    const QString snapshotName = (objectAt > 0) ? objectName.mid(objectAt + 1).trimmed() : QString();
    auto findDatasetNode = [&](QTreeWidgetItem* node, auto&& self) -> QTreeWidgetItem* {
        if (!node) {
            return nullptr;
        }
        if (node->data(0, Qt::UserRole).toString().trimmed() == datasetName) {
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
    const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    QSignalBlocker treeBlocker(targetTree);
    std::unique_ptr<QSignalBlocker> selectionBlocker;
    if (targetTree->selectionModel()) {
        selectionBlocker = std::make_unique<QSignalBlocker>(targetTree->selectionModel());
    }
    for (QTreeWidgetItem* parent = datasetItem->parent(); parent; parent = parent->parent()) {
        parent->setExpanded(true);
    }
    targetTree->setCurrentItem(datasetItem);
    refreshConnContentPropertiesFor(targetTree);
    syncConnContentPropertyColumnsFor(targetTree, token);
    datasetItem->setExpanded(true);
    targetTree->scrollToItem(datasetItem, QAbstractItemView::PositionAtCenter);
    if (!snapshotName.isEmpty()) {
        targetTree->setFocus();
        return true;
    }

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

    if (change.focusPermissionsNode) {
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
        targetTree->setFocus();
        return true;
    }

    QString propName = change.propertyName.trimmed();

    if (QTreeWidgetItem* propsNode = findChild(datasetItem, [](QTreeWidgetItem* child) {
            return child->data(0, kConnPropGroupNodeRole).toBool()
                   && child->data(0, kConnPropGroupNameRole).toString().trimmed().isEmpty()
                   && (child->data(0, kConnStatePartRole).toString().trimmed() == QStringLiteral("syn:ds_prop")
                       || child->data(0, kConnStatePartRole).toString().trimmed() == QStringLiteral("syn:snap_prop"));
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

    targetTree->setFocus();
    return true;
}

bool MainWindow::showInlinePropertyNodesForTree(const QTreeWidget* tree) const {
    Q_UNUSED(tree);
    return true;
}

bool MainWindow::showInlinePermissionsNodesForTree(const QTreeWidget* tree) const {
    Q_UNUSED(tree);
    return true;
}

bool MainWindow::showPoolInfoNodeForTree(const QTreeWidget* tree) const {
    Q_UNUSED(tree);
    return true;
}

bool MainWindow::showInlineGsaNodeForTree(const QTreeWidget* tree) const {
    Q_UNUSED(tree);
    return true;
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
        m_store.setLanguage(m_language);
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

    auto applyPropColumnsSetting = [this](int cols) {
        int bounded = qBound(4, cols, 16);
        if ((bounded % 2) != 0) {
            ++bounded;
        }
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
            const int connIdx = m_topDetailConnIdx;
            saveTopTreeStateForConnection(connIdx);
            QTreeWidgetItem* cur = tree->currentItem();
            QTreeWidgetItem* owner = cur;
            while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
                   && !owner->data(0, kIsPoolRootRole).toBool()) {
                owner = owner->parent();
            }
            const QString token = connContentTokenForTree(tree);
            Q_UNUSED(owner);
            syncConnContentPropertyColumnsFor(tree, token);
            syncConnContentPoolColumnsFor(tree, QString());
            restoreTopTreeStateForConnection(connIdx);
            // Tras cambiar el número de columnas, reajustar anchos como "Ajustar tamaño de todas las columnas".
            resizeTreeColumnsToVisibleContent(tree);
        };
        refreshOneConnContentTree(m_connContentTree);
    };

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
    topLayout->setSpacing(6);
    m_topMainSplit = nullptr;
    m_rightMainSplit = nullptr;

    auto* leftPane = new QWidget(topArea);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
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

    m_connectionsTable = nullptr;
    updateConnectivityMatrixButtonState();

    m_connActionsBox = new QGroupBox(
        trk(QStringLiteral("t_actions_box_001"),
            QStringLiteral("Acciones"),
            QStringLiteral("Actions"),
            QStringLiteral("操作")),
        connectionsTab);
    m_connActionsBox->setFont(QApplication::font());
    m_connActionsBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* connActionsLayout = new QVBoxLayout(m_connActionsBox);
    connActionsLayout->setContentsMargins(6, 8, 6, 6);
    connActionsLayout->setSpacing(6);

    auto* selectionRow = new QWidget(connectionsTab);
    selectionRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* selectionRowLayout = new QHBoxLayout(selectionRow);
    selectionRowLayout->setContentsMargins(2, 2, 2, 2);
    selectionRowLayout->setSpacing(6);
    auto* actionButtonsBox = new QWidget(m_connActionsBox);
    auto* actionButtonsLayout = new QVBoxLayout(actionButtonsBox);
    actionButtonsLayout->setContentsMargins(0, 0, 0, 0);
    actionButtonsLayout->setSpacing(6);
    m_connOriginSelectionLabel = new QLabel(
        trk(QStringLiteral("t_conn_origin_sel1"),
            QStringLiteral("Origen:(vacío)"),
            QStringLiteral("Source:(empty)"),
            QStringLiteral("源：（空）")),
        m_connActionsBox);
    m_connOriginSelectionLabel->setWordWrap(true);
    m_connOriginSelectionLabel->setMinimumHeight(18);
    m_connOriginSelectionLabel->setFont(baseUiFont);
    selectionRowLayout->addWidget(m_connOriginSelectionLabel, 1);
    m_btnApplyConnContentProps = new TooltipPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        m_connActionsBox);
    m_btnDiscardPendingChanges = new QPushButton(
        trk(QStringLiteral("t_discard_changes_001"),
            QStringLiteral("Deshacer cambios"),
            QStringLiteral("Discard changes"),
            QStringLiteral("撤销更改")),
        m_connActionsBox);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    m_btnConnCopy = new QPushButton(
        trk(QStringLiteral("t_copy_001"),
            QStringLiteral("Copiar"),
            QStringLiteral("Copy"),
            QStringLiteral("复制")),
        m_connActionsBox);
    m_btnConnCopy->setObjectName(QStringLiteral("connCopyButton"));
    m_btnConnClone = new QPushButton(
        trk(QStringLiteral("t_clone_btn_001"),
            QStringLiteral("Clonar"),
            QStringLiteral("Clone"),
            QStringLiteral("克隆")),
        m_connActionsBox);
    m_btnConnClone->setObjectName(QStringLiteral("connCloneButton"));
    m_btnConnMove = new QPushButton(
        trk(QStringLiteral("t_move_btn_001"),
            QStringLiteral("Mover"),
            QStringLiteral("Move"),
            QStringLiteral("移动")),
        m_connActionsBox);
    m_btnConnMove->setObjectName(QStringLiteral("connMoveButton"));
    m_btnConnDiff = new QPushButton(
        trk(QStringLiteral("t_diff_btn_001"),
            QStringLiteral("Diff")),
        m_connActionsBox);
    m_btnConnDiff->setObjectName(QStringLiteral("connDiffButton"));
    m_btnConnLevel = new QPushButton(
        trk(QStringLiteral("t_level_btn_001"),
            QStringLiteral("Nivelar"),
            QStringLiteral("Level"),
            QStringLiteral("同步快照")),
        m_connActionsBox);
    m_btnConnLevel->setObjectName(QStringLiteral("connLevelButton"));
    m_btnApplyConnContentProps->setFont(baseUiFont);
    m_btnDiscardPendingChanges->setFont(baseUiFont);
    m_btnConnCopy->setFont(baseUiFont);
    m_btnConnClone->setFont(baseUiFont);
    m_btnConnMove->setFont(baseUiFont);
    m_btnConnDiff->setFont(baseUiFont);
    m_btnConnLevel->setFont(baseUiFont);
    m_btnConnSync = new QPushButton(
        trk(QStringLiteral("t_sync_btn_001"),
            QStringLiteral("Sincronizar"),
            QStringLiteral("Sync"),
            QStringLiteral("同步文件")),
        m_connActionsBox);
    m_btnConnSync->setObjectName(QStringLiteral("connSyncButton"));
    m_btnConnSync->setFont(baseUiFont);
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
    m_btnConnMove->setToolTip(
        trk(QStringLiteral("t_tt_move_001"),
            QStringLiteral("Añade un zfs rename pendiente para mover el dataset Origen dentro del dataset Destino.\n"
                           "Requiere: dataset seleccionado en Origen y Destino, misma conexión y mismo pool."),
            QStringLiteral("Queue a pending zfs rename to move the Source dataset under the Target dataset.\n"
                           "Requires: dataset selected in Source and Target, same connection and same pool."),
            QStringLiteral("添加一个待处理的 zfs rename，将来源数据集移动到目标数据集下面。\n"
                           "条件：来源和目标都选择数据集，且属于同一连接和同一存储池。")));
    m_btnConnDiff->setToolTip(
        trk(QStringLiteral("t_tt_diff_001"),
            QStringLiteral("Compara un snapshot de Origen con su dataset padre actual o con otro snapshot del mismo dataset,\n"
                           "si ambos están en la misma conexión y el mismo pool."),
            QStringLiteral("Compares a Source snapshot against its current parent dataset or another snapshot from the same dataset,\n"
                           "when both are in the same connection and the same pool."),
            QStringLiteral("比较源端快照与其当前父数据集，或与同一数据集中的另一个快照，\n"
                           "前提是两者位于同一连接和同一存储池。")));
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
                           "Requiere: dataset seleccionado (no snapshot) en Origen y Destino.\n"
                           "Si no están montados, en Linux, macOS y FreeBSD puede usarse un montaje temporal."),
            QStringLiteral("Sync dataset contents from Source to Target with rsync.\n"
                           "Requires: dataset selected (not snapshot) in Source and Target.\n"
                           "If not mounted, Linux, macOS and FreeBSD can use a temporary mount."),
            QStringLiteral("使用 rsync 同步源端到目标端的数据集内容。\n"
                           "条件：源端和目标端都选择数据集（非快照）。\n"
                           "若未挂载，在 Linux、macOS 和 FreeBSD 上可使用临时挂载。")));
    m_btnApplyConnContentProps->setMinimumHeight(stdLeftBtnH);
    m_btnDiscardPendingChanges->setMinimumHeight(stdLeftBtnH);
    m_btnConnCopy->setMinimumHeight(stdLeftBtnH);
    m_btnConnClone->setMinimumHeight(stdLeftBtnH);
    m_btnConnMove->setMinimumHeight(stdLeftBtnH);
    m_btnConnDiff->setMinimumHeight(stdLeftBtnH);
    m_btnConnLevel->setMinimumHeight(stdLeftBtnH);
    m_btnConnSync->setMinimumHeight(stdLeftBtnH);
    m_btnApplyConnContentProps->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnDiscardPendingChanges->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnConnCopy->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnClone->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnConnMove->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
    connRightBtns->addWidget(m_btnConnMove, 1, 1);
    connRightBtns->addWidget(m_btnConnLevel, 2, 0);
    connRightBtns->addWidget(m_btnConnDiff, 2, 1);
    actionButtonsLayout->addLayout(connRightBtns);
    const int actionButtonsMinHeight = (stdLeftBtnH * 3) + (connRightBtns->verticalSpacing() * 2);
    actionButtonsBox->setMinimumHeight(actionButtonsMinHeight);
    actionButtonsBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* pendingChangesBox = new QGroupBox(
        trk(QStringLiteral("t_pending_changes_tab001"),
            QStringLiteral("Cambios pendientes"),
            QStringLiteral("Pending changes"),
            QStringLiteral("待处理更改")),
        connectionsTab);
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
    pendingButtonsCol->addStretch(1);
    pendingChangesBody->addLayout(pendingButtonsCol, 0);
    m_pendingChangesList = new QListWidget(pendingChangesBox);
    m_pendingChangesList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pendingChangesList->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pendingChangesList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pendingChangesList->setMinimumHeight(0);
    m_pendingChangesList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_pendingChangesList->setIconSize(QSize(14, 14));
    m_pendingChangesList->setSpacing(1);
    connect(m_pendingChangesList, &QListWidget::currentItemChanged, this, [this]() {
        activatePendingChangeAtCursor();
    });
    m_pendingChangesList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pendingChangesList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!m_pendingChangesList) {
            return;
        }
        QListWidgetItem* item = m_pendingChangesList->itemAt(pos);
        if (!item) {
            return;
        }
        const QString line = item->text().trimmed();
        if (line.isEmpty()) {
            return;
        }
        PendingChange change;
        const bool hasPendingChange = findPendingChangeByDisplayLine(line, &change);
        QMenu menu(m_pendingChangesList);
        QAction* aExecute = (hasPendingChange && change.executableIndividually)
                                ? menu.addAction(QStringLiteral("Ejecutar"))
                                : nullptr;
        QAction* aDelete = menu.addAction(QStringLiteral("Eliminar"));
        if (!aExecute && !aDelete) {
            return;
        }
        QAction* picked = menu.exec(m_pendingChangesList->viewport()->mapToGlobal(pos));
        if (aExecute && picked == aExecute) {
            executePendingQueuedChangeLine(line);
        } else if (aDelete && picked == aDelete) {
            if (!removePendingQueuedChangeLine(line)) {
                m_pendingOrderedDisplayLines.removeAll(line);
                m_pendingItemStatus.remove(line);
                updatePendingChangesList();
            }
        }
    });
    pendingChangesBody->addWidget(m_pendingChangesList, 1);
    pendingChangesLayout->addLayout(pendingChangesBody, 1);
    pendingChangesBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    pendingChangesBox->setMinimumHeight(0);
    pendingChangesBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connActionsLayout->addWidget(actionButtonsBox, 0, Qt::AlignTop);
    connActionsLayout->addStretch(1);
    auto* actionsPendingRow = new QWidget(connectionsTab);
    auto* actionsPendingRowLayout = new QHBoxLayout(actionsPendingRow);
    actionsPendingRowLayout->setContentsMargins(0, 0, 0, 0);
    actionsPendingRowLayout->setSpacing(6);
    actionsPendingRowLayout->addWidget(m_connActionsBox, 0, Qt::AlignTop);
    actionsPendingRowLayout->addWidget(pendingChangesBox, 1);
    actionsPendingRowLayout->setStretch(0, 0);
    actionsPendingRowLayout->setStretch(1, 1);
    connLayout->addWidget(selectionRow, 0);
    connLayout->addWidget(actionsPendingRow, 1);
    connectionsTab->setLayout(connLayout);

    // Legacy left "Datasets" tab removed from UI.
    // Legacy "advanced" layer removed from visible UI.
    const int actionsBoxHeight = qMax(actionButtonsMinHeight + 12, m_connActionsBox->minimumSizeHint().height());
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setFixedHeight(actionsBoxHeight);
    }
    if (m_connActionsBox) {
        m_connActionsBox->setMinimumHeight(actionsBoxHeight);
        m_connActionsBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    pendingChangesBox->setMinimumHeight(actionsBoxHeight);
    m_btnAdvancedBreakdown = nullptr;
    m_btnAdvancedAssemble = nullptr;
    m_btnAdvancedFromDir = nullptr;
    m_btnAdvancedToDir = nullptr;

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
    topTreeConfig.visualOptions = {m_showInlinePropertyNodesTop,
                                   m_showInlinePermissionsNodesTop,
                                   m_showInlineGsaNodeTop,
                                   m_showPoolInfoNodeTop};
    topTreeConfig.groupPoolsByConnectionRoots = true;
    m_topDatasetTreeWidget = new ConnectionDatasetTreeWidget(topTreeConfig, m_topConnContentDelegate, m_connContentPage);
    m_topDatasetPane = m_topDatasetTreeWidget->pane();
    m_topConnContentCoordinator = m_topDatasetTreeWidget->coordinator();
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
    delete m_bottomConnContentDelegate;
    m_bottomConnContentDelegate = nullptr;
    m_bottomDatasetTreeWidget = nullptr;
    m_bottomDatasetPane = nullptr;
    m_bottomConnContentCoordinator = nullptr;
    m_bottomConnContentTree = nullptr;
    // Mantener esquema de columnas idéntico en ambos árboles (superior/inferior)
    // incluso cuando uno de ellos esté vacío.
    syncConnContentPropertyColumnsFor(m_connContentTree, connContentTokenForTree(m_connContentTree));
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
            for (int cols = 4; cols <= 16; cols += 2) {
                QAction* act = propColsMenu->addAction(QString::number(cols));
                act->setCheckable(true);
                act->setData(cols);
                if (cols == qBound(4, m_connPropColumnsSetting, 16)) {
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
    stateProgressLayout->setContentsMargins(0, 3, 0, 0);
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
    m_statusText->setFixedHeight(30);
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
    m_lastDetailText->setFixedHeight(30);
    detailLayout->addWidget(detailLabel, 0);
    detailLayout->addWidget(m_lastDetailText, 1);
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

    leftPane->setMinimumHeight(stateProgressRow->sizeHint().height() + actionsBoxHeight + 2);
    leftPane->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    leftPane->setMaximumWidth(QWIDGETSIZE_MAX);
    auto* topBottomPane = new QWidget(topArea);
    auto* topBottomLayout = new QVBoxLayout(topBottomPane);
    topBottomLayout->setContentsMargins(0, 0, 0, 0);
    topBottomLayout->setSpacing(6);
    topBottomLayout->addWidget(stateProgressRow, 0);
    topBottomLayout->addWidget(leftPane, 1);
    const int defaultBottomInfoMinHeight = stateProgressRow->sizeHint().height() + actionsBoxHeight + 2;
    topBottomPane->setMinimumHeight(defaultBottomInfoMinHeight);

    m_bottomInfoSplit = new QSplitter(Qt::Vertical, topArea);
    m_bottomInfoSplit->setChildrenCollapsible(false);
    m_bottomInfoSplit->setHandleWidth(4);
    m_bottomInfoSplit->addWidget(m_rightStack);
    m_bottomInfoSplit->addWidget(topBottomPane);
    m_bottomInfoSplit->setStretchFactor(0, 1);
    m_bottomInfoSplit->setStretchFactor(1, 0);
    m_bottomInfoSplit->setSizes({700, 220});
    topLayout->addWidget(m_bottomInfoSplit, 1);

    loadPersistedAppLogToView();

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
        auto* jobsTab = new QWidget(m_logsTabs);
        auto* jobsLay = new QVBoxLayout(jobsTab);
        jobsLay->setContentsMargins(4, 4, 4, 4);
        jobsLay->setSpacing(4);
        m_jobsListWidget = new QListWidget(jobsTab);
        m_jobsListWidget->setAlternatingRowColors(true);
        m_jobsListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        auto* jobsBtnRow = new QWidget(jobsTab);
        auto* jobsBtnLay = new QHBoxLayout(jobsBtnRow);
        jobsBtnLay->setContentsMargins(0, 0, 0, 0);
        auto* cancelBtn  = new QPushButton(tr("Cancelar seleccionado"), jobsBtnRow);
        auto* refreshBtn = new QPushButton(tr("Refrescar"), jobsBtnRow);
        jobsBtnLay->addWidget(refreshBtn);
        jobsBtnLay->addWidget(cancelBtn);
        jobsBtnLay->addStretch(1);
        jobsLay->addWidget(m_jobsListWidget, 1);
        jobsLay->addWidget(jobsBtnRow, 0);
        m_logsTabs->addTab(jobsTab, tr("Transferencias"));

        connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::pollDaemonJobs);
        connect(cancelBtn, &QPushButton::clicked, this, [this]() {
            if (!m_jobsListWidget) return;
            auto* item = m_jobsListWidget->currentItem();
            if (!item) return;
            const QString jobId   = item->data(Qt::UserRole).toString();
            const int srcConnIdx  = item->data(Qt::UserRole + 1).toInt();
            if (jobId.isEmpty() || srcConnIdx < 0 || srcConnIdx >= m_profiles.size()) return;
            const ConnectionProfile& sp = m_profiles[srcConnIdx];
            QStringList args;
            args << QStringLiteral("--job-cancel") << jobId;
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

    m_logsTabs->setCurrentIndex(1);

    auto* bottomTabsPane = new QWidget(central);
    auto* bottomTabsLayout = new QVBoxLayout(bottomTabsPane);
    bottomTabsLayout->setContentsMargins(0, 0, 0, 0);
    bottomTabsLayout->setSpacing(6);
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
    if (m_bottomInfoSplit && !m_bottomInfoSplitState.isEmpty()) {
        m_bottomInfoSplit->restoreState(m_bottomInfoSplitState);
    }
    if (m_bottomInfoSplit) {
        // Older saved states can carry an obsolete horizontal orientation.
        // Keep this splitter vertical so the actions/pending strip stays
        // between dataset tree and logs on every platform.
        m_bottomInfoSplit->setOrientation(Qt::Vertical);
    }
    if (m_verticalMainSplit && !m_verticalMainSplitState.isEmpty()) {
        m_verticalMainSplit->restoreState(m_verticalMainSplitState);
    }
    if (m_verticalMainSplit) {
        m_verticalMainSplit->setOrientation(Qt::Vertical);
    }
    int minChangesHeight = topBottomPane->sizeHint().height();
    if (m_bottomInfoSplit) {
        const QList<int> bottomInfoSizes = m_bottomInfoSplit->sizes();
        if (bottomInfoSizes.size() >= 2 && bottomInfoSizes.at(1) > 0) {
            minChangesHeight = qMax(minChangesHeight, bottomInfoSizes.at(1));
        }
    }
    const int targetActionsHeight = qMax(actionsBoxHeight,
                                         m_connActionsBox ? m_connActionsBox->minimumSizeHint().height() : 0);
    const int targetChangesMin =
        selectionRow->sizeHint().height() + targetActionsHeight + connLayout->spacing();
    minChangesHeight = qMax(1, qMax(minChangesHeight, targetChangesMin));
    topBottomPane->setMinimumHeight(minChangesHeight);

    int minLogsHeight = bottomTabsPane->sizeHint().height();
    if (m_verticalMainSplit) {
        const QList<int> mainSizes = m_verticalMainSplit->sizes();
        if (mainSizes.size() >= 2 && mainSizes.at(1) > 0) {
            minLogsHeight = qMax(minLogsHeight, mainSizes.at(1));
        }
    }
    minLogsHeight = qMax(1, minLogsHeight / 2);

    if (m_bottomInfoSplit) {
        connect(m_bottomInfoSplit, &QSplitter::splitterMoved, this, [this, minChangesHeight](int, int) {
            if (!m_bottomInfoSplit || m_bottomInfoSplit->property("_enforcingMinChanges").toBool()) {
                return;
            }
            const QList<int> sizes = m_bottomInfoSplit->sizes();
            if (sizes.size() < 2) {
                return;
            }
            const int upper = sizes.at(0);
            const int lower = sizes.at(1);
            if (lower >= minChangesHeight) {
                return;
            }
            const int total = upper + lower;
            if (total <= minChangesHeight) {
                return;
            }
            m_bottomInfoSplit->setProperty("_enforcingMinChanges", true);
            m_bottomInfoSplit->setSizes({total - minChangesHeight, minChangesHeight});
            m_bottomInfoSplit->setProperty("_enforcingMinChanges", false);
        });
    }
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
    auto refreshInlinePropsVisualBottom = [this, connTokenFromTreeSelectionBottom](QTreeWidget* tree, const QString& explicitToken) {
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
        const QString token = explicitToken.trimmed().isEmpty() ? connTokenFromTreeSelectionBottom(tree)
                                                                : explicitToken.trimmed();
        if (token.isEmpty()) {
            return;
        }
        QSignalBlocker blocker(tree);
        syncConnContentPropertyColumnsFor(tree, token);
    };
    auto rebuildInlineConnTree = [this](QTreeWidget* tree, const QString& token) {
        if (!tree || token.trimmed().isEmpty()) {
            return;
        }
        if (tree == m_connContentTree) {
            rebuildConnectionEntityTabs();
            return;
        }
        if (tree == m_bottomConnContentTree) {
            updateSecondaryConnectionDetail();
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
        rebuildConnContentTreeFor(tree, token, connIdx, poolName, true);
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
        const DatasetPermissionsCacheEntry* cachedPermissions =
            datasetPermissionsEntry(ctx.connIdx, ctx.poolName, ctx.datasetName);
        auto reloadTargetNames = [cachedPermissions, targetType, targetName]() {
            targetName->clear();
            if (!cachedPermissions) {
                return;
            }
            const QString kind = targetType->currentData().toString();
            const QStringList names =
                (kind == QStringLiteral("group")) ? cachedPermissions->systemGroups : cachedPermissions->systemUsers;
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
            QString token = child->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
            if (token.isEmpty()) {
                token = child->data(0, kConnStatePartRole).toString().trimmed();
            }
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
            const QString explicitPart = node->data(0, kConnStatePartRole).toString().trimmed();
            if (!explicitPart.isEmpty()) {
                return explicitPart;
            }
            if (node->data(0, kConnPermissionsNodeRole).toBool()) {
                const QString kind = node->data(0, kConnPermissionsKindRole).toString();
                if (!kind.isEmpty()) {
                    const QString entry = node->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                    return entry.isEmpty() ? QStringLiteral("perm|%1").arg(kind)
                                           : QStringLiteral("perm|%1|%2").arg(kind, entry);
                }
            }
            if (node->data(0, kConnPropGroupNodeRole).toBool()) {
                const QString groupName = node->data(0, kConnPropGroupNameRole).toString().trimmed();
                return groupName.isEmpty() ? QStringLiteral("syn:ds_prop")
                                           : QStringLiteral("syn:prop_group:%1").arg(groupName.toLower());
            }
            if (node->data(0, kConnSnapshotHoldsNodeRole).toBool()) {
                return QStringLiteral("syn:snapshot_holds");
            }
            if (node->data(0, kConnSnapshotHoldItemRole).toBool()) {
                return QStringLiteral("hold|%1").arg(node->data(0, kConnSnapshotHoldTagRole).toString().trimmed());
            }
            if (QTreeWidgetItem* parent = node->parent()) {
                return QStringLiteral("idx:%1").arg(parent->indexOfChild(node));
            }
            return QStringLiteral("idx:0");
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
            refreshPermissionsTreeNode(tree, refreshedOwner, true);
            refreshedOwner->setExpanded(ownerExpanded);
            restoreExpandedPaths(refreshedOwner, ownerExpandedPaths);
            appLog(QStringLiteral("DEBUG"),
                   QStringLiteral("executePermissionCommand after local-restore action=%1 dataset=%2 ownerExpanded=%3")
                       .arg(actionName,
                            datasetName,
                            refreshedOwner->isExpanded() ? QStringLiteral("1") : QStringLiteral("0")));
            restoreConnContentTreeState(tree, token);
            QTreeWidgetItem* ownerNode =
                findConnContentDatasetItemFor(tree, connIdx, poolName, datasetName);
            if (ownerNode) {
                populateDatasetPermissionsNode(tree, ownerNode, false);
                restoreConnContentTreeState(tree, token);
            }
        }
        return true;
    };
    auto openEditDatasetDialogBottom = [this](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        const DatasetSelectionContext ctx = currentConnContentSelection(tree);
        if (!ctx.valid || ctx.datasetName.trimmed().isEmpty() || !ctx.snapshotName.trimmed().isEmpty()) {
            return;
        }
        refreshConnContentPropertiesFor(tree);
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

    struct PermissionMenuActions {
        QAction* refreshPerms{nullptr};
        QAction* newGrant{nullptr};
        QAction* newSet{nullptr};
        QAction* editGrant{nullptr};
        QAction* deleteGrant{nullptr};
        QAction* renameSet{nullptr};
        QAction* deleteSet{nullptr};
    };
    auto buildPermissionMenu = [](QMenu& menu, const QString& kind) {
        PermissionMenuActions actions;
        actions.refreshPerms = menu.addAction(QStringLiteral("Refrescar permisos"));
        actions.newGrant = menu.addAction(QStringLiteral("Nueva delegación"));
        actions.newSet = menu.addAction(QStringLiteral("Nuevo set de permisos"));
        if (kind == QStringLiteral("grant") || kind == QStringLiteral("grant_perm")) {
            menu.addSeparator();
            actions.editGrant = menu.addAction(QStringLiteral("Editar delegación"));
            actions.deleteGrant = menu.addAction(QStringLiteral("Eliminar delegación"));
        } else if (kind == QStringLiteral("set") || kind == QStringLiteral("set_perm")) {
            menu.addSeparator();
            actions.renameSet = menu.addAction(QStringLiteral("Renombrar conjunto de permisos"));
            actions.deleteSet = menu.addAction(QStringLiteral("Eliminar set"));
        }
        return actions;
    };

    auto showConnContentPermissionsContextMenu =
        [this, permissionNodeItem, permissionOwnerItem, permissionTokensFromNode,
         refreshPermissionsTreeNode, promptPermissionGrant,
         promptPermissionSet, buildPermissionMenu](
            QTreeWidget* tree, bool isBottom, QTreeWidgetItem* item, const QPoint& pos) -> bool {
            if (!tree || !item) {
                return false;
            }
            QTreeWidgetItem* permNode = permissionNodeItem(item);
            if (!permNode) {
                return false;
            }
            QTreeWidgetItem* owner = permissionOwnerItem(item);
            if (!owner) {
                return true;
            }
            const QString token = QStringLiteral("%1::%2")
                                      .arg(owner->data(0, kConnIdxRole).toInt())
                                      .arg(owner->data(0, kPoolNameRole).toString().trimmed());
            DatasetSelectionContext ctx = currentConnContentSelection(tree);
            if (ctx.valid
                && ctx.snapshotName.isEmpty()
                && !isWindowsConnection(ctx.connIdx)
                && permNode->data(0, kConnPermissionsKindRole).toString() == QStringLiteral("root")
                && permNode->childCount() == 0) {
                populateDatasetPermissionsNode(tree, owner, false);
            }
            if (!ctx.valid || !ctx.snapshotName.isEmpty() || isWindowsConnection(ctx.connIdx)) {
                return true;
            }
            QMenu permMenu(this);
            const QString kind = permNode->data(0, kConnPermissionsKindRole).toString();
            const PermissionMenuActions permActions = buildPermissionMenu(permMenu, kind);
            QAction* picked = permMenu.exec(tree->viewport()->mapToGlobal(pos));
            if (!picked) {
                return true;
            }
            if (picked == permActions.refreshPerms) {
                refreshPermissionsTreeNode(tree, item, true);
                return true;
            }
            if (picked == permActions.newGrant) {
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
                    return true;
                }
                auto* it = datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
                if (!it) {
                    return true;
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
                mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                populateDatasetPermissionsNode(tree, owner, false);
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
                                tree->setCurrentItem(grantNode);
                                break;
                            }
                        }
                    }
                }
                return true;
            }
            if (picked == permActions.editGrant) {
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
                    return true;
                }
                auto* it = datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
                if (it) {
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
                        mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                        populateDatasetPermissionsNode(tree, owner, false);
                        updateApplyPropsButtonState();
                    }
                }
                return true;
            }
            if (picked == permActions.newSet) {
                QString setName;
                QStringList tokens;
                if (!promptPermissionSet(ctx,
                                         QStringLiteral("Nuevo set de permisos"),
                                         QString(),
                                         {},
                                         false,
                                         setName,
                                         tokens)) {
                    return true;
                }
                auto* it = datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
                if (it) {
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
                        mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                        populateDatasetPermissionsNode(tree, owner, false);
                        updateApplyPropsButtonState();
                    }
                }
                return true;
            }
            if (picked == permActions.renameSet) {
                QTreeWidgetItem* setNode = permNode;
                if (kind == QStringLiteral("set_perm") && permNode->parent()) {
                    setNode = permNode->parent();
                }
                const QString oldSetName = setNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                bool ok = false;
                QString newSetName = QInputDialog::getText(
                    this,
                    QStringLiteral("Renombrar conjunto de permisos"),
                    QStringLiteral("Nuevo nombre"),
                    QLineEdit::Normal,
                    oldSetName,
                    &ok).trimmed();
                if (!ok || newSetName.isEmpty()) {
                    return true;
                }
                if (!newSetName.startsWith(QLatin1Char('@'))) {
                    newSetName.prepend(QLatin1Char('@'));
                }
                auto* it = datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
                if (it) {
                    for (DatasetPermissionSet& s : it->permissionSets) {
                        if (s.name.compare(oldSetName, Qt::CaseInsensitive) == 0) {
                            s.name = newSetName;
                            it->dirty = true;
                            break;
                        }
                    }
                    mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    populateDatasetPermissionsNode(tree, owner, false);
                    updateApplyPropsButtonState();
                }
                return true;
            }
            if (picked == permActions.deleteGrant) {
                auto* it = datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
                if (it) {
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
                    mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    populateDatasetPermissionsNode(tree, owner, false);
                    updateApplyPropsButtonState();
                }
                return true;
            }
            if (picked == permActions.deleteSet) {
                QString setName = permNode->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                if (kind == QStringLiteral("set_perm") && permNode->parent()) {
                    setName = permNode->parent()->data(0, kConnPermissionsEntryNameRole).toString().trimmed();
                }
                auto* it = datasetPermissionsEntryMutable(ctx.connIdx, ctx.poolName, ctx.datasetName);
                if (it) {
                    for (int i = it->permissionSets.size() - 1; i >= 0; --i) {
                        if (it->permissionSets.at(i).name.compare(setName, Qt::CaseInsensitive) == 0) {
                            it->permissionSets.removeAt(i);
                            it->dirty = true;
                        }
                    }
                    mirrorDatasetPermissionsEntryToModel(ctx.connIdx, ctx.poolName, ctx.datasetName);
                    populateDatasetPermissionsNode(tree, owner, false);
                    updateApplyPropsButtonState();
                }
                return true;
            }
            return true;
        };
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
            syncConnContentPoolColumnsFor(tree, token);
        } else {
            syncConnContentPropertyColumnsFor(tree, token);
        }
    };
    auto openEditDatasetDialog = [this](QTreeWidget* tree) {
        if (!tree) {
            return;
        }
        const DatasetSelectionContext ctx = currentConnContentSelection(tree);
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

        refreshConnContentPropertiesFor(tree);
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
            clearAllPendingChanges();
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
        m_activeConnActionName = trk(QStringLiteral("t_clone_btn_001"),
                                     QStringLiteral("Clonar"),
                                     QStringLiteral("Clone"),
                                     QStringLiteral("克隆"));
        logUiAction(QStringLiteral("Clonar snapshot (botón Conexiones)"));
        executeConnectionTransferAction(QStringLiteral("clone"));
        if (!actionsLocked()) {
            m_activeConnActionBtn = nullptr;
            m_activeConnActionName.clear();
            updateConnectionActionsState();
        }
    });
    connect(m_btnConnMove, &QPushButton::clicked, this, [this]() {
        if (actionsLocked()) {
            return;
        }
        m_activeConnActionBtn = m_btnConnMove;
        m_activeConnActionName = trk(QStringLiteral("t_move_btn_001"),
                                     QStringLiteral("Mover"),
                                     QStringLiteral("Move"),
                                     QStringLiteral("移动"));
        logUiAction(QStringLiteral("Mover dataset (botón Conexiones)"));
        executeConnectionTransferAction(QStringLiteral("move"));
        m_activeConnActionBtn = nullptr;
        m_activeConnActionName.clear();
        updateConnectionActionsState();
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
