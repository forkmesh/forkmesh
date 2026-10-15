// Ad-hoc agent runs are titled and named from their first prompt: the Agents
// list shows a short title instead of the raw typed prompt, and the branch and
// worktree share one `<id>-<slug>` name with no `agent/adhoc-` prefix.

#include "AgentWorktree.h"

#include <cstdio>
#include <QTemporaryDir>

namespace {

int failures = 0;

void expectEq(const QString &actual, const QString &expected, const char *what)
{
    if (actual == expected)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", what,
                 qPrintable(expected), qPrintable(actual));
}

} // namespace

int main()
{
    using forkmesh::agentwt::adHocName;
    using forkmesh::agentwt::promptTitle;

    expectEq(promptTitle(QStringLiteral(
                 "have the agent determine the title to display and the branch "
                 "/ worktree names based on the first prompt please show the "
                 "agent title in the list of agents in desktop")),
             QStringLiteral("Have the agent determine the title to display"),
             "long run-on prompt is cut to its first clause");
    expectEq(promptTitle(QStringLiteral(
                 "remove the forkmesh version from the footer on the dashboard "
                 "pages plesae so its just no footer")),
             QStringLiteral("Remove the forkmesh version from the footer"),
             "trailing preposition dropped at the word cap");
    expectEq(promptTitle(QStringLiteral(
                 "Can you please fix the login crash on Windows? It happens "
                 "every time.")),
             QStringLiteral("Fix the login crash on Windows"),
             "polite opener and later sentences dropped");
    expectEq(promptTitle(QStringLiteral("\n\n## Add dark mode\n\nDetails here")),
             QStringLiteral("Add dark mode"),
             "markdown heading on the first non-empty line");
    expectEq(promptTitle(QStringLiteral("fix it")), QStringLiteral("Fix it"),
             "short prompt kept whole");
    expectEq(promptTitle(QStringLiteral("   \n  ")), QString(),
             "blank prompt has no title");

    using forkmesh::agentwt::agentTitleRequest;
    using forkmesh::agentwt::parseAgentTitle;

    const QString request = agentTitleRequest(QStringLiteral(
        "  the titles for agents are not looking great  "));
    expectEq(QString::number(request.contains(QStringLiteral(
                 "<<<\nthe titles for agents are not looking great\n>>>"))),
             QStringLiteral("1"), "title request fences the trimmed prompt");
    expectEq(QString::number(request.contains(QStringLiteral("{\"title\""))),
             QStringLiteral("1"), "title request asks for a JSON title");
    expectEq(QString::number(
                 agentTitleRequest(QString(7000, QLatin1Char('x')))
                     .count(QLatin1Char('x')) <= 6000 + 8),
             QStringLiteral("1"), "title request caps a long prompt");

    expectEq(parseAgentTitle(
                 "```json\n{\"title\": \"Improve descriptive titles for "
                 "starting agents\"}\n```\n"),
             QStringLiteral("Improve descriptive titles for starting agents"),
             "fenced JSON reply");
    expectEq(parseAgentTitle("{\"title\":\"  \\\"fix the login crash on "
                             "Windows.\\\"  \"}"),
             QStringLiteral("Fix the login crash on Windows"),
             "quotes, trailing period and case cleaned");
    expectEq(parseAgentTitle("Sure! Here you go."), QString(),
             "reply without JSON has no title");
    expectEq(parseAgentTitle("{\"name\":\"Fix it\"}"), QString(),
             "JSON without a title field has no title");
    expectEq(parseAgentTitle("{\"title\":\"one two three four five six "
                             "seven eight nine ten eleven twelve thirteen\"}"),
             QString(), "a paragraph is not a title");
    expectEq(parseAgentTitle("{\"title\":\"Rework the extraordinarily "
                             "complicated synchronization pipeline "
                             "configuration handling\"}"),
             QStringLiteral("Rework the extraordinarily complicated "
                            "synchronization pipeline"),
             "long title cut at a word boundary");

    expectEq(adHocName(304, QStringLiteral(
                            "Have the agent determine the title to display")),
             QStringLiteral("304-agent-determine-title-display"),
             "branch/worktree name is id plus slug");
    expectEq(adHocName(7, QStringLiteral("???")), QStringLiteral("7"),
             "slug-less title still names by id");

    using forkmesh::agentwt::addScript;
    expectEq(addScript(QStringLiteral("/w/7-fix"), QStringLiteral("agent/7-fix"),
                       QStringLiteral("abc123"), false),
             QStringLiteral("git worktree add -b "
                            "'agent/7-fix' '/w/7-fix' 'abc123'"),
             "worktree script quotes branch, path and base");
    expectEq(addScript(QStringLiteral("/w/8"),
                       QStringLiteral("x';touch${IFS}/tmp/pwned;'"),
                       QStringLiteral("main';id;'"), false),
             QStringLiteral("git worktree add -b "
                            "'x'\\'';touch${IFS}/tmp/pwned;'\\''' '/w/8' "
                            "'main'\\'';id;'\\'''"),
             "a branch name from a pull request cannot break out of its quotes");
    expectEq(addScript(QStringLiteral("/w/9"),
                       QStringLiteral("feat';reboot;'"), QString(), true),
             QStringLiteral("git worktree add '/w/9' "
                            "'feat'\\'';reboot;'\\'''"),
             "a resumed run's branch is quoted too");

    QTemporaryDir workspaces;
    auto workspace = [&workspaces](const QString &name, const QString &branch) {
        const QString path = workspaces.filePath(name);
        QDir().mkpath(path);
        const QString gitDir = workspaces.filePath(QStringLiteral("git-") + name);
        QDir().mkpath(gitDir);
        QFile pointer(path + QStringLiteral("/.git"));
        pointer.open(QIODevice::WriteOnly);
        pointer.write("gitdir: " + gitDir.toUtf8() + "\n");
        pointer.close();
        QFile head(gitDir + QStringLiteral("/HEAD"));
        head.open(QIODevice::WriteOnly);
        head.write("ref: refs/heads/" + branch.toUtf8() + "\n");
        return path;
    };
    const QString original = workspace(QStringLiteral("2091-original"), QStringLiteral("2091-original"));
    workspace(QStringLiteral("20910-unrelated"), QStringLiteral("20910-unrelated"));
    const auto recovered = forkmesh::agentwt::existingWorkspace(workspaces.path(), 2091);
    expectEq(recovered.path, original, "recover the existing workspace after saved names drift");
    expectEq(recovered.branch, QStringLiteral("2091-original"), "recover its actual branch");
    workspace(QStringLiteral("2091-other"), QStringLiteral("2091-other"));
    expectEq(forkmesh::agentwt::existingWorkspace(workspaces.path(), 2091).path,
             QString(), "ambiguous workspaces are never guessed");

    if (failures == 0)
        std::printf("agent prompt title tests passed\n");
    return failures == 0 ? 0 : 1;
}
