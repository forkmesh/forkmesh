#include "AgentSonifier.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#ifdef FORKMESH_HAVE_MULTIMEDIA
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#endif

#include <array>
#include <cmath>

namespace forkmesh::sonify {
namespace {

constexpr int kPumpOpening = -1;
constexpr int kPumpFailed = -2;

// A quarter of a million sines a second is too much arithmetic to put on the
// UI thread, which is where the pipe backend renders. One table, linearly
// interpolated, is indistinguishable at these amplitudes and about ten times
// cheaper.
constexpr int kTableSize = 4096;

const std::array<float, kTableSize + 1> &sineTable()
{
    static const std::array<float, kTableSize + 1> table = [] {
        std::array<float, kTableSize + 1> t{};
        for (int i = 0; i <= kTableSize; ++i)
            t[size_t(i)] = float(std::sin(2.0 * M_PI * double(i) / kTableSize));
        return t;
    }();
    return table;
}

// `phase` is in cycles, not radians: one whole number is one period.
inline double osc(double phase)
{
    const std::array<float, kTableSize + 1> &t = sineTable();
    const double wrapped = phase - std::floor(phase);
    const double x = wrapped * kTableSize;
    const int i = int(x);
    const double f = x - i;
    return double(t[size_t(i)]) + (double(t[size_t(i) + 1]) - double(t[size_t(i)])) * f;
}

// D major pentatonic, in semitones above the tonic. Five steps to the octave,
// and no interval in it can sound wrong against another, which is what lets
// several agents play at once without anyone arranging it.
constexpr int kScale[5] = {0, 2, 4, 7, 9};

double releaseSeconds(Timbre timbre)
{
    switch (timbre) {
    case Timbre::Bell:
        return 1.6;
    case Timbre::Pad:
        return 0.45;
    case Timbre::Glass:
        return 0.3;
    case Timbre::Wood:
        return 0.12;
    case Timbre::Chirp:
        return 0.3;
    case Timbre::Thud:
        return 0.1;
    case Timbre::Trill:
        return 0.25;
    case Timbre::Buzz:
        return 0.15;
    case Timbre::Swell:
        return 0.6;
    case Timbre::Ping:
        return 0.2;
    case Timbre::Twang:
        return 0.45;
    case Timbre::Mallet:
        break;
    }
    return 0.25;
}

double noteLife(const Note &note)
{
    return note.seconds + releaseSeconds(note.timbre);
}

inline double attack(double rel, double seconds)
{
    return rel < seconds ? rel / seconds : 1.0;
}

// One partial of an instrument: where it sits above the fundamental, how loud
// it starts and how fast it dies away. The set of them is the instrument.
struct Partial {
    double ratio;
    double amp;
    double decay;
};

struct InstrumentSpec {
    const Partial *partials;
    int count;
    double attackSeconds;
    double vibratoHz;    // 0: none
    double vibratoDepth; // in cycles of the fundamental
};

// Six instruments that stay out of each other's way: two wooden, two metallic,
// one plucked, one blown. Any of them can sound with any other - they share
// one scale - but no two are mistakable for each other.
constexpr Partial kMarimba[] = {{1, 1.0, 5.5}, {3.9, 0.30, 13}, {9.2, 0.10, 22}};
constexpr Partial kCelesta[] = {{1, 1.0, 3.2}, {3.0, 0.40, 5.5}, {5.0, 0.15, 8}};
constexpr Partial kPluck[] = {
    {1, 1.0, 4.5}, {2, 0.55, 7}, {3, 0.30, 9}, {4, 0.18, 12}, {5, 0.10, 15}};
constexpr Partial kKalimba[] = {{1, 1.0, 4.0}, {2, 0.22, 7}, {3.1, 0.12, 9}};
constexpr Partial kChime[] = {
    {1, 1.0, 1.7}, {2.01, 0.50, 2.6}, {2.76, 0.28, 4.0}, {5.41, 0.14, 6.5}};
constexpr Partial kReed[] = {{1, 1.0, 1.0}, {2, 0.28, 1.4}, {3, 0.14, 1.8}, {4, 0.07, 2.2}};

const InstrumentSpec &instrumentSpec(Instrument instrument)
{
    static const InstrumentSpec specs[] = {
        {kMarimba, 3, 0.003, 0, 0},     // Marimba
        {kCelesta, 3, 0.002, 0, 0},     // Celesta
        {kPluck, 5, 0.0015, 0, 0},      // Pluck
        {kKalimba, 3, 0.004, 0, 0},     // Kalimba
        {kChime, 4, 0.002, 0, 0},       // Chime
        {kReed, 4, 0.050, 5.2, 0.004},  // Reed
    };
    return specs[qBound(0, int(instrument), 5)];
}

// How the work being done shapes whichever instrument is playing it: a shell
// command is struck and damped, thinking is breathed in slowly, a tool result
// rings on.
struct Shape {
    double decayScale;
    double attackScale;
    double brightness;        // >1 leans on the upper partials
    double bendDepth = 0;     // >0 strikes sharp and falls, <0 swoops up from flat
    double bendRate = 22;     // how fast the bend settles, per second
    double trillDepth = 0;    // pitch shaken by this fraction of the note...
    double trillHz = 0;       // ...this many times a second
    double tremoloDepth = 0;  // loudness shaken by this much...
    double tremoloHz = 0;     // ...this many times a second
    double swellSeconds = 0;  // >0: breathed in over this long, backwards
};

Shape shapeForTimbre(Timbre timbre)
{
    switch (timbre) {
    case Timbre::Bell:
        return {0.5, 1.0, 1.0};
    case Timbre::Glass:
        return {1.1, 0.7, 1.7};
    case Timbre::Wood:
        return {2.4, 0.6, 0.8, 0.35, 22};
    case Timbre::Pad:
        return {0.35, 12.0, 0.6};
    case Timbre::Chirp:
        return {1.0, 1.0, 1.3, -0.45, 16};
    case Timbre::Thud:
        return {3.2, 0.5, 0.45, 0.9, 30};
    case Timbre::Trill:
        return {0.8, 1.0, 1.1, 0, 22, 0.06, 13.0};
    case Timbre::Buzz:
        return {1.4, 0.6, 1.9, 0.08, 10, 0, 0, 0.55, 31.0};
    case Timbre::Swell:
        return {0.2, 1.0, 0.9, 0, 22, 0, 0, 0, 0, 0.35};
    case Timbre::Ping:
        return {1.9, 0.5, 0.25};
    case Timbre::Twang:
        return {0.9, 0.8, 1.5, 0.12, 5, 0.012, 5.5};
    case Timbre::Mallet:
        break;
    }
    return {1.0, 1.0, 1.0};
}

// One note's waveform at `rel` seconds into it, before gain and panning.
double sampleNote(const Note &note, double rel)
{
    const InstrumentSpec &spec = instrumentSpec(note.instrument);
    Shape shape = shapeForTimbre(note.timbre);
    const double bloom = qBound(0.0, note.bloom, 1.0);
    // The event's own shading: every seed a slightly different instrument.
    double stretch = 0;
    if (note.seed != 0) {
        shape.brightness *= 0.78 + 0.44 * seedUnit(note.seed, 0);
        shape.decayScale *= 0.8 + 0.4 * seedUnit(note.seed, 1);
        shape.bendDepth += (seedUnit(note.seed, 2) - 0.5) * 0.08;
        stretch = (seedUnit(note.seed, 3) - 0.5) * 0.03;
    }

    // The bend is the integral of a falling pitch, not a falling phase.
    double phase = rel * note.frequency;
    if (shape.bendDepth != 0)
        phase = note.frequency
                * (rel + shape.bendDepth * (1.0 - std::exp(-rel * shape.bendRate)) / shape.bendRate);
    if (spec.vibratoDepth > 0)
        phase += spec.vibratoDepth * osc(rel * spec.vibratoHz);
    // Phase moved by a sine: the pitch swings by trillDepth of the note.
    if (shape.trillDepth > 0)
        phase += shape.trillDepth * note.frequency / (2.0 * M_PI * shape.trillHz)
                 * osc(rel * shape.trillHz);

    double v = 0;
    double norm = 0;
    for (int i = 0; i < spec.count; ++i) {
        const Partial &partial = spec.partials[i];
        const double amp = partial.amp * (i == 0 ? 1.0 : std::pow(shape.brightness, i));
        // Work done fills the instrument out: an agent an hour into a job
        // rings longer and carries more of its own harmonics than a new one.
        const double decay = partial.decay * shape.decayScale * (1.0 - 0.25 * bloom);
        // A stretched (or squeezed) partial series: bell-like one way,
        // hollow the other, never out of tune with the fundamental.
        const double ratio = partial.ratio * (1.0 + stretch * i);
        v += amp * osc(phase * ratio) * std::exp(-rel * decay);
        norm += amp;
    }
    if (bloom > 0) {
        // Two partials that only a long-running agent has: an octave and a
        // twelfth, quiet, which reads as warmth rather than as new notes.
        const double extra = 0.16 * bloom;
        v += extra * osc(phase * 2) * std::exp(-rel * 3.0 * shape.decayScale);
        v += extra * 0.5 * osc(phase * 3) * std::exp(-rel * 4.0 * shape.decayScale);
        norm += extra * 1.5;
    }
    // The low register sits around 110-150 Hz, under what laptop speakers and
    // earbuds reproduce: a voice down there was simply not heard. Its octave
    // and twelfth carry it - the ear hears the missing fundamental from them -
    // so a low agent stays low and still reaches the room.
    const double low = qBound(0.0, (260.0 - note.frequency) / 130.0, 1.0);
    if (low > 0) {
        v += 0.55 * low * osc(phase * 2) * std::exp(-rel * 2.6 * shape.decayScale);
        v += 0.30 * low * osc(phase * 3) * std::exp(-rel * 3.4 * shape.decayScale);
        norm += 0.5 * low;
    }
    v /= qMax(1.0, 0.55 * norm); // keep every instrument about as loud as the rest

    double env = attack(rel, spec.attackSeconds * shape.attackScale);
    if (shape.swellSeconds > 0) {
        const double rise = qMin(1.0, rel / shape.swellSeconds);
        env *= rise * rise;
    }
    if (shape.tremoloDepth > 0)
        env *= 1.0 - shape.tremoloDepth * (0.5 + 0.5 * osc(rel * shape.tremoloHz));
    if (rel > note.seconds) {
        const double r = releaseSeconds(note.timbre);
        env *= std::max(0.0, 1.0 - (rel - note.seconds) / r);
    }
    return v * env * 0.75;
}

inline double softClip(double x)
{
    if (x > -0.7 && x < 0.7)
        return x;
    return std::tanh(x);
}

QString eventType(const QJsonObject &event)
{
    return event.value(QStringLiteral("type")).toString();
}

// A tool call to the kind of work it is. Unknown tools count as doing
// something rather than as nothing, so a new CLI's vocabulary still plays. The
// input matters for a shell: a test run and a push are different work.
Motion motionForTool(const QString &rawName, const QJsonObject &input)
{
    const QString name = rawName.toLower();
    const auto has = [&name](const char *needle) { return name.contains(QLatin1String(needle)); };
    if (name.startsWith(QLatin1String("mcp__")))
        return Motion::Mcp;
    if (has("askuser") || name.startsWith(QLatin1String("ask")))
        return Motion::Ask;
    if (has("skill"))
        return Motion::Skill;
    // The agent's own bookkeeping of the work: todo lists, task lists. Before
    // "task" and "write", which TaskCreate and TodoWrite also contain.
    if (has("todo") || name == QLatin1String("taskcreate") || name == QLatin1String("taskupdate")
        || name == QLatin1String("tasklist") || name == QLatin1String("taskget"))
        return Motion::Todo;
    if (has("plan"))
        return Motion::Plan;
    // Watching work already running. Before "task": TaskOutput is waiting.
    if (has("monitor") || has("bashoutput") || has("taskoutput") || has("sleep")
        || has("wait") || has("killshell") || has("taskstop"))
        return Motion::Wait;
    // Out to the web. Before "search" and "fetch", which these also contain.
    if (has("web") || has("browser") || has("url"))
        return Motion::Web;
    if (has("task") || has("agent") || has("workflow"))
        return Motion::Delegate;
    if (has("image") || has("screenshot"))
        return Motion::Image;
    if (has("glob") || name == QLatin1String("ls") || has("list_dir") || has("listdir")
        || has("list_files") || has("tree"))
        return Motion::Scan;
    if (has("grep") || has("search") || has("find"))
        return Motion::Search;
    if (has("edit") || has("patch") || has("apply") || has("notebook"))
        return Motion::Write;
    if (has("write") || has("create"))
        return Motion::Create;
    if (has("read") || has("fetch") || has("view") || has("list"))
        return Motion::Read;
    // A shell: what the command is decides the note.
    const QJsonValue command = input.value(QStringLiteral("command"));
    QString text = command.toString();
    if (command.isArray()) {
        const QJsonArray words = command.toArray();
        // Codex wraps a command in the shell that runs it: read the command.
        text = words.size() == 3 && words.at(1).toString() == QLatin1String("-lc")
                   ? words.at(2).toString()
                   : QString();
        if (text.isEmpty()) {
            for (const QJsonValue &word : words)
                text += word.toString() + QLatin1Char(' ');
        }
    }
    return motionForCommand(text, input.value(QStringLiteral("run_in_background")).toBool());
}

// Deltas are the CLI thinking out loud a token at a time: hundreds a turn.
// They are allowed to colour the texture but never to queue, so the melody
// stays on the events that mean something.
bool isDelta(const QJsonObject &event)
{
    const QString type = eventType(event);
    return type == QLatin1String("stream_event")
           || type == QLatin1String("_codex_agent_delta")
           || type == QLatin1String("_codex_tool_delta");
}

// String bytes anywhere in the value, stopping once the cap is reached: a
// 400 KB tool result must not cost a 400 KB walk on the UI thread.
void countBytes(const QJsonValue &value, int *total, int cap)
{
    if (*total >= cap)
        return;
    switch (value.type()) {
    case QJsonValue::String:
        *total += value.toString().size();
        break;
    case QJsonValue::Double:
        *total += 4;
        break;
    case QJsonValue::Array:
        for (const QJsonValue &v : value.toArray()) {
            countBytes(v, total, cap);
            if (*total >= cap)
                return;
        }
        break;
    case QJsonValue::Object: {
        const QJsonObject obj = value.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            countBytes(it.value(), total, cap);
            if (*total >= cap)
                return;
        }
        break;
    }
    default:
        break;
    }
}

constexpr int kPayloadCap = 20000;

// A stable little number for one note of one voice: which of a motion's steps
// it takes, which ornament a tick gets. Stirred by how many notes the voice has
// played and by the event's own weight, so a run that reads, gets an answer,
// reads again does not ping-pong between the same two pitches forever.
quint32 stir(int count, int bytes)
{
    quint32 h = quint32(count) * 2654435761u ^ quint32(bytes) * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x2545f491u;
    h ^= h >> 13;
    return h;
}

// How far the melody moves, and in which direction, for one kind of work. The
// direction is the meaning - work going out climbs, data coming back settles -
// and the size varies from note to note, so the line is a tune and not a
// see-saw.
int stepForMotion(Motion motion, int bytes, int count = 0)
{
    const quint32 h = stir(count, bytes);
    switch (motion) {
    case Motion::Speak: {
        // Prose wanders rather than climbing, so a long answer sounds like a
        // tune instead of a siren.
        static const int walk[8] = {1, -1, 2, -1, 1, -2, 3, -2};
        return walk[h % 8];
    }
    case Motion::Think: {
        // Under its breath: small steps either way, mostly down.
        static const int murmur[6] = {-1, 1, -2, -1, 2, -1};
        return murmur[h % 6];
    }
    case Motion::Read: {
        static const int steps[4] = {1, 2, 1, 3};
        return steps[h % 4];
    }
    case Motion::Search: {
        static const int steps[4] = {2, 3, 2, 4};
        return steps[h % 4];
    }
    case Motion::Web: {
        // Out past the machine: the biggest leaps of any work.
        static const int steps[4] = {4, 3, 5, 3};
        return steps[h % 4];
    }
    case Motion::Plan: {
        // Ticking off a list: up a step, back one, up again.
        static const int steps[4] = {2, -1, 1, -1};
        return steps[h % 4];
    }
    case Motion::Write: {
        static const int steps[4] = {2, 1, 3, 2};
        return steps[h % 4];
    }
    case Motion::Run: {
        static const int steps[4] = {1, 1, 2, 3};
        return steps[h % 4];
    }
    case Motion::Delegate: {
        static const int steps[3] = {3, 4, 2};
        return steps[h % 3];
    }
    case Motion::Arrive:
        // The bigger the answer, the further it settles.
        return (bytes > 8000 ? -3 : (bytes > 800 ? -2 : -1)) - int(h % 2);
    case Motion::Notice: {
        static const int steps[3] = {0, 1, -1};
        return steps[h % 3];
    }
    case Motion::Scan: {
        static const int steps[4] = {1, 2, 1, 2};
        return steps[h % 4];
    }
    case Motion::Create: {
        // Something new: a confident climb.
        static const int steps[4] = {3, 2, 4, 2};
        return steps[h % 4];
    }
    case Motion::Test: {
        // Back and forth, checking.
        static const int steps[4] = {1, -1, 2, -1};
        return steps[h % 4];
    }
    case Motion::Build: {
        // Churning low: down more than up.
        static const int steps[4] = {-1, 1, -2, 1};
        return steps[h % 4];
    }
    case Motion::Git: {
        // Through history and back.
        static const int steps[4] = {2, -2, 3, -1};
        return steps[h % 4];
    }
    case Motion::Install: {
        // Piling up, a step at a time.
        static const int steps[4] = {1, 1, 1, 2};
        return steps[h % 4];
    }
    case Motion::Spawn: {
        static const int steps[3] = {4, 5, 3};
        return steps[h % 3];
    }
    case Motion::Wait: {
        static const int steps[4] = {0, 1, 0, -1};
        return steps[h % 4];
    }
    case Motion::Ask: {
        // A question goes up at the end.
        static const int steps[2] = {3, 4};
        return steps[h % 2];
    }
    case Motion::Skill: {
        static const int steps[3] = {3, 2, 4};
        return steps[h % 3];
    }
    case Motion::Mcp: {
        static const int steps[3] = {2, 3, 5};
        return steps[h % 3];
    }
    case Motion::Todo: {
        static const int steps[4] = {1, -1, 2, -1};
        return steps[h % 4];
    }
    case Motion::Fumble: {
        static const int steps[2] = {-2, -3};
        return steps[h % 2];
    }
    case Motion::Denied: {
        static const int steps[2] = {-3, -4};
        return steps[h % 2];
    }
    case Motion::Image: {
        static const int steps[3] = {-1, -2, 2};
        return steps[h % 3];
    }
    case Motion::Compact:
        // Folding the whole line down to make room.
        return -4 - int(h % 2);
    case Motion::Retry: {
        static const int steps[3] = {0, 1, 0};
        return steps[h % 3];
    }
    case Motion::Start:
    case Motion::Prompt:
    case Motion::Finish:
    case Motion::Fail:
    case Motion::Close:
    case Motion::Seal:
    case Motion::Hook:
    case Motion::Frame:
    case Motion::Compose:
    case Motion::Usage:
    case Motion::System:
    case Motion::Tick:
    case Motion::Network:
    case Motion::Connect:
    case Motion::Task:
    case Motion::LogError:
    case Motion::LogStall:
    case Motion::LogSync:
    case Motion::LogGit:
    case Motion::LogCloud:
    case Motion::LogTask:
    case Motion::LogAccount:
    case Motion::LogInfo:
    case Motion::Visitor:
    case Motion::Client:
    case Motion::Socket:
    case Motion::Rest:
        break;
    }
    return 0;
}

Timbre timbreForMotion(Motion motion)
{
    switch (motion) {
    case Motion::Think:
        return Timbre::Pad;
    case Motion::Read:
    case Motion::Search:
    case Motion::Web:
    case Motion::Notice:
    case Motion::Compose:
    case Motion::Tick:
    case Motion::Network:
        return Timbre::Glass;
    case Motion::Run:
    case Motion::Frame:
        return Timbre::Wood;
    case Motion::Usage:
    case Motion::Plan:
        return Timbre::Mallet;
    case Motion::System:
    case Motion::Write:
    case Motion::Delegate:
    case Motion::Prompt:
    case Motion::Start:
    case Motion::Finish:
        return Timbre::Bell;
    case Motion::Scan:
    case Motion::Seal:
        return Timbre::Ping;
    case Motion::Create:
    case Motion::Spawn:
        return Timbre::Chirp;
    case Motion::Test:
    case Motion::Retry:
        return Timbre::Trill;
    case Motion::Build:
    case Motion::Install:
        return Timbre::Thud;
    case Motion::Git:
    case Motion::Skill:
    case Motion::Hook:
        return Timbre::Twang;
    case Motion::Wait:
    case Motion::Compact:
        return Timbre::Swell;
    case Motion::Fumble:
    case Motion::Denied:
    case Motion::Fail:
        return Timbre::Buzz;
    case Motion::Mcp:
    case Motion::Image:
        return Timbre::Glass;
    case Motion::Close:
        return Timbre::Wood;
    case Motion::Ask:
        return Timbre::Bell;
    case Motion::Connect:
        return Timbre::Chirp;
    case Motion::Task:
    case Motion::LogTask:
        return Timbre::Wood;
    case Motion::LogError:
        return Timbre::Buzz;
    case Motion::LogStall:
        return Timbre::Swell;
    case Motion::LogSync:
    case Motion::LogInfo:
        return Timbre::Glass;
    case Motion::LogGit:
        return Timbre::Twang;
    case Motion::LogCloud:
        return Timbre::Ping;
    case Motion::LogAccount:
        return Timbre::Bell;
    case Motion::Visitor:
        return Timbre::Chirp;
    case Motion::Client:
        return Timbre::Bell;
    case Motion::Socket:
        return Timbre::Glass;
    case Motion::Todo:
    case Motion::Speak:
    case Motion::Arrive:
    case Motion::Rest:
        break;
    }
    return Timbre::Mallet;
}

double baseGainForMotion(Motion motion)
{
    switch (motion) {
    case Motion::Think:
        return 0.10;
    case Motion::Arrive:
        return 0.15;
    case Motion::Read:
    case Motion::Search:
        return 0.16;
    case Motion::Web:
        return 0.17;
    case Motion::Plan:
        return 0.20;
    case Motion::Speak:
        return 0.19;
    case Motion::Run:
        return 0.18;
    case Motion::Notice:
        return 0.20;
    case Motion::Write:
    case Motion::Delegate:
        return 0.23;
    case Motion::Start:
        return 0.26;
    case Motion::Finish:
        return 0.27;
    case Motion::Prompt:
        return 0.28;
    case Motion::Wait:
        return 0.12;
    case Motion::Scan:
        return 0.15;
    case Motion::Install:
    case Motion::Todo:
    case Motion::Image:
        return 0.18;
    case Motion::Mcp:
    case Motion::Retry:
        return 0.18;
    case Motion::Test:
    case Motion::Spawn:
    case Motion::Fumble:
        return 0.20;
    case Motion::Build:
    case Motion::Git:
    case Motion::Skill:
        return 0.21;
    case Motion::Denied:
    case Motion::Compact:
        return 0.22;
    case Motion::Create:
        return 0.23;
    case Motion::Fail:
        return 0.25;
    case Motion::Ask:
        return 0.27;
    case Motion::Frame:
    case Motion::Compose:
    case Motion::Usage:
    case Motion::System:
    case Motion::Tick:
    case Motion::Close:
    case Motion::Seal:
    case Motion::Hook:
    case Motion::Network:
        return 0.05;
    case Motion::Connect:
    case Motion::Task:
    case Motion::LogError:
    case Motion::LogStall:
    case Motion::LogSync:
    case Motion::LogGit:
    case Motion::LogCloud:
    case Motion::LogTask:
    case Motion::LogAccount:
    case Motion::LogInfo:
    case Motion::Visitor:
    case Motion::Client:
        return 0.06;
    case Motion::Socket:
        return 0.04;
    case Motion::Rest:
        break;
    }
    return 0.18;
}

double secondsForTimbre(Timbre timbre)
{
    switch (timbre) {
    case Timbre::Bell:
        return 0.9;
    case Timbre::Pad:
        return 0.8;
    case Timbre::Glass:
        return 0.4;
    case Timbre::Wood:
        return 0.18;
    case Timbre::Chirp:
        return 0.3;
    case Timbre::Thud:
        return 0.16;
    case Timbre::Trill:
        return 0.45;
    case Timbre::Buzz:
        return 0.3;
    case Timbre::Swell:
        return 0.7;
    case Timbre::Ping:
        return 0.12;
    case Timbre::Twang:
        return 0.5;
    case Timbre::Mallet:
        break;
    }
    return 0.34;
}

} // namespace

Voice voiceForSession(int sessionId, const QString &title, int salt)
{
    // The agent's own number and name pick its voice, so #281 sounds like #281
    // every time you switch it on, this week and next -- and two agents in the
    // same fleet almost never sound alike.
    quint32 hash = quint32(qHash(title.trimmed().toLower(), 0x5bf03635u));
    hash ^= quint32(sessionId) * 2654435761u;
    hash ^= quint32(salt) * 0x9e3779b9u;
    hash ^= hash >> 15;
    hash *= 0x2545f491u;
    hash ^= hash >> 13;

    static const double pans[5] = {0.5, 0.26, 0.74, 0.14, 0.86};
    static const double cents[5] = {0, 5, -5, 9, -9};

    // Each trait is drawn from its own stir of the hash rather than from the
    // next few bits of one: consecutive session numbers otherwise come out
    // looking alike, which is exactly the case this has to get right.
    auto draw = [&hash](quint32 sides) {
        hash ^= hash << 13;
        hash ^= hash >> 17;
        hash ^= hash << 5;
        return hash % sides;
    };

    Voice voice;
    voice.index = sessionId;
    voice.instrument = Instrument(draw(6));
    voice.octave = int(draw(3)); // three registers, tonic upwards
    voice.pan = pans[draw(5)];
    voice.degreeOffset = int(draw(5)); // which degree of the scale it calls home
    voice.slotOffset = draw(2) ? 0.5 : 0.0; // on the beat, or between them
    voice.detune = std::pow(2.0, cents[draw(5)] / 1200.0);
    return voice;
}

void applyUsage(Voice *voice, int turns, qint64 tokens)
{
    if (!voice)
        return;
    // Every few turns the line steps up the scale: a run that has been at it a
    // while sits plainly higher than one that just started, and you can hear a
    // long agent climb over the course of an afternoon. Capped at an octave so
    // it never walks out of its register.
    voice->drift = qBound(0, qMax(0, turns) / 6, 5);
    // Tokens open the instrument out rather than move it: more partials, a
    // longer ring. Log scaled, because the difference between ten thousand and
    // twenty thousand tokens should not be the difference between two sounds.
    const double burned = double(qMax<qint64>(0, tokens));
    voice->bloom = qBound(0.0, std::log1p(burned) / std::log1p(400000.0), 1.0);
}

QString voiceDescription(const Voice &voice)
{
    static const char *instruments[6] = {"marimba", "celesta", "plucked string",
                                         "kalimba", "chime",   "reed"};
    QString where;
    if (voice.pan < 0.2)
        where = QStringLiteral("hard left");
    else if (voice.pan < 0.45)
        where = QStringLiteral("left");
    else if (voice.pan > 0.8)
        where = QStringLiteral("hard right");
    else if (voice.pan > 0.55)
        where = QStringLiteral("right");
    else
        where = QStringLiteral("centre");
    QString register_;
    if (voice.octave == 0)
        register_ = QStringLiteral("low");
    else if (voice.octave == 1)
        register_ = QStringLiteral("middle");
    else
        register_ = QStringLiteral("high");
    return QStringLiteral("%1, %2 register, %3")
        .arg(QString::fromLatin1(instruments[qBound(0, int(voice.instrument), 5)]),
             register_, where);
}

double degreeFrequency(int degree, const Voice &voice)
{
    // A voice is centred on its own degree of the scale, and walks up it as
    // the run takes turns; the melody's own degree is counted from there.
    degree += voice.home();
    // Floored division, so the scale keeps walking below its own tonic.
    int octave = degree / 5;
    int step = degree % 5;
    if (step < 0) {
        step += 5;
        --octave;
    }
    const int semitones = 12 * (voice.octave + octave) + kScale[step];
    return kTonicHz * std::pow(2.0, semitones / 12.0) * voice.detune;
}

Motion motionForCommand(const QString &command, bool background)
{
    if (background)
        return Motion::Spawn;
    const QString c = command.toLower();
    const auto has = [&c](const char *needle) { return c.contains(QLatin1String(needle)); };
    if (has("pytest") || has("ctest") || has("cargo test") || has("go test")
        || has("npm test") || has("npm run test") || has("yarn test") || has("pnpm test")
        || has("flutter test") || has("dart test") || has("jest") || has("vitest")
        || has("playwright") || has("gradlew test") || has("-tests") || has("unittest"))
        return Motion::Test;
    if (has("npm install") || has("npm i ") || has("npm ci") || has("pip install")
        || has("pip3 install") || has("uv add") || has("uv pip") || has("uv sync")
        || has("apt install") || has("apt-get install") || has("brew install")
        || has("cargo add") || has("cargo install") || has("yarn add") || has("yarn install")
        || has("pnpm add") || has("pnpm install") || has("pub get") || has("go get")
        || has("gem install") || has("poetry add") || has("poetry install"))
        return Motion::Install;
    static const QRegularExpression tsc(QStringLiteral("\\btsc\\b"));
    if (has("ninja") || has("cmake") || has("make ") || c.trimmed() == QLatin1String("make")
        || has("cargo build") || has("go build") || has("npm run build") || c.contains(tsc)
        || has("gradlew") || has("flutter build") || has("xcodebuild") || has("webpack")
        || has("vite build") || has("g++ ") || has("gcc ") || has("clang"))
        return Motion::Build;
    // git or gh at the start of any piece of the line.
    static const QRegularExpression git(QStringLiteral("(^|[;&|(]\\s*)(git|gh)\\s"));
    if (c.trimmed().contains(git))
        return Motion::Git;
    static const QRegularExpression sleep(QStringLiteral("(^|[;&|]\\s*)sleep\\s"));
    if (c.trimmed().contains(sleep))
        return Motion::Wait;
    return Motion::Run;
}

Motion motionForEvent(const QJsonObject &event)
{
    const QString type = eventType(event);
    if (type.isEmpty())
        return Motion::Rest; // not an event at all
    if (type == QLatin1String("system")) {
        const QString sub = event.value(QStringLiteral("subtype")).toString();
        if (sub == QLatin1String("init"))
            return Motion::Start;
        if (sub.startsWith(QLatin1String("hook")))
            return Motion::Hook;
        if (sub.contains(QLatin1String("compact"))
            || event.value(QStringLiteral("status")).toString() == QLatin1String("compacting"))
            return Motion::Compact;
        if (sub.contains(QLatin1String("retry")))
            return Motion::Retry;
        // A runtime that answers with an error envelope has done something,
        // and it is the one thing you want to hear across the room.
        return sub == QLatin1String("error") ? Motion::Notice : Motion::System;
    }
    // Token counts and rate limits: the meter, not the work.
    if (type == QLatin1String("rate_limit_event") || type == QLatin1String("_codex_usage")
        || type == QLatin1String("_codex_rate_limits"))
        return Motion::Usage;
    if (type == QLatin1String("_local_user") || type == QLatin1String("_local_ask_answer"))
        return Motion::Prompt;
    if (type == QLatin1String("_local_notice")
        || type == QLatin1String("_runtime_protocol_error"))
        return Motion::Notice;
    if (type == QLatin1String("_codex_agent_delta")
        || type == QLatin1String("_codex_agent_complete"))
        return Motion::Speak;
    if (type == QLatin1String("_codex_reasoning_complete"))
        return Motion::Think;
    if (type == QLatin1String("_codex_tool_delta"))
        return Motion::Arrive;
    if (type == QLatin1String("stream_event")) {
        const QJsonObject inner = event.value(QStringLiteral("event")).toObject();
        const QString innerType = inner.value(QStringLiteral("type")).toString();
        // The closing message_delta carries the turn's usage and stop reason.
        if (innerType == QLatin1String("message_delta"))
            return Motion::Usage;
        if (innerType == QLatin1String("ping"))
            return Motion::Tick;
        if (innerType == QLatin1String("content_block_stop")
            || innerType == QLatin1String("message_stop"))
            return Motion::Close;
        if (innerType != QLatin1String("content_block_delta"))
            return Motion::Frame; // message and block opening
        const QString delta = inner.value(QStringLiteral("delta"))
                                  .toObject()
                                  .value(QStringLiteral("type"))
                                  .toString();
        if (delta == QLatin1String("thinking_delta"))
            return Motion::Think;
        if (delta == QLatin1String("text_delta"))
            return Motion::Speak;
        if (delta == QLatin1String("input_json_delta"))
            return Motion::Compose; // a tool's input arriving
        if (delta == QLatin1String("signature_delta") || delta == QLatin1String("citations_delta"))
            return Motion::Seal; // a thinking block's signature sealing it
        return Motion::Frame;
    }
    if (type == QLatin1String("assistant")) {
        const QJsonArray content = event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        Motion best = Motion::Rest;
        for (const QJsonValue &bv : content) {
            const QJsonObject block = bv.toObject();
            const QString btype = block.value(QStringLiteral("type")).toString();
            if (btype == QLatin1String("tool_use")) {
                // A turn that calls a tool is heard as that tool: it is the
                // part of the turn that does something.
                return motionForTool(block.value(QStringLiteral("name")).toString(),
                                     block.value(QStringLiteral("input")).toObject());
            }
            if (btype == QLatin1String("text")
                && !block.value(QStringLiteral("text")).toString().trimmed().isEmpty())
                best = Motion::Speak;
            else if (btype == QLatin1String("thinking") && best == Motion::Rest)
                best = Motion::Think;
        }
        return best == Motion::Rest ? Motion::Frame : best;
    }
    if (type == QLatin1String("user")) {
        const QJsonArray content = event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue &bv : content) {
            const QJsonObject block = bv.toObject();
            if (block.value(QStringLiteral("type")).toString() != QLatin1String("tool_result"))
                continue;
            // What came back: an error, a refusal, a picture, or an answer.
            const QJsonValue result = block.value(QStringLiteral("content"));
            QString text = result.toString();
            bool image = false;
            for (const QJsonValue &part : result.toArray()) {
                const QJsonObject piece = part.toObject();
                const QString kind = piece.value(QStringLiteral("type")).toString();
                if (kind == QLatin1String("image"))
                    image = true;
                else if (kind == QLatin1String("text") && text.size() < 2000)
                    text += piece.value(QStringLiteral("text")).toString();
            }
            if (block.value(QStringLiteral("is_error")).toBool()) {
                const QString lower = text.left(2000).toLower();
                for (const char *refusal : {"denied", "permission", "rejected", "doesn't want",
                                            "not allowed", "blocked", "refused"}) {
                    if (lower.contains(QLatin1String(refusal)))
                        return Motion::Denied;
                }
                return Motion::Fumble;
            }
            return image ? Motion::Image : Motion::Arrive;
        }
        // A plain user turn inside the stream is the operator steering.
        return content.isEmpty() ? Motion::Frame : Motion::Prompt;
    }
    // The turn is over. A single bell on the root closes the phrase where the
    // run closed it - the full cadence still waits for the session's status,
    // which is the only thing that knows a failure from a finish.
    if (type == QLatin1String("result")) {
        const bool failed = event.value(QStringLiteral("is_error")).toBool()
                            || event.value(QStringLiteral("subtype")).toString()
                                   .startsWith(QLatin1String("error"));
        return failed ? Motion::Fail : Motion::Finish;
    }
    // Heartbeats, control frames, anything of no known kind: not work, but in
    // the log, so heard and seen - as the quietest thing there is.
    return Motion::Tick;
}

namespace {

quint32 fnv(const QString &text, quint32 h = 2166136261u)
{
    for (const QChar c : text) {
        h ^= c.unicode();
        h *= 16777619u;
    }
    return h;
}

QString contentText(const QJsonValue &content, int limit)
{
    if (content.isString())
        return content.toString().left(limit);
    QString text;
    for (const QJsonValue &part : content.toArray()) {
        const QJsonObject piece = part.toObject();
        text += piece.value(QStringLiteral("text")).toString().left(limit);
        text += piece.value(QStringLiteral("type")).toString();
        if (text.size() >= limit)
            break;
    }
    return text.left(limit);
}

} // namespace

quint32 eventSeed(const QJsonObject &event)
{
    const QString type = eventType(event);
    QString key = type + QLatin1Char('|') + event.value(QStringLiteral("subtype")).toString();
    const QJsonObject message = event.value(QStringLiteral("message")).toObject();
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();
    if (type == QLatin1String("assistant") || type == QLatin1String("user")) {
        for (const QJsonValue &bv : content) {
            const QJsonObject block = bv.toObject();
            const QString btype = block.value(QStringLiteral("type")).toString();
            key += QLatin1Char('|') + btype;
            if (btype == QLatin1String("tool_use")) {
                key += block.value(QStringLiteral("name")).toString();
                const QJsonObject input = block.value(QStringLiteral("input")).toObject();
                for (const char *field : {"command", "file_path", "notebook_path", "path",
                                          "pattern", "url", "query", "skill", "prompt",
                                          "description"}) {
                    const QJsonValue value = input.value(QLatin1String(field));
                    key += value.isArray() ? contentText(value, 200)
                                           : value.toString().left(200);
                }
            } else if (btype == QLatin1String("tool_result")) {
                key += contentText(block.value(QStringLiteral("content")), 200);
            } else {
                key += block.value(QStringLiteral("text")).toString().left(200);
                key += block.value(QStringLiteral("thinking")).toString().left(200);
            }
        }
        if (content.isEmpty())
            key += message.value(QStringLiteral("content")).toString().left(200);
    } else if (type == QLatin1String("stream_event")) {
        const QJsonObject inner = event.value(QStringLiteral("event")).toObject();
        const QJsonObject delta = inner.value(QStringLiteral("delta")).toObject();
        key += inner.value(QStringLiteral("type")).toString()
               + delta.value(QStringLiteral("type")).toString()
               + delta.value(QStringLiteral("text")).toString().left(40)
               + delta.value(QStringLiteral("thinking")).toString().left(40)
               + delta.value(QStringLiteral("partial_json")).toString().left(40);
    } else {
        key += event.value(QStringLiteral("text")).toString().left(200)
               + event.value(QStringLiteral("result")).toString().left(200)
               + event.value(QStringLiteral("status")).toString();
    }
    const quint32 h = fnv(key);
    return h == 0 ? 1u : h;
}

int payloadBytes(const QJsonObject &event)
{
    int total = 0;
    countBytes(QJsonValue(event), &total, kPayloadCap);
    return qMin(total, kPayloadCap);
}

namespace {

// A grace note: the quiet one between the beats. Bookkeeping sounds as one,
// and so does work the melody has no slot left for, so nothing in the log goes
// unheard - and, because the picture marks exactly the notes that sound,
// nothing goes unseen either.
Note graceNote(Motion motion, int bytes, const Voice &voice, Melody *melody,
               double nowSeconds)
{
    constexpr double kGraceSeconds = kSlotSeconds / 4;
    double grace = std::ceil(nowSeconds / kGraceSeconds - 1e-9);
    if (grace <= melody->lastGrace)
        grace = melody->lastGrace + 1;
    const double start = grace * kGraceSeconds;
    if (start - nowSeconds > kMaxLeadSeconds)
        return {}; // even the grace notes are a whole lead behind: let it go
    // Back to back, a flurry: each one a little softer than the one before, so
    // a stream of framing events is a shimmer that dies away, not a drill.
    melody->graceRun = grace <= melody->lastGrace + 1.5 ? melody->graceRun + 1 : 0;
    melody->lastGrace = grace;
    melody->graceCount += 1;
    const quint32 h = stir(melody->graceCount, bytes);

    const bool tick = isFaint(motion);
    Note note;
    note.timbre = timbreForMotion(motion);
    note.instrument = voice.instrument;
    note.bloom = voice.bloom;
    note.seconds = tick ? 0.04 + 0.03 * double(h % 3) / 2.0 : 0.1;
    note.startSeconds = start;
    // A tick shimmers somewhere above wherever the line is - a different
    // degree each time, so a burst of them is a sparkle rather than one pitch
    // repeated; a crowded-out note is a glance at where it would have gone.
    // Each kind of bookkeeping has its own figure, so the log's quiet parts
    // are tellable apart by ear.
    static const int shimmer[7] = {5, 7, 4, 8, 6, 9, 3};
    int offset = tick ? shimmer[h % 7] : stepForMotion(motion, bytes, melody->count);
    double gainScale = 1.0;
    switch (motion) {
    case Motion::Frame:
        // A dry click a hair above the line as a message or block opens...
        offset = 7;
        note.seconds = 0.03;
        gainScale = 0.8;
        break;
    case Motion::Close:
        // ...and a lower one as it closes, so the two frame the phrase.
        offset = 5;
        note.seconds = 0.03;
        gainScale = 0.75;
        break;
    case Motion::Seal:
        // A glint at the very top: something sealed.
        offset = 9 + int(h % 2);
        note.seconds = 0.025;
        gainScale = 0.9;
        break;
    case Motion::Hook:
        // A sagging string a little above the line: the machinery answering.
        offset = 2 + int(h % 3);
        note.seconds = 0.1;
        break;
    case Motion::Compose:
        // Typing: a run of taps stepping up while the arguments stream in,
        // folding back down every five.
        offset = 3 + melody->graceRun % 5;
        note.seconds = 0.035;
        break;
    case Motion::Usage:
        // The meter: a low soft tap under the line.
        offset = -3 - int(h % 2);
        note.seconds = 0.07;
        gainScale = 1.1;
        break;
    case Motion::System:
        // The machinery: a small bell a fifth above, held a moment longer.
        offset = 4 + int(h % 2) * 5;
        note.seconds = 0.12;
        break;
    default:
        break;
    }
    note.frequency = degreeFrequency(melody->degree + offset, voice);
    note.pan = qBound(0.0, voice.pan + (double(h % 5) - 2.0) * 0.03, 1.0);
    const double weight = std::log1p(bytes) / std::log1p(kPayloadCap);
    const double thinning = qMax(0.3, 1.0 / (1.0 + 0.3 * melody->graceRun));
    note.gain = (tick ? (0.035 + 0.02 * weight) * gainScale
                      : baseGainForMotion(motion) * (0.3 + 0.12 * weight))
                * thinning;
    return note;
}

} // namespace

Note networkNote(const Voice &voice, Melody *melody, double nowSeconds, bool sent,
                 qint64 bytes)
{
    if (!melody || bytes <= 0)
        return {};
    // Sampled a few times a second, a busy connection would otherwise be one
    // pitch ticking at the sampler's rate. Hold the bytes and sound them at
    // most every kNetworkSpacing seconds, as one note for all of them.
    double &last = sent ? melody->lastSent : melody->lastReceived;
    qint64 &held = sent ? melody->heldSent : melody->heldReceived;
    held += bytes;
    if (nowSeconds - last < kNetworkSpacing)
        return {};
    const qint64 moved = held;
    const int weightBytes = int(qMin<qint64>(moved, kPayloadCap));
    Note note = graceNote(Motion::Network, weightBytes, voice, melody, nowSeconds);
    if (note.isRest())
        return note;
    last = nowSeconds;
    held = 0;
    // Out is up, in is down, as everywhere else in the piece; how far up or
    // down is how much moved, so a trickle and a download are different notes.
    const int level = qBound(0, int(std::log2(1.0 + double(moved) / 512.0)), 12);
    const int lilt = int(stir(melody->graceCount, level) % 2);
    note.frequency = degreeFrequency(
        melody->degree + (sent ? 4 + level / 2 : level / 3) + lilt, voice);
    const double weight = std::log1p(double(moved)) / std::log1p(4.0e6);
    note.gain = 0.04 + 0.05 * qBound(0.0, weight, 1.0);
    note.seconds = sent ? 0.05 : 0.07;
    // A packet going out swoops up and away; one coming in is a small ping as
    // it lands, which is a flight after it left the host.
    note.timbre = sent ? Timbre::Chirp : Timbre::Ping;
    if (!sent)
        note.startSeconds += kPacketFlightSeconds;
    return note;
}

Motion motionForLogBadge(const QString &badge)
{
    struct Family {
        const char *badge;
        Motion motion;
    };
    // The Logs page's badges (LogActivityLights::categories()), by family.
    static const Family kFamilies[] = {
        {"ERROR", Motion::LogError},     {"STALL", Motion::LogStall},
        {"BGBLOCK", Motion::LogStall},   {"MIRROR", Motion::LogSync},
        {"SYNC", Motion::LogSync},       {"PIN", Motion::LogSync},
        {"PEER", Motion::LogSync},       {"NODE", Motion::LogSync},
        {"FORKED", Motion::LogSync},     {"STATUS", Motion::LogSync},
        {"GIT", Motion::LogGit},         {"PULL", Motion::LogGit},
        {"MERGE", Motion::LogGit},       {"PUBLISH", Motion::LogGit},
        {"ISSUE", Motion::LogGit},       {"BOUNTY", Motion::LogGit},
        {"ACTIONS", Motion::LogGit},     {"CLOUD", Motion::LogCloud},
        {"NETWORK", Motion::LogCloud},   {"DB", Motion::LogCloud},
        {"APP", Motion::LogCloud},       {"CRON", Motion::LogCloud},
        {"WEB", Motion::LogCloud},       {"DAEMON", Motion::LogCloud},
        {"IDS", Motion::LogCloud},       {"CERT", Motion::LogCloud},
        {"OPS", Motion::LogCloud},       {"JOB", Motion::LogCloud},
        {"MAIL", Motion::LogCloud},      {"SYSTEM", Motion::LogCloud},
        {"DEPLOY", Motion::LogCloud},
        {"BGTASK", Motion::LogTask},     {"BGNOTE", Motion::LogTask},
        {"SESSION", Motion::LogAccount}, {"ACCOUNT", Motion::LogAccount},
        {"IDENTITY", Motion::LogAccount}, {"CRYPTO", Motion::LogAccount},
        {"ADMIN", Motion::LogAccount},   {"PROMPT", Motion::LogAccount},
    };
    const QString key = badge.trimmed().toUpper();
    for (const Family &family : kFamilies) {
        if (key == QLatin1String(family.badge))
            return family.motion;
    }
    return Motion::LogInfo;
}

Voice desktopVoice()
{
    Voice voice;
    voice.index = AgentSonifier::kDesktop;
    voice.instrument = Instrument::Chime;
    voice.octave = 1;
    voice.pan = 0.5;
    voice.degreeOffset = 0;
    voice.slotOffset = 0.25; // between the agents' beats and off-beats
    voice.detune = 1.0;
    return voice;
}

Note cueNote(Motion motion, const Voice &voice, Melody *melody, double nowSeconds,
             quint32 seed)
{
    if (!melody || !isCue(motion))
        return {};
    Note note = graceNote(motion, 0, voice, melody, nowSeconds);
    if (note.isRest())
        return note;
    const int pick = int(seed % 3u);
    // Where it sits over the line, how long it rings and how loud: an error
    // is the one that falls below the line and carries, the INFO chatter the
    // faintest glint at the top.
    int offset = 0;
    double seconds = 0.1;
    double gain = 0.05;
    switch (motion) {
    case Motion::Connect:
        offset = 6 + pick;
        seconds = 0.14;
        gain = 0.07;
        break;
    case Motion::Task:
        offset = 1 + pick % 2;
        seconds = 0.08;
        gain = 0.05;
        break;
    case Motion::LogError:
        offset = -3 - pick % 2;
        seconds = 0.28;
        gain = 0.11;
        break;
    case Motion::LogStall:
        offset = -2;
        seconds = 0.4;
        gain = 0.08;
        break;
    case Motion::LogSync:
        offset = 3 + pick;
        seconds = 0.12;
        break;
    case Motion::LogGit:
        offset = 2 + pick;
        seconds = 0.2;
        gain = 0.06;
        break;
    case Motion::LogCloud:
        offset = 7 + pick % 2;
        seconds = 0.06;
        break;
    case Motion::LogTask:
        offset = pick % 2;
        seconds = 0.06;
        gain = 0.045;
        break;
    case Motion::LogAccount:
        offset = 5;
        seconds = 0.25;
        break;
    case Motion::LogInfo:
        offset = 8 + pick;
        seconds = 0.04;
        gain = 0.035;
        break;
    case Motion::Visitor:
        // Somebody walking in off the web: a chirp a fourth over the line.
        offset = 4 + pick % 2;
        seconds = 0.18;
        gain = 0.07;
        break;
    case Motion::Client:
        offset = 2;
        seconds = 0.3;
        gain = 0.06;
        break;
    case Motion::Socket:
        // A frame on the relay: the faintest tick, high for one received,
        // a step lower for one sent (the seed's low bit carries which).
        offset = (seed & 1u) ? 9 : 7;
        seconds = 0.03;
        gain = 0.03;
        break;
    default:
        break;
    }
    const double thinning = qMax(0.3, 1.0 / (1.0 + 0.3 * melody->graceRun));
    note.frequency = degreeFrequency(melody->degree + offset, voice);
    note.seconds = seconds;
    note.gain = gain * thinning;
    note.seed = seed;
    return note;
}

Note noteForEvent(const QJsonObject &event, const Voice &voice, Melody *melody,
                  double nowSeconds)
{
    if (!melody)
        return {};
    const Motion motion = motionForEvent(event);
    if (motion == Motion::Rest)
        return {};
    const int bytes = payloadBytes(event);
    const quint32 seed = eventSeed(event);
    if (isBookkeeping(motion)) {
        Note grace = graceNote(motion, bytes, voice, melody, nowSeconds);
        grace.seed = seed;
        return grace;
    }
    const bool ambient = isDelta(event);

    // Quantise: the next free slot on the shared grid, never the same slot
    // this voice just used.
    double slot = std::ceil(nowSeconds / kSlotSeconds - 1e-9);
    const double minSlot = melody->lastSlot + (ambient ? 2 : 1);
    if (slot < minSlot)
        slot = minSlot;
    // A voice either plays on the beat or between the beats; both are in time
    // with everyone else, and two agents working at the same pace interlock
    // instead of landing on top of each other.
    const double start = (slot + voice.slotOffset) * kSlotSeconds;
    const double lead = start - nowSeconds;
    // The run has outrun the melody, or a delta would have to queue for it:
    // it is still heard, as a grace note, and the melody stays near the run.
    if (lead > kMaxLeadSeconds || (ambient && lead > kSlotSeconds)) {
        Note grace = graceNote(motion, bytes, voice, melody, nowSeconds);
        grace.seed = seed;
        return grace;
    }

    melody->count += 1;
    const quint32 h = stir(melody->count, bytes);
    int degree = melody->degree;
    const bool root = motion == Motion::Start || motion == Motion::Prompt
                      || motion == Motion::Finish || motion == Motion::Fail;
    if (root)
        // An instruction opens the phrase on the root; a result lands on it;
        // a failed turn sinks to the fifth below it instead.
        degree = motion == Motion::Fail ? -2 : 0;
    else
        // Which of its steps a kind of work takes is stirred by what the
        // event was about, too: two different files read in a row are two
        // different intervals.
        degree += stepForMotion(motion, bytes, melody->count + int(seed % 11u));
    // Two octaves of room; past either end the line folds back rather than
    // running off into a register nothing else is using.
    if (degree > 9)
        degree -= 5;
    else if (degree < -2)
        degree += 5;
    // The same pitch twice running is where a line starts to sound like an
    // alarm: step off it, whichever way the stir says.
    if (!root && degree == melody->degree && melody->lastSlot >= 0)
        degree += degree >= 9 ? -1 : (degree <= -2 ? 1 : ((h & 1) ? 1 : -1));

    Note note;
    note.timbre = timbreForMotion(motion);
    note.instrument = voice.instrument;
    note.bloom = voice.bloom;
    note.seed = seed;
    // Held a little longer or shorter by what it was, so a phrase breathes.
    note.seconds = secondsForTimbre(note.timbre) * (0.85 + 0.3 * seedUnit(seed, 1));
    note.startSeconds = start;
    note.frequency = degreeFrequency(degree, voice);
    // A drum sits an octave under the line it belongs to.
    if (note.timbre == Timbre::Thud)
        note.frequency *= 0.5;
    note.pan = voice.pan;
    const double weight = std::log1p(bytes) / std::log1p(kPayloadCap);
    // A player never strikes two notes exactly alike.
    const double touch = 0.88 + 0.24 * double(h % 101) / 100.0;
    note.gain = baseGainForMotion(motion) * (0.72 + 0.38 * weight) * (ambient ? 0.55 : 1.0)
                * touch;

    melody->degree = degree;
    melody->lastSlot = slot;
    return note;
}

QVector<Note> doneCadence(const Voice &voice, bool success, double startSeconds)
{
    QVector<Note> notes;
    const double step = success ? 0.18 : 0.22;
    const int rising[4] = {0, 2, 3, 5};
    const int falling[4] = {4, 3, 1, 0};
    for (int i = 0; i < 4; ++i) {
        Note note;
        note.timbre = Timbre::Bell;
        note.instrument = voice.instrument;
        note.bloom = voice.bloom;
        note.startSeconds = startSeconds + i * step;
        note.seconds = 0.5;
        note.frequency = degreeFrequency(success ? rising[i] : falling[i], voice);
        note.pan = voice.pan;
        note.gain = success ? 0.30 : 0.24;
        notes.append(note);
    }
    // The chord under it: for a finish the octave, third and root together, so
    // the root sounds twice and the figure is plainly an ending; for a failure
    // or a stop the bare root and the fifth below it, which settles instead.
    const int chordUp[3] = {5, 3, 0};
    const int chordDown[2] = {0, -2};
    const int chordSize = success ? 3 : 2;
    for (int i = 0; i < chordSize; ++i) {
        Note note;
        note.timbre = Timbre::Bell;
        note.instrument = voice.instrument;
        note.bloom = voice.bloom;
        note.startSeconds = startSeconds + 4 * step;
        note.seconds = 1.8;
        note.frequency = degreeFrequency(success ? chordUp[i] : chordDown[i], voice);
        note.pan = voice.pan;
        note.gain = success ? 0.26 : 0.22;
        notes.append(note);
    }
    return notes;
}

// ---------------------------------------------------------------- mixer ----

bool Mixer::addNote(const Note &note)
{
    if (note.isRest())
        return false;
    QMutexLocker lock(&m_mutex);
    // The render loop is O(notes) per sample and it holds this lock, which the
    // UI thread waits on to add the next one. A dozen busy agents come to
    // perhaps forty notes ringing at once; past this there is nothing musical
    // left to hear anyway, so the ceiling costs nothing and bounds both.
    if (m_notes.size() >= 64)
        return false;
    m_notes.append(note);
    return true;
}

void Mixer::addNotes(const QVector<Note> &notes)
{
    for (const Note &note : notes)
        addNote(note);
}

void Mixer::clear()
{
    QMutexLocker lock(&m_mutex);
    m_notes.clear();
}

void Mixer::setEchoDepth(double depth)
{
    QMutexLocker lock(&m_mutex);
    m_echoDepth = qBound(0.0, depth, 1.0);
}

double Mixer::now() const
{
    QMutexLocker lock(&m_mutex);
    return double(m_frame) / kSampleRate;
}

int Mixer::pendingNotes() const
{
    QMutexLocker lock(&m_mutex);
    const double t = double(m_frame) / kSampleRate;
    int count = 0;
    for (const Note &note : m_notes) {
        if (note.startSeconds + noteLife(note) > t)
            ++count;
    }
    return count;
}

void Mixer::render(qint16 *out, int frames)
{
    if (!out || frames <= 0)
        return;
    QMutexLocker lock(&m_mutex);

    // A short ping-pong echo, a dotted note behind the grid: it is what makes
    // sparse events sound like a room rather than a series of beeps.
    constexpr double kEchoSeconds = 0.45;
    const double echoFeedback = 0.34 * m_echoDepth;
    const double echoWet = 0.26 * m_echoDepth;
    const bool dry = m_echoDepth <= 0;
    const int echoFrames = int(kEchoSeconds * kSampleRate);
    if (dry) {
        // Nothing reads the delay line, so do not keep one: an interface cue is
        // a few dozen milliseconds and pays for every sample it renders.
        m_echoLeft.clear();
        m_echoRight.clear();
        m_echoIndex = 0;
    } else if (m_echoLeft.size() != echoFrames) {
        m_echoLeft.assign(size_t(echoFrames), 0.0f);
        m_echoRight.assign(size_t(echoFrames), 0.0f);
        m_echoIndex = 0;
    }

    for (int i = 0; i < frames; ++i) {
        const double t = double(m_frame + i) / kSampleRate;
        double left = 0;
        double right = 0;
        for (const Note &note : m_notes) {
            const double rel = t - note.startSeconds;
            if (rel < 0 || rel > noteLife(note))
                continue;
            const double s = sampleNote(note, rel) * note.gain;
            // Equal power, so a note moved across the field keeps its weight.
            left += s * std::cos(note.pan * M_PI / 2);
            right += s * std::sin(note.pan * M_PI / 2);
        }

        double wetL = 0;
        double wetR = 0;
        if (!dry) {
            const size_t e = size_t(m_echoIndex);
            wetL = double(m_echoLeft[e]);
            wetR = double(m_echoRight[e]);
            m_echoLeft[e] = float(left + wetR * echoFeedback);
            m_echoRight[e] = float(right + wetL * echoFeedback);
            m_echoIndex = (m_echoIndex + 1) % echoFrames;
        }

        out[2 * i] = qint16(softClip(left + wetL * echoWet) * 30000.0);
        out[2 * i + 1] = qint16(softClip(right + wetR * echoWet) * 30000.0);
    }
    m_frame += frames;

    const double t = double(m_frame) / kSampleRate;
    for (int i = m_notes.size() - 1; i >= 0; --i) {
        if (m_notes[i].startSeconds + noteLife(m_notes[i]) <= t)
            m_notes.removeAt(i);
    }
}

// --------------------------------------------------------------- player ----

#ifdef FORKMESH_HAVE_MULTIMEDIA
namespace {
// Pull-mode source: Qt asks for as much as its buffer wants, whenever it wants.
class SinkSource : public QIODevice
{
public:
    explicit SinkSource(Mixer *mixer) : m_mixer(mixer) {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 65536 + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const int frames = int(maxSize / (kChannels * sizeof(qint16)));
        if (frames <= 0 || !m_mixer)
            return 0;
        m_mixer->render(reinterpret_cast<qint16 *>(data), frames);
        return qint64(frames) * kChannels * qint64(sizeof(qint16));
    }
    qint64 writeData(const char *, qint64) override { return 0; }

private:
    Mixer *m_mixer = nullptr;
};
} // namespace

struct Player::QtBackend {
    QAudioSink *sink = nullptr;
    SinkSource *source = nullptr;
};
#else
// Declared in the header either way, so the unique_ptr has a complete type.
struct Player::QtBackend {
};
#endif

// The render loop. Lives on the player's own thread, owns the process it
// feeds, and paces itself by the wall clock: a thread that falls behind skips
// the audio it missed rather than playing it late.
class Player::Pump : public QObject
{
public:
    Pump(Mixer *mixer, QStringList programs, std::shared_ptr<std::atomic<int>> opened)
        : m_mixer(mixer), m_programs(std::move(programs)), m_opened(std::move(opened))
    {
    }

    void open()
    {
        for (int i = 0; i < m_programs.size(); ++i) {
            if (openProgram(m_programs.at(i))) {
                m_opened->store(i);
                return;
            }
        }
        m_opened->store(kPumpFailed);
    }

    void silence()
    {
        QMutexLocker lock(&m_mixerMutex);
        m_mixer = nullptr;
    }

    void close()
    {
        if (m_timer) {
            m_timer->stop();
            delete m_timer;
            m_timer = nullptr;
        }
        if (m_process) {
            // Closing stdin is the polite way out; a player that ignores it is
            // killed and waited for, so the process is never left running
            // behind a destroyed QProcess.
            m_process->closeWriteChannel();
            if (!m_process->waitForFinished(600)) {
                m_process->kill();
                m_process->waitForFinished(400);
            }
            delete m_process;
            m_process = nullptr;
        }
    }

private:
    bool openProgram(const QString &program)
    {
        if (!program.isEmpty()) {
            m_process = new QProcess(this);
            m_process->setProgram(program);
            m_process->setArguments(Player::pipeArgumentsFor(program));
            // Write-only, and both of its own channels discarded: nothing here
            // reads them, and an unread pipe is a way to wedge the player.
            m_process->setStandardOutputFile(QProcess::nullDevice());
            m_process->setStandardErrorFile(QProcess::nullDevice());
            m_process->start(QIODevice::WriteOnly);
            if (!m_process->waitForStarted(1500)) {
                m_process->kill();
                m_process->waitForFinished(200);
                delete m_process;
                m_process = nullptr;
                return false;
            }
        }
        m_startMs = QDateTime::currentMSecsSinceEpoch();
        m_frames = 0;
        m_timer = new QTimer(this);
        m_timer->setInterval(20);
        connect(m_timer, &QTimer::timeout, this, &Pump::render);
        m_timer->start();
        return true;
    }

    void render()
    {
        {
            QMutexLocker lock(&m_mixerMutex);
            if (!m_mixer)
                return;
            // Stay one buffer ahead of the wall clock.
            constexpr qint64 kLeadMs = 160;
            const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_startMs;
            qint64 want = (elapsed + kLeadMs) * kSampleRate / 1000 - m_frames;
            if (want <= 0)
                return;
            const qint64 maxFrames = kSampleRate / 2; // half a second of catch-up
            if (want > maxFrames) {
                // Something held this thread up long enough that the gap is
                // audible either way; skip the missed audio instead of playing it
                // late and drifting further behind the run being described.
                m_frames += want - maxFrames;
                want = maxFrames;
            }
            m_buffer.resize(int(want) * kChannels * int(sizeof(qint16)));
            m_mixer->render(reinterpret_cast<qint16 *>(m_buffer.data()), int(want));
            m_frames += want;
        }
        if (!m_process)
            return; // the silent backend: rendered, and thrown away
        if (m_process->state() != QProcess::Running) {
            m_timer->stop();
            return;
        }
        m_process->write(m_buffer);
    }

    QMutex m_mixerMutex;
    Mixer *m_mixer = nullptr;
    QStringList m_programs;
    std::shared_ptr<std::atomic<int>> m_opened;
    QProcess *m_process = nullptr;
    QTimer *m_timer = nullptr;
    QByteArray m_buffer;
    qint64 m_frames = 0; // frames rendered since the pump started
    qint64 m_startMs = 0;
};

Player::Player(Mixer *mixer, QObject *parent) : QObject(parent), m_mixer(mixer) {}

Player::~Player()
{
    shutdown(true);
}

QStringList Player::pipePrograms()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    return {QStringLiteral("pw-cat"), QStringLiteral("paplay"), QStringLiteral("aplay")};
#else
    return {};
#endif
}

QStringList Player::availablePipePrograms()
{
    static QMutex mutex;
    static QHash<QByteArray, QStringList> byPath;
    const QByteArray path = qgetenv("PATH");
    QMutexLocker lock(&mutex);
    const auto known = byPath.constFind(path);
    if (known != byPath.constEnd())
        return *known;
    QStringList programs;
    for (const QString &program : pipePrograms()) {
        if (!pipeArgumentsFor(program).isEmpty() &&
            !QStandardPaths::findExecutable(program).isEmpty())
            programs.append(program);
    }
    byPath.insert(path, programs);
    return programs;
}

void Player::warmPipePrograms()
{
    QThreadPool::globalInstance()->start([] { availablePipePrograms(); });
}

QStringList Player::pipeArgumentsFor(const QString &program)
{
    const QString rate = QString::number(kSampleRate);
    const QString channels = QString::number(kChannels);
    if (program == QLatin1String("pw-cat")) {
        // --raw is what makes it read PCM from stdin; without it pw-cat tries
        // to parse the stream as a sound file and exits.
        return {QStringLiteral("--playback"),
                QStringLiteral("--raw"),
                QStringLiteral("--format"),
                QStringLiteral("s16"),
                QStringLiteral("--rate"),
                rate,
                QStringLiteral("--channels"),
                channels,
                QStringLiteral("-")};
    }
    if (program == QLatin1String("paplay")) {
        return {QStringLiteral("--raw"),
                QStringLiteral("--format=s16le"),
                QStringLiteral("--rate=%1").arg(rate),
                QStringLiteral("--channels=%1").arg(channels),
                QStringLiteral("--stream-name=ForkMesh agents")};
    }
    if (program == QLatin1String("aplay")) {
        return {QStringLiteral("-q"),
                QStringLiteral("-t"),
                QStringLiteral("raw"),
                QStringLiteral("-f"),
                QStringLiteral("S16_LE"),
                QStringLiteral("-c"),
                channels,
                QStringLiteral("-r"),
                rate,
                QStringLiteral("-")};
    }
    return {};
}

double Player::latencySeconds() const
{
    switch (m_backend) {
    case Backend::Pipe:
        return 0.18; // the pump's 160 ms lead, plus the player's own quantum
    case Backend::Qt:
        return 0.10;
    case Backend::Silent:
    case Backend::None:
        break;
    }
    return 0;
}

QString Player::backendName() const
{
    switch (m_backend) {
    case Backend::Qt:
        return QStringLiteral("qt");
    case Backend::Pipe: {
        const int opened = m_opened ? m_opened->load() : kPumpFailed;
        const QString program = m_pipePrograms.value(opened < 0 ? 0 : opened);
        return program.isEmpty() ? QStringLiteral("pipe") : program;
    }
    case Backend::Silent:
        return QStringLiteral("silent");
    case Backend::None:
        break;
    }
    return QStringLiteral("none");
}

bool Player::isRunning() const
{
    if (m_backend == Backend::None || !m_running)
        return false;
    return !m_opened || m_opened->load() != kPumpFailed;
}

bool Player::start()
{
    if (m_running) {
        if (isRunning())
            return true;
        shutdown(false);
        return false;
    }
    if (!m_mixer)
        return false;

    // The window tests and any headless run set this: the schedule is still
    // rendered, so the mixer clock advances, but nothing opens a device.
    const QByteArray forced = qgetenv("FORKMESH_SONIFY_BACKEND");
    if (forced == "none")
        return false;
    const bool wantSilent = forced == "silent";
    const bool wantPipe = forced == "pipe";

#ifdef FORKMESH_HAVE_MULTIMEDIA
    if (!wantSilent && !wantPipe) {
        QAudioFormat format;
        format.setSampleRate(kSampleRate);
        format.setChannelCount(kChannels);
        format.setSampleFormat(QAudioFormat::Int16);
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (!device.isNull() && device.isFormatSupported(format)) {
            m_qt = std::make_unique<QtBackend>();
            m_qt->sink = new QAudioSink(device, format, this);
            m_qt->source = new SinkSource(m_mixer);
            m_qt->source->open(QIODevice::ReadOnly);
            m_qt->sink->start(m_qt->source);
            if (m_qt->sink->error() == QAudio::NoError) {
                m_backend = Backend::Qt;
                m_running = true;
                return true;
            }
            m_qt->sink->stop();
            delete m_qt->source;
            m_qt.reset();
        }
    }
#endif

    // Candidates, best first; the silent backend is a pump with no process.
    QStringList programs;
    if (!wantSilent) {
        programs = availablePipePrograms();
        if (programs.isEmpty())
            return false;
    } else {
        programs.append(QString());
    }

    auto *thread = new QThread;
    thread->setObjectName(QStringLiteral("forkmesh-sonify"));
    m_opened = std::make_shared<std::atomic<int>>(kPumpOpening);
    auto *pump = new Pump(m_mixer, programs, m_opened);
    pump->moveToThread(thread);
    // Shut the process and the timer down from the pump's own thread,
    // on the way out of its event loop: by the time stop() returns
    // from wait() the object is inert and safe to delete from here.
    // Teardown can happen while the application object is already
    // being destroyed, which is too late to post it any work.
    connect(thread, &QThread::finished, pump, [pump] { pump->close(); },
            Qt::DirectConnection);
    thread->start();
    QMetaObject::invokeMethod(pump, [pump] { pump->open(); }, Qt::QueuedConnection);
    m_pump = pump;
    m_thread = thread;
    m_pipePrograms = programs;
    m_backend = wantSilent ? Backend::Silent : Backend::Pipe;
    m_running = true;
    return true;
}

void Player::stop()
{
    shutdown(!QCoreApplication::instance() || QCoreApplication::closingDown());
}

void Player::shutdown(bool waitForPump)
{
    m_running = false;
    if (m_pump) {
        Pump *pump = m_pump;
        QThread *thread = m_thread;
        m_pump = nullptr;
        m_thread = nullptr;
        pump->silence();
        if (waitForPump) {
            thread->quit();
            if (thread->wait(3000)) {
                delete pump;
                delete thread;
            }
        } else {
            QObject::connect(thread, &QThread::finished, thread,
                             [pump, thread] {
                                 delete pump;
                                 thread->deleteLater();
                             });
            thread->quit();
        }
    }
    m_pipePrograms.clear();
    m_opened.reset();
#ifdef FORKMESH_HAVE_MULTIMEDIA
    if (m_qt) {
        if (m_qt->sink)
            m_qt->sink->stop();
        delete m_qt->source;
        m_qt.reset();
    }
#endif
    m_backend = Backend::None;
}

// ------------------------------------------------------------- sonifier ----

AgentSonifier::AgentSonifier(QObject *parent) : QObject(parent) {}

AgentSonifier::~AgentSonifier() = default;

bool AgentSonifier::listen(int sessionId, const QString &title)
{
    if (m_voices.contains(sessionId))
        return true;
    if (!m_player)
        m_player = std::make_unique<Player>(&m_mixer, this);
    if (!m_player->isRunning() && !m_player->start())
        return false;

    Listener listener;
    listener.voice = voiceForSession(sessionId, title);
    // Two agents that happen to hash to the same instrument in the same
    // register would be hard to tell apart, which is the one thing this is
    // for; re-roll until the pair differs or the seats run out.
    for (int salt = 1; salt <= 8; ++salt) {
        bool clash = false;
        for (const Listener &other : std::as_const(m_voices)) {
            if (other.voice.instrument == listener.voice.instrument &&
                (other.voice.octave == listener.voice.octave ||
                 qFuzzyCompare(other.voice.pan, listener.voice.pan))) {
                clash = true;
                break;
            }
        }
        if (!clash)
            break;
        listener.voice = voiceForSession(sessionId, title, salt);
    }
    if (!m_voices.isEmpty()) {
        // The hash does not know who else is playing, so a fleet can all land
        // on one side of the room. Seat a newcomer on the emptier side; it
        // keeps its distance from the middle, which is the part of the seat
        // that belongs to the agent.
        double weight = 0;
        for (const Listener &other : std::as_const(m_voices))
            weight += other.voice.pan - 0.5;
        if (std::abs(weight) > 1e-9 && (weight > 0) == (listener.voice.pan > 0.5))
            listener.voice.pan = 1.0 - listener.voice.pan;
    }

    // Two notes announcing the seat this agent took, so you know it is on and
    // which line in the music is now it.
    const double slot = std::ceil(m_mixer.now() / kSlotSeconds) + 1;
    for (int i = 0; i < 2 && !isMuted(Motion::Start); ++i) {
        Note note;
        note.timbre = Timbre::Bell;
        note.instrument = listener.voice.instrument;
        note.startSeconds = (slot + i + listener.voice.slotOffset) * kSlotSeconds;
        note.seconds = 0.45;
        note.frequency = degreeFrequency(i == 0 ? 0 : 3, listener.voice);
        note.pan = listener.voice.pan;
        note.gain = 0.22;
        schedule(sessionId, Motion::Start, note);
    }
    listener.melody.lastSlot = slot + 1;
    m_voices.insert(sessionId, listener);
    return true;
}

void AgentSonifier::updateUsage(int sessionId, int turns, qint64 tokens)
{
    auto it = m_voices.find(sessionId);
    if (it == m_voices.end())
        return;
    const int drift = it->voice.drift;
    const double bloom = it->voice.bloom;
    applyUsage(&it->voice, turns, tokens);
    if (it->voice.drift == drift && qFuzzyCompare(1 + it->voice.bloom, 1 + bloom))
        return;
    // The line moved up the scale: keep the melody where it was in absolute
    // pitch rather than jumping it, so the change is a drift and not a lurch.
    it->melody.degree -= it->voice.drift - drift;
}

void AgentSonifier::stopListening(int sessionId)
{
    if (m_voices.remove(sessionId) == 0)
        return;
    closeWhenQuiet();
}

void AgentSonifier::closeWhenQuiet()
{
    if (!m_voices.isEmpty() || m_desktopOn || !m_player)
        return;
    // Let whatever is still ringing finish before the device closes.
    QTimer::singleShot(2000, this, [this] {
        if (m_voices.isEmpty() && !m_desktopOn && m_player)
            m_player->stop();
    });
}

bool AgentSonifier::listenDesktop()
{
    if (m_desktopOn)
        return true;
    if (!m_player)
        m_player = std::make_unique<Player>(&m_mixer, this);
    if (!m_player->isRunning() && !m_player->start())
        return false;
    m_desktop = Listener();
    m_desktop.voice = desktopVoice();
    m_desktopOn = true;
    return true;
}

void AgentSonifier::stopDesktop()
{
    if (!m_desktopOn)
        return;
    m_desktopOn = false;
    closeWhenQuiet();
}

AgentSonifier::Listener *AgentSonifier::listenerFor(int sessionId)
{
    if (sessionId == kDesktop)
        return m_desktopOn ? &m_desktop : nullptr;
    auto it = m_voices.find(sessionId);
    return it == m_voices.end() ? nullptr : &*it;
}

bool AgentSonifier::hearsCue(int sessionId, Motion motion) const
{
    if (!isCue(motion) || isMuted(motion))
        return false;
    return m_desktopOn || (sessionId != kDesktop && m_voices.contains(sessionId));
}

bool AgentSonifier::handleCue(int sessionId, Motion motion, quint32 seed)
{
    if (!hearsCue(sessionId, motion))
        return false;
    // Its own agent's voice when that one is heard; the desktop's otherwise.
    int owner = sessionId;
    Listener *listener = sessionId == kDesktop ? nullptr : listenerFor(sessionId);
    if (!listener) {
        owner = kDesktop;
        listener = listenerFor(kDesktop);
    }
    if (!listener)
        return false;
    const Note note = cueNote(motion, listener->voice, &listener->melody, m_mixer.now(), seed);
    if (note.isRest())
        return false;
    schedule(owner, motion, note);
    return true;
}

void AgentSonifier::stopAll()
{
    const QList<int> sessions = m_voices.keys();
    for (int sessionId : sessions)
        m_voices.remove(sessionId);
    m_desktopOn = false;
    m_mixer.clear();
    if (m_player)
        m_player->stop();
}

Voice AgentSonifier::voiceFor(int sessionId) const
{
    const auto it = m_voices.constFind(sessionId);
    return it == m_voices.constEnd() ? Voice() : it->voice;
}

QString AgentSonifier::voiceDescriptionFor(int sessionId) const
{
    const auto it = m_voices.constFind(sessionId);
    return it == m_voices.constEnd() ? QString() : voiceDescription(it->voice);
}

void AgentSonifier::handleEvent(int sessionId, const QJsonObject &event)
{
    auto it = m_voices.find(sessionId);
    if (it == m_voices.end())
        return;
    // Work after a cadence is a new turn on the same session: it earns its own
    // ending when it gets there, whether or not this particular event found a
    // free slot to sound in.
    const Motion motion = motionForEvent(event);
    if (motion != Motion::Rest && !isBookkeeping(motion))
        it->cadenceRung = false;
    if (isMuted(motion))
        return;
    const Note note = noteForEvent(event, it->voice, &it->melody, m_mixer.now());
    if (note.isRest())
        return;
    schedule(sessionId, motion, note);
}

void AgentSonifier::handleFinished(int sessionId, bool success)
{
    auto it = m_voices.find(sessionId);
    if (it == m_voices.end() || it->cadenceRung)
        return;
    it->cadenceRung = true;
    const double start = (std::ceil(m_mixer.now() / kSlotSeconds) + 1) * kSlotSeconds;
    const Motion ending = success ? Motion::Finish : Motion::Notice;
    if (!isMuted(ending)) {
        const QVector<Note> cadence = doneCadence(it->voice, success, start);
        for (const Note &note : cadence)
            schedule(sessionId, ending, note);
    }
    // Nothing else from this voice lands on top of the ending.
    it->melody.lastSlot = (start + 2.6) / kSlotSeconds;
    it->melody.degree = 0;
}

void AgentSonifier::schedule(int sessionId, Motion motion, const Note &note)
{
    if (!m_mixer.addNote(note))
        return; // the mixer was full: nothing will sound, so nothing is drawn
    if (!m_observer)
        return;
    const double latency = m_player ? m_player->latencySeconds() : 0.0;
    m_observer(sessionId, motion, note, note.startSeconds - m_mixer.now() + latency);
}

void AgentSonifier::handleNetwork(int sessionId, qint64 sent, qint64 received)
{
    // Its own voice when it is heard; the desktop's while everything is.
    Listener *it = listenerFor(sessionId);
    if (!it)
        it = listenerFor(kDesktop);
    if (!it || isMuted(Motion::Network))
        return;
    const double now = m_mixer.now();
    const Note out = networkNote(it->voice, &it->melody, now, true, sent);
    if (!out.isRest())
        schedule(sessionId, Motion::Network, out);
    const Note in = networkNote(it->voice, &it->melody, now, false, received);
    if (!in.isRest())
        schedule(sessionId, Motion::Network, in);
}

void AgentSonifier::setMuted(Motion motion, bool muted)
{
    const int index = int(motion);
    if (index > 0 && index < kMotionCount)
        m_muted[index] = muted;
}

bool AgentSonifier::isMuted(Motion motion) const
{
    const int index = int(motion);
    return index > 0 && index < kMotionCount && m_muted[index];
}

QString AgentSonifier::backendName() const
{
    return m_player ? m_player->backendName() : QStringLiteral("none");
}

} // namespace forkmesh::sonify
