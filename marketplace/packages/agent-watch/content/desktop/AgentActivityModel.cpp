#include "AgentActivityModel.h"

#include "ChatVisitorPresence.h"

#include <QFileInfo>
#include <QLocale>
#include <QJsonArray>
#include <QSet>
#include <QRegularExpression>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace forkmesh::agentvis {
namespace {

// Two marks of the same kind off the same body landing this close together
// sound as one - the notes of a chord - and so are drawn as one ring, fed by
// each. Tighter than the grace notes' quarter-slot, so every note that sounds
// on its own is seen on its own.
constexpr double kMergeWindow = 0.03;

// One style per Motion, in the enum's own order. styleFor() indexes this
// directly and the tests check the two stay the same length.
constexpr MotionStyle kStyles[] = {
    // Rest: never drawn. Present so the table has no hole in it.
    {0.00, 0.0, 0.0, 0.0, 0, false, false, "", "", ""},
    {0.33, 2.6, 1.8, 1.6, 0, false, false, "Start",
     "a run opening, or a session announcing itself",
     "a bell on the root: the phrase opens"},
    {0.60, 1.5, 1.3, 1.0, 0, false, false, "Speaking",
     "the agent writing prose to you",
     "a mallet wandering up and down the scale"},
    {0.78, 1.2, 2.0, 0.7, 0, false, false, "Thinking",
     "reasoning, under its breath",
     "a soft pad breathed in slowly, drifting down"},
    {0.52, 1.8, 1.1, 0.8, 0, false, false, "Reading",
     "a file read, a folder listed",
     "a bright glass note, a step or two up"},
    {0.16, 2.2, 1.3, 0.8, 3, false, true, "Searching",
     "grep, glob, find: looking through the tree",
     "a glass note leaping two to four steps up"},
    {0.555, 2.6, 1.5, 0.9, 0, false, false, "Web",
     "a page fetched, the web searched",
     "a far glass note leaping three to five steps up"},
    {0.87, 1.7, 1.4, 1.9, 4, false, false, "Writing",
     "an edit or a new file landing on disk",
     "a ringing bell, climbing"},
    {0.08, 1.4, 0.9, 1.2, 4, false, false, "Command",
     "a shell command being run",
     "a dry wooden knock that bends down, climbing"},
    {0.70, 2.4, 1.6, 1.1, 6, false, false, "Delegating",
     "work handed to a subagent, a skill or an MCP server",
     "a bell leaping three or four steps up"},
    {0.28, 1.6, 1.5, 1.0, 0, false, false, "Planning",
     "a todo list written, a plan put forward",
     "a mallet ticking off a list, up a step and back"},
    {0.42, 1.9, 1.1, 0.9, 0, true, false, "Result",
     "a tool's answer coming back",
     "a mallet falling, further for a bigger answer"},
    {0.95, 2.2, 1.7, 1.5, 0, true, false, "Prompt",
     "you, steering it",
     "a loud bell back on the root"},
    {0.00, 2.0, 1.4, 1.4, 5, false, true, "Notice",
     "a guardrail, a protocol error, a refusal, a stop",
     "a bright glass note held level; a failed run falls to the root"},
    {0.24, 3.0, 2.4, 1.7, 0, false, false, "Done",
     "a turn's result landing, a run finishing",
     "a bell on the root; a finished run rings a rising cadence"},
    {0.50, 1.6, 1.0, 0.8, 0, false, true, "Listing",
     "a folder listed, a glob expanded",
     "a short pure ping, a step or two up"},
    {0.82, 2.2, 1.6, 1.8, 0, false, false, "New file",
     "a file written from nothing",
     "a note swooping up into place, climbing"},
    {0.30, 1.8, 1.5, 1.2, 0, false, false, "Tests",
     "a test suite running",
     "a trill, shaken back and forth"},
    {0.05, 2.0, 1.4, 1.6, 0, false, false, "Build",
     "a compile, a bundle, a make",
     "a low drum an octave down, bent hard"},
    {0.98, 1.9, 1.4, 1.2, 0, false, false, "Git",
     "git or gh: history read, a commit, a push",
     "a sagging string, through history and back"},
    {0.14, 2.0, 1.6, 1.2, 0, true, false, "Install",
     "a package manager fetching dependencies",
     "a low drum piling up a step at a time"},
    {0.62, 2.8, 1.6, 1.2, 0, false, false, "Background",
     "a command sent off to run on its own",
     "a swoop leaping four or five steps up"},
    {0.66, 1.3, 2.2, 0.8, 0, false, true, "Waiting",
     "polling background work, sleeping on it",
     "a slow swell, held nearly level"},
    {0.90, 2.4, 2.0, 1.6, 0, false, false, "Question",
     "the agent asking you something",
     "a bell going up at the end, like a question"},
    {0.74, 2.2, 1.5, 1.2, 0, false, false, "Skill",
     "a skill called in",
     "a twanging string leaping up"},
    {0.48, 2.4, 1.5, 1.1, 0, false, false, "MCP",
     "a tool on an MCP server",
     "a far glass note, two to five steps up"},
    {0.26, 1.5, 1.4, 1.0, 0, false, false, "Todo",
     "the todo list or task list updated",
     "a mallet ticking up and back"},
    {0.02, 2.0, 1.3, 1.5, 0, true, false, "Error",
     "a tool call that came back as an error",
     "a rough buzz, falling"},
    {0.97, 2.2, 1.5, 1.7, 0, true, true, "Refused",
     "a tool call refused: a permission, a hook, you",
     "a harder buzz, falling further"},
    {0.20, 2.0, 1.4, 1.0, 0, true, false, "Image",
     "a picture back: a screenshot, an image read",
     "a glass note, shaded by the picture"},
    {0.68, 3.0, 2.2, 1.4, 0, true, false, "Compacting",
     "the context folded down to make room",
     "a swell sinking four or five steps"},
    {0.11, 1.6, 1.3, 1.0, 0, false, true, "Retry",
     "an API call failed and is being tried again",
     "a trill held level"},
    {0.00, 3.0, 2.4, 1.8, 0, true, false, "Failed",
     "a turn that ended in an error",
     "a buzz sinking to the fifth under the root"},
    {0.64, 0.9, 0.7, 0.6, 0, false, false, "Framing",
     "a message or block opening in the stream",
     "a dry wooden click just above the line"},
    {0.46, 1.1, 0.9, 0.6, 0, false, false, "Tool call",
     "a tool call's arguments streaming in, piece by piece",
     "soft glass taps stepping upwards, like typing"},
    {0.12, 1.0, 1.1, 0.7, 0, true, false, "Usage",
     "tokens counted, rate limits reported, a turn's stop reason",
     "a low soft mallet tap under the line"},
    {0.36, 1.2, 1.2, 0.8, 6, false, false, "System",
     "a hook firing, a status change, a compaction, a retry",
     "a small bell a fifth above, held a moment"},
    {0.58, 0.7, 0.6, 0.5, 0, false, false, "Other",
     "anything else the log records: heartbeats, control frames",
     "a faint high sparkle between the beats"},
    {0.60, 0.9, 0.7, 0.6, 0, true, false, "Closing",
     "a message or block closing in the stream",
     "a dry wooden click, under the opening one"},
    {0.15, 0.8, 0.6, 0.5, 0, false, false, "Seal",
     "a thinking block's signature, a citation",
     "a glinting ping at the very top"},
    {0.40, 1.1, 1.1, 0.8, 0, false, false, "Hook",
     "a hook starting or answering",
     "a sagging string a little above the line"},
    {0.50, 1.0, 1.0, 0.6, 0, false, false, "Network",
     "bytes an agent sent or received over the network",
     "a faint tick: higher for sent, lower for received"},
    {0.53, 2.2, 1.4, 1.0, 0, false, false, "New host",
     "a host joining the field: the first bytes to somewhere new",
     "a chirp swooping up high"},
    {0.10, 1.4, 1.2, 0.9, 0, false, false, "Desktop work",
     "the desktop's background work starting or finishing",
     "a soft wooden knock a step above the line"},
    {0.99, 2.2, 1.6, 1.5, 0, true, false, "Log: error",
     "an ERROR line in the desktop's log",
     "a rough buzz falling below the line"},
    {0.11, 2.0, 1.8, 1.2, 0, false, true, "Log: stall",
     "a STALL or BGBLOCK line: the interface held up, work turned away",
     "a low swell, breathed in"},
    {0.50, 1.6, 1.2, 0.9, 0, false, false, "Log: mesh",
     "a MIRROR, SYNC, PIN, PEER, NODE, FORKED or STATUS line",
     "a glass note a few steps up"},
    {0.61, 1.7, 1.3, 1.0, 0, false, false, "Log: git",
     "a GIT, PULL, MERGE, PUBLISH, ISSUE, BOUNTY or ACTIONS line",
     "a twanging string a step or two up"},
    {0.08, 1.6, 1.1, 0.9, 0, true, false, "Log: cloud",
     "a CLOUD, NETWORK or DB line: the site and its database",
     "a short pure ping high over the line"},
    {0.04, 1.3, 1.1, 0.8, 0, false, false, "Log: background",
     "a BGTASK or BGNOTE line: background work reported",
     "a dry wooden tap on the line"},
    {0.79, 1.8, 1.4, 1.0, 0, false, false, "Log: account",
     "a SESSION, ACCOUNT, IDENTITY, CRYPTO, ADMIN or PROMPT line",
     "a small bell a fifth up"},
    {0.57, 1.2, 1.0, 0.7, 0, false, false, "Log: info",
     "an INFO line, or any badge without a family",
     "the faintest glass glint at the top"},
    {0.46, 2.4, 1.6, 1.2, 0, false, false, "Web visitor",
     "a browser on the site - a guest in the chat or the world, someone signed in "
     "on the web, or a request in the Worker's live log - arriving or leaving; "
     "drawn as its country's flag, with a warning sign when it looks like an attack",
     "a chirp a fourth over the line"},
    {0.66, 2.2, 1.5, 1.1, 0, false, false, "Client",
     "another desktop node or a phone joining the relay room, or leaving it",
     "a small bell a step over the line"},
    {0.52, 1.0, 0.8, 0.6, 0, true, false, "Socket frame",
     "a bead between this desktop and the row beside it: warm, a frame it sent "
     "(presence, chat, typing - one to the whole room reaches everybody in it); "
     "cool, a frame received, or a site visitor's request",
     "the faintest tick: higher for one received, lower for one sent"},
};
static_assert(int(std::size(kStyles)) == forkmesh::sonify::kMotionCount,
              "every Motion needs a style, and the styles are indexed by Motion");

// The six instruments, as six colour families. An agent's shade within its
// family comes from the degree of the scale it calls home, which is the other
// half of what makes two runs on the same instrument tellable apart.
constexpr double kInstrumentHues[6] = {
    0.08, // Marimba: warm wood
    0.55, // Celesta: bright cyan
    0.92, // Pluck: rose
    0.33, // Kalimba: green
    0.72, // Chime: violet
    0.17, // Reed: amber
};

double wrapHue(double hue)
{
    hue = std::fmod(hue, 1.0);
    return hue < 0 ? hue + 1.0 : hue;
}

qint64 joinKey(int a, int b)
{
    return (qint64(qMin(a, b)) << 32) | quint32(qMax(a, b));
}

double decay(double value, double seconds, double halfLife)
{
    if (value <= 0 || seconds <= 0 || halfLife <= 0)
        return value <= 0 ? 0 : value;
    return value * std::pow(0.5, seconds / halfLife);
}

// A stable little number in 0..1 for an agent, for the places a picture needs
// to break a tie that the sound does not have to: which way a pulse fans, how
// far off its row a body sits when two share one.
double jitter(int sessionId, int stir)
{
    quint32 h = quint32(sessionId) * 2654435761u + quint32(stir) * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x2545f491u;
    h ^= h >> 13;
    return double(h % 10000u) / 10000.0;
}

QString oneLine(const QString &text, int limit)
{
    QString line = text.simplified();
    if (line.size() > limit)
        line = line.left(limit - 1).trimmed() + QChar(0x2026);
    return line;
}

// The first thing in a tool's input that says what it is working on.
QString toolTarget(const QJsonObject &input)
{
    for (const char *key : {"file_path", "notebook_path", "path", "url", "pattern",
                            "query", "skill", "description", "prompt"}) {
        const QString value = input.value(QLatin1String(key)).toString();
        if (!value.trimmed().isEmpty())
            return value;
    }
    const QJsonValue command = input.value(QStringLiteral("command"));
    if (command.isString())
        return command.toString();
    if (command.isArray()) {
        QStringList words;
        for (const QJsonValue &word : command.toArray())
            words.append(word.toString());
        // Codex wraps a command in the shell that runs it: show the command.
        if (words.size() == 3 && words.at(1) == QLatin1String("-lc"))
            return words.at(2);
        return words.join(QLatin1Char(' '));
    }
    const QJsonArray changes = input.value(QStringLiteral("changes")).toArray();
    if (!changes.isEmpty())
        return changes.first().toObject().value(QStringLiteral("path")).toString();
    const QString tool = input.value(QStringLiteral("tool")).toString();
    return tool;
}

// The words a tool result came back with, the first of them.
QString toolResultText(const QJsonObject &event)
{
    const QJsonArray content = event.value(QStringLiteral("message"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != QLatin1String("tool_result"))
            continue;
        const QJsonValue result = block.value(QStringLiteral("content"));
        if (result.isString())
            return result.toString().left(400);
        for (const QJsonValue &part : result.toArray()) {
            const QString text = part.toObject().value(QStringLiteral("text")).toString();
            if (!text.isEmpty())
                return text.left(400);
        }
    }
    return {};
}

QJsonObject firstToolUse(const QJsonObject &event)
{
    if (event.value(QStringLiteral("type")).toString() != QLatin1String("assistant"))
        return {};
    const QJsonArray content = event.value(QStringLiteral("message"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("tool_use"))
            return block;
    }
    return {};
}

} // namespace

QString shortSubject(const QString &subject)
{
    if (subject.startsWith(QLatin1String("http://"))
        || subject.startsWith(QLatin1String("https://"))) {
        const QString host = QUrl(subject).host();
        return host.isEmpty() ? oneLine(subject, 40) : host;
    }
    const QString name = QFileInfo(subject).fileName();
    return name.isEmpty() ? oneLine(subject, 40) : name;
}

QString promptForEvent(const QJsonObject &event, int limit)
{
    QString text = event.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        const QJsonValue content = event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"));
        if (content.isString()) {
            text = content.toString();
        } else {
            for (const QJsonValue &part : content.toArray()) {
                const QJsonObject piece = part.toObject();
                if (piece.value(QStringLiteral("type")).toString() == QLatin1String("text"))
                    text += piece.value(QStringLiteral("text")).toString();
            }
        }
    }
    text = text.trimmed();
    if (text.size() > limit)
        text = text.left(limit - 1).trimmed() + QChar(0x2026);
    return text;
}

QString proseForEvent(const QJsonObject &event, int limit)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type != QLatin1String("assistant") && type != QLatin1String("_codex_agent_complete"))
        return {};
    return promptForEvent(event, limit);
}

QString plainProse(const QString &text, int limit)
{
    QStringList kept;
    bool fenced = false;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        // A code block is not prose: the words round it are.
        if (line.startsWith(QLatin1String("```"))) {
            fenced = !fenced;
            continue;
        }
        if (fenced || line.isEmpty())
            continue;
        while (line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('>')))
            line = line.mid(1).trimmed();
        if (line.startsWith(QLatin1String("- ")) || line.startsWith(QLatin1String("* ")))
            line = line.mid(2);
        kept << line;
    }
    QString plain = kept.join(QLatin1Char(' '));
    for (const char *mark : {"**", "__", "`"})
        plain.remove(QLatin1String(mark));
    plain = plain.simplified();
    if (plain.size() > limit)
        plain = plain.left(limit - 1).trimmed() + QChar(0x2026);
    return plain;
}

QString subjectForEvent(const QJsonObject &event)
{
    const QJsonObject tool = firstToolUse(event);
    if (tool.isEmpty())
        return {};
    const QJsonObject input = tool.value(QStringLiteral("input")).toObject();
    for (const char *key : {"file_path", "notebook_path", "path", "url"}) {
        const QString value = input.value(QLatin1String(key)).toString().trimmed();
        if (!value.isEmpty())
            return value;
    }
    const QJsonArray changes = input.value(QStringLiteral("changes")).toArray();
    if (!changes.isEmpty())
        return changes.first().toObject().value(QStringLiteral("path")).toString();
    return {};
}

QString actionForEvent(const QJsonObject &event)
{
    const Motion motion = sonify::motionForEvent(event);
    const QString type = event.value(QStringLiteral("type")).toString();
    switch (motion) {
    case Motion::Rest:
    // Bookkeeping: a mark and a tick, never a line of the feed.
    case Motion::Frame:
    case Motion::Compose:
    case Motion::Usage:
    case Motion::System:
    case Motion::Tick:
    case Motion::Close:
    case Motion::Seal:
    case Motion::Hook:
    case Motion::Network:
        return {};
    case Motion::Compact:
        return QStringLiteral("compacting its context");
    case Motion::Retry:
        return QStringLiteral("retrying the API");
    case Motion::Fail: {
        const QString text = event.value(QStringLiteral("result")).toString();
        return text.isEmpty() ? QStringLiteral("turn failed")
                              : QStringLiteral("failed: ") + oneLine(text, 60);
    }
    case Motion::Fumble:
    case Motion::Denied:
    case Motion::Image: {
        if (motion == Motion::Image)
            return QStringLiteral("image in");
        const QString text = oneLine(toolResultText(event), 60);
        const QString word = motion == Motion::Denied ? QStringLiteral("refused")
                                                      : QStringLiteral("error");
        return text.isEmpty() ? word : word + QStringLiteral(": ") + text;
    }
    case Motion::Start:
        return QStringLiteral("started");
    case Motion::Think:
        return QStringLiteral("thinking");
    case Motion::Finish:
        return QStringLiteral("turn done");
    case Motion::Notice: {
        const QString text = event.value(QStringLiteral("text")).toString();
        return text.isEmpty() ? QStringLiteral("notice")
                              : QStringLiteral("notice: ") + oneLine(text, 60);
    }
    case Motion::Prompt: {
        const QString text = promptForEvent(event, 200);
        return text.isEmpty() ? QStringLiteral("prompted")
                              : QStringLiteral("asked: ") + oneLine(text, 60);
    }
    case Motion::Arrive: {
        if (type == QLatin1String("_codex_tool_delta"))
            return QStringLiteral("output streaming");
        const int bytes = sonify::payloadBytes(event);
        return bytes >= 1000 ? QStringLiteral("result in, %1 KB").arg(bytes / 1000)
                             : QStringLiteral("result in");
    }
    case Motion::Speak: {
        if (type == QLatin1String("stream_event")
            || type == QLatin1String("_codex_agent_delta"))
            return QStringLiteral("writing a reply");
        QString text = event.value(QStringLiteral("text")).toString();
        if (text.isEmpty()) {
            const QJsonArray content = event.value(QStringLiteral("message"))
                                           .toObject()
                                           .value(QStringLiteral("content"))
                                           .toArray();
            for (const QJsonValue &value : content) {
                const QJsonObject block = value.toObject();
                if (block.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                    text = block.value(QStringLiteral("text")).toString();
                    break;
                }
            }
        }
        return text.trimmed().isEmpty() ? QStringLiteral("speaking")
                                        : QStringLiteral("says: ") + oneLine(text, 60);
    }
    case Motion::Read:
    case Motion::Search:
    case Motion::Web:
    case Motion::Write:
    case Motion::Run:
    case Motion::Delegate:
    case Motion::Plan:
    case Motion::Scan:
    case Motion::Create:
    case Motion::Test:
    case Motion::Build:
    case Motion::Git:
    case Motion::Install:
    case Motion::Spawn:
    case Motion::Wait:
    case Motion::Ask:
    case Motion::Skill:
    case Motion::Mcp:
    case Motion::Todo:
        break;
    }
    const QJsonObject tool = firstToolUse(event);
    const QString name = tool.value(QStringLiteral("name")).toString();
    const QJsonObject input = tool.value(QStringLiteral("input")).toObject();
    QString target = toolTarget(input);
    const QString subject = subjectForEvent(event);
    if (!subject.isEmpty() && target == subject)
        target = shortSubject(subject);
    // Anything a shell ran - tests, a build, git - reads as the command.
    if (input.contains(QStringLiteral("command")) && !target.isEmpty())
        return QStringLiteral("$ ") + oneLine(target, 56);
    const QString verb = name.startsWith(QLatin1String("mcp__"))
                             ? name.section(QStringLiteral("__"), 1, 1)
                                   + QLatin1Char('.')
                                   + name.section(QStringLiteral("__"), 2)
                             : name;
    if (target.isEmpty())
        return verb.isEmpty() ? QString::fromLatin1(styleFor(motion).label) : verb;
    return (verb.isEmpty() ? QString::fromLatin1(styleFor(motion).label) : verb)
           + QLatin1Char(' ') + oneLine(target, 56);
}

QString labelForEvent(const QJsonObject &event)
{
    const QString action = actionForEvent(event);
    if (!action.isEmpty())
        return action;
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type.isEmpty())
        return {};
    // Bookkeeping: name the piece of the log it was.
    QString what = event.value(QStringLiteral("subtype")).toString();
    if (what.isEmpty() && type == QLatin1String("stream_event")) {
        const QJsonObject inner = event.value(QStringLiteral("event")).toObject();
        what = inner.value(QStringLiteral("delta")).toObject()
                   .value(QStringLiteral("type")).toString();
        if (what.isEmpty())
            what = inner.value(QStringLiteral("type")).toString();
    }
    if (what.isEmpty())
        what = type.startsWith(QLatin1Char('_')) ? type.mid(1) : type;
    return QStringLiteral("log: ") + what;
}

const MotionStyle &styleFor(Motion motion)
{
    const int index = int(motion);
    if (index < 0 || index >= int(std::size(kStyles)))
        return kStyles[0];
    return kStyles[size_t(index)];
}

double hueForVoice(const Voice &voice)
{
    const int instrument = qBound(0, int(voice.instrument), 5);
    return wrapHue(kInstrumentHues[instrument]
                   + (voice.degreeOffset - 2) * 0.018);
}

double seatX(const Voice &voice)
{
    return qBound(0.0, voice.pan, 1.0);
}

double seatY(const Voice &voice)
{
    // Three registers stacked bottom to top, with the climb the turns earn
    // folded in: a run that has been going all afternoon sits plainly above
    // one that has just started, exactly as it sounds above it.
    const double register_ = voice.octave + voice.drift / 6.0;
    // The top strip is kept for the network's hosts.
    return qBound(0.13, 1.0 - (register_ + 0.45) / 3.3, 0.90);
}

Band bandFor(State state)
{
    switch (state) {
    case State::Running:
        return Band::Working;
    case State::Waiting:
        return Band::Waiting;
    case State::Idle:
    case State::Done:
    case State::Failed:
        break;
    }
    return Band::Resting;
}

QString bandLabel(Band band)
{
    switch (band) {
    case Band::Working:
        return QStringLiteral("working");
    case Band::Waiting:
        return QStringLiteral("waiting on you");
    case Band::Resting:
        break;
    }
    return QStringLiteral("idle or finished");
}

Scene::Scene()
{
    m_clock.start();
    std::fill(std::begin(m_lastSounded), std::end(m_lastSounded), -1e9);
    std::fill(std::begin(m_lastLanded), std::end(m_lastLanded), -1e9);
    std::fill(std::begin(m_lastTextSeconds), std::end(m_lastTextSeconds), -1e9);
}

QString Scene::lastText(Motion motion) const
{
    const int index = int(motion);
    return index >= 0 && index < sonify::kMotionCount ? m_lastText[index] : QString();
}

double Scene::lastTextSeconds(Motion motion) const
{
    const int index = int(motion);
    return index >= 0 && index < sonify::kMotionCount ? m_lastTextSeconds[index] : -1e9;
}

int Scene::landCount(Motion motion) const
{
    const int index = int(motion);
    return index >= 0 && index < sonify::kMotionCount ? m_landCount[index] : 0;
}

void Scene::noteText(Motion motion, const QString &text, double seconds)
{
    const int index = int(motion);
    if (text.isEmpty() || index <= 0 || index >= sonify::kMotionCount
        || seconds < m_lastTextSeconds[index])
        return;
    m_lastText[index] = text;
    m_lastTextSeconds[index] = seconds;
}

double Scene::lastSounded(Motion motion) const
{
    const int index = int(motion);
    return index >= 0 && index < sonify::kMotionCount ? m_lastSounded[index] : -1e9;
}

double Scene::lastLanded(Motion motion) const
{
    const int index = int(motion);
    return index >= 0 && index < sonify::kMotionCount ? m_lastLanded[index] : -1e9;
}

int Scene::soundCount(Motion motion) const
{
    const int index = int(motion);
    return index >= 0 && index < sonify::kMotionCount ? m_soundCount[index] : 0;
}

void Scene::land(const Pulse &pulse)
{
    const int index = int(pulse.motion);
    if (index <= 0 || index >= sonify::kMotionCount)
        return;
    m_lastLanded[index] = qMax(m_lastLanded[index], pulse.bornSeconds);
    m_landCount[index] += 1;
    noteText(pulse.motion, pulse.text, pulse.bornSeconds);
    if (pulse.heard) {
        m_lastSounded[index] = qMax(m_lastSounded[index], pulse.bornSeconds);
        m_soundCount[index] += 1;
    }
    if (Body *body = find(pulse.sessionId))
        body->kinds[index] += 1;
    else if (Host *mirror = mirrorHost(pulse.sessionId))
        mirror->kinds[index] += 1;
    else if (pulse.sessionId == kDesktop)
        m_desktopKinds[index] += 1;
    Mark mark;
    mark.sessionId = pulse.sessionId;
    mark.motion = pulse.motion;
    mark.seconds = pulse.bornSeconds;
    mark.strength = pulse.strength;
    mark.heard = pulse.heard;
    mark.text = pulse.text;
    m_history.append(mark);
    if (m_history.size() > kMaxHistory)
        m_history.remove(0, m_history.size() - kMaxHistory);
}

void Scene::tick()
{
    advanceTo(double(m_clock.nsecsElapsed()) / 1e9);
}

void Scene::advanceTo(double seconds)
{
    if (seconds < m_now) {
        // A clock that went backwards is a caller bug, not a state to model.
        m_now = seconds;
        m_lastAdvance = seconds;
        return;
    }
    const double elapsed = seconds - m_lastAdvance;
    m_now = seconds;
    if (elapsed <= 0)
        return;
    m_lastAdvance = seconds;
    for (Body &body : m_bodies)
        body.energy = decay(body.energy, elapsed, kEnergyHalfLife);
    for (Host &host : m_hosts)
        host.energy = decay(host.energy, elapsed, kEnergyHalfLife);
    for (Client &client : m_clients)
        client.energy = decay(client.energy, elapsed, kEnergyHalfLife);
    for (Client &client : m_clients) {
        if (client.web && client.present() && m_now - client.lastFrameSeconds >= kWebVisitorIdle)
            client.leftSeconds = m_now;
    }
    m_clients.removeIf([this](const Client &client) {
        return !client.present() && m_now - client.leftSeconds >= kClientLinger;
    });
    m_frames.removeIf([this](const Frame &frame) {
        return m_now - frame.bornSeconds >= frame.seconds;
    });

    // A mark scheduled for a note strikes its body when the note sounds, not
    // when it was asked for: the glow and the ring land on the
    // beat you hear.
    for (Pulse &pulse : m_pulses) {
        if (!pulse.struck && !pulse.pending(m_now))
            strike(pulse);
    }
    m_pulses.removeIf([this](const Pulse &pulse) { return pulse.expired(m_now); });
    m_tasks.removeIf([this](const Task &task) {
        if (task.kind != TaskKind::Desktop && task.running()
            && m_now - task.bornSeconds >= kAgentTaskLimit)
            return true;
        return !task.running() && m_now - task.endSeconds >= kTaskLinger;
    });
    m_processes.removeIf([this](const Process &process) {
        return !process.running() && m_now - process.endSeconds >= kProcessLinger;
    });
    separate(elapsed);
    settleHosts(elapsed);

    for (auto it = m_joins.begin(); it != m_joins.end();) {
        it->strength = decay(it->strength, elapsed, kJoinHalfLife);
        if (it->strength < 0.02)
            it = m_joins.erase(it);
        else
            ++it;
    }

    const double cutoff = m_now - 60.0;
    auto first = std::lower_bound(m_eventTimes.begin(), m_eventTimes.end(), cutoff);
    if (first != m_eventTimes.begin())
        m_eventTimes.erase(m_eventTimes.begin(), first);
    auto firstFrame = std::lower_bound(m_frameTimes.begin(), m_frameTimes.end(), cutoff);
    if (firstFrame != m_frameTimes.begin())
        m_frameTimes.erase(m_frameTimes.begin(), firstFrame);
    auto firstArrival = std::lower_bound(m_arrivals.begin(), m_arrivals.end(), cutoff);
    if (firstArrival != m_arrivals.begin())
        m_arrivals.erase(m_arrivals.begin(), firstArrival);
    int stale = 0;
    while (stale < m_history.size() && m_now - m_history.at(stale).seconds > kHistorySeconds)
        ++stale;
    if (stale > 0)
        m_history.remove(0, stale);
}

Body *Scene::find(int sessionId)
{
    for (Body &body : m_bodies) {
        if (body.sessionId == sessionId)
            return &body;
    }
    return nullptr;
}

const Body *Scene::body(int sessionId) const
{
    for (const Body &body : m_bodies) {
        if (body.sessionId == sessionId)
            return &body;
    }
    return nullptr;
}

bool Scene::working(int sessionId) const
{
    const Body *found = body(sessionId);
    return found && (found->state == State::Running || found->state == State::Waiting);
}

Body &Scene::addAgent(int sessionId, const QString &title)
{
    if (Body *existing = find(sessionId)) {
        if (existing->title.isEmpty() && !title.isEmpty()) {
            existing->title = title;
            existing->voice = sonify::voiceForSession(sessionId, title);
            existing->voice.drift = 0;
            sonify::applyUsage(&existing->voice, existing->turns, existing->tokens);
            existing->hue = hueForVoice(existing->voice);
            relayout();
        }
        return *existing;
    }
    Body body;
    body.sessionId = sessionId;
    body.title = title;
    body.voice = sonify::voiceForSession(sessionId, title);
    body.hue = hueForVoice(body.voice);
    body.bornSeconds = m_now;
    m_bodies.append(body);
    relayout();
    return m_bodies.last();
}

void Scene::setVoice(int sessionId, const Voice &voice)
{
    Body *body = find(sessionId);
    if (!body)
        return;
    // The mixer moves a voice that would have clashed with one already playing.
    // Take its answer: the picture's job is to show where the sound actually
    // is, not where it would have been.
    const int drift = body->voice.drift;
    const double bloom = body->voice.bloom;
    body->voice = voice;
    if (body->voice.drift == 0)
        body->voice.drift = drift;
    if (body->voice.bloom <= 0)
        body->voice.bloom = bloom;
    body->hue = hueForVoice(body->voice);
    relayout();
}

void Scene::setTitle(int sessionId, const QString &title)
{
    if (Body *body = find(sessionId); body && body->title != title)
        body->title = title;
}

void Scene::setTask(int sessionId, const QString &task)
{
    if (Body *body = find(sessionId); body && body->task != task)
        body->task = task;
}

void Scene::setSaid(int sessionId, const QString &said)
{
    if (Body *body = find(sessionId); body && !said.isEmpty() && body->said != said) {
        body->said = said;
        body->saidSeconds = m_now;
    }
}

void Scene::setState(int sessionId, State state)
{
    Body *body = find(sessionId);
    if (!body || body->state == state)
        return;
    const bool moves = bandFor(body->state) != bandFor(state);
    body->state = state;
    if (moves)
        relayout();
}

int Scene::kindCount(int sessionId, Motion motion) const
{
    const int index = int(motion);
    if (index <= 0 || index >= sonify::kMotionCount)
        return 0;
    if (sessionId == kDesktop)
        return m_desktopKinds[index];
    if (const Host *mirror = mirrorForSource(sessionId))
        return mirror->kinds[index];
    const Body *found = body(sessionId);
    return found ? found->kinds[index] : 0;
}

void Scene::setListening(int sessionId, bool listening)
{
    if (Body *body = find(sessionId))
        body->listening = listening;
}

void Scene::setPrompt(int sessionId, const QString &prompt)
{
    if (Body *body = find(sessionId))
        body->prompt = prompt.trimmed().left(600);
}

void Scene::setUsage(int sessionId, int turns, qint64 tokens)
{
    Body *body = find(sessionId);
    if (!body)
        return;
    body->turns = qMax(0, turns);
    body->tokens = qMax<qint64>(0, tokens);
    sonify::applyUsage(&body->voice, body->turns, body->tokens);
    body->size = body->voice.bloom;
    relayout();
}

void Scene::removeAgent(int sessionId)
{
    const auto sameSession = [sessionId](const Body &body) {
        return body.sessionId == sessionId;
    };
    if (m_bodies.removeIf(sameSession) == 0)
        return;
    m_pulses.removeIf([sessionId](const Pulse &pulse) {
        return pulse.sessionId == sessionId;
    });
    m_feed.removeIf([sessionId](const Action &action) {
        return action.sessionId == sessionId;
    });
    m_history.removeIf([sessionId](const Mark &mark) {
        return mark.sessionId == sessionId;
    });
    m_packets.removeIf([sessionId](const Packet &packet) {
        return packet.sessionId == sessionId;
    });
    for (auto it = m_flows.begin(); it != m_flows.end();) {
        if (it->sessionId == sessionId)
            it = m_flows.erase(it);
        else
            ++it;
    }
    for (auto it = m_joins.begin(); it != m_joins.end();) {
        if (it->a == sessionId || it->b == sessionId)
            it = m_joins.erase(it);
        else
            ++it;
    }
    // The desktop's work still running for it is the desktop's now; its own
    // subagents, shells and processes leave with it.
    m_tasks.removeIf([sessionId](const Task &task) {
        return task.sessionId == sessionId && task.kind != TaskKind::Desktop;
    });
    for (Task &task : m_tasks) {
        if (task.sessionId == sessionId)
            task.sessionId = kDesktop;
    }
    forgetAgentTasks(sessionId);
    m_processes.removeIf([sessionId](const Process &process) {
        return process.sessionId == sessionId;
    });
    m_hiddenProcesses.remove(sessionId);
    relayout();
}

void Scene::clear()
{
    m_bodies.clear();
    m_pulses.clear();
    m_joins.clear();
    m_eventTimes.clear();
    m_feed.clear();
    m_history.clear();
    std::fill(std::begin(m_lastSounded), std::end(m_lastSounded), -1e9);
    std::fill(std::begin(m_lastLanded), std::end(m_lastLanded), -1e9);
    std::fill(std::begin(m_soundCount), std::end(m_soundCount), 0);
    std::fill(std::begin(m_landCount), std::end(m_landCount), 0);
    m_desktopKinds.fill(0);
    m_rows.clear();
    std::fill(std::begin(m_lastTextSeconds), std::end(m_lastTextSeconds), -1e9);
    for (QString &text : m_lastText)
        text.clear();
    m_logs.clear();
    m_hosts.clear();
    m_flows.clear();
    m_packets.clear();
    m_traffic.clear();
    m_renamed.clear();
    seatMirrors();
    m_tasks.clear();
    m_agentTaskIds.clear();
    m_outputChecks.clear();
    m_processes.clear();
    m_hiddenProcesses.clear();
    m_desktopMelody = forkmesh::sonify::Melody();
    m_clients.clear();
    m_frames.clear();
    m_frameTimes.clear();
    m_arrivals.clear();
    m_totalFrames = 0;
    m_socket = RelaySocket();
    // The blocks stay: they are rules made at the edge, not things seen here.
}

void Scene::relayout()
{
    // Each band takes as many rows as it needs, top to bottom; the rows share
    // the height between the hosts and the desktop, none taller than a
    // comfortable step.
    m_separating = true; // let the next advance ease the new seats apart
    QVector<Body *> banded[kBandCount];
    for (Body &body : m_bodies)
        banded[int(bandFor(body.state))].append(&body);
    int rowCount = 0;
    for (QVector<Body *> &band : banded) {
        std::stable_sort(band.begin(), band.end(), [](const Body *a, const Body *b) {
            const double ax = seatX(a->voice);
            const double bx = seatX(b->voice);
            return ax != bx ? ax < bx : a->sessionId < b->sessionId;
        });
        rowCount += (int(band.size()) + kRowSize - 1) / kRowSize;
    }
    m_rows.clear();
    if (rowCount == 0)
        return;
    int bands = 0;
    for (const QVector<Body *> &band : banded)
        bands += band.isEmpty() ? 0 : 1;
    const double step =
        qMin(0.26, (kRowsBottom - kRowsTop - bands * kBandHeader) / rowCount);
    // Neighbours in the grid are as far apart as the grid allows: only a body
    // swollen past its place, or one still gliding in, is pushed.
    m_space = qMin(kPersonalSpace, step * 0.95);
    double y = kRowsTop + step / 2;
    for (int b = 0; b < kBandCount; ++b) {
        const QVector<Body *> &band = banded[b];
        if (!band.isEmpty())
            y += kBandHeader; // the strip its name is written in
        for (int first = 0; first < band.size(); first += kRowSize) {
            const int count = qMin(kRowSize, int(band.size()) - first);
            BandRow row;
            row.band = Band(b);
            row.y = y;
            row.height = step;
            row.count = first == 0 ? int(band.size()) : 0;
            m_rows.append(row);
            // Spread evenly across the row, so every body has the same room
            // for its name whatever its seat in the mix.
            const double slot = 0.88 / count;
            for (int i = 0; i < count; ++i) {
                Body *body = band.at(first + i);
                const double x = 0.06 + slot * (i + 0.5);
                // Glide from where it was drawn rather than jump: the push
                // separate() eases away starts at the old place.
                if (body->seated) {
                    body->sepX += body->x - x;
                    body->sepY += body->y - y;
                }
                body->seated = true;
                body->x = x;
                body->y = y;
                body->slot = slot;
            }
            y += step;
        }
    }
}

void Scene::touchJoins(int sessionId)
{
    const Body *self = find(sessionId);
    if (!self)
        return;
    // Only a real shared subject joins two agents: the same file or page,
    // close enough together to be the same job. Acting in the same second is
    // not a connection, and drawing it as one said something untrue.
    if (self->subject.isEmpty() || m_now - self->subjectSeconds >= 0.01)
        return;
    for (const Body &other : m_bodies) {
        if (other.sessionId == sessionId || other.subject != self->subject
            || m_now - other.subjectSeconds > kSharedWindow)
            continue;
        Join &join = m_joins[joinKey(sessionId, other.sessionId)];
        join.a = qMin(sessionId, other.sessionId);
        join.b = qMax(sessionId, other.sessionId);
        join.strength = qMin(1.0, join.strength + 0.6);
        join.lastSeconds = m_now;
        join.meetings += 1;
        join.why = QStringLiteral("both on ") + shortSubject(self->subject);
        join.shared = true;
    }
}

void Scene::record(int sessionId, Motion motion, const QString &text)
{
    if (text.isEmpty())
        return;
    // A reply streaming a token at a time, or the same command twice running,
    // is one line that counts up rather than a feed full of copies.
    for (int i = m_feed.size() - 1; i >= 0 && i >= m_feed.size() - 4; --i) {
        Action &recent = m_feed[i];
        if (recent.sessionId == sessionId && recent.text == text
            && m_now - recent.seconds < 20.0) {
            recent.repeats += 1;
            recent.seconds = m_now;
            if (i != m_feed.size() - 1)
                m_feed.move(i, m_feed.size() - 1);
            return;
        }
    }
    Action action;
    action.sessionId = sessionId;
    action.motion = motion;
    action.text = text;
    action.seconds = m_now;
    m_feed.append(action);
    if (m_feed.size() > kFeedSize)
        m_feed.remove(0, m_feed.size() - kFeedSize);
}

bool Scene::ingest(int sessionId, const QString &title, const QJsonObject &event,
                   bool heard)
{
    watchAgentWork(addAgent(sessionId, title).sessionId, event);
    const Motion motion = sonify::motionForEvent(event);
    if (motion == Motion::Rest) {
        // Not work: no note, and so no mark. The agent is still put in the
        // field by it - a run whose only traffic is bookkeeping is still a run
        // you should be able to see sitting there.
        addAgent(sessionId, title);
        return false;
    }
    if (sonify::isBookkeeping(motion)) {
        // Bookkeeping: not work, so it does not wake the agent, join it to
        // anyone or write a line - but it is in the log, so it ticks and it
        // leaves the faintest mark, exactly as the music plays it.
        Body &body = addAgent(sessionId, title);
        const int id = body.sessionId;
        if (heard) {
            caption(id, motion, labelForEvent(event));
        } else {
            const sonify::Note played =
                sonify::noteForEvent(event, body.voice, &body.melody, m_now);
            if (!played.isRest())
                note(id, motion, played.startSeconds - m_now, played.gain,
                     /*heard=*/false, labelForEvent(event), played.seed);
        }
        return false;
    }
    Body &body = addAgent(sessionId, title);
    body.events += 1;
    body.lastMotion = motion;
    body.lastEventSeconds = m_now;
    const QString doing = actionForEvent(event);
    if (!doing.isEmpty()) {
        body.doing = doing;
        body.doingSeconds = m_now;
    }
    const QString subject = subjectForEvent(event);
    if (!subject.isEmpty()) {
        body.subject = subject;
        body.subjectSeconds = m_now;
    }
    if (motion == Motion::Prompt) {
        const QString prompt = promptForEvent(event);
        if (!prompt.isEmpty()) {
            body.lastPrompt = prompt;
            body.lastPromptSeconds = m_now;
        }
    }
    if (const QString said = proseForEvent(event); !said.isEmpty() && said != body.said) {
        body.said = said;
        body.saidSeconds = m_now;
    }
    const int sessionIdCopy = body.sessionId;

    m_totalEvents += 1;
    m_eventTimes.append(m_now);
    record(sessionIdCopy, motion, doing);
    touchJoins(sessionIdCopy);

    if (heard) {
        // The sonifier placed the note, and note() the mark, a moment ago:
        // tell the mark what it was for.
        caption(sessionIdCopy, motion, doing.isEmpty() ? labelForEvent(event) : doing);
        return true;
    }

    // Nobody is listening, so the field works out the note itself, on its own
    // clock, exactly as the sonifier would: same melody, same grid, same
    // rule for dropping a note the run has outrun. The mark is drawn for the
    // note, so an unheard agent is marked the way it would sound.
    const sonify::Note played =
        sonify::noteForEvent(event, body.voice, &body.melody, m_now);
    if (played.isRest())
        return true;
    note(sessionIdCopy, motion, played.startSeconds - m_now, played.gain,
         /*heard=*/false, doing.isEmpty() ? labelForEvent(event) : doing,
         played.seed);
    return true;
}

void Scene::cue(int sessionId, Motion motion, const QString &text, quint32 seed,
               bool heard)
{
    if (!sonify::isCue(motion))
        return;
    Body *body = sessionId == kDesktop ? nullptr : find(sessionId);
    Host *mirror = body ? nullptr : mirrorHost(sessionId);
    if (!body && !mirror)
        sessionId = kDesktop;
    // A line nobody hears as its agent's is heard as the desktop's.
    if (heard && body && !body->listening)
        sessionId = kDesktop;
    if (heard) {
        // The sonifier placed the note, and note() the mark, a moment ago -
        // on whichever voice played it.
        caption(sessionId, motion, text);
        if (sessionId != kDesktop)
            caption(kDesktop, motion, text);
        return;
    }
    sonify::Melody &melody = body ? body->melody
                                  : (mirror ? mirror->melody : m_desktopMelody);
    const sonify::Voice voice = body ? body->voice : sonify::desktopVoice();
    const sonify::Note played = sonify::cueNote(motion, voice, &melody, m_now, seed);
    if (played.isRest())
        return;
    note(sessionId, motion, played.startSeconds - m_now, played.gain,
         /*heard=*/false, text, played.seed);
}

void Scene::caption(int sessionId, Motion motion, const QString &text)
{
    if (text.isEmpty())
        return;
    for (int i = m_pulses.size() - 1; i >= 0; --i) {
        Pulse &pulse = m_pulses[i];
        if (m_now - pulse.createdSeconds > 0.5)
            break;
        if (pulse.sessionId == sessionId && pulse.motion == motion && pulse.text.isEmpty()) {
            pulse.text = text;
            if (pulse.struck)
                noteText(motion, text, pulse.bornSeconds);
            // Already landed (no delay): its history entry has no words yet.
            for (int j = m_history.size() - 1; j >= 0 && j >= m_history.size() - 8; --j) {
                Mark &mark = m_history[j];
                if (mark.sessionId == sessionId && mark.motion == motion
                    && mark.text.isEmpty() && qFuzzyCompare(1.0 + mark.seconds,
                                                            1.0 + pulse.bornSeconds)) {
                    mark.text = text;
                    break;
                }
            }
            return;
        }
    }
}

void Scene::note(int sessionId, Motion motion, double delaySeconds,
                 double gain, bool heard, const QString &text,
                 quint32 seed)
{
    if (motion == Motion::Rest)
        return;
    Body *body = find(sessionId);
    // The desktop has no body: its marks land on its node at the bottom, and
    // a mirror's land on its seat along the top.
    if (!body && sessionId != kDesktop && !mirrorHost(sessionId))
        return;
    // Loudness is the mark's strength: the sonifier's gains run about 0.1
    // (thinking) to 0.3 (a prompt, the cadence).
    const double strength = qBound(0.15, gain / 0.3, 1.0);
    const double born = m_now + qMax(0.0, delaySeconds);

    // A run of the same gesture off one body landing on the same beat feeds
    // the ripple already there rather than stacking another on top of it.
    for (Pulse &pulse : m_pulses) {
        if (pulse.sessionId != sessionId || pulse.motion != motion)
            continue;
        if (std::abs(born - pulse.bornSeconds) > kMergeWindow)
            continue;
        pulse.strength = qMin(1.0, pulse.strength + 0.25 * strength);
        if (heard && !pulse.heard) {
            // The field had drawn this note for itself; it is really sounding.
            pulse.heard = true;
            if (pulse.struck) {
                const int index = int(motion);
                m_lastSounded[index] = qMax(m_lastSounded[index], pulse.bornSeconds);
                m_soundCount[index] += 1;
            }
        }
        return;
    }
    Pulse &pulse = addPulse(sessionId, motion, born, strength);
    pulse.heard = heard;
    pulse.text = text;
    pulse.seed = seed;
    pulse.createdSeconds = m_now;
    if (!pulse.pending(m_now))
        strike(pulse);
}

Pulse &Scene::addPulse(int sessionId, Motion motion, double bornSeconds,
                       double strength)
{
    const Body *body = find(sessionId);
    Host *mirror = body ? nullptr : mirrorHost(sessionId);
    const int events = body ? body->events
                            : (mirror ? mirror->marks++ : m_desktopMarks++);
    const MotionStyle &style = styleFor(motion);
    Pulse pulse;
    pulse.sessionId = sessionId;
    pulse.motion = motion;
    pulse.bornSeconds = bornSeconds;
    pulse.seconds = style.seconds;
    pulse.strength = qBound(0.15, strength, 1.0);
    // The golden angle, so consecutive marks off one body fan out instead of
    // landing on each other.
    pulse.angle = std::fmod(events * 0.381966 + jitter(sessionId, 3), 1.0);
    if (m_pulses.size() >= kMaxPulses)
        m_pulses.removeFirst();
    m_pulses.append(pulse);
    return m_pulses.last();
}

void Scene::strike(Pulse &pulse)
{
    pulse.struck = true;
    land(pulse);
    Body *body = find(pulse.sessionId);
    if (!body)
        return;
    if (sonify::isFaint(pulse.motion)) {
        // A tick is a glint, not a gesture: it barely lights the body.
        body->energy = qMin(1.0, body->energy + 0.04);
        return;
    }
    body->energy = qMin(1.0, body->energy + 0.25 + 0.45 * pulse.strength);
    body->lastNoteSeconds = pulse.bornSeconds;
}

void Scene::separate(double elapsed)
{
    const double glide = 1.0 - std::exp(-elapsed / kGlideSeconds);

    // Personal space. Where every body would be, relaxed a few times so no two
    // overlap; the push is then eased in, so neighbours part slowly. The field
    // is wider than it is tall, so x counts for less.
    constexpr double kAspect = 1.6;
    const int count = int(m_bodies.size());
    QVector<double> px(count), py(count), ox(count, 0.0), oy(count, 0.0);
    for (int i = 0; i < count; ++i) {
        const Body &body = m_bodies.at(i);
        px[i] = body.x * kAspect;
        py[i] = body.y;
    }
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < count; ++i) {
            for (int j = i + 1; j < count; ++j) {
                double ddx = (px[j] + ox[j]) - (px[i] + ox[i]);
                double ddy = (py[j] + oy[j]) - (py[i] + oy[i]);
                double distance = std::sqrt(ddx * ddx + ddy * ddy);
                if (distance >= m_space)
                    continue;
                if (distance < 1e-6) {
                    // Exactly on top of each other: part them by their ids.
                    ddx = jitter(m_bodies.at(i).sessionId, 17) - 0.5;
                    ddy = jitter(m_bodies.at(j).sessionId, 19) - 0.5;
                    distance = std::sqrt(ddx * ddx + ddy * ddy) + 1e-6;
                }
                const double push = (m_space - distance) / 2.0;
                ox[i] -= ddx / distance * push;
                oy[i] -= ddy / distance * push;
                ox[j] += ddx / distance * push;
                oy[j] += ddy / distance * push;
            }
        }
    }
    m_separating = false;
    for (int i = 0; i < count; ++i) {
        Body &body = m_bodies[i];
        const double toX = ox[i] / kAspect - body.sepX;
        const double toY = oy[i] - body.sepY;
        if (std::abs(toX) < 1e-4 && std::abs(toY) < 1e-4) {
            body.sepX += toX;
            body.sepY += toY;
        } else {
            body.sepX += toX * glide;
            body.sepY += toY * glide;
            m_separating = true;
        }
        // Wherever it is pushed, it stays in the field and below the hosts' strip.
        body.dx = qBound(0.03, body.x + body.sepX, 0.97) - body.x;
        body.dy = qBound(0.12, body.y + body.sepY, 0.95) - body.y;
    }
}

void Scene::finish(int sessionId, bool success)
{
    Body *body = find(sessionId);
    if (!body)
        return;
    body->state = success ? State::Done : State::Failed;
    body->lastEventSeconds = m_now;
    body->energy = qMin(1.0, body->energy + 0.5);

    const Motion motion = success ? Motion::Finish : Motion::Notice;
    const MotionStyle &style = styleFor(motion);
    // The ending is drawn the way the cadence is played: bigger, slower and
    // louder than anything the stream itself puts out.
    Pulse pulse;
    pulse.sessionId = sessionId;
    pulse.motion = motion;
    pulse.bornSeconds = m_now;
    pulse.seconds = style.seconds * 1.6;
    pulse.strength = 1.0;
    pulse.angle = 0;
    pulse.struck = true;
    pulse.createdSeconds = m_now;
    pulse.text = success ? QStringLiteral("run finished") : QStringLiteral("run failed or stopped");
    if (m_pulses.size() >= kMaxPulses)
        m_pulses.removeFirst();
    m_pulses.append(pulse);
    land(pulse);
}

QVector<Join> Scene::joins() const
{
    QVector<Join> live;
    live.reserve(m_joins.size());
    for (const Join &join : m_joins)
        live.append(join);
    std::sort(live.begin(), live.end(), [](const Join &a, const Join &b) {
        if (!qFuzzyCompare(1 + a.strength, 1 + b.strength))
            return a.strength < b.strength;
        return joinKey(a.a, a.b) < joinKey(b.a, b.b);
    });
    return live;
}

int Scene::activeCount() const
{
    int count = 0;
    for (const Body &body : m_bodies) {
        if (m_now - body.lastEventSeconds <= kJoinWindow)
            ++count;
    }
    return count;
}

QString addressInfo(const QString &peer)
{
    if (peer.contains(QLatin1Char(':'))) {
        const QString lower = peer.toLower();
        const QString kind = QStringLiteral("IPv6, ");
        if (lower == QLatin1String("::1"))
            return kind + QStringLiteral("loopback");
        if (lower.startsWith(QLatin1String("fe8")) || lower.startsWith(QLatin1String("fe9"))
            || lower.startsWith(QLatin1String("fea")) || lower.startsWith(QLatin1String("feb")))
            return kind + QStringLiteral("link-local");
        if (lower.startsWith(QLatin1String("fc")) || lower.startsWith(QLatin1String("fd")))
            return kind + QStringLiteral("private (unique local)");
        return kind + QStringLiteral("public");
    }
    const QStringList parts = peer.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return {};
    int octet[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        octet[i] = parts.at(i).toInt(&ok);
        if (!ok || octet[i] < 0 || octet[i] > 255)
            return {};
    }
    const QString kind = QStringLiteral("IPv4, ");
    if (octet[0] == 127)
        return kind + QStringLiteral("loopback");
    if (octet[0] == 10 || (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31)
        || (octet[0] == 192 && octet[1] == 168))
        return kind + QStringLiteral("private (LAN)");
    if (octet[0] == 100 && octet[1] >= 64 && octet[1] <= 127)
        return kind + QStringLiteral("shared (carrier NAT)");
    if (octet[0] == 169 && octet[1] == 254)
        return kind + QStringLiteral("link-local");
    return kind + QStringLiteral("public");
}

QString portService(quint16 port)
{
    switch (port) {
    case 22: return QStringLiteral("ssh");
    case 25: case 465: case 587: return QStringLiteral("smtp");
    case 53: return QStringLiteral("dns");
    case 80: return QStringLiteral("http");
    case 143: case 993: return QStringLiteral("imap");
    case 443: return QStringLiteral("https");
    case 853: return QStringLiteral("dns over tls");
    case 3306: return QStringLiteral("mysql");
    case 5432: return QStringLiteral("postgres");
    case 6379: return QStringLiteral("redis");
    case 8080: case 8000: case 3000: return QStringLiteral("http (dev)");
    case 8443: return QStringLiteral("https (alt)");
    case 9418: return QStringLiteral("git");
    case 11434: return QStringLiteral("ollama");
    default: return {};
    }
}

QString formatRate(double bytesPerSecond)
{
    if (bytesPerSecond >= 1e6)
        return QStringLiteral("%1 MB/s").arg(bytesPerSecond / 1e6, 0, 'f', 1);
    if (bytesPerSecond >= 1e3)
        return QStringLiteral("%1 KB/s").arg(bytesPerSecond / 1e3, 0, 'f', 0);
    return QStringLiteral("%1 B/s").arg(qRound(bytesPerSecond));
}

QString formatBytes(qint64 bytes)
{
    if (bytes >= 1000000)
        return QStringLiteral("%1 MB").arg(double(bytes) / 1e6, 0, 'f', 1);
    if (bytes >= 1000)
        return QStringLiteral("%1 KB").arg(double(bytes) / 1e3, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

QString describeEndpoint(const QString &peer, quint16 port)
{
    QString text = peer.contains(QLatin1Char(':')) && port
                       ? QLatin1Char('[') + peer + QLatin1Char(']')
                       : peer;
    if (port)
        text += QLatin1Char(':') + QString::number(port);
    const QString info = addressInfo(peer);
    if (!info.isEmpty())
        text += QStringLiteral(" · ") + info;
    if (port) {
        const QString service = portService(port);
        text += service.isEmpty() ? QStringLiteral(" · TCP")
                                  : QStringLiteral(" · TCP ") + service;
    }
    return text;
}

QStringList parseWhois(const QString &text)
{
    // Registry field names for each fact: ARIN, then RIPE/APNIC/AFRINIC,
    // then LACNIC and the smaller registries.
    struct Field {
        const char *label;
        QStringList keys;
    };
    const Field fields[] = {
        {"network", {QStringLiteral("netname")}},
        {"range", {QStringLiteral("netrange"), QStringLiteral("inetnum"),
                   QStringLiteral("inet6num")}},
        {"cidr", {QStringLiteral("cidr"), QStringLiteral("route"), QStringLiteral("route6")}},
        {"owner", {QStringLiteral("orgname"), QStringLiteral("org-name"),
                   QStringLiteral("organization"), QStringLiteral("owner")}},
        {"about", {QStringLiteral("descr")}},
        {"country", {QStringLiteral("country")}},
        {"AS", {QStringLiteral("originas"), QStringLiteral("origin"), QStringLiteral("aut-num")}},
        {"abuse", {QStringLiteral("orgabuseemail"), QStringLiteral("abuse-mailbox")}},
        {"registered", {QStringLiteral("regdate"), QStringLiteral("created")}},
        {"updated", {QStringLiteral("updated"), QStringLiteral("last-modified"),
                     QStringLiteral("changed")}},
    };
    constexpr int kFields = int(sizeof(fields) / sizeof(fields[0]));
    QString found[kFields];
    QString lastKey;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            lastKey.clear();
            continue;
        }
        if (line.startsWith(QLatin1Char('%')) || line.startsWith(QLatin1Char('#')))
            continue;
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed().toLower();
        const QString value = line.mid(colon + 1).simplified();
        // "descr" runs over several lines: keep the first of a run.
        const bool continued = key == lastKey;
        lastKey = key;
        if (value.isEmpty() || (continued && key == QLatin1String("descr")))
            continue;
        for (int i = 0; i < kFields; ++i) {
            if (fields[i].keys.contains(key)) {
                found[i] = value;
                break;
            }
        }
    }
    QStringList lines;
    for (int i = 0; i < kFields; ++i) {
        if (!found[i].isEmpty())
            lines << QString::fromLatin1(fields[i].label) + QStringLiteral(": ") + found[i];
    }
    return lines;
}

namespace {

// Folds bytes on one address:port into a capped, most-recent-first list.
void noteEndpoint(QList<Endpoint> &endpoints, const Endpoint &seen)
{
    Endpoint merged = seen;
    for (int i = 0; i < endpoints.size(); ++i) {
        if (endpoints[i].address == seen.address && endpoints[i].port == seen.port) {
            merged.up += endpoints[i].up;
            merged.down += endpoints[i].down;
            merged.lastSeconds = qMax(merged.lastSeconds, endpoints[i].lastSeconds);
            endpoints.removeAt(i);
            break;
        }
    }
    int at = 0;
    while (at < endpoints.size() && endpoints[at].lastSeconds > merged.lastSeconds)
        ++at;
    endpoints.insert(at, merged);
    while (endpoints.size() > Scene::kMaxEndpoints)
        endpoints.removeLast();
}

void mergeEndpoints(QList<Endpoint> &into, const QList<Endpoint> &from)
{
    for (const Endpoint &endpoint : from)
        noteEndpoint(into, endpoint);
}

} // namespace

bool Scene::network(int sessionId, const QString &named, qint64 up, qint64 down,
                    const QString &address, quint16 port, qint64 pid, const QString &process)
{
    if (named.isEmpty() || (up <= 0 && down <= 0))
        return false;
    // One of your mirrors by any of its addresses, whatever a lookup made of it.
    QString hostName = m_mirrorByAddress.value(address.isEmpty() ? named : address);
    if (hostName.isEmpty())
        hostName = m_mirrorByAddress.value(named, named);
    up = qMax<qint64>(0, up);
    down = qMax<qint64>(0, down);
    Host *host = nullptr;
    bool joined = false;
    for (Host &candidate : m_hosts) {
        if (candidate.name == hostName) {
            host = &candidate;
            break;
        }
    }
    if (!host) {
        const auto others = std::count_if(m_hosts.cbegin(), m_hosts.cend(),
                                          [](const Host &h) { return !h.mirror; });
        if (others >= kMaxHosts) {
            // Full: the quietest host makes room. A mirror keeps its seat.
            auto quietest = std::min_element(m_hosts.begin(), m_hosts.end(),
                                             [](const Host &a, const Host &b) {
                                                 if (a.mirror != b.mirror)
                                                     return !a.mirror;
                                                 return a.lastSeconds < b.lastSeconds;
                                             });
            m_hosts.erase(quietest);
        }
        Host fresh;
        fresh.name = hostName;
        fresh.bornSeconds = m_now;
        m_hosts.append(fresh);
        host = &m_hosts.last();
        settleHosts(0);
        host->x = host->slot;
        joined = true;
    }
    host->lastSeconds = m_now;
    host->up += up;
    host->down += down;
    host->upRecent += double(up);
    host->downRecent += double(down);

    const QString flowKey = QString::number(sessionId) + QLatin1Char('|') + hostName;
    // A mirror was already on the field: what is new is this agent reaching it.
    if (host->mirror && !m_flows.contains(flowKey))
        joined = true;
    Flow &flow = m_flows[flowKey];
    flow.sessionId = sessionId;
    flow.host = hostName;
    flow.upRecent += double(up);
    flow.downRecent += double(down);
    flow.lastSeconds = m_now;

    if (!address.isEmpty()) {
        const Endpoint seen{address, port, up, down, m_now};
        noteEndpoint(host->endpoints, seen);
        noteEndpoint(flow.endpoints, seen);
    }

    Traffic record;
    record.serial = ++m_trafficSerial;
    record.seconds = m_now;
    record.sessionId = sessionId;
    record.host = hostName;
    record.address = address.isEmpty() ? named : address;
    record.port = port;
    record.pid = pid;
    record.process = process;
    record.up = up;
    record.down = down;
    m_traffic.append(record);
    if (m_traffic.size() >= kMaxTraffic + kMaxTraffic / 8)
        m_traffic.remove(0, m_traffic.size() - kMaxTraffic);

    if (sessionId != kDesktop) {
        if (Body *body = find(sessionId)) {
            body->netUp += up;
            body->netDown += down;
            body->netUpRecent += double(up);
            body->netDownRecent += double(down);
            body->lastNetSeconds = m_now;
        }
    }

    // The packets: a little stream of them for each direction, more and
    // bigger the more bytes moved, launched a beat apart so they read as a
    // flow and not a single blob.
    const auto launch = [&](qint64 bytes, bool outward) {
        if (bytes <= 0)
            return;
        const int count = qBound(1, 1 + int(std::log2(1.0 + double(bytes) / 2048.0)), 6);
        const double size = qBound(0.2, std::log10(1.0 + double(bytes)) / 6.0, 1.0);
        for (int i = 0; i < count; ++i) {
            Packet packet;
            packet.sessionId = sessionId;
            packet.host = hostName;
            packet.up = outward;
            packet.bornSeconds = m_now + i * 0.07;
            // The first lands exactly when its note sounds (networkNote).
            packet.seconds = forkmesh::sonify::kPacketFlightSeconds
                             + (i == 0 ? 0.0 : 0.25 * jitter(sessionId + i, int(bytes % 97)));
            packet.size = size;
            if (m_packets.size() >= kMaxPackets)
                m_packets.removeFirst();
            m_packets.append(packet);
        }
    };
    launch(up, true);
    launch(down, false);
    return joined;
}

QString Scene::hostNamed(const QString &name) const
{
    QString current = name;
    for (int hop = 0; hop < 16; ++hop) {
        const auto next = m_renamed.constFind(current);
        if (next == m_renamed.constEnd() || next.value() == current)
            break;
        current = next.value();
    }
    return current;
}

QVector<Traffic> Scene::traffic(int sessionId, const QString &host, quint64 after) const
{
    QVector<Traffic> found;
    const QString name = host.isEmpty() ? QString() : hostNamed(host);
    auto from = std::upper_bound(m_traffic.cbegin(), m_traffic.cend(), after,
                                 [](quint64 serial, const Traffic &record) {
                                     return serial < record.serial;
                                 });
    for (; from != m_traffic.cend(); ++from) {
        if (sessionId != kAnySession && from->sessionId != sessionId)
            continue;
        if (!name.isEmpty() && from->host != name)
            continue;
        found.append(*from);
    }
    return found;
}

QVector<Connection> Scene::connections(int sessionId, const QString &host) const
{
    QHash<QString, int> index;
    QVector<Connection> found;
    const double recent = m_now - kConnectionRateSeconds;
    for (const Traffic &record : traffic(sessionId, host)) {
        const QString key = QString::number(record.sessionId) + QLatin1Char('|')
                            + QString::number(record.pid) + QLatin1Char('|') + record.address
                            + QLatin1Char('|') + QString::number(record.port);
        auto at = index.constFind(key);
        if (at == index.constEnd()) {
            Connection fresh;
            fresh.sessionId = record.sessionId;
            fresh.address = record.address;
            fresh.port = record.port;
            fresh.pid = record.pid;
            fresh.firstSeconds = record.seconds;
            at = index.insert(key, int(found.size()));
            found.append(fresh);
        }
        Connection &connection = found[at.value()];
        connection.host = record.host;
        if (!record.process.isEmpty())
            connection.process = record.process;
        connection.up += record.up;
        connection.down += record.down;
        connection.lastSeconds = record.seconds;
        ++connection.samples;
        if (record.seconds > recent) {
            connection.upRate += double(record.up) / kConnectionRateSeconds;
            connection.downRate += double(record.down) / kConnectionRateSeconds;
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Connection &a, const Connection &b) {
        if (a.lastSeconds != b.lastSeconds)
            return a.lastSeconds > b.lastSeconds;
        return a.up + a.down > b.up + b.down;
    });
    return found;
}

void Scene::setAddressNetwork(const QString &address, const QStringList &lines)
{
    if (!address.isEmpty())
        m_addressNetwork.insert(address, lines);
}

void Scene::renameHost(const QString &from, const QString &to)
{
    if (from == to || to.isEmpty())
        return;
    const Host *named = host(from);
    if (named && named->mirror)
        return;
    m_renamed.remove(to);
    m_renamed.insert(from, to);
    for (Traffic &record : m_traffic) {
        if (record.host == from)
            record.host = to;
    }
    int existing = -1;
    for (int i = 0; i < m_hosts.size(); ++i) {
        if (m_hosts[i].name == to)
            existing = i;
    }
    for (int i = 0; i < m_hosts.size(); ++i) {
        if (m_hosts[i].name != from)
            continue;
        if (existing >= 0) {
            // Two addresses of one name: fold the old into the named one,
            // keeping where it was reached and what it moved.
            Host &into = m_hosts[existing];
            const Host &old = m_hosts[i];
            into.up += old.up;
            into.down += old.down;
            into.upRecent += old.upRecent;
            into.downRecent += old.downRecent;
            into.lastSeconds = qMax(into.lastSeconds, old.lastSeconds);
            mergeEndpoints(into.endpoints, old.endpoints);
            m_hosts.removeAt(i);
        } else {
            m_hosts[i].name = to;
        }
        break;
    }
    QHash<QString, Flow> renamed;
    for (Flow flow : std::as_const(m_flows)) {
        if (flow.host == from)
            flow.host = to;
        Flow &into = renamed[QString::number(flow.sessionId) + QLatin1Char('|') + flow.host];
        into.sessionId = flow.sessionId;
        into.host = flow.host;
        into.upRecent += flow.upRecent;
        into.downRecent += flow.downRecent;
        into.lastSeconds = qMax(into.lastSeconds, flow.lastSeconds);
        mergeEndpoints(into.endpoints, flow.endpoints);
    }
    m_flows = renamed;
    for (Packet &packet : m_packets) {
        if (packet.host == from)
            packet.host = to;
    }
}

void Scene::setMirrors(const QList<MirrorSeat> &mirrors)
{
    m_mirrors.clear();
    QSet<QString> names;
    for (const MirrorSeat &seat : mirrors) {
        const QString name = seat.name.trimmed();
        if (name.isEmpty() || names.contains(name))
            continue;
        names.insert(name);
        MirrorSeat kept{name, {}};
        for (const QString &address : seat.addresses) {
            const QString trimmed = address.trimmed();
            if (!trimmed.isEmpty() && !kept.addresses.contains(trimmed))
                kept.addresses.append(trimmed);
        }
        m_mirrors.append(kept);
        if (m_mirrors.size() >= kMaxMirrors)
            break;
    }
    seatMirrors();
}

void Scene::seatMirrors()
{
    m_mirrorByAddress.clear();
    QSet<QString> listed;
    for (const MirrorSeat &seat : std::as_const(m_mirrors)) {
        listed.insert(seat.name);
        for (const QString &address : seat.addresses) {
            if (address != seat.name)
                m_mirrorByAddress.insert(address, seat.name);
        }
    }
    // A mirror no longer listed is an ordinary host again.
    for (Host &host : m_hosts)
        host.mirror = listed.contains(host.name);
    for (const MirrorSeat &seat : std::as_const(m_mirrors)) {
        if (!host(seat.name)) {
            Host fresh;
            fresh.name = seat.name;
            fresh.bornSeconds = m_now;
            // Nobody has talked to it yet: it sits dim until someone does.
            fresh.lastSeconds = m_now - kHostLinger;
            fresh.mirror = true;
            m_hosts.append(fresh);
        }
        // Traffic drawn before the mirror was known, under its address or
        // the name that address resolved to, is the mirror's.
        for (const QString &address : seat.addresses)
            renameHost(address, seat.name);
    }
    QList<QPair<QString, QString>> reached;
    for (const Host &host : std::as_const(m_hosts)) {
        if (host.mirror)
            continue;
        for (const Endpoint &endpoint : host.endpoints) {
            const QString mirror = m_mirrorByAddress.value(endpoint.address);
            if (!mirror.isEmpty()) {
                reached.append({host.name, mirror});
                break;
            }
        }
    }
    for (const auto &[from, to] : std::as_const(reached))
        renameHost(from, to);
    // Each seated mirror is a source of its own, by its place in the list, so
    // its marks keep their id for as long as it is listed.
    for (Host &host : m_hosts) {
        if (!host.mirror) {
            host.source = 0;
            continue;
        }
        host.source = 0;
        for (int i = 0; i < m_mirrors.size() && i < kMirrorSources; ++i) {
            if (m_mirrors.at(i).name == host.name) {
                host.source = kMirrorFirst - i;
                break;
            }
        }
    }
    // Mirrors to the left, in the order they are listed; the rest keep theirs.
    std::stable_sort(m_hosts.begin(), m_hosts.end(), [this](const Host &a, const Host &b) {
        if (a.mirror != b.mirror)
            return a.mirror;
        if (!a.mirror)
            return false;
        const auto at = [this](const QString &name) {
            for (int i = 0; i < m_mirrors.size(); ++i) {
                if (m_mirrors.at(i).name == name)
                    return i;
            }
            return int(m_mirrors.size());
        };
        return at(a.name) < at(b.name);
    });
    settleHosts(0);
    for (Host &host : m_hosts) {
        if (host.mirror && host.bornSeconds == m_now)
            host.x = host.slot;
    }
}

int Scene::mirrorSource(const QString &name) const
{
    const Host *seat = host(name);
    return seat && seat->mirror ? seat->source : 0;
}

const Host *Scene::mirrorForSource(int sessionId) const
{
    if (!isMirrorSource(sessionId))
        return nullptr;
    for (const Host &candidate : m_hosts) {
        if (candidate.mirror && candidate.source == sessionId)
            return &candidate;
    }
    return nullptr;
}

Host *Scene::mirrorHost(int sessionId)
{
    return const_cast<Host *>(mirrorForSource(sessionId));
}

int Scene::mirrorForLog(const QString &badge, const QString &text) const
{
    // forkmesh::hosts::mainLogLine: "Mirror <node> <source>: <line>", under
    // the badge every forwarded host line is written with.
    if (badge.compare(QLatin1String("MIRROR"), Qt::CaseInsensitive) != 0)
        return 0;
    if (!text.startsWith(QLatin1String("Mirror ")))
        return 0;
    const int start = 7;
    const int space = text.indexOf(QLatin1Char(' '), start);
    const QString node = text.mid(start, space < 0 ? -1 : space - start);
    if (node.isEmpty())
        return 0;
    for (const Host &candidate : m_hosts) {
        if (candidate.mirror && candidate.name.compare(node, Qt::CaseInsensitive) == 0)
            return candidate.source;
    }
    return 0;
}

int Scene::lineRate(int sessionId) const
{
    int count = 0;
    for (const LogLine &line : m_logs) {
        if (line.sessionId == sessionId && m_now - line.seconds <= 60.0)
            count += line.repeats;
    }
    return count;
}

const Host *Scene::host(const QString &name) const
{
    for (const Host &candidate : m_hosts) {
        if (candidate.name == name)
            return &candidate;
    }
    return nullptr;
}

double Scene::upRate() const
{
    double recent = 0;
    for (const Host &host : m_hosts)
        recent += host.upRecent;
    return rateOf(recent);
}

double Scene::downRate() const
{
    double recent = 0;
    for (const Host &host : m_hosts)
        recent += host.downRecent;
    return rateOf(recent);
}

void Scene::settleHosts(double elapsed)
{
    if (elapsed > 0) {
        for (Host &host : m_hosts) {
            host.upRecent = decay(host.upRecent, elapsed, kRateHalfLife);
            host.downRecent = decay(host.downRecent, elapsed, kRateHalfLife);
        }
        for (auto it = m_flows.begin(); it != m_flows.end();) {
            it->upRecent = decay(it->upRecent, elapsed, kRateHalfLife);
            it->downRecent = decay(it->downRecent, elapsed, kRateHalfLife);
            if (m_now - it->lastSeconds > kHostLinger)
                it = m_flows.erase(it);
            else
                ++it;
        }
        for (Body &body : m_bodies) {
            body.netUpRecent = decay(body.netUpRecent, elapsed, kRateHalfLife);
            body.netDownRecent = decay(body.netDownRecent, elapsed, kRateHalfLife);
        }
        m_packets.removeIf([this](const Packet &packet) {
            return m_now - packet.bornSeconds >= packet.seconds;
        });
        const int before = m_hosts.size();
        m_hosts.removeIf([this](const Host &host) {
            return !host.mirror && m_now - host.lastSeconds > kHostLinger;
        });
        if (m_hosts.size() != before) {
            m_packets.removeIf([this](const Packet &packet) { return !host(packet.host); });
        }
    }
    // Hosts share the top of the field evenly, in the order they appeared, and
    // glide to a new slot when one arrives or leaves.
    const int count = m_hosts.size();
    for (int i = 0; i < count; ++i) {
        Host &host = m_hosts[i];
        host.slot = count == 1 ? 0.5 : 0.08 + 0.84 * double(i) / double(count - 1);
        const double glide = 1.0 - std::exp(-qMax(0.0, elapsed) / 0.8);
        host.x += (host.slot - host.x) * glide;
    }
}

int Scene::workingCount() const
{
    int count = 0;
    for (const Body &body : m_bodies) {
        if (body.working(m_now))
            ++count;
    }
    return count;
}

int Scene::eventRate() const
{
    return int(m_eventTimes.size());
}

quint32 Scene::logSeed(const QString &badge, const QString &text)
{
    // Digits stripped, so "retry 3 of 5" and "retry 4 of 5" shade alike and a
    // different message does not.
    static const QRegularExpression digits(QStringLiteral("\\d+"));
    QString shape = badge + QLatin1Char('|') + text.left(80);
    shape.remove(digits);
    const quint32 seed = quint32(qHash(shape, 0x2f6b1d3u));
    return seed ? seed : 1u;
}

bool Scene::outputLine(const QString &text)
{
    static const QRegularExpression forwarded(QStringLiteral(
        "^(?:Node [nr][0-9]{1,4} (?:core|runner) [A-Za-z0-9@._:-]+ [a-z]+"
        "|Mirror \\S+ (?:node|mirror|web|ssh|system)): "));
    return forwarded.match(text).hasMatch();
}

int Scene::agentForLog(const QString &text) const
{
    static const QRegularExpression mention(
        QStringLiteral("(?:#|\\b(?:agent|session|run)\\s+#?)(-?\\d{1,9})\\b"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = mention.globalMatch(text);
    while (it.hasNext()) {
        bool ok = false;
        const int id = it.next().captured(1).toInt(&ok);
        if (ok && body(id))
            return id;
    }
    return kDesktop;
}

int Scene::ownerForLog(const QString &text) const
{
    const int named = agentForLog(text);
    return working(named) ? named : kDesktop;
}

int Scene::log(const QString &badge, const QString &accent, const QString &text,
               double ageSeconds, bool heard)
{
    const QString line = oneLine(text, 400);
    if (line.isEmpty())
        return kDesktop;
    // A forwarded host line belongs to the mirror it was streamed from; any
    // other line to the agent it names, or to the desktop.
    const int mirror = mirrorForLog(badge, line);
    const int sessionId = mirror != 0 ? mirror : ownerForLog(line);
    const double seconds = m_now - qMax(0.0, ageSeconds);
    const Motion motion = sonify::motionForLogBadge(badge);
    // Every new line sounds and leaves its kind's mark - off the agent it
    // names, or off the desktop - repeats included: a retry loop is heard
    // retrying. A backlog seeded on open is what was said, not news.
    if (ageSeconds <= 0)
        cue(sessionId, motion, badge + QStringLiteral(": ") + line, logSeed(badge, line),
            heard);
    // A retry loop logging the same line over and over is one line, counted.
    if (!m_logs.isEmpty()) {
        LogLine &last = m_logs.last();
        if (last.text == line && last.badge == badge && seconds - last.seconds < 30.0) {
            last.repeats += 1;
            last.seconds = seconds;
            return sessionId;
        }
    }
    LogLine entry;
    entry.motion = motion;
    entry.badge = badge;
    entry.accent = accent;
    entry.text = line;
    entry.sessionId = sessionId;
    entry.seconds = seconds;
    entry.output = outputLine(line);
    m_logs.append(entry);
    if (m_logs.size() > kLogSize)
        m_logs.remove(0, m_logs.size() - kLogSize);
    // A line about an agent lights it a little, the way its own work does,
    // and a mirror's own line lights its seat and is kept for the cursor.
    if (sessionId != kDesktop && ageSeconds <= 0) {
        if (Body *about = find(sessionId))
            about->energy = qMax(about->energy, 0.35);
    }
    if (Host *seat = mirrorHost(sessionId)) {
        seat->lines += 1;
        seat->lastLine = line;
        seat->lastLineSeconds = seconds;
        if (ageSeconds <= 0)
            seat->energy = qMax(seat->energy, 0.6);
    }
    return sessionId;
}

int Scene::logRate() const
{
    int count = 0;
    // Every line, not back to the first old one: a seeded backlog lands after
    // lines that arrived while it was being read.
    for (const LogLine &line : m_logs) {
        if (m_now - line.seconds <= 60.0)
            count += line.repeats;
    }
    return count;
}

bool Scene::logSparking() const
{
    return !m_logs.isEmpty() && m_logs.last().sparking(m_now);
}

void Scene::taskStarted(quint64 id, int sessionId, const QString &word,
                        const QString &detail)
{
    // A desktop ticket is the agent's only while it works: the merge or
    // cleanup a stop sets off in its worktree is the desktop tidying up.
    placeTask(id, working(sessionId) ? sessionId : kDesktop, word, detail);
}

void Scene::placeTask(quint64 id, int sessionId, const QString &word,
                      const QString &detail)
{
    if (id == 0)
        return;
    for (Task &task : m_tasks) {
        if (task.id == id)
            return;
    }
    if (m_tasks.size() >= kMaxTasks) {
        // Full: a finished one makes room first, else the oldest.
        auto done = std::find_if(m_tasks.begin(), m_tasks.end(),
                                 [](const Task &task) { return !task.running(); });
        m_tasks.erase(done != m_tasks.end() ? done : m_tasks.begin());
    }
    Task task;
    task.id = id;
    task.sessionId = sessionId == kDesktop || find(sessionId) ? sessionId : kDesktop;
    task.word = word.isEmpty() ? QStringLiteral("work") : word;
    task.detail = detail;
    task.bornSeconds = m_now;
    // Round its owner by the golden angle, so neighbours never overlay.
    int siblings = 0;
    for (const Task &other : std::as_const(m_tasks)) {
        if (other.sessionId == task.sessionId)
            ++siblings;
    }
    task.angle = std::fmod(0.618034 * siblings + jitter(task.sessionId, 23), 1.0);
    m_tasks.append(task);
}

void Scene::taskFinished(quint64 id, bool failed)
{
    for (Task &task : m_tasks) {
        if (task.id == id && task.running()) {
            task.endSeconds = m_now;
            task.failed = failed;
            return;
        }
    }
}

void Scene::clearTasks()
{
    m_tasks.removeIf([](const Task &task) { return task.kind == TaskKind::Desktop; });
}

namespace {

// A tool call's words as one line: the description a subagent was given, the
// command a shell runs.
QString taskLine(const QString &text)
{
    return text.simplified().left(200);
}

// The text of every block of a user event - a string, text parts and the
// results that came back - which is where task notifications arrive.
QString userText(const QJsonObject &event)
{
    const QJsonValue content =
        event.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    if (content.isString())
        return content.toString();
    QString text;
    for (const QJsonValue &value : content.toArray()) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            text += block.value(QStringLiteral("text")).toString() + QLatin1Char('\n');
    }
    return text;
}

QString resultText(const QJsonObject &block)
{
    const QJsonValue result = block.value(QStringLiteral("content"));
    if (result.isString())
        return result.toString();
    QString text;
    for (const QJsonValue &part : result.toArray())
        text += part.toObject().value(QStringLiteral("text")).toString() + QLatin1Char('\n');
    return text;
}

// The id a background launch answers with: "Command running in background
// with ID: b1x9", "agentId: a7f2".
QString launchedId(const QString &text)
{
    static const QRegularExpression id(
        QStringLiteral("(?:\\bID|agentId|agent_id|task_id|shell_id|bash_id)\\s*[:=]\\s*`?([A-Za-z0-9_-]{3,})"));
    const QRegularExpressionMatch match = id.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

// "completed", "failed", "killed", from an output check or a notification.
QString statusIn(const QString &text)
{
    static const QRegularExpression status(QStringLiteral("<status>\\s*([A-Za-z_]+)\\s*</status>"));
    const QRegularExpressionMatch match = status.match(text);
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}

bool statusEnded(const QString &status)
{
    return status == QLatin1String("completed") || status == QLatin1String("failed")
           || status == QLatin1String("killed") || status == QLatin1String("error")
           || status == QLatin1String("stopped") || status == QLatin1String("cancelled");
}

bool statusFailed(const QString &status)
{
    return status != QLatin1String("completed");
}

QString taskKey(int sessionId, const QString &id)
{
    return QString::number(sessionId) + QLatin1Char('|') + id;
}

} // namespace

void Scene::watchAgentWork(int sessionId, const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("assistant")) {
        const QJsonArray content = event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue &value : content) {
            const QJsonObject block = value.toObject();
            if (block.value(QStringLiteral("type")).toString() != QLatin1String("tool_use"))
                continue;
            const QString name = block.value(QStringLiteral("name")).toString();
            const QString callId = block.value(QStringLiteral("id")).toString();
            const QJsonObject input = block.value(QStringLiteral("input")).toObject();
            QString target;
            for (const char *field : {"task_id", "shell_id", "bash_id", "agent_id"}) {
                if (target.isEmpty())
                    target = input.value(QLatin1String(field)).toString();
            }
            if (name == QLatin1String("Agent") || name == QLatin1String("Task")) {
                const QString agentType =
                    input.value(QStringLiteral("subagent_type")).toString();
                QString detail = input.value(QStringLiteral("description")).toString();
                if (detail.isEmpty())
                    detail = input.value(QStringLiteral("prompt")).toString();
                startAgentTask(sessionId, callId, TaskKind::Subagent,
                               agentType.isEmpty() ? QStringLiteral("subagent")
                                                   : QStringLiteral("subagent · ") + agentType,
                               taskLine(detail));
            } else if (name == QLatin1String("Bash")
                       && input.value(QStringLiteral("run_in_background")).toBool()) {
                QString detail = input.value(QStringLiteral("description")).toString();
                const QString command = input.value(QStringLiteral("command")).toString();
                if (!command.isEmpty())
                    detail = QStringLiteral("$ ") + taskLine(command);
                startAgentTask(sessionId, callId, TaskKind::Shell,
                               QStringLiteral("background shell"), detail);
            } else if (!target.isEmpty()
                       && (name == QLatin1String("KillShell") || name == QLatin1String("KillBash")
                           || name == QLatin1String("TaskStop"))) {
                endAgentTask(taskKey(sessionId, target), /*failed=*/false);
            } else if (!target.isEmpty() && !callId.isEmpty()
                       && (name == QLatin1String("BashOutput")
                           || name == QLatin1String("TaskOutput"))) {
                m_outputChecks.insert(taskKey(sessionId, callId), taskKey(sessionId, target));
            }
        }
        return;
    }
    if (type != QLatin1String("user"))
        return;
    const QJsonArray content = event.value(QStringLiteral("message"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != QLatin1String("tool_result"))
            continue;
        const QString call = taskKey(sessionId, block.value(QStringLiteral("tool_use_id")).toString());
        const bool error = block.value(QStringLiteral("is_error")).toBool();
        const QString text = resultText(block);
        const QString checked = m_outputChecks.take(call);
        if (!checked.isEmpty()) {
            const QString status = statusIn(text);
            if (statusEnded(status))
                endAgentTask(checked, statusFailed(status));
            continue;
        }
        const quint64 id = m_agentTaskIds.value(call);
        if (id == 0)
            continue;
        const auto task = std::find_if(m_tasks.begin(), m_tasks.end(),
                                       [id](const Task &task) { return task.id == id; });
        const bool shell = task != m_tasks.end() && task->kind == TaskKind::Shell;
        // Launched into the background: it runs on under the id it was given.
        static const QRegularExpression background(
            QStringLiteral("\\b(background|async)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        const QString launched = launchedId(text);
        if (!error && (shell || background.match(text).hasMatch()) && !launched.isEmpty()) {
            m_agentTaskIds.remove(call);
            m_agentTaskIds.insert(taskKey(sessionId, launched), id);
            continue;
        }
        if (!error && shell)
            continue; // still running, under an id the result did not say
        endAgentTask(call, error);
    }
    // A background task ending is announced in the conversation.
    static const QRegularExpression notice(
        QStringLiteral("<(?:task|shell|agent|bash)[-_]id>\\s*([^<\\s]+)\\s*</(?:task|shell|agent|bash)[-_]id>"));
    const QString text = userText(event);
    if (!text.contains(QLatin1String("-id>")) && !text.contains(QLatin1String("_id>")))
        return;
    QRegularExpressionMatchIterator it = notice.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString status = statusIn(text.mid(match.capturedEnd(), 400));
        endAgentTask(taskKey(sessionId, match.captured(1)),
                     statusEnded(status) && statusFailed(status));
    }
}

void Scene::startAgentTask(int sessionId, const QString &toolUseId, TaskKind kind,
                           const QString &word, const QString &detail)
{
    if (toolUseId.isEmpty())
        return;
    const QString key = taskKey(sessionId, toolUseId);
    // Well away from the desktop's ticket ids, which count up from 1.
    const quint64 id = (quint64(qHash(key)) | (quint64(1) << 63));
    placeTask(id, sessionId, word, detail);
    for (Task &task : m_tasks) {
        if (task.id == id)
            task.kind = kind;
    }
    m_agentTaskIds.insert(key, id);
}

void Scene::endAgentTask(const QString &key, bool failed)
{
    const quint64 id = m_agentTaskIds.take(key);
    if (id != 0)
        taskFinished(id, failed);
}

void Scene::forgetAgentTasks(int sessionId)
{
    const QString prefix = QString::number(sessionId) + QLatin1Char('|');
    for (auto it = m_agentTaskIds.begin(); it != m_agentTaskIds.end();) {
        if (it.key().startsWith(prefix))
            it = m_agentTaskIds.erase(it);
        else
            ++it;
    }
    for (auto it = m_outputChecks.begin(); it != m_outputChecks.end();) {
        if (it.key().startsWith(prefix))
            it = m_outputChecks.erase(it);
        else
            ++it;
    }
}

void Scene::sampleProcesses(int sessionId, const QList<ProcessInfo> &processes,
                            qint64 rootPid)
{
    if (!find(sessionId) && !processes.isEmpty())
        return;
    // The agent runtime is not one of its processes: claude or codex (and a
    // sandbox or the node running it) straight under the root, and anything
    // it hangs on. What they start is the agent's, and hangs off its body.
    QSet<qint64> runtime{rootPid};
    QHash<qint64, int> depth;
    depth.insert(rootPid, 0);
    QList<const ProcessInfo *> shown;
    for (const ProcessInfo &info : processes) {
        const bool underRuntime = runtime.contains(info.parentPid);
        const QString name = info.name;
        if (underRuntime
            && (name == QLatin1String("claude") || name == QLatin1String("codex")
                || name == QLatin1String("bwrap")
                || (name == QLatin1String("node") && info.parentPid == rootPid))) {
            runtime.insert(info.pid);
            continue;
        }
        const auto parent = depth.constFind(info.parentPid);
        const int parentDepth = parent != depth.constEnd() ? *parent : 0;
        depth.insert(info.pid, underRuntime ? 1 : parentDepth + 1);
        shown.append(&info);
    }

    QHash<qint64, int> index; // pid -> m_processes index, this agent's running ones
    int running = 0;
    for (int i = 0; i < m_processes.size(); ++i) {
        const Process &process = m_processes.at(i);
        if (process.sessionId == sessionId && process.running()) {
            index.insert(process.pid, i);
            ++running;
        }
    }
    const int before = int(m_processes.size());
    QSet<int> seen; // indexes below `before` still alive
    int hidden = 0;
    for (const ProcessInfo *info : std::as_const(shown)) {
        const auto at = index.constFind(info->pid);
        if (at != index.constEnd() && m_processes.at(*at).name == info->name) {
            Process &process = m_processes[*at];
            const double elapsed = m_now - process.sampledSeconds;
            if (elapsed > 0.05) {
                process.cpu = qMax<qint64>(0, info->cpuTicks - process.cpuTicks)
                              / kClockTicks / elapsed;
                process.cpuTicks = info->cpuTicks;
                process.sampledSeconds = m_now;
            }
            if (!info->command.isEmpty())
                process.command = info->command;
            seen.insert(*at);
            continue;
        }
        if (running >= kMaxProcesses) {
            ++hidden;
            continue;
        }
        Process process;
        process.sessionId = sessionId;
        process.pid = info->pid;
        process.parentPid = runtime.contains(info->parentPid) ? 0 : info->parentPid;
        process.name = info->name;
        process.command = info->command;
        process.depth = depth.value(info->pid, 1);
        process.bornSeconds = m_now;
        process.sampledSeconds = m_now;
        process.cpuTicks = info->cpuTicks;
        // The agent's own children go round it by the golden angle; a child
        // fans out from its parent's bearing, alternating either side of it.
        int siblings = 0;
        double parentAngle = -1;
        for (const Process &other : std::as_const(m_processes)) {
            if (other.sessionId != sessionId || !other.running())
                continue;
            if (other.parentPid == process.parentPid)
                ++siblings;
            if (process.parentPid != 0 && other.pid == process.parentPid)
                parentAngle = other.angle;
        }
        if (parentAngle < 0) {
            process.angle =
                std::fmod(0.618034 * siblings + jitter(sessionId, 31) + 0.19, 1.0);
        } else {
            const double side = siblings % 2 == 0 ? 1.0 : -1.0;
            process.angle = std::fmod(parentAngle + side * 0.022 * ((siblings + 1) / 2) + 1.0,
                                      1.0);
        }
        m_processes.append(process);
        ++running;
    }
    for (int i = 0; i < before; ++i) {
        Process &process = m_processes[i];
        if (process.sessionId == sessionId && process.running() && !seen.contains(i)) {
            process.endSeconds = m_now;
            process.cpu = 0;
        }
    }
    if (hidden > 0)
        m_hiddenProcesses.insert(sessionId, hidden);
    else
        m_hiddenProcesses.remove(sessionId);
}

const Process *Scene::process(int sessionId, qint64 pid) const
{
    for (const Process &process : m_processes) {
        if (process.sessionId == sessionId && process.pid == pid && process.running())
            return &process;
    }
    return nullptr;
}

int Scene::runningProcesses() const
{
    return int(std::count_if(m_processes.cbegin(), m_processes.cend(),
                             [](const Process &process) { return process.running(); }));
}

int Scene::busyProcesses() const
{
    return int(std::count_if(m_processes.cbegin(), m_processes.cend(),
                             [](const Process &process) { return process.busy(); }));
}

int Scene::runningTasks() const
{
    int count = 0;
    for (const Task &task : m_tasks) {
        if (task.running())
            ++count;
    }
    return count;
}

bool Scene::moving() const
{
    if (!m_pulses.isEmpty() || !m_packets.isEmpty() || !m_joins.isEmpty() || logSparking()
        || !m_frames.isEmpty())
        return true;
    // A client arriving opens its ring, one leaving fades, and one a frame
    // just crossed to glows down like a body.
    for (const Client &client : m_clients) {
        if (!client.present() || m_now - client.bornSeconds < kClientArrival
            || client.energy > 0.05)
            return true;
    }
    // The desktop's work and a subagent are work for as long as they run. A
    // background shell is often a server left up for hours, and a process
    // tree always holds a few sleepers (an MCP server, a watcher): those move
    // the field as they come and go, and a process while it burns CPU.
    for (const Task &task : m_tasks) {
        if (!task.running() || task.kind != TaskKind::Shell
            || m_now - task.bornSeconds < 3.0)
            return true;
    }
    for (const Process &process : m_processes) {
        if (!process.running() || m_now - process.bornSeconds < 1.0
            || (process.busy() && m_now - process.sampledSeconds < 2.0))
            return true;
    }
    for (const Body &body : m_bodies) {
        // A body's glow fades with its energy; a run with nothing happening
        // is drawn once and left.
        if (body.active(m_now) || body.energy > 0.05)
            return true;
    }
    // A mirror's seat glows with the journal it is streaming, and fades the
    // same way a body does.
    for (const Host &host : m_hosts) {
        if (host.energy > 0.05)
            return true;
    }
    // Bodies still easing apart from a newcomer.
    if (m_separating)
        return true;
    return upRate() + downRate() > kQuietRate;
}

ClientKind clientKindFor(const ClientInfo &info)
{
    const QString platform = info.platform.trimmed().toLower();
    if (platform == QLatin1String("android") || platform == QLatin1String("ios"))
        return ClientKind::Phone;
    if (platform == QLatin1String("web"))
        return ClientKind::Visitor;
    if (ChatVisitorPresence::isTransientVisitor(info.accountKind, info.name))
        return ClientKind::Visitor;
    // The site's chat and the world advertise no platform and no node: every
    // desktop and phone has always sent both.
    if (platform.isEmpty() && info.nodeName.trimmed().isEmpty())
        return ClientKind::Visitor;
    return ClientKind::Node;
}

QString clientKindLabel(ClientKind kind)
{
    switch (kind) {
    case ClientKind::Node:
        return QStringLiteral("desktop node");
    case ClientKind::Phone:
        return QStringLiteral("phone");
    case ClientKind::Visitor:
        return QStringLiteral("web visitor");
    }
    return {};
}

QString browserForUserAgent(const QString &userAgent)
{
    if (!userAgent.startsWith(QLatin1String("Mozilla/")))
        return {};
    static const QRegularExpression bot(QStringLiteral("bot|crawl|spider|slurp|preview"),
                                        QRegularExpression::CaseInsensitiveOption);
    if (bot.match(userAgent).hasMatch())
        return QStringLiteral("a crawler");
    // Most specific first: every Chromium says Safari, and Edge says Chrome.
    static const char *const kBrowsers[][2] = {
        {"Edg/", "Edge"},      {"OPR/", "Opera"},   {"Firefox/", "Firefox"},
        {"Chrome/", "Chrome"}, {"Safari/", "Safari"},
    };
    for (const auto &browser : kBrowsers) {
        if (userAgent.contains(QLatin1String(browser[0])))
            return QString::fromLatin1(browser[1]);
    }
    return QStringLiteral("a browser");
}

QString toolForUserAgent(const QString &userAgent, bool *attack)
{
    if (attack)
        *attack = false;
    const QString agent = userAgent.trimmed();
    if (agent.isEmpty())
        return QStringLiteral("no user agent");
    // Scanners and exploit kits: nothing but an attack ever sends these.
    static const char *const kAttacks[] = {
        "sqlmap",   "nikto",    "nuclei",  "zgrab",   "masscan",     "nmap",
        "gobuster", "dirbuster", "feroxbuster", "ffuf", "wpscan",    "whatweb",
        "acunetix", "netsparker", "openvas", "wfuzz",  "commix",     "xsstrike",
        "jaeles",   "hydra",    "l9explore", "l9tcpid",
    };
    for (const char *name : kAttacks) {
        if (agent.contains(QLatin1String(name), Qt::CaseInsensitive)) {
            if (attack)
                *attack = true;
            return QString::fromLatin1(name);
        }
    }
    if (agent.startsWith(QLatin1String("Mozilla/")))
        return {};
    // Scripts and libraries: somebody's code rather than somebody's browser.
    static const char *const kTools[][2] = {
        {"curl/", "curl"},
        {"wget/", "Wget"},
        {"python-requests/", "python-requests"},
        {"python-urllib/", "python-urllib"},
        {"python-httpx/", "python-httpx"},
        {"aiohttp/", "aiohttp"},
        {"go-http-client/", "Go-http-client"},
        {"libwww-perl/", "libwww-perl"},
        {"java/", "Java"},
        {"apache-httpclient/", "Apache-HttpClient"},
        {"scrapy/", "Scrapy"},
        {"pycurl/", "PycURL"},
        {"node-fetch/", "node-fetch"},
        {"axios/", "axios"},
    };
    for (const auto &tool : kTools) {
        if (agent.contains(QLatin1String(tool[0]), Qt::CaseInsensitive))
            return QString::fromLatin1(tool[1]);
    }
    return {};
}

QString systemForUserAgent(const QString &userAgent)
{
    // Most specific first: an iPad says "like Mac OS X", Android says Linux.
    static const char *const kSystems[][2] = {
        {"iPhone", "iOS"},   {"iPad", "iOS"},         {"iPod", "iOS"},
        {"Android", "Android"}, {"CrOS", "ChromeOS"}, {"Windows", "Windows"},
        {"Macintosh", "macOS"}, {"Mac OS X", "macOS"}, {"Linux", "Linux"},
        {"X11", "Linux"},
    };
    for (const auto &system : kSystems) {
        if (userAgent.contains(QLatin1String(system[0])))
            return QString::fromLatin1(system[1]);
    }
    return {};
}

QString flagForCountry(const QString &code)
{
    const QString upper = code.trimmed().toUpper();
    if (upper.size() != 2 || upper == QLatin1String("XX"))
        return {};
    char32_t letters[2];
    for (int i = 0; i < 2; ++i) {
        const QChar c = upper.at(i);
        if (c < QLatin1Char('A') || c > QLatin1Char('Z'))
            return {};
        letters[i] = char32_t(0x1F1E6 + (c.unicode() - 'A'));
    }
    return QString::fromUcs4(letters, 2);
}

QString countryName(const QString &code)
{
    const QString upper = code.trimmed().toUpper();
    if (upper == QLatin1String("T1"))
        return QStringLiteral("Tor");
    if (upper == QLatin1String("XX") || upper.isEmpty())
        return QStringLiteral("somewhere unknown");
    const QLocale::Territory territory = QLocale::codeToTerritory(upper);
    if (territory == QLocale::AnyTerritory)
        return upper;
    return QLocale::territoryToString(territory);
}

bool isProbePath(const QString &path)
{
    const QString p = path.toLower();
    if (p.isEmpty())
        return false;
    // Nothing on this site is PHP, ASP or a CGI script, and none of its
    // dotfiles or a WordPress's are served.
    static const char *const kProbes[] = {
        "/.env",      "/.git/",      "/.aws/",       "/.ssh/",     "/.svn/",
        "/.htaccess", "/.htpasswd",  "/.ds_store",   "/.vscode/",  "/wp-login",
        "/wp-admin",  "/wp-content", "/wp-includes", "xmlrpc.php", "wlwmanifest",
        "phpmyadmin", "/phpinfo",    "/cgi-bin/",    "/boaform",   "/actuator",
        "/vendor/phpunit", "/hnap1", "/manager/html", "/server-status", "/etc/passwd",
        "../",        "%2e%2e",      "/solr/",       "/owa/",      "/autodiscover/",
    };
    for (const char *probe : kProbes) {
        if (p.contains(QLatin1String(probe)))
            return true;
    }
    for (const char *suffix : {".php", ".asp", ".aspx", ".jsp", ".cgi"}) {
        if (p.endsWith(QLatin1String(suffix)))
            return true;
    }
    return false;
}

QString threatLabel(Threat threat)
{
    switch (threat) {
    case Threat::None:
        return {};
    case Threat::Watch:
        return QStringLiteral("watch");
    case Threat::Danger:
        return QStringLiteral("danger");
    }
    return {};
}

QString blockTargetKey(BlockTarget target)
{
    switch (target) {
    case BlockTarget::Address:
        return QStringLiteral("ip");
    case BlockTarget::Range:
        return QStringLiteral("ip_range");
    case BlockTarget::Network:
        return QStringLiteral("asn");
    case BlockTarget::Country:
        return QStringLiteral("country");
    case BlockTarget::UserAgent:
        return QStringLiteral("user_agent");
    }
    return {};
}

Client *Scene::findClient(const QString &id)
{
    for (Client &client : m_clients) {
        if (client.info.id == id)
            return &client;
    }
    return nullptr;
}

const Client *Scene::client(const QString &id) const
{
    return const_cast<Scene *>(this)->findClient(id);
}

int Scene::clientCount(ClientKind kind) const
{
    return int(std::count_if(m_clients.cbegin(), m_clients.cend(), [kind](const Client &c) {
        return c.present() && c.kind == kind;
    }));
}

void Scene::sortClients()
{
    std::stable_sort(m_clients.begin(), m_clients.end(), [](const Client &a, const Client &b) {
        if (a.kind != b.kind)
            return int(a.kind) < int(b.kind);
        return a.bornSeconds < b.bornSeconds;
    });
}

QList<ClientChange> Scene::setClients(const QList<ClientInfo> &clients, bool announce)
{
    QList<ClientChange> changes;
    QSet<QString> listed;
    for (const ClientInfo &info : clients) {
        if (info.id.isEmpty() || listed.contains(info.id))
            continue;
        listed.insert(info.id);
        const ClientKind kind = clientKindFor(info);
        if (Client *known = findClient(info.id); known && !known->web) {
            // Back before it had faded out: the same seat, not a new arrival.
            const bool returned = !known->present();
            known->info = info;
            known->kind = kind;
            known->leftSeconds = -1;
            if (returned && announce)
                changes.append({info, kind, true});
            continue;
        }
        if (m_clients.size() >= kMaxClients)
            continue;
        Client fresh;
        fresh.info = info;
        fresh.kind = kind;
        // Seeded into a room already full, nobody is arriving.
        fresh.bornSeconds = announce ? m_now : m_now - kClientArrival - 1.0;
        m_clients.append(fresh);
        if (announce) {
            changes.append({info, kind, true});
            m_arrivals.append(m_now);
        }
    }
    for (Client &client : m_clients) {
        // The roster speaks for the relay room, not for the site's visitors.
        if (client.web || !client.present() || listed.contains(client.info.id))
            continue;
        if (!announce) {
            // Gone while nobody watched: no farewell for it now.
            client.leftSeconds = m_now - kClientLinger;
            continue;
        }
        client.leftSeconds = m_now;
        changes.append({client.info, client.kind, false});
    }
    if (!announce) {
        m_clients.removeIf([this](const Client &client) {
            return !client.present() && m_now - client.leftSeconds >= kClientLinger;
        });
    }
    sortClients();
    return changes;
}

void Scene::socketFrame(bool outgoing, const QString &type, const QString &client,
                        qint64 bytes)
{
    bytes = qMax<qint64>(0, bytes);
    ++m_totalFrames;
    if (m_frameTimes.size() >= 4096)
        m_frameTimes.removeFirst();
    m_frameTimes.append(m_now);
    const double size = qBound(0.2, std::log10(1.0 + double(bytes)) / 5.0, 1.0);
    const auto launch = [&](const QString &id, int order) {
        Frame frame;
        frame.client = id;
        frame.up = outgoing;
        frame.type = type;
        // A frame to the whole room reaches its nearest clients first.
        frame.bornSeconds = m_now + order * 0.03;
        frame.seconds = 0.8 + 0.2 * jitter(int(m_totalFrames % 997), order);
        frame.size = size;
        if (m_frames.size() >= kMaxFrames)
            m_frames.removeFirst();
        m_frames.append(frame);
    };
    const auto touch = [&](Client &c) {
        c.energy = qMin(1.0, c.energy + 0.6);
        c.lastType = type;
        c.lastFrameSeconds = m_now;
        // A site visitor's are requests, tallied by status in webRequest.
        if (!c.web)
            ++c.types[type];
        if (outgoing) {
            ++c.framesOut;
            c.bytesOut += bytes;
        } else {
            ++c.framesIn;
            c.bytesIn += bytes;
        }
    };
    Client *named = client.isEmpty() ? nullptr : findClient(client);
    if (named && named->present()) {
        touch(*named);
        launch(named->info.id, 0);
        return;
    }
    if (outgoing && client.isEmpty()) {
        // To the whole room: everybody in it is sent it. A site visitor seen
        // only in the Worker's log is not in the room, and is sent nothing.
        int order = 0;
        for (Client &c : m_clients) {
            if (!c.present() || c.web)
                continue;
            touch(c);
            if (order < kRoomFanout)
                launch(c.info.id, order++);
        }
        if (order == 0)
            launch(QString(), 0);
        return;
    }
    launch(QString(), 0);
}

bool Scene::webRequest(const QString &visitor, const QString &name, const QString &detail,
                       const QString &request, qint64 bytes)
{
    WebHit hit;
    hit.visitor = visitor;
    hit.name = name;
    hit.detail = detail;
    hit.request = request;
    hit.bytes = bytes;
    return webRequest(hit);
}

bool Scene::webRequest(const WebHit &hit)
{
    if (hit.visitor.isEmpty())
        return false;
    const QString id = QStringLiteral("web:") + hit.visitor;
    Client *client = findClient(id);
    bool joined = false;
    if (!client || !client->present()) {
        if (!client) {
            if (m_clients.size() >= kMaxClients)
                return false;
            Client fresh;
            fresh.web = true;
            fresh.kind = ClientKind::Visitor;
            m_clients.append(fresh);
            client = &m_clients.last();
            client->info.id = id;
        }
        // Back after going quiet is another visit.
        client->bornSeconds = m_now;
        client->leftSeconds = -1;
        client->requestTimes.clear();
        joined = true;
        m_arrivals.append(m_now);
    }
    client->info.name = hit.name;
    client->info.platform = QStringLiteral("web");
    client->info.detail = hit.detail;
    // What the edge said last is what it is now; a blank never forgets.
    const auto keep = [](QString &field, const QString &value) {
        if (!value.trimmed().isEmpty())
            field = value.trimmed();
    };
    keep(client->info.country, hit.country.toUpper());
    keep(client->info.address, hit.address);
    keep(client->info.asn, hit.asn);
    keep(client->info.asOrganization, hit.asOrganization);
    if (!hit.userAgent.isEmpty() || client->info.userAgent.isEmpty())
        client->info.userAgent = hit.userAgent;
    client->tool = toolForUserAgent(client->info.userAgent, &client->attackTool);
    if (client->info.userAgent.isEmpty() && hit.userAgent.isEmpty())
        client->tool = QStringLiteral("no user agent");

    ++client->requests;
    client->requestTimes.append(m_now);
    const double cutoff = m_now - 60.0;
    auto first = std::lower_bound(client->requestTimes.begin(), client->requestTimes.end(), cutoff);
    if (first != client->requestTimes.begin())
        client->requestTimes.erase(client->requestTimes.begin(), first);
    if (hit.status > 0)
        ++client->types[QStringLiteral("%1xx").arg(hit.status / 100)];
    if (hit.status >= 400 && hit.status < 500)
        ++client->refused;
    else if (hit.status >= 500)
        ++client->failed;
    if (isProbePath(hit.path)) {
        ++client->probes;
        client->lastProbe = hit.path;
    }
    client->recent.prepend({hit.request, m_now});
    if (client->recent.size() > Client::kRecentRequests)
        client->recent.resize(Client::kRecentRequests);
    for (const BlockRule &rule : std::as_const(m_blocks)) {
        if (matchesBlock(*client, rule))
            client->blocked = true;
    }
    if (joined)
        sortClients();
    socketFrame(false, hit.request, id, hit.bytes);
    assess();
    return joined;
}

int Client::requestRate(double now) const
{
    return int(std::count_if(requestTimes.cbegin(), requestTimes.cend(),
                             [now](double at) { return at >= now - 60.0; }));
}

namespace {

// The /24 an IPv4 address is in, or the /48 of an IPv6 one: the ranges
// Cloudflare's access rules take, and the size a hosting provider hands out.
QString rangeFor(const QString &address)
{
    if (address.contains(QLatin1Char('.')) && !address.contains(QLatin1Char(':'))) {
        const QStringList parts = address.split(QLatin1Char('.'));
        if (parts.size() != 4)
            return {};
        return QStringLiteral("%1.%2.%3.0/24").arg(parts.at(0), parts.at(1), parts.at(2));
    }
    if (!address.contains(QLatin1Char(':')))
        return {};
    // The first three groups, with "::" spelled out.
    const int gap = address.indexOf(QStringLiteral("::"));
    QStringList head = address.left(gap < 0 ? address.size() : gap)
                           .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (gap >= 0) {
        const QStringList tail =
            address.mid(gap + 2).split(QLatin1Char(':'), Qt::SkipEmptyParts);
        while (head.size() + tail.size() < 8)
            head.append(QStringLiteral("0"));
        head.append(tail);
    }
    if (head.size() < 3)
        return {};
    QStringList groups;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const uint value = head.at(i).toUInt(&ok, 16);
        if (!ok || value > 0xffff)
            return {};
        groups << QString::number(value, 16);
    }
    return groups.join(QLatin1Char(':')) + QStringLiteral("::/48");
}

QString networkFor(const ClientInfo &info)
{
    return info.asn.isEmpty() ? QString() : QStringLiteral("AS") + info.asn;
}

QString plural(int count, const char *word)
{
    return QStringLiteral("%1 %2%3")
        .arg(count)
        .arg(QString::fromLatin1(word))
        .arg(count == 1 ? QString() : QStringLiteral("s"));
}

} // namespace

void Scene::assess()
{
    // The crowds: newcomers out of one range, or one network, in the last
    // two minutes. Many addresses from one hosting network arriving together
    // is what a botnet or a rented scanner looks like; a popular page shared
    // somewhere brings them from everywhere.
    QHash<QString, int> ranges;
    QHash<QString, int> networks;
    for (const Client &c : std::as_const(m_clients)) {
        if (!c.web || !c.present() || m_now - c.bornSeconds > kCrowdSeconds)
            continue;
        if (const QString range = rangeFor(c.info.address); !range.isEmpty())
            ++ranges[range];
        if (const QString network = networkFor(c.info); !network.isEmpty())
            ++networks[network];
    }
    for (Client &c : m_clients) {
        if (!c.web)
            continue;
        Threat threat = Threat::None;
        QStringList reasons;
        const auto flag = [&](Threat level, const QString &why) {
            threat = qMax(threat, level);
            reasons << why;
        };
        if (c.attackTool)
            flag(Threat::Danger, QStringLiteral("runs %1, an attack tool").arg(c.tool));
        const int rate = c.requestRate(m_now);
        if (rate >= kDangerRate)
            flag(Threat::Danger, QStringLiteral("%1 a minute: a flood").arg(plural(rate, "request")));
        else if (rate >= kWatchRate)
            flag(Threat::Watch, QStringLiteral("%1 a minute").arg(plural(rate, "request")));
        if (c.probes >= 3)
            flag(Threat::Danger, QStringLiteral("%1 for paths scanners try, latest %2")
                                     .arg(plural(c.probes, "request"), c.lastProbe));
        else if (c.probes > 0)
            flag(Threat::Watch, QStringLiteral("asked for %1, a path scanners try")
                                    .arg(c.lastProbe));
        if (c.refused >= 20 && c.refused * 2 >= c.requests)
            flag(Threat::Watch, QStringLiteral("%1 of %2 refused (4xx)")
                                    .arg(c.refused)
                                    .arg(plural(c.requests, "request")));
        if (c.failed >= 10)
            flag(Threat::Watch, QStringLiteral("%1 the site failed (5xx)")
                                    .arg(plural(c.failed, "request")));
        if (c.present() && m_now - c.bornSeconds <= kCrowdSeconds) {
            const QString range = rangeFor(c.info.address);
            const int inRange = ranges.value(range);
            if (inRange >= kCrowdDanger)
                flag(Threat::Danger, QStringLiteral("one of %1 new visitors out of %2")
                                         .arg(inRange)
                                         .arg(range));
            else if (inRange >= kCrowdWatch)
                flag(Threat::Watch, QStringLiteral("one of %1 new visitors out of %2")
                                        .arg(inRange)
                                        .arg(range));
            // A network is bigger than a range: it takes twice the crowd.
            const QString network = networkFor(c.info);
            const int inNetwork = networks.value(network);
            if (inNetwork >= kCrowdDanger * 2 && inRange < kCrowdDanger)
                flag(Threat::Danger, QStringLiteral("one of %1 new visitors out of %2 %3")
                                         .arg(inNetwork)
                                         .arg(network, c.info.asOrganization)
                                         .trimmed());
            else if (inNetwork >= kCrowdWatch * 2 && inRange < kCrowdWatch)
                flag(Threat::Watch, QStringLiteral("one of %1 new visitors out of %2 %3")
                                        .arg(inNetwork)
                                        .arg(network, c.info.asOrganization)
                                        .trimmed());
        }
        c.threat = threat;
        c.threatReasons = reasons;
    }
}

Threat Scene::siteThreat() const
{
    Threat threat = Threat::None;
    for (const Client &c : m_clients) {
        if (c.present() && !c.blocked)
            threat = qMax(threat, c.threat);
    }
    const int arrivals = arrivalRate();
    if (arrivals >= kSurgeDanger)
        threat = Threat::Danger;
    else if (arrivals >= kSurgeWatch)
        threat = qMax(threat, Threat::Watch);
    return threat;
}

QStringList Scene::siteThreatReasons() const
{
    QStringList reasons;
    const int arrivals = arrivalRate();
    if (arrivals >= kSurgeWatch)
        reasons << QStringLiteral("%1 in the last minute").arg(plural(arrivals, "newcomer"));
    int danger = 0;
    int watch = 0;
    int blocked = 0;
    for (const Client &c : m_clients) {
        if (!c.present())
            continue;
        if (c.blocked)
            ++blocked;
        else if (c.threat == Threat::Danger)
            ++danger;
        else if (c.threat == Threat::Watch)
            ++watch;
    }
    if (danger > 0)
        reasons << QStringLiteral("%1 in danger").arg(plural(danger, "visitor"));
    if (watch > 0)
        reasons << QStringLiteral("%1 to watch").arg(plural(watch, "visitor"));
    if (blocked > 0)
        reasons << QStringLiteral("%1 blocked").arg(plural(blocked, "visitor"));
    return reasons;
}

int Scene::flaggedCount() const
{
    return int(std::count_if(m_clients.cbegin(), m_clients.cend(), [](const Client &c) {
        return c.present() && !c.blocked && c.threat != Threat::None;
    }));
}

int Scene::arrivalRate() const
{
    return int(std::count_if(m_arrivals.cbegin(), m_arrivals.cend(),
                             [this](double at) { return at >= m_now - 60.0; }));
}

bool Scene::matchesBlock(const Client &client, const BlockRule &rule) const
{
    if (!client.web)
        return false;
    switch (rule.target) {
    case BlockTarget::Address:
        return !client.info.address.isEmpty() && client.info.address == rule.value;
    case BlockTarget::Range:
        return !client.info.address.isEmpty() && rangeFor(client.info.address) == rule.value;
    case BlockTarget::Network:
        return !client.info.asn.isEmpty() && networkFor(client.info) == rule.value;
    case BlockTarget::Country:
        return !client.info.country.isEmpty() && client.info.country == rule.value;
    case BlockTarget::UserAgent:
        return !client.info.userAgent.isEmpty() && client.info.userAgent == rule.value;
    }
    return false;
}

QList<BlockRule> Scene::blockRules(const QString &clientId) const
{
    QList<BlockRule> rules;
    const Client *client = this->client(clientId);
    if (!client || !client->web || client->info.address.isEmpty())
        return rules;
    const ClientInfo &info = client->info;
    const auto rule = [&](BlockTarget target, const QString &value, const QString &label) {
        BlockRule made;
        made.target = target;
        made.value = value;
        made.label = label;
        made.visitors = 0;
        for (const Client &c : m_clients) {
            if (!c.present() || !matchesBlock(c, made))
                continue;
            ++made.visitors;
            if (c.threat != Threat::None)
                ++made.flagged;
        }
        return made;
    };
    rules << rule(BlockTarget::Address, info.address, QStringLiteral("the address ") + info.address);
    const QString range = rangeFor(info.address);
    if (!range.isEmpty())
        rules << rule(BlockTarget::Range, range, QStringLiteral("the range ") + range);
    const QString network = networkFor(info);
    if (!network.isEmpty())
        rules << rule(BlockTarget::Network, network,
                      QStringLiteral("the network %1 %2").arg(network, info.asOrganization).trimmed());
    if (!info.userAgent.isEmpty())
        rules << rule(BlockTarget::UserAgent, info.userAgent,
                      QStringLiteral("the user agent \"%1\"")
                          .arg(info.userAgent.size() > 60 ? info.userAgent.left(59) + QChar(0x2026)
                                                          : info.userAgent));
    if (!info.country.isEmpty())
        rules << rule(BlockTarget::Country, info.country,
                      QStringLiteral("everyone in %1").arg(countryName(info.country)));

    // The narrowest rule that catches the attack this visitor is part of and
    // nobody innocent beside it. A tool that is nothing but an attack is
    // stopped by what it calls itself, from any address; a crowd from one
    // range by the range; a crowd spread over a network's ranges by the
    // network. A country is only ever the answer to a flood nothing
    // narrower holds, and even then it catches everybody there.
    const auto find = [&rules](BlockTarget target) -> BlockRule * {
        for (BlockRule &r : rules) {
            if (r.target == target)
                return &r;
        }
        return nullptr;
    };
    const auto clean = [](const BlockRule &r) { return r.visitors - r.flagged == 0; };
    BlockRule *pick = find(BlockTarget::Address);
    pick->reason = QStringLiteral("this address alone: nobody else is caught");
    if (BlockRule *agent = find(BlockTarget::UserAgent); agent && client->attackTool) {
        pick = agent;
        pick->reason = QStringLiteral("%1 is an attack tool: blocked by name from every "
                                      "address, and no browser sends it")
                           .arg(client->tool);
    } else if (BlockRule *r = find(BlockTarget::Range); r && r->flagged >= 3 && clean(*r)) {
        pick = r;
        pick->reason = QStringLiteral("%1 share this range, and nobody else "
                                      "is in it")
                           .arg(plural(r->flagged, "flagged visitor"));
    } else if (BlockRule *n = find(BlockTarget::Network);
               n && n->flagged >= 5 && (n->visitors - n->flagged) * 5 <= n->visitors) {
        pick = n;
        pick->reason = QStringLiteral("%1 across its ranges, and nearly "
                                      "everybody from it is flagged")
                           .arg(plural(n->flagged, "flagged visitor"));
    } else if (BlockRule *country = find(BlockTarget::Country);
               country && country->flagged >= kCrowdDanger
               && (country->visitors - country->flagged) * 10 <= country->visitors) {
        int networksFlagged = 0;
        QSet<QString> seen;
        for (const Client &c : m_clients) {
            if (c.present() && c.threat != Threat::None && c.info.country == info.country
                && !seen.contains(c.info.asn)) {
                seen.insert(c.info.asn);
                ++networksFlagged;
            }
        }
        if (networksFlagged > 10) {
            pick = country;
            pick->reason = QStringLiteral("a flood of %1 out of %2 networks there, and "
                                          "hardly anybody else from it")
                               .arg(plural(country->flagged, "flagged visitor"))
                               .arg(networksFlagged);
        }
    }
    pick->recommended = true;
    for (BlockRule &r : rules) {
        if (!r.reason.isEmpty())
            continue;
        const int innocent = r.visitors - r.flagged;
        r.reason = innocent > 0 ? QStringLiteral("also catches %1 not flagged")
                                      .arg(plural(innocent, "visitor"))
                                : QStringLiteral("catches %1, every one flagged")
                                      .arg(plural(r.visitors, "visitor"));
    }
    return rules;
}

QList<BlockRule> Scene::suggestedBlocks() const
{
    QList<BlockRule> merged;
    for (const Client &c : m_clients) {
        if (!c.web || !c.present() || c.blocked || c.threat == Threat::None)
            continue;
        for (const BlockRule &rule : blockRules(c.info.id)) {
            if (!rule.recommended)
                continue;
            const bool known = std::any_of(merged.cbegin(), merged.cend(), [&](const BlockRule &m) {
                return m.target == rule.target && m.value == rule.value;
            });
            if (!known)
                merged << rule;
        }
    }
    // A range or a network rule already catches the addresses in it.
    merged.removeIf([&merged, this](const BlockRule &rule) {
        if (rule.target != BlockTarget::Address)
            return false;
        const Client *owner = nullptr;
        for (const Client &c : m_clients) {
            if (c.web && c.info.address == rule.value)
                owner = &c;
        }
        return owner && std::any_of(merged.cbegin(), merged.cend(), [&](const BlockRule &wider) {
                   return wider.target != BlockTarget::Address && matchesBlock(*owner, wider);
               });
    });
    std::stable_sort(merged.begin(), merged.end(), [](const BlockRule &a, const BlockRule &b) {
        return a.flagged > b.flagged;
    });
    return merged;
}

void Scene::noteBlocked(const BlockRule &rule)
{
    const bool known = std::any_of(m_blocks.cbegin(), m_blocks.cend(), [&](const BlockRule &b) {
        return b.target == rule.target && b.value == rule.value;
    });
    if (!known)
        m_blocks << rule;
    for (Client &c : m_clients) {
        if (matchesBlock(c, rule))
            c.blocked = true;
    }
}

int Scene::frameRate() const
{
    return int(m_frameTimes.size());
}

QString describeBody(const Body &body)
{
    QString text = QStringLiteral("#%1").arg(body.sessionId);
    if (!body.title.isEmpty())
        text += QLatin1Char(' ') + body.title;
    if (!body.task.isEmpty())
        text += QStringLiteral(" · ") + body.task;
    text += QStringLiteral(" — ") + sonify::voiceDescription(body.voice);
    if (body.turns > 0)
        text += QStringLiteral(" — %1 turn%2")
                    .arg(body.turns)
                    .arg(body.turns == 1 ? QString() : QStringLiteral("s"));
    if (body.tokens > 0) {
        text += body.tokens >= 1000
                    ? QStringLiteral(", %1k tokens").arg(body.tokens / 1000)
                    : QStringLiteral(", %1 tokens").arg(body.tokens);
    }
    if (body.events > 0)
        text += QStringLiteral(", %1 event%2")
                    .arg(body.events)
                    .arg(body.events == 1 ? QString() : QStringLiteral("s"));
    return text;
}

} // namespace forkmesh::agentvis
