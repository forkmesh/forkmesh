#include "AgentAnnouncer.h"

#include "AgentSonifier.h"
#include "AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace forkmesh::announce {

namespace {

// Status strings as AgentStore.h spells them. Repeated here rather than
// included so this file, like the sonifier, needs nothing but Qt Core.
const QLatin1String kRunning("running");
const QLatin1String kWaiting("waiting");
const QLatin1String kSuccess("success");
const QLatin1String kFailed("failed");
const QLatin1String kStopped("stopped");

constexpr int kMaxPending = 16;
constexpr qint64 kColourGapMs = 10 * 1000;       // silence before filler
constexpr qint64 kColourRepeatGapMs = 22 * 1000; // between two fillers
constexpr qint64 kBenchReminderMs = 120 * 1000;
constexpr qint64 kReintroduceMs = 180 * 1000;
constexpr qint64 kQuietMs = 90 * 1000;
constexpr qint64 kQuietRepeatMs = 5 * 60 * 1000;

QString numberUnderHundred(int n)
{
    static const char *const ones[] = {
        "zero",    "one",     "two",       "three",    "four",     "five",    "six",
        "seven",   "eight",   "nine",      "ten",      "eleven",   "twelve",  "thirteen",
        "fourteen", "fifteen", "sixteen",  "seventeen", "eighteen", "nineteen"};
    static const char *const tens[] = {"",      "",      "twenty",  "thirty", "forty",
                                       "fifty", "sixty", "seventy", "eighty", "ninety"};
    n = qBound(0, n, 99);
    if (n < 20)
        return QString::fromLatin1(ones[n]);
    QString words = QString::fromLatin1(tens[n / 10]);
    if (n % 10)
        words += QLatin1Char('-') + QString::fromLatin1(ones[n % 10]);
    return words;
}

// How long each kind of play keeps its agent from being called for the same
// thing again. Reading is constant and so almost never news; a commit always is.
qint64 cooldownMs(PlayKind kind)
{
    switch (kind) {
    case PlayKind::Read:
    case PlayKind::Search:
    case PlayKind::Think:
    case PlayKind::Speak:
        return 45 * 1000;
    case PlayKind::Web:
        return 30 * 1000;
    case PlayKind::Shell:
        return 25 * 1000;
    case PlayKind::Edit:
        return 15 * 1000;
    case PlayKind::Fumble:
        return 12 * 1000;
    case PlayKind::Test:
    case PlayKind::Build:
    case PlayKind::Delegate:
        return 10 * 1000;
    case PlayKind::Coach:
        return 5 * 1000;
    default:
        return 0;
    }
}

Priority priorityFor(PlayKind kind)
{
    switch (kind) {
    case PlayKind::Test:
    case PlayKind::Build:
    case PlayKind::Commit:
    case PlayKind::Push:
    case PlayKind::Delegate:
    case PlayKind::Fumble:
    case PlayKind::Coach:
        return Priority::Highlight;
    default:
        return Priority::Play;
    }
}

#define POOL(name, ...) const char *const name[] = {__VA_ARGS__}
#define PICK(name) pick(name, int(std::size(name)))

// %1 is the agent, %2 the detail. The first time an agent is named it comes
// with an introduction ("agent two eighty-five, working on the worker fix,"),
// so %1 is never made possessive; a pattern that opens with "%1 " may also be
// given a link from the previous call ("Meanwhile, ").
POOL(kKickOff, "%1 is off the blocks!", "%1 takes the field!",
     "Here we go! %1 is under way.", "%1 is out of the gate and running!");
POOL(kBackIn, "%1 is back in the game!", "%1 gets the nod and heads back out there.",
     "%1 is up and running again.");
POOL(kWaitingOnYou,
     "%1 has pulled up and is looking to the bench. That one needs you, coach.",
     "Time out on the field! %1 is waiting on a decision.",
     "%1 stops and signals the sideline. Your call, coach.");
POOL(kFinish, "%1 crosses the line! A clean finish, %2.",
     "And that is the final whistle for %1! Job done, %2.",
     "%1 brings it home! That is a success, %2.");
POOL(kFinishPlain, "%1 crosses the line! A clean finish.",
     "And that is the final whistle for %1! Job done.");
POOL(kFail, "Oh, and %1 goes down! That run has failed.",
     "Heartbreak for %1. That one ends in failure.",
     "%1 cannot finish. That run is marked as failed.");
POOL(kStop, "%1 has been pulled from the game.",
     "The whistle blows on %1. Stopped by the bench.", "%1 is called off the field.");

POOL(kEditIn, "%1 lands an edit in %2.", "%1 is into %2, and the change goes in.",
     "%1 with a neat touch on %2.", "%1 reworks %2. Nicely done.");
POOL(kEdit, "%1 lands another edit.", "%1 keeps the changes coming.");
POOL(kEditTally, "%1 racks up edit number %2. Busy shift.",
     "%1 is up to %2 edits now, and showing no sign of slowing.");
POOL(kTest, "%1 sends it to the test suite. This is the moment of truth.",
     "Tests are running for %1. Hold your breath.",
     "%1 steps up to the line. The tests are under way.");
POOL(kBuild, "%1 fires up the build. The compilers are warm.",
     "%1 kicks off a build, and now we wait.", "%1 goes to the compiler. Big moment.");
POOL(kCommit, "And it is in the books! %1 makes the commit!",
     "%1 commits! That work is on the board.", "%1 puts it away. That is a commit!");
POOL(kPush, "%1 pushes it upstream!", "%1 sends it up the line. That is a push.");
POOL(kShell, "%1 drops into the shell.", "%1 is working the terminal.",
     "%1 runs a command. Let us see what comes back.");
POOL(kReadOne, "%1 takes a look at %2.", "%1 is reading through %2.");
POOL(kStudy, "%1 is studying the film. %2 look-ups in a row.",
     "%1 is doing the homework, %2 reads and searches and counting.");
POOL(kSearch, "%1 is hunting through the code base.", "%1 goes searching for a way through.");
POOL(kWeb, "%1 goes out to the web for answers.", "%1 is checking the outside sources.");
POOL(kDelegate, "%1 calls for backup! A sub agent comes off the bench.",
     "%1 makes a substitution, and hands part of the job to a helper.");
POOL(kThink, "%1 slows it right down, thinking this one through.",
     "%1 is taking a moment. You can see the wheels turning.");
POOL(kSpeak, "%1 is talking it over, laying out the plan.", "%1 has something to say.");
POOL(kCoach, "Instructions coming in from the sideline for %1!",
     "The coach has a word with %1.", "New orders from the bench for %1.");
POOL(kFumble, "Ooh! That one bounces back on %1. A tool error.",
     "%1 takes a knock. That call came back with an error.",
     "A stumble from %1. The tool says no.");
POOL(kFumbleTally, "%1 is having a rough stretch. That is %2 errors now.",
     "Another error for %1, number %2. They will want to shake that off.");
POOL(kTurns, "%1 passes %2 turns, and is still going strong.",
     "%1 clocks up turn number %2. What an engine.");
POOL(kTokens, "%1 has now burned through %2 tokens. That is a serious shift.",
     "%1 goes past %2 tokens on the meter.");

POOL(kLinks, "Meanwhile, ", "Elsewhere on the field, ", "At the same time, ", "");

POOL(kSolo, "%1 is out there alone, %2.", "All eyes on %1, %2.");
POOL(kQuiet, "We have not heard from %1 in %2. Deep in thought, or stuck in traffic?",
     "%1 has gone quiet, %2 without a move.");
POOL(kStamina, "%1 has been out there for %2 now. That is stamina.",
     "%1 is %2 into this run, and still working.");
POOL(kBench, "A reminder from the booth. %1 is still waiting on you, coach.",
     "%1 is still standing at the sideline, waiting for an answer.");

// Every sentence in the line, not just the first: an agent's name can land
// after "Here we go!".
QString capitalised(QString text)
{
    bool sentenceStart = true;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?')) {
            // "1.2 million" is not the end of a sentence.
            sentenceStart = i + 1 >= text.size() || text.at(i + 1).isSpace();
        } else if (c.isLetter()) {
            if (sentenceStart)
                text[i] = c.toUpper();
            sentenceStart = false;
        } else if (c.isDigit()) {
            sentenceStart = false;
        }
    }
    return text;
}

QString plural(qint64 n, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
}

} // namespace

// ---------------------------------------------------------------------------
// Words

QString spokenNumber(int number)
{
    int n = std::abs(number);
    if (n < 100)
        return numberUnderHundred(n);
    if (n >= 10000)
        return QString::number(n);
    const int head = n / 100;
    const int rest = n % 100;
    if (rest == 0) {
        if (n % 1000 == 0)
            return numberUnderHundred(n / 1000) + QStringLiteral(" thousand");
        return numberUnderHundred(head) + QStringLiteral(" hundred");
    }
    if (rest < 10)
        return numberUnderHundred(head) + QStringLiteral(" oh ") + numberUnderHundred(rest);
    return numberUnderHundred(head) + QLatin1Char(' ') + numberUnderHundred(rest);
}

QString spokenCount(qint64 count)
{
    count = qMax<qint64>(0, count);
    if (count < 1000)
        return QString::number(count);
    if (count < 1000000)
        return QStringLiteral("%1 thousand").arg(qMax<qint64>(1, (count + 500) / 1000));
    const double millions = std::round(double(count) / 100000.0) / 10.0;
    QString figure = QString::number(millions, 'f', 1);
    if (figure.endsWith(QLatin1String(".0")))
        figure.chop(2);
    return figure + QStringLiteral(" million");
}

QString spokenFileName(const QString &path)
{
    QString base = path.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    base = base.mid(base.lastIndexOf(QLatin1Char('/')) + 1);
    while (base.startsWith(QLatin1Char('.')))
        base.remove(0, 1);
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        base.truncate(dot);
    QString spaced;
    for (int i = 0; i < base.size(); ++i) {
        const QChar c = base.at(i);
        if (!c.isLetterOrNumber()) {
            spaced += QLatin1Char(' ');
            continue;
        }
        if (i > 0) {
            const QChar prev = base.at(i - 1);
            const bool camel = c.isUpper() && prev.isLower();
            // "HTTPServer" splits before the last capital of the run.
            const bool acronymEnd = c.isUpper() && prev.isUpper() && i + 1 < base.size()
                                    && base.at(i + 1).isLower();
            const bool digitEdge = prev.isLetterOrNumber() && c.isDigit() != prev.isDigit();
            if (camel || acronymEnd || digitEdge)
                spaced += QLatin1Char(' ');
        }
        spaced += c;
    }
    QStringList words = spaced.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // README, TODO, LICENSE: said as words, not spelled out. An acronym inside
    // a longer name ("HTTP Server") is left to be spelled.
    if (words.size() == 1 && words.first().size() >= 4
        && words.first() == words.first().toUpper())
        words.first() = words.first().toLower();
    if (words.size() > 5)
        words = words.mid(0, 5);
    return words.join(QLatin1Char(' '));
}

QString spokenTitle(const QString &title, int maxWords)
{
    QString cleaned;
    for (const QChar c : title) {
        if (c.isLetterOrNumber() || c == QLatin1Char('\''))
            cleaned += c;
        else
            cleaned += QLatin1Char(' ');
    }
    QStringList words = cleaned.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // Branch-style names open with bookkeeping: "adhoc-285-announcer-mode".
    static const QStringList lead = {QStringLiteral("adhoc"), QStringLiteral("agent"),
                                     QStringLiteral("issue"), QStringLiteral("task")};
    while (!words.isEmpty()) {
        bool number = false;
        words.first().toLongLong(&number);
        if (number || lead.contains(words.first().toLower()))
            words.removeFirst();
        else
            break;
    }
    if (words.size() > maxWords) {
        words = words.mid(0, qMax(1, maxWords));
        // Do not stop on a word that leaves the phrase hanging.
        static const QStringList dangling = {
            QStringLiteral("the"), QStringLiteral("a"),    QStringLiteral("an"),
            QStringLiteral("to"),  QStringLiteral("and"),  QStringLiteral("of"),
            QStringLiteral("for"), QStringLiteral("that"), QStringLiteral("in"),
            QStringLiteral("on"),  QStringLiteral("with"), QStringLiteral("or")};
        while (words.size() > 1 && dangling.contains(words.last().toLower()))
            words.removeLast();
    }
    return words.join(QLatin1Char(' '));
}

QString spokenDuration(qint64 milliseconds)
{
    const qint64 seconds = qMax<qint64>(0, milliseconds) / 1000;
    if (seconds < 45)
        return QStringLiteral("under a minute");
    if (seconds < 90)
        return QStringLiteral("about a minute");
    if (seconds < 150)
        return QStringLiteral("a couple of minutes");
    const qint64 minutes = (seconds + 30) / 60;
    if (minutes < 55)
        return QStringLiteral("%1 minutes").arg(minutes);
    if (minutes < 65)
        return QStringLiteral("about an hour");
    if (minutes < 80)
        return QStringLiteral("just over an hour");
    const qint64 halves = (minutes + 15) / 30; // half hours, rounded
    if (halves % 2)
        return halves / 2 == 1 ? QStringLiteral("an hour and a half")
                               : QStringLiteral("%1 and a half hours").arg(halves / 2);
    return QStringLiteral("%1 hours").arg(halves / 2);
}

QString speakable(const QString &line)
{
    QString out;
    out.reserve(line.size());
    for (const QChar c : line) {
        if (c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char(',')
            || c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?')
            || c == QLatin1Char('\'') || c == QLatin1Char('-'))
            out += c;
        else
            out += QLatin1Char(' ');
    }
    out = out.simplified();
    // An introduction followed by a pattern's own comma, or an empty detail.
    static const QRegularExpression doubled(QStringLiteral("\\s*([,.!?])(\\s*[,.])+"));
    out.replace(doubled, QStringLiteral("\\1"));
    // "...for agent five, working on x,!" - the stronger mark wins.
    static const QRegularExpression commaThenStop(QStringLiteral(",\\s*([.!?])"));
    out.replace(commaThenStop, QStringLiteral("\\1"));
    static const QRegularExpression spaceBefore(QStringLiteral("\\s+([,.!?])"));
    out.replace(spaceBefore, QStringLiteral("\\1"));
    return out.trimmed();
}

QString spokenProse(const QString &markdown, int maxChars)
{
    static const QRegularExpression link(QStringLiteral("!?\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression url(QStringLiteral("\\b(?:https?|ftp)://\\S+"));
    static const QRegularExpression code(QStringLiteral("`([^`]*)`"));
    // A file, by path or by a name with a known extension: "src/Foo.cpp",
    // "entry.py". A bare "and/or" is not one.
    static const QRegularExpression path(QStringLiteral(
        "(?<![\\w@])(?:[\\w.~-]*/)+[\\w-]+\\.\\w+|\\b[\\w-]+\\.(?:cpp|h|hpp|c|py|js|ts|tsx|jsx|dart|"
        "kt|go|rs|md|json|toml|yaml|yml|sh|html|css|txt|cmake|ui|qss)\\b"));
    static const QRegularExpression listMark(QStringLiteral("^(?:[-*+]|\\d+[.)])\\s+"));

    QStringList sentences;
    bool fenced = false;
    for (QString line : markdown.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
            fenced = !fenced;
            continue;
        }
        // Code and tables do not read aloud; the words round them do.
        if (fenced || line.isEmpty() || line.startsWith(QLatin1Char('|'))
            || line == QLatin1String("---") || line == QLatin1String("***"))
            continue;
        while (line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('>')))
            line = line.mid(1).trimmed();
        line.remove(listMark);
        line.replace(link, QStringLiteral("\\1"));
        line.replace(url, QStringLiteral("a link"));
        line.replace(code, QStringLiteral("\\1"));
        for (const char *mark : {"**", "__"})
            line.remove(QLatin1String(mark));
        QString named;
        int from = 0;
        for (auto it = path.globalMatch(line); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            named += line.mid(from, match.capturedStart() - from);
            named += spokenFileName(match.captured());
            from = match.capturedEnd();
        }
        named += line.mid(from);
        line = named.simplified();
        if (line.isEmpty())
            continue;
        // A heading or list item has no full stop of its own; without one the
        // voice runs it into the next line.
        const QChar last = line.back();
        if (last != QLatin1Char('.') && last != QLatin1Char('!') && last != QLatin1Char('?')
            && last != QLatin1Char(':'))
            line += QLatin1Char('.');
        sentences << line;
    }
    QString text = sentences.join(QLatin1Char(' '));
    if (text.size() <= maxChars)
        return text;
    // Stop at the last full stop that leaves most of the budget said, or else
    // at a word, rather than in the middle of one.
    const QString head = text.left(maxChars);
    int stop = -1;
    for (const QChar mark : {QLatin1Char('.'), QLatin1Char('!'), QLatin1Char('?')})
        stop = qMax(stop, int(head.lastIndexOf(QString(mark) + QLatin1Char(' '))));
    if (stop >= maxChars / 2)
        return head.left(stop + 1);
    const int space = head.lastIndexOf(QLatin1Char(' '));
    return (space > 0 ? head.left(space) : head) + QLatin1Char('.');
}

// ---------------------------------------------------------------------------
// Plays

PlayKind playForCommand(const QString &command)
{
    const QString c = command.toLower();
    auto has = [&c](const char *needle) { return c.contains(QLatin1String(needle)); };
    if (has("git commit"))
        return PlayKind::Commit;
    if (has("git push"))
        return PlayKind::Push;
    if (has("pytest") || has("ctest") || has("cargo test") || has("go test")
        || has("npm test") || has("npm run test") || has("yarn test") || has("pnpm test")
        || has("dart test") || has("jest") || has("vitest")
        || has("playwright") || has("gradlew test") || has("-tests") || has("unittest"))
        return PlayKind::Test;
    if (has("ninja") || has("cmake --build") || has("make ") || c.trimmed() == QLatin1String("make")
        || has("cargo build") || has("go build") || has("npm run build") || has("tsc")
        || has("gradlew") || has("xcodebuild"))
        return PlayKind::Build;
    return PlayKind::Shell;
}

Play playForEvent(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("assistant")) {
        const QJsonArray content = event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue &bv : content) {
            const QJsonObject block = bv.toObject();
            if (block.value(QStringLiteral("type")).toString() != QLatin1String("tool_use"))
                continue;
            const QString name = block.value(QStringLiteral("name")).toString().toLower();
            const QJsonObject input = block.value(QStringLiteral("input")).toObject();
            QString file = input.value(QStringLiteral("file_path")).toString();
            if (file.isEmpty())
                file = input.value(QStringLiteral("notebook_path")).toString();
            if (file.isEmpty())
                file = input.value(QStringLiteral("path")).toString();
            if (name == QLatin1String("todowrite"))
                return {};
            if (name.contains(QLatin1String("edit")) || name == QLatin1String("write")
                || name.contains(QLatin1String("patch")))
                return {PlayKind::Edit, spokenFileName(file)};
            if (name == QLatin1String("bash") || name.contains(QLatin1String("shell"))
                || name.contains(QLatin1String("exec")))
                return {playForCommand(input.value(QStringLiteral("command")).toString()),
                        QString()};
            if (name == QLatin1String("read"))
                return {PlayKind::Read, spokenFileName(file)};
            if (name.startsWith(QLatin1String("web")))
                return {PlayKind::Web, QString()};
            break; // anything else: the sonifier's coarser reading will do
        }
    }
    if (type == QLatin1String("user")) {
        const QJsonArray content = event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue &bv : content) {
            const QJsonObject block = bv.toObject();
            if (block.value(QStringLiteral("type")).toString() == QLatin1String("tool_result")
                && block.value(QStringLiteral("is_error")).toBool())
                return {PlayKind::Fumble, QString()};
        }
    }
    switch (sonify::motionForEvent(event)) {
    case sonify::Motion::Speak:
        return {PlayKind::Speak, QString()};
    case sonify::Motion::Think:
        return {PlayKind::Think, QString()};
    case sonify::Motion::Read:
        return {PlayKind::Read, QString()};
    case sonify::Motion::Search:
    case sonify::Motion::Scan:
        return {PlayKind::Search, QString()};
    case sonify::Motion::Write:
    case sonify::Motion::Create:
        return {PlayKind::Edit, QString()};
    case sonify::Motion::Run:
    case sonify::Motion::Git:
    case sonify::Motion::Install:
    case sonify::Motion::Spawn:
        return {PlayKind::Shell, QString()};
    case sonify::Motion::Test:
        return {PlayKind::Test, QString()};
    case sonify::Motion::Build:
        return {PlayKind::Build, QString()};
    case sonify::Motion::Fumble:
    case sonify::Motion::Denied:
        return {PlayKind::Fumble, QString()};
    case sonify::Motion::Delegate:
    case sonify::Motion::Skill:
    case sonify::Motion::Mcp:
        return {PlayKind::Delegate, QString()};
    case sonify::Motion::Prompt:
        return {PlayKind::Coach, QString()};
    default:
        // Start is covered by the status change, which knows whether this is
        // a first run or a resume; arrivals and notices are not news.
        return {};
    }
}

qint64 shelfLifeMs(Priority priority)
{
    switch (priority) {
    case Priority::Headline:
        return 90 * 1000;
    case Priority::Highlight:
        return 25 * 1000;
    default:
        return 10 * 1000;
    }
}

// ---------------------------------------------------------------------------
// Commentator

Commentator::Commentator()
    : m_random(QRandomGenerator::securelySeeded())
{
}

void Commentator::setSeed(quint32 seed)
{
    m_random.seed(seed);
}

int Commentator::runningAgents() const
{
    int running = 0;
    for (const Agent &a : m_agents)
        running += a.facts.status == kRunning ? 1 : 0;
    return running;
}

Commentator::Agent &Commentator::agent(int sessionId, qint64 nowMs)
{
    auto it = m_agents.find(sessionId);
    if (it == m_agents.end()) {
        Agent fresh;
        fresh.facts.id = sessionId;
        fresh.firstSeenMs = nowMs;
        it = m_agents.insert(sessionId, fresh);
    }
    return it.value();
}

QString Commentator::pick(const char *const *pool, int count)
{
    if (count <= 0)
        return QString();
    int index = int(m_random.bounded(quint32(count)));
    const quintptr key = quintptr(pool);
    // Never the same line twice running from one pool.
    if (count > 1 && m_lastPick.value(key, -1) == index)
        index = (index + 1) % count;
    m_lastPick.insert(key, index);
    return QString::fromUtf8(pool[index]);
}

QString Commentator::nameFor(Agent &agent, bool allowIntroduction)
{
    const QString title = spokenTitle(agent.facts.title);
    // Sessions from outside the app (external CLIs, cloud runs) have no
    // number anyone would recognise; they go by what they are working on.
    if (agent.facts.id < 0) {
        agent.introduced = true;
        return title.isEmpty() ? QStringLiteral("an outside agent")
                               : QStringLiteral("the outside agent on %1").arg(title);
    }
    const QString number = spokenNumber(agent.facts.id);
    if (allowIntroduction && !agent.introduced && !title.isEmpty()) {
        agent.introduced = true;
        return QStringLiteral("agent %1, working on %2,").arg(number, title);
    }
    switch (m_random.bounded(4u)) {
    case 0:
        return QStringLiteral("number %1").arg(number);
    case 1:
        return number;
    default:
        return QStringLiteral("agent %1").arg(number);
    }
}

QString Commentator::render(const QString &pattern, Agent &agent, const QString &detail,
                            bool mayLink)
{
    const bool opensWithName = pattern.startsWith(QLatin1String("%1 "));
    QString text = pattern;
    text.replace(QLatin1String("%1"), nameFor(agent, true));
    text.replace(QLatin1String("%2"), detail);
    if (mayLink && opensWithName && m_lastSpokenSession != -1
        && m_lastSpokenSession != agent.facts.id)
        text.prepend(PICK(kLinks));
    return capitalised(speakable(text));
}

void Commentator::queue(Agent &agent, PlayKind kind, Priority priority,
                        const QString &pattern, const QString &detail, qint64 nowMs)
{
    if (pattern.isEmpty())
        return;
    Pending call;
    call.pattern = pattern;
    call.detail = detail;
    call.priority = priority;
    call.kind = kind;
    call.sessionId = agent.facts.id;
    call.atMs = nowMs;
    m_pending.append(call);
    agent.lastKindMs.insert(int(kind), nowMs);
    agent.studyStreak = 0;
    if (m_pending.size() <= kMaxPending)
        return;
    // Over the limit, the oldest of the least important goes.
    int victim = 0;
    for (int i = 1; i < m_pending.size(); ++i) {
        if (m_pending.at(i).priority < m_pending.at(victim).priority)
            victim = i;
    }
    m_pending.remove(victim);
}

void Commentator::callStatusChange(Agent &agent, const QString &from, qint64 nowMs)
{
    const QString &to = agent.facts.status;
    // A new state makes what was queued about the old one untrue.
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [&agent](const Pending &p) {
                                       return p.sessionId == agent.facts.id
                                              && p.priority == Priority::Headline;
                                   }),
                    m_pending.end());
    if (to == kRunning) {
        m_signedOff = false;
        agent.lastEventMs = nowMs;
        const bool resumed = from == kWaiting || from == kSuccess || from == kFailed
                             || from == kStopped;
        queue(agent, PlayKind::KickOff, Priority::Headline,
              resumed ? PICK(kBackIn) : PICK(kKickOff), QString(), nowMs);
    } else if (to == kWaiting) {
        queue(agent, PlayKind::None, Priority::Headline, PICK(kWaitingOnYou), QString(),
              nowMs);
    } else if (to == kSuccess) {
        ++m_finishes;
        QString stats;
        if (agent.facts.turns > 0)
            stats = plural(agent.facts.turns, "turn", "turns");
        if (agent.facts.tokens >= 1000)
            stats += (stats.isEmpty() ? QString() : QStringLiteral(" and "))
                     + spokenCount(agent.facts.tokens) + QStringLiteral(" tokens");
        queue(agent, PlayKind::None, Priority::Headline,
              stats.isEmpty() ? PICK(kFinishPlain) : PICK(kFinish), stats, nowMs);
    } else if (to == kFailed) {
        ++m_failures;
        queue(agent, PlayKind::None, Priority::Headline, PICK(kFail), QString(), nowMs);
    } else if (to == kStopped) {
        queue(agent, PlayKind::None, Priority::Headline, PICK(kStop), QString(), nowMs);
    }
}

void Commentator::callMilestones(Agent &agent, qint64 nowMs)
{
    const int turnMark = agent.facts.turns / 25 * 25;
    if (turnMark > agent.turnMark) {
        agent.turnMark = turnMark;
        queue(agent, PlayKind::None, Priority::Highlight, PICK(kTurns),
              QString::number(turnMark), nowMs);
    }
    static const qint64 marks[] = {100000, 250000, 500000, 1000000, 2000000, 5000000};
    qint64 tokenMark = 0;
    for (qint64 mark : marks) {
        if (agent.facts.tokens >= mark)
            tokenMark = mark;
    }
    if (tokenMark > agent.tokenMark) {
        agent.tokenMark = tokenMark;
        queue(agent, PlayKind::None, Priority::Highlight, PICK(kTokens),
              spokenCount(tokenMark), nowMs);
    }
}

void Commentator::updateAgent(const AgentFacts &facts, qint64 nowMs)
{
    const bool known = m_agents.contains(facts.id);
    Agent &a = agent(facts.id, nowMs);
    const QString before = a.facts.status;
    a.facts = facts;
    if (!known || before.isEmpty()) {
        // First sight. What it has already done is history, not news - and
        // before the broadcast opens, the opening line does the introductions.
        a.turnMark = facts.turns / 25 * 25;
        a.tokenMark = facts.tokens;
        if (facts.status == kRunning) {
            a.lastEventMs = nowMs;
            if (m_opened)
                callStatusChange(a, QString(), nowMs);
        }
        return;
    }
    if (before != facts.status)
        callStatusChange(a, before, nowMs);
    if (facts.status == kRunning)
        callMilestones(a, nowMs);
}

void Commentator::forgetAgent(int sessionId)
{
    m_agents.remove(sessionId);
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [sessionId](const Pending &p) {
                                       return p.sessionId == sessionId;
                                   }),
                    m_pending.end());
}

void Commentator::handleEvent(int sessionId, const QJsonObject &event, qint64 nowMs)
{
    const Play play = playForEvent(event);
    if (play.kind == PlayKind::None)
        return;
    Agent &a = agent(sessionId, nowMs);
    a.lastEventMs = nowMs;
    a.quietCalledMs = 0;

    const bool study = play.kind == PlayKind::Read || play.kind == PlayKind::Search;
    if (study)
        ++a.studyStreak;
    if (play.kind == PlayKind::Edit)
        ++a.edits;
    if (play.kind == PlayKind::Fumble)
        ++a.fumbles;

    const qint64 last = a.lastKindMs.value(int(play.kind), 0);
    if (last > 0 && nowMs - last < cooldownMs(play.kind))
        return;
    // Reading is what agents do all day. It is worth a remark when it has
    // become a streak, or when there has been nothing else to say about them.
    if (study && a.studyStreak < 4 && nowMs - a.lastMentionMs < 20 * 1000)
        return;

    const Priority priority = priorityFor(play.kind);
    switch (play.kind) {
    case PlayKind::Edit:
        if (a.edits > 0 && a.edits % 10 == 0)
            queue(a, play.kind, Priority::Highlight, PICK(kEditTally),
                  QString::number(a.edits), nowMs);
        else if (!play.detail.isEmpty())
            queue(a, play.kind, priority, PICK(kEditIn), play.detail, nowMs);
        else
            queue(a, play.kind, priority, PICK(kEdit), QString(), nowMs);
        break;
    case PlayKind::Test:
        queue(a, play.kind, priority, PICK(kTest), QString(), nowMs);
        break;
    case PlayKind::Build:
        queue(a, play.kind, priority, PICK(kBuild), QString(), nowMs);
        break;
    case PlayKind::Commit:
        queue(a, play.kind, priority, PICK(kCommit), QString(), nowMs);
        break;
    case PlayKind::Push:
        queue(a, play.kind, priority, PICK(kPush), QString(), nowMs);
        break;
    case PlayKind::Shell:
        queue(a, play.kind, priority, PICK(kShell), QString(), nowMs);
        break;
    case PlayKind::Read:
    case PlayKind::Search: {
        const int streak = a.studyStreak;
        if (streak >= 4)
            queue(a, play.kind, priority, PICK(kStudy), QString::number(streak), nowMs);
        else if (play.kind == PlayKind::Read && !play.detail.isEmpty())
            queue(a, play.kind, priority, PICK(kReadOne), play.detail, nowMs);
        else
            queue(a, play.kind, priority, PICK(kSearch), QString(), nowMs);
        break;
    }
    case PlayKind::Web:
        queue(a, play.kind, priority, PICK(kWeb), QString(), nowMs);
        break;
    case PlayKind::Delegate:
        queue(a, play.kind, priority, PICK(kDelegate), QString(), nowMs);
        break;
    case PlayKind::Think:
        queue(a, play.kind, priority, PICK(kThink), QString(), nowMs);
        break;
    case PlayKind::Speak:
        queue(a, play.kind, priority, PICK(kSpeak), QString(), nowMs);
        break;
    case PlayKind::Coach:
        queue(a, play.kind, priority, PICK(kCoach), QString(), nowMs);
        break;
    case PlayKind::Fumble:
        if (a.fumbles >= 3)
            queue(a, play.kind, priority, PICK(kFumbleTally), QString::number(a.fumbles),
                  nowMs);
        else
            queue(a, play.kind, priority, PICK(kFumble), QString(), nowMs);
        break;
    default:
        break;
    }
}

QString Commentator::openingLine()
{
    QList<Agent *> running;
    int waiting = 0;
    for (Agent &a : m_agents) {
        if (a.facts.status == kRunning)
            running.append(&a);
        else if (a.facts.status == kWaiting)
            ++waiting;
    }
    std::sort(running.begin(), running.end(),
              [](const Agent *l, const Agent *r) { return l->facts.id < r->facts.id; });
    QString line = QStringLiteral("Welcome to ForkMesh live! ");
    if (running.isEmpty()) {
        m_signedOff = true; // the empty field has just been described
        line += QStringLiteral("The field is empty right now, but I will call it the "
                               "moment anyone moves.");
    } else if (running.size() == 1) {
        line += QStringLiteral("We have one agent on the field. ");
    } else {
        line += QStringLiteral("We have %1 agents on the field. ").arg(running.size());
    }
    if (!running.isEmpty() && running.size() <= 3) {
        QStringList roster;
        for (Agent *a : running) {
            const QString title = spokenTitle(a->facts.title, 5);
            a->introduced = true;
            if (a->facts.id < 0)
                roster << (title.isEmpty() ? QStringLiteral("an outside agent")
                                           : QStringLiteral("an outside agent on %1").arg(title));
            else if (title.isEmpty())
                roster << QStringLiteral("agent %1").arg(spokenNumber(a->facts.id));
            else
                roster << QStringLiteral("agent %1 on %2").arg(spokenNumber(a->facts.id), title);
        }
        line += QStringLiteral("Out there right now, ") + roster.join(QStringLiteral(", and "))
                + QStringLiteral(". ");
    }
    if (waiting > 0)
        line += waiting == 1
                    ? QStringLiteral("And one more at the sideline, waiting on a decision.")
                    : QStringLiteral("And %1 more at the sideline, waiting on a decision.")
                          .arg(waiting);
    return capitalised(speakable(line));
}

QString Commentator::colourLine(qint64 nowMs)
{
    QList<Agent *> running;
    Agent *benched = nullptr;
    for (Agent &a : m_agents) {
        if (a.facts.status == kRunning)
            running.append(&a);
        else if (a.facts.status == kWaiting && !benched)
            benched = &a;
    }
    if (running.isEmpty()) {
        if (benched && nowMs - m_lastCallMs >= kBenchReminderMs)
            return render(PICK(kBench), *benched, QString(), false);
        if (m_signedOff)
            return QString();
        m_signedOff = true;
        return QStringLiteral("And that clears the field. No agents running. I will be "
                              "right here when the action picks up.");
    }
    std::sort(running.begin(), running.end(), [](const Agent *l, const Agent *r) {
        return l->facts.turns != r->facts.turns ? l->facts.turns > r->facts.turns
                                                : l->facts.id < r->facts.id;
    });
    // Five kinds of filler, taken in turn; a kind with nothing to say passes.
    for (int attempt = 0; attempt < 5; ++attempt) {
        const int turn = m_colourTurn++ % 5;
        if (turn == 0) {
            Agent *leader = running.first();
            if (running.size() == 1) {
                const QString stats =
                    plural(leader->facts.turns, "turn", "turns") + QStringLiteral(" in and ")
                    + spokenCount(leader->facts.tokens) + QStringLiteral(" tokens on the clock");
                return render(PICK(kSolo), *leader, stats, false);
            }
            if (leader->facts.turns <= 0)
                continue;
            Agent *second = running.at(1);
            return capitalised(speakable(
                QStringLiteral("A look at the board. %1 leads the pack with %2, and %3 is "
                               "right behind on %4.")
                    .arg(nameFor(*leader, false), plural(leader->facts.turns, "turn", "turns"),
                         nameFor(*second, false))
                    .arg(second->facts.turns)));
        }
        if (turn == 1) {
            for (Agent *a : running) {
                if (a->lastEventMs > 0 && nowMs - a->lastEventMs >= kQuietMs
                    && (a->quietCalledMs == 0 || nowMs - a->quietCalledMs >= kQuietRepeatMs)) {
                    a->quietCalledMs = nowMs;
                    return render(PICK(kQuiet), *a, spokenDuration(nowMs - a->lastEventMs),
                                  false);
                }
            }
            continue;
        }
        if (turn == 2) {
            Agent *veteran = nullptr;
            for (Agent *a : running) {
                if (a->facts.runningSinceMs > 0
                    && (!veteran || a->facts.runningSinceMs < veteran->facts.runningSinceMs))
                    veteran = a;
            }
            if (!veteran || nowMs - veteran->facts.runningSinceMs < 150 * 1000)
                continue;
            return render(PICK(kStamina), *veteran,
                          spokenDuration(nowMs - veteran->facts.runningSinceMs), false);
        }
        if (turn == 3) {
            const QString still = running.size() == 1
                                      ? QStringLiteral("one still running")
                                      : QStringLiteral("%1 still running").arg(running.size());
            // One agent and nothing finished is not a scoreboard.
            if (m_finishes + m_failures == 0 && running.size() < 2)
                continue;
            if (m_finishes + m_failures == 0)
                return capitalised(speakable(
                    QStringLiteral("%1, and no finishes yet. It is anybody's game.").arg(still)));
            return capitalised(speakable(
                QStringLiteral("On the session so far, %1, %2, and %3.")
                    .arg(plural(m_finishes, "finish", "finishes"),
                         plural(m_failures, "failure", "failures"), still)));
        }
        if (benched)
            return render(PICK(kBench), *benched, QString(), false);
    }
    return QString();
}

Call Commentator::nextCall(qint64 nowMs)
{
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [nowMs](const Pending &p) {
                                       return nowMs - p.atMs > shelfLifeMs(p.priority);
                                   }),
                    m_pending.end());
    Call call;
    call.atMs = nowMs;
    if (!m_opened) {
        m_opened = true;
        // Whatever piled up before the microphone went live is old news.
        m_pending.clear();
        call.text = openingLine();
        call.priority = Priority::Headline;
        m_lastCallMs = nowMs;
        return call;
    }
    if (m_pending.isEmpty()) {
        const qint64 gap = m_lastWasColour ? kColourRepeatGapMs : kColourGapMs;
        if (nowMs - m_lastCallMs < gap)
            return call;
        call.text = colourLine(nowMs);
        call.priority = Priority::Colour;
        if (!call.text.isEmpty()) {
            m_lastCallMs = nowMs;
            m_lastWasColour = true;
        }
        return call;
    }
    // The most important thing first. Headlines in the order they happened;
    // below that, whoever has been out of the commentary longest, newest play.
    int best = 0;
    for (int i = 1; i < m_pending.size(); ++i) {
        const Pending &p = m_pending.at(i);
        const Pending &b = m_pending.at(best);
        if (p.priority != b.priority) {
            if (p.priority > b.priority)
                best = i;
            continue;
        }
        if (p.priority == Priority::Headline)
            continue; // the earlier one stands
        const qint64 pMention = m_agents.value(p.sessionId).lastMentionMs;
        const qint64 bMention = m_agents.value(b.sessionId).lastMentionMs;
        if (p.sessionId == b.sessionId ? p.atMs >= b.atMs : pMention < bMention)
            best = i;
    }
    const Pending chosen = m_pending.takeAt(best);
    if (chosen.priority != Priority::Headline) {
        // Saying the newest thing about an agent retires its older small talk.
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [&chosen](const Pending &p) {
                                           return p.sessionId == chosen.sessionId
                                                  && p.priority == Priority::Play;
                                       }),
                        m_pending.end());
    }
    Agent &a = agent(chosen.sessionId, nowMs);
    if (a.lastMentionMs > 0 && nowMs - a.lastMentionMs > kReintroduceMs)
        a.introduced = false;
    call.text = render(chosen.pattern, a, chosen.detail,
                       chosen.priority != Priority::Headline);
    call.priority = chosen.priority;
    call.kind = chosen.kind;
    call.sessionId = chosen.sessionId;
    a.lastMentionMs = nowMs;
    m_lastSpokenSession = chosen.sessionId;
    m_lastCallMs = nowMs;
    m_lastWasColour = false;
    return call;
}

QString Commentator::signOff()
{
    return QStringLiteral("That is all from the booth. This has been ForkMesh live.");
}

// ---------------------------------------------------------------------------
// The voice

QString voiceDirectory()
{
    const QByteArray forced = qgetenv("FORKMESH_ANNOUNCER_DIR");
    if (!forced.isEmpty())
        return QString::fromLocal8Bit(forced);
    // Machine-local: the engine is a native binary for this CPU.
    return forkmesh::paths::localDataRoot() + QStringLiteral("/announcer");
}

int voiceSampleRate(const QString &modelPath)
{
    QFile config(modelPath + QStringLiteral(".json"));
    if (!config.open(QIODevice::ReadOnly))
        return 22050;
    const int rate = QJsonDocument::fromJson(config.readAll())
                         .object()
                         .value(QStringLiteral("audio"))
                         .toObject()
                         .value(QStringLiteral("sample_rate"))
                         .toInt();
    return rate >= 8000 && rate <= 48000 ? rate : 22050;
}

namespace {

QString firstVoiceIn(const QString &directory)
{
    QDir dir(directory);
    if (!dir.exists())
        return QString();
    QStringList models = dir.entryList({QStringLiteral("*.onnx")}, QDir::Files, QDir::Name);
    // An English voice if there is one: the commentary is written in English.
    std::stable_sort(models.begin(), models.end(), [](const QString &l, const QString &r) {
        return l.startsWith(QLatin1String("en_")) && !r.startsWith(QLatin1String("en_"));
    });
    for (const QString &model : models) {
        const QString path = dir.filePath(model);
        if (QFileInfo::exists(path + QStringLiteral(".json")))
            return path;
    }
    return QString();
}

bool isExecutableFile(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable();
}

} // namespace

VoicePaths locateVoice()
{
    VoicePaths voice;
    const QString envPiper = QString::fromLocal8Bit(qgetenv("FORKMESH_ANNOUNCER_PIPER"));
    const QString envModel = QString::fromLocal8Bit(qgetenv("FORKMESH_ANNOUNCER_MODEL"));
    const QString own = voiceDirectory();
#ifdef Q_OS_WIN
    const QString ownPiper = own + QStringLiteral("/piper/piper.exe");
#else
    const QString ownPiper = own + QStringLiteral("/piper/piper");
#endif
    if (isExecutableFile(envPiper)) {
        voice.piper = envPiper;
    } else if (isExecutableFile(ownPiper)) {
        voice.piper = ownPiper;
    } else {
        voice.piper = QStandardPaths::findExecutable(QStringLiteral("piper-tts"));
        if (voice.piper.isEmpty()) {
            // A bare `piper` on PATH is, on most distributions, the gaming
            // mouse configurator. Only the one that ships with its phoneme
            // data beside it is the speech engine.
            const QString candidate = QStandardPaths::findExecutable(QStringLiteral("piper"));
            if (!candidate.isEmpty()) {
                const QString home = QFileInfo(QFileInfo(candidate).canonicalFilePath()).path();
                if (QFileInfo::exists(home + QStringLiteral("/espeak-ng-data")))
                    voice.piper = candidate;
            }
        }
    }
    if (!envModel.isEmpty() && QFileInfo::exists(envModel)) {
        voice.model = envModel;
    } else {
        const QString home = QDir::homePath();
        const QStringList places = {own,
                                    home + QStringLiteral("/.local/share/piper-voices"),
                                    home + QStringLiteral("/.local/share/piper/voices"),
                                    QStringLiteral("/usr/local/share/piper-voices"),
                                    QStringLiteral("/usr/share/piper-voices")};
        for (const QString &place : places) {
            voice.model = firstVoiceIn(place);
            if (!voice.model.isEmpty())
                break;
        }
    }
    if (!voice.model.isEmpty())
        voice.sampleRate = voiceSampleRate(voice.model);
    return voice;
}

QByteArray resampleToPlayerFormat(const QByteArray &mono, int fromRate, qint16 *carry)
{
    const int frames = mono.size() / 2;
    if (frames <= 0 || fromRate <= 0)
        return QByteArray();
    const qint16 *in = reinterpret_cast<const qint16 *>(mono.constData());
    // The input, one sample late, with the previous chunk's last sample in
    // front: every output frame then lies between two known inputs. For the
    // usual 22.05 kHz voice the step is exactly a half and the join is exact.
    auto sample = [&](int index) -> double {
        return index <= 0 ? double(carry ? *carry : 0) : double(in[index - 1]);
    };
    const double step = double(fromRate) / double(sonify::kSampleRate);
    const int outFrames = int(std::floor(double(frames) / step));
    QByteArray out;
    out.resize(outFrames * 2 * int(sizeof(qint16)));
    qint16 *dst = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < outFrames; ++i) {
        const double pos = i * step;
        const int index = int(pos);
        const double t = pos - index;
        const double value = sample(index) * (1.0 - t) + sample(index + 1) * t;
        const qint16 s = qint16(qBound(-32768.0, std::round(value), 32767.0));
        dst[2 * i] = s;
        dst[2 * i + 1] = s;
    }
    if (carry)
        *carry = in[frames - 1];
    return out;
}

Speaker::Speaker(QObject *parent)
    : QObject(parent)
{
}

Speaker::~Speaker()
{
    stop();
}

QString Speaker::backendName() const
{
    switch (m_backend) {
    case Backend::Piper:
        return QStringLiteral("piper");
    case Backend::Silent:
        return QStringLiteral("silent");
    default:
        return QStringLiteral("none");
    }
}

bool Speaker::start()
{
    if (m_backend != Backend::None)
        return true;
    m_stopping = false;
    m_restarts = 0;
    const QByteArray forced = qgetenv("FORKMESH_ANNOUNCER_BACKEND");
    if (forced == "none")
        return false;
    if (forced == "silent") {
        m_backend = Backend::Silent;
        return true;
    }
    m_voice = locateVoice();
    if (!m_voice.ok() || !startProcesses())
        return false;
    m_backend = Backend::Piper;
    return true;
}

bool Speaker::startProcesses()
{
    QString playerProgram;
    for (const QString &program : sonify::Player::pipePrograms()) {
        if (!QStandardPaths::findExecutable(program).isEmpty()
            && !sonify::Player::pipeArgumentsFor(program).isEmpty()) {
            playerProgram = program;
            break;
        }
    }
    if (playerProgram.isEmpty())
        return false;

    m_player = new QProcess(this);
    m_player->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_player->setStandardOutputFile(QProcess::nullDevice());
    m_player->start(playerProgram, sonify::Player::pipeArgumentsFor(playerProgram));

    m_piper = new QProcess(this);
    m_piper->setWorkingDirectory(QFileInfo(m_voice.piper).path());
    connect(m_piper, &QProcess::readyReadStandardOutput, this, &Speaker::onSpeech);
    connect(m_piper, &QProcess::readyReadStandardError, this, &Speaker::onPiperLog);
    connect(m_piper, &QProcess::finished, this, [this] {
        if (m_stopping)
            return;
        // Give it a few lives: a voice that dies on one odd line should not
        // end the broadcast, one that cannot start at all should.
        QProcess *dead = m_piper;
        QProcess *player = m_player;
        m_piper = nullptr;
        m_player = nullptr;
        m_synthesising = false;
        if (dead)
            dead->deleteLater();
        if (player) {
            player->disconnect(this);
            player->kill();
            player->deleteLater();
        }
        if (++m_restarts > 3 || !startProcesses())
            fail(QStringLiteral("The announcer's voice stopped and could not be restarted."));
    });
    // A little quicker and a little less even than the defaults: a caster,
    // not an audiobook.
    m_piper->start(m_voice.piper,
                   {QStringLiteral("--model"), m_voice.model, QStringLiteral("--output_raw"),
                    QStringLiteral("--length_scale"), QStringLiteral("0.92"),
                    QStringLiteral("--noise_scale"), QStringLiteral("0.75"),
                    QStringLiteral("--sentence_silence"), QStringLiteral("0.18")});
    m_carry = 0;
    m_oddByte = false;
    m_playEndMs = 0;
    return true;
}

void Speaker::stop()
{
    m_stopping = true;
    m_backend = Backend::None;
    m_synthesising = false;
    m_playEndMs = 0;
    if (m_synthesisGuard)
        m_synthesisGuard->stop();
    for (QProcess *process : {m_piper, m_player}) {
        if (!process)
            continue;
        // Disconnected first: nothing here wants to hear about a death it
        // caused, least of all halfway through its own destructor.
        process->disconnect(this);
        process->kill();
        process->waitForFinished(200);
        process->deleteLater();
    }
    m_piper = nullptr;
    m_player = nullptr;
}

void Speaker::fail(const QString &reason)
{
    stop();
    if (onFailed)
        onFailed(reason);
}

void Speaker::say(const QString &line)
{
    const QString text = speakable(line);
    if (text.isEmpty() || m_backend == Backend::None)
        return;
    m_recent.append(text);
    while (m_recent.size() > 50)
        m_recent.removeFirst();
    if (m_backend != Backend::Piper || !m_piper)
        return;
    if (m_player && m_player->state() == QProcess::NotRunning) {
        // The system player went away (a sound server restart): bring it back
        // rather than synthesise into nothing.
        const QString program = m_player->program();
        m_player->start(program, sonify::Player::pipeArgumentsFor(program));
        m_playEndMs = 0;
    }
    m_synthesising = true;
    if (!m_synthesisGuard) {
        m_synthesisGuard = new QTimer(this);
        m_synthesisGuard->setSingleShot(true);
        // Piper reports the end of each utterance on stderr; if that line
        // ever changes, this keeps the broadcast from waiting on it forever.
        connect(m_synthesisGuard, &QTimer::timeout, this, [this] { m_synthesising = false; });
    }
    m_synthesisGuard->start(20 * 1000);
    m_piper->write(text.toUtf8() + '\n');
}

bool Speaker::busy(qint64 nowMs) const
{
    if (m_backend != Backend::Piper)
        return false;
    // A breath between lines, so two calls do not run into one sentence.
    return m_synthesising || nowMs < m_playEndMs + 350;
}

void Speaker::onSpeech()
{
    if (!m_piper)
        return;
    QByteArray pcm = m_piper->readAllStandardOutput();
    if (m_oddByte) {
        pcm.prepend(m_heldByte);
        m_oddByte = false;
    }
    if (pcm.size() % 2) {
        m_heldByte = pcm.at(pcm.size() - 1);
        m_oddByte = true;
        pcm.chop(1);
    }
    if (pcm.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 milliseconds = qint64(pcm.size() / 2) * 1000 / qMax(1, m_voice.sampleRate);
    m_playEndMs = qMax(now, m_playEndMs) + milliseconds;
    if (m_player && m_player->state() != QProcess::NotRunning)
        m_player->write(resampleToPlayerFormat(pcm, m_voice.sampleRate, &m_carry));
}

void Speaker::onPiperLog()
{
    if (!m_piper)
        return;
    m_logTail += m_piper->readAllStandardError();
    if (m_logTail.contains("Real-time factor")) {
        m_synthesising = false;
        m_logTail.clear();
        if (m_synthesisGuard)
            m_synthesisGuard->stop();
    } else if (m_logTail.size() > 4096) {
        m_logTail = m_logTail.right(1024);
    }
}

} // namespace forkmesh::announce
