#include "ClaudeSessionScan.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <iostream>

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QByteArray line(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

static void writeTranscript(const QString &projects, const QString &uuid,
                            const QStringList &cwds,
                            const QString &branch = QStringLiteral("main"))
{
    const QString dir = projects + QStringLiteral("/p-") + uuid;
    QDir().mkpath(dir);
    QFile f(dir + QLatin1Char('/') + uuid + QStringLiteral(".jsonl"));
    f.open(QIODevice::WriteOnly);
    f.write(line({{QStringLiteral("type"), QStringLiteral("queue-operation")},
                  {QStringLiteral("sessionId"), uuid}}));
    for (const QString &cwd : cwds)
        f.write(line({{QStringLiteral("type"), QStringLiteral("user")},
                      {QStringLiteral("cwd"), cwd},
                      {QStringLiteral("gitBranch"), branch},
                      {QStringLiteral("sessionId"), uuid}}));
}

static QStringList uuids(const QList<ExternalClaudeSession> &sessions)
{
    QStringList out;
    for (const ExternalClaudeSession &s : sessions)
        out << s.uuid;
    out.sort();
    return out;
}

static const ExternalClaudeSession *find(const QList<ExternalClaudeSession> &sessions,
                                         const QString &uuid)
{
    for (const ExternalClaudeSession &s : sessions)
        if (s.uuid == uuid)
            return &s;
    return nullptr;
}

int main(int argc, char **argv)
{
    QTemporaryDir home;
    if (!home.isValid()) {
        std::cerr << "FAIL: temp dir\n";
        return 1;
    }
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_CACHE_HOME", (home.path() + QStringLiteral("/cache")).toUtf8());
    qunsetenv("FORKMESH_HOME");
    qunsetenv("FORKMESH_PROJECTS_DIR");
    QCoreApplication app(argc, argv);

    const QString projects = home.path() + QStringLiteral("/.claude/projects");
    const QString repo = home.path() + QStringLiteral("/repo");
    const QString worktree = repo + QStringLiteral("/.worktrees/7-task");
    QDir().mkpath(worktree + QStringLiteral("/desktop/build"));
    QDir().mkpath(repo + QStringLiteral("/app"));

    writeTranscript(projects, QStringLiteral("agent-cd"),
                    {worktree, worktree, worktree + QStringLiteral("/desktop/build")});
    writeTranscript(projects, QStringLiteral("agent-plain"), {worktree});
    writeTranscript(projects, QStringLiteral("user-root"),
                    {repo, repo + QStringLiteral("/app")});
    writeTranscript(projects, QStringLiteral("user-sub"),
                    {repo + QStringLiteral("/app")});
    writeTranscript(projects, QStringLiteral("gone"),
                    {repo + QStringLiteral("/.worktrees/deleted")});
    writeTranscript(projects, QStringLiteral("other-repo"),
                    {home.path() + QStringLiteral("/elsewhere")});

    const qint64 window = 90 * 1000;

    const QList<ExternalClaudeSession> agentRunning =
        ClaudeSessionScan::scan(repo, QSet<QString>{worktree}, window);
    check(uuids(agentRunning) ==
              QStringList({QStringLiteral("user-root"), QStringLiteral("user-sub")}),
          "an agent that cd'd into its worktree's subfolder stays ForkMesh's own");
    const ExternalClaudeSession *root = find(agentRunning, QStringLiteral("user-root"));
    check(root && root->cwd == repo,
          "a session keeps the folder it was launched in, not where it cd'd to");

    const QList<ExternalClaudeSession> noAgents =
        ClaudeSessionScan::scan(repo, QSet<QString>(), window);
    check(uuids(noAgents) == QStringList({QStringLiteral("agent-cd"),
                                          QStringLiteral("agent-plain"),
                                          QStringLiteral("user-root"),
                                          QStringLiteral("user-sub")}),
          "with no ForkMesh agents every live session in the repo is listed");

    const QList<ExternalClaudeSession> directCheckout =
        ClaudeSessionScan::scan(repo, QSet<QString>{repo}, window);
    check(uuids(directCheckout) == QStringList({QStringLiteral("agent-cd"),
                                                QStringLiteral("agent-plain"),
                                                QStringLiteral("user-sub")}),
          "a no-worktree agent claims only the checkout root, not its subfolders");

    const ExternalClaudeSession *named = find(noAgents, QStringLiteral("agent-plain"));
    check(named && named->gitBranch == QStringLiteral("main"),
          "a session on a branch keeps its branch name");

    writeTranscript(projects, QStringLiteral("detached"), {repo},
                    QStringLiteral("HEAD"));
    const QList<ExternalClaudeSession> withDetached =
        ClaudeSessionScan::scan(repo, QSet<QString>(), window);
    const ExternalClaudeSession *detached =
        find(withDetached, QStringLiteral("detached"));
    check(detached && detached->gitBranch.isEmpty(),
          "a detached checkout names no branch, so refs/heads/HEAD is never probed");

    if (failures == 0)
        std::cout << "claude session scan tests passed\n";
    return failures == 0 ? 0 : 1;
}
