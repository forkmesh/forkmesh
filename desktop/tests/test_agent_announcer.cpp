// The commentary, checked without a voice: how things are worded for a speech
// engine, what each transcript event amounts to, and what the commentator
// chooses to say - and not say - as a fleet of agents runs. Qt Core alone.

#include "AgentAnnouncer.h"
#include "AgentSonifier.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::announce;

static int failures = 0;

static void check(bool ok, const QString &what)
{
    if (ok)
        return;
    std::cerr << "FAIL: " << qPrintable(what) << "\n";
    ++failures;
}

static void checkEqual(const QString &got, const QString &want, const QString &what)
{
    check(got == want, QStringLiteral("%1: got \"%2\", want \"%3\"").arg(what, got, want));
}

static QJsonObject parse(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

static QJsonObject toolUse(const char *name, const char *inputJson)
{
    return parse(QByteArray(R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":")")
                     .append(name)
                     .append(R"(","input":)")
                     .append(inputJson)
                     .append("}]}}"));
}

static QJsonObject toolResult(bool error)
{
    return parse(QByteArray(R"({"type":"user","message":{"content":[{"type":"tool_result","is_error":)")
                     .append(error ? "true" : "false")
                     .append(R"(,"content":"x"}]}})"));
}

static AgentFacts facts(int id, const char *title, const char *status, int turns = 0,
                        qint64 tokens = 0)
{
    AgentFacts f;
    f.id = id;
    f.title = QString::fromLatin1(title);
    f.status = QString::fromLatin1(status);
    f.turns = turns;
    f.tokens = tokens;
    f.runningSinceMs = 1000;
    return f;
}

static void testWords()
{
    checkEqual(spokenNumber(7), QStringLiteral("seven"), QStringLiteral("number 7"));
    checkEqual(spokenNumber(42), QStringLiteral("forty-two"), QStringLiteral("number 42"));
    checkEqual(spokenNumber(285), QStringLiteral("two eighty-five"), QStringLiteral("number 285"));
    checkEqual(spokenNumber(205), QStringLiteral("two oh five"), QStringLiteral("number 205"));
    checkEqual(spokenNumber(300), QStringLiteral("three hundred"), QStringLiteral("number 300"));
    checkEqual(spokenNumber(1234), QStringLiteral("twelve thirty-four"),
               QStringLiteral("number 1234"));
    checkEqual(spokenNumber(2000), QStringLiteral("two thousand"), QStringLiteral("number 2000"));
    checkEqual(spokenNumber(2164), QStringLiteral("twenty-one sixty-four"),
               QStringLiteral("number 2164"));

    checkEqual(spokenCount(950), QStringLiteral("950"), QStringLiteral("count 950"));
    checkEqual(spokenCount(45200), QStringLiteral("45 thousand"), QStringLiteral("count 45200"));
    checkEqual(spokenCount(1240000), QStringLiteral("1.2 million"), QStringLiteral("count 1.24M"));
    checkEqual(spokenCount(2000000), QStringLiteral("2 million"), QStringLiteral("count 2M"));

    checkEqual(spokenFileName(QStringLiteral("desktop/src/MainWindowAgents.cpp")),
               QStringLiteral("Main Window Agents"), QStringLiteral("camel case file"));
    checkEqual(spokenFileName(QStringLiteral("worker/entry.py")), QStringLiteral("entry"),
               QStringLiteral("plain file"));
    checkEqual(spokenFileName(QStringLiteral("README.md")), QStringLiteral("readme"),
               QStringLiteral("shouted file"));
    checkEqual(spokenFileName(QStringLiteral("src/test_agent_sonifier.cpp")),
               QStringLiteral("test agent sonifier"), QStringLiteral("snake case file"));
    checkEqual(spokenFileName(QStringLiteral(".gitignore")), QStringLiteral("gitignore"),
               QStringLiteral("dot file"));
    checkEqual(spokenFileName(QStringLiteral("HTTPServer.h")), QStringLiteral("HTTP Server"),
               QStringLiteral("acronym file"));

    checkEqual(spokenTitle(QStringLiteral("adhoc-285-announcer-mode-continually")),
               QStringLiteral("announcer mode continually"), QStringLiteral("branch title"));
    checkEqual(spokenTitle(QStringLiteral("Fix the worker's startup memory for the deploy box"), 5),
               QStringLiteral("Fix the worker's startup memory"),
               QStringLiteral("long title keeps a whole phrase"));
    checkEqual(spokenTitle(QStringLiteral("Make a better list of the things"), 5),
               QStringLiteral("Make a better list"), QStringLiteral("title drops a dangling word"));

    checkEqual(spokenDuration(20 * 1000), QStringLiteral("under a minute"),
               QStringLiteral("20 seconds"));
    checkEqual(spokenDuration(12 * 60 * 1000), QStringLiteral("12 minutes"),
               QStringLiteral("12 minutes"));
    checkEqual(spokenDuration(70 * 60 * 1000), QStringLiteral("just over an hour"),
               QStringLiteral("70 minutes"));
    checkEqual(spokenDuration(95 * 60 * 1000), QStringLiteral("an hour and a half"),
               QStringLiteral("95 minutes"));
    checkEqual(spokenDuration(180 * 60 * 1000), QStringLiteral("3 hours"),
               QStringLiteral("3 hours"));

    checkEqual(speakable(QStringLiteral("agent 5, working on x,, lands `an` edit\nin <b>y</b> .")),
               QStringLiteral("agent 5, working on x, lands an edit in b y b."),
               QStringLiteral("speakable strips markup and doubled commas"));
    check(!speakable(QStringLiteral("a\nb")).contains(QLatin1Char('\n')),
          QStringLiteral("a spoken line is one line"));
}

static void testSpokenProse()
{
    checkEqual(spokenProse(QStringLiteral(
                   "## Done\n\nI fixed `desktop/src/MainWindowAgents.cpp` and **entry.py**.\n\n"
                   "```cpp\nint x = 1;\n```\n\n- first point\n- second point\n\n"
                   "| a | b |\n|---|---|\n\nSee [the docs](https://example.com/x) or "
                   "https://example.com/y and/or ask.")),
               QStringLiteral("Done. I fixed Main Window Agents and entry. first point. "
                              "second point. See the docs or a link and/or ask."),
               QStringLiteral("prose drops code, tables and URLs and names files"));
    const QString long_ = QStringLiteral("One sentence here. ").repeated(10)
                          + QStringLiteral("and a tail without an end");
    const QString cut = spokenProse(long_, 100);
    check(cut.size() <= 100 && cut.endsWith(QLatin1Char('.')),
          QStringLiteral("long prose is cut at a full stop: ") + cut);
    const QString words = spokenProse(QStringLiteral("word ").repeated(40), 50);
    check(words.size() <= 51 && words.endsWith(QStringLiteral("word.")),
          QStringLiteral("prose with no full stop is cut at a word: ") + words);
    checkEqual(spokenProse(QStringLiteral("```\nonly code\n```")), QString(),
               QStringLiteral("a reply of only code says nothing"));
}

static void testPlays()
{
    check(playForCommand(QStringLiteral("git commit -m x")) == PlayKind::Commit,
          QStringLiteral("git commit is a commit"));
    check(playForCommand(QStringLiteral("cd x && git push origin y")) == PlayKind::Push,
          QStringLiteral("git push is a push"));
    check(playForCommand(QStringLiteral("python -m pytest worker/tests")) == PlayKind::Test,
          QStringLiteral("pytest is a test run"));
    check(playForCommand(QStringLiteral("./build/forkmesh-sonifier-tests")) == PlayKind::Test,
          QStringLiteral("a -tests binary is a test run"));
    check(playForCommand(QStringLiteral("ninja -C build forkmesh")) == PlayKind::Build,
          QStringLiteral("ninja is a build"));
    check(playForCommand(QStringLiteral("ls -la")) == PlayKind::Shell,
          QStringLiteral("ls is just a command"));

    Play play = playForEvent(toolUse("Edit", R"({"file_path":"/x/desktop/src/MainWindow.cpp"})"));
    check(play.kind == PlayKind::Edit, QStringLiteral("Edit is an edit"));
    checkEqual(play.detail, QStringLiteral("Main Window"), QStringLiteral("the edit names its file"));
    check(playForEvent(toolUse("Bash", R"({"command":"ctest --output-on-failure"})")).kind
              == PlayKind::Test,
          QStringLiteral("Bash running ctest is a test run"));
    check(playForEvent(toolUse("Read", R"({"file_path":"/x/a.txt"})")).kind == PlayKind::Read,
          QStringLiteral("Read is a read"));
    check(playForEvent(toolUse("Grep", R"({"pattern":"x"})")).kind == PlayKind::Search,
          QStringLiteral("Grep is a search"));
    check(playForEvent(toolUse("WebFetch", R"({"url":"https://x"})")).kind == PlayKind::Web,
          QStringLiteral("WebFetch goes to the web"));
    check(playForEvent(toolUse("Task", R"({"prompt":"x"})")).kind == PlayKind::Delegate,
          QStringLiteral("Task is a substitution"));
    check(playForEvent(toolUse("TodoWrite", R"({"todos":[]})")).kind == PlayKind::None,
          QStringLiteral("a todo list is not a play"));
    check(playForEvent(toolResult(true)).kind == PlayKind::Fumble,
          QStringLiteral("an error result is a fumble"));
    check(playForEvent(toolResult(false)).kind == PlayKind::None,
          QStringLiteral("an ordinary result is not news"));
    check(playForEvent(parse(R"({"type":"system","subtype":"init"})")).kind == PlayKind::None,
          QStringLiteral("init is left to the status change"));
}

static void testOpening()
{
    Commentator booth;
    booth.setSeed(1);
    booth.updateAgent(facts(285, "adhoc-285-announcer-mode", "running", 3), 1000);
    booth.updateAgent(facts(280, "blog any network requests", "running", 9), 1000);
    booth.updateAgent(facts(270, "old job", "success", 40), 1000);
    booth.updateAgent(facts(271, "needs an answer", "waiting", 4), 1000);
    // Things that happened before the microphone went live are not replayed.
    booth.handleEvent(285, toolUse("Bash", R"({"command":"git commit -m x"})"), 1000);

    const Call opening = booth.nextCall(2000);
    check(opening.text.startsWith(QStringLiteral("Welcome to ForkMesh live")),
          QStringLiteral("the broadcast opens with a welcome: ") + opening.text);
    check(opening.text.contains(QStringLiteral("2 agents on the field")),
          QStringLiteral("the opening counts the running agents: ") + opening.text);
    check(opening.text.contains(QStringLiteral("agent two eighty on blog any network requests")),
          QStringLiteral("the opening names who is out there: ") + opening.text);
    check(opening.text.contains(QStringLiteral("one more at the sideline")),
          QStringLiteral("the opening mentions the waiting agent: ") + opening.text);
    check(!opening.text.contains(QStringLiteral("old job")),
          QStringLiteral("finished runs are not in the opening"));
    check(booth.nextCall(2500).text.isEmpty(),
          QStringLiteral("nothing to add straight after the opening"));

    Commentator empty;
    const Call alone = empty.nextCall(1000);
    check(alone.text.contains(QStringLiteral("field is empty")),
          QStringLiteral("an empty field is said once: ") + alone.text);
    check(empty.nextCall(60 * 1000).text.isEmpty() && empty.nextCall(600 * 1000).text.isEmpty(),
          QStringLiteral("and then the booth keeps quiet until something moves"));
}

static void testHeadlines()
{
    Commentator booth;
    booth.setSeed(7);
    booth.updateAgent(facts(12, "fix the worker", "queued"), 0);
    booth.nextCall(100); // the opening

    booth.updateAgent(facts(12, "fix the worker", "running"), 1000);
    Call call = booth.nextCall(1200);
    check(call.priority == Priority::Headline && call.sessionId == 12,
          QStringLiteral("a start is a headline"));
    check(call.text.contains(QStringLiteral("gent twelve, working on fix the worker")),
          QStringLiteral("the first mention introduces the agent: ") + call.text);

    // An edit and a commit waiting together: the commit is said first, and
    // saying it retires the smaller talk about the same agent.
    booth.handleEvent(12, toolUse("Edit", R"({"file_path":"a/EntryPoint.py"})"), 2000);
    booth.handleEvent(12, toolUse("Bash", R"({"command":"git commit -m done"})"), 2100);
    call = booth.nextCall(2200);
    check(call.kind == PlayKind::Commit, QStringLiteral("the commit outranks the edit: ") + call.text);
    check(call.text.contains(QStringLiteral("commit")), QStringLiteral("and says so: ") + call.text);
    check(!call.text.contains(QStringLiteral("working on")),
          QStringLiteral("an agent is only introduced once: ") + call.text);
    check(booth.nextCall(2300).text.isEmpty(),
          QStringLiteral("the edit behind the commit is dropped, not read out late"));

    booth.updateAgent(facts(12, "fix the worker", "waiting", 5), 5000);
    call = booth.nextCall(5100);
    check(call.priority == Priority::Headline
              && (call.text.contains(QStringLiteral("coach"))
                  || call.text.contains(QStringLiteral("decision"))),
          QStringLiteral("waiting on you is a headline: ") + call.text);

    booth.updateAgent(facts(12, "fix the worker", "running", 5), 6000);
    call = booth.nextCall(6100);
    check(call.text.contains(QStringLiteral("back")) || call.text.contains(QStringLiteral("again")),
          QStringLiteral("a resume is not called as a first start: ") + call.text);

    booth.updateAgent(facts(12, "fix the worker", "success", 31, 245000), 9000);
    call = booth.nextCall(9100);
    check(call.text.contains(QStringLiteral("31 turns"))
              && call.text.contains(QStringLiteral("245 thousand tokens")),
          QStringLiteral("a finish reads the final score: ") + call.text);

    booth.updateAgent(facts(13, "second job", "running"), 10000);
    booth.nextCall(10100);
    booth.updateAgent(facts(13, "second job", "failed"), 11000);
    call = booth.nextCall(11100);
    check(call.text.contains(QStringLiteral("fail")), QStringLiteral("a failure is called: ") + call.text);

    // Field now empty: said once, then silence.
    call = booth.nextCall(11100 + 30 * 1000);
    check(call.text.contains(QStringLiteral("No agents running")),
          QStringLiteral("the cleared field is called: ") + call.text);
    check(booth.nextCall(11100 + 120 * 1000).text.isEmpty(),
          QStringLiteral("and only once"));
}

static void testStaleness()
{
    Commentator booth;
    booth.setSeed(3);
    booth.updateAgent(facts(5, "job", "running"), 0);
    booth.nextCall(10);

    booth.handleEvent(5, toolUse("Edit", R"({"file_path":"a.cpp"})"), 1000);
    check(booth.pendingCalls() == 1, QStringLiteral("an edit is queued"));
    // The voice was busy for a long time. The edit is no longer worth saying;
    // a finish from the same stretch still is.
    booth.updateAgent(facts(5, "job", "success", 3), 2000);
    const Call call = booth.nextCall(1000 + shelfLifeMs(Priority::Play) + 5000);
    check(call.priority == Priority::Headline,
          QStringLiteral("a stale play is skipped for the headline: ") + call.text);
    check(booth.pendingCalls() == 0, QStringLiteral("and is gone"));
    check(shelfLifeMs(Priority::Headline) > shelfLifeMs(Priority::Highlight)
              && shelfLifeMs(Priority::Highlight) > shelfLifeMs(Priority::Play),
          QStringLiteral("bigger news keeps longer"));
}

static void testThrottling()
{
    Commentator booth;
    booth.setSeed(4);
    booth.updateAgent(facts(8, "job", "running"), 0);
    booth.nextCall(10);

    // A burst of edits is one remark, not twenty.
    for (int i = 0; i < 9; ++i)
        booth.handleEvent(8, toolUse("Edit", R"({"file_path":"a.cpp"})"), 1000 + i * 100);
    check(booth.pendingCalls() == 1, QStringLiteral("a burst of edits queues one call, got %1")
                                         .arg(booth.pendingCalls()));
    Call call = booth.nextCall(2000);
    check(call.kind == PlayKind::Edit, QStringLiteral("and it is the edit"));

    // Reading is only news as a streak.
    for (int i = 0; i < 3; ++i)
        booth.handleEvent(8, toolUse("Read", R"({"file_path":"b.cpp"})"), 3000 + i * 100);
    check(booth.pendingCalls() == 0, QStringLiteral("a few reads right after a call say nothing"));
    booth.handleEvent(8, toolUse("Grep", R"({"pattern":"x"})"), 3400);
    call = booth.nextCall(3500);
    check(call.text.contains(QStringLiteral("4")),
          QStringLiteral("the fourth look-up is called as a streak: ") + call.text);

    // The tenth edit is a tally.
    booth.handleEvent(8, toolUse("Edit", R"({"file_path":"a.cpp"})"), 60 * 1000);
    call = booth.nextCall(60 * 1000 + 100);
    check(call.text.contains(QStringLiteral("10")) && call.priority == Priority::Highlight,
          QStringLiteral("the tenth edit is a highlight: ") + call.text);

    // Milestones: only ones crossed while we were watching.
    Commentator veteran;
    veteran.setSeed(5);
    veteran.updateAgent(facts(9, "long job", "running", 60, 300000), 0);
    veteran.nextCall(10);
    veteran.updateAgent(facts(9, "long job", "running", 61, 310000), 1000);
    check(veteran.pendingCalls() == 0,
          QStringLiteral("turns and tokens spent before the broadcast are not milestones"));
    veteran.updateAgent(facts(9, "long job", "running", 75, 510000), 2000);
    check(veteran.pendingCalls() == 2, QStringLiteral("crossing 75 turns and 500k tokens is two calls"));
    const QString first = veteran.nextCall(2100).text + veteran.nextCall(2200).text;
    check(first.contains(QStringLiteral("75")) && first.contains(QStringLiteral("500 thousand")),
          QStringLiteral("and they say the figures: ") + first);
}

static void testFairnessAndLinks()
{
    Commentator booth;
    booth.setSeed(11);
    booth.updateAgent(facts(1, "one", "running"), 0);
    booth.updateAgent(facts(2, "two", "running"), 0);
    booth.nextCall(10);

    booth.handleEvent(1, toolUse("Edit", R"({"file_path":"a.cpp"})"), 1000);
    Call call = booth.nextCall(1100);
    check(call.sessionId == 1, QStringLiteral("agent one is called"));
    // Both have a play waiting; the one we have not heard about goes first.
    booth.handleEvent(1, toolUse("Bash", R"({"command":"ls"})"), 2000);
    booth.handleEvent(2, toolUse("Edit", R"({"file_path":"b.cpp"})"), 1900);
    call = booth.nextCall(2100);
    check(call.sessionId == 2,
          QStringLiteral("the agent out of the commentary longest goes first: ") + call.text);

    // Over many calls the same sentence never comes twice in a row.
    Commentator chatty;
    chatty.setSeed(12);
    chatty.updateAgent(facts(3, "", "running"), 0);
    chatty.nextCall(10);
    QString previous;
    bool repeated = false;
    for (int i = 0; i < 12; ++i) {
        const qint64 at = 1000 + i * 60 * 1000;
        chatty.handleEvent(3, toolUse("Bash", R"({"command":"ctest"})"), at);
        const QString text = chatty.nextCall(at + 100).text;
        // Strip the way the agent is named; the sentence is what must differ.
        QString sentence = text;
        sentence.remove(QStringLiteral("agent three"), Qt::CaseInsensitive)
            .remove(QStringLiteral("number three"), Qt::CaseInsensitive)
            .remove(QStringLiteral("three"), Qt::CaseInsensitive);
        repeated = repeated || sentence == previous;
        previous = sentence;
    }
    check(!repeated, QStringLiteral("no template is used twice running"));
}

static void testColour()
{
    Commentator booth;
    booth.setSeed(21);
    booth.updateAgent(facts(1, "one", "running", 40, 90000), 1000);
    booth.updateAgent(facts(2, "two", "running", 12, 20000), 1000);
    booth.nextCall(1010);
    check(booth.nextCall(4000).text.isEmpty(), QStringLiteral("no filler straight away"));

    QStringList fillers;
    qint64 now = 1000;
    for (int i = 0; i < 6; ++i) {
        now += 30 * 1000;
        const Call call = booth.nextCall(now);
        check(call.priority == Priority::Colour && !call.text.isEmpty(),
              QStringLiteral("a quiet field gets colour (%1)").arg(i));
        fillers << call.text;
    }
    const QString all = fillers.join(QLatin1Char('|'));
    check(all.contains(QStringLiteral("leads the pack with 40 turns")),
          QStringLiteral("colour includes the standings: ") + all);
    check(all.contains(QStringLiteral("still running")),
          QStringLiteral("colour includes the tally: ") + all);
    check(all.contains(QStringLiteral("quiet")) || all.contains(QStringLiteral("not heard")),
          QStringLiteral("colour notices an agent that has gone silent: ") + all);
    check(fillers.at(0) != fillers.at(1), QStringLiteral("two fillers in a row differ"));

    // Real news always goes ahead of filler.
    booth.handleEvent(2, toolUse("Bash", R"({"command":"git push"})"), now + 40 * 1000);
    check(booth.nextCall(now + 40 * 1000 + 50).kind == PlayKind::Push,
          QStringLiteral("a play beats colour"));

    booth.forgetAgent(1);
    booth.forgetAgent(2);
    check(booth.knownAgents() == 0 && booth.runningAgents() == 0,
          QStringLiteral("forgotten agents are gone"));
}

static void testResample()
{
    // 22.05 kHz mono to 44.1 kHz stereo: twice the frames, both channels equal,
    // and the chunk join interpolates from the previous chunk's last sample.
    const qint16 in[] = {100, 300, -200};
    qint16 carry = 0;
    const QByteArray out = resampleToPlayerFormat(
        QByteArray(reinterpret_cast<const char *>(in), sizeof(in)), 22050, &carry);
    check(out.size() == 3 * 2 * 2 * int(sizeof(qint16)),
          QStringLiteral("2x frames, 2 channels: %1 bytes").arg(out.size()));
    const qint16 *o = reinterpret_cast<const qint16 *>(out.constData());
    check(o[0] == o[1] && o[2] == o[3], QStringLiteral("left equals right"));
    check(o[0] == 0 && o[2] == 50 && o[4] == 100 && o[6] == 200 && o[8] == 300 && o[10] == 50,
          QStringLiteral("linear interpolation between neighbours"));
    check(carry == -200, QStringLiteral("the last sample carries to the next chunk"));
    const qint16 next[] = {0};
    const QByteArray joined = resampleToPlayerFormat(
        QByteArray(reinterpret_cast<const char *>(next), sizeof(next)), 22050, &carry);
    const qint16 *j = reinterpret_cast<const qint16 *>(joined.constData());
    check(j[0] == -200 && j[2] == -100, QStringLiteral("the join continues from the carry"));
    check(resampleToPlayerFormat(QByteArray(), 22050, &carry).isEmpty(),
          QStringLiteral("nothing in, nothing out"));
}

static void testSpeaker()
{
    qputenv("FORKMESH_ANNOUNCER_BACKEND", "none");
    Speaker refused;
    check(!refused.start() && !refused.isRunning(), QStringLiteral("backend none refuses"));

    qputenv("FORKMESH_ANNOUNCER_BACKEND", "silent");
    Speaker speaker;
    check(speaker.start() && speaker.isRunning(), QStringLiteral("the silent backend starts"));
    checkEqual(speaker.backendName(), QStringLiteral("silent"), QStringLiteral("backend name"));
    speaker.say(QStringLiteral("Agent five\ncrosses the line!"));
    check(speaker.recentLines() == QStringList{QStringLiteral("Agent five crosses the line!")},
          QStringLiteral("what was said is kept, as one line"));
    check(!speaker.busy(0), QStringLiteral("the silent backend is never busy"));
    speaker.stop();
    check(!speaker.isRunning(), QStringLiteral("stop stops"));
    qunsetenv("FORKMESH_ANNOUNCER_BACKEND");

    // With no voice installed and none on PATH there is nothing to find.
    qputenv("FORKMESH_ANNOUNCER_DIR", "/nonexistent/forkmesh-announcer");
    qputenv("FORKMESH_ANNOUNCER_PIPER", "/nonexistent/piper");
    check(locateVoice().piper != QStringLiteral("/nonexistent/piper"),
          QStringLiteral("a piper that is not there is not used"));
    qunsetenv("FORKMESH_ANNOUNCER_DIR");
    qunsetenv("FORKMESH_ANNOUNCER_PIPER");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testWords();
    testSpokenProse();
    testPlays();
    testOpening();
    testHeadlines();
    testStaleness();
    testThrottling();
    testFairnessAndLinks();
    testColour();
    testResample();
    testSpeaker();
    if (failures == 0)
        std::cout << "test_agent_announcer: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
