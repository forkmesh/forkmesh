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
    using forkmesh::agentwt::adHocBranchName;
    using forkmesh::agentwt::parseAgentTitle;

    const QString request = agentTitleRequest(QStringLiteral(
        "  the titles for agents are not looking great  "));
    expectEq(QString::number(request.contains(QStringLiteral(
                 "<description>the titles for agents are not looking great</description>"))),
             QStringLiteral("1"), "title request fences the trimmed prompt");
    expectEq(QString::number(request.contains(QStringLiteral("<title>")) &&
                             request.contains(QStringLiteral("<branch>"))),
             QStringLiteral("1"), "title request asks for a tagged title and branch");
    expectEq(QString::number(
                 agentTitleRequest(QString(7000, QLatin1Char('x')))
                     .count(QLatin1Char('x')) <= 4000 + 8),
             QStringLiteral("1"), "title request caps a long prompt");

    const auto tagged = parseAgentTitle(
        "<title>Safari reports export button</title>\n"
        "<branch>safari-reports-export</branch>\n");
    expectEq(tagged.title, QStringLiteral("Safari reports export button"),
             "tagged title");
    expectEq(tagged.branch, QStringLiteral("safari-reports-export"),
             "tagged branch");
    expectEq(parseAgentTitle("<title>Slow backup</title>"
                             "<branch>claude/slow-backup</branch>")
                 .branch,
             QStringLiteral("slow-backup"), "claude/ prefix dropped");
    expectEq(parseAgentTitle("<title>Mobile login: button bug!</title>"
                             "<branch>Mobile Login</branch>")
                 .branch,
             QStringLiteral("mobile-login-button-bug"),
             "invalid branch rebuilt from the title");
    expectEq(parseAgentTitle("<title>Mobile login</title>").branch,
             QStringLiteral("mobile-login"), "missing branch rebuilt from the title");
    expectEq(parseAgentTitle("<title>Écran d’accueil</title>").branch,
             QStringLiteral("cran-d-accueil"), "non-ASCII letters dropped from the slug");
    expectEq(parseAgentTitle("Sure! Here you go.").title, QString(),
             "reply without tags has no title");
    expectEq(parseAgentTitle("<branch>fix-it</branch>").title, QString(),
             "branch alone has no title");
    expectEq(parseAgentTitle(QByteArray("<title>") + QByteArray(201, 'a') +
                             QByteArray("</title>"))
                 .title,
             QString(), "an over-long title is refused");
    expectEq(adHocBranchName(42, QStringLiteral("slow-backup")),
             QStringLiteral("42-slow-backup"), "branch name keeps the session id");
    expectEq(adHocBranchName(42, QString()), QStringLiteral("42"),
             "empty branch names by id");

    using forkmesh::agentwt::isPromptNamerLabel;
    using forkmesh::agentwt::promptNamerLabel;
    expectEq(promptNamerLabel(QStringLiteral("Claude Code"), QStringLiteral("opus")),
             QStringLiteral("Claude Code namer (opus)"), "namer label names the model");
    expectEq(promptNamerLabel(QStringLiteral("Codex"), QString()),
             QStringLiteral("Codex namer"), "namer label without a model");
    expectEq(QString::number(isPromptNamerLabel(QStringLiteral("Claude Code namer (opus)"),
                                                QStringLiteral("Claude Code"))),
             QStringLiteral("1"), "namer label recognised");
    expectEq(QString::number(isPromptNamerLabel(QStringLiteral("Claude Code session"),
                                                QStringLiteral("Claude Code"))),
             QStringLiteral("0"), "session title is not a namer label");

    expectEq(adHocName(304, QStringLiteral(
                            "Have the agent determine the title to display")),
             QStringLiteral("304-agent-determine-title-display"),
             "branch/worktree name is id plus slug");
    expectEq(adHocName(7, QStringLiteral("???")), QStringLiteral("7"),
             "slug-less title still names by id");

    using forkmesh::agentwt::addScript;
    expectEq(addScript(QStringLiteral("/w/7-fix"), QStringLiteral("agent/7-fix"),
                       QStringLiteral("abc123"), false),
             QStringLiteral("if git rev-parse --verify --quiet "
                            "'refs/heads/agent/7-fix' >/dev/null; "
                            "then git worktree add '/w/7-fix' 'agent/7-fix'; "
                            "else git worktree add -b "
                            "'agent/7-fix' '/w/7-fix' 'abc123'; fi"),
             "worktree script reuses a branch a failed attempt left, else creates it");
    expectEq(addScript(QStringLiteral("/w/8"),
                       QStringLiteral("x';touch${IFS}/tmp/pwned;'"),
                       QStringLiteral("main';id;'"), false),
             QStringLiteral("if git rev-parse --verify --quiet "
                            "'refs/heads/x'\\'';touch${IFS}/tmp/pwned;'\\''' >/dev/null; "
                            "then git worktree add '/w/8' "
                            "'x'\\'';touch${IFS}/tmp/pwned;'\\'''; "
                            "else git worktree add -b "
                            "'x'\\'';touch${IFS}/tmp/pwned;'\\''' '/w/8' "
                            "'main'\\'';id;'\\'''; fi"),
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
