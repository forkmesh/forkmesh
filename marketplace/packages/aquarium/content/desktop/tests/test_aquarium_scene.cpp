#include "AquariumOverlay.h"
#include "AquariumScene.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QThread>
#include <QVBoxLayout>
#include <QWindow>

#include <cmath>

using namespace forkmesh::aquarium;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        qInfo("PASS: %s", qPrintable(message));
    } else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

QList<Landmark> desk()
{
    QList<Landmark> marks;
    const auto add = [&](Spot kind, QRectF rect, const QString &text) { marks.append({kind, rect, text}); };
    add(Spot::Heading, QRectF(260, 70, 220, 28), QStringLiteral("Agents"));
    for (int i = 0; i < 4; ++i)
        add(Spot::Tab, QRectF(260 + i * 96, 120, 90, 26), QStringLiteral("Tab %1").arg(i + 1));
    add(Spot::Button, QRectF(900, 70, 96, 30), QStringLiteral("Merge"));
    add(Spot::Button, QRectF(1004, 70, 96, 30), QStringLiteral("Refresh"));
    add(Spot::Button, QRectF(1108, 70, 120, 30), QStringLiteral("New agent"));
    add(Spot::Label, QRectF(260, 170, 360, 18), QStringLiteral("Three agents are working on this branch"));
    add(Spot::Label, QRectF(660, 170, 180, 18), QStringLiteral("Updated now"));
    add(Spot::Input, QRectF(260, 640, 640, 40), QStringLiteral("Ask an agent"));
    add(Spot::Toggle, QRectF(940, 650, 18, 18), QStringLiteral("Auto merge"));
    add(Spot::Picture, QRectF(40, 70, 40, 40), QString());
    add(Spot::Progress, QRectF(260, 210, 400, 10), QString());
    for (int i = 0; i < 5; ++i)
        add(Spot::Row, QRectF(260, 240 + i * 30, 640, 28), QStringLiteral("Row number %1").arg(i + 1));
    add(Spot::Document, QRectF(920, 240, 320, 300), QStringLiteral("The transcript of the run"));
    return marks;
}

Pet spider()
{
    Pet pet;
    pet.present = true;
    pet.frame = QRectF(1000, 682, 94, 94);
    pet.body = QRectF(1008, 690, 78, 78);
    pet.image = QImage(94, 94, QImage::Format_ARGB32_Premultiplied);
    pet.image.fill(QColor(80, 40, 120, 200));
    return pet;
}

bool finite(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

void paintOnce(const Scene &scene, QImage &canvas)
{
    QPainter painter(&canvas);
    scene.paint(painter);
}

void pump(int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
}

void mouse(QWidget *window, QEvent::Type type, const QPoint &at)
{
    const QPointF local(at);
    const QPointF global(window->mapToGlobal(at));
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, local, local, global, Qt::LeftButton, held, Qt::NoModifier);
    QCoreApplication::sendEvent(window->windowHandle(), &event);
}

void click(QWidget *window, QWidget *target)
{
    const QPoint at = target->mapTo(window, target->rect().center());
    mouse(window, QEvent::MouseButtonPress, at);
    mouse(window, QEvent::MouseButtonRelease, at);
}

void doubleClick(QWidget *window, QWidget *target)
{
    const QPoint at = target->mapTo(window, target->rect().center());
    click(window, target);
    mouse(window, QEvent::MouseButtonPress, at);
    mouse(window, QEvent::MouseButtonDblClick, at);
    mouse(window, QEvent::MouseButtonRelease, at);
}

void overlayInput()
{
    QMainWindow window;
    window.resize(800, 600);
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *bar = new QWidget;
    bar->setObjectName(QStringLiteral("windowChromeBar"));
    bar->setFixedHeight(40);
    auto *barLayout = new QHBoxLayout(bar);
    auto *title = new QLabel(QStringLiteral("ForkMesh"));
    barLayout->addWidget(title);
    barLayout->addStretch();
    auto *minimize = new QPushButton(QStringLiteral("_"));
    barLayout->addWidget(minimize);
    auto *button = new QPushButton(QStringLiteral("Merge"));
    layout->addWidget(bar);
    layout->addWidget(button);
    layout->addStretch();
    window.setCentralWidget(central);
    int clicks = 0;
    QObject::connect(button, &QPushButton::clicked, [&] { ++clicks; });
    int barClicks = 0;
    QObject::connect(minimize, &QPushButton::clicked, [&] { ++barClicks; });
    window.show();
    pump(50);

    Overlay overlay(&window);
    overlay.open(false);
    check(overlay.isVisible(), QStringLiteral("the background tank covers the window"));
    click(&window, button);
    check(clicks == 1, QStringLiteral("the first click on the background tank reaches the button under it"));
    check(!overlay.opened() && !overlay.isVisible(), QStringLiteral("a click drains the background tank at once"));

    overlay.open(true);
    click(&window, button);
    check(clicks == 2, QStringLiteral("the first click on the preview tank reaches the button under it"));
    check(!overlay.opened(), QStringLiteral("a click closes the preview tank"));

    overlay.open(false);
    click(&window, title);
    doubleClick(&window, bar);
    check(overlay.opened(), QStringLiteral("pressing or double-clicking the top bar keeps the tank"));
    click(&window, minimize);
    check(barClicks == 1 && !overlay.opened(), QStringLiteral("a top-bar button gets its click and drains the tank"));

    pump(1100);
    overlay.open(false);
    overlay.activated();
    pump(900);
    check(!overlay.opened(), QStringLiteral("coming back to the app drains the tank"));

    overlay.open(false);
    overlay.activated();
    pump(100);
    click(&window, title);
    pump(900);
    check(overlay.opened(), QStringLiteral("coming back by pressing the top bar keeps the tank"));

    const QPoint top = title->mapTo(&window, title->rect().center());
    pump(1100);
    mouse(&window, QEvent::MouseButtonPress, top);
    overlay.activated();
    mouse(&window, QEvent::MouseButtonRelease, top);
    pump(900);
    check(overlay.opened(), QStringLiteral("a top-bar press just before the app comes back keeps the tank"));

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(window.windowHandle(), &escape);
    check(!overlay.opened(), QStringLiteral("Esc drains the tank"));

    overlay.open(true);
    overlay.activated();
    pump(700);
    check(overlay.opened(), QStringLiteral("the preview tank ignores the app coming back"));
    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(window.windowHandle(), &key);
    check(!overlay.opened(), QStringLiteral("typing drains the tank"));

    overlay.swimmers = [](bool) {
        return QList<Swimmer>{{7, QStringLiteral("ada"), QStringLiteral("claude|opus"), Mood::Working, 0}};
    };
    int picked = 0;
    overlay.picked = [&](int id) { picked = id; };
    overlay.open(false);
    overlay.advance(3);
    const QList<Scene::FishInfo> swimming = overlay.scene().fish();
    check(swimming.size() == 1, QStringLiteral("the agent swims in the tank"));
    if (swimming.size() == 1) {
        const QPoint at = overlay.mapTo(&window, swimming.first().pos.toPoint());
        const int before = clicks;
        mouse(&window, QEvent::MouseButtonPress, at);
        mouse(&window, QEvent::MouseButtonRelease, at);
        pump(50);
        check(picked == 7, QStringLiteral("clicking a fish opens its agent"));
        check(!overlay.opened(), QStringLiteral("clicking a fish drains the tank"));
        check(clicks == before, QStringLiteral("the click on a fish does not reach the window under it"));
    }
    picked = 0;
    overlay.open(false);
    overlay.advance(1);
    const bool water = overlay.scene().fishAt(button->mapTo(&window, button->rect().center())) == 0;
    click(&window, button);
    pump(50);
    check(!water || picked == 0, QStringLiteral("clicking open water opens no agent"));
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QSizeF size(1280, 800);
    QImage backdrop(size.toSize(), QImage::Format_ARGB32_Premultiplied);
    backdrop.fill(QColor(240, 242, 246));
    QImage canvas(size.toSize(), QImage::Format_ARGB32_Premultiplied);

    QSet<QString> names;
    for (int i = 0; i < kTricks; ++i)
        names.insert(Scene::trickName(Trick(i)));
    check(names.size() == kTricks && !names.contains(QString()), QStringLiteral("60 named tricks, all different"));
    check(kElementTricks == 30 && kPetTricks == 30, QStringLiteral("30 element tricks and 30 spider tricks"));

    Scene scene(42);
    scene.setSize(size);
    scene.setBackdrop(backdrop);
    scene.setLandmarks(desk());
    scene.setPet(spider());
    scene.setSwimmers({{1, QStringLiteral("ada"), QStringLiteral("claude|opus"), Mood::Working, 0},
                       {2, QStringLiteral("bob"), QStringLiteral("codex|gpt"), Mood::Working, 0},
                       {3, QStringLiteral("cy"), QStringLiteral("claude|sonnet"), Mood::Waiting, 0}});
    for (int i = 0; i < 60; ++i)
        scene.step(1.0 / 30);
    check(scene.fish().size() == 3, QStringLiteral("one fish per active agent"));
    for (const Scene::FishInfo &info : scene.fish())
        check(scene.fishAt(info.pos) == info.id, QStringLiteral("a click on %1 finds that fish").arg(info.name));
    check(scene.fishAt(QPointF(-500, -500)) == 0, QStringLiteral("a click away from every fish finds none"));

    for (int i = 0; i < kTricks; ++i) {
        const Trick trick = Trick(i);
        const QString name = Scene::trickName(trick);
        scene.takeCues();
        scene.takeNotes();
        check(scene.viable(trick), name + QStringLiteral(" has something to act on"));
        if (!scene.perform(1, trick)) {
            check(false, name + QStringLiteral(" starts"));
            continue;
        }
        bool done = false;
        bool inside = true;
        QList<PetCue> cues;
        for (int frame = 0; frame < 30 * 30; ++frame) {
            scene.step(1.0 / 30);
            cues += scene.takeCues();
            if (frame % 9 == 0)
                paintOnce(scene, canvas);
            for (const Scene::FishInfo &fish : scene.fish()) {
                if (!finite(fish.pos) || fish.pos.x() < -250 || fish.pos.x() > size.width() + 250 ||
                    fish.pos.y() > size.height() + 100)
                    inside = false;
            }
            const QList<Scene::FishInfo> all = scene.fish();
            const auto it = std::find_if(all.begin(), all.end(), [](const Scene::FishInfo &f) { return f.id == 1; });
            if (it != all.end() && (!it->trick || *it->trick != trick)) {
                done = true;
                break;
            }
        }
        const QList<Note> notes = scene.takeNotes();
        bool noted = false;
        for (const Note &note : notes)
            noted = noted || (note.fishId == 1 && !note.text.isEmpty() && note.fish == QStringLiteral("ada"));
        check(done, name + QStringLiteral(" finishes"));
        check(noted, name + QStringLiteral(" is logged"));
        check(inside, name + QStringLiteral(" keeps every fish in the tank"));
        int reactions = 0;
        for (const PetCue &cue : std::as_const(cues))
            reactions += cue.cue != Cue::Look ? 1 : 0;
        if (Scene::petTrick(trick))
            check(reactions > 0, name + QStringLiteral(" makes the spider react"));
    }

    {
        Scene bare(7);
        bare.setSize(size);
        bare.setLandmarks(desk());
        bare.setSwimmers({{1, QStringLiteral("ada"), QString(), Mood::Working, 0}, {2, QStringLiteral("bob"), QString(), Mood::Working, 0}});
        bool petTricks = false;
        QSet<int> seen;
        for (int frame = 0; frame < 30 * 120; ++frame) {
            bare.step(1.0 / 30);
            for (const Scene::FishInfo &fish : bare.fish()) {
                if (fish.trick) {
                    petTricks = petTricks || Scene::petTrick(*fish.trick);
                    seen.insert(int(*fish.trick));
                }
            }
        }
        check(!petTricks, QStringLiteral("no spider tricks while the spider is away"));
        check(seen.size() >= 15, QStringLiteral("fish vary their element tricks (%1 seen)").arg(seen.size()));
        check(bare.takeCues().isEmpty(), QStringLiteral("no spider cues while the spider is away"));
        check(!bare.perform(1, Trick::Hello), QStringLiteral("a spider trick cannot start without the spider"));
    }

    {
        Scene lively(9);
        lively.setSize(size);
        lively.setBackdrop(backdrop);
        lively.setLandmarks(desk());
        lively.setPet(spider());
        lively.setSwimmers({{1, QStringLiteral("ada"), QString(), Mood::Working, 0},
                            {2, QStringLiteral("bob"), QString(), Mood::Working, 0},
                            {3, QStringLiteral("cy"), QString(), Mood::Working, 0}});
        QSet<int> seen;
        int sharing = 0;
        for (int frame = 0; frame < 30 * 400; ++frame) {
            lively.step(1.0 / 30);
            int petBusy = 0;
            for (const Scene::FishInfo &fish : lively.fish()) {
                if (fish.trick) {
                    seen.insert(int(*fish.trick));
                    petBusy += Scene::petTrick(*fish.trick) ? 1 : 0;
                }
            }
            sharing = std::max(sharing, petBusy);
        }
        check(seen.size() >= 45, QStringLiteral("over time the fish do most of the 60 tricks (%1 seen)").arg(seen.size()));
        check(sharing <= 1, QStringLiteral("one fish plays with the spider at a time"));
        lively.setSwimmers({{2, QStringLiteral("bob"), QString(), Mood::Working, 0}});
        for (int frame = 0; frame < 30 * 8; ++frame)
            lively.step(1.0 / 30);
        check(lively.fish().size() == 1, QStringLiteral("fish of finished agents swim away"));
        lively.setSwimmers({});
        for (int frame = 0; frame < 30 * 8; ++frame)
            lively.step(1.0 / 30);
        check(lively.fish().isEmpty(), QStringLiteral("an empty tank when no agent runs"));
        paintOnce(lively, canvas);
    }

    {
        Scene painted(11);
        painted.setSize(size);
        painted.setBackdrop(backdrop);
        painted.setLandmarks(desk());
        painted.setPet(spider());
        painted.setSwimmers({{1, QStringLiteral("ada"), QString(), Mood::Working, 0, 12, 400, false},
                             {2, QStringLiteral("bob"), QString(), Mood::Waiting, 0},
                             {3, QStringLiteral("cy"), QString(), Mood::Working, 0, 3, 40, true},
                             {4, QStringLiteral("dee"), QString(), Mood::Stopped, 0, 5, 90, false},
                             {5, QStringLiteral("eli"), QString(), Mood::Merged, 0, 2, 10, false},
                             {6, QStringLiteral("fay"), QString(), Mood::Failed, 0, 1, 4, true}});
        QImage whole(size.toSize(), QImage::Format_RGB32);
        QImage pieces(size.toSize(), QImage::Format_RGB32);
        {
            QPainter first(&pieces);
            painted.paint(first);
        }
        painted.dirtyRegion();
        int worst = 0;
        for (int frame = 0; frame < 30 * 40; ++frame) {
            painted.step(1.0 / 30);
            const QRegion dirty = painted.dirtyRegion();
            {
                QPainter p(&pieces);
                p.setClipRegion(dirty);
                painted.paint(p);
            }
            if (frame % 60 != 59)
                continue;
            {
                QPainter p(&whole);
                painted.paint(p);
            }
            int differing = 0;
            for (int y = 0; y < whole.height(); y += 2) {
                const QRgb *a = reinterpret_cast<const QRgb *>(whole.constScanLine(y));
                const QRgb *b = reinterpret_cast<const QRgb *>(pieces.constScanLine(y));
                for (int x = 0; x < whole.width(); x += 2) {
                    if (std::abs(qRed(a[x]) - qRed(b[x])) + std::abs(qGreen(a[x]) - qGreen(b[x])) +
                            std::abs(qBlue(a[x]) - qBlue(b[x])) > 24)
                        ++differing;
                }
            }
            worst = std::max(worst, differing);
        }
        check(worst < 8, QStringLiteral("repainting only what changed leaves no trails (%1 stray pixels)").arg(worst));
    }

    {
        Scene moods(5);
        moods.setSize(size);
        moods.setBackdrop(backdrop);
        moods.setLandmarks(desk());
        moods.setPet(spider());
        const QList<Mood> all = {Mood::Working, Mood::Waiting, Mood::Queued, Mood::Done,
                                 Mood::Merged, Mood::Failed, Mood::Stopped};
        QList<Swimmer> roster;
        for (int i = 0; i < all.size(); ++i)
            roster.append({i + 1, QStringLiteral("agent %1").arg(i + 1), QStringLiteral("claude|opus"), all.at(i), 0});
        moods.setSwimmers(roster);
        QSet<Mood> tricked;
        double lowestStopped = 0;
        double failedDepth = 1e9;
        for (int frame = 0; frame < 30 * 120; ++frame) {
            moods.step(1.0 / 30);
            for (const Scene::FishInfo &fish : moods.fish()) {
                if (fish.trick)
                    tricked.insert(fish.mood);
                if (frame > 30 * 40 && fish.mood == Mood::Stopped)
                    lowestStopped = std::max(lowestStopped, size.height() - 22 - fish.pos.y());
                if (frame > 30 * 40 && fish.mood == Mood::Failed)
                    failedDepth = std::min(failedDepth, fish.pos.y());
            }
            if (frame % 97 == 0)
                paintOnce(moods, canvas);
        }
        check(moods.fish().size() == all.size(), QStringLiteral("every agent has a fish, stopped and finished ones too"));
        check(tricked.contains(Mood::Working) && tricked.contains(Mood::Done),
              QStringLiteral("working and finished fish play tricks"));
        check(!tricked.contains(Mood::Stopped) && !tricked.contains(Mood::Failed) && !tricked.contains(Mood::Queued),
              QStringLiteral("stopped, failed and queued fish play no tricks"));
        check(lowestStopped > 0 && lowestStopped < 40,
              QStringLiteral("a stopped fish sleeps on the sand (%1 px above it)").arg(lowestStopped));
        check(failedDepth > size.height() * 0.6, QStringLiteral("a failed fish keeps to the bottom of the tank"));

        roster[0].mood = Mood::Stopped;
        moods.setSwimmers(roster);
        moods.step(1.0 / 30);
        const QList<Scene::FishInfo> after = moods.fish();
        const auto first = std::find_if(after.begin(), after.end(), [](const Scene::FishInfo &f) { return f.id == 1; });
        check(first != after.end() && first->mood == Mood::Stopped && !first->trick,
              QStringLiteral("a fish whose agent stops drops its trick and changes mood"));
    }

    {
        Scene sizes(6);
        sizes.setSize(size);
        sizes.setSwimmers({{1, QStringLiteral("same"), QString(), Mood::Working, 0, 0, 0, false},
                           {2, QStringLiteral("same"), QString(), Mood::Working, 0, 40, 0, false}});
        const QList<Scene::FishInfo> fish = sizes.fish();
        check(fish.size() == 2 && fish.at(1).length > fish.at(0).length * 1.3,
              QStringLiteral("a fish with more changed files is bigger"));
    }

    {
        const auto differ = [](const QImage &a, const QImage &b) {
            int count = 0;
            for (int y = 0; y < a.height(); ++y) {
                for (int x = 0; x < a.width(); ++x) {
                    const QRgb p = a.pixel(x, y);
                    const QRgb q = b.pixel(x, y);
                    if (std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q)) +
                            std::abs(qAlpha(p) - qAlpha(q)) > 40)
                        ++count;
                }
            }
            return count;
        };
        const auto look = [](Mood mood, int files, bool conflicted) {
            return Scene::portrait({1, QStringLiteral("ada"), QStringLiteral("claude|opus"), mood, 0, files, 50, conflicted}, 96);
        };
        const QImage working = look(Mood::Working, 0, false);
        int opaque = 0;
        for (int y = 0; y < working.height(); ++y)
            for (int x = 0; x < working.width(); ++x)
                opaque += qAlpha(working.pixel(x, y)) > 128 ? 1 : 0;
        check(opaque > 96 * 96 / 8, QStringLiteral("a portrait draws the fish (%1 px)").arg(opaque));
        for (const Mood mood : {Mood::Waiting, Mood::Merged, Mood::Failed, Mood::Stopped, Mood::Queued})
            check(differ(working, look(mood, 0, false)) > 30,
                  QStringLiteral("a %1 fish looks different from a working one").arg(Scene::moodName(mood)));
        check(differ(working, look(Mood::Working, 0, true)) > 60, QStringLiteral("a conflicted fish looks different"));
        check(differ(working, look(Mood::Working, 8, false)) > 30, QStringLiteral("changed files show on the fish"));
    }

    {
        Scene snag(8);
        snag.setSize(size);
        snag.setSwimmers({{1, QStringLiteral("ada"), QString(), Mood::Working, 0, 2, 20, true}});
        for (int frame = 0; frame < 60; ++frame)
            snag.step(1.0 / 30);
        snag.dirtyRegion();
        const QRegion dirty = snag.dirtyRegion();
        const Scene::FishInfo fish = snag.fish().first();
        check(fish.conflicted && dirty.contains(QPoint(int(fish.pos.x()), int(fish.pos.y() * 0.5))),
              QStringLiteral("the fishing line of a conflicted fish is repainted"));
    }

    {
        Scene sparse(3);
        sparse.setSize(size);
        sparse.setLandmarks({{Spot::Button, QRectF(100, 100, 80, 30), QStringLiteral("Go")}});
        sparse.setSwimmers({{1, QStringLiteral("ada"), QString(), Mood::Working, 0}});
        sparse.step(1.0 / 30);
        check(!sparse.perform(1, Trick::Mirror), QStringLiteral("no mirror trick without a text box"));
        check(sparse.perform(1, Trick::Boop), QStringLiteral("a boop needs only a button"));
    }

    overlayInput();

    return failures == 0 ? 0 : 1;
}
