#include "HomeBoard.h"
#include "HomeLayout.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>

#include <memory>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

HomeTile tile(const QString &key, const QString &title,
              const QString &detail = QString(), int badge = 0,
              bool live = false)
{
    HomeTile made;
    made.key = key;
    made.title = title;
    made.icon = QStringLiteral("home");
    made.detail = detail;
    made.badge = badge;
    made.live = live;
    return made;
}

void settle(HomeBoard *board, int width, int height)
{
    board->resize(width, height);
    QCoreApplication::processEvents();
}

void mouse(HomeBoard *board, QEvent::Type type, const QPoint &pos,
           Qt::MouseButtons held)
{
    QMouseEvent event(type, pos, board->mapToGlobal(pos), Qt::LeftButton, held,
                      Qt::NoModifier);
    QApplication::sendEvent(board, &event);
}

void click(HomeBoard *board, const QPoint &pos)
{
    mouse(board, QEvent::MouseButtonPress, pos, Qt::LeftButton);
    mouse(board, QEvent::MouseButtonRelease, pos, Qt::NoButton);
}

void drag(HomeBoard *board, const QPoint &from, const QPoint &to)
{
    mouse(board, QEvent::MouseButtonPress, from, Qt::LeftButton);
    mouse(board, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
    mouse(board, QEvent::MouseMove, to, Qt::LeftButton);
    mouse(board, QEvent::MouseButtonRelease, to, Qt::NoButton);
}

bool wheel(HomeBoard *board, const QPoint &angle)
{
    const QPointF pos(10, 10);
    QWheelEvent event(pos, board->mapToGlobal(pos), QPoint(), angle,
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(board, &event);
    return event.isAccepted();
}

bool trigger(HomeBoard *board, const QString &key, const char *action)
{
    const QPoint at = key.isEmpty() ? QPoint(1, board->height() - 2)
                                    : board->tileRect(key).center();
    std::unique_ptr<QMenu> menu(board->contextMenuFor(at));
    QAction *found = menu->findChild<QAction *>(QString::fromLatin1(action));
    if (!found || !found->isEnabled())
        return false;
    found->trigger();
    return true;
}

void layoutChecks()
{
    using HomeLayout::Pages;
    const QStringList present = {"a", "b", "c", "d"};
    check(HomeLayout::resolve({}, present) == Pages({present}),
          "no saved layout puts every icon on one page");
    check(HomeLayout::resolve({{"c", "gone", "a"}, {"b", "c"}}, present) ==
              Pages({{"c", "a"}, {"b", "d"}}),
          "resolve drops unknown and repeated keys and appends new ones");
    Pages pages = HomeLayout::moveToPage({present}, "b", 1);
    check(pages == Pages({{"a", "c", "d"}, {"b"}}),
          "moving past the last page makes a new page");
    check(HomeLayout::moveToPage(pages, "b", 0) == Pages({{"a", "c", "d", "b"}}),
          "a page left empty by a move disappears");
    check(HomeLayout::moveWithinPage({present}, "d", 0) ==
              Pages({{"d", "a", "b", "c"}}),
          "an icon moves within its page");
    check(HomeLayout::removePage({{"a"}, {"b", "c"}}, 1) ==
              Pages({{"a", "b", "c"}}),
          "removing a page hands its icons to the page before");
    check(HomeLayout::removePage({{"a"}, {"b"}}, 0) == Pages({{"b", "a"}}),
          "removing the first page hands its icons to the next");
    check(HomeLayout::removePage({{"a"}}, 0) == Pages({{"a"}}),
          "the only page is never removed");
    check(HomeLayout::fromJson(HomeLayout::toJson({{"a", "b"}, {}})) ==
              Pages({{"a", "b"}, {}}),
          "the layout round-trips through JSON");
    check(HomeLayout::fromJson(QStringLiteral("not json")).isEmpty(),
          "a corrupt saved layout reads as none");
}

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    HomeBoard board;
    int iconRequests = 0;
    board.setIconProvider([&iconRequests](const QString &, const QColor &,
                                          int size) {
        ++iconRequests;
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        return pixmap;
    });
    board.show();
    const int columns = 4;
    settle(&board,
           columns * (HomeBoard::kTileWidth + HomeBoard::kGap) +
               HomeBoard::kGap,
           400);

    const QVector<HomeGroup> groups = {
        {QStringLiteral("work"), QStringLiteral("Work"),
         {tile(QStringLiteral("agents"), QStringLiteral("Agents"),
               QStringLiteral("3 running"), 5, true),
          tile(QStringLiteral("code"), QStringLiteral("Code")),
          tile(QStringLiteral("git"), QStringLiteral("Git"),
               QStringLiteral("2 changed"), 2),
          tile(QStringLiteral("repos"), QStringLiteral("Repos")),
          tile(QStringLiteral("tasks"), QStringLiteral("Tasks"),
               QStringLiteral("4 open"), 4)}},
        {QStringLiteral("people"), QStringLiteral("People"),
         {tile(QStringLiteral("chat"), QStringLiteral("Chat"),
               QStringLiteral("Caught up"))}},
    };
    check(board.setGroups(groups), "first groups change the board");
    check(!board.setGroups(groups), "identical groups are a no-op");
    check(board.visibleCount() == 6, "every tile is placed");
    check(board.pageCount() == 1 && !board.customized(),
          "with no saved layout every icon is on one page");
    check(board.visibleKeys().first() == QLatin1String("agents") &&
              board.visibleKeys().last() == QLatin1String("chat"),
          "tiles keep the group and rail order");

    const QRect first = board.tileRect(QStringLiteral("agents"));
    const QRect fifth = board.tileRect(QStringLiteral("tasks"));
    check(first.top() == HomeBoard::kGap &&
              first.width() == HomeBoard::kTileWidth,
          "the first row starts at the top");
    check(fifth.left() == first.left() && fifth.top() > first.top(),
          "a fifth tile wraps to a second row at four columns");
    check(board.contentHeight() > board.tileRect(QStringLiteral("chat")).bottom(),
          "content height covers the last tile");
    check(board.minimumHeight() == board.contentHeight(),
          "the board asks the scroll area for its full height");

    check(board.animating(), "a live tile animates while shown");

    QString activated;
    board.setOnActivate([&activated](const QString &key) { activated = key; });
    click(&board, board.tileRect(QStringLiteral("git")).center());
    check(activated == QLatin1String("git"), "clicking a tile activates it");
    activated.clear();
    click(&board, QPoint(2, 2));
    check(activated.isEmpty(), "clicking the gap activates nothing");

    QString uninstalled;
    board.setTileMenu([&uninstalled](QMenu *menu, const QString &key) {
        if (key != QLatin1String("git"))
            return;
        QAction *action = menu->addAction(QStringLiteral("Uninstall Git"));
        action->setObjectName(QStringLiteral("packageUninstallAction"));
        QObject::connect(action, &QAction::triggered,
                         [&uninstalled, key] { uninstalled = key; });
    });
    check(trigger(&board, QStringLiteral("git"), "packageUninstallAction") &&
              uninstalled == QLatin1String("git"),
          "a tile's right-click menu carries the actions the host adds for it");
    check(!trigger(&board, QStringLiteral("agents"), "packageUninstallAction"),
          "the host adds nothing to tiles it has no actions for");
    board.setTileMenu(nullptr);

    HomeLayout::Pages saved;
    int layoutChanges = 0;
    board.setOnLayoutChanged([&](const HomeLayout::Pages &pages) {
        saved = pages;
        ++layoutChanges;
    });
    int dotsCount = -1;
    int dotsPage = -1;
    board.setOnPagesChanged([&](int count, int page) {
        dotsCount = count;
        dotsPage = page;
    });

    check(!trigger(&board, QStringLiteral("agents"), "homeMoveLeft"),
          "the first icon cannot move left");
    check(trigger(&board, QStringLiteral("agents"), "homeMoveRight"),
          "right-click offers Move Right");
    check(board.visibleKeys().mid(0, 2) ==
              QStringList({QStringLiteral("code"), QStringLiteral("agents")}),
          "Move Right swaps an icon with its neighbour");
    check(layoutChanges == 1 && board.customized() && saved == board.pages(),
          "an arranged layout is handed out to be saved");

    check(trigger(&board, QStringLiteral("chat"), "homeMoveToNewPage"),
          "right-click offers Move to Page > New Page");
    check(board.pageCount() == 2 && dotsCount == 2 && dotsPage == 0,
          "a new page appears and the dots count it");
    check(!board.visibleKeys().contains(QStringLiteral("chat")),
          "the moved icon leaves the current page");
    check(trigger(&board, QStringLiteral("repos"), "homeMoveToPage2"),
          "right-click offers Move to Page > Page 2");
    check(!trigger(&board, QStringLiteral("git"), "homeMoveToPage1"),
          "an icon's own page is not offered");

    check(wheel(&board, QPoint(-120, 0)) && board.page() == 1 && dotsPage == 1,
          "a sideways swipe turns to the next page");
    check(board.visibleKeys() ==
              QStringList({QStringLiteral("chat"), QStringLiteral("repos")}),
          "page two holds the icons moved to it, in order");
    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QApplication::sendEvent(&board, &left);
    check(board.page() == 0, "the Left key turns back a page");
    board.setPage(1);

    drag(&board, board.tileRect(QStringLiteral("repos")).center(),
         board.tileRect(QStringLiteral("chat")).center());
    check(board.visibleKeys() ==
              QStringList({QStringLiteral("repos"), QStringLiteral("chat")}),
          "dragging an icon onto another reorders the page");
    check(!board.dragging() && saved == board.pages(),
          "a finished drag is saved");

    check(trigger(&board, QString(), "homeAddPage") && board.pageCount() == 3 &&
              board.page() == 2 && board.visibleCount() == 0,
          "Add Page opens a new empty page");
    check(trigger(&board, QString(), "homeRemovePage") &&
              board.pageCount() == 2 && board.page() == 1,
          "Remove Page drops the empty page");

    HomeBoard restored;
    restored.resize(board.size());
    restored.setSavedPages(saved);
    restored.setGroups(groups);
    restored.setPage(1);
    check(restored.pageCount() == 2 &&
              restored.visibleKeys() ==
                  QStringList({QStringLiteral("repos"), QStringLiteral("chat")}),
          "a saved layout is restored");

    board.setFilter(QStringLiteral("chat"));
    check(board.visibleKeys() == QStringList({QStringLiteral("chat")}),
          "the filter matches tile titles");
    check(dotsCount == 0, "searching hides the page dots");
    board.setFilter(QStringLiteral("agents"));
    check(board.visibleKeys() == QStringList({QStringLiteral("agents")}),
          "the filter searches every page, not just the current one");
    board.setFilter(QStringLiteral("work"));
    check(board.visibleCount() == 5, "the filter matches group titles");
    board.setFilter(QStringLiteral("open"));
    check(board.firstVisibleKey() == QLatin1String("tasks"),
          "the filter matches the detail line");
    check(!wheel(&board, QPoint(-120, 0)),
          "swiping does not turn pages while searching");
    board.setFilter(QStringLiteral("nothing-here"));
    check(board.visibleCount() == 0 && board.firstVisibleKey().isEmpty(),
          "an unmatched filter shows nothing");
    board.setFilter(QString());
    check(dotsCount == 2, "clearing the search brings the dots back");

    check(trigger(&board, QString(), "homeResetLayout") &&
              board.pageCount() == 1 && saved.isEmpty() &&
              !board.customized() && board.visibleCount() == 6,
          "Reset Home Screen puts every icon back on one page");
    check(!trigger(&board, QString(), "homeResetLayout"),
          "reset is offered only after arranging");

    int pulls = 0;
    bool atTop = true;
    board.setOnPull([&pulls] { ++pulls; });
    board.setPullGate([&atTop] { return atTop; });
    check(!wheel(&board, QPoint(0, -120)) && pulls == 0,
          "scrolling down does not pull the search");
    check(wheel(&board, QPoint(0, 60)) && pulls == 0,
          "a short pull is not enough");
    wheel(&board, QPoint(0, 60));
    check(pulls == 1, "pulling down at the top asks for the search");
    atTop = false;
    check(!wheel(&board, QPoint(0, 240)) && pulls == 1,
          "below the top a wheel up scrolls instead of pulling");
    atTop = true;
    mouse(&board, QEvent::MouseButtonPress, QPoint(2, 2), Qt::LeftButton);
    mouse(&board, QEvent::MouseMove, QPoint(2, 2 + HomeBoard::kPullDragPx),
          Qt::LeftButton);
    mouse(&board, QEvent::MouseButtonRelease,
          QPoint(2, 2 + HomeBoard::kPullDragPx), Qt::NoButton);
    check(pulls == 2, "dragging the empty space down pulls the search");

    QString typed;
    board.setOnType([&typed](const QString &text) { typed += text; });
    QKeyEvent letter(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier,
                     QStringLiteral("g"));
    QApplication::sendEvent(&board, &letter);
    check(typed == QLatin1String("g"), "typing on the board starts a search");

    iconRequests = 0;
    board.repaint();
    check(iconRequests == 6, "every tile paints its icon");

    board.hide();
    QCoreApplication::processEvents();
    check(!board.animating(), "a hidden board stops animating");

    QVector<HomeGroup> quiet = groups;
    quiet[0].tiles[0].live = false;
    board.show();
    board.setGroups(quiet);
    QCoreApplication::processEvents();
    check(!board.animating(), "no live tile means no timer");

    HomePageDots dots;
    dots.resize(200, 24);
    int picked = -1;
    dots.setOnPick([&picked](int index) { picked = index; });
    dots.setPages(3, 0);
    const QPoint third = dots.dotRect(2).center();
    QMouseEvent press(QEvent::MouseButtonPress, third, dots.mapToGlobal(third),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dots, &press);
    check(picked == 2, "clicking a dot picks its page");

    layoutChecks();

    if (failures == 0)
        std::printf("home board: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
