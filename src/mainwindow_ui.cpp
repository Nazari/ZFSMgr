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
#include <QTimer>
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
        setGridSize(QSize(cellWidth, 56));
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

    targetTree->setFocus();
    return true;
}

bool MainWindow::showInlinePropertyNodesForTree(const QTreeWidget* tree) const {
    if (tree == m_bottomConnContentTree && m_bottomDatasetPane) {
        return m_bottomDatasetPane->visualOptions().showInlineProperties;
    }
    if (tree == m_connContentTree && m_topDatasetPane) {
        return m_topDatasetPane->visualOptions().showInlineProperties;
    }
    return (tree == m_bottomConnContentTree) ? m_showInlinePropertyNodesBottom
                                             : m_showInlinePropertyNodesTop;
}

bool MainWindow::showInlinePermissionsNodesForTree(const QTreeWidget* tree) const {
    if (tree == m_bottomConnContentTree && m_bottomDatasetPane) {
        return m_bottomDatasetPane->visualOptions().showInlinePermissions;
    }
    if (tree == m_connContentTree && m_topDatasetPane) {
        return m_topDatasetPane->visualOptions().showInlinePermissions;
    }
    return (tree == m_bottomConnContentTree) ? m_showInlinePermissionsNodesBottom
                                             : m_showInlinePermissionsNodesTop;
}

bool MainWindow::showPoolInfoNodeForTree(const QTreeWidget* tree) const {
    if (tree == m_bottomConnContentTree && m_bottomDatasetPane) {
        return m_bottomDatasetPane->visualOptions().showPoolInfo;
    }
    if (tree == m_connContentTree && m_topDatasetPane) {
        return m_topDatasetPane->visualOptions().showPoolInfo;
    }
    return (tree == m_bottomConnContentTree) ? m_showPoolInfoNodeBottom
                                             : m_showPoolInfoNodeTop;
}

bool MainWindow::showInlineGsaNodeForTree(const QTreeWidget* tree) const {
    if (tree == m_bottomConnContentTree && m_bottomDatasetPane) {
        return m_bottomDatasetPane->visualOptions().showInlineGsa;
    }
    if (tree == m_connContentTree && m_topDatasetPane) {
        return m_topDatasetPane->visualOptions().showInlineGsa;
    }
    return (tree == m_bottomConnContentTree) ? m_showInlineGsaNodeBottom
                                             : m_showInlineGsaNodeTop;
}

void MainWindow::setShowInlinePropertyNodesForTree(QTreeWidget* tree, bool visible) {
    if (tree == m_bottomConnContentTree) {
        m_showInlinePropertyNodesBottom = visible;
        if (m_bottomDatasetPane) {
            auto options = m_bottomDatasetPane->visualOptions();
            options.showInlineProperties = visible;
            m_bottomDatasetPane->setVisualOptions(options);
        }
    } else {
        m_showInlinePropertyNodesTop = visible;
        if (m_topDatasetPane) {
            auto options = m_topDatasetPane->visualOptions();
            options.showInlineProperties = visible;
            m_topDatasetPane->setVisualOptions(options);
        }
    }
}

void MainWindow::setShowInlinePermissionsNodesForTree(QTreeWidget* tree, bool visible) {
    if (tree == m_bottomConnContentTree) {
        m_showInlinePermissionsNodesBottom = visible;
        if (m_bottomDatasetPane) {
            auto options = m_bottomDatasetPane->visualOptions();
            options.showInlinePermissions = visible;
            m_bottomDatasetPane->setVisualOptions(options);
        }
    } else {
        m_showInlinePermissionsNodesTop = visible;
        if (m_topDatasetPane) {
            auto options = m_topDatasetPane->visualOptions();
            options.showInlinePermissions = visible;
            m_topDatasetPane->setVisualOptions(options);
        }
    }
}

void MainWindow::setShowInlineGsaNodeForTree(QTreeWidget* tree, bool visible) {
    if (tree == m_bottomConnContentTree) {
        m_showInlineGsaNodeBottom = visible;
        if (m_bottomDatasetPane) {
            auto options = m_bottomDatasetPane->visualOptions();
            options.showInlineGsa = visible;
            m_bottomDatasetPane->setVisualOptions(options);
        }
    } else {
        m_showInlineGsaNodeTop = visible;
        if (m_topDatasetPane) {
            auto options = m_topDatasetPane->visualOptions();
            options.showInlineGsa = visible;
            m_topDatasetPane->setVisualOptions(options);
        }
    }
}

void MainWindow::setShowPoolInfoNodeForTree(const QTreeWidget* tree, bool visible) {
    if (tree == m_bottomConnContentTree) {
        m_showPoolInfoNodeBottom = visible;
        if (m_bottomDatasetPane) {
            auto options = m_bottomDatasetPane->visualOptions();
            options.showPoolInfo = visible;
            m_bottomDatasetPane->setVisualOptions(options);
        }
    } else {
        m_showPoolInfoNodeTop = visible;
        if (m_topDatasetPane) {
            auto options = m_topDatasetPane->visualOptions();
            options.showPoolInfo = visible;
            m_topDatasetPane->setVisualOptions(options);
        }
    }
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

    QAction* confirmAct = appMenu->addAction(
        trk(QStringLiteral("t_show_confirm_001"),
            QStringLiteral("Mostrar confirmación antes de ejecutar acciones"),
            QStringLiteral("Show confirmation before executing actions"),
            QStringLiteral("执行操作前显示确认")));
    m_confirmActionsMenuAction = confirmAct;
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
            const bool isBottomTree = (tree == m_bottomConnContentTree);
            const int connIdx = isBottomTree ? m_bottomDetailConnIdx : m_topDetailConnIdx;
            if (isBottomTree) {
                saveBottomTreeStateForConnection(connIdx);
            } else {
                saveTopTreeStateForConnection(connIdx);
            }
            QTreeWidgetItem* cur = tree->currentItem();
            QTreeWidgetItem* owner = cur;
            while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
                   && !owner->data(0, kIsPoolRootRole).toBool()) {
                owner = owner->parent();
            }
            const bool poolMode = owner && owner->data(0, kIsPoolRootRole).toBool();
            const QString token = connContentTokenForTree(tree);
            if (poolMode) {
                syncConnContentPoolColumnsFor(tree, token);
            } else {
                syncConnContentPropertyColumnsFor(tree, token);
            }
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
    m_topMainSplit = new QSplitter(Qt::Horizontal, topArea);
    m_topMainSplit->setChildrenCollapsible(true);
    m_topMainSplit->setHandleWidth(4);

    auto* leftPane = new QWidget(m_topMainSplit);
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
    m_connectionsTable->setObjectName(QStringLiteral("connectionsTable"));
    m_connectionsTable->setColumnCount(3);
    m_connectionsTable->setHorizontalHeaderLabels({
        trk(QStringLiteral("t_connections_001"),
            QStringLiteral("Conexión"),
            QStringLiteral("Connection"),
            QStringLiteral("连接")),
        QStringLiteral("O"),
        QStringLiteral("D")
    });
    m_connectionsTable->horizontalHeader()->setVisible(true);
    m_connectionsTable->verticalHeader()->setVisible(false);
    m_connectionsTable->setAlternatingRowColors(false);
    m_connectionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_connectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_connectionsTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_connectionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_connectionsTable->setFont(QApplication::font());
    if (m_connectionsTable->horizontalHeader()) {
        m_connectionsTable->horizontalHeader()->setFont(QApplication::font());
    }
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_connectionsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_connectionsTable->setColumnWidth(1, 34);
    m_connectionsTable->setColumnWidth(2, 34);
    m_connectionsTable->setItemDelegateForColumn(0, new ConnectionRowTextDelegate(m_connectionsTable));
    m_connectionsTable->setItemDelegateForColumn(1, new LightCenteredCheckDelegate(m_connectionsTable));
    m_connectionsTable->setItemDelegateForColumn(2, new LightCenteredCheckDelegate(m_connectionsTable));
#ifdef Q_OS_MAC
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        m_connectionsTable->setStyle(fusion);
    }
#endif
    m_connectivityMatrixBtn = new QPushButton(
        trk(QStringLiteral("t_connectivity_btn_001"),
            QStringLiteral("Conectividad"),
            QStringLiteral("Connectivity"),
            QStringLiteral("连通性")),
        m_connectionsTable->viewport());
    m_connectivityMatrixBtn->setObjectName(QStringLiteral("zfsmgrConnectivityMatrixBtn"));
    m_connectivityMatrixBtn->setFont(QApplication::font());
    m_connectivityMatrixBtn->raise();
    m_connectivityMatrixBtn->installEventFilter(this);
    updateConnectivityMatrixButtonState();
    m_connectionsTable->installEventFilter(this);
    m_connectionsTable->viewport()->installEventFilter(this);
    connListBoxLayout->addWidget(m_connectionsTable, 1);
    connLayout->addWidget(connListBox, 1);

    m_connActionsBox = new QGroupBox(
        trk(QStringLiteral("t_actions_box_001"),
            QStringLiteral("Datasets seleccionados"),
            QStringLiteral("Selected datasets"),
            QStringLiteral("已选数据集")),
        connectionsTab);
    m_connActionsBox->setFont(QApplication::font());
    auto* connActionsLayout = new QHBoxLayout(m_connActionsBox);
    connActionsLayout->setContentsMargins(6, 8, 6, 6);
    connActionsLayout->setSpacing(8);

    m_btnConnBreakdown = new QPushButton(
        trk(QStringLiteral("t_breakdown_btn1"),
            QStringLiteral("Desglosar"),
            QStringLiteral("Break down"),
            QStringLiteral("拆分")),
        m_connActionsBox);
    m_btnConnBreakdown->setObjectName(QStringLiteral("connBreakdownButton"));
    m_btnConnAssemble = new QPushButton(
        trk(QStringLiteral("t_assemble_btn1"),
            QStringLiteral("Ensamblar"),
            QStringLiteral("Assemble"),
            QStringLiteral("组装")),
        m_connActionsBox);
    m_btnConnAssemble->setObjectName(QStringLiteral("connAssembleButton"));
    m_btnConnFromDir = new QPushButton(
        trk(QStringLiteral("t_from_dir_btn1"),
            QStringLiteral("Desde Dir"),
            QStringLiteral("From Dir"),
            QStringLiteral("来自目录")),
        m_connActionsBox);
    m_btnConnFromDir->setObjectName(QStringLiteral("connFromDirButton"));
    m_btnConnToDir = new QPushButton(
        trk(QStringLiteral("t_to_dir_btn_001"),
            QStringLiteral("Hacia Dir"),
            QStringLiteral("To Dir"),
            QStringLiteral("到目录")),
        m_connActionsBox);
    m_btnConnToDir->setObjectName(QStringLiteral("connToDirButton"));
    m_btnConnBreakdown->setFont(baseUiFont);
    m_btnConnAssemble->setFont(baseUiFont);
    m_btnConnFromDir->setFont(baseUiFont);
    m_btnConnToDir->setFont(baseUiFont);
    m_btnConnBreakdown->setToolTip(
        trk(QStringLiteral("t_tt_breakdown001"), QStringLiteral("Construye datasets a partir de directorios. "
                           "Requiere dataset accesible por mountpoint; en Linux, macOS y FreeBSD puede montarse temporalmente. "
                           "Permite seleccionar directorios a desglosar. "
                           "No se ejecuta si hay conflictos de mountpoint."),
            QStringLiteral("Builds datasets from directories. "
                           "Requires the dataset to be reachable by mountpoint; on Linux, macOS and FreeBSD it can be mounted temporarily. "
                           "Lets you select directories to split. "
                           "Will not run if mountpoint conflicts exist."),
            QStringLiteral("从目录构建数据集。"
                           "要求数据集可通过挂载点访问；在 Linux、macOS 和 FreeBSD 上可临时挂载。"
                           "可选择要拆分的目录。"
                           "若存在挂载点冲突则不会执行。")));
    m_btnConnAssemble->setToolTip(
        trk(QStringLiteral("t_tt_assemble001"), QStringLiteral("Convierte datasets en directorios. "
                           "Requiere dataset accesible por mountpoint; en Linux, macOS y FreeBSD puede montarse temporalmente. "
                           "Permite seleccionar subdatasets a ensamblar. "
                           "zfs destroy solo se ejecuta si rsync finaliza OK."),
            QStringLiteral("Converts datasets into directories. "
                           "Requires the dataset to be reachable by mountpoint; on Linux, macOS and FreeBSD it can be mounted temporarily. "
                           "Lets you select child datasets to assemble. "
                           "zfs destroy runs only if rsync succeeds."),
            QStringLiteral("将数据集转换为目录。"
                           "要求数据集可通过挂载点访问；在 Linux、macOS 和 FreeBSD 上可临时挂载。"
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
    m_connOriginSelectionLabel->setFont(baseUiFont);
    connActionRightLayout->addWidget(m_connOriginSelectionLabel);
    m_connDestSelectionLabel = new QLabel(
        trk(QStringLiteral("t_conn_dest_sel01"),
            QStringLiteral("Destino:(vacío)"),
            QStringLiteral("Target:(empty)"),
            QStringLiteral("目标：（空）")),
        connActionRightBox);
    m_connDestSelectionLabel->setWordWrap(true);
    m_connDestSelectionLabel->setMinimumHeight(20);
    m_connDestSelectionLabel->setFont(baseUiFont);
    connActionRightLayout->addWidget(m_connDestSelectionLabel);
    m_btnApplyConnContentProps = new TooltipPushButton(
        trk(QStringLiteral("t_apply_changes_001"),
            QStringLiteral("Aplicar cambios"),
            QStringLiteral("Apply changes"),
            QStringLiteral("应用更改")),
        connActionRightBox);
    m_btnDiscardPendingChanges = new QPushButton(
        trk(QStringLiteral("t_discard_changes_001"),
            QStringLiteral("Deshacer cambios"),
            QStringLiteral("Discard changes"),
            QStringLiteral("撤销更改")),
        connActionRightBox);
    m_btnApplyConnContentProps->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    m_btnConnCopy = new QPushButton(
        trk(QStringLiteral("t_copy_001"),
            QStringLiteral("Copiar"),
            QStringLiteral("Copy"),
            QStringLiteral("复制")),
        connActionRightBox);
    m_btnConnCopy->setObjectName(QStringLiteral("connCopyButton"));
    m_btnConnClone = new QPushButton(
        trk(QStringLiteral("t_clone_btn_001"),
            QStringLiteral("Clonar")),
        connActionRightBox);
    m_btnConnClone->setObjectName(QStringLiteral("connCloneButton"));
    m_btnConnMove = new QPushButton(
        trk(QStringLiteral("t_move_btn_001"),
            QStringLiteral("Mover"),
            QStringLiteral("Move"),
            QStringLiteral("移动")),
        connActionRightBox);
    m_btnConnMove->setObjectName(QStringLiteral("connMoveButton"));
    m_btnConnDiff = new QPushButton(
        trk(QStringLiteral("t_diff_btn_001"),
            QStringLiteral("Diff")),
        connActionRightBox);
    m_btnConnDiff->setObjectName(QStringLiteral("connDiffButton"));
    m_btnConnLevel = new QPushButton(
        trk(QStringLiteral("t_level_btn_001"),
            QStringLiteral("Nivelar"),
            QStringLiteral("Level"),
            QStringLiteral("同步快照")),
        connActionRightBox);
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
        connActionRightBox);
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

    auto* rightPane = new QWidget(m_topMainSplit);
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
    m_connectionEntityTabs = nullptr;
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
    delete m_topConnContentDelegate;
    m_topConnContentDelegate = new MainWindowConnectionDatasetTreeDelegate(this, this);
    ConnectionDatasetTreeWidget::Config topTreeConfig;
    topTreeConfig.treeName = QStringLiteral("originDatasetTreeWidget");
    topTreeConfig.primaryColumnTitle = trk(QStringLiteral("t_origin_dataset_col001"),
                                           QStringLiteral("Origen:Dataset"),
                                           QStringLiteral("Origin:Dataset"),
                                           QStringLiteral("源:数据集"));
    topTreeConfig.role = ConnectionDatasetTreePane::Role::Top;
    topTreeConfig.visualOptions = {m_showInlinePropertyNodesTop,
                                   m_showInlinePermissionsNodesTop,
                                   m_showInlineGsaNodeTop,
                                   m_showPoolInfoNodeTop};
    m_topDatasetTreeWidget = new ConnectionDatasetTreeWidget(topTreeConfig, m_topConnContentDelegate, m_connContentPage);
    m_topDatasetPane = m_topDatasetTreeWidget->pane();
    m_topConnContentCoordinator = m_topDatasetTreeWidget->coordinator();
    m_connContentTree = m_topDatasetTreeWidget->tree();
    m_connContentTree->setItemDelegate(new ConnContentPropBorderDelegate(m_connContentTree));
    // Las acciones se exponen por menú contextual del árbol.
    if (m_btnConnBreakdown) m_btnConnBreakdown->setVisible(false);
    if (m_btnConnAssemble) m_btnConnAssemble->setVisible(false);
    if (m_btnConnFromDir) m_btnConnFromDir->setVisible(false);
    if (m_btnConnToDir) m_btnConnToDir->setVisible(false);
    connContentLayout->addWidget(m_topDatasetTreeWidget, 1);
    m_btnApplyConnContentProps->setEnabled(false);
    if (m_btnDiscardPendingChanges) m_btnDiscardPendingChanges->setEnabled(false);
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
    m_rightMainSplit = new QSplitter(Qt::Vertical, rightPane);
    m_rightMainSplit->setChildrenCollapsible(false);
    m_rightMainSplit->setHandleWidth(4);
    m_rightMainSplit->addWidget(m_rightStack);
    auto* bottomConnBox = new QWidget(m_rightMainSplit);
    auto* bottomConnLayout = new QVBoxLayout(bottomConnBox);
    bottomConnLayout->setContentsMargins(6, 6, 6, 6);
    m_bottomConnectionEntityTabs = new QTabBar(bottomConnBox);
    m_bottomConnectionEntityTabs->setObjectName(QStringLiteral("zfsmgrEntityTabsBottom"));
    m_bottomConnectionEntityTabs->setExpanding(false);
    m_bottomConnectionEntityTabs->setDrawBase(false);
    m_bottomConnectionEntityTabs->setUsesScrollButtons(true);
    m_bottomConnectionEntityTabs->setVisible(false);
    bottomConnLayout->addWidget(m_bottomConnectionEntityTabs, 0);
    delete m_bottomConnContentDelegate;
    m_bottomConnContentDelegate = new MainWindowConnectionDatasetTreeDelegate(this, this);
    ConnectionDatasetTreeWidget::Config bottomTreeConfig;
    bottomTreeConfig.treeName = QStringLiteral("destinationDatasetTreeWidget");
    bottomTreeConfig.primaryColumnTitle = trk(QStringLiteral("t_target_dataset_col001"),
                                              QStringLiteral("Destino:Dataset"),
                                              QStringLiteral("Destination:Dataset"),
                                              QStringLiteral("目标:数据集"));
    bottomTreeConfig.role = ConnectionDatasetTreePane::Role::Bottom;
    bottomTreeConfig.visualOptions = {m_showInlinePropertyNodesBottom,
                                      m_showInlinePermissionsNodesBottom,
                                      m_showInlineGsaNodeBottom,
                                      m_showPoolInfoNodeBottom};
    m_bottomDatasetTreeWidget = new ConnectionDatasetTreeWidget(bottomTreeConfig, m_bottomConnContentDelegate, bottomConnBox);
    m_bottomDatasetPane = m_bottomDatasetTreeWidget->pane();
    m_bottomConnContentCoordinator = m_bottomDatasetTreeWidget->coordinator();
    m_bottomConnContentTree = m_bottomDatasetTreeWidget->tree();
    m_bottomConnContentTree->setItemDelegate(new ConnContentPropBorderDelegate(m_bottomConnContentTree));
    // Mantener esquema de columnas idéntico en ambos árboles (superior/inferior)
    // incluso cuando uno de ellos esté vacío.
    {
        const QList<QTreeWidget*> trees{m_connContentTree, m_bottomConnContentTree};
        for (QTreeWidget* tree : trees) {
            if (!tree) {
                continue;
            }
            syncConnContentPropertyColumnsFor(tree, connContentTokenForTree(tree));
        }
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
    installTreeHeaderContextMenu(m_connContentTree);
    installTreeHeaderContextMenu(m_bottomConnContentTree);
    auto* bottomContentSplit = new QSplitter(Qt::Vertical, bottomConnBox);
    bottomContentSplit->setChildrenCollapsible(true);
    bottomContentSplit->setHandleWidth(4);
    bottomContentSplit->addWidget(m_bottomDatasetTreeWidget);
    m_rightMainSplit->setStretchFactor(0, 1);
    m_rightMainSplit->setStretchFactor(1, 1);
    m_rightMainSplit->setSizes({500, 500});
    auto equalizeTreeHeights = [this, rightConnectionsPage, bottomConnBox]() {
        QTreeWidget* topTree = m_topDatasetPane ? m_topDatasetPane->tree() : nullptr;
        if (!m_rightMainSplit || !topTree || !m_bottomConnContentTree
            || !rightConnectionsPage || !bottomConnBox) {
            return;
        }
        const int total = m_rightMainSplit->size().height() > 0
                              ? m_rightMainSplit->size().height()
                              : m_rightMainSplit->sizes().value(0, 0) + m_rightMainSplit->sizes().value(1, 0);
        if (total <= 0) {
            return;
        }
        const int topTreeH = topTree->viewport() ? topTree->viewport()->height() : topTree->height();
        const int bottomTreeH = m_bottomConnContentTree->viewport() ? m_bottomConnContentTree->viewport()->height()
                                                                     : m_bottomConnContentTree->height();
        if (topTreeH <= 0 || bottomTreeH <= 0) {
            int topH = total / 2;
            topH = qBound(120, topH, total - 120);
            m_rightMainSplit->setSizes({topH, total - topH});
            return;
        }
        const int delta = topTreeH - bottomTreeH;
        if (qAbs(delta) <= 1) {
            return;
        }
        QList<int> sizes = m_rightMainSplit->sizes();
        if (sizes.size() < 2) {
            int topH = total / 2;
            topH = qBound(120, topH, total - 120);
            m_rightMainSplit->setSizes({topH, total - topH});
            return;
        }
        int topH = sizes[0] - (delta / 2);
        topH = qBound(120, topH, total - 120);
        m_rightMainSplit->setSizes({topH, total - topH});
    };
    QTimer::singleShot(0, this, equalizeTreeHeights);
    QTimer::singleShot(80, this, equalizeTreeHeights);
    QTimer::singleShot(180, this, equalizeTreeHeights);
    QTimer::singleShot(320, this, equalizeTreeHeights);
    QTimer::singleShot(520, this, equalizeTreeHeights);
    QTimer::singleShot(900, this, equalizeTreeHeights);
    rightLayout->addWidget(m_rightMainSplit, 1);

    m_topMainSplit->addWidget(leftPane);
    m_topMainSplit->addWidget(rightPane);
    m_topMainSplit->setStretchFactor(0, 0);
    m_topMainSplit->setStretchFactor(1, 1);
    m_topMainSplit->setSizes({leftFixedWidth, qMax(leftFixedWidth * 2, width() - leftFixedWidth)});
    topLayout->addWidget(m_topMainSplit, 1);

    m_logsTabs = new QTabWidget(central);
    m_logsTabs->setObjectName(QStringLiteral("zfsmgrLogTabs"));

    auto* combinedLogTab = new QWidget(m_logsTabs);
    auto* logLayout = new QVBoxLayout(combinedLogTab);
    logLayout->setContentsMargins(6, 6, 6, 6);
    logLayout->setSpacing(4);

    QFont combinedLogFont = baseUiFont;
    combinedLogFont.setPointSize(qMax(6, baseUiFont.pointSize() - 2));

    auto* stateProgressBox = new QGroupBox(
        trk(QStringLiteral("t_state_progress_col_001"),
            QStringLiteral("Estado y progreso"),
            QStringLiteral("Status and progress"),
            QStringLiteral("状态和进度")),
        combinedLogTab);
    auto* stateProgressLayout = new QVBoxLayout(stateProgressBox);
    stateProgressLayout->setContentsMargins(6, 6, 6, 6);
    stateProgressLayout->setSpacing(4);
    auto* statusBox = new QGroupBox(trk(QStringLiteral("t_status_col_001"),
                                        QStringLiteral("Estado"),
                                        QStringLiteral("Status"),
                                        QStringLiteral("状态")),
                                    stateProgressBox);
    auto* statusLayout = new QVBoxLayout(statusBox);
    statusLayout->setContentsMargins(6, 6, 6, 6);
    statusLayout->setSpacing(4);
    m_statusText = new QTextEdit(statusBox);
    m_statusText->setFont(combinedLogFont);
    m_statusText->setReadOnly(true);
    m_statusText->setAcceptRichText(false);
    m_statusText->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_statusText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_statusText->setLineWrapMode(QTextEdit::WidgetWidth);
    m_statusText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    m_statusText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_statusText->setPlainText(trk(QStringLiteral("t_status_loading_001"),
                                   QStringLiteral("Loading..."),
                                   QStringLiteral("Loading..."),
                                   QStringLiteral("加载中...")));
    statusLayout->addWidget(m_statusText, 1);

    auto* detailBox = new QGroupBox(trk(QStringLiteral("t_detail_lbl001"),
                                        QStringLiteral("Progreso"),
                                        QStringLiteral("Progress"),
                                        QStringLiteral("进度")),
                                    stateProgressBox);
    auto* detailLayout = new QVBoxLayout(detailBox);
    detailLayout->setContentsMargins(6, 6, 6, 6);
    detailLayout->setSpacing(4);
    m_lastDetailText = new QTextEdit(detailBox);
    m_lastDetailText->setFont(combinedLogFont);
    m_lastDetailText->setReadOnly(true);
    m_lastDetailText->setAcceptRichText(false);
    m_lastDetailText->setLineWrapMode(QTextEdit::WidgetWidth);
    m_lastDetailText->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_lastDetailText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lastDetailText->setStyleSheet(QStringLiteral("background:#f6f9fc; border:1px solid #c5d3e0;"));
    m_lastDetailText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    detailLayout->addWidget(m_lastDetailText, 1);
    stateProgressLayout->addWidget(statusBox, 1);
    stateProgressLayout->addWidget(detailBox, 1);

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
    stateProgressBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    appLogBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    logLayout->addWidget(appLogBox, 1);

    auto* pendingChangesBox = new QGroupBox(
        trk(QStringLiteral("t_pending_changes_tab001"),
            QStringLiteral("Cambios pendientes"),
            QStringLiteral("Pending changes"),
            QStringLiteral("待处理更改")),
        bottomConnBox);
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
    m_pendingChangesView = new QPlainTextEdit(pendingChangesBox);
    m_pendingChangesView->setReadOnly(true);
    m_pendingChangesView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_pendingChangesView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pendingChangesView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(m_pendingChangesView, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        activatePendingChangeAtCursor();
    });
    m_pendingChangesView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pendingChangesView, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!m_pendingChangesView) {
            return;
        }
        QTextCursor cursor = m_pendingChangesView->cursorForPosition(pos);
        m_pendingChangesView->setTextCursor(cursor);
        const QString line = cursor.block().text().trimmed();
        if (line.isEmpty()) {
            return;
        }
        QMenu menu(m_pendingChangesView);
        QAction* aExecute = menu.addAction(QStringLiteral("Ejecutar"));
        QAction* aDelete = menu.addAction(QStringLiteral("Eliminar"));
        QAction* picked = menu.exec(m_pendingChangesView->viewport()->mapToGlobal(pos));
        if (picked == aExecute) {
            executePendingQueuedChangeLine(line);
        } else if (picked == aDelete) {
            removePendingQueuedChangeLine(line);
        }
    });
    pendingChangesBody->addWidget(m_pendingChangesView, 1);
    pendingChangesLayout->addLayout(pendingChangesBody, 1);
    pendingChangesBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    pendingChangesBox->setMinimumHeight(0);
    bottomContentSplit->addWidget(pendingChangesBox);
    bottomContentSplit->setStretchFactor(0, 1);
    bottomContentSplit->setStretchFactor(1, 0);
    bottomContentSplit->setSizes({700, 220});
    bottomConnLayout->addWidget(bottomContentSplit, 1);

    loadPersistedAppLogToView();

    m_logsTabs->addTab(combinedLogTab,
                       trk(QStringLiteral("t_combined_log001"),
                           QStringLiteral("Log combinado"),
                           QStringLiteral("Combined log"),
                           QStringLiteral("组合日志")));
    m_logsTabs->setCurrentIndex(0);

    auto* bottomTabsPane = new QWidget(central);
    auto* bottomTabsLayout = new QHBoxLayout(bottomTabsPane);
    bottomTabsLayout->setContentsMargins(0, 0, 0, 0);
    bottomTabsLayout->setSpacing(6);
    bottomTabsLayout->addWidget(stateProgressBox, 1);
    bottomTabsLayout->addWidget(m_logsTabs, 2);

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
    QTimer::singleShot(0, this, [this]() {
        if (!m_mainWindowGeometryState.isEmpty()) {
            restoreGeometry(m_mainWindowGeometryState);
        }
        if (m_topMainSplit && !m_topMainSplitState.isEmpty()) {
            m_topMainSplit->restoreState(m_topMainSplitState);
        }
        if (m_rightMainSplit && !m_rightMainSplitState.isEmpty()) {
            m_rightMainSplit->restoreState(m_rightMainSplitState);
        }
        if (m_connDetailSplit && !m_connDetailSplitState.isEmpty()) {
            m_connDetailSplit->restoreState(m_connDetailSplitState);
        }
        if (m_verticalMainSplit && !m_verticalMainSplitState.isEmpty()) {
            m_verticalMainSplit->restoreState(m_verticalMainSplitState);
        }
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
    connect(m_connectionsTable, &QTableWidget::currentCellChanged, this,
            [this](int, int, int, int) { onConnectionSelectionChanged(); });
    if (m_connectivityMatrixBtn) {
        connect(m_connectivityMatrixBtn, &QPushButton::clicked, this, [this]() {
            openConnectivityMatrixDialog();
        });
        QTimer::singleShot(0, this, [this]() { repositionConnectivityButton(); });
    }
    connect(m_connectionsTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!m_connectionsTable || !item || m_syncConnSelectorChecks) {
            return;
        }
        const int col = item->column();
        if (col != 1 && col != 2) {
            return;
        }
        bool ok = false;
        const int connIdx = item->data(Qt::UserRole).toInt(&ok);
        if (!ok || connIdx < 0 || connIdx >= m_profiles.size()) {
            return;
        }
        const Qt::CheckState src = (m_connectionsTable->item(item->row(), 1)
                                        ? m_connectionsTable->item(item->row(), 1)->checkState()
                                        : Qt::Unchecked);
        const Qt::CheckState dst = (m_connectionsTable->item(item->row(), 2)
                                        ? m_connectionsTable->item(item->row(), 2)->checkState()
                                        : Qt::Unchecked);
        QString mode;
        if (src == Qt::Checked && dst == Qt::Checked) {
            mode = QStringLiteral("both");
        } else if (src == Qt::Checked) {
            mode = QStringLiteral("source");
        } else if (dst == Qt::Checked) {
            mode = QStringLiteral("target");
        }
        applyConnectionDisplayMode(connIdx, mode);
    });
    connect(m_connectionsTable, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (!m_connectionsTable || row < 0) {
            return;
        }
        if (col == 1 || col == 2) {
            return;
        }
        QTableWidgetItem* it = m_connectionsTable->item(row, 0);
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
            Q_UNUSED(idx);
            // El árbol superior ya no debe reconstruirse por pestaña/pool individual.
            // Se mantiene el tab bar solo por compatibilidad visual residual.
        });
    }
    if (m_bottomConnectionEntityTabs) {
        connect(m_bottomConnectionEntityTabs, &QTabBar::currentChanged, this, [this](int idx) {
            Q_UNUSED(idx);
            // El árbol inferior ya no debe reconstruirse por pestaña/pool individual.
            // Se mantiene el tab bar solo por compatibilidad visual residual.
        });
    }
    auto connTokenFromTreeSelectionBottom = [this](QTreeWidget* tree) -> QString {
        if (!tree) {
            return QString();
        }
        if (tree == m_bottomConnContentTree) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* root = tree->topLevelItem(i);
                if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
                    continue;
                }
                const int connIdx = root->data(0, kConnIdxRole).toInt();
                const QString poolName = root->data(0, kPoolNameRole).toString().trimmed();
                if (connIdx >= 0 && connIdx < m_profiles.size() && !poolName.isEmpty()) {
                    return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
                }
            }
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
            saveConnContentTreeStateFor(tree, token);
            rebuildConnectionEntityTabs();
            return;
        }
        if (tree == m_bottomConnContentTree) {
            saveConnContentTreeStateFor(tree, token);
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
            refreshPermissionsTreeNode(tree, refreshedOwner, true);
            refreshedOwner->setExpanded(ownerExpanded);
            restoreExpandedPaths(refreshedOwner, ownerExpandedPaths);
            appLog(QStringLiteral("DEBUG"),
                   QStringLiteral("executePermissionCommand after local-restore action=%1 dataset=%2 ownerExpanded=%3")
                       .arg(actionName,
                            datasetName,
                            refreshedOwner->isExpanded() ? QStringLiteral("1") : QStringLiteral("0")));
            restoreConnContentTreeState(tree, token);
            QPointer<QTreeWidget> safeTree(tree);
            QTimer::singleShot(0, this, [this, safeTree, connIdx, poolName, datasetName, token]() {
                if (!safeTree) {
                    return;
                }
                QTreeWidgetItem* ownerNode =
                    findConnContentDatasetItemFor(safeTree, connIdx, poolName, datasetName);
                if (ownerNode) {
                    populateDatasetPermissionsNode(safeTree, ownerNode, false);
                    restoreConnContentTreeState(safeTree, token);
                }
            });
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
            m_connectionsTable->setCurrentCell(idxAt.row(), 0);
        } else {
            rowForMenu = m_connectionsTable->currentRow();
        }
        if (rowForMenu >= 0 && rowForMenu < m_connectionsTable->rowCount()) {
            QTableWidgetItem* it = m_connectionsTable->item(rowForMenu, 0);
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
        const bool hasWindowsUnixLayerReady =
            hasConn
            && connIdx < m_states.size()
            && isWindowsConnection(connIdx)
            && m_states[connIdx].unixFromMsysOrMingw
            && m_states[connIdx].missingUnixCommands.isEmpty()
            && !m_states[connIdx].detectedUnixCommands.isEmpty();
        const bool canManageGsa =
            hasConn && !actionsLocked() && !isDisconnected
            && connIdx < m_states.size()
            && gsaMenuLabelForConnection(connIdx).compare(
                   trk(QStringLiteral("t_gsa_ok_001"),
                       QStringLiteral("GSA actualizado y funcionando"),
                       QStringLiteral("GSA updated and running"),
                       QStringLiteral("GSA 已更新并运行中")),
                   Qt::CaseInsensitive) != 0;
        const bool canUninstallGsa =
            hasConn && !actionsLocked() && !isDisconnected
            && connIdx < m_states.size()
            && m_states[connIdx].gsaInstalled;
        const zfsmgr::uilogic::ConnectionContextMenuState menuState =
            zfsmgr::uilogic::buildConnectionContextMenuState(
                hasConn,
                isDisconnected,
                actionsLocked(),
                hasConn && isLocalConnection(connIdx),
                hasConn && isConnectionRedirectedToLocal(connIdx),
                hasConn && isWindowsConnection(connIdx),
                hasWindowsUnixLayerReady,
                canManageGsa,
                canUninstallGsa);

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
        QAction* aInstallHelpers = menu.addAction(
            trk(QStringLiteral("t_install_helpers_ctx001"),
                QStringLiteral("Instalar comandos auxiliares"),
                QStringLiteral("Install helper commands"),
                QStringLiteral("安装辅助命令")));
        QMenu* refreshMenu = menu.addMenu(
            trk(QStringLiteral("t_refresh_conn_ctx001"),
                QStringLiteral("Refrescar"),
                QStringLiteral("Refresh"),
                QStringLiteral("刷新")));
        QAction* aRefresh = refreshMenu->addAction(
            trk(QStringLiteral("t_refresh_this_conn_001"),
                QStringLiteral("Esta conexión"),
                QStringLiteral("This connection"),
                QStringLiteral("此连接")));
        QAction* aRefreshAll = refreshMenu->addAction(
            trk(QStringLiteral("t_refresh_all_001"),
                QStringLiteral("Todas las conexiones"),
                QStringLiteral("All connections"),
                QStringLiteral("所有连接")));
        QMenu* gsaMenu = menu.addMenu(QStringLiteral("GSA"));
        QAction* aManageGsa = gsaMenu->addAction(hasConn ? gsaMenuLabelForConnection(connIdx)
                                                         : trk(QStringLiteral("t_gsa_install_001"),
                                                               QStringLiteral("Instalar gestor de snapshots"),
                                                               QStringLiteral("Install snapshot manager"),
                                                               QStringLiteral("安装快照管理器")));
        QAction* aUninstallGsa = gsaMenu->addAction(
            trk(QStringLiteral("t_gsa_uninstall_001"),
                QStringLiteral("Desinstalar el GSA"),
                QStringLiteral("Uninstall GSA"),
                QStringLiteral("卸载 GSA")));
        menu.addSeparator();
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
        aConnect->setEnabled(menuState.canConnect);
        aDisconnect->setEnabled(menuState.canDisconnect);
        aInstallMsys->setEnabled(menuState.canInstallMsys);
        const bool canInstallHelpers =
            hasConn && !actionsLocked() && !isDisconnected
            && connIdx < m_states.size()
            && m_states[connIdx].helperInstallSupported;
        aInstallHelpers->setEnabled(canInstallHelpers);
        aManageGsa->setEnabled(menuState.canManageGsa);
        aUninstallGsa->setEnabled(menuState.canUninstallGsa);
        gsaMenu->setEnabled(menuState.gsaSubmenuEnabled);
        aRefresh->setEnabled(menuState.canRefreshThis);
        aRefreshAll->setEnabled(menuState.canRefreshAll);
        aEdit->setEnabled(menuState.canEditDelete);
        aDelete->setEnabled(menuState.canEditDelete);
        aNewConn->setEnabled(menuState.canNewConnection);
        aNewPool->setEnabled(menuState.canNewPool);

        QAction* chosen = menu.exec(m_connectionsTable->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if (chosen == aConnect && hasConn) {
            logUiAction(QStringLiteral("Conectar conexión (menú conexiones)"));
            beginTransientUiBusy(
                trk(QStringLiteral("t_connecting_conn_busy_001"),
                    QStringLiteral("Conectando %1..."),
                    QStringLiteral("Connecting %1..."),
                    QStringLiteral("正在连接 %1...")).arg(m_profiles[connIdx].name));
            setConnectionDisconnected(connIdx, false);
            appLog(QStringLiteral("NORMAL"), QStringLiteral("Conexión marcada como conectada: %1").arg(m_profiles[connIdx].name));
            rebuildConnectionsTable();
            populateAllPoolsTables();
            refreshConnectionByIndex(connIdx);
            endTransientUiBusy();
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
        } else if (chosen == aInstallHelpers) {
            logUiAction(QStringLiteral("Instalar comandos auxiliares (menú conexiones)"));
            installHelperCommandsForSelectedConnection();
        } else if (chosen == aManageGsa && hasConn) {
            logUiAction(QStringLiteral("Gestionar GSA (menú conexiones)"));
            installOrUpdateGsaForConnection(connIdx);
        } else if (chosen == aUninstallGsa && hasConn) {
            logUiAction(QStringLiteral("Desinstalar GSA (menú conexiones)"));
            uninstallGsaForConnection(connIdx);
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
    if (m_connContentPropsTable) {
        connect(m_connContentPropsTable, &QTableWidget::cellChanged, this, [this](int row, int col) {
            onDatasetPropsCellChanged(row, col);
        });
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
