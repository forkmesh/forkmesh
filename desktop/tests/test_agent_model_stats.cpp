#include "../src/AgentModelStats.h"

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QStringList>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    if (condition) {
        std::printf("ok   - %s\n", qPrintable(what));
        return;
    }
    ++failures;
    std::printf("FAIL - %s\n", qPrintable(what));
}

QByteArray commit(const QString &hash, const QString &parents, qint64 secs,
                  const QStringList &trailers = {})
{
    return QStringLiteral("\x1e%1\x1f%2\x1f%3\x1f%4\n")
        .arg(hash, parents, QString::number(secs), trailers.join(QChar(0x1d)))
        .toUtf8();
}

const AgentModelStatsRow *rowFor(const QVector<AgentModelStatsRow> &rows,
                                 const QString &label)
{
    for (const AgentModelStatsRow &row : rows)
        if (row.label == label)
            return &row;
    return nullptr;
}

void testTrailerNames()
{
    check(AgentModelStats::trailerModelName(
              QStringLiteral("Claude Opus 5.5 (1M context) <noreply@anthropic.com>")) ==
              QStringLiteral("Opus 5.5"),
          QStringLiteral("context note and vendor prefix are dropped"));
    check(AgentModelStats::trailerModelName(
              QStringLiteral("Claude Sonnet 5 <noreply@anthropic.com>")) ==
              QStringLiteral("Sonnet 5"),
          QStringLiteral("plain Claude trailer names the model"));
    check(AgentModelStats::trailerModelName(
              QStringLiteral("Cursor <cursoragent@cursor.com>")) ==
              QStringLiteral("Cursor"),
          QStringLiteral("other agent vendors are kept"));
    check(AgentModelStats::trailerModelName(
              QStringLiteral("Jane Doe <jane@example.com>")).isEmpty(),
          QStringLiteral("human co-authors are not models"));
    check(AgentModelStats::trailerModelName(
              QStringLiteral("mainnode <F4rlozoS0ls2>")).isEmpty(),
          QStringLiteral("node keys are not models"));
}

void testGitHistory()
{
    QByteArray log;
    log += commit(QStringLiteral("m3"), QStringLiteral("d1 h1"), 400);
    log += commit(QStringLiteral("h1"), QStringLiteral("m2"), 390,
                  {QStringLiteral("Co-Authored-By: Jane <jane@example.com>")});
    log += commit(QStringLiteral("d1"), QStringLiteral("m2"), 380,
                  {QStringLiteral("Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>")});
    log += commit(QStringLiteral("m2"), QStringLiteral("m1 b3"), 300);
    log += commit(QStringLiteral("b3"), QStringLiteral("b2"), 290,
                  {QStringLiteral("Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>")});
    log += commit(QStringLiteral("b2"), QStringLiteral("b1 m1"), 280);
    log += commit(QStringLiteral("b1"), QStringLiteral("root"), 270,
                  {QStringLiteral("Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"),
                   QStringLiteral("Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>")});
    log += commit(QStringLiteral("m1"), QStringLiteral("root a1"), 200);
    log += commit(QStringLiteral("a1"), QStringLiteral("root"), 190,
                  {QStringLiteral("Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"),
                   QStringLiteral("ForkMesh-Agent: claude-code/claude-sonnet-5")});
    log += commit(QStringLiteral("root"), QString(), 100);

    const AgentModelGitHistory history = AgentModelStats::parseGitHistory(log);
    const auto credit = [&](const QString &key) {
        return history.models.value(key);
    };
    check(history.mergesScanned == 4,
          QStringLiteral("three mainline merges and one merge of main into a branch"));
    check(history.mergesCredited == 3,
          QStringLiteral("two agent merges and one direct agent commit are credited"));
    check(credit(QStringLiteral("name:opus 5")).merges == 1,
          QStringLiteral("the branch's dominant model gets the merge"));
    check(credit(QStringLiteral("name:opus 5")).commits == 2,
          QStringLiteral("every co-authored commit counts"));
    check(credit(QStringLiteral("name:sonnet 5")).merges == 0 &&
              credit(QStringLiteral("name:sonnet 5")).commits == 1,
          QStringLiteral("a minority co-author gets commits, not the merge"));
    check(credit(AgentModelStats::rowKey(QStringLiteral("claude-code"),
                                         QStringLiteral("claude-sonnet-5")))
                  .merges == 1,
          QStringLiteral("the ForkMesh-Agent trailer names the exact model"));
    check(credit(QStringLiteral("name:haiku 4.5")).merges == 1 &&
              credit(QStringLiteral("name:haiku 4.5")).lastMergeMs == 380000,
          QStringLiteral("an agent commit landed straight on the mainline counts"));
    check(!history.models.contains(QStringLiteral("name:jane")),
          QStringLiteral("a human-only merge credits nobody"));
}

void testImportedHistory()
{
    QByteArray log;
    log += commit(QStringLiteral("i1"), QStringLiteral("r0 s2"), 500);
    log += commit(QStringLiteral("s2"), QStringLiteral("s1 x1"), 490);
    log += commit(QStringLiteral("x1"), QStringLiteral("s0"), 480,
                  {QStringLiteral("Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>")});
    log += commit(QStringLiteral("s1"), QStringLiteral("s0"), 470,
                  {QStringLiteral("Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>")});
    log += commit(QStringLiteral("s0"), QString(), 460,
                  {QStringLiteral("Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>")});
    log += commit(QStringLiteral("r0"), QString(), 100);
    const AgentModelGitHistory history = AgentModelStats::parseGitHistory(log);
    check(history.mergesScanned == 2 && history.mergesCredited == 2,
          QStringLiteral("merges inside merged-in history are credited too"));
    check(history.models.value(QStringLiteral("name:opus 4.8")).merges == 1 &&
              history.models.value(QStringLiteral("name:fable 5")).merges == 1,
          QStringLiteral("the import goes to its mainline's model, its own merge to "
                         "the branch's model"));
}

void testBuild()
{
    const QList<AgentModelCatalogEntry> catalog = {
        {QStringLiteral("claude-code"), QStringLiteral("claude-opus-5"),
         QStringLiteral("Opus 5"), QStringLiteral("Claude Code")},
        {QStringLiteral("claude-code"), QStringLiteral("claude-haiku-4-5"),
         QStringLiteral("Haiku 4.5"), QStringLiteral("Claude Code")},
        {QStringLiteral("codex"), QStringLiteral("gpt-5.5"),
         QStringLiteral("GPT-5.5"), QStringLiteral("Codex")},
    };
    QList<AgentSession> sessions;
    AgentSession merged;
    merged.provider = QStringLiteral("claude-code");
    merged.model = QStringLiteral("Claude-Opus-5");
    merged.status = AgentStatus::Success;
    merged.merged = true;
    merged.prNumber = 7;
    merged.costUsd = 2.0;
    merged.numTurns = 4;
    merged.startedAtMs = 1000;
    merged.finishedAtMs = 61000;
    sessions << merged;
    AgentSession failed = merged;
    failed.status = AgentStatus::Failed;
    failed.merged = false;
    failed.prNumber = 0;
    failed.costUsd = 1.0;
    failed.numTurns = 2;
    sessions << failed;
    AgentSession codex;
    codex.provider = QStringLiteral("codex");
    codex.model = QStringLiteral("gpt-5.5");
    codex.status = AgentStatus::Running;
    codex.promptTokens = 1000000;
    codex.completionTokens = 100000;
    sessions << codex;

    QHash<QString, AgentModelOutcome> retired;
    retired.insert(AgentStore::modelOutcomeKey(QStringLiteral("claude-code"),
                                               QStringLiteral("claude-opus-5")),
                   AgentModelOutcome{3, 2});

    AgentModelGitHistory git;
    git.models.insert(QStringLiteral("name:opus 5"),
                      AgentModelGitCredit{QStringLiteral("name:opus 5"), QString(),
                                          QString(), QStringLiteral("Opus 5"), 40, 90, 5});
    git.models.insert(QStringLiteral("name:cursor"),
                      AgentModelGitCredit{QStringLiteral("name:cursor"), QString(),
                                          QString(), QStringLiteral("Cursor"), 1, 10, 0});

    AgentToolModelStats tools;
    tools.provider = QStringLiteral("claude-code");
    tools.model = QStringLiteral("claude-opus-5");
    tools.calls = 10;
    tools.errors = 1;
    tools.inputTokens = 300;
    tools.resultTokens = 700;

    const QVector<AgentModelStatsRow> rows =
        AgentModelStats::build(catalog, sessions, retired, git, {tools}, 100000);
    const AgentModelStatsRow *opus = rowFor(rows, QStringLiteral("Opus 5"));
    check(opus && opus->catalogIndex == 0, QStringLiteral("catalog models keep their row"));
    if (opus) {
        check(opus->runs == 5 && opus->retiredRuns == 3,
              QStringLiteral("live and retired runs add up"));
        check(opus->succeeded == 1 && opus->failed == 1,
              QStringLiteral("success and failure are counted"));
        check(qFuzzyCompare(opus->successRate(), 0.5),
              QStringLiteral("success rate is over finished runs"));
        check(opus->merged == 3, QStringLiteral("merged counts live and retired runs"));
        check(opus->gitMerges == 40 && opus->gitCommits == 90,
              QStringLiteral("git backfill lands on the matching label"));
        check(qFuzzyCompare(opus->costUsd(), 3.0) && qFuzzyCompare(opus->costPerRun(), 1.5),
              QStringLiteral("reported cost is summed per stored run"));
        check(qFuzzyCompare(opus->costPerMerge(), 1.0), QStringLiteral("cost per merge"));
        check(opus->pullRequests == 1, QStringLiteral("pull requests are counted"));
        check(qFuzzyCompare(opus->turnsPerRun(), 3.0), QStringLiteral("turns per run"));
        check(qFuzzyCompare(opus->averageRunMs(), 60000.0), QStringLiteral("average run time"));
        check(qFuzzyCompare(opus->tokensPerToolCall(), 100.0) &&
                  qFuzzyCompare(opus->toolErrorRate(), 0.1),
              QStringLiteral("tool efficiency comes from the ledger"));
    }
    const AgentModelStatsRow *haiku = rowFor(rows, QStringLiteral("Haiku 4.5"));
    check(haiku && !haiku->hasActivity(), QStringLiteral("unused catalog models are listed"));
    const AgentModelStatsRow *gpt = rowFor(rows, QStringLiteral("GPT-5.5"));
    check(gpt && gpt->active == 1, QStringLiteral("running runs are live"));
    check(gpt && qFuzzyCompare(gpt->estimatedUsd, 2.25),
          QStringLiteral("cost is estimated from tokens when none is reported"));
    const AgentModelStatsRow *cursor = rowFor(rows, QStringLiteral("Cursor"));
    check(cursor && cursor->gitMerges == 1 && cursor->catalogIndex < 0,
          QStringLiteral("git-only models get their own row"));
}

void testRepositoryHistory()
{
    const QString repo = QStringLiteral(FORKMESH_SOURCE_ROOT);
    if (!QFile::exists(repo + QStringLiteral("/.git")))
        return;
    QProcess git;
    git.setWorkingDirectory(repo);
    git.start(QStringLiteral("git"), AgentModelStats::gitLogArguments(QStringLiteral("HEAD")));
    if (!git.waitForFinished(60000) || git.exitCode() != 0)
        return;
    const AgentModelGitHistory history = AgentModelStats::parseGitHistory(git.readAllStandardOutput());
    check(history.commitsScanned > 0, QStringLiteral("this repository's log parses"));
}

} // namespace

int main()
{
    testTrailerNames();
    testGitHistory();
    testImportedHistory();
    testBuild();
    testRepositoryHistory();
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
