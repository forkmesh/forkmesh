#include "GenieLandmarks.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStringList>
#include <QTabBar>

#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

GenieStage flat(double left, double right, double floor, const QList<QRectF> &blocks = {})
{
    GenieStage stage;
    stage.left = left;
    stage.right = right;
    stage.side = 78;
    stage.floor = [floor](double) { return floor; };
    stage.blocks = blocks;
    return stage;
}

GenieMark mark(GenieMarkKind kind, const QRectF &rect, int group = -1)
{
    return GenieMark{kind, rect, group};
}

bool clearAt(const GenieStage &stage, double x)
{
    return GenieKeepClear::coverageAt(stage, x) <= 0;
}

void testBody()
{
    const QRectF body = GenieKeepClear::body(100, 500, 78);
    check(body.bottom() < 500, "the body ends above the floor line");
    check(body.left() > 100 - 39 && body.right() < 100 + 39, "the body sits inside the widget's width");
    check(GenieKeepClear::coverage(body, {QRectF(0, 0, 50, 50)}) == 0, "a far rect does not cover it");
    check(GenieKeepClear::coverage(body, {QRectF(90, 480, 10, 10)}) == 100, "coverage is the overlap area");
    check(GenieKeepClear::coverage(body, {QRectF(0, 500, 400, 30)}) == 0,
          "a button below the floor line, on the composer it stands on, is not covered");
}

void testClearSpot()
{
    const GenieStage empty = flat(40, 760, 500);
    check(GenieKeepClear::clearSpot(empty, 300) == 300, "with nothing to avoid it stays where it wanted");
    check(GenieKeepClear::clearSpot(empty, 5000) == 760, "the wanted spot is clamped to the floor");

    const QRectF button(280, 460, 60, 30);
    const GenieStage one = flat(40, 760, 500, {button});
    check(!clearAt(one, 300), "standing on the button covers it");
    const double moved = GenieKeepClear::clearSpot(one, 300);
    check(clearAt(one, moved), "the clear spot does not cover the button");
    check(moved < 280, "it steps to the nearer side of the button");
    check(std::abs(moved - 300) < 80, "the clear spot is next to where it was");

    const double right = GenieKeepClear::clearSpot(one, 335);
    check(clearAt(one, right) && right > 340, "from the right half it steps off to the right");

    const GenieStage wall = flat(40, 760, 500, {QRectF(0, 470, 300, 30), QRectF(300, 480, 500, 30)});
    const double least = GenieKeepClear::clearSpot(wall, 100);
    check(GenieKeepClear::coverageAt(wall, least) < GenieKeepClear::coverageAt(wall, 100),
          "with no clear spot it takes the least covered one");

    const QRectF high(100, 100, 400, 40);
    const GenieStage above = flat(40, 760, 500, {high});
    check(GenieKeepClear::clearSpot(above, 200) == 200, "buttons far above the body do not move it");

    GenieStage composer = flat(40, 760, 500, {QRectF(400, 440, 40, 24)});
    composer.floor = [](double x) { return x >= 380 && x <= 700 ? 430.0 : 500.0; };
    check(clearAt(composer, 420), "on the composer its buttons below the top edge are not covered");
    check(!clearAt(composer, 375), "beside the composer at the bottom the composer's button is covered");

    QList<QRectF> logos;
    for (int x = 50; x < 760; x += 140)
        logos << QRectF(x, 470, 40, 40);
    const GenieStage row = flat(40, 760, 500, logos);
    const double gap = GenieKeepClear::clearSpot(row, 70);
    check(clearAt(row, gap), "it finds a gap between pictures");
}

void testBag()
{
    GenieInteractionBag bag(7);
    QSet<int> seen;
    for (int i = 0; i < kGenieInteractionCount; ++i)
        seen.insert(int(bag.take([](GenieInteraction) { return true; })));
    check(seen.size() == kGenieInteractionCount, "all interactions are used before one repeats");
    check(!seen.contains(int(GenieInteraction::None)), "the bag never hands out None");

    GenieInteractionBag picky(3);
    for (int i = 0; i < 30; ++i)
        check(picky.take([](GenieInteraction k) { return k == GenieInteraction::Wave; }) == GenieInteraction::Wave,
              "a page that fits one interaction keeps getting it");
    check(picky.left() < kGenieInteractionCount, "the bag holds each kind at most once");

    GenieInteractionBag none(5);
    check(none.take([](GenieInteraction) { return false; }) == GenieInteraction::None,
          "nothing fits means no interaction");
    check(none.left() == kGenieInteractionCount, "a miss leaves one round in the bag");

    GenieInteractionBag order(11);
    const GenieInteraction first = order.take([](GenieInteraction k) { return k != GenieInteraction::Sniff; });
    QSet<int> rest{int(first)};
    for (int i = 1; i < kGenieInteractionCount - 1; ++i)
        rest.insert(int(order.take([](GenieInteraction k) { return k != GenieInteraction::Sniff; })));
    check(rest.size() == kGenieInteractionCount - 1, "skipped kinds wait while the others are used once each");
    check(order.take([](GenieInteraction) { return true; }) == GenieInteraction::Sniff,
          "the skipped kind comes first once it fits");
}

void testFits()
{
    const QList<GenieInteraction> kinds = GeniePlay::all();
    check(kinds.size() == kGenieInteractionCount && kGenieInteractionCount >= 12, "at least 12 interactions");
    QSet<QString> names;
    for (const GenieInteraction kind : kinds) {
        bool any = false;
        for (int m = 0; m <= int(GenieMarkKind::Progress); ++m)
            any = any || GeniePlay::fits(kind, GenieMarkKind(m));
        check(any, "every interaction fits some landmark");
        names.insert(GeniePlay::name(kind));
    }
    check(names.size() == kGenieInteractionCount, "every interaction has its own name");
    check(GeniePlay::fits(GenieInteraction::Sniff, GenieMarkKind::Picture), "it sniffs pictures");
    check(!GeniePlay::fits(GenieInteraction::Sniff, GenieMarkKind::Button), "it does not sniff buttons");
    check(GeniePlay::fits(GenieInteraction::Cheer, GenieMarkKind::Progress), "it cheers progress bars");
    check(GeniePlay::fits(GenieInteraction::Eureka, GenieMarkKind::Input), "eureka at a text box");
    check(GeniePlay::fits(GenieInteraction::Startle, GenieMarkKind::Button), "surprise at a button");
    check(GeniePlay::fits(GenieInteraction::Read, GenieMarkKind::Heading), "it reads headings");
}

void testMagic()
{
    const QList<GenieInteraction> kinds = GeniePlay::all();
    check(kGenieInteractionCount == 44 && kGenieMagicCount == 30, "14 plays and 30 genie effects");
    QList<GenieInteraction> magic;
    for (const GenieInteraction kind : kinds) {
        if (GeniePlay::magic(kind))
            magic << kind;
    }
    check(magic.size() == kGenieMagicCount, "30 of the interactions are genie effects");
    check(magic.first() == GenieInteraction::Poof && magic.last() == GenieInteraction::Whirlwind,
          "the effects run from Poof to Whirlwind");
    check(!GeniePlay::magic(GenieInteraction::Compare) && !GeniePlay::magic(GenieInteraction::None),
          "the older plays are not effects");
    const QStringList expected{
        QStringLiteral("Poof"),       QStringLiteral("Levitate"),      QStringLiteral("Lamp"),
        QStringLiteral("Wishes"),     QStringLiteral("Smoke rings"),   QStringLiteral("Shimmer"),
        QStringLiteral("Stardust"),   QStringLiteral("Carpet"),        QStringLiteral("Vortex"),
        QStringLiteral("Crystal"),    QStringLiteral("Runes"),         QStringLiteral("Halo"),
        QStringLiteral("Fireflies"),  QStringLiteral("Whirl"),         QStringLiteral("Grow"),
        QStringLiteral("Bottle"),     QStringLiteral("Rainbow"),       QStringLiteral("Fireworks"),
        QStringLiteral("Bubbles"),    QStringLiteral("Glitter"),       QStringLiteral("Jewel beam"),
        QStringLiteral("Ripple"),     QStringLiteral("Moons"),         QStringLiteral("Constellation"),
        QStringLiteral("Crescent"),   QStringLiteral("Mirage"),        QStringLiteral("Coins"),
        QStringLiteral("Flourish"),   QStringLiteral("Incense"),       QStringLiteral("Whirlwind")};
    QStringList names;
    for (const GenieInteraction kind : std::as_const(magic))
        names << GeniePlay::name(kind);
    check(names == expected, "the effects carry the 30 shared names in order");
    for (const GenieInteraction kind : std::as_const(magic)) {
        int fitting = 0;
        for (int m = 0; m <= int(GenieMarkKind::Progress); ++m)
            fitting += GeniePlay::fits(kind, GenieMarkKind(m)) ? 1 : 0;
        check(fitting >= 2, "every effect fits at least two kinds of landmark");
    }
    for (int m = 0; m <= int(GenieMarkKind::Progress); ++m) {
        int fitting = 0;
        for (const GenieInteraction kind : std::as_const(magic))
            fitting += GeniePlay::fits(kind, GenieMarkKind(m)) ? 1 : 0;
        check(fitting >= 4, "every kind of landmark gets several effects");
    }

    GenieInteractionBag bag(21);
    QSet<int> seen;
    for (int i = 0; i < kGenieMagicCount; ++i)
        seen.insert(int(bag.take([](GenieInteraction k) { return GeniePlay::magic(k); })));
    check(seen.size() == kGenieMagicCount, "all 30 effects are used before one repeats");
    QSet<int> rest;
    for (int i = 0; i < kGenieInteractionCount - kGenieMagicCount; ++i)
        rest.insert(int(bag.take([](GenieInteraction) { return true; })));
    check(rest.size() == kGenieInteractionCount - kGenieMagicCount, "the older plays wait in the same bag");
    for (const int k : std::as_const(rest))
        check(!GeniePlay::magic(GenieInteraction(k)), "the rest of the round is the older plays");

    const QRectF button(500, 380, 80, 28);
    const GenieStage stage = flat(40, 760, 500, {button});
    const QList<GenieMark> marks{mark(GenieMarkKind::Button, button)};
    const GeniePlan poof = GeniePlay::plan(GenieInteraction::Poof, marks, stage, 150, 0);
    check(poof.valid() && GenieKeepClear::coverageAt(stage, poof.spot) <= 0, "it poofs to a clear spot by a button");
    check(!GeniePlay::plan(GenieInteraction::Poof, marks, stage, poof.spot, 0).valid(),
          "no poof when it is already beside it");
    const GeniePlan lamp = GeniePlay::plan(GenieInteraction::Lamp, marks, stage, 150, 0);
    check(lamp.valid() && std::abs(lamp.spot - button.center().x()) < 120, "it rubs the lamp next to the button");
    check(!GeniePlay::plan(GenieInteraction::Crescent, marks, stage, 150, 0).valid(),
          "no crescent over a button");
}

void testPlans()
{
    const QRectF button(500, 380, 80, 28);
    const GenieStage stage = flat(40, 760, 500, {button});
    const QList<GenieMark> marks{mark(GenieMarkKind::Button, button)};

    const GeniePlan wave = GeniePlay::plan(GenieInteraction::Wave, marks, stage, 200, 0);
    check(wave.valid(), "it waves at a button it can reach");
    check(std::abs(wave.spot - button.center().x()) < 120, "it waves from next to the button");
    check(GenieKeepClear::coverageAt(stage, wave.spot) <= 0, "the interaction spot keeps the button clear");

    check(!GeniePlay::plan(GenieInteraction::Sniff, marks, stage, 200, 0).valid(), "no picture, nothing to sniff");
    check(!GeniePlay::plan(GenieInteraction::Wave, {mark(GenieMarkKind::Button, QRectF(500, 20, 80, 28))}, stage,
                           200, 0)
               .valid(),
          "a button far above is out of reach");
    check(!GeniePlay::plan(GenieInteraction::Wave, {mark(GenieMarkKind::Button, QRectF(500, 520, 80, 28))}, stage,
                           200, 0)
               .valid(),
          "a button below its floor is not played with");

    const GeniePlan silkLow = GeniePlay::plan(GenieInteraction::Silk, marks, stage, 200, 0);
    check(silkLow.valid(), "silk reaches a button 100 px up");
    check(!GeniePlay::plan(GenieInteraction::Silk, {mark(GenieMarkKind::Button, QRectF(500, 440, 80, 28))}, stage,
                           200, 0)
               .valid(),
          "silk needs some height");
    check(GeniePlay::plan(GenieInteraction::Silk, {mark(GenieMarkKind::Heading, QRectF(300, 60, 200, 24))}, stage,
                          200, 0)
              .valid(),
          "silk reaches a heading high up");

    const GeniePlan hop = GeniePlay::plan(GenieInteraction::Hop, marks, stage, 120, 0);
    check(hop.valid(), "it hops toward a button");
    check(hop.spot < button.left(), "it lands beside the button on the near side");
    check(GenieKeepClear::coverageAt(stage, hop.spot) <= 0, "the hop lands clear of the button");
    check(!GeniePlay::plan(GenieInteraction::Hop, marks, stage, hop.spot, 0).valid(),
          "no hop when it is already beside it");

    const QList<GenieMark> tabs{mark(GenieMarkKind::Tab, QRectF(100, 300, 90, 28), 1),
                                mark(GenieMarkKind::Tab, QRectF(190, 300, 90, 28), 1),
                                mark(GenieMarkKind::Tab, QRectF(280, 300, 90, 28), 1),
                                mark(GenieMarkKind::Tab, QRectF(600, 100, 90, 28), 2)};
    const GeniePlan nod = GeniePlay::plan(GenieInteraction::TabNod, tabs, flat(40, 760, 500), 200, 0);
    check(nod.valid() && nod.marks.size() == 3, "it nods along the row of three tabs");
    check(nod.valid() && nod.marks.first().left() < nod.marks.last().left(), "tabs are walked left to right");
    check(!GeniePlay::plan(GenieInteraction::TabNod, {tabs.last()}, flat(40, 760, 500), 200, 0).valid(),
          "one tab is not a row");
    const GeniePlan count = GeniePlay::plan(GenieInteraction::Count, tabs, flat(40, 760, 500), 200, 0);
    check(count.valid() && count.marks.size() == 3, "it counts the tabs");

    QList<GenieMark> rows;
    for (int i = 0; i < 9; ++i)
        rows << mark(GenieMarkKind::Row, QRectF(40, 100 + i * 30, 600, 28), 4);
    const GeniePlan rowCount = GeniePlay::plan(GenieInteraction::Count, rows, flat(40, 760, 500), 200, 0);
    check(rowCount.valid() && rowCount.marks.size() == 5, "it counts at most five rows");
    check(rowCount.valid() && rowCount.marks.last().top() == 340, "it counts the rows nearest its floor");

    const QList<GenieMark> pair{mark(GenieMarkKind::Button, QRectF(300, 400, 70, 28)),
                                mark(GenieMarkKind::Button, QRectF(420, 404, 70, 28))};
    const GeniePlan compare = GeniePlay::plan(GenieInteraction::Compare, pair, flat(40, 760, 500), 100, 0);
    check(compare.valid() && compare.marks.size() == 2, "it compares two buttons side by side");
    check(!GeniePlay::plan(GenieInteraction::Compare, {pair.first()}, flat(40, 760, 500), 100, 0).valid(),
          "one button is nothing to compare");

    check(!GeniePlay::plan(GenieInteraction::Read, {mark(GenieMarkKind::Label, QRectF(300, 400, 20, 16))},
                           flat(40, 760, 500), 100, 0)
               .valid(),
          "too short to read");

    QList<GenieMark> many;
    for (int i = 0; i < 8; ++i)
        many << mark(GenieMarkKind::Picture, QRectF(60 + i * 90, 380, 40, 40));
    QSet<int> spots;
    for (quint32 roll = 0; roll < 8; ++roll) {
        const GeniePlan sniff = GeniePlay::plan(GenieInteraction::Sniff, many, flat(40, 760, 500), 60, roll);
        if (sniff.valid())
            spots.insert(int(sniff.marks.first().left()));
    }
    check(spots.size() > 1 && spots.size() <= 4, "the roll picks among the nearest few landmarks");
}

void testScan()
{
    QWidget host;
    host.resize(800, 600);
    auto *button = new QPushButton(QStringLiteral("Save"), &host);
    button->setGeometry(20, 540, 80, 30);
    auto *hidden = new QPushButton(QStringLiteral("Hidden"), &host);
    hidden->setGeometry(120, 540, 80, 30);
    hidden->hide();
    auto *logo = new QLabel(&host);
    QPixmap picture(32, 32);
    picture.fill(Qt::red);
    logo->setPixmap(picture);
    logo->setGeometry(220, 530, 40, 40);
    auto *title = new QLabel(QStringLiteral("A big heading"), &host);
    QFont big = title->font();
    big.setPixelSize(big.pixelSize() > 0 ? big.pixelSize() * 2 : 30);
    title->setFont(big);
    title->setGeometry(20, 20, 400, 40);
    auto *plain = new QLabel(QStringLiteral("some words"), &host);
    plain->setGeometry(20, 80, 300, 20);
    auto *link = new QLabel(QStringLiteral("<a href=\"https://forkmesh.com\">site</a>"), &host);
    link->setTextInteractionFlags(Qt::TextBrowserInteraction);
    link->setGeometry(20, 110, 100, 20);
    auto *tabs = new QTabBar(&host);
    tabs->addTab(QStringLiteral("One"));
    tabs->addTab(QStringLiteral("Two"));
    tabs->addTab(QStringLiteral("Three"));
    tabs->setGeometry(300, 140, 400, 30);
    auto *line = new QLineEdit(&host);
    line->setGeometry(20, 200, 200, 26);
    auto *progress = new QProgressBar(&host);
    progress->setGeometry(20, 240, 200, 12);
    auto *list = new QListWidget(&host);
    list->addItems({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
    list->setGeometry(300, 200, 300, 200);
    auto *bar = new QScrollBar(Qt::Vertical, &host);
    bar->setGeometry(780, 0, 16, 600);
    auto *outside = new QPushButton(QStringLiteral("Off"), &host);
    outside->setGeometry(900, 10, 60, 30);
    auto *skipped = new QPushButton(QStringLiteral("Me"), &host);
    skipped->setGeometry(600, 540, 60, 30);
    host.show();
    QApplication::processEvents();

    const GenieScene scene = GenieLandmarks::scan(&host, {skipped});
    const auto count = [&](GenieMarkKind kind) {
        int n = 0;
        for (const GenieMark &m : scene.marks)
            n += m.kind == kind ? 1 : 0;
        return n;
    };
    const auto blocked = [&](const QRect &r) {
        for (const QRectF &b : scene.blocks)
            if (b.intersects(QRectF(r)))
                return true;
        return false;
    };
    check(count(GenieMarkKind::Button) == 1, "one visible button inside the host");
    check(count(GenieMarkKind::Picture) == 1, "the logo is a picture");
    check(count(GenieMarkKind::Heading) == 1, "the big label is a heading");
    check(count(GenieMarkKind::Label) == 2, "plain and link labels are labels");
    check(count(GenieMarkKind::Tab) == 3, "each tab is a landmark");
    check(count(GenieMarkKind::Input) == 1, "the line edit is a text box");
    check(count(GenieMarkKind::Progress) == 1, "the progress bar is a landmark");
    check(count(GenieMarkKind::Row) == 3, "the list rows are landmarks");
    check(blocked(button->geometry()), "the button is kept clear");
    check(blocked(logo->geometry()), "the logo is kept clear");
    check(blocked(link->geometry()), "a link label is kept clear");
    check(blocked(line->geometry()), "a line edit is kept clear");
    check(blocked(QRect(300, 140, 30, 30)), "tabs are kept clear");
    check(!blocked(title->geometry()), "a heading may be stood in front of");
    check(!blocked(QRect(310, 260, 200, 40)), "list rows may be stood in front of");
    check(!blocked(progress->geometry()), "a progress bar may be stood in front of");
    check(!blocked(bar->geometry()), "scroll bars are not buttons");
    check(!blocked(hidden->geometry()), "a hidden button is ignored");
    check(!blocked(skipped->geometry()), "skipped widgets are ignored");
    QSet<int> groups;
    for (const GenieMark &m : scene.marks)
        if (m.kind == GenieMarkKind::Tab)
            groups.insert(m.group);
    check(groups.size() == 1 && !groups.contains(-1), "the tabs of one bar share a group");
    for (const QRectF &b : scene.blocks)
        check(QRectF(host.rect()).contains(b), "blocks are clipped to the host");
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testBody();
    testClearSpot();
    testBag();
    testFits();
    testMagic();
    testPlans();
    testScan();
    if (failures) {
        std::cerr << failures << " genie landmark check(s) failed\n";
        return 1;
    }
    std::cout << "genie landmark tests passed\n";
    return 0;
}
