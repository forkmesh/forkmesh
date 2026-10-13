// The agents' field as a widget, driven directly and offscreen: it paints a
// field of agents, its hit test finds the body under a point, and a click or a
// double click reaches the callbacks the window hangs on it.

#include "AgentActivityView.h"
#include "AgentTrafficWindow.h"

#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QLineF>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPixmap>
#include <QSet>
#include <QTreeWidget>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace forkmesh::agentvis;
using forkmesh::sonify::Motion;

static int failures = 0;

static void check(bool ok, const QString &what)
{
    if (ok)
        return;
    std::cerr << "FAIL: " << qPrintable(what) << "\n";
    ++failures;
}

static QJsonObject toolUse(const char *name)
{
    return QJsonDocument::fromJson(
               QByteArray(R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":")")
                   .append(name)
                   .append(R"(","input":{"a":"b"}}]}})"))
        .object();
}

static void click(QWidget *widget, QEvent::Type type, const QPointF &at)
{
    QMouseEvent event(type, at, widget->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

static int distinctColours(const QImage &image)
{
    QSet<QRgb> colours;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4)
            colours.insert(image.pixel(x, y));
    }
    return int(colours.size());
}

static QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

// Working only: resting agents fade out of the picture and out of reach;
// the list of sounds comes and goes with the width, and lights up on a note.
static void testWorkingOnlyAndSounds()
{
    Scene scene;
    scene.addAgent(1, QStringLiteral("working"));
    scene.addAgent(2, QStringLiteral("resting"));
    scene.setState(1, State::Running);
    scene.setState(2, State::Done);
    scene.ingest(1, QStringLiteral("working"),
                 parse(R"({"type":"assistant","message":{"content":[{"type":"tool_use",)"
                       R"("name":"Edit","input":{"file_path":"/src/a.cpp"}}]}})"));

    AgentActivityView view(&scene);
    view.resize(1100, 700);
    QImage image(view.size(), QImage::Format_ARGB32);
    view.render(&image);
    check(!view.soundListRect().isEmpty(), QStringLiteral("a wide window has the list of sounds"));
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion == Motion::Rest)
            continue;
        check(view.soundListRect().contains(view.soundRowRect(motion)),
              QStringLiteral("every kind of sound has a row: %1")
                  .arg(QLatin1String(styleFor(motion).label)));
    }
    check(!view.soundListRect().intersects(QRectF(view.bodyPosition(1), QSizeF(1, 1))),
          QStringLiteral("the list does not cover the field"));

    // A heard note lights its row; the rest stay dark.
    scene.advanceTo(scene.now() + 3.0);
    scene.note(1, Motion::Write, 0.0, 0.25);
    check(view.soundGlow(Motion::Write) > 0.9, QStringLiteral("a note that sounds lights its row"));
    check(view.soundGlow(Motion::Search) < 0.01, QStringLiteral("a kind that did not stays dark"));
    scene.advanceTo(scene.now() + 5.0);
    check(view.soundGlow(Motion::Write) < 0.05, QStringLiteral("and the glow fades"));

    // The list reads most frequent first: going down it, no row has landed
    // more than the one above.
    int landedKinds = 0;
    double previousTop = -1;
    int previousCount = std::numeric_limits<int>::max();
    QVector<Motion> byRow;
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion != Motion::Rest)
            byRow.append(motion);
    }
    std::sort(byRow.begin(), byRow.end(), [&](Motion a, Motion b) {
        return view.soundRowRect(a).top() < view.soundRowRect(b).top();
    });
    bool ordered = true;
    for (const Motion motion : byRow) {
        const int count = scene.landCount(motion);
        landedKinds += count > 0 ? 1 : 0;
        ordered = ordered && count <= previousCount && view.soundRowRect(motion).top() > previousTop;
        previousCount = count;
        previousTop = view.soundRowRect(motion).top();
    }
    check(landedKinds > 0, QStringLiteral("something has landed to sort by"));
    check(ordered, QStringLiteral("the list of sounds has the most frequent kind on top"));
    check(scene.landCount(byRow.first()) > 0,
          QStringLiteral("a kind that has landed sits above one that has not"));

    // Every row has two switches, its figure and its sound, and a click on
    // either flips it and tells the owner.
    Motion toggledMotion = Motion::Rest;
    bool toggledSound = false;
    bool toggledOn = true;
    view.setOnToggleKind([&](Motion motion, bool sound, bool on) {
        toggledMotion = motion;
        toggledSound = sound;
        toggledOn = on;
    });
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion == Motion::Rest)
            continue;
        const QRectF eye = view.kindSwitchRect(motion, false);
        const QRectF speaker = view.kindSwitchRect(motion, true);
        check(!eye.isEmpty() && !speaker.isEmpty() && !eye.intersects(speaker)
                  && view.soundRowRect(motion).contains(eye)
                  && view.soundRowRect(motion).contains(speaker),
              QStringLiteral("%1 has a figure switch and a sound switch on its row")
                  .arg(QLatin1String(styleFor(motion).label)));
    }
    check(view.motionShown(Motion::Compose) && view.motionHeard(Motion::Compose),
          QStringLiteral("every kind starts shown and heard"));
    click(&view, QEvent::MouseButtonPress, view.kindSwitchRect(Motion::Compose, false).center());
    check(!view.motionShown(Motion::Compose) && toggledMotion == Motion::Compose && !toggledSound
              && !toggledOn,
          QStringLiteral("clicking the eye hides that kind's figures"));
    click(&view, QEvent::MouseButtonPress, view.kindSwitchRect(Motion::Usage, true).center());
    check(!view.motionHeard(Motion::Usage) && view.motionShown(Motion::Usage)
              && toggledMotion == Motion::Usage && toggledSound && !toggledOn,
          QStringLiteral("clicking the speaker silences that kind, and only that"));
    click(&view, QEvent::MouseMove, view.kindSwitchRect(Motion::Usage, true).center());
    check(!view.hoverLines().isEmpty()
              && view.hoverLines().first().startsWith(QStringLiteral("Usage sound: off")),
          QStringLiteral("the switch under the cursor says what it does"));
    click(&view, QEvent::MouseButtonDblClick, view.kindSwitchRect(Motion::Compose, false).center());
    check(view.motionShown(Motion::Compose), QStringLiteral("a quick second click flips it back"));
    view.setMotionHeard(Motion::Usage, true);
    view.render(&image);

    // Painted: the body is where the hit test says it is, drift included.
    const QPointF at = view.bodyPosition(1);
    check(view.sessionAt(at) == 1, QStringLiteral("the body is hit where it is drawn"));

    view.setWorkingOnly(true);
    // Let the fade run: presence eases on the scene's clock.
    for (int i = 0; i < 40; ++i) {
        scene.advanceTo(scene.now() + 0.05);
        view.render(&image);
    }
    check(view.presence(1) > 0.99, QStringLiteral("the working agent stays"));
    check(view.presence(2) < 0.01, QStringLiteral("the resting one fades out"));
    check(view.sessionAt(view.bodyPosition(2)) != 2,
          QStringLiteral("and cannot be clicked once gone"));

    view.setShowSounds(false);
    check(view.soundListRect().isEmpty(), QStringLiteral("the list can be switched off"));

    view.resize(500, 400);
    view.setShowSounds(true);
    check(view.soundListRect().isEmpty(), QStringLiteral("a narrow window keeps the field whole"));

}

// Logs: the column on the left comes with the button, the list of sounds
// gives way to it on a window too narrow for both, and a click on it asks for
// the page.
static void testLogs()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(7, QStringLiteral("seven"));
    scene.log(QStringLiteral("ERROR"), QStringLiteral("#f85149"),
              QStringLiteral("agent 7 failed to start"));
    scene.log(QStringLiteral("CLOUD"), QStringLiteral("#58a6ff"),
              QStringLiteral("GET /api/status 200"));

    AgentActivityView view(&scene);
    view.resize(1300, 700);
    QImage image(view.size(), QImage::Format_ARGB32);
    view.render(&image);
    check(view.logRect().isEmpty(), QStringLiteral("the log is off until asked for"));
    const double fieldLeftBefore = view.bodyPosition(7).x();

    view.setShowLogs(true);
    view.render(&image);
    check(!view.logRect().isEmpty() && view.logRect().left() < 20,
          QStringLiteral("the log takes a column on the left"));
    check(!view.soundListRect().isEmpty(),
          QStringLiteral("a wide window keeps the list of sounds too"));
    check(view.bodyPosition(7).x() > view.logRect().right(),
          QStringLiteral("the field moves over for it"));
    check(view.bodyPosition(7).x() != fieldLeftBefore,
          QStringLiteral("the body is re-seated in the narrower field"));
    check(view.headline().contains(QStringLiteral("log line")),
          QStringLiteral("the headline counts the log"));

    int opened = 0;
    view.setOnOpenLogs([&opened] { ++opened; });
    click(&view, QEvent::MouseButtonPress, view.logRect().center());
    check(opened == 1, QStringLiteral("clicking the column opens the Logs page"));

    check(!view.hideOutput(), QStringLiteral("output shows in the log until hidden"));
    scene.log(QStringLiteral("CLOUD"), QStringLiteral("#58a6ff"),
              QStringLiteral("Node n1 core forkmesh-app info: slow request 259ms"));
    view.render(&image);
    const QImage withOutput = image.copy(view.logRect().toRect());
    view.setHideOutput(true);
    view.render(&image);
    check(view.hideOutput() && image.copy(view.logRect().toRect()) != withOutput,
          QStringLiteral("hiding output redraws the column without it"));
    view.setHideOutput(false);
    view.render(&image);
    check(image.copy(view.logRect().toRect()) == withOutput,
          QStringLiteral("and showing it again puts it back"));

    view.resize(900, 600);
    view.render(&image);
    check(!view.logRect().isEmpty() && view.soundListRect().isEmpty(),
          QStringLiteral("too narrow for both, the log stays and the sounds give way"));

    view.resize(500, 400);
    view.render(&image);
    check(view.logRect().isEmpty(), QStringLiteral("a narrow window keeps the field whole"));
}

static void testTasksNetworkAndStillness()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(7, QStringLiteral("seven"));
    scene.setState(7, State::Waiting);
    scene.advanceTo(40.0);

    AgentActivityView view(&scene);
    view.resize(900, 600);
    view.setBackdrop(AgentActivityView::Backdrop::Space);
    QImage first(view.size(), QImage::Format_ARGB32);
    QImage second(view.size(), QImage::Format_ARGB32);
    view.render(&first);
    scene.advanceTo(55.0);
    view.render(&second);
    check(!scene.moving() && first == second,
          QStringLiteral("a quiet field paints the same frame fifteen seconds later"));

    scene.taskStarted(1, 7, QStringLiteral("git status"), QStringLiteral("git status --porcelain"));
    scene.taskStarted(2, Scene::kDesktop, QStringLiteral("fetch"));
    view.render(&first);
    const QPointF agentTask = view.taskPosition(scene.tasks().at(0));
    const QPointF desktopTask = view.taskPosition(scene.tasks().at(1));
    check(view.rect().contains(agentTask.toPoint()) && view.rect().contains(desktopTask.toPoint()),
          QStringLiteral("background tasks are drawn in the window"));
    check(QLineF(agentTask, view.bodyPosition(7)).length() > view.bodyRadius(7),
          QStringLiteral("an agent's task orbits outside its body"));
    check(desktopTask.y() > view.height() * 0.75
              && std::abs(desktopTask.x() - view.width() / 2.0) < view.width() * 0.3,
          QStringLiteral("the desktop's own task circles its node at the foot of the field"));
    check(view.headline().contains(QStringLiteral("2 background tasks")),
          QStringLiteral("the headline counts background work"));
    scene.setPrompt(7, QStringLiteral("make the activity window show the prompt on hover "
                                      "so we can see what each agent was asked to do"));
    click(&view, QEvent::MouseMove, view.bodyPosition(7));
    const QStringList agentLines = view.hoverLines();
    check(agentLines.join(QLatin1Char('\n')).contains(QStringLiteral("prompt: make the activity"))
              && agentLines.join(QLatin1Char(' ')).contains(QStringLiteral("each agent")),
          QStringLiteral("hovering an agent shows its prompt, wrapped"));
    click(&view, QEvent::MouseMove, agentTask);
    check(!view.hoverLines().isEmpty()
              && view.hoverLines().first() == QStringLiteral("background: git status"),
          QStringLiteral("a task under the cursor says what it is"));

    scene.network(Scene::kDesktop, QStringLiteral("example.com"), 4000, 90000);
    check(view.headline().contains(QStringLiteral("network ↓")),
          QStringLiteral("the headline carries network in and out"));
    view.render(&second);
    check(first != second, QStringLiteral("work and traffic change the picture"));
}

static void testProcessesAndAgentTasks()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(7, QStringLiteral("seven"));
    scene.advanceTo(40.0);
    AgentActivityView view(&scene);
    view.resize(900, 600);
    QImage before(view.size(), QImage::Format_ARGB32);
    QImage after(view.size(), QImage::Format_ARGB32);
    view.render(&before);

    scene.sampleProcesses(7,
                          {{11, 10, QStringLiteral("bash"), QStringLiteral("bash -c make"), 0},
                           {12, 11, QStringLiteral("make"), QStringLiteral("make -j8"), 0}},
                          10);
    scene.advanceTo(42.0);
    scene.sampleProcesses(7,
                          {{11, 10, QStringLiteral("bash"), QStringLiteral("bash -c make"), 0},
                           {12, 11, QStringLiteral("make"), QStringLiteral("make -j8"), 150}},
                          10);
    const Process *make = scene.process(7, 12);
    const Process *bash = scene.process(7, 11);
    const QPointF makeAt = view.processPosition(*make);
    const QPointF bashAt = view.processPosition(*bash);
    check(view.rect().contains(makeAt.toPoint()), QStringLiteral("a process is drawn in the window"));
    check(QLineF(makeAt, view.bodyPosition(7)).length() > QLineF(bashAt, view.bodyPosition(7)).length(),
          QStringLiteral("a child hangs further out than its parent"));
    check(view.headline().contains(QStringLiteral("2 processes (1 busy)")),
          QStringLiteral("the headline counts processes and the busy ones"));
    view.render(&after);
    check(before != after, QStringLiteral("the process tree changes the picture"));
    click(&view, QEvent::MouseMove, makeAt);
    const QString card = view.hoverLines().join(QLatin1Char('\n'));
    check(card.startsWith(QStringLiteral("process: make (pid 12)"))
              && card.contains(QStringLiteral("make -j8"))
              && card.contains(QStringLiteral("busy, 75% of a core"))
              && card.contains(QStringLiteral("started by bash (pid 11)")),
          QStringLiteral("a process under the cursor says what it is and who started it"));

    view.setShowProcesses(false);
    check(!view.headline().contains(QStringLiteral("processes")),
          QStringLiteral("switched off, the processes go"));

    scene.ingest(7, QString(), QJsonDocument::fromJson(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","id":"tu1","name":"Agent","input":{"description":"look around"}}]}})").object());
    const QPointF task = view.taskPosition(scene.tasks().last());
    click(&view, QEvent::MouseMove, task);
    check(!view.hoverLines().isEmpty() && view.hoverLines().first() == QStringLiteral("subagent")
              && view.hoverLines().value(1) == QStringLiteral("look around"),
          QStringLiteral("a subagent under the cursor says what it was sent to do"));
}

// An agent with a face wears it as its body, its number tagged on top; one
// without keeps the plain disc, and clearing the face puts the disc back.
static void testFaces()
{
    Scene scene;
    scene.addAgent(7, QStringLiteral("faced"));
    scene.setState(7, State::Running);
    AgentActivityView view(&scene);
    view.resize(900, 600);

    const auto centreColour = [&view] {
        QImage image(view.size(), QImage::Format_ARGB32);
        image.fill(Qt::black);
        view.render(&image);
        return QColor(image.pixel(view.bodyPosition(7).toPoint()));
    };
    const QColor plain = centreColour();

    QPixmap green(64, 64);
    green.fill(QColor(0, 255, 0));
    view.setFace(7, QStringLiteral("test|green"), QIcon(green));
    check(view.hasFace(7), QStringLiteral("the face is kept"));
    const QColor faced = centreColour();
    check(faced.green() > 200 && faced.red() < 60 && faced.blue() < 60,
          QStringLiteral("the body is drawn as its face"));

    // The tag sits on top of the body, straddling its top edge.
    QImage tagged(view.size(), QImage::Format_ARGB32);
    tagged.fill(Qt::black);
    view.render(&tagged);
    const QPointF top = view.bodyPosition(7) - QPointF(0, view.bodyRadius(7) + 3);
    check(QColor(tagged.pixel(top.toPoint())) != QColor(Qt::black),
          QStringLiteral("the agent's number is tagged on top of it"));

    view.setFace(7, QString(), QIcon());
    check(!view.hasFace(7) && centreColour() == plain,
          QStringLiteral("clearing the face puts the plain disc back"));
}

// The cursor's card ends with a key: every kind of mark the agent under it
// has made, with how many; a row of the list of sounds says what its kind is
// and how much of it there has been.
static void testHostPlaces()
{
    using Place = AgentActivityView::HostPlace;
    const struct {
        const char *name;
        bool mirror;
        Place place;
    } cases[] = {
        {"this machine", false, Place::Machine},
        {"fmnext-core-1", true, Place::Mirror},
        {"app.forkmesh.com", false, Place::Mesh},
        {"api.anthropic.com", false, Place::Ai},
        {"generativelanguage.googleapis.com", false, Place::Ai},
        {"github.com", false, Place::Git},
        {"objects.githubusercontent.com", false, Place::Git},
        {"registry.npmjs.org", false, Place::Package},
        {"files.pythonhosted.org", false, Place::Package},
        {"storage.googleapis.com", false, Place::Cloud},
        {"s3.amazonaws.com", false, Place::Cloud},
        {"example.org", false, Place::Web},
        {"203.0.113.7", false, Place::Web},
    };
    for (const auto &item : cases)
        check(AgentActivityView::hostPlace(QString::fromLatin1(item.name), item.mirror) == item.place,
              QStringLiteral("%1 is drawn as %2")
                  .arg(QLatin1String(item.name),
                       AgentActivityView::hostPlaceLabel(item.place)));
}

static void testHoverKey()
{
    Scene scene;
    scene.advanceTo(1.0);
    scene.addAgent(9, QStringLiteral("#9 tidy the docs"));
    scene.setState(9, State::Running);
    scene.note(9, Motion::Read, 0.0, 0.3);
    scene.advanceTo(1.5);
    scene.note(9, Motion::Read, 0.0, 0.3);
    scene.advanceTo(2.0);
    scene.note(9, Motion::Write, 0.0, 0.3);
    scene.advanceTo(12.0);

    AgentActivityView view(&scene);
    view.resize(1100, 700);
    view.show();
    QApplication::processEvents();
    check(view.hoverKinds().isEmpty(), QStringLiteral("no key with nothing under the cursor"));
    click(&view, QEvent::MouseMove, view.bodyPosition(9));
    const auto kinds = view.hoverKinds();
    check(kinds.size() == 2 && kinds.value(0).first == Motion::Read && kinds.value(0).second == 2
              && kinds.value(1).first == Motion::Write && kinds.value(1).second == 1,
          QStringLiteral("the key counts the agent's marks by kind, most first"));
    check(view.hoverLines().value(0) == QStringLiteral("#9 tidy the docs"),
          QStringLiteral("the card names the agent once"));
    scene.setTask(9, QStringLiteral("sort the index"));
    check(view.hoverLines().value(0) == QStringLiteral("#9 tidy the docs · sort the index"),
          QStringLiteral("the card names the project and the task"));
    scene.setSaid(9, QStringLiteral("I **sorted** the index."));
    check(view.hoverLines().join(QLatin1Char('\n')).contains(
              QStringLiteral("said: I sorted the index.")),
          QStringLiteral("the card has what it last said, plain"));
    const QString card = view.hoverLines().join(QLatin1Char('\n'));
    check(card.contains(QStringLiteral("size: tokens burned"))
              && card.contains(QStringLiteral("outer ring: running")),
          QStringLiteral("the card says what the body's picture means"));
    check(card.contains(QStringLiteral("icons round it")),
          QStringLiteral("the card says the marks round it are the list's icons"));
    QImage image(view.size(), QImage::Format_ARGB32);
    view.render(&image);

    const QRectF row = view.soundRowRect(Motion::Read);
    click(&view, QEvent::MouseMove, QPointF(row.left() + row.width() * 0.3, row.center().y()));
    const QStringList rowLines = view.hoverLines();
    check(rowLines.value(0) == QString::fromLatin1(styleFor(Motion::Read).label)
              && rowLines.join(QLatin1Char('\n')).contains(QStringLiteral("2 seen")),
          QStringLiteral("a row of the list says what it stands for and how many"));
    check(view.hoverKinds().isEmpty(), QStringLiteral("the list itself has no key"));

    check(!scene.rows().isEmpty() && scene.rows().first().band == Band::Working,
          QStringLiteral("the field names the working row"));
}

static void testMirrors()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(7, QStringLiteral("seven"));
    scene.setMirrors({{QStringLiteral("mirror-a"), {QStringLiteral("203.0.113.5")}}});
    AgentActivityView view(&scene);
    view.resize(900, 600);
    QImage idle(view.size(), QImage::Format_ARGB32);
    QImage busy(view.size(), QImage::Format_ARGB32);
    view.render(&idle);
    const QPointF mirror = view.hostPosition(QStringLiteral("mirror-a"));
    check(view.rect().contains(mirror.toPoint()),
          QStringLiteral("a mirror is on the field before anyone talks to it"));
    for (const int outside : {-1000, -1001, -1000000}) {
        scene.addAgent(outside, QStringLiteral("external"));
        check(!scene.mirrorForSource(outside) && view.flowEnd(outside) != mirror,
              QStringLiteral("an external or cloud agent's marks land on its own body, "
                             "not on a mirror's seat (%1)").arg(outside));
        scene.removeAgent(outside);
    }
    click(&view, QEvent::MouseMove, mirror);
    QString card = view.hoverLines().join(QLatin1Char('\n'));
    check(view.hoverLines().value(0) == QStringLiteral("mirror-a")
              && card.contains(QStringLiteral("one of your mirror nodes"))
              && card.contains(QStringLiteral("nobody is talking to it")),
          QStringLiteral("hovering a quiet mirror says what it is"));
    check(view.headline().contains(QStringLiteral("1 mirror (0 in use)")),
          QStringLiteral("the headline counts the mirrors and who is using them"));
    scene.network(7, QStringLiteral("203.0.113.5"), 4000, 90000, QStringLiteral("203.0.113.5"), 22);
    check(view.headline().contains(QStringLiteral("1 mirror (1 in use)")),
          QStringLiteral("and counts one in use as soon as traffic crosses to it"));
    view.render(&busy);
    check(idle != busy, QStringLiteral("an agent reaching a mirror changes the picture"));
    click(&view, QEvent::MouseMove, QPointF(1, 1));
    click(&view, QEvent::MouseMove, view.hostPosition(QStringLiteral("mirror-a")));
    card = view.hoverLines().join(QLatin1Char('\n'));
    check(card.contains(QStringLiteral("talking: #7")),
          QStringLiteral("and the mirror's card names the agent talking to it"));

    // The mirror's own work: the journal it streams into the desktop's log
    // marks off its seat, not off the desktop's node at the foot.
    QImage quiet(view.size(), QImage::Format_ARGB32);
    QImage working(view.size(), QImage::Format_ARGB32);
    view.render(&quiet);
    scene.log(QStringLiteral("MIRROR"), QStringLiteral("#39c5cf"),
              QStringLiteral("Mirror mirror-a mirror: forkmesh-mirror-node synced "
                             "forkmesh-web"));
    const int source = scene.mirrorSource(QStringLiteral("mirror-a"));
    check(view.flowEnd(source) == view.hostPosition(QStringLiteral("mirror-a")),
          QStringLiteral("a mirror's marks are anchored on its seat"));
    view.render(&working);
    check(quiet != working, QStringLiteral("a mirror's own log line shows on the field"));
    click(&view, QEvent::MouseMove, QPointF(1, 1));
    click(&view, QEvent::MouseMove, view.hostPosition(QStringLiteral("mirror-a")));
    card = view.hoverLines().join(QLatin1Char('\n'));
    check(card.contains(QStringLiteral("its log: 1 line a minute"))
              && card.contains(QStringLiteral("synced forkmesh-web")),
          QStringLiteral("and its card says what it has been doing"));
    // Its mark lands a beat after the line, as an agent's does.
    scene.advanceTo(scene.now() + 3.0);
    view.render(&working);
    click(&view, QEvent::MouseMove, QPointF(1, 1));
    click(&view, QEvent::MouseMove, view.hostPosition(QStringLiteral("mirror-a")));
    bool keyed = false;
    for (const auto &[motion, count] : view.hoverKinds())
        keyed = keyed || count > 0;
    check(keyed, QStringLiteral("the key under the cursor counts the mirror's own marks"));
}

static int visibleRows(const QTreeWidget *list)
{
    int shown = 0;
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        shown += list->topLevelItem(i)->isHidden() ? 0 : 1;
    return shown;
}

static void testConnectionClicks()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(7, QStringLiteral("seven"));
    const QString host = QStringLiteral("api.example.com");
    const QString address = QStringLiteral("93.184.216.34");
    scene.network(7, host, 4000, 90000, address, 443, 4242, QStringLiteral("curl"));
    AgentActivityView view(&scene);
    view.resize(900, 600);
    view.grab();
    scene.advanceTo(11.0);
    view.grab();

    int clickedSession = 0;
    QString clickedHost;
    view.setOnConnectionClicked([&](int sessionId, const QString &name) {
        clickedSession = sessionId;
        clickedHost = name;
    });
    const QPointF seat = view.hostPosition(host);
    check(view.hostAt(seat) == host && view.hostAt(QPointF(1, 1)).isEmpty(),
          QStringLiteral("a host along the top is found under the cursor"));
    click(&view, QEvent::MouseMove, seat);
    check(view.hoverLines().contains(QStringLiteral("click to watch its traffic live")),
          QStringLiteral("a host's card says a click shows its traffic"));
    click(&view, QEvent::MouseButtonPress, seat);
    check(clickedSession == Scene::kAnySession && clickedHost == host,
          QStringLiteral("clicking a host asks for all of its traffic"));

    const QPointF from = view.flowEnd(7);
    const QPointF span = seat - from;
    const double length = std::hypot(span.x(), span.y());
    const QPointF normal(-span.y() / length, span.x() / length);
    const QPointF middle = (from + seat) / 2.0 + normal * (length * 0.06);
    const AgentActivityView::FlowHit hit = view.flowAt(middle);
    check(hit.sessionId == 7 && hit.host == host,
          QStringLiteral("the line between an agent and a host is found under the cursor"));
    check(view.flowAt(QPointF(1, view.height() - 1)).host.isEmpty(),
          QStringLiteral("and bare field is no connection"));
    click(&view, QEvent::MouseMove, middle);
    check(view.hoverLines().value(0) == QStringLiteral("#7 → api.example.com")
              && view.hoverLines().contains(QStringLiteral("click to watch its traffic live")),
          QStringLiteral("hovering a connection names both ends"));
    clickedSession = 0;
    click(&view, QEvent::MouseButtonPress, middle);
    check(clickedSession == 7 && clickedHost == host,
          QStringLiteral("clicking the line asks for that agent's traffic to that host"));
    view.setShowNetwork(false);
    check(view.hostAt(seat).isEmpty() && view.flowAt(middle).host.isEmpty(),
          QStringLiteral("with the network hidden there is nothing to click"));
    view.setShowNetwork(true);

    AgentTrafficWindow window(&scene, 7, host);
    check(window.heading() == QStringLiteral("#7 seven → api.example.com"),
          QStringLiteral("the traffic list is headed by both ends"));
    QTreeWidget *connections = window.connectionList();
    QTreeWidget *live = window.trafficList();
    check(connections->topLevelItemCount() == 1
              && connections->topLevelItem(0)->text(AgentTrafficWindow::ConnectionProcess)
                     == QStringLiteral("curl (4242)")
              && connections->topLevelItem(0)->text(AgentTrafficWindow::ConnectionPort)
                     == QStringLiteral("443 https"),
          QStringLiteral("a connection row names its process and service"));
    check(live->topLevelItemCount() == 2,
          QStringLiteral("what moved before the list opened is already in it, each way"));

    scene.advanceTo(12.0);
    scene.network(7, host, 700, 0, address, 443, 4242, QStringLiteral("curl"));
    window.refresh();
    check(live->topLevelItemCount() == 3
              && live->topLevelItem(0)->text(AgentTrafficWindow::TrafficDirection)
                     == QStringLiteral("↑ out")
              && live->topLevelItem(0)->text(AgentTrafficWindow::TrafficBytes)
                     == QStringLiteral("700 B"),
          QStringLiteral("new traffic flows in at the top"));

    window.setPaused(true);
    scene.network(7, host, 10, 10, address, 443, 4242, QStringLiteral("curl"));
    window.refresh();
    check(live->topLevelItemCount() == 3, QStringLiteral("a paused list holds still"));
    window.setPaused(false);
    check(live->topLevelItemCount() == 5, QStringLiteral("and catches up when resumed"));

    scene.network(7, host, 20, 0, address, 22, 5151, QStringLiteral("ssh"));
    scene.network(8, host, 30, 0, address, 443, 6161, QStringLiteral("node"));
    window.refresh();
    check(connections->topLevelItemCount() == 2 && live->topLevelItemCount() == 6,
          QStringLiteral("another agent's traffic stays out of this agent's list"));
    window.setFilter(QStringLiteral("ssh"));
    check(visibleRows(live) == 1 && visibleRows(connections) == 1,
          QStringLiteral("the filter narrows both lists"));
    window.setFilter(QString());
    for (int i = 0; i < connections->topLevelItemCount(); ++i) {
        if (connections->topLevelItem(i)->text(AgentTrafficWindow::ConnectionProcess)
            == QStringLiteral("curl (4242)"))
            connections->topLevelItem(i)->setSelected(true);
    }
    check(visibleRows(live) == 5, QStringLiteral("selecting a connection shows only its traffic"));
    connections->clearSelection();
    check(visibleRows(live) == 6, QStringLiteral("and clearing it shows everything again"));

    scene.renameHost(host, QStringLiteral("named.example.com"));
    window.refresh();
    check(window.host() == QStringLiteral("named.example.com")
              && window.heading().endsWith(QStringLiteral("named.example.com")),
          QStringLiteral("the list follows its host through a lookup"));
    window.clearTraffic();
    check(live->topLevelItemCount() == 0, QStringLiteral("clear empties the live list"));
    scene.network(7, QStringLiteral("named.example.com"), 1, 0, address, 443, 4242,
                  QStringLiteral("curl"));
    window.refresh();
    check(live->topLevelItemCount() == 1, QStringLiteral("and traffic keeps coming after it"));

    AgentTrafficWindow everyone(&scene, Scene::kAnySession, QStringLiteral("named.example.com"));
    check(everyone.heading() == QStringLiteral("named.example.com")
              && everyone.connectionList()->topLevelItemCount() == 3,
          QStringLiteral("a host's list holds every agent talking to it"));
    if (qEnvironmentVariableIsSet("FORKMESH_SAVE_TRAFFIC"))
        everyone.grab().save(qEnvironmentVariable("FORKMESH_SAVE_TRAFFIC"));
}

static void testRelayClients()
{
    Scene scene;
    scene.advanceTo(10.0);
    AgentActivityView view(&scene);
    view.resize(900, 600);
    QImage empty(view.size(), QImage::Format_ARGB32);
    QImage full(view.size(), QImage::Format_ARGB32);
    view.render(&empty);
    scene.setClients({{QStringLiteral("v1"), QStringLiteral("Guest 7"), QString(),
                       QStringLiteral("guest"), QString(), QString()},
                      {QStringLiteral("n1"), QStringLiteral("frank"), QStringLiteral("linux"),
                       QStringLiteral("node"), QStringLiteral("frank-box"),
                       QStringLiteral("1.2.3")}},
                     /*announce=*/false);
    view.render(&full);
    check(empty != full, QStringLiteral("the relay's clients are drawn"));
    check(view.headline().contains(QStringLiteral("1 web visitor"))
              && view.headline().contains(QStringLiteral("1 other client")),
          QStringLiteral("the headline counts web visitors and other clients"));

    const QPointF guest = view.clientPosition(QStringLiteral("v1"));
    const QPointF frank = view.clientPosition(QStringLiteral("n1"));
    const QPointF node = view.flowEnd(Scene::kDesktop);
    check(!guest.isNull() && !frank.isNull() && frank.x() < node.x() && guest.x() < frank.x(),
          QStringLiteral("the row runs left from the desktop, desktops nearest"));

    scene.socketFrame(false, QStringLiteral("chat"), QStringLiteral("v1"), 300);
    click(&view, QEvent::MouseMove, guest);
    QString card = view.hoverLines().join(QLatin1Char('\n'));
    check(view.hoverLines().value(0) == QStringLiteral("Guest 7")
              && card.contains(QStringLiteral("web visitor"))
              && card.contains(QStringLiteral("↓ 1 from it"))
              && card.contains(QStringLiteral("latest frame: chat")),
          QStringLiteral("a visitor's card says who it is and what it sent"));

    RelaySocket socket;
    socket.connected = true;
    socket.endpoint = QStringLiteral("wss://relay.example/room");
    socket.framesIn = 40;
    socket.framesOut = 12;
    scene.setRelaySocket(socket);
    click(&view, QEvent::MouseMove, QPointF(1, 1));
    click(&view, QEvent::MouseMove, node);
    card = view.hoverLines().join(QLatin1Char('\n'));
    check(card.contains(QStringLiteral("relay websocket: connected to wss://relay.example/room"))
              && card.contains(QStringLiteral("frames ↓ 40"))
              && card.contains(QStringLiteral("1 web visitor · 1 desktop node · 0 phones")),
          QStringLiteral("the desktop's card describes the relay socket and the room"));

    scene.webRequest(QStringLiteral("198.51.100.4"), QStringLiteral("Firefox in Oslo, NO"),
                     QStringLiteral("198.51.100.4 · AS2119 Telenor"),
                     QStringLiteral("GET /docs 200"));
    check(view.headline().contains(QStringLiteral("2 web visitors")),
          QStringLiteral("a visitor from the Worker's log is counted with the rest"));
    click(&view, QEvent::MouseMove, QPointF(1, 1));
    click(&view, QEvent::MouseMove, view.clientPosition(QStringLiteral("web:198.51.100.4")));
    card = view.hoverLines().join(QLatin1Char('\n'));
    check(view.hoverLines().value(0) == QStringLiteral("Firefox in Oslo, NO")
              && card.contains(QStringLiteral("from the Worker's live log"))
              && card.contains(QStringLiteral("AS2119 Telenor"))
              && card.contains(QStringLiteral("1 request · 1 in the last minute"))
              && card.contains(QStringLiteral("latest: GET /docs 200")),
          QStringLiteral("a site visitor's card says where it is and what it asked for"));

    // Everything the Worker's log said: its country, its user agent, and
    // what it has been asking for.
    Scene::WebHit probe;
    probe.visitor = QStringLiteral("203.0.113.9");
    probe.name = QStringLiteral("curl in Frankfurt, DE");
    probe.request = QStringLiteral("GET /.env 404");
    probe.path = QStringLiteral("/.env");
    probe.status = 404;
    probe.country = QStringLiteral("DE");
    probe.address = QStringLiteral("203.0.113.9");
    probe.asn = QStringLiteral("24940");
    probe.asOrganization = QStringLiteral("Hetzner");
    probe.userAgent = QStringLiteral("curl/8.5.0");
    scene.webRequest(probe);
    const QPointF prober = view.clientPosition(QStringLiteral("web:203.0.113.9"));
    check(view.clientAt(prober) == QStringLiteral("web:203.0.113.9"),
          QStringLiteral("a visitor is found under the cursor"));
    click(&view, QEvent::MouseMove, QPointF(1, 1));
    click(&view, QEvent::MouseMove, prober);
    card = view.hoverLines().join(QLatin1Char('\n'));
    check(card.contains(QStringLiteral("a script (curl)"))
              && card.contains(QStringLiteral("from: Germany (DE)"))
              && card.contains(QStringLiteral("agent: curl/8.5.0"))
              && card.contains(QStringLiteral("⚠ watch:"))
              && card.contains(QStringLiteral("/.env"))
              && card.contains(QStringLiteral("answered: 4xx 1"))
              && card.contains(QStringLiteral("click to block it - best: the address 203.0.113.9")),
          QStringLiteral("its card shows its country, its agent, why it is flagged and the "
                         "best way to block it"));
    QImage flagged(view.size(), QImage::Format_ARGB32);
    view.render(&flagged);
    check(view.headline().contains(QStringLiteral("⚠ watch")),
          QStringLiteral("the headline warns of a flagged visitor"));
    QString clicked;
    view.setOnClientClicked([&clicked](const QString &id, const QPoint &) { clicked = id; });
    click(&view, QEvent::MouseButtonPress, prober);
    check(clicked == QStringLiteral("web:203.0.113.9"),
          QStringLiteral("clicking a visitor asks to block it"));
    click(&view, QEvent::MouseMove, QPointF(1, 1));
    click(&view, QEvent::MouseMove, node);
    card = view.hoverLines().join(QLatin1Char('\n'));
    check(card.contains(QStringLiteral("visitors from: Germany 1")),
          QStringLiteral("the desktop's card says where the site's visitors are"));

    // The key explains the beads and the signs.
    check(!view.soundListRect().isEmpty(), QStringLiteral("the list of sounds is drawn"));
    {
        click(&view, QEvent::MouseMove, QPointF(1, 1));
        click(&view, QEvent::MouseMove, view.soundRowRect(Motion::Socket).center());
        card = view.hoverLines().join(QLatin1Char('\n'));
        check(card.contains(QStringLiteral("warm (orange)")) && card.contains(QStringLiteral("cool (teal)"))
                  && card.contains(QStringLiteral("whole room")),
              QStringLiteral("the key says what the beads to the visitors are"));
        click(&view, QEvent::MouseMove, QPointF(1, 1));
        click(&view, QEvent::MouseMove, view.soundRowRect(Motion::Visitor).center());
        card = view.hoverLines().join(QLatin1Char('\n'));
        check(card.contains(QStringLiteral("flag")) && card.contains(QStringLiteral("warning sign")),
              QStringLiteral("and what a visitor's flag and warning sign mean"));
    }

    view.setShowNetwork(false);
    check(!view.headline().contains(QStringLiteral("web visitor")),
          QStringLiteral("the clients belong to the network layer"));
}

static void testGpuFallbackAndFrameRate()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(3, QStringLiteral("three"));
    scene.setState(3, State::Waiting);
    scene.advanceTo(40.0);

    AgentActivityView view(&scene);
    view.resize(900, 600);
    // The offscreen platform has no OpenGL: asking for the GPU leaves the
    // view painting itself, at the software frame rate.
    view.setGpuRendering(true);
    check(!AgentActivityView::gpuAvailable() && !view.gpuRendering(),
          QStringLiteral("with no OpenGL the field falls back to software"));
    check(view.busyIntervalMs() == 33,
          QStringLiteral("software painting runs at thirty frames a second"));

    QImage plain(view.size(), QImage::Format_ARGB32);
    QImage readout(view.size(), QImage::Format_ARGB32);
    view.render(&plain);
    check(!view.showFrameRate(), QStringLiteral("the frame rate is off until asked for"));
    view.setShowFrameRate(true);
    view.render(&readout);
    check(plain != readout, QStringLiteral("the frame rate is drawn once asked for"));
    check(view.frameRateText() == QStringLiteral("CPU · idle"),
          QStringLiteral("a still field reads idle, not the rate it last moved at"));

    scene.ingest(3, QStringLiteral("three"), toolUse("Bash"));
    for (int frame = 0; frame < 5; ++frame)
        view.render(&readout);
    check(view.framesPerSecond() >= 5,
          QStringLiteral("every painted frame is counted"));
    check(view.frameRateText().startsWith(QStringLiteral("CPU · ")) &&
              view.frameRateText().contains(QStringLiteral(" fps · ")),
          QStringLiteral("a moving field reads its engine, frame rate and frame time"));
}

static void testLedGoesRound()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(4, QStringLiteral("four"));
    scene.addAgent(5, QStringLiteral("five"));
    scene.setSaid(5, QStringLiteral("Done."));
    scene.advanceTo(60.0);
    AgentActivityView view(&scene);
    view.resize(900, 600);
    QImage first(view.size(), QImage::Format_ARGB32);
    QImage second(view.size(), QImage::Format_ARGB32);
    view.render(&first);
    scene.advanceTo(75.0);
    view.render(&second);
    check(!scene.moving() && first == second
              && view.frameRateText() == QStringLiteral("CPU · idle"),
          QStringLiteral("a reply that fits its display holds still"));

    scene.setSaid(4, QStringLiteral("I sorted the index, rebuilt the docs, ran every test "
                                    "twice and pushed the branch for review."));
    view.render(&first);
    check(view.frameRateText() != QStringLiteral("CPU · idle"),
          QStringLiteral("a reply too long for its display scrolls"));
    // Long after one pass through, it is still going round.
    scene.advanceTo(scene.now() + 600.0);
    view.render(&first);
    scene.advanceTo(scene.now() + 0.1);
    view.render(&second);
    check(first != second && view.frameRateText() != QStringLiteral("CPU · idle"),
          QStringLiteral("a long reply keeps going round, not once and stop"));
}

int runAgentActivityViewTests()
{
    Scene scene;
    scene.advanceTo(20.0);
    AgentActivityView view(&scene);
    view.resize(880, 560);
    view.show();
    QApplication::processEvents();

    check(view.headline().startsWith(QStringLiteral("0 agents")),
          QStringLiteral("an empty field says so"));
    const int emptyColours = distinctColours(view.grab().toImage());
    check(view.sessionAt(QPointF(440, 280)) == -1,
          QStringLiteral("nothing is under the cursor in an empty field"));

    for (int session = 1; session <= 5; ++session) {
        scene.ingest(session, QStringLiteral("agent %1").arg(session),
                     toolUse(session % 2 ? "Bash" : "Read"));
        scene.setUsage(session, session * 3, qint64(session) * 90000);
    }
    // Newcomers fade in, and their marks land on their notes: both on the
    // scene's clock, so let a second of it pass between two frames.
    view.grab();
    scene.advanceTo(21.0);
    view.update();
    QApplication::processEvents();

    const QImage painted = view.grab().toImage();
    if (qEnvironmentVariableIsSet("FORKMESH_SAVE_FIELD"))
        painted.save(qEnvironmentVariable("FORKMESH_SAVE_FIELD"));
    check(!painted.isNull() && painted.size() == view.size() * view.devicePixelRatio(),
          QStringLiteral("the field paints offscreen at the widget's size"));
    check(distinctColours(painted) > emptyColours,
          QStringLiteral("agents, ripples and links are actually drawn"));
    check(view.headline().startsWith(QStringLiteral("5 agents")) &&
              view.headline().contains(QStringLiteral("working now")),
          QStringLiteral("the headline counts the fleet and who is working"));

    QSet<QString> places;
    for (int session = 1; session <= 5; ++session) {
        const QPointF at = view.bodyPosition(session);
        check(view.rect().contains(at.toPoint()),
              QStringLiteral("agent %1 is drawn inside the widget").arg(session));
        check(view.bodyRadius(session) > 0,
              QStringLiteral("agent %1 has a body to hit").arg(session));
        check(view.sessionAt(at) == session,
              QStringLiteral("the hit test finds agent %1 at its own centre").arg(session));
        places.insert(QStringLiteral("%1,%2").arg(qRound(at.x())).arg(qRound(at.y())));
    }
    check(places.size() == 5, QStringLiteral("no two agents are drawn on the same spot"));
    check(view.bodyRadius(5) >= view.bodyRadius(1),
          QStringLiteral("the agent that burned more tokens is not drawn smaller"));
    check(view.bodyRadius(99) <= 0 || view.sessionAt(view.bodyPosition(99)) != 99,
          QStringLiteral("an agent that is not in the field cannot be hit"));

    int selected = -1;
    int toggled = -1;
    view.setOnSelect([&selected](int session) { selected = session; });
    view.setOnToggleListen([&toggled](int session) { toggled = session; });
    click(&view, QEvent::MouseButtonPress, view.bodyPosition(3));
    check(selected == 3, QStringLiteral("a click picks the agent out"));
    click(&view, QEvent::MouseButtonDblClick, view.bodyPosition(4));
    check(toggled == 4, QStringLiteral("a double click asks for the agent to be heard"));

    click(&view, QEvent::MouseMove, QPointF(2, 2));
    check(view.hoverLines().isEmpty(), QStringLiteral("bare field has no words"));
    click(&view, QEvent::MouseMove, view.bodyPosition(2));
    check(view.hoveredSession() == 2 && !view.hoverLines().isEmpty()
              && view.hoverLines().first().contains(QStringLiteral("agent 2")),
          QStringLiteral("the agent under the cursor is named, and only then"));

    // No timeline along the bottom: the field has the whole height.
    check(view.bodyPosition(1).y() < view.height(), QStringLiteral("bodies are in the window"));

    view.setShowSounds(false);
    check(!view.showSounds(), QStringLiteral("the list of sounds can be switched off"));
    QApplication::processEvents();
    check(!view.grab().toImage().isNull(), QStringLiteral("and it still paints without it"));

    // A different size re-lays the field; bodies stay inside it and hittable.
    view.resize(420, 300);
    QApplication::processEvents();
    for (int session = 1; session <= 5; ++session) {
        const QPointF at = view.bodyPosition(session);
        check(view.rect().contains(at.toPoint()) && view.sessionAt(at) == session,
              QStringLiteral("agent %1 survives a resize").arg(session));
    }

    scene.removeAgent(3);
    view.update();
    QApplication::processEvents();
    check(view.headline().startsWith(QStringLiteral("4 agents")),
          QStringLiteral("an agent leaving the scene leaves the view"));

    testWorkingOnlyAndSounds();
    testHostPlaces();
    testHoverKey();
    testLogs();
    testTasksNetworkAndStillness();
    testProcessesAndAgentTasks();
    testFaces();
    testMirrors();
    testRelayClients();
    testConnectionClicks();
    testGpuFallbackAndFrameRate();
    testLedGoesRound();

    if (failures == 0)
        std::cout << "ALL TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
