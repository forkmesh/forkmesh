// The agents' field, checked without a window: what an event does to it, the
// seats it gives agents, and the links it draws between agents working at the
// same moment. Qt Core alone, like the sonifier's suite, and it checks that the
// picture and the music agree - about what counts as work, and about when a
// mark lands: on its note, not on its event. Also the network diff that feeds
// the field's packets.

#include "AgentActivityModel.h"
#include "AgentNetworkSampler.h"
#include "AgentSonifier.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace forkmesh::agentvis;
using forkmesh::sonify::kMotions;
using forkmesh::sonify::Motion;
using forkmesh::sonify::motionForEvent;
using forkmesh::sonify::voiceForSession;

static int failures = 0;

static void check(bool ok, const QString &what)
{
    if (ok)
        return;
    std::cerr << "FAIL: " << qPrintable(what) << "\n";
    ++failures;
}

static QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

static QJsonObject toolUse(const char *name)
{
    return parse(QByteArray(
                     R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":")")
                     .append(name)
                     .append(R"(","input":{"a":"b"}}]}})")
                     .constData());
}

static void testEveryMotionHasAMark()
{
    QSet<QString> labels;
    for (const Motion motion : kMotions) {
        const MotionStyle &style = styleFor(motion);
        if (motion == Motion::Rest) {
            check(std::strlen(style.label) == 0, QStringLiteral("rest draws nothing"));
            continue;
        }
        check(std::strlen(style.label) > 0,
              QStringLiteral("motion %1 has a legend word").arg(int(motion)));
        check(std::strlen(style.description) > 0,
              QStringLiteral("motion %1 says what its word means").arg(int(motion)));
        check(style.seconds > 0 && style.reach > 0,
              QStringLiteral("motion %1 has a ripple that lives and travels").arg(int(motion)));
        check(style.hue >= 0 && style.hue <= 1,
              QStringLiteral("motion %1 has a colour on the wheel").arg(int(motion)));
        check(!labels.contains(QString::fromLatin1(style.label)),
              QStringLiteral("legend word %1 is used once").arg(QLatin1String(style.label)));
        labels.insert(QString::fromLatin1(style.label));
    }
    check(styleFor(Motion::Arrive).inbound, QStringLiteral("data coming back falls inwards"));
    check(!styleFor(Motion::Run).inbound, QStringLiteral("work going out ripples outwards"));
}

static void testSeatsFollowTheVoice()
{
    for (int session = 1; session <= 40; ++session) {
        const Voice voice = voiceForSession(session, QStringLiteral("agent %1").arg(session));
        const double x = seatX(voice);
        const double y = seatY(voice);
        const double hue = hueForVoice(voice);
        check(x >= 0 && x <= 1 && y >= 0 && y <= 1 && hue >= 0 && hue <= 1,
              QStringLiteral("session %1 sits inside the field").arg(session));
        check(x == seatX(voice) && y == seatY(voice),
              QStringLiteral("session %1 keeps its seat").arg(session));
    }
    Scene scene;
    const Body &body = scene.addAgent(7, QStringLiteral("fix the worker"));
    check(body.sessionId == 7 && body.title == QStringLiteral("fix the worker"),
          QStringLiteral("an added agent is in the field under its own name"));
    check(body.hue == hueForVoice(body.voice),
          QStringLiteral("a body is the colour of the instrument it would be heard on"));
    scene.addAgent(7, QStringLiteral("fix the worker"));
    check(scene.bodies().size() == 1, QStringLiteral("adding an agent twice adds it once"));
    check(!describeBody(*scene.body(7)).isEmpty(),
          QStringLiteral("a body can be described in words"));
}

// Agents are seated in rows by what they are doing - working on top, waiting
// on you under it, idle or finished along the bottom - and each row reads
// left to right in the order they sit in the stereo mix.
static void testRowsByState()
{
    Scene scene;
    scene.advanceTo(0);
    for (int session = 1; session <= 12; ++session)
        scene.addAgent(session, QStringLiteral("agent %1").arg(session));
    for (int session = 1; session <= 10; ++session)
        scene.setState(session, State::Running);
    scene.setState(11, State::Waiting);
    scene.setState(12, State::Failed);
    check(scene.rows().size() == 4, QStringLiteral("ten working take two rows, then one each"));
    check(scene.rows().value(0).band == Band::Working && scene.rows().value(0).count == 10
              && scene.rows().value(1).count == 0
              && scene.rows().value(2).band == Band::Waiting
              && scene.rows().value(3).band == Band::Resting,
          QStringLiteral("the rows run working, waiting, resting, counted once each"));
    for (int i = 1; i < scene.rows().size(); ++i)
        check(scene.rows().at(i).y > scene.rows().at(i - 1).y, QStringLiteral("rows go down the field"));
    check(scene.rows().first().y - scene.rows().first().height / 2 >= Scene::kRowsTop - 1e-9
              && scene.rows().last().y + scene.rows().last().height / 2 <= Scene::kRowsBottom + 1e-9,
          QStringLiteral("the rows stay between the hosts and the desktop"));
    check(scene.body(1)->y < scene.body(11)->y && scene.body(11)->y < scene.body(12)->y,
          QStringLiteral("working above waiting above finished"));
    QVector<const Body *> firstRow;
    for (const Body &body : scene.bodies()) {
        if (std::abs(body.y - scene.rows().first().y) < 1e-9)
            firstRow.append(&body);
    }
    check(firstRow.size() == Scene::kRowSize, QStringLiteral("a full row holds kRowSize"));
    std::sort(firstRow.begin(), firstRow.end(),
              [](const Body *a, const Body *b) { return a->x < b->x; });
    for (int i = 1; i < firstRow.size(); ++i)
        check(seatX(firstRow.at(i - 1)->voice) <= seatX(firstRow.at(i)->voice),
              QStringLiteral("left to right in a row is left to right in the mix"));
    check(bandLabel(Band::Waiting) == QStringLiteral("waiting on you")
              && bandFor(State::Done) == Band::Resting && bandFor(State::Idle) == Band::Resting,
          QStringLiteral("every state has a named row"));

    // Finishing moves it down, gliding from where it was.
    const double before = scene.body(1)->y + scene.body(1)->dy;
    scene.setState(1, State::Done);
    check(scene.body(1)->y > before, QStringLiteral("a finished agent's seat is lower"));
    scene.advanceTo(0.05);
    check(std::abs(scene.body(1)->y + scene.body(1)->dy - before) < 0.2,
          QStringLiteral("and it glides there rather than jumping"));
    scene.advanceTo(10.0);
    check(std::abs(scene.body(1)->dy) < 0.02, QStringLiteral("until it sits in its new row"));
}

// Every mark an agent makes is counted by kind, for the cursor's key; the
// desktop's own marks likewise.
static void testKindCounts()
{
    Scene scene;
    scene.advanceTo(1.0);
    scene.addAgent(4, QStringLiteral("four"));
    // Apart in time: notes on one beat feed a single ripple.
    scene.note(4, Motion::Read, 0.0, 0.3);
    scene.advanceTo(1.5);
    scene.note(4, Motion::Read, 0.0, 0.3);
    scene.advanceTo(2.0);
    scene.note(4, Motion::Run, 0.0, 0.3);
    scene.cue(Scene::kDesktop, Motion::Task, QStringLiteral("sync"), 1);
    scene.advanceTo(3.0);
    check(scene.kindCount(4, Motion::Read) == 2 && scene.kindCount(4, Motion::Run) == 1
              && scene.kindCount(4, Motion::Search) == 0,
          QStringLiteral("an agent's marks are counted by kind"));
    check(scene.kindCount(Scene::kDesktop, Motion::Task) >= 1
              && scene.kindCount(Scene::kDesktop, Motion::Read) == 0,
          QStringLiteral("the desktop's own marks are counted apart"));
    check(scene.kindCount(99, Motion::Read) == 0 && scene.kindCount(4, Motion::Rest) == 0,
          QStringLiteral("nobody and rest count nothing"));
}

static void testWorkLeavesAMarkAndBookkeepingDoesNot()
{
    Scene scene;
    scene.advanceTo(10.0);
    const QJsonObject work = toolUse("Bash");
    // Bookkeeping between turns: the ear ticks for it, quietly.
    const QJsonObject quiet = parse(R"({"type":"system","subtype":"status"})");
    check(motionForEvent(work) != Motion::Rest && motionForEvent(quiet) == Motion::System,
          QStringLiteral("the fixtures mean what the sonifier says they mean"));

    check(scene.ingest(3, QStringLiteral("runner"), work),
          QStringLiteral("work the ear has a note for leaves a mark"));
    check(scene.body(3) != nullptr, QStringLiteral("an event brings its agent into the field"));
    check(scene.pulses().size() == 1 && scene.pulses().first().sessionId == 3 &&
              scene.pulses().first().motion == Motion::Run,
          QStringLiteral("the mark is the motion the music is written from"));
    check(scene.body(3)->events == 1 && scene.body(3)->lastMotion == Motion::Run,
          QStringLiteral("the body remembers"));
    // It glows when its note sounds, on the music's grid - not before.
    scene.advanceTo(scene.pulses().first().bornSeconds + 0.01);
    check(scene.body(3)->energy > 0, QStringLiteral("and glows on the beat"));
    check(scene.totalEvents() == 1 && scene.activeCount() == 1,
          QStringLiteral("the field counts it"));

    check(!scene.ingest(3, QStringLiteral("runner"), quiet),
          QStringLiteral("bookkeeping is not work"));
    check(scene.pulses().size() == 2 && scene.pulses().last().motion == Motion::System &&
              scene.totalEvents() == 1,
          QStringLiteral("but it ticks, so it is marked, just as quietly and uncounted"));
}

static void testTimeOnlyMovesWhenTheCallerMovesIt()
{
    Scene scene;
    scene.advanceTo(100.0);
    scene.ingest(1, QStringLiteral("a"), toolUse("Read"));
    const double born = scene.pulses().first().bornSeconds;
    const double seconds = scene.pulses().first().seconds;
    check(scene.body(1)->energy == 0 && scene.pulses().size() == 1,
          QStringLiteral("a scene nobody advances never changes"));

    // The note sounds, the body lights; then it is left alone.
    scene.advanceTo(born);
    const double energy = scene.body(1)->energy;
    check(energy > 0, QStringLiteral("the glow arrives with the note"));
    scene.advanceTo(born + Scene::kEnergyHalfLife);
    check(scene.body(1)->energy < energy * 0.75 && scene.body(1)->energy > energy * 0.25,
          QStringLiteral("a glow left alone halves in its half-life"));

    scene.advanceTo(born + seconds + 60.0);
    check(scene.pulses().isEmpty(), QStringLiteral("a ripple is gone once it has lived"));
    check(scene.body(1) != nullptr && scene.activeCount() == 0,
          QStringLiteral("the agent stays; it is just not working this second"));
}

static QJsonObject toolUseWith(const char *name, const char *input);

// Acting in the same second is not a connection: only a shared file or page
// joins two agents.
static void testOnlySharedWorkJoins()
{
    Scene scene;
    scene.advanceTo(50.0);
    scene.ingest(9, QStringLiteral("nine"), toolUse("Edit"));
    scene.advanceTo(50.0 + Scene::kJoinWindow / 2);
    scene.ingest(4, QStringLiteral("four"), toolUse("Grep"));
    check(scene.joins().isEmpty(),
          QStringLiteral("two agents merely acting at once are not linked"));

    scene.advanceTo(60.0);
    scene.ingest(9, QStringLiteral("nine"),
                 toolUseWith("Edit", R"({"file_path":"/src/One.cpp"})"));
    scene.advanceTo(61.0);
    scene.ingest(4, QStringLiteral("four"),
                 toolUseWith("Read", R"({"file_path":"/src/One.cpp"})"));
    QVector<Join> joins = scene.joins();
    check(joins.size() == 1, QStringLiteral("two agents on one file are one link"));
    if (joins.size() == 1) {
        check(joins.first().a == 4 && joins.first().b == 9,
              QStringLiteral("a link names the lower session first"));
        check(joins.first().meetings == 1 && joins.first().strength > 0,
              QStringLiteral("a first meeting lights the link"));
    }
    scene.advanceTo(51.0 + Scene::kJoinHalfLife * 40);
    check(scene.joins().isEmpty(), QStringLiteral("a link left alone fades out"));

    scene.advanceTo(400.0);
    scene.ingest(9, QStringLiteral("nine"),
                 toolUseWith("Edit", R"({"file_path":"/src/Two.cpp"})"));
    scene.ingest(4, QStringLiteral("four"),
                 toolUseWith("Edit", R"({"file_path":"/src/Two.cpp"})"));
    check(scene.joins().size() == 1, QStringLiteral("the same file again links them again"));
    scene.removeAgent(4);
    check(scene.body(4) == nullptr && scene.joins().isEmpty(),
          QStringLiteral("an agent leaving takes its links with it"));
}

// Every mark that lands is kept for the river and the rings, with its words,
// and a heard one lights its kind in the list of sounds.
static void testHistoryAndSounds()
{
    Scene scene;
    scene.advanceTo(5.0);
    scene.ingest(1, QStringLiteral("one"),
                 toolUseWith("Bash", R"({"command":"ninja -C build"})"));
    scene.advanceTo(8.0);
    check(!scene.history().isEmpty(), QStringLiteral("a landed mark is kept"));
    if (!scene.history().isEmpty()) {
        const Mark &mark = scene.history().last();
        check(mark.sessionId == 1 && mark.motion == Motion::Build,
              QStringLiteral("as the kind of work it was: a build"));
        check(mark.text.contains(QStringLiteral("ninja")),
              QStringLiteral("with its words for the cursor: %1").arg(mark.text));
        check(!mark.heard, QStringLiteral("an unheard agent's mark says so"));
    }
    check(scene.lastLanded(Motion::Build) > 0 && scene.lastSounded(Motion::Build) < 0,
          QStringLiteral("a silent mark is seen but not counted as sounding"));

    // Heard: the sonifier's note comes first, the event's words a moment after.
    scene.note(2, Motion::Search, 0.0, 0.2); // no body yet: ignored
    scene.addAgent(2, QStringLiteral("two"));
    scene.note(2, Motion::Search, 0.0, 0.2);
    scene.ingest(2, QStringLiteral("two"),
                 toolUseWith("Grep", R"({"pattern":"kSlotSeconds"})"), /*heard=*/true);
    check(scene.lastSounded(Motion::Search) >= 8.0 && scene.soundCount(Motion::Search) == 1,
          QStringLiteral("a heard note lights its kind"));
    check(!scene.history().isEmpty() && scene.history().last().heard
              && scene.history().last().text.contains(QStringLiteral("kSlotSeconds")),
          QStringLiteral("and its mark learns what it was for"));

    const QJsonObject framing =
        parse(R"({"type":"stream_event","event":{"type":"message_start"}})");
    check(labelForEvent(framing) == QStringLiteral("log: message_start"),
          QStringLiteral("bookkeeping still has words: %1").arg(labelForEvent(framing)));

    scene.advanceTo(8.0 + Scene::kHistorySeconds + 5);
    check(scene.history().isEmpty(), QStringLiteral("marks leave the history in time"));
}

static void testUsageStateAndEndings()
{
    Scene scene;
    scene.advanceTo(5.0);
    scene.addAgent(2, QStringLiteral("two"));
    const double before = scene.body(2)->size;
    scene.setUsage(2, 12, 480000);
    check(scene.body(2)->turns == 12 && scene.body(2)->tokens == 480000,
          QStringLiteral("usage is kept"));
    check(scene.body(2)->size >= before && scene.body(2)->size <= 1.0,
          QStringLiteral("tokens swell the body, within the field"));
    scene.setState(2, State::Waiting);
    scene.setListening(2, true);
    scene.setTitle(2, QStringLiteral("renamed"));
    check(scene.body(2)->state == State::Waiting && scene.body(2)->listening &&
              scene.body(2)->title == QStringLiteral("renamed"),
          QStringLiteral("state, listening and title are the caller's to set"));
    scene.setTask(2, QStringLiteral("tidy the docs"));
    check(scene.body(2)->task == QStringLiteral("tidy the docs")
              && scene.body(2)->title == QStringLiteral("renamed")
              && describeBody(*scene.body(2)).contains(QStringLiteral("renamed · tidy the docs")),
          QStringLiteral("the task is kept beside the project and described with it"));

    // Its latest reply, whole, off the finished turn and not the stream.
    scene.ingest(2, QStringLiteral("renamed"),
                 parse(R"({"type":"stream_event","event":{"type":"content_block_delta",)"
                       R"("delta":{"type":"text_delta","text":"Half"}}})"));
    check(scene.body(2)->said.isEmpty(), QStringLiteral("a streaming delta is not a reply"));
    scene.ingest(2, QStringLiteral("renamed"),
                 parse(R"({"type":"assistant","message":{"content":[)"
                       R"({"type":"text","text":"All **done**: the `docs` build."}]}})"));
    check(scene.body(2)->said == QStringLiteral("All **done**: the `docs` build."),
          QStringLiteral("an assistant turn's words are what it last said"));
    scene.ingest(2, QStringLiteral("renamed"),
                 parse(R"({"type":"_codex_agent_complete","text":"Codex says hi"})"));
    check(scene.body(2)->said == QStringLiteral("Codex says hi"),
          QStringLiteral("a Codex agent message is a reply too"));
    scene.setSaid(2, QString());
    check(scene.body(2)->said == QStringLiteral("Codex says hi"),
          QStringLiteral("nothing to say leaves the last reply in place"));
    check(plainProse(QStringLiteral("## Done\n\n- **one** `a`\n```\ncode\n```\nend"))
              == QStringLiteral("Done one a end"),
          QStringLiteral("a display's line has no markdown and no code blocks"));
    check(plainProse(QString(300, QLatin1Char('x')), 20).size() == 20,
          QStringLiteral("a long reply is cut to the display's limit"));

    const int pulses = scene.pulses().size();
    scene.finish(2, true);
    check(scene.pulses().size() == pulses + 1 &&
              scene.pulses().last().motion == Motion::Finish,
          QStringLiteral("an ending is a ring of its own"));

    for (int i = 0; i < Scene::kMaxPulses + 50; ++i)
        scene.ingest(2, QStringLiteral("renamed"), toolUse("Read"));
    check(scene.pulses().size() <= Scene::kMaxPulses,
          QStringLiteral("ripples in flight are capped"));
    check(scene.eventRate() > 0, QStringLiteral("the rate is counted from the marks"));

    scene.clear();
    check(scene.bodies().isEmpty() && scene.pulses().isEmpty() && scene.joins().isEmpty(),
          QStringLiteral("clear empties the field"));
}

static QJsonObject toolUseWith(const char *name, const char *input)
{
    return parse(QByteArray(R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":")")
                     .append(name)
                     .append(R"(","input":)")
                     .append(input)
                     .append("}]}}")
                     .constData());
}

static int pulsesFor(const Scene &scene, int sessionId, bool includePending)
{
    int count = 0;
    for (const Pulse &pulse : scene.pulses()) {
        if (pulse.sessionId == sessionId
            && (includePending || !pulse.pending(scene.now())))
            ++count;
    }
    return count;
}

static void testStyles()
{
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion == Motion::Rest)
            continue;
        check(QString::fromLatin1(styleFor(motion).label).size() > 0,
              QStringLiteral("every sounding motion has a mark (%1)").arg(int(motion)));
    }
    check(QString::fromLatin1(styleFor(Motion::Rest).label).isEmpty(),
          QStringLiteral("rest draws nothing"));
}

static void testSeats()
{
    Scene scene;
    scene.advanceTo(0);
    const Body &body = scene.addAgent(7, QStringLiteral("fix the worker"));
    check(body.x > 0.3 && body.x < 0.7, QStringLiteral("a lone agent sits in the middle of its row"));
    check(scene.bodies().size() == 1, QStringLiteral("one agent, one body"));
    scene.addAgent(7, QStringLiteral("fix the worker"));
    check(scene.bodies().size() == 1, QStringLiteral("adding twice is one body"));
    scene.removeAgent(7);
    check(scene.bodies().isEmpty(), QStringLiteral("removed agents leave the field"));
}

// A mark lands on the grid slot the music would play its note in, not the
// instant the event arrived; and a heard agent's mark waits for the note.
static void testMarksLandOnTheNote()
{
    Scene scene;
    scene.advanceTo(10.05);
    const QJsonObject read = toolUseWith("Read", R"({"file_path":"/src/a.cpp"})");
    check(scene.ingest(1, QStringLiteral("one"), read), QStringLiteral("a read is work"));
    check(pulsesFor(scene, 1, true) == 1, QStringLiteral("the read is scheduled a mark"));
    const Pulse pulse = scene.pulses().first();
    const double grid = pulse.bornSeconds / forkmesh::sonify::kSlotSeconds;
    const auto voice = scene.body(1)->voice;
    check(std::abs(grid - voice.slotOffset - std::round(grid - voice.slotOffset)) < 1e-6,
          QStringLiteral("the mark is born on the music's grid"));
    check(pulse.bornSeconds >= 10.05, QStringLiteral("never before the event"));
    const double energyBefore = scene.body(1)->energy;
    scene.advanceTo(pulse.bornSeconds + 0.01);
    check(scene.body(1)->energy > energyBefore,
          QStringLiteral("the body lights when its note sounds"));

    // Heard: the field waits for the sonifier to place the note.
    Scene heard;
    heard.advanceTo(1.0);
    heard.ingest(2, QStringLiteral("two"), read, /*heard=*/true);
    check(pulsesFor(heard, 2, true) == 0,
          QStringLiteral("a heard agent's mark waits for its note"));
    heard.note(2, Motion::Read, 0.4, 0.2);
    check(pulsesFor(heard, 2, true) == 1 && pulsesFor(heard, 2, false) == 0,
          QStringLiteral("the note schedules the mark, not yet visible"));
    heard.advanceTo(1.41);
    check(pulsesFor(heard, 2, false) == 1, QStringLiteral("and it shows when the note sounds"));
}

// Everything in the log is seen, bookkeeping included, but bookkeeping does
// not wake an agent or write a line of the feed.
static void testEverythingIsMarked()
{
    Scene scene;
    scene.advanceTo(5.0);
    const QJsonObject usage = parse(R"({"type":"_codex_usage","tokens":10})");
    check(!scene.ingest(3, QStringLiteral("three"), usage),
          QStringLiteral("usage is not work"));
    check(pulsesFor(scene, 3, true) == 1, QStringLiteral("but it leaves a mark"));
    check(scene.pulses().first().motion == Motion::Usage, QStringLiteral("a usage tap"));
    check(!scene.body(3)->working(scene.now()), QStringLiteral("and does not wake the agent"));
    check(scene.feed().isEmpty(), QStringLiteral("nor fill the feed"));
    const double seatX = scene.body(3)->dx;
    const double seatY = scene.body(3)->dy;
    scene.advanceTo(8.0);
    check(scene.body(3)->dx == seatX && scene.body(3)->dy == seatY,
          QStringLiteral("a tick does not move the body"));

    const QJsonObject framing = parse(R"({"type":"stream_event","event":{"type":"message_start"}})");
    scene.ingest(3, QStringLiteral("three"), framing);
    check(pulsesFor(scene, 3, true) >= 1, QStringLiteral("stream framing is marked too"));
}

// A flood of events is never silently lost in the picture: what the melody
// cannot take becomes grace notes, and those are marked like any other.
static void testFloodIsMarked()
{
    Scene scene;
    scene.advanceTo(2.0);
    const QJsonObject read = toolUseWith("Read", R"({"file_path":"/src/a.cpp"})");
    for (int i = 0; i < 12; ++i)
        scene.ingest(4, QStringLiteral("four"), read);
    check(pulsesFor(scene, 4, true) >= 10,
          QStringLiteral("a burst of reads is still marked (%1)").arg(pulsesFor(scene, 4, true)));
}

// Bodies do not bounce: running, sounding notes high or low, a body stays on
// its seat.
static void testBodiesHoldStill()
{
    Scene scene;
    scene.advanceTo(0);
    scene.addAgent(5, QStringLiteral("five"));
    scene.setState(5, State::Running);
    scene.advanceTo(3.0);
    const double dx0 = scene.body(5)->dx;
    const double dy0 = scene.body(5)->dy;
    scene.advanceTo(9.0);
    check(scene.body(5)->dx == dx0 && scene.body(5)->dy == dy0,
          QStringLiteral("a running body does not float"));

    scene.note(5, Motion::Delegate, 0.0, 0.25);
    scene.ingest(5, QStringLiteral("five"), toolUseWith("Bash", R"({"command":"ninja"})"));
    scene.advanceTo(9.2);
    check(scene.body(5)->energy > 0.2, QStringLiteral("a note lights the body"));
    check(scene.body(5)->dx == dx0 && scene.body(5)->dy == dy0,
          QStringLiteral("but does not knock, lift or sway it"));
}

static void testQuietFieldHoldsStill()
{
    Scene scene;
    scene.advanceTo(0);
    scene.addAgent(6, QStringLiteral("six"));
    scene.setState(6, State::Waiting);
    scene.addAgent(7, QStringLiteral("seven"));
    scene.setState(7, State::Done);
    scene.advanceTo(20.0); // long enough for the seats to have eased apart
    check(!scene.moving(), QStringLiteral("nothing running, nothing moving"));
    const double dx = scene.body(6)->dx;
    const double dy = scene.body(6)->dy;
    scene.advanceTo(40.0);
    check(scene.body(6)->dx == dx && scene.body(6)->dy == dy,
          QStringLiteral("a quiet body does not float"));

    // Running with nothing happening is not motion: there is nothing to draw
    // that changes until an event arrives.
    scene.setState(6, State::Running);
    check(scene.moving(), QStringLiteral("starting to work glides it up to the working row"));
    scene.advanceTo(60.0);
    check(!scene.moving(), QStringLiteral("a running agent between events holds still"));
    scene.setState(6, State::Waiting);

    // Traffic moves it while it lasts, and it settles after.
    scene.advanceTo(70.0);
    check(!scene.moving(), QStringLiteral("quiet again"));
    scene.network(Scene::kDesktop, QStringLiteral("example.com"), 50000, 200000);
    check(scene.moving(), QStringLiteral("network traffic moves the field"));
    check(scene.downRate() > scene.upRate() && scene.upRate() > Scene::kQuietRate,
          QStringLiteral("in and out are measured apart"));
    scene.advanceTo(130.0);
    check(!scene.moving(), QStringLiteral("and the field comes to rest after it"));
}

static void testBackgroundTasks()
{
    Scene scene;
    scene.advanceTo(0);
    scene.addAgent(8, QStringLiteral("eight"));
    scene.setState(8, State::Running);
    scene.advanceTo(1.0);
    check(!scene.moving(), QStringLiteral("no tasks, quiet"));
    scene.taskStarted(11, 8, QStringLiteral("git status"), QStringLiteral("git status --porcelain"));
    scene.taskStarted(12, 99, QStringLiteral("fetch"));
    scene.taskStarted(12, 99, QStringLiteral("fetch"));
    check(scene.tasks().size() == 2, QStringLiteral("a ticket is one task, however often it is told"));
    check(scene.tasks().at(0).sessionId == 8,
          QStringLiteral("work in a working agent's worktree is its"));
    check(scene.tasks().at(1).sessionId == Scene::kDesktop,
          QStringLiteral("work for nobody in the field is the desktop's"));
    check(scene.runningTasks() == 2 && scene.moving(),
          QStringLiteral("running background work moves the field"));

    scene.advanceTo(2.0);
    scene.taskFinished(11, /*failed=*/true);
    check(scene.runningTasks() == 1 && scene.tasks().at(0).failed,
          QStringLiteral("a finished task stays, in its outcome"));
    scene.advanceTo(2.0 + Scene::kTaskLinger + 0.1);
    check(scene.tasks().size() == 1, QStringLiteral("and leaves after it has flashed"));

    // Stopping or merging a run sets off git work in its worktree; that is
    // the desktop tidying up, not the run waking.
    scene.addAgent(9, QStringLiteral("nine"));
    for (const State state : {State::Idle, State::Done, State::Failed}) {
        scene.setState(9, state);
        scene.taskStarted(20 + int(state), 9, QStringLiteral("merge"));
        check(scene.tasks().last().sessionId == Scene::kDesktop,
              QStringLiteral("work in a stopped or finished agent's worktree is the desktop's"));
        scene.taskFinished(20 + int(state));
    }
    scene.setState(9, State::Waiting);
    scene.taskStarted(30, 9, QStringLiteral("diff"));
    check(scene.tasks().last().sessionId == 9,
          QStringLiteral("an agent waiting on you is still at work"));
    scene.taskFinished(30);
    scene.removeAgent(9);

    scene.taskStarted(13, 8, QStringLiteral("diff"));
    scene.removeAgent(8);
    check(scene.tasks().last().sessionId == Scene::kDesktop,
          QStringLiteral("an agent leaving hands its running work to the desktop"));
    scene.taskFinished(12);
    scene.taskFinished(13);
    scene.advanceTo(10.0);
    check(scene.tasks().isEmpty() && !scene.moving(),
          QStringLiteral("when the work is done the field is still"));
}

static void testAgentBackgroundWork()
{
    Scene scene;
    scene.advanceTo(0);
    scene.addAgent(8, QStringLiteral("eight"));
    scene.advanceTo(1.0);
    scene.ingest(8, QString(), parse(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","id":"tu_agent","name":"Agent",
         "input":{"description":"Find the sampler","subagent_type":"Explore"}},
        {"type":"tool_use","id":"tu_shell","name":"Bash",
         "input":{"command":"ninja -C build","run_in_background":true}},
        {"type":"tool_use","id":"tu_plain","name":"Bash","input":{"command":"ls"}}]}})"));
    check(scene.tasks().size() == 2, QStringLiteral("a subagent and a background shell are tasks, a plain command is not"));
    check(scene.tasks().at(0).kind == TaskKind::Subagent
              && scene.tasks().at(0).word == QStringLiteral("subagent · Explore")
              && scene.tasks().at(0).sessionId == 8,
          QStringLiteral("the subagent orbits its agent, by its type"));
    check(scene.tasks().at(1).kind == TaskKind::Shell
              && scene.tasks().at(1).detail == QStringLiteral("$ ninja -C build"),
          QStringLiteral("the shell says what it runs"));

    scene.advanceTo(2.0);
    scene.ingest(8, QString(), parse(R"({"type":"user","message":{"content":[
        {"type":"tool_result","tool_use_id":"tu_shell",
         "content":"Command running in background with ID: b1x9"},
        {"type":"tool_result","tool_use_id":"tu_agent","content":"found it"}]}})"));
    check(scene.runningTasks() == 1 && !scene.tasks().at(0).running()
              && !scene.tasks().at(0).failed,
          QStringLiteral("a subagent ends with its result; the shell runs on"));
    scene.advanceTo(40.0);
    scene.advanceTo(48.0); // the notes struck on that advance have settled
    check(!scene.moving(), QStringLiteral("a shell left running does not keep the field moving"));

    scene.ingest(8, QString(), parse(R"({"type":"user","message":{"content":
        "<task-notification><task-id>b1x9</task-id><status>failed</status></task-notification>"}})"));
    check(scene.runningTasks() == 0 && scene.tasks().last().failed,
          QStringLiteral("the notification ends the shell, in its outcome"));

    // An output check that finds it done ends it too.
    scene.advanceTo(50.0);
    scene.ingest(8, QString(), parse(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","id":"tu_bg2","name":"Bash",
         "input":{"command":"sleep 100","run_in_background":true}}]}})"));
    scene.ingest(8, QString(), parse(R"({"type":"user","message":{"content":[
        {"type":"tool_result","tool_use_id":"tu_bg2",
         "content":"Command running in background with ID: c77"}]}})"));
    scene.ingest(8, QString(), parse(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","id":"tu_out","name":"BashOutput","input":{"bash_id":"c77"}}]}})"));
    check(scene.runningTasks() == 1, QStringLiteral("checking on a shell is not a task"));
    scene.ingest(8, QString(), parse(R"({"type":"user","message":{"content":[
        {"type":"tool_result","tool_use_id":"tu_out","content":"<status>completed</status>"}]}})"));
    check(scene.runningTasks() == 0 && !scene.tasks().last().failed,
          QStringLiteral("an output check that finds it completed ends it"));

    // An agent leaving takes its own work with it.
    scene.ingest(8, QString(), parse(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","id":"tu_a2","name":"Task","input":{"description":"x"}}]}})"));
    scene.taskStarted(5, 8, QStringLiteral("fetch"));
    scene.clearTasks();
    check(scene.tasks().size() >= 1 && scene.tasks().last().kind == TaskKind::Subagent,
          QStringLiteral("clearing the desktop's tasks leaves the agents'"));
    scene.removeAgent(8);
    check(scene.runningTasks() == 0, QStringLiteral("and removing the agent ends them"));
}

static void testProcessTree()
{
    Scene scene;
    scene.advanceTo(0);
    scene.addAgent(8, QStringLiteral("eight"));
    scene.advanceTo(1.0);
    // 100 is the agent's root (a sandbox), 101 claude inside it.
    QList<ProcessInfo> tree{
        {101, 100, QStringLiteral("claude"), QStringLiteral("claude -p"), 500},
        {102, 101, QStringLiteral("bash"), QStringLiteral("bash -c ninja"), 1},
        {103, 102, QStringLiteral("ninja"), QStringLiteral("ninja -C build"), 10},
        {104, 101, QStringLiteral("node"), QStringLiteral("node mcp.js"), 3},
    };
    scene.sampleProcesses(8, tree, 100);
    check(scene.processes().size() == 3, QStringLiteral("the agent runtime is not one of its processes"));
    const Process *bash = scene.process(8, 102);
    const Process *ninja = scene.process(8, 103);
    const Process *node = scene.process(8, 104);
    check(bash && bash->parentPid == 0 && bash->depth == 1,
          QStringLiteral("what claude started hangs off the body"));
    check(ninja && ninja->parentPid == 102 && ninja->depth == 2,
          QStringLiteral("a grandchild hangs off its parent"));
    check(node && node->depth == 1, QStringLiteral("an MCP server under claude is drawn"));
    check(scene.moving(), QStringLiteral("processes appearing move the field"));

    scene.advanceTo(3.0);
    tree[2].cpuTicks = 10 + 200; // two cores' worth over two seconds
    scene.sampleProcesses(8, tree, 100);
    check(std::abs(scene.process(8, 103)->cpu - 1.0) < 0.01 && scene.process(8, 103)->busy(),
          QStringLiteral("CPU is read off the tick counters"));
    check(!scene.process(8, 104)->busy() && scene.busyProcesses() == 1,
          QStringLiteral("a sleeper is not busy"));
    check(scene.moving(), QStringLiteral("a busy process moves the field"));

    scene.advanceTo(5.0);
    tree.removeAt(2);
    scene.sampleProcesses(8, tree, 100);
    check(scene.runningProcesses() == 2, QStringLiteral("a process missing from the sample has ended"));
    scene.advanceTo(5.0 + Scene::kProcessLinger + 0.1);
    check(scene.processes().size() == 2, QStringLiteral("and leaves after a moment"));
    scene.advanceTo(20.0);
    scene.sampleProcesses(8, tree, 100);
    check(!scene.moving(), QStringLiteral("sleeping processes leave the field still"));

    scene.sampleProcesses(8, {}, 0);
    check(scene.runningProcesses() == 0, QStringLiteral("an empty sample ends them all"));

    QList<ProcessInfo> crowd;
    for (int i = 0; i < Scene::kMaxProcesses + 5; ++i)
        crowd.append({1000 + i, 100, QStringLiteral("cc1plus"), QString(), 0});
    scene.sampleProcesses(8, crowd, 100);
    check(scene.runningProcesses() == Scene::kMaxProcesses && scene.hiddenProcesses(8) == 5,
          QStringLiteral("a crowd is drawn up to the cap and counted past it"));
    scene.removeAgent(8);
    check(scene.processes().isEmpty(), QStringLiteral("processes leave with their agent"));
}

static void testJoinsSayWhy()
{
    Scene scene;
    scene.advanceTo(1.0);
    scene.ingest(10, QStringLiteral("ten"), toolUseWith("Edit", R"({"file_path":"/src/Shared.cpp"})"));
    scene.advanceTo(40.0); // long after: only the file connects them
    scene.ingest(11, QStringLiteral("eleven"), toolUseWith("Read", R"({"file_path":"/src/Shared.cpp"})"));
    const QVector<Join> joins = scene.joins();
    check(joins.size() == 1, QStringLiteral("the same file joins two agents"));
    if (!joins.isEmpty()) {
        check(joins.first().shared, QStringLiteral("and the join knows it was the file"));
        check(joins.first().why.contains(QStringLiteral("Shared.cpp")),
              QStringLiteral("and says which: %1").arg(joins.first().why));
    }

}

static void testActions()
{
    check(actionForEvent(toolUseWith("Edit", R"({"file_path":"/a/b/MainWindow.cpp"})"))
              == QStringLiteral("Edit MainWindow.cpp"),
          QStringLiteral("an edit names its file"));
    check(actionForEvent(toolUseWith("Bash", R"({"command":"git status"})"))
              == QStringLiteral("$ git status"),
          QStringLiteral("a command is shown as one"));
    check(actionForEvent(parse(R"({"type":"_local_user","text":"try again"})"))
              .contains(QStringLiteral("try again")),
          QStringLiteral("a prompt quotes itself"));
    check(subjectForEvent(toolUseWith("Read", R"({"file_path":"/x/y.h"})")) == QStringLiteral("/x/y.h"),
          QStringLiteral("the subject is the path"));
    check(shortSubject(QStringLiteral("https://example.com/a/b")) == QStringLiteral("example.com"),
          QStringLiteral("a URL is its host"));

    Scene scene;
    scene.advanceTo(1.0);
    const QJsonObject delta = parse(R"({"type":"stream_event","event":{"type":"content_block_delta",)"
                                    R"("delta":{"type":"text_delta","text":"a"}}})");
    for (int i = 0; i < 30; ++i)
        scene.ingest(30, QStringLiteral("x"), delta);
    check(scene.feed().size() == 1 && scene.feed().first().repeats == 30,
          QStringLiteral("a streaming reply is one line of the feed, counted"));
}

// The network diff: a baseline first, then only what moved; a socket that
// appears later counts in full; bytes land on the session that holds it.
static void testNetworkLedger()
{
    using namespace forkmesh::agentnet;
    Ledger ledger;
    Sample first;
    first.sockets = {Socket{1, QStringLiteral("1.2.3.4"), 443, 1000, 5000},
                     Socket{2, QStringLiteral("5.6.7.8"), 443, 10, 10},
                     Socket{9, QStringLiteral("9.9.9.9"), 443, 77, 77}};
    first.owner = {{1, 7}, {2, 8}};
    check(ledger.update(first).isEmpty(), QStringLiteral("the first look is a baseline"));

    Sample second = first;
    second.sockets[0].sent = 1500;
    second.sockets[0].received = 9000;
    second.sockets.append(Socket{3, QStringLiteral("1.2.3.4"), 443, 200, 300});
    second.owner.insert(3, 7);
    const QList<Delta> deltas = ledger.update(second);
    check(deltas.size() == 1, QStringLiteral("only session 7 moved, to one host"));
    if (!deltas.isEmpty()) {
        check(deltas.first().sessionId == 7 && deltas.first().peer == QStringLiteral("1.2.3.4"),
              QStringLiteral("to the right host, for the right session"));
        check(deltas.first().up == 500 + 200 && deltas.first().down == 4000 + 300,
              QStringLiteral("the diff plus the new socket in full"));
    }
    check(isLoopback(QStringLiteral("127.0.0.1")) && isLoopback(QStringLiteral("::1"))
              && !isLoopback(QStringLiteral("10.0.0.1")),
          QStringLiteral("loopback is this machine"));

    Scene scene;
    scene.advanceTo(1.0);
    scene.addAgent(7, QStringLiteral("seven"));
    scene.network(7, QStringLiteral("api.example.com"), 4096, 120000);
    check(scene.hosts().size() == 1, QStringLiteral("a host appears at the top"));
    check(!scene.packets().isEmpty(), QStringLiteral("and packets leave at once"));
    check(scene.body(7)->netDown == 120000, QStringLiteral("the body counts what it moved"));
    check(scene.downRate() > scene.upRate(), QStringLiteral("the rates follow the bytes"));
    scene.renameHost(QStringLiteral("api.example.com"), QStringLiteral("named.example.com"));
    check(scene.host(QStringLiteral("named.example.com")) != nullptr
              && scene.flows().first().host == QStringLiteral("named.example.com"),
          QStringLiteral("a lookup renames the host and its flows"));
    scene.advanceTo(1.0 + Scene::kHostLinger + 1);
    check(scene.hosts().isEmpty() && scene.packets().isEmpty(),
          QStringLiteral("a quiet host leaves"));

    // Each connection keeps its address and port through a lookup, and two
    // addresses that resolve to one name fold together with both kept.
    scene.network(7, QStringLiteral("140.82.112.3"), 100, 200, QStringLiteral("140.82.112.3"), 443);
    scene.network(7, QStringLiteral("140.82.112.4"), 10, 20, QStringLiteral("140.82.112.4"), 22);
    scene.renameHost(QStringLiteral("140.82.112.3"), QStringLiteral("github.com"));
    scene.renameHost(QStringLiteral("140.82.112.4"), QStringLiteral("github.com"));
    const Host *github = scene.host(QStringLiteral("github.com"));
    check(github && scene.hosts().size() == 1 && github->endpoints.size() == 2
              && github->up == 110 && github->down == 220,
          QStringLiteral("a resolved host keeps every address it was reached on"));
    check(scene.flows().size() == 1 && scene.flows().first().endpoints.size() == 2,
          QStringLiteral("and so does the session's flow to it"));
    check(describeEndpoint(QStringLiteral("140.82.112.3"), 443)
              == QStringLiteral("140.82.112.3:443 · IPv4, public · TCP https"),
          QStringLiteral("a connection's network details read on one line"));
    check(describeEndpoint(QStringLiteral("fd00::5"), 9418)
              == QStringLiteral("[fd00::5]:9418 · IPv6, private (unique local) · TCP git"),
          QStringLiteral("IPv6 is bracketed and scoped"));
    check(addressInfo(QStringLiteral("192.168.1.9")) == QStringLiteral("IPv4, private (LAN)")
              && addressInfo(QStringLiteral("100.70.1.1")).contains(QStringLiteral("carrier"))
              && addressInfo(QStringLiteral("this machine")).isEmpty(),
          QStringLiteral("address scopes"));

    // An address with no name: whois says who holds it. ARIN refers the
    // client on to RIPE, whose (later, more specific) block wins.
    const QStringList whois = parseWhois(QStringLiteral(
        "# ARIN WHOIS data\n"
        "NetRange:       185.0.0.0 - 185.255.255.255\n"
        "NetName:        RIPE-185\n"
        "OriginAS:\n"
        "Organization:   RIPE Network Coordination Centre (RIPE)\n"
        "\n"
        "% This is the RIPE Database query service.\n"
        "inetnum:        185.199.108.0 - 185.199.111.255\n"
        "netname:        US-GITHUB-20170413\n"
        "descr:          GitHub, Inc.\n"
        "descr:          88 Colin P Kelly Jr St\n"
        "country:        US\n"
        "abuse-mailbox:  abuse@github.com\n"
        "created:        2017-04-13T10:00:00Z\n"
        "\n"
        "route:          185.199.108.0/24\n"
        "origin:         AS54113\n"));
    check(whois.contains(QStringLiteral("network: US-GITHUB-20170413"))
              && whois.contains(QStringLiteral("range: 185.199.108.0 - 185.199.111.255"))
              && whois.contains(QStringLiteral("about: GitHub, Inc."))
              && whois.contains(QStringLiteral("owner: RIPE Network Coordination Centre (RIPE)"))
              && whois.contains(QStringLiteral("cidr: 185.199.108.0/24"))
              && whois.contains(QStringLiteral("country: US"))
              && whois.contains(QStringLiteral("AS: AS54113"))
              && whois.contains(QStringLiteral("abuse: abuse@github.com")),
          QStringLiteral("whois names the network, range, owner and AS of a nameless address"));
    check(parseWhois(QStringLiteral("% no entries found\n")).isEmpty(),
          QStringLiteral("an empty whois answer yields nothing"));
    scene.setAddressNetwork(QStringLiteral("185.199.108.1"), whois);
    check(scene.addressNetwork(QStringLiteral("185.199.108.1")) == whois,
          QStringLiteral("the scene keeps what whois said for the cursor"));
}

static void testTrafficLog()
{
    using namespace forkmesh::agentnet;
    Ledger ledger;
    Sample first;
    first.sockets = {Socket{1, QStringLiteral("1.2.3.4"), 443, 100, 100},
                     Socket{2, QStringLiteral("1.2.3.4"), 443, 100, 100}};
    first.owner = {{1, 7}, {2, 7}};
    first.holder = {{1, 4242}, {2, 5151}};
    first.names = {{4242, QStringLiteral("git")}, {5151, QStringLiteral("curl")}};
    ledger.update(first);
    Sample second = first;
    second.sockets[0].sent = 300;
    second.sockets[1].received = 900;
    QList<Delta> deltas = ledger.update(second);
    std::sort(deltas.begin(), deltas.end(),
              [](const Delta &a, const Delta &b) { return a.pid < b.pid; });
    check(deltas.size() == 2, QStringLiteral("two processes on one address are two connections"));
    if (deltas.size() == 2) {
        check(deltas[0].pid == 4242 && deltas[0].process == QStringLiteral("git")
                  && deltas[0].up == 200 && deltas[0].down == 0,
              QStringLiteral("each names the process holding its socket"));
        check(deltas[1].pid == 5151 && deltas[1].process == QStringLiteral("curl")
                  && deltas[1].down == 800,
              QStringLiteral("and counts only its own bytes"));
    }

    Scene scene;
    scene.advanceTo(1.0);
    scene.addAgent(7, QStringLiteral("seven"));
    const QString address = QStringLiteral("140.82.112.3");
    scene.network(7, address, 100, 2000, address, 443, 4242, QStringLiteral("git"));
    scene.network(8, address, 5, 5, address, 443, 5151, QStringLiteral("curl"));
    const quint64 firstSerial = scene.traffic(Scene::kAnySession, address).value(0).serial;
    scene.advanceTo(2.0);
    scene.network(7, address, 50, 0, address, 443, 4242, QStringLiteral("git"));
    check(scene.traffic(Scene::kAnySession, address).size() == 3
              && scene.traffic(7, address).size() == 2,
          QStringLiteral("every sample is logged, by host and by session"));
    check(scene.traffic(Scene::kAnySession, address, firstSerial).size() == 2,
          QStringLiteral("a reader asks only for what came after what it has"));
    const Traffic newest = scene.traffic(7, address).last();
    check(newest.pid == 4242 && newest.process == QStringLiteral("git") && newest.port == 443
              && newest.address == address && newest.up == 50 && newest.seconds == 2.0,
          QStringLiteral("a record keeps who, where, which process and how much"));

    scene.renameHost(address, QStringLiteral("github.com"));
    check(scene.hostNamed(address) == QStringLiteral("github.com")
              && scene.traffic(Scene::kAnySession, address).size() == 3
              && scene.traffic(Scene::kAnySession, address).first().host
                     == QStringLiteral("github.com"),
          QStringLiteral("a lookup renames the log, and the old name still finds it"));

    QVector<Connection> connections =
        scene.connections(Scene::kAnySession, QStringLiteral("github.com"));
    check(connections.size() == 2, QStringLiteral("one connection per session and process"));
    if (connections.size() == 2) {
        const Connection &git = connections.first();
        check(git.sessionId == 7 && git.pid == 4242 && git.up == 150 && git.down == 2000
                  && git.samples == 2 && git.firstSeconds == 1.0 && git.lastSeconds == 2.0,
              QStringLiteral("the latest connection first, with its totals"));
        check(qAbs(git.upRate - 150 / Scene::kConnectionRateSeconds) < 1e-9,
              QStringLiteral("its rate is the bytes of the last few seconds"));
    }
    scene.advanceTo(10.0);
    connections = scene.connections(7, QStringLiteral("github.com"));
    check(connections.size() == 1 && connections.first().upRate == 0
              && connections.first().downRate == 0,
          QStringLiteral("a quiet connection keeps its totals and drops its rate"));

    for (int i = 0; i < Scene::kMaxTraffic + Scene::kMaxTraffic / 4; ++i)
        scene.network(7, address, 1, 1, address, 443, 4242, QStringLiteral("git"));
    const QVector<Traffic> all = scene.traffic(Scene::kAnySession, QString());
    check(all.size() <= Scene::kMaxTraffic + Scene::kMaxTraffic / 8
              && all.last().serial == scene.lastTraffic(),
          QStringLiteral("the log is capped and keeps the newest"));
}

// The desktop's log in the field: a line finds the agent it names, repeats
// count up, a seeded backlog does not spark, and the column is capped.
static void testLogs()
{
    Scene scene;
    scene.advanceTo(100.0);
    scene.addAgent(281, QStringLiteral("fix the worker"));
    scene.setState(281, State::Running);
    check(scene.agentForLog(QStringLiteral("agent 281 finished")) == 281,
          QStringLiteral("'agent N' names the agent"));
    check(scene.agentForLog(QStringLiteral("rebuild for #281 queued")) == 281,
          QStringLiteral("'#N' names the agent"));
    check(scene.agentForLog(QStringLiteral("Session 281: stopped")) == 281,
          QStringLiteral("'session N' names the agent, any case"));
    check(scene.agentForLog(QStringLiteral("issue #12 opened")) == Scene::kDesktop,
          QStringLiteral("a number that is no agent in the field is the desktop's"));
    check(scene.agentForLog(QStringLiteral("port 281")) == Scene::kDesktop,
          QStringLiteral("a bare number names nobody"));

    // Stopping or merging one logs its number; that is the desktop's doing.
    scene.addAgent(282, QStringLiteral("stopped one"));
    scene.setState(282, State::Failed);
    const double stoppedBefore = scene.body(282)->energy;
    const int pulsesBefore = int(scene.pulses().size());
    check(scene.agentForLog(QStringLiteral("Stopped #282")) == 282
              && scene.ownerForLog(QStringLiteral("Stopped #282")) == Scene::kDesktop,
          QStringLiteral("a line naming a stopped agent is the desktop's"));
    check(scene.log(QStringLiteral("AGENT"), QStringLiteral("#58a6ff"),
                    QStringLiteral("Merged #282 into main")) == Scene::kDesktop
              && scene.body(282)->energy <= stoppedBefore,
          QStringLiteral("and it does not light the agent"));
    bool markedStopped = false;
    for (int i = pulsesBefore; i < scene.pulses().size(); ++i)
        markedStopped = markedStopped || scene.pulses().at(i).sessionId == 282;
    check(!markedStopped, QStringLiteral("nor throw a mark off it"));
    scene.removeAgent(282);
    scene.clearLogs();

    const double energyBefore = scene.body(281)->energy;
    check(scene.log(QStringLiteral("AGENT"), QStringLiteral("#58a6ff"),
                    QStringLiteral("agent 281 finished")) == 281,
          QStringLiteral("log() reports whom the line was put against"));
    check(scene.body(281)->energy > energyBefore,
          QStringLiteral("a line about a working agent lights it"));

    check(scene.logs().size() == 1 && scene.logs().last().sparking(scene.now()),
          QStringLiteral("a new line sparks"));
    scene.log(QStringLiteral("AGENT"), QStringLiteral("#58a6ff"),
              QStringLiteral("agent 281 finished"));
    check(scene.logs().size() == 1 && scene.logs().last().repeats == 2,
          QStringLiteral("the same line again is counted, not repeated"));
    check(scene.logRate() == 2, QStringLiteral("the rate counts repeats"));
    scene.advanceTo(scene.now() + LogLine::kSparkSeconds + 0.1);
    check(!scene.logSparking(), QStringLiteral("the spark goes out"));

    scene.log(QStringLiteral("ERROR"), QStringLiteral("#f85149"),
              QStringLiteral("an old failure"), 600.0);
    check(!scene.logSparking() && scene.logs().last().sessionId == Scene::kDesktop,
          QStringLiteral("a backlog line is the desktop's and born spent"));
    check(scene.logRate() == 2, QStringLiteral("a ten-minute-old line is not this minute's"));

    for (int i = 0; i < Scene::kLogSize + 20; ++i)
        scene.log(QStringLiteral("INFO"), QString(), QStringLiteral("line %1").arg(i));
    check(scene.logs().size() == Scene::kLogSize, QStringLiteral("the log is capped"));
    check(scene.logs().last().text == QStringLiteral("line %1").arg(Scene::kLogSize + 19),
          QStringLiteral("and keeps the newest"));
    scene.clearLogs();
    check(scene.logs().isEmpty(), QStringLiteral("clearLogs empties it"));

    check(Scene::outputLine(QStringLiteral(
              "Node n2 core forkmesh-app info: cron minute 29839378 ran on n2")),
          QStringLiteral("a core node's journal line is output"));
    check(Scene::outputLine(QStringLiteral("Node r1 runner earlyoom info: mem avail: 656")),
          QStringLiteral("a runner's journal line is output"));
    check(Scene::outputLine(QStringLiteral("Mirror mirror35 ssh: Accepted publickey")),
          QStringLiteral("a mirror host's forwarded line is output"));
    check(!Scene::outputLine(QStringLiteral(
              "Mirror: synced forkmesh/forkmesh into /home/f/mirrors/forkmesh.git.")),
          QStringLiteral("the desktop's own mirror sync is not"));
    check(!Scene::outputLine(QStringLiteral("Node connected: n3 is online")),
          QStringLiteral("nor its note that a node came online"));
    scene.log(QStringLiteral("CLOUD"), QString(),
              QStringLiteral("Node n3 core rqlite info: [http] stats"));
    scene.log(QStringLiteral("INFO"), QString(), QStringLiteral("net GET 200 /dashboard"));
    check(scene.logs().size() == 2 && scene.logs().first().output
              && !scene.logs().last().output,
          QStringLiteral("each line records whether it is output"));
}

static void testDanceSeedPromptAndLiveText()
{
    Scene scene;
    scene.advanceTo(10.0);
    const QJsonObject build = toolUseWith("Bash", R"({"command":"ninja -C build"})");
    scene.ingest(4, QStringLiteral("four"), build);
    check(!scene.pulses().isEmpty() && scene.pulses().last().seed == forkmesh::sonify::eventSeed(build),
          QStringLiteral("a mark carries its event's seed"));
    const double born = scene.pulses().isEmpty() ? 10.0 : scene.pulses().last().bornSeconds;
    scene.advanceTo(born + 0.01);
    const Body *body = scene.body(4);
    check(body && body->energy > 0.2, QStringLiteral("a note lights its body"));
    check(scene.lastText(Motion::Build).contains(QStringLiteral("ninja"))
              && scene.landCount(Motion::Build) == 1,
          QStringLiteral("the kind's row knows the latest one, word for word"));
    scene.ingest(4, QStringLiteral("four"),
                 toolUseWith("Bash", R"({"command":"cmake --build out"})"));
    scene.advanceTo(born + 2.0);
    check(scene.lastText(Motion::Build).contains(QStringLiteral("cmake"))
              && scene.landCount(Motion::Build) == 2,
          QStringLiteral("and the next replaces it as it lands"));
    scene.advanceTo(born + 30.0);
    body = scene.body(4);
    check(body && body->energy < 0.05 && !scene.moving(),
          QStringLiteral("then the glow fades and the field is still"));

    scene.setPrompt(4, QStringLiteral("  fix the build  "));
    scene.ingest(4, QStringLiteral("four"),
                 parse(R"({"type":"_local_user","text":"now add a test for it"})"));
    body = scene.body(4);
    check(body && body->prompt == QStringLiteral("fix the build")
              && body->lastPrompt == QStringLiteral("now add a test for it"),
          QStringLiteral("the body keeps what it was asked"));
}

// A log line, a host joining and background work each leave their own kind of
// mark: off the agent a line names, off the desktop's node otherwise.
static void testCuesAreMarked()
{
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(281, QStringLiteral("fix the worker"));
    scene.setState(281, State::Running);
    scene.advanceTo(12.0);
    scene.log(QStringLiteral("ERROR"), QStringLiteral("#f85149"),
              QStringLiteral("agent 281 could not push"));
    scene.log(QStringLiteral("MIRROR"), QStringLiteral("#39c5cf"),
              QStringLiteral("mirror caught up"));
    scene.advanceTo(scene.now() + 2.0);
    check(scene.logs().last().motion == Motion::LogSync,
          QStringLiteral("a line knows its kind"));
    check(scene.landCount(Motion::LogError) == 1 && scene.landCount(Motion::LogSync) == 1,
          QStringLiteral("every new line lands a mark of its badge's kind"));
    bool onAgent = false;
    bool onDesktop = false;
    for (const Mark &mark : scene.history()) {
        onAgent = onAgent || (mark.motion == Motion::LogError && mark.sessionId == 281);
        onDesktop = onDesktop
                    || (mark.motion == Motion::LogSync && mark.sessionId == Scene::kDesktop);
    }
    check(onAgent, QStringLiteral("a line about an agent marks the agent"));
    check(onDesktop, QStringLiteral("a line about nobody marks the desktop"));

    const int before = scene.landCount(Motion::LogInfo);
    scene.log(QStringLiteral("INFO"), QString(), QStringLiteral("old news"), 600.0);
    scene.advanceTo(scene.now() + 2.0);
    check(scene.landCount(Motion::LogInfo) == before,
          QStringLiteral("a backlog seeded on open does not sound"));

    check(scene.network(Scene::kDesktop, QStringLiteral("example.com"), 100, 100),
          QStringLiteral("a host new to the field says so"));
    check(!scene.network(Scene::kDesktop, QStringLiteral("example.com"), 100, 100),
          QStringLiteral("and only the first time"));
    scene.cue(Scene::kDesktop, Motion::Connect, QStringLiteral("connected to example.com"), 7);
    scene.cue(281, Motion::Task, QStringLiteral("fetch started"), 9);
    scene.cue(404, Motion::Task, QStringLiteral("status started"), 11);
    scene.cue(281, Motion::Run, QStringLiteral("not a cue"), 13);
    scene.advanceTo(scene.now() + 2.0);
    check(scene.landCount(Motion::Connect) == 1, QStringLiteral("a new host is marked"));
    check(scene.landCount(Motion::Task) == 2,
          QStringLiteral("background work is marked, an unknown owner's on the desktop"));
    check(scene.landCount(Motion::Run) == 0, QStringLiteral("cue() takes only cues"));
    check(scene.lastText(Motion::Connect) == QStringLiteral("connected to example.com"),
          QStringLiteral("a cue's mark says what it was"));
    check(scene.body(404) == nullptr, QStringLiteral("a cue never invents an agent"));
    check(Scene::logSeed(QStringLiteral("GIT"), QStringLiteral("retry 3"))
                  == Scene::logSeed(QStringLiteral("GIT"), QStringLiteral("retry 4"))
              && Scene::logSeed(QStringLiteral("GIT"), QStringLiteral("retry"))
                     != Scene::logSeed(QStringLiteral("GIT"), QStringLiteral("merged")),
          QStringLiteral("a line's shade is its words, numbers aside"));
}

static void testMirrorsAreSources()
{
    // Every saved host streams its journal into the desktop's log under the
    // MIRROR badge; those lines are the mirror's own activity, not the
    // desktop's, and they mark off its seat.
    Scene scene;
    scene.advanceTo(10.0);
    scene.addAgent(281, QStringLiteral("fix the worker"));
    scene.setState(281, State::Running);
    scene.setMirrors({{QStringLiteral("mirror35"), {QStringLiteral("203.0.113.5")}},
                      {QStringLiteral("mirror36"), {QStringLiteral("203.0.113.6")}}});
    const int first = scene.mirrorSource(QStringLiteral("mirror35"));
    const int second = scene.mirrorSource(QStringLiteral("mirror36"));
    check(first != 0 && second != 0 && first != second
              && Scene::isMirrorSource(first) && Scene::isMirrorSource(second)
              && !Scene::isMirrorSource(Scene::kDesktop) && !Scene::isMirrorSource(281),
          QStringLiteral("each seated mirror is a source of its own"));
    check(scene.mirrorForSource(first)
              && scene.mirrorForSource(first)->name == QStringLiteral("mirror35"),
          QStringLiteral("and the field can find it again by its id"));

    check(scene.log(QStringLiteral("MIRROR"), QStringLiteral("#39c5cf"),
                    QStringLiteral("Mirror mirror35 mirror: forkmesh-mirror-node "
                                   "synced forkmesh-web")) == first,
          QStringLiteral("a forwarded host line is put against the mirror it came from"));
    const Host *seat = scene.mirrorForSource(first);
    check(seat && seat->lines == 1 && seat->energy > 0
              && seat->lastLine.contains(QStringLiteral("synced forkmesh-web")),
          QStringLiteral("the mirror's seat lights and keeps what it last said"));
    check(scene.logs().last().sessionId == first && scene.lineRate(first) == 1
              && scene.lineRate(Scene::kDesktop) == 0,
          QStringLiteral("its line counts against it, not the desktop"));
    int marks = 0;
    for (const Pulse &pulse : scene.pulses()) {
        if (pulse.sessionId == first)
            ++marks;
    }
    check(marks > 0, QStringLiteral("and it throws a mark of its own"));
    scene.advanceTo(scene.now() + 3.0);
    check(scene.kindCount(first, scene.logs().last().motion) > 0
              && scene.kindCount(Scene::kDesktop, scene.logs().last().motion) == 0,
          QStringLiteral("the mark lands in the mirror's own count"));

    // Only a line that names a seated mirror, and only under that badge.
    check(scene.mirrorForLog(QStringLiteral("MIRROR"),
                             QStringLiteral("Mirror nowhere ssh: hello")) == 0
              && scene.mirrorForLog(QStringLiteral("INFO"),
                                    QStringLiteral("Mirror mirror35 ssh: hello")) == 0
              && scene.mirrorForLog(QStringLiteral("MIRROR"),
                                    QStringLiteral("Mirror mirror36 web: GET /")) == second,
          QStringLiteral("a line is only a mirror's when it names one of them"));
    check(scene.log(QStringLiteral("MIRROR"), QStringLiteral("#39c5cf"),
                    QStringLiteral("Mirror nowhere ssh: hello")) == Scene::kDesktop,
          QStringLiteral("a host with no seat is still the desktop's"));
    check(scene.log(QStringLiteral("AGENT"), QStringLiteral("#58a6ff"),
                    QStringLiteral("agent 281 finished")) == 281,
          QStringLiteral("and an agent's line is still the agent's"));

    scene.advanceTo(scene.now() + 30.0);
    check(scene.mirrorForSource(first)->energy < 0.01,
          QStringLiteral("a mirror that has gone quiet dims again"));
}

static void testMirrorsAreSeated()
{
    Scene scene;
    scene.advanceTo(1.0);
    scene.addAgent(7, QStringLiteral("seven"));
    // Traffic before the mirrors are known, under an address and under the
    // name a reverse lookup gave another of the mirror's addresses.
    scene.network(7, QStringLiteral("example.com"), 10, 10, QStringLiteral("93.184.216.34"), 443);
    scene.network(7, QStringLiteral("203.0.113.5"), 100, 200, QStringLiteral("203.0.113.5"), 22);
    scene.network(7, QStringLiteral("vultr.example"), 5, 5, QStringLiteral("203.0.113.6"), 443);
    scene.setMirrors({{QStringLiteral("mirror-a"),
                       {QStringLiteral("203.0.113.5"), QStringLiteral("203.0.113.6")}},
                      {QStringLiteral("mirror-b"), {QStringLiteral("198.51.100.9")}}});
    const Host *a = scene.host(QStringLiteral("mirror-a"));
    const Host *b = scene.host(QStringLiteral("mirror-b"));
    check(a && a->mirror && b && b->mirror, QStringLiteral("every mirror has a seat"));
    check(a && a->up == 105 && a->down == 205 && !scene.host(QStringLiteral("203.0.113.5"))
              && !scene.host(QStringLiteral("vultr.example")),
          QStringLiteral("traffic already drawn to a mirror's addresses is folded into it"));
    check(scene.hosts().size() == 3 && scene.hosts().at(0).name == QStringLiteral("mirror-a")
              && scene.hosts().at(1).name == QStringLiteral("mirror-b")
              && scene.hosts().at(2).name == QStringLiteral("example.com"),
          QStringLiteral("mirrors sit at the left, in the order listed"));
    check(scene.mirrorForAddress(QStringLiteral("198.51.100.9")) == QStringLiteral("mirror-b"),
          QStringLiteral("an address names its mirror"));

    // An agent's first word with a mirror is a connection, even though the
    // mirror was already there; its next is not.
    check(scene.network(7, QStringLiteral("198.51.100.9"), 50, 50,
                        QStringLiteral("198.51.100.9"), 22),
          QStringLiteral("reaching a mirror is worth a cue"));
    check(!scene.network(7, QStringLiteral("whatever.dns"), 50, 50,
                         QStringLiteral("198.51.100.9"), 22),
          QStringLiteral("but only the first time"));
    check(scene.host(QStringLiteral("mirror-b"))->up == 100
              && !scene.host(QStringLiteral("whatever.dns")),
          QStringLiteral("traffic to a mirror's address is the mirror's, whatever its name"));
    scene.renameHost(QStringLiteral("mirror-b"), QStringLiteral("elsewhere"));
    check(scene.host(QStringLiteral("mirror-b")), QStringLiteral("nothing renames a mirror"));

    // Quiet mirrors stay; quiet hosts leave; a cleared field keeps the seats.
    scene.advanceTo(1.0 + Scene::kHostLinger + 1);
    check(scene.hosts().size() == 2 && scene.host(QStringLiteral("mirror-a"))
              && scene.host(QStringLiteral("mirror-b")),
          QStringLiteral("a quiet mirror keeps its seat"));
    scene.clear();
    check(scene.hosts().size() == 2 && scene.hosts().first().mirror,
          QStringLiteral("clearing the field keeps the mirrors"));
    for (int i = 0; i < Scene::kMaxHosts + 2; ++i)
        scene.network(7, QStringLiteral("h%1.example").arg(i), 1, 1);
    check(scene.host(QStringLiteral("mirror-a")) && scene.host(QStringLiteral("mirror-b"))
              && scene.hosts().size() == Scene::kMaxHosts + 2,
          QStringLiteral("a full strip never evicts a mirror"));

    // Unlisted, a mirror is an ordinary host again and leaves when quiet.
    scene.setMirrors({{QStringLiteral("mirror-b"), {QStringLiteral("198.51.100.9")}}});
    const Host *unlisted = scene.host(QStringLiteral("mirror-a"));
    check(!unlisted || !unlisted->mirror,
          QStringLiteral("an unlisted mirror is no longer held"));
    scene.advanceTo(scene.now() + 0.1);
    check(!scene.host(QStringLiteral("mirror-a")) && scene.host(QStringLiteral("mirror-b")),
          QStringLiteral("and a never-used one leaves at once"));
}

static void testRelayClients()
{
    // Who else is on the relay sits in a row beside the desktop: a browser
    // on the site is a web visitor, a phone is a phone, anything with a
    // platform and a node name is another desktop.
    check(clientKindFor({QStringLiteral("a"), QStringLiteral("Guest 12"), QString(),
                         QStringLiteral("guest"), QString(), QString()})
              == ClientKind::Visitor,
          QStringLiteral("a chat guest is a web visitor"));
    check(clientKindFor({QStringLiteral("b"), QStringLiteral("dana"), QString(),
                         QStringLiteral("user"), QString(), QString()})
              == ClientKind::Visitor,
          QStringLiteral("somebody signed in on the site is a web visitor"));
    check(clientKindFor({QStringLiteral("c"), QStringLiteral("World visitor · 4"), QString(),
                         QString(), QString(), QString()})
              == ClientKind::Visitor,
          QStringLiteral("a world visitor is a web visitor"));
    check(clientKindFor({QStringLiteral("d"), QStringLiteral("erin"), QStringLiteral("ios"),
                         QStringLiteral("user"), QStringLiteral("erin"), QString()})
              == ClientKind::Phone,
          QStringLiteral("the mobile app is a phone"));
    check(clientKindFor({QStringLiteral("e"), QStringLiteral("frank"), QStringLiteral("linux"),
                         QStringLiteral("node"), QStringLiteral("frank-box"),
                         QStringLiteral("1.2.3")})
              == ClientKind::Node,
          QStringLiteral("a desktop is a node"));
    check(clientKindFor({QStringLiteral("f"), QStringLiteral("gus"), QStringLiteral("macos"),
                         QStringLiteral("user"), QString(), QString()})
              == ClientKind::Node,
          QStringLiteral("a desktop signed in as a user is still a desktop"));

    Scene scene;
    scene.advanceTo(10.0);
    const ClientInfo visitor{QStringLiteral("v1"), QStringLiteral("Guest 7"), QString(),
                             QStringLiteral("guest"), QString(), QString()};
    const ClientInfo node{QStringLiteral("n1"), QStringLiteral("frank"), QStringLiteral("linux"),
                          QStringLiteral("node"), QStringLiteral("frank-box"), QString()};
    // Opening on a room already full: everybody is there, nobody arrives.
    QList<ClientChange> changes = scene.setClients({visitor}, /*announce=*/false);
    check(changes.isEmpty() && scene.clientCount(ClientKind::Visitor) == 1,
          QStringLiteral("seeding the row announces nobody"));
    check(!scene.moving(), QStringLiteral("a seeded client does not animate an arrival"));

    changes = scene.setClients({visitor, node});
    check(changes.size() == 1 && changes.first().joined
              && changes.first().kind == ClientKind::Node,
          QStringLiteral("a newcomer is announced once"));
    check(scene.clients().first().info.id == QStringLiteral("n1"),
          QStringLiteral("desktops sit nearest the node, visitors further out"));
    check(scene.moving(), QStringLiteral("an arrival opens its ring"));
    check(scene.setClients({visitor, node}).isEmpty(),
          QStringLiteral("the same roster again changes nothing"));

    // Frames: from a client to the desktop, and from the desktop to the room.
    scene.socketFrame(false, QStringLiteral("chat"), QStringLiteral("v1"), 420);
    const Client *guest = scene.client(QStringLiteral("v1"));
    check(guest && guest->framesIn == 1 && guest->bytesIn == 420
              && guest->lastType == QStringLiteral("chat") && guest->energy > 0.5,
          QStringLiteral("a frame from a visitor lights it and counts"));
    check(scene.frames().size() == 1 && !scene.frames().first().up
              && scene.frames().first().client == QStringLiteral("v1"),
          QStringLiteral("a received frame is a bead coming in from its client"));
    scene.socketFrame(true, QStringLiteral("presence"), QString(), 180);
    check(scene.frames().size() == 3,
          QStringLiteral("a frame to the room is a bead to everybody in it"));
    check(scene.client(QStringLiteral("n1"))->framesOut == 1
              && scene.client(QStringLiteral("v1"))->framesOut == 1,
          QStringLiteral("and each of them counts it"));
    scene.socketFrame(false, QStringLiteral("hello"), QStringLiteral("stranger"), 90);
    check(scene.frames().last().client.isEmpty(),
          QStringLiteral("a frame from nobody in the row comes from the room"));
    check(scene.frameRate() == 3 && scene.totalFrames() == 3,
          QStringLiteral("the rate counts frames, not beads"));
    scene.advanceTo(scene.now() + 2.0);
    check(scene.frames().isEmpty(), QStringLiteral("beads land and are gone"));

    // Leaving: a farewell, a fade, then gone.
    changes = scene.setClients({node});
    check(changes.size() == 1 && !changes.first().joined
              && changes.first().kind == ClientKind::Visitor,
          QStringLiteral("a visitor leaving is announced"));
    check(scene.client(QStringLiteral("v1")) && scene.clientCount(ClientKind::Visitor) == 0,
          QStringLiteral("it fades out rather than vanishing"));
    check(scene.moving(), QStringLiteral("and the fade moves the field"));
    scene.socketFrame(false, QStringLiteral("typing"), QStringLiteral("v1"), 60);
    check(scene.client(QStringLiteral("v1"))->framesIn == 1,
          QStringLiteral("a client that left takes no more frames"));
    scene.advanceTo(scene.now() + Scene::kClientLinger + 0.1);
    check(!scene.client(QStringLiteral("v1")), QStringLiteral("and then it is gone"));
    scene.advanceTo(scene.now() + 5.0);
    check(!scene.moving(), QStringLiteral("a quiet room holds still"));

    // Gone while nobody watched: no farewell when the window opens again.
    check(scene.setClients({}, /*announce=*/false).isEmpty() && scene.clients().isEmpty(),
          QStringLiteral("re-seeding drops the departed silently"));

    RelaySocket socket;
    socket.connected = true;
    socket.framesIn = 12;
    scene.setRelaySocket(socket);
    check(scene.relaySocket().connected && scene.relaySocket().framesIn == 12,
          QStringLiteral("the socket's counters are kept for the cursor"));
    scene.clear();
    check(!scene.relaySocket().connected && scene.totalFrames() == 0,
          QStringLiteral("clear() forgets the relay too"));

    // The site's own visitors, from the Worker's live log.
    check(browserForUserAgent(QStringLiteral(
              "Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 Firefox/130.0"))
                  == QStringLiteral("Firefox")
              && browserForUserAgent(QStringLiteral(
                     "Mozilla/5.0 (Windows NT 10.0) AppleWebKit/537.36 Chrome/128.0 "
                     "Safari/537.36 Edg/128.0"))
                     == QStringLiteral("Edge")
              && browserForUserAgent(QStringLiteral(
                     "Mozilla/5.0 (compatible; Googlebot/2.1)"))
                     == QStringLiteral("a crawler")
              && browserForUserAgent(QStringLiteral("curl/8.5.0")).isEmpty()
              && browserForUserAgent(QStringLiteral("ForkMesh/1.2.3 (linux)")).isEmpty(),
          QStringLiteral("a browser is named, anything else is not a visitor"));
    Scene site;
    site.advanceTo(50.0);
    site.setClients({{QStringLiteral("n1"), QStringLiteral("frank"), QStringLiteral("linux"),
                      QStringLiteral("node"), QStringLiteral("frank-box"), QString(), QString()}},
                    false);
    check(site.webRequest(QStringLiteral("198.51.100.4"), QStringLiteral("Firefox in Oslo, NO"),
                          QStringLiteral("198.51.100.4"), QStringLiteral("GET / 200")),
          QStringLiteral("a browser's first request seats it"));
    check(!site.webRequest(QStringLiteral("198.51.100.4"), QStringLiteral("Firefox in Oslo, NO"),
                           QString(), QStringLiteral("GET /docs 200")),
          QStringLiteral("and its next is the same visitor"));
    const Client *web = site.client(QStringLiteral("web:198.51.100.4"));
    check(web && web->web && web->kind == ClientKind::Visitor && web->requests == 2
              && web->framesIn == 2 && web->lastType == QStringLiteral("GET /docs 200"),
          QStringLiteral("each request is a frame in from the visitor"));
    check(site.clientCount(ClientKind::Visitor) == 1,
          QStringLiteral("and counts as a web visitor"));
    site.setClients({}, true);
    check(site.client(QStringLiteral("web:198.51.100.4"))->present()
              && !site.client(QStringLiteral("n1"))->present(),
          QStringLiteral("the relay's roster does not speak for the site's visitors"));
    site.advanceTo(site.now() + Scene::kWebVisitorIdle + 1.0);
    check(!site.client(QStringLiteral("web:198.51.100.4"))
              || !site.client(QStringLiteral("web:198.51.100.4"))->present(),
          QStringLiteral("a visitor quiet for five minutes has left"));
    site.advanceTo(site.now() + Scene::kClientLinger + 1.0);
    check(site.clients().isEmpty(), QStringLiteral("and is gone once it has faded"));
    check(site.webRequest(QStringLiteral("198.51.100.4"), QStringLiteral("Firefox"), QString(),
                          QStringLiteral("GET / 200")),
          QStringLiteral("coming back later is another visit"));

    for (const Motion motion : {Motion::Visitor, Motion::Client, Motion::Socket}) {
        scene.cue(Scene::kDesktop, motion, QStringLiteral("relay"), 5);
        scene.advanceTo(scene.now() + 2.0);
        check(scene.landCount(motion) == 1,
              QStringLiteral("relay cue %1 is marked").arg(int(motion)));
    }
}

// A site visitor as the Worker's log reports it.
static Scene::WebHit hit(const QString &address, const QString &path, int status = 200,
                         const QString &agent = QStringLiteral(
                             "Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 "
                             "Firefox/130.0"),
                         const QString &asn = QStringLiteral("2119"),
                         const QString &country = QStringLiteral("NO"))
{
    Scene::WebHit h;
    h.visitor = address;
    h.name = QStringLiteral("Firefox in Oslo, NO");
    h.request = QStringLiteral("GET %1 %2").arg(path).arg(status);
    h.path = path;
    h.status = status;
    h.country = country;
    h.address = address;
    h.asn = asn;
    h.asOrganization = QStringLiteral("Telenor");
    h.userAgent = agent;
    return h;
}

static void testSiteVisitors()
{
    // Who is a browser, a script, an attack; where they are.
    bool attack = false;
    check(toolForUserAgent(QStringLiteral("sqlmap/1.7.2#stable (https://sqlmap.org)"), &attack)
                  == QStringLiteral("sqlmap")
              && attack,
          QStringLiteral("an attack tool is named and known for one"));
    check(toolForUserAgent(QStringLiteral("Mozilla/5.0 (compatible; Nuclei - Open-source "
                                          "project (github.com/projectdiscovery/nuclei))"),
                           &attack)
                  == QStringLiteral("nuclei")
              && attack,
          QStringLiteral("even one dressed as a browser"));
    check(toolForUserAgent(QStringLiteral("curl/8.5.0"), &attack) == QStringLiteral("curl")
              && !attack
              && toolForUserAgent(QStringLiteral("python-requests/2.31")) == QStringLiteral("python-requests")
              && toolForUserAgent(QString()) == QStringLiteral("no user agent")
              && toolForUserAgent(QStringLiteral("ForkMesh/1.2.3 (linux)")).isEmpty()
              && toolForUserAgent(QStringLiteral("Mozilla/5.0 (X11) Firefox/130.0")).isEmpty(),
          QStringLiteral("a script is named; a browser and the desktop's own calls are not"));
    check(systemForUserAgent(QStringLiteral("Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X)"))
                  == QStringLiteral("iOS")
              && systemForUserAgent(QStringLiteral("Mozilla/5.0 (Linux; Android 14)"))
                     == QStringLiteral("Android")
              && systemForUserAgent(QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64)"))
                     == QStringLiteral("Windows")
              && systemForUserAgent(QStringLiteral("curl/8.5.0")).isEmpty(),
          QStringLiteral("the system is read off the user agent, most specific first"));
    check(flagForCountry(QStringLiteral("no")) == QString::fromUcs4(U"\U0001F1F3\U0001F1F4", 2)
              && flagForCountry(QStringLiteral("XX")).isEmpty()
              && flagForCountry(QStringLiteral("T1")).isEmpty()
              && flagForCountry(QStringLiteral("NOR")).isEmpty(),
          QStringLiteral("a country's flag is its two regional indicators"));
    check(countryName(QStringLiteral("NO")) == QStringLiteral("Norway")
              && countryName(QStringLiteral("T1")) == QStringLiteral("Tor"),
          QStringLiteral("a country is named"));
    check(isProbePath(QStringLiteral("/.env")) && isProbePath(QStringLiteral("/wp-login.php"))
              && isProbePath(QStringLiteral("/.git/config"))
              && isProbePath(QStringLiteral("/static/../../etc/passwd"))
              && !isProbePath(QStringLiteral("/docs")) && !isProbePath(QStringLiteral("/")),
          QStringLiteral("a scanner's paths are told from the site's own"));

    Scene site;
    site.advanceTo(50.0);
    site.setClients({{QStringLiteral("n1"), QStringLiteral("frank"), QStringLiteral("linux"),
                      QStringLiteral("node"), QStringLiteral("frank-box"), QString(), QString()}},
                    false);
    site.webRequest(hit(QStringLiteral("198.51.100.4"), QStringLiteral("/docs")));
    site.webRequest(hit(QStringLiteral("198.51.100.4"), QStringLiteral("/missing"), 404));
    // Looked up afresh every time: a newcomer can move the others in the list.
    const auto web = [&site](const char *address) {
        return site.client(QStringLiteral("web:") + QLatin1String(address));
    };
    const Client *oslo = web("198.51.100.4");
    check(oslo && web("198.51.100.4")->info.country == QStringLiteral("NO")
              && web("198.51.100.4")->info.address == QStringLiteral("198.51.100.4")
              && web("198.51.100.4")->info.asn == QStringLiteral("2119") && !web("198.51.100.4")->info.userAgent.isEmpty(),
          QStringLiteral("a site visitor keeps where it is and what it runs"));
    check(web("198.51.100.4")->recent.size() == 2 && web("198.51.100.4")->recent.first().first == QStringLiteral("GET /missing 404")
              && web("198.51.100.4")->types.value(QStringLiteral("2xx")) == 1
              && web("198.51.100.4")->types.value(QStringLiteral("4xx")) == 1 && web("198.51.100.4")->refused == 1,
          QStringLiteral("and its latest pages, newest first, with how they were answered"));
    check(web("198.51.100.4")->threat == Threat::None && site.siteThreat() == Threat::None,
          QStringLiteral("an ordinary visitor is not a threat"));

    // The desktop's frames to the room go to the room, not to the site.
    site.socketFrame(true, QStringLiteral("presence"), QString(), 120);
    check(site.client(QStringLiteral("n1"))->framesOut == 1 && web("198.51.100.4")->framesOut == 0
              && web("198.51.100.4")->lastType == QStringLiteral("GET /missing 404"),
          QStringLiteral("a frame to the room is not sent to a visitor outside it"));

    // One visitor: a probe, then an attack tool, then a flood.
    site.webRequest(hit(QStringLiteral("203.0.113.9"), QStringLiteral("/.env"), 404));
    const auto prober = [&web] { return web("203.0.113.9"); };
    check(prober()->threat == Threat::Watch && prober()->threatReasons.join(QString()).contains(
                                                 QStringLiteral("/.env")),
          QStringLiteral("a probe for /.env is worth watching"));
    site.webRequest(hit(QStringLiteral("203.0.113.9"), QStringLiteral("/wp-login.php"), 404));
    site.webRequest(hit(QStringLiteral("203.0.113.9"), QStringLiteral("/.git/config"), 404));
    check(prober()->threat == Threat::Danger && site.siteThreat() == Threat::Danger
              && site.flaggedCount() == 1,
          QStringLiteral("three probes are danger, and so is the site"));
    QList<BlockRule> rules = site.blockRules(QStringLiteral("web:203.0.113.9"));
    const auto recommended = [](const QList<BlockRule> &list) {
        for (const BlockRule &rule : list) {
            if (rule.recommended)
                return rule;
        }
        return BlockRule{};
    };
    check(rules.size() == 5 && recommended(rules).target == BlockTarget::Address
              && recommended(rules).value == QStringLiteral("203.0.113.9"),
          QStringLiteral("a lone prober is blocked by its address"));

    site.webRequest(hit(QStringLiteral("192.0.2.66"), QStringLiteral("/"), 200,
                        QStringLiteral("sqlmap/1.7.2#stable (https://sqlmap.org)")));
    const auto tool = [&web] { return web("192.0.2.66"); };
    check(tool()->threat == Threat::Danger && tool()->attackTool,
          QStringLiteral("an attack tool is danger from its first request"));
    rules = site.blockRules(QStringLiteral("web:192.0.2.66"));
    check(recommended(rules).target == BlockTarget::UserAgent
              && recommended(rules).value.startsWith(QStringLiteral("sqlmap")),
          QStringLiteral("and is blocked by name, from every address"));

    for (int i = 0; i < Scene::kWatchRate; ++i)
        site.webRequest(hit(QStringLiteral("198.51.100.4"), QStringLiteral("/docs")));
    check(web("198.51.100.4")->threat == Threat::Watch,
          QStringLiteral("a fast rate is worth watching"));
    site.advanceTo(site.now() + 61.0);
    site.webRequest(hit(QStringLiteral("198.51.100.4"), QStringLiteral("/docs")));
    check(web("198.51.100.4")->threat == Threat::None && web("198.51.100.4")->requestRate(site.now()) == 1,
          QStringLiteral("and is forgotten a minute later"));

    // A crowd out of one range: each of them alone is nothing.
    for (int i = 1; i <= Scene::kCrowdDanger; ++i)
        site.webRequest(hit(QStringLiteral("45.55.10.%1").arg(i), QStringLiteral("/"), 200,
                            QStringLiteral("Mozilla/5.0 (X11) Chrome/120 Safari/537.36"),
                            QStringLiteral("14061"), QStringLiteral("US")));
    const auto crowd = [&web] { return web("45.55.10.1"); };
    check(crowd()->threat == Threat::Danger
              && crowd()->threatReasons.join(QString()).contains(QStringLiteral("45.55.10.0/24")),
          QStringLiteral("twenty newcomers out of one range are danger"));
    rules = site.blockRules(QStringLiteral("web:45.55.10.1"));
    check(recommended(rules).target == BlockTarget::Range
              && recommended(rules).value == QStringLiteral("45.55.10.0/24")
              && recommended(rules).visitors == Scene::kCrowdDanger,
          QStringLiteral("and are blocked by their range, which holds nobody else"));
    const QList<BlockRule> suggested = site.suggestedBlocks();
    int rangeRules = 0;
    int crowdAddresses = 0;
    for (const BlockRule &rule : suggested) {
        if (rule.target == BlockTarget::Range)
            ++rangeRules;
        if (rule.target == BlockTarget::Address && rule.value.startsWith(QStringLiteral("45.55.10.")))
            ++crowdAddresses;
    }
    check(rangeRules == 1 && crowdAddresses == 0,
          QStringLiteral("the suggestions merge the crowd into one range rule"));
    check(site.arrivalRate() >= Scene::kCrowdDanger,
          QStringLiteral("every newcomer counts towards the arrival rate"));

    // Blocked: marked, and so is whoever the rule catches later.
    site.noteBlocked(recommended(rules));
    check(crowd()->blocked && !web("198.51.100.4")->blocked,
          QStringLiteral("a block marks the visitors it catches, and nobody else"));
    site.webRequest(hit(QStringLiteral("45.55.10.200"), QStringLiteral("/")));
    check(site.client(QStringLiteral("web:45.55.10.200"))->blocked,
          QStringLiteral("and anyone it catches afterwards"));
    check(site.blockRules(QStringLiteral("n1")).isEmpty(),
          QStringLiteral("a desktop in the relay room has no address to block"));
}

int runAgentActivityModelTests()
{
    testTrafficLog();
    testLogs();
    testEveryMotionHasAMark();
    testSeatsFollowTheVoice();
    testRowsByState();
    testKindCounts();
    testWorkLeavesAMarkAndBookkeepingDoesNot();
    testTimeOnlyMovesWhenTheCallerMovesIt();
    testOnlySharedWorkJoins();
    testHistoryAndSounds();
    testUsageStateAndEndings();
    testStyles();
    testSeats();
    testMarksLandOnTheNote();
    testEverythingIsMarked();
    testFloodIsMarked();
    testBodiesHoldStill();
    testQuietFieldHoldsStill();
    testBackgroundTasks();
    testAgentBackgroundWork();
    testProcessTree();
    testDanceSeedPromptAndLiveText();
    testJoinsSayWhy();
    testActions();
    testNetworkLedger();
    testCuesAreMarked();
    testMirrorsAreSeated();
    testMirrorsAreSources();
    testRelayClients();
    testSiteVisitors();
    if (failures == 0)
        std::cout << "ALL TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
