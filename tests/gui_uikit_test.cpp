// replaceScrollWidget (src/gui/uikit.cpp): a page that rebuilds itself from inside a handler
// of one of its own widgets. A double click on a directory of the Files tab did exactly that
// with QScrollArea::setWidget, which deletes the old widget at once: Qt went on using the
// table it was dispatching the double click to, and crashed (2026-11-23). Without a screen:
// QT_QPA_PLATFORM=offscreen.

#include "../src/gui/uikit.h"

#include <QApplication>
#include <QPointer>
#include <QScrollArea>
#include <QTableWidget>
#include <QTest>

#include <cstdio>

namespace {

QTableWidget* page(QScrollArea* area, int* rebuilt) {
    auto* t = new QTableWidget(1, 1);
    t->setItem(0, 0, new QTableWidgetItem(QStringLiteral("dir")));
    QObject::connect(t, &QTableWidget::cellDoubleClicked, area, [area, rebuilt](int, int) {
        ++*rebuilt;
        zfsmgr::gui::replaceScrollWidget(area, page(area, rebuilt));
    });
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QScrollArea area;
    area.setWidgetResizable(true);
    area.resize(300, 200);
    int rebuilt = 0;
    QTableWidget* first = page(&area, &rebuilt);
    zfsmgr::gui::replaceScrollWidget(&area, first);
    area.show();
    if (!QTest::qWaitForWindowExposed(&area)) {
        std::fprintf(stderr, "FAIL: the window was never shown\n");
        return 1;
    }
    QPointer<QTableWidget> old = first;
    const QRect cell = first->visualItemRect(first->item(0, 0));
    // As a person does it: a click, and a second one that is the double click.
    QTest::mouseClick(first->viewport(), Qt::LeftButton, {}, cell.center());
    QTest::mouseDClick(first->viewport(), Qt::LeftButton, {}, cell.center());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    int fails = 0;
    if (rebuilt != 1) {
        std::fprintf(stderr, "FAIL: the double click rebuilt the page %d times\n", rebuilt);
        ++fails;
    }
    if (old) {
        std::fprintf(stderr, "FAIL: the old page was not deleted afterwards\n");
        ++fails;
    }
    if (area.widget() == nullptr || area.widget() == old) {
        std::fprintf(stderr, "FAIL: the new page is not in place\n");
        ++fails;
    }
    if (fails == 0) {
        std::printf("gui uikit: all good\n");
    }
    return fails == 0 ? 0 : 1;
}
