#include "PullReviewAll.h"

#include <QCoreApplication>

#include <iostream>

using namespace forkmesh::pullreviewall;
namespace pullmergeall = forkmesh::pullmergeall;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static pullmergeall::Target target(int number, const QString &branch)
{
    pullmergeall::Target t;
    t.number = number;
    t.title = QStringLiteral("Change %1").arg(number);
    t.authorName = QStringLiteral("Sample Author");
    t.branch = branch;
    t.base = QStringLiteral("main");
    return t;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString prompt = agentPrompt(
        QStringLiteral("acme"), QStringLiteral("app"), QStringLiteral("/work/app"),
        {target(3, QStringLiteral("feature/a")), target(9, QStringLiteral("pr/9"))},
        QStringLiteral("/data/pull-reviews/run.json"));
    check(prompt.section(QLatin1Char('\n'), 0, 0).contains(QStringLiteral("acme/app")),
          "the prompt's first line names the repository");
    check(prompt.contains(QStringLiteral("security")) &&
              prompt.contains(QStringLiteral("stability")) &&
              prompt.contains(QStringLiteral("maintainability")),
          "the prompt asks for security, stability and maintainability review");
    check(prompt.contains(QStringLiteral("- #3 \"Change 3\": branch `feature/a` into `main` (by Sample Author)")),
          "the prompt lists each pull request with branch, base and author");
    check(prompt.contains(QStringLiteral("`/data/pull-reviews/run.json`")),
          "the prompt names the report file");
    check(prompt.contains(QStringLiteral("untrusted input")), "pull content is marked untrusted");
    check(prompt.contains(QStringLiteral("Never")) && prompt.contains(QStringLiteral("push")),
          "the prompt forbids changing the checkout");
    check(prompt.contains(QStringLiteral("Do not post, sign or approve anything yourself")),
          "the agent leaves posting to ForkMesh");
    check(runTitle(1) == QStringLiteral("Review 1 open pull request with AI") &&
              runTitle(4) == QStringLiteral("Review 4 open pull requests with AI"),
          "run titles count the pull requests");

    const QByteArray json = R"({
        "model": "claude-opus-5-5",
        "reviews": [
          {"number": 3, "summary": "Adds a login page.", "findings": []},
          {"number": 9, "summary": "Adds an export.", "findings": [
            {"path": "src/export.cpp", "line_start": 12, "line_end": 12,
             "severity": "bug", "comment": "The file handle leaks.",
             "original": "  open(path);", "fix": "  auto f = open(path);"},
            {"path": "src/export.cpp", "line_start": 40, "line_end": 41,
             "severity": "warning", "comment": "No error is reported here.",
             "original": null, "fix": null},
            {"path": "", "line_start": 1, "comment": "dropped"}
          ]},
          {"number": 0, "summary": "dropped"},
          {"number": 9, "summary": "Adds an export, reviewed again.", "findings": [
            {"path": "src/export.cpp", "line_start": 12, "line_end": 12,
             "severity": "bug", "comment": "The file handle leaks.",
             "original": "  open(path);", "fix": "  auto f = open(path);"},
            {"path": "src/export.cpp", "line_start": 40, "line_end": 41,
             "severity": "odd", "comment": "No error is reported here.",
             "original": null, "fix": null}
          ]}
        ]})";
    Report report;
    QString error;
    check(parseReport(json, &report, &error), "a well-formed report parses");
    check(report.model == QStringLiteral("claude-opus-5-5"), "the reported model is kept");
    check(report.reviews.size() == 2, "a pull request reviewed twice keeps one review");
    const Review approved = report.reviews.value(0);
    const Review changes = report.reviews.value(1);
    check(approved.number == 3 && approved.findings.isEmpty(), "a clean review has no findings");
    check(changes.number == 9 && changes.summary.contains(QStringLiteral("again")),
          "the later review of the same pull request wins");
    check(changes.findings.size() == 2, "findings without a path are dropped");
    check(!changes.findings.value(0).suggestionPatch.isEmpty(),
          "a finding with original and fix becomes a one-click suggestion");
    check(changes.findings.value(1).suggestionPatch.isEmpty(),
          "a finding without a fix stays a conversation");
    check(changes.findings.value(1).severity == QStringLiteral("warning"),
          "an unknown severity becomes a warning");
    check(reviewState(approved) == QStringLiteral("approved"),
          "no findings approves the pull request");
    check(reviewState(changes) == QStringLiteral("changes_requested"),
          "any finding requests changes");

    check(!parseReport("not json", &report, &error) && !error.isEmpty(),
          "a broken report is refused with a reason");
    check(!parseReport(R"({"model": "x"})", &report, &error),
          "a report without reviews is refused");

    check(reviewerLabel(QStringLiteral("Claude Code"), QStringLiteral("Opus 5.5"),
                        QStringLiteral("claude-opus-5-5")) ==
              QStringLiteral("Claude Code (Opus 5.5)"),
          "the session's own model wins over the agent's claim");
    check(reviewerLabel(QStringLiteral("Codex"), QString(), QStringLiteral("gpt-5")) ==
              QStringLiteral("Codex (gpt-5, as the agent reported it)"),
          "an unnamed session model falls back to what the agent reported");
    check(reviewerLabel(QStringLiteral("Codex"), QString(), QString()) ==
              QStringLiteral("Codex (its default model)"),
          "with no model named the label says so");

    const QString approvedBody =
        summaryBody(approved, {}, QStringLiteral("Claude Code (Opus 5.5)"));
    check(approvedBody.contains(QStringLiteral("approved")) &&
              approvedBody.contains(QStringLiteral("Adds a login page.")),
          "an approval summary carries the verdict and the agent's summary");
    check(approvedBody.trimmed().endsWith(
              QStringLiteral("Reviewed for security, stability and maintainability by "
                             "Claude Code (Opus 5.5).")),
          "the summary ends with the model that reviewed");

    const QString changesBody = summaryBody(
        changes, {QStringLiteral("t1"), QStringLiteral("t2")}, QStringLiteral("Codex (gpt-5)"));
    check(changesBody.contains(QStringLiteral("changes requested")) &&
              changesBody.contains(QStringLiteral("2 finding(s), 1 with a one-click fix")),
          "a changes summary counts findings and fixes");
    check(changesBody.contains(QStringLiteral("[src/export.cpp:12](forkmesh-pull-thread:t1)")),
          "each finding links to its thread");
    check(changesBody.contains(QStringLiteral("[src/export.cpp:40-41](forkmesh-pull-thread:t2)")),
          "a multi-line finding links with its line range");
    check(changesBody.contains(QStringLiteral("[Apply fix](applyfix:t1)")) &&
              !changesBody.contains(QStringLiteral("applyfix:t2")),
          "only findings with a suggestion get an Apply fix link");
    check(!summaryBody(changes, {}, QStringLiteral("x"))
               .contains(QStringLiteral("forkmesh-pull-thread:")),
          "a finding whose thread was not posted is listed without a link");
    Review hostile = changes;
    hostile.summary = QStringLiteral("[click](javascript:alert(1)) <script>");
    const QString hostileBody = summaryBody(hostile, {}, QStringLiteral("x"));
    check(!hostileBody.contains(QStringLiteral("[click](")) &&
              hostileBody.contains(QStringLiteral("\\[click\\]")) &&
              hostileBody.contains(QStringLiteral("\\<script\\>")),
          "the agent's summary is escaped, so it cannot inject links or tags");

    QHash<int, Run> runs;
    Run run;
    run.owner = QStringLiteral("acme");
    run.name = QStringLiteral("app");
    run.reportPath = QStringLiteral("/data/pull-reviews/run.json");
    run.targets = {target(3, QStringLiteral("feature/a"))};
    runs.insert(12, run);
    Run noReport = run;
    noReport.reportPath.clear();
    runs.insert(13, noReport);
    const QHash<int, Run> decoded = decodeRuns(encodeRuns(runs));
    check(decoded.size() == 1 && decoded.value(12).reportPath == run.reportPath &&
              decoded.value(12).targets.size() == 1 &&
              decoded.value(12).targets.value(0).branch == QStringLiteral("feature/a"),
          "saved runs round-trip and a run without a report is dropped");

    if (failures)
        std::cerr << failures << " check(s) failed\n";
    else
        std::cout << "pull review all tests passed\n";
    return failures ? 1 : 0;
}
