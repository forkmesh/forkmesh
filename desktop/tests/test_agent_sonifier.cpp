// The music itself, checked without an audio device: the scale, what each
// transcript event does to the melody, the grid several agents share, and the
// mixer's output. Everything here runs on Qt Core alone.

#include "AgentSonifier.h"
#include "UiSoundCues.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cmath>
#include <iostream>
#include <vector>

using namespace forkmesh::sonify;

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

static Voice probeVoice()
{
    return voiceForSession(1, QStringLiteral("probe"));
}

static void testScale()
{
    // The plain voice: centred on the tonic, so the intervals are readable.
    const Voice voice;
    const double root = degreeFrequency(0, voice);
    check(std::abs(degreeFrequency(5, voice) - root * 2) < 0.01,
          QStringLiteral("five degrees is an octave"));
    check(std::abs(degreeFrequency(-5, voice) - root / 2) < 0.01,
          QStringLiteral("five degrees down is an octave down"));
    // Degree -1 is the B below the tonic: three semitones down.
    check(std::abs(degreeFrequency(-1, voice) - root * std::pow(2.0, -3.0 / 12.0)) < 0.01,
          QStringLiteral("the scale keeps walking below its tonic"));
    for (int degree = -7; degree <= 12; ++degree) {
        check(degreeFrequency(degree, voice) > 20 && degreeFrequency(degree, voice) < 8000,
              QStringLiteral("degree %1 stays audible").arg(degree));
    }
}

static void testVoices()
{
    // A voice belongs to the agent, not to the order you switched it on: the
    // same session and title always sound the same.
    const Voice a = voiceForSession(281, QStringLiteral("create a listen button"));
    const Voice b = voiceForSession(281, QStringLiteral("create a listen button"));
    check(a.instrument == b.instrument && a.octave == b.octave && a.pan == b.pan
              && a.degreeOffset == b.degreeOffset && a.slotOffset == b.slotOffset,
          QStringLiteral("an agent sounds the same every time"));
    check(a.index == 281, QStringLiteral("the voice knows its session"));

    // Different agents differ, and the salt re-rolls a clash.
    int same = 0;
    for (int id = 1; id <= 40; ++id) {
        const Voice v = voiceForSession(id, QStringLiteral("agent %1").arg(id));
        check(v.pan >= 0 && v.pan <= 1, QStringLiteral("voice %1 is in the field").arg(id));
        check(v.octave >= 0 && v.octave <= 2, QStringLiteral("voice %1 is in range").arg(id));
        check(std::abs(v.detune - 1.0) < 0.01, QStringLiteral("voice %1 stays in tune").arg(id));
        if (v.instrument == a.instrument && v.octave == a.octave)
            ++same;
    }
    check(same <= 6, QStringLiteral("agents rarely share an instrument and a register"));
    const Voice rolled = voiceForSession(281, QStringLiteral("create a listen button"), 1);
    check(rolled.instrument != a.instrument || rolled.octave != a.octave
              || rolled.pan != a.pan,
          QStringLiteral("the salt re-rolls a clash"));

    check(!voiceDescription(a).isEmpty() && voiceDescription(a).contains(QLatin1Char(',')),
          QStringLiteral("a voice can be described in words"));
}

static void testUsageShapesTheVoice()
{
    Voice voice = voiceForSession(12, QStringLiteral("long runner"));
    const double fresh = degreeFrequency(0, voice);
    applyUsage(&voice, 0, 0);
    check(voice.drift == 0 && voice.bloom == 0,
          QStringLiteral("a run that has done nothing sounds as it started"));

    applyUsage(&voice, 50, 120000);
    check(voice.drift > 0, QStringLiteral("turns walk the line up the scale"));
    check(degreeFrequency(0, voice) > fresh, QStringLiteral("a long run sits higher"));
    check(voice.bloom > 0.5 && voice.bloom <= 1.0,
          QStringLiteral("tokens fill the instrument out"));

    // Both are bounded: an agent that runs all day stays in its register.
    applyUsage(&voice, 100000, 100000000);
    check(voice.drift <= 5, QStringLiteral("the drift is capped"));
    check(voice.bloom <= 1.0, QStringLiteral("the bloom is capped"));
    check(degreeFrequency(0, voice) < 4000, QStringLiteral("it never runs off the top"));
}

static void testInstrumentsSoundDifferent()
{
    // Two instruments playing the same note must produce different samples,
    // which is the whole point of giving each agent one.
    Note base;
    base.frequency = 440;
    base.seconds = 0.4;
    base.gain = 0.3;
    for (int a = 0; a < 6; ++a) {
        for (int b = a + 1; b < 6; ++b) {
            Mixer left;
            Mixer right;
            Note na = base;
            na.instrument = Instrument(a);
            Note nb = base;
            nb.instrument = Instrument(b);
            left.addNote(na);
            right.addNote(nb);
            std::vector<qint16> la(size_t(4410) * kChannels);
            std::vector<qint16> lb(la.size());
            left.render(la.data(), 4410);
            right.render(lb.data(), 4410);
            double diff = 0;
            double energy = 0;
            for (size_t i = 0; i < la.size(); ++i) {
                diff += std::abs(double(la[i]) - double(lb[i]));
                energy += std::abs(double(la[i]));
            }
            check(energy > 0 && diff > energy * 0.2,
                  QStringLiteral("instruments %1 and %2 sound different").arg(a).arg(b));
        }
    }
}

static void testMotions()
{
    check(motionForEvent(parse(R"({"type":"system","subtype":"init"})")) == Motion::Start,
          QStringLiteral("init starts the piece"));
    check(motionForEvent(parse(R"({"type":"result","subtype":"success"})")) == Motion::Finish,
          QStringLiteral("the result event closes the phrase with a single bell"));
    check(motionForEvent(parse(R"({"type":"system","subtype":"status"})")) == Motion::System,
          QStringLiteral("a status change between turns is the machinery"));
    check(motionForEvent(parse(R"({"type":"system","subtype":"hook_response"})")) == Motion::Hook,
          QStringLiteral("a hook is its own quiet kind"));
    check(motionForEvent(parse(R"({"type":"keep_alive"})")) == Motion::Tick,
          QStringLiteral("a line of no known kind still ticks: it is in the log"));
    check(motionForEvent(parse(R"({"type":"_local_user","text":"go"})")) == Motion::Prompt,
          QStringLiteral("the operator steering is a prompt"));
    check(motionForEvent(parse(R"({"type":"_local_notice","text":"careful"})")) == Motion::Notice,
          QStringLiteral("a local notice is a notice"));
    check(motionForEvent(toolUse("Read")) == Motion::Read, QStringLiteral("Read reads"));
    check(motionForEvent(toolUse("Grep")) == Motion::Search, QStringLiteral("Grep searches"));
    check(motionForEvent(toolUse("Edit")) == Motion::Write, QStringLiteral("Edit writes"));
    check(motionForEvent(toolUse("Bash")) == Motion::Run, QStringLiteral("Bash runs"));
    check(motionForEvent(toolUse("Task")) == Motion::Delegate, QStringLiteral("Task delegates"));
    check(motionForEvent(toolUse("mcp__forkmesh__read_file")) == Motion::Mcp,
          QStringLiteral("an MCP call is its own kind"));
    check(motionForEvent(toolUse("SomeBrandNewTool")) != Motion::Rest,
          QStringLiteral("an unknown tool still sounds"));
    check(motionForEvent(parse(
              R"({"type":"assistant","message":{"content":[{"type":"text","text":"hello"}]}})"))
              == Motion::Speak,
          QStringLiteral("prose speaks"));
    check(motionForEvent(parse(
              R"({"type":"assistant","message":{"content":[{"type":"thinking","thinking":"hm"}]}})"))
              == Motion::Think,
          QStringLiteral("reasoning thinks"));
    check(motionForEvent(parse(
              R"({"type":"user","message":{"content":[{"type":"tool_result","content":"x"}]}})"))
              == Motion::Arrive,
          QStringLiteral("a tool result arrives"));
    check(motionForEvent(parse(R"({"type":"_codex_agent_complete","text":"done"})"))
              == Motion::Speak,
          QStringLiteral("codex prose speaks"));
    check(motionForEvent(parse(R"({"type":"_codex_reasoning_complete","text":"hm"})"))
              == Motion::Think,
          QStringLiteral("codex reasoning thinks"));
    check(motionForEvent(parse(R"({"type":"_codex_usage","tokens":10})")) == Motion::Usage,
          QStringLiteral("usage bookkeeping is the meter: everything in the log is heard"));
    check(motionForEvent(parse(R"({"type":"rate_limit_event"})")) == Motion::Usage,
          QStringLiteral("a rate limit report is the meter"));
    check(motionForEvent(parse(R"({"type":"stream_event","event":{"type":"message_delta"}})"))
              == Motion::Usage,
          QStringLiteral("the closing message delta carries the usage"));
    check(motionForEvent(parse(R"({"type":"stream_event","event":{"type":"content_block_delta",)"
                               R"("delta":{"type":"input_json_delta","partial_json":"{"}}})"))
              == Motion::Compose,
          QStringLiteral("a tool's input streaming in is a tool call being written"));
    check(motionForEvent(toolUse("WebFetch")) == Motion::Web
              && motionForEvent(toolUse("WebSearch")) == Motion::Web,
          QStringLiteral("the web is its own kind, not reading or searching"));
    check(motionForEvent(toolUse("TodoWrite")) == Motion::Todo
              && motionForEvent(toolUse("TaskCreate")) == Motion::Todo
              && motionForEvent(toolUse("ExitPlanMode")) == Motion::Plan,
          QStringLiteral("a todo list is not a plan"));
    for (const Motion motion : kMotions) {
        check(isBookkeeping(motion) == (motion == Motion::Frame || motion == Motion::Compose
                                        || motion == Motion::Usage || motion == Motion::System
                                        || motion == Motion::Tick || motion == Motion::Close
                                        || motion == Motion::Seal || motion == Motion::Hook),
              QStringLiteral("bookkeeping is exactly the log's eight quiet kinds"));
    }
    check(motionForEvent(parse(R"({"type":"stream_event","event":{"type":"content_block_delta",)"
                               R"("delta":{"type":"text_delta","text":"a"}}})")) == Motion::Speak,
          QStringLiteral("a text delta speaks"));
    check(motionForEvent(parse(R"({"type":"stream_event","event":{"type":"message_start"}})"))
              == Motion::Frame,
          QStringLiteral("stream framing is framing"));
    check(motionForEvent(QJsonObject()) == Motion::Rest,
          QStringLiteral("only a non-event is a rest"));
    const Voice voice = probeVoice();
    Melody melody;
    const Note tick = noteForEvent(parse(R"({"type":"_codex_rate_limits"})"), voice, &melody, 0.0);
    check(!tick.isRest() && tick.gain < 0.1,
          QStringLiteral("bookkeeping is a quiet tick, not silence"));
    check(melody.lastSlot < 0 && melody.degree == 0,
          QStringLiteral("and a tick never moves the melody"));
}

static void testPayload()
{
    const int small = payloadBytes(parse(R"({"type":"user","message":{"content":"ok"}})"));
    QJsonObject big = parse(R"({"type":"user","message":{"content":""}})");
    big.insert(QStringLiteral("blob"), QString(50000, QLatin1Char('x')));
    const int large = payloadBytes(big);
    check(large > small, QStringLiteral("a bigger event weighs more"));
    check(large <= 20000, QStringLiteral("the weight is capped"));
}

static void testGrid()
{
    const Voice voice = probeVoice();
    Melody melody;
    const QJsonObject read = toolUse("Read");

    const Note first = noteForEvent(read, voice, &melody, 0.0);
    check(!first.isRest(), QStringLiteral("the first event sounds"));
    // On the grid, allowing for whichever half of the beat this voice plays on.
    const double grid = first.startSeconds / kSlotSeconds - voice.slotOffset;
    check(std::abs(grid - std::round(grid)) < 1e-6,
          QStringLiteral("notes land on the shared grid"));

    // Two events in the same instant take consecutive slots, not one chord.
    const Note second = noteForEvent(read, voice, &melody, 0.0);
    check(!second.isRest() && second.startSeconds >= first.startSeconds + kSlotSeconds - 1e-9,
          QStringLiteral("one voice plays one note per slot"));

    // A tool that floods the stream must not push the music behind the run:
    // what the melody cannot take is still heard, as quiet grace notes, and
    // only past a whole lead of those is anything let go.
    int graces = 0;
    int dropped = 0;
    double latest = 0;
    for (int i = 0; i < 40; ++i) {
        const Note note = noteForEvent(read, voice, &melody, 0.0);
        if (note.isRest()) {
            ++dropped;
            continue;
        }
        latest = qMax(latest, note.startSeconds);
        if (note.seconds < 0.2)
            ++graces;
    }
    check(graces > 10, QStringLiteral("a flood becomes grace notes, not silence"));
    check(latest <= kMaxLeadSeconds + kSlotSeconds,
          QStringLiteral("and still never runs behind the run"));
    check(dropped < 20, QStringLiteral("so far less of it is lost"));
    check((melody.lastSlot + voice.slotOffset) * kSlotSeconds
              <= kMaxLeadSeconds + kSlotSeconds,
          QStringLiteral("the melody stays near the run"));

    // Deltas colour the texture but never queue behind a busy voice.
    Melody busy;
    busy.lastSlot = 6;
    const QJsonObject delta =
        parse(R"({"type":"stream_event","event":{"type":"content_block_delta",)"
              R"("delta":{"type":"text_delta","text":"a"}}})");
    const Note grace = noteForEvent(delta, voice, &busy, 0.0);
    check(!grace.isRest() && grace.startSeconds <= kSlotSeconds && busy.lastSlot == 6,
          QStringLiteral("a delta never waits in line: it is a grace note now"));
}

static void testDirection()
{
    const Voice voice = probeVoice();
    Melody melody;
    const Note out = noteForEvent(toolUse("Read"), voice, &melody, 0.0);
    const int afterRead = melody.degree;
    check(afterRead > 0, QStringLiteral("work going out climbs"));
    const Note back =
        noteForEvent(parse(R"({"type":"user","message":{"content":)"
                           R"([{"type":"tool_result","content":"x"}]}})"),
                     voice, &melody, 0.4);
    check(melody.degree < afterRead && back.frequency < out.frequency,
          QStringLiteral("data coming back falls again"));

    // A prompt puts the phrase back on the root.
    melody.degree = 6;
    noteForEvent(parse(R"({"type":"_local_user","text":"try again"})"), voice, &melody, 1.0);
    check(melody.degree == 0, QStringLiteral("a new instruction returns to the root"));

    // However long a run goes on, the line stays in its two octaves.
    for (int i = 0; i < 200; ++i) {
        noteForEvent(toolUse("Task"), voice, &melody, i * 2.0);
        check(melody.degree <= 9 && melody.degree >= -2,
              QStringLiteral("the melody stays in range"));
    }
}

// The usual loop - call a tool, get its answer, call again - used to be two
// pitches traded back and forth all afternoon. It is a tune now.
static void testNoSeeSaw()
{
    const Voice voice = probeVoice();
    Melody melody;
    const QJsonObject result =
        parse(R"({"type":"user","message":{"content":[{"type":"tool_result","content":"x"}]}})");
    QSet<int> pitches;
    double previous = 0;
    int repeats = 0;
    for (int i = 0; i < 40; ++i) {
        const Note note = noteForEvent(i % 2 ? result : toolUse("Read"), voice, &melody, i * 1.0);
        check(!note.isRest(), QStringLiteral("every step of the loop sounds"));
        pitches.insert(qRound(note.frequency));
        if (qFuzzyCompare(note.frequency, previous))
            ++repeats;
        previous = note.frequency;
    }
    check(pitches.size() >= 5,
          QStringLiteral("a read/answer loop walks, not see-saws (%1 pitches)").arg(pitches.size()));
    check(repeats == 0, QStringLiteral("and never strikes one pitch twice running"));

    // Notices hold the line level; twice running they still step off it.
    Melody level;
    double last = 0;
    int same = 0;
    for (int i = 0; i < 30; ++i) {
        const Note note =
            noteForEvent(parse(R"({"type":"_local_notice","text":"x"})"), voice, &level, i * 1.0);
        if (qFuzzyCompare(note.frequency, last))
            ++same;
        last = note.frequency;
    }
    check(same == 0, QStringLiteral("no melodic note repeats the one before it"));
}

// A burst of bookkeeping is a sparkle that dies away, not one pitch drilled.
static void testTicksVary()
{
    const Voice voice = probeVoice();
    Melody melody;
    const QJsonObject framing = parse(R"({"type":"keep_alive"})");
    QSet<int> pitches;
    double first = 0;
    double latest = 0;
    for (int i = 0; i < 16; ++i) {
        const Note note = noteForEvent(framing, voice, &melody, 0.0);
        if (note.isRest())
            continue;
        pitches.insert(qRound(note.frequency));
        if (first <= 0)
            first = note.gain;
        latest = note.gain;
    }
    check(pitches.size() >= 4, QStringLiteral("ticks shimmer over several pitches"));
    check(latest < first, QStringLiteral("and a flurry of them thins out"));
}

// The log's quiet kinds are tellable apart by ear: framing clicks on wood,
// a tool call types upwards on glass, usage taps under the line, the
// machinery rings a small bell above it.
static void testBookkeepingKindsSoundApart()
{
    const Voice voice = probeVoice();
    const auto first = [&voice](const char *json) {
        Melody melody;
        return noteForEvent(parse(json), voice, &melody, 0.0);
    };
    const Note frame = first(R"({"type":"stream_event","event":{"type":"content_block_start"}})");
    const Note compose = first(R"({"type":"stream_event","event":{"type":"content_block_delta",)"
                               R"("delta":{"type":"input_json_delta","partial_json":"{"}}})");
    const Note usage = first(R"({"type":"_codex_usage","tokens":10})");
    const Note system = first(R"({"type":"system","subtype":"status"})");
    const Note other = first(R"({"type":"keep_alive"})");
    const double line = degreeFrequency(0, voice);
    check(!frame.isRest() && !compose.isRest() && !usage.isRest() && !system.isRest()
              && !other.isRest(),
          QStringLiteral("every quiet kind sounds"));
    check(frame.timbre == Timbre::Wood && compose.timbre == Timbre::Glass
              && usage.timbre == Timbre::Mallet && system.timbre == Timbre::Bell,
          QStringLiteral("each quiet kind has its own timbre"));
    check(usage.frequency < line && system.frequency > line && frame.frequency > line,
          QStringLiteral("usage sits under the line, framing and the machinery above it"));
    check(frame.gain < 0.1 && compose.gain < 0.1 && usage.gain < 0.1 && system.gain < 0.1,
          QStringLiteral("and all of them stay quiet"));

    // A tool call's arguments streaming in climb, a tap a step up each time.
    Melody melody;
    const QJsonObject typing = parse(
        R"({"type":"stream_event","event":{"type":"content_block_delta",)"
        R"("delta":{"type":"input_json_delta","partial_json":"a"}}})");
    double last = 0;
    int climbs = 0;
    for (int i = 0; i < 4; ++i) {
        const Note note = noteForEvent(typing, voice, &melody, 0.0);
        if (!note.isRest() && note.frequency > last)
            ++climbs;
        last = note.frequency;
    }
    check(climbs == 4, QStringLiteral("a tool call types upwards"));

    // Any kind can be switched off in the mix, and back on.
    AgentSonifier sonifier;
    check(!sonifier.isMuted(Motion::Frame), QStringLiteral("everything sounds by default"));
    sonifier.setMuted(Motion::Frame, true);
    check(sonifier.isMuted(Motion::Frame) && !sonifier.isMuted(Motion::Compose),
          QStringLiteral("one kind is silenced on its own"));
    sonifier.setMuted(Motion::Frame, false);
    check(!sonifier.isMuted(Motion::Frame), QStringLiteral("and heard again"));
}

// Sampled a few times a second, the network used to tick at the sampler's
// rate on one pitch. It sounds at most every kNetworkSpacing now, per way.
static void testNetworkIsSpaced()
{
    const Voice voice = probeVoice();
    Melody melody;
    int sent = 0;
    for (int i = 0; i < 15; ++i) {
        if (!networkNote(voice, &melody, i * 0.4, true, 4096).isRest())
            ++sent;
    }
    check(sent >= 2 && sent <= int(6.0 / kNetworkSpacing) + 1,
          QStringLiteral("network notes are spaced out (%1 in 6 s)").arg(sent));
    const Note small = networkNote(voice, &melody, 100.0, false, 100);
    const Note big = networkNote(voice, &melody, 200.0, false, 4000000);
    check(!small.isRest() && !big.isRest() && !qFuzzyCompare(small.frequency, big.frequency),
          QStringLiteral("a trickle and a download are different notes"));

    // A packet sounds as it leaves, and again as the one coming in lands.
    Melody fresh;
    const Note out = networkNote(voice, &fresh, 300.0, true, 8192);
    const Note in = networkNote(voice, &fresh, 300.0, false, 8192);
    check(!out.isRest() && out.timbre == Timbre::Chirp && out.startSeconds < 300.5,
          QStringLiteral("a sent packet chirps as it leaves"));
    check(!in.isRest() && in.timbre == Timbre::Ping
              && in.startSeconds >= 300.0 + kPacketFlightSeconds,
          QStringLiteral("a received packet pings as it lands"));
}

static void testCadence()
{
    const Voice voice = probeVoice();
    const QVector<Note> good = doneCadence(voice, true, 10.0);
    const QVector<Note> bad = doneCadence(voice, false, 10.0);
    check(good.size() >= 5 && bad.size() >= 5, QStringLiteral("the cadence is a figure"));

    const double root = degreeFrequency(0, voice);
    auto onRoot = [&](const QVector<Note> &notes) {
        int hits = 0;
        for (const Note &note : notes) {
            const double octaves = std::log2(note.frequency / root);
            if (std::abs(octaves - std::round(octaves)) < 0.01)
                ++hits;
        }
        return hits;
    };
    check(onRoot(good) >= 2, QStringLiteral("a finish lands twice on the root"));
    check(onRoot(bad) >= 2, QStringLiteral("a failure also lands on the root"));
    check(good.first().frequency < good[3].frequency,
          QStringLiteral("a finish rises"));
    check(bad.first().frequency > bad[3].frequency, QStringLiteral("a failure falls"));

    // It has to be heard over whatever the stream is playing.
    Melody melody;
    const Note stream = noteForEvent(toolUse("Read"), voice, &melody, 0.0);
    check(good.first().gain > stream.gain, QStringLiteral("done is louder than work"));
    double last = 0;
    for (const Note &note : good)
        last = std::max(last, note.startSeconds + note.seconds);
    check(last - 10.0 > 1.5, QStringLiteral("done rings on"));
}

static void testMixer()
{
    Mixer mixer;
    std::vector<qint16> buffer(size_t(4410) * kChannels);

    mixer.render(buffer.data(), 4410);
    bool silent = true;
    for (qint16 s : buffer)
        silent = silent && s == 0;
    check(silent, QStringLiteral("an empty mixer is silent"));
    check(mixer.idle(), QStringLiteral("an empty mixer is idle"));
    check(std::abs(mixer.now() - 0.1) < 1e-6, QStringLiteral("the clock follows the render"));

    Note note;
    note.startSeconds = mixer.now();
    note.seconds = 0.3;
    note.frequency = 440;
    note.gain = 0.25;
    note.timbre = Timbre::Bell;
    mixer.addNote(note);
    check(mixer.pendingNotes() == 1, QStringLiteral("a scheduled note is pending"));

    mixer.render(buffer.data(), 4410);
    qint16 peak = 0;
    for (qint16 s : buffer)
        peak = std::max<qint16>(peak, qint16(std::abs(int(s))));
    check(peak > 200, QStringLiteral("a scheduled note is audible"));

    // Ten loud notes at once still cannot clip the output.
    for (int i = 0; i < 10; ++i) {
        Note loud = note;
        loud.startSeconds = mixer.now();
        loud.gain = 0.9;
        loud.frequency = 220 + 40 * i;
        loud.pan = i / 9.0;
        mixer.addNote(loud);
    }
    for (int i = 0; i < 4; ++i) {
        mixer.render(buffer.data(), 4410);
        for (qint16 s : buffer)
            check(s > -32760 && s < 32760, QStringLiteral("the mix never clips"));
    }

    // Notes are forgotten once they have finished ringing.
    for (int i = 0; i < 60; ++i)
        mixer.render(buffer.data(), 4410);
    check(mixer.idle(), QStringLiteral("finished notes are dropped"));
}

static void testSonifier()
{
    qputenv("FORKMESH_SONIFY_BACKEND", "silent");
    AgentSonifier sonifier;
    check(sonifier.listen(7, QStringLiteral("first agent")),
          QStringLiteral("listening starts"));
    check(sonifier.isListening(7) && sonifier.listenerCount() == 1,
          QStringLiteral("the session is in the mix"));
    check(sonifier.backendName() == QLatin1String("silent"),
          QStringLiteral("the silent backend is used headless"));
    check(sonifier.pendingNotes() > 0, QStringLiteral("switching on is audible"));

    // A second agent takes the next seat; both play into one mixer.
    check(sonifier.listen(9, QStringLiteral("second agent")),
          QStringLiteral("a second agent joins"));
    const Voice first = sonifier.voiceFor(7);
    const Voice second = sonifier.voiceFor(9);
    check(first.instrument != second.instrument || first.octave != second.octave,
          QStringLiteral("two agents heard together never share an instrument "
                         "and a register"));
    check(!sonifier.voiceDescriptionFor(7).isEmpty()
              && sonifier.voiceDescriptionFor(11).isEmpty(),
          QStringLiteral("a listened agent can say what it sounds like"));

    // Usage moves a voice while it plays.
    sonifier.updateUsage(7, 40, 90000);
    check(sonifier.voiceFor(7).drift > 0 && sonifier.voiceFor(7).bloom > 0,
          QStringLiteral("usage shapes the voice as the run goes on"));
    sonifier.updateUsage(11, 40, 90000); // not listening: no-op, no crash
    check(sonifier.listenerCount() == 2, QStringLiteral("two agents are heard at once"));

    const int before = sonifier.pendingNotes();
    sonifier.handleEvent(7, toolUse("Bash"));
    sonifier.handleEvent(9, toolUse("Read"));
    check(sonifier.pendingNotes() > before, QStringLiteral("events become notes"));

    // An event for a session nobody is listening to costs nothing.
    const int quiet = sonifier.pendingNotes();
    sonifier.handleEvent(11, toolUse("Bash"));
    check(sonifier.pendingNotes() == quiet, QStringLiteral("unheard sessions stay silent"));

    const int beforeDone = sonifier.pendingNotes();
    sonifier.handleFinished(7, true);
    const int afterDone = sonifier.pendingNotes();
    check(afterDone >= beforeDone + 5, QStringLiteral("finishing rings the cadence"));
    sonifier.handleFinished(7, true);
    check(sonifier.pendingNotes() == afterDone, QStringLiteral("the cadence rings once"));

    // A follow-up turn on the same session plays on, and can end again.
    sonifier.handleEvent(7, toolUse("Read"));
    sonifier.handleFinished(7, false);
    check(sonifier.pendingNotes() > afterDone, QStringLiteral("a second turn ends too"));

    sonifier.stopListening(7);
    check(!sonifier.isListening(7) && sonifier.isListening(9),
          QStringLiteral("one agent can be switched off alone"));
    sonifier.stopAll();
    check(sonifier.listenerCount() == 0, QStringLiteral("everything can be switched off"));
    qunsetenv("FORKMESH_SONIFY_BACKEND");
}

static void testPipeArguments()
{
    for (const QString &program : Player::pipePrograms()) {
        const QStringList args = Player::pipeArgumentsFor(program);
        check(!args.isEmpty(), QStringLiteral("%1 has arguments").arg(program));
        const QString line = args.join(QLatin1Char(' '));
        check(line.contains(QString::number(kSampleRate)),
              QStringLiteral("%1 is told the sample rate").arg(program));
        // Every one of them has to be told the stream is headerless PCM, or it
        // tries to parse it as a sound file and exits.
        check(line.contains(QLatin1String("raw")),
              QStringLiteral("%1 is told the stream is raw").arg(program));
    }
    check(Player::pipeArgumentsFor(QStringLiteral("nonesuch")).isEmpty(),
          QStringLiteral("an unknown player is not fed PCM"));
}

static void testStopDoesNotWaitForThePlayer()
{
#if defined(Q_OS_LINUX)
    QTemporaryDir bin;
    if (!bin.isValid())
        return;
    const QString fake = bin.path() + QStringLiteral("/pw-cat");
    QFile script(fake);
    script.open(QIODevice::WriteOnly);
    script.write("#!/bin/sh\nexec sleep 5\n");
    script.close();
    script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                          QFileDevice::ExeOwner);
    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", bin.path().toUtf8() + ':' + oldPath);
    qputenv("FORKMESH_SONIFY_BACKEND", "pipe");
    Mixer mixer;
    {
        Player player(&mixer);
        check(player.start() && player.backend() == Player::Backend::Pipe,
              QStringLiteral("a pipe player starts on the stub"));
        QElapsedTimer stopping;
        stopping.start();
        player.stop();
        check(stopping.elapsed() < 300,
              QStringLiteral("stop() hands a slow player's exit to its own "
                             "thread instead of freezing the caller"));
        check(!player.isRunning(), QStringLiteral("a stopped player is idle"));
        QElapsedTimer draining;
        draining.start();
        while (draining.elapsed() < 2500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    qunsetenv("FORKMESH_SONIFY_BACKEND");
    qputenv("PATH", oldPath);
#endif
}

static void testUiCues()
{
    const QVector<Cue> cues = allCues();
    check(cues.size() == kCueCount, QStringLiteral("allCues lists every cue"));
    QSet<QString> names;
    for (Cue cue : cues) {
        const QString name = cueName(cue);
        check(!names.contains(name), QStringLiteral("cue name %1 is unique").arg(name));
        names.insert(name);
        const QVector<Note> notes = cueNotes(cue, 10.0);
        check(!notes.isEmpty(), QStringLiteral("%1 makes a sound").arg(name));
        for (const Note &note : notes) {
            check(!note.isRest(), QStringLiteral("%1 has no rests").arg(name));
            check(note.startSeconds >= 10.0 && note.startSeconds < 10.5,
                  QStringLiteral("%1 starts when asked, not on the grid").arg(name));
            // Short: the figure is over (bar its ring) well inside a second.
            check(note.startSeconds + note.seconds < 11.0,
                  QStringLiteral("%1 is short").arg(name));
            check(note.gain > 0 && note.gain <= 0.15,
                  QStringLiteral("%1 stays quiet").arg(name));
            check(qFuzzyCompare(note.pan, 0.5),
                  QStringLiteral("%1 sits in the middle, not in an agent's seat").arg(name));
        }
        check(cueNotes(cue, 0, 0).isEmpty(),
              QStringLiteral("%1 at volume 0 takes no mixer slots").arg(name));
        const QVector<Note> half = cueNotes(cue, 0, 0.5);
        check(half.size() == notes.size() &&
                  std::abs(half.first().gain - notes.first().gain * 0.5) < 1e-9,
              QStringLiteral("%1 follows the volume").arg(name));
    }
    // The click is the commonest sound, so it has to be the quietest thing a
    // button can make.
    check(cueNotes(Cue::Click, 0).first().gain <= cueNotes(Cue::Error, 0).first().gain,
          QStringLiteral("a click is softer than an error"));
    // Typing is the one thing more frequent than clicking.
    check(cueNotes(Cue::Keystroke, 0).first().gain <
                  cueNotes(Cue::Click, 0).first().gain &&
              cueNotes(Cue::Keystroke, 0).first().seconds <
                  cueNotes(Cue::Click, 0).first().seconds,
          QStringLiteral("a keystroke is softer and shorter than a click"));

    // Same grammar as the agents: going out rises, coming back falls.
    auto rises = [](Cue cue) {
        const QVector<Note> n = cueNotes(cue, 0);
        return n.last().frequency > n.first().frequency;
    };
    check(rises(Cue::ToggleOn), QStringLiteral("switching on rises"));
    check(!rises(Cue::ToggleOff), QStringLiteral("switching off falls"));
    check(rises(Cue::Send), QStringLiteral("sending rises"));
    check(!rises(Cue::Arrive), QStringLiteral("an arrival falls"));
    check(rises(Cue::Open), QStringLiteral("opening rises"));
    check(!rises(Cue::Close), QStringLiteral("closing falls"));
    check(!rises(Cue::Error), QStringLiteral("an error falls"));
    check(!rises(Cue::Delete), QStringLiteral("a delete falls"));
    check(rises(Cue::Merge), QStringLiteral("a merge rises"));

    const QVector<Note> merge = cueNotes(Cue::Merge, 0);
    int immediate = 0;
    QVector<const Note *> swish;
    for (const Note &note : merge) {
        if (note.startSeconds == 0 && note.seconds <= 0.05)
            ++immediate;
        if (note.timbre == Timbre::Chirp)
            swish.append(&note);
    }
    check(immediate > 0, QStringLiteral("a merge snaps the moment it is clicked"));
    check(swish.size() >= 6, QStringLiteral("a merge swishes up before it lands"));
    bool buildsUp = true;
    for (int i = 1; i < swish.size(); ++i)
        buildsUp = buildsUp && swish[i]->startSeconds > swish[i - 1]->startSeconds &&
                   swish[i]->frequency > swish[i - 1]->frequency &&
                   swish[i]->gain > swish[i - 1]->gain;
    check(buildsUp, QStringLiteral("the merge swish climbs and grows louder"));
    check(merge.last().startSeconds > swish.last()->startSeconds &&
              merge.last().timbre == Timbre::Bell,
          QStringLiteral("the merge build-up lands on a bell"));

    // Above the agents' registers, so a cue never lands inside a voice.
    const double tonic = degreeFrequency(0, interfaceVoice());
    check(tonic > kTonicHz * 3.5, QStringLiteral("the interface sings above the agents"));
}

static void testCueLabelsAndWeights()
{
    check(cueForLabel(QStringLiteral("Delete"), Cue::Click) == Cue::Delete,
          QStringLiteral("a Delete button deletes"));
    check(cueForLabel(QStringLiteral("  &Remove branch "), Cue::Click) == Cue::Delete,
          QStringLiteral("mnemonics and padding do not hide a remove"));
    check(cueForLabel(QStringLiteral("Uninstall"), Cue::Click) == Cue::Delete,
          QStringLiteral("uninstalling is a delete"));
    check(cueForLabel(QStringLiteral("Delete…"), Cue::Click) == Cue::Click,
          QStringLiteral("a Delete that asks first is still a click"));
    check(cueForLabel(QStringLiteral("Remove..."), Cue::Click) == Cue::Click,
          QStringLiteral("three dots ask first too"));
    check(cueForLabel(QStringLiteral("Send"), Cue::Click) == Cue::Send,
          QStringLiteral("a Send button sends"));
    check(cueForLabel(QStringLiteral("Post comment"), Cue::Click) == Cue::Send,
          QStringLiteral("posting is a send"));
    check(cueForLabel(QStringLiteral("Save"), Cue::Click) == Cue::Click,
          QStringLiteral("everything else keeps the fallback"));
    check(cueForLabel(QStringLiteral("Deleted items"), Cue::Click) == Cue::Click,
          QStringLiteral("the whole first word must match"));
    check(cueForLabel(QString(), Cue::Select) == Cue::Select,
          QStringLiteral("an icon button keeps the fallback"));

    // One gesture, several cues: the weightier is what happened.
    check(cueWeight(Cue::Click) > cueWeight(Cue::Close),
          QStringLiteral("choosing a menu item outweighs the menu closing"));
    check(cueWeight(Cue::Open) > cueWeight(Cue::Click),
          QStringLiteral("a dialog outweighs the click that opened it"));
    check(cueWeight(Cue::Send) > cueWeight(Cue::Click) &&
              cueWeight(Cue::Delete) > cueWeight(Cue::Click),
          QStringLiteral("what a button did outweighs pressing it"));
    check(cueWeight(Cue::Error) > cueWeight(Cue::Success),
          QStringLiteral("an error outweighs everything"));
    for (Cue cue : allCues())
        check(cue == Cue::Keystroke || cueWeight(cue) > cueWeight(Cue::Keystroke),
              QStringLiteral("%1 outweighs a keystroke").arg(cueName(cue)));
    QSet<int> weights;
    for (Cue cue : allCues())
        weights.insert(cueWeight(cue));
    check(weights.size() == kCueCount - 1,
          QStringLiteral("only the two toggles share a weight"));
}

static double peakBetween(Mixer &mixer, double fromSeconds, double toSeconds)
{
    // Render up to `fromSeconds` and discard, then measure up to `toSeconds`.
    std::vector<qint16> buffer(4096 * kChannels);
    auto renderTo = [&](double t, double *peak) {
        while (mixer.now() < t) {
            const int frames = qMin(4096, int(std::ceil((t - mixer.now()) * kSampleRate)));
            mixer.render(buffer.data(), qMax(1, frames));
            if (peak)
                for (int i = 0; i < qMax(1, frames) * kChannels; ++i)
                    *peak = std::max(*peak, std::abs(double(buffer[size_t(i)])));
        }
    };
    renderTo(fromSeconds, nullptr);
    double peak = 0;
    renderTo(toSeconds, &peak);
    return peak;
}

static void testDryMixer()
{
    // One click, then listen to the gap after it has finished ringing. The
    // agents' mixer echoes it back at 0.45s; the interface's must not.
    const QVector<Note> click = cueNotes(Cue::Click, 0.0);
    double end = 0;
    for (const Note &note : click)
        end = std::max(end, note.startSeconds + note.seconds + 0.3);

    Mixer wet;
    wet.addNotes(click);
    check(peakBetween(wet, end, end + 0.4) > 0,
          QStringLiteral("the agents' room echoes a note after it ends"));

    Mixer dry;
    dry.setEchoDepth(0);
    dry.addNotes(click);
    check(peakBetween(dry, 0, end) > 0, QStringLiteral("a dry mixer still plays"));
    check(peakBetween(dry, end, end + 0.4) == 0,
          QStringLiteral("a dry mixer is silent once the note is over"));
}

static QJsonObject shell(const char *command, bool background = false)
{
    QJsonObject input;
    input.insert(QStringLiteral("command"), QString::fromUtf8(command));
    if (background)
        input.insert(QStringLiteral("run_in_background"), true);
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("tool_use"));
    block.insert(QStringLiteral("name"), QStringLiteral("Bash"));
    block.insert(QStringLiteral("input"), input);
    QJsonObject message;
    message.insert(QStringLiteral("content"), QJsonArray{block});
    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("assistant"));
    event.insert(QStringLiteral("message"), message);
    return event;
}

static std::vector<qint16> rendered(const Note &note)
{
    Mixer mixer;
    mixer.setEchoDepth(0);
    Note placed = note;
    placed.startSeconds = 0;
    mixer.addNote(placed);
    std::vector<qint16> samples(size_t(8820) * kChannels);
    mixer.render(samples.data(), 8820);
    return samples;
}

static double difference(const std::vector<qint16> &a, const std::vector<qint16> &b)
{
    double diff = 0;
    double energy = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff += std::abs(double(a[i]) - double(b[i]));
        energy += std::abs(double(a[i]));
    }
    return energy > 0 ? diff / energy : 0;
}

static void testMoreKinds()
{
    check(motionForEvent(shell("pytest -q tests/")) == Motion::Test, QStringLiteral("pytest tests"));
    check(motionForEvent(shell("cd desktop && ninja -C build")) == Motion::Build,
          QStringLiteral("ninja builds"));
    check(motionForEvent(shell("git log --oneline -5")) == Motion::Git
              && motionForEvent(shell("cd x && gh pr view 3")) == Motion::Git,
          QStringLiteral("git and gh are git"));
    check(motionForEvent(shell("npm install left-pad")) == Motion::Install,
          QStringLiteral("a package manager installs"));
    check(motionForEvent(shell("ls -la", /*background=*/true)) == Motion::Spawn,
          QStringLiteral("a backgrounded command spawns"));
    check(motionForEvent(shell("sleep 5")) == Motion::Wait, QStringLiteral("sleep waits"));
    check(motionForEvent(shell("cat tsconfig.json")) == Motion::Run,
          QStringLiteral("a word that merely contains tsc is not a build"));
    check(motionForEvent(toolUse("Glob")) == Motion::Scan, QStringLiteral("Glob scans"));
    check(motionForEvent(toolUse("Write")) == Motion::Create
              && motionForEvent(toolUse("NotebookEdit")) == Motion::Write,
          QStringLiteral("a new file is created, an edit is written"));
    check(motionForEvent(toolUse("AskUserQuestion")) == Motion::Ask, QStringLiteral("asking"));
    check(motionForEvent(toolUse("Skill")) == Motion::Skill, QStringLiteral("a skill"));
    check(motionForEvent(toolUse("TaskOutput")) == Motion::Wait
              && motionForEvent(toolUse("Monitor")) == Motion::Wait,
          QStringLiteral("watching background work is waiting"));
    check(motionForEvent(parse(R"({"type":"user","message":{"content":[{"type":"tool_result",)"
                               R"("is_error":true,"content":"exit 1"}]}})"))
              == Motion::Fumble,
          QStringLiteral("an error back is a fumble"));
    check(motionForEvent(parse(R"({"type":"user","message":{"content":[{"type":"tool_result",)"
                               R"("is_error":true,"content":"Permission to use Bash was denied"}]}})"))
              == Motion::Denied,
          QStringLiteral("a refusal is denied"));
    check(motionForEvent(parse(R"({"type":"user","message":{"content":[{"type":"tool_result",)"
                               R"("content":[{"type":"image","source":{}}]}]}})"))
              == Motion::Image,
          QStringLiteral("a picture back is an image"));
    check(motionForEvent(parse(R"({"type":"system","subtype":"compact_boundary"})"))
              == Motion::Compact,
          QStringLiteral("compaction"));
    check(motionForEvent(parse(R"({"type":"system","subtype":"api_retry"})")) == Motion::Retry,
          QStringLiteral("a retry"));
    check(motionForEvent(parse(R"({"type":"result","subtype":"error_max_turns","is_error":true})"))
              == Motion::Fail,
          QStringLiteral("a turn that ends in an error fails"));
    check(motionForEvent(parse(R"({"type":"stream_event","event":{"type":"content_block_stop"}})"))
              == Motion::Close,
          QStringLiteral("a block closing is its own click"));
    check(motionForEvent(parse(R"({"type":"stream_event","event":{"type":"content_block_delta",)"
                               R"("delta":{"type":"signature_delta","signature":"x"}}})"))
              == Motion::Seal,
          QStringLiteral("a signature seals"));
    check(kMotionCount >= 40, QStringLiteral("twice the kinds there were"));

    // Every new timbre is its own sound.
    Note base;
    base.frequency = 440;
    base.seconds = 0.4;
    base.gain = 0.3;
    const Timbre timbres[] = {Timbre::Mallet, Timbre::Bell, Timbre::Glass, Timbre::Wood,
                              Timbre::Pad, Timbre::Chirp, Timbre::Thud, Timbre::Trill,
                              Timbre::Buzz, Timbre::Swell, Timbre::Ping, Timbre::Twang};
    for (const Timbre a : timbres) {
        for (const Timbre b : timbres) {
            // The five the piece started with are already told apart
            // elsewhere; every new one must stand clear of all the rest.
            if (int(a) >= int(b) || int(b) < int(Timbre::Chirp))
                continue;
            Note na = base;
            na.timbre = a;
            Note nb = base;
            nb.timbre = b;
            check(difference(rendered(na), rendered(nb)) > 0.15,
                  QStringLiteral("timbres %1 and %2 sound different").arg(int(a)).arg(int(b)));
        }
    }

    // The event's own data shades its note: the same command the same way,
    // a different one differently.
    const quint32 one = eventSeed(shell("ls src"));
    check(one != 0 && one == eventSeed(shell("ls src")),
          QStringLiteral("the same event hashes the same"));
    check(one != eventSeed(shell("ls tests")), QStringLiteral("a different one does not"));
    const Voice voice = probeVoice();
    Melody m1;
    Melody m2;
    const Note a = noteForEvent(shell("ls src"), voice, &m1, 0.0);
    const Note b = noteForEvent(shell("ls tests"), voice, &m2, 0.0);
    check(a.seed == one && b.seed != one, QStringLiteral("the note carries the seed"));
    Note plain = a;
    plain.seed = 0;
    Note shaded = plain;
    shaded.seed = 0x5a3c91e7u;
    check(difference(rendered(plain), rendered(shaded)) > 0.01,
          QStringLiteral("a seed changes the sound"));
}

// Everything the log and the network say outside a transcript has a kind and
// a sound: every badge on the Logs page, a new host, background work.
static void testCuesCoverTheLog()
{
    const char *badges[] = {"SESSION", "STATUS", "PEER", "NODE", "FORKED", "MIRROR",
                            "SYNC", "ACCOUNT", "ACTIONS", "PIN", "GIT", "BGTASK",
                            "BGBLOCK", "BGNOTE", "PUBLISH", "PULL", "MERGE", "ISSUE",
                            "PROMPT", "BOUNTY", "CRYPTO", "IDENTITY", "ADMIN", "NETWORK",
                            "CLOUD", "DB", "APP", "CRON", "WEB", "DAEMON", "IDS",
                            "CERT", "OPS", "JOB", "MAIL", "SYSTEM", "DEPLOY", "STALL", "ERROR", "INFO"};
    QSet<int> families;
    for (const char *badge : badges) {
        const Motion motion = motionForLogBadge(QString::fromLatin1(badge));
        check(isLog(motion) && isCue(motion),
              QStringLiteral("badge %1 sounds as a log kind").arg(QLatin1String(badge)));
        families.insert(int(motion));
    }
    check(families.size() == 8, QStringLiteral("every log family is some badge's"));
    check(motionForLogBadge(QStringLiteral("error")) == Motion::LogError,
          QStringLiteral("badges match in any case"));
    check(motionForLogBadge(QStringLiteral("SOMETHINGNEW")) == Motion::LogInfo,
          QStringLiteral("an unknown badge is info, not silence"));
    check(isCue(Motion::Connect) && isCue(Motion::Task) && !isCue(Motion::Network)
              && !isCue(Motion::Run),
          QStringLiteral("cues are the log, a new host and background work"));
    check(isCue(Motion::Visitor) && isCue(Motion::Client) && isCue(Motion::Socket)
              && !isLog(Motion::Visitor) && !isLog(Motion::Socket),
          QStringLiteral("the relay's clients and frames are cues, not log lines"));
    {
        Melody in, out;
        check(cueNote(Motion::Socket, desktopVoice(), &in, 1.0, 0x11u).frequency
                  > cueNote(Motion::Socket, desktopVoice(), &out, 1.0, 0x10u).frequency,
              QStringLiteral("a frame received ticks higher than one sent"));
    }

    // Each kind is heard, quietly, as a grace note - and never moves a melody.
    const Voice voice = desktopVoice();
    QSet<int> timbres;
    double errorGain = 0;
    double infoGain = 1;
    for (const Motion motion : kMotions) {
        if (!isCue(motion))
            continue;
        Melody melody;
        const Note note = cueNote(motion, voice, &melody, 1.0, 0x1234u);
        check(!note.isRest() && note.gain < 0.15,
              QStringLiteral("cue %1 is a quiet note").arg(int(motion)));
        check(melody.lastSlot < 0 && melody.degree == 0,
              QStringLiteral("cue %1 leaves the melody where it was").arg(int(motion)));
        timbres.insert(int(note.timbre));
        if (motion == Motion::LogError)
            errorGain = note.gain;
        if (motion == Motion::LogInfo)
            infoGain = note.gain;
    }
    check(timbres.size() >= 7, QStringLiteral("the log's kinds sound apart"));
    check(errorGain > 2 * infoGain, QStringLiteral("an error carries, info whispers"));
    Melody melody;
    check(cueNote(Motion::Run, voice, &melody, 1.0, 1).isRest(),
          QStringLiteral("transcript work is not a cue"));
    Melody a, b;
    check(cueNote(Motion::LogSync, voice, &a, 1.0, 0).frequency
              != cueNote(Motion::LogSync, voice, &b, 1.0, 1).frequency,
          QStringLiteral("what a line says shades its note"));

    // The desktop voice: the log, background work and the desktop's traffic.
    qputenv("FORKMESH_SONIFY_BACKEND", "silent");
    {
        AgentSonifier sonifier;
        QList<int> owners;
        QList<Motion> kinds;
        sonifier.setNoteObserver([&](int sessionId, Motion motion, const Note &, double) {
            owners.append(sessionId);
            kinds.append(motion);
        });
        check(!sonifier.hearsCue(AgentSonifier::kDesktop, Motion::LogError)
                  && !sonifier.handleCue(AgentSonifier::kDesktop, Motion::LogError, 1),
              QStringLiteral("nothing plays the log until it is asked for"));
        check(sonifier.listenDesktop() && sonifier.isListeningDesktop(),
              QStringLiteral("the desktop's voice switches on"));
        check(sonifier.handleCue(AgentSonifier::kDesktop, Motion::LogError, 1)
                  && owners.last() == AgentSonifier::kDesktop
                  && kinds.last() == Motion::LogError,
              QStringLiteral("a log line plays on the desktop's voice"));
        sonifier.listen(7, QStringLiteral("agent"));
        check(sonifier.handleCue(7, Motion::LogGit, 2) && owners.last() == 7,
              QStringLiteral("a line about a heard agent plays in its voice"));
        check(sonifier.handleCue(8, Motion::LogGit, 3)
                  && owners.last() == AgentSonifier::kDesktop,
              QStringLiteral("a line about an unheard agent plays on the desktop's"));
        sonifier.setMuted(Motion::LogGit, true);
        check(!sonifier.handleCue(7, Motion::LogGit, 4),
              QStringLiteral("a silenced kind stays silent"));
        check(!sonifier.handleCue(7, Motion::Run, 4),
              QStringLiteral("only cues are cues"));
        const int before = owners.size();
        sonifier.handleNetwork(AgentSonifier::kDesktop, 90000, 400000);
        check(owners.size() > before && owners.last() == AgentSonifier::kDesktop
                  && kinds.last() == Motion::Network,
              QStringLiteral("the desktop's own traffic is heard"));
        // A fresh desktop voice: the traffic above spent its spacing.
        sonifier.stopDesktop();
        sonifier.listenDesktop();
        sonifier.handleNetwork(8, 90000, 0);
        check(owners.last() == 8 && kinds.last() == Motion::Network,
              QStringLiteral("an unheard agent's packets sound while everything is heard"));
        sonifier.stopDesktop();
        check(!sonifier.isListeningDesktop() && sonifier.isListening(7),
              QStringLiteral("the desktop's voice goes without taking the agents"));
        check(!sonifier.handleCue(8, Motion::LogInfo, 5),
              QStringLiteral("and an unheard agent's line is silent again"));
        const int quiet = owners.size();
        sonifier.handleNetwork(9, 90000, 400000);
        check(owners.size() == quiet, QStringLiteral("and so are its packets"));
        sonifier.stopAll();
    }
    qunsetenv("FORKMESH_SONIFY_BACKEND");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testScale();
    testVoices();
    testUsageShapesTheVoice();
    testInstrumentsSoundDifferent();
    testMoreKinds();
    testMotions();
    testPayload();
    testGrid();
    testDirection();
    testNoSeeSaw();
    testTicksVary();
    testBookkeepingKindsSoundApart();
    testNetworkIsSpaced();
    testCadence();
    testMixer();
    testSonifier();
    testPipeArguments();
    testStopDoesNotWaitForThePlayer();
    testUiCues();
    testCueLabelsAndWeights();
    testDryMixer();
    testCuesCoverTheLog();
    if (failures == 0)
        std::cout << "test_agent_sonifier: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
