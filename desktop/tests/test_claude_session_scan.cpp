#include "ClaudeSessionImport.h"
#include "ClaudeSessionScan.h"

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
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

static void writeLines(const QString &path, const QList<QJsonObject> &objects)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    for (const QJsonObject &o : objects)
        f.write(line(o));
}

static QJsonObject userLine(const QString &cwd, const QJsonValue &content,
                            const QString &stamp = QStringLiteral("2026-10-01T08:14:20.456Z"))
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("user")},
                       {QStringLiteral("cwd"), cwd},
                       {QStringLiteral("gitBranch"), QStringLiteral("main")},
                       {QStringLiteral("timestamp"), stamp},
                       {QStringLiteral("entrypoint"), QStringLiteral("cli")},
                       {QStringLiteral("message"),
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), content}}}};
}

static QJsonObject assistantLine(const QString &text)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("timestamp"), QStringLiteral("2026-10-01T08:15:00.000Z")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("model"), QStringLiteral("claude-opus-5-5")},
                     {QStringLiteral("role"), QStringLiteral("assistant")},
                     {QStringLiteral("content"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                             {QStringLiteral("text"), text}}}}}}};
}

static void importTests(const QString &home, const QString &repo)
{
    const QString config = home + QStringLiteral("/.claude");
    const QString key = ClaudeSessionScan::projectKey(repo);
    check(key.size() == repo.size() && key.startsWith(QLatin1Char('-')) &&
              !key.contains(QLatin1Char('/')) && !key.contains(QLatin1Char('.')),
          "a project folder is the cwd with every non-alphanumeric turned into '-'");

    const QString dir = config + QStringLiteral("/projects/") + key;
    writeLines(dir + QStringLiteral("/renamed.jsonl"),
               {userLine(repo, QStringLiteral("fix the login page")),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("ai-title")},
                            {QStringLiteral("aiTitle"), QStringLiteral("Login page fix")}},
                assistantLine(QStringLiteral("Done.")),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("custom-title")},
                            {QStringLiteral("customTitle"), QStringLiteral("My login work")}}});
    writeLines(dir + QStringLiteral("/titled.jsonl"),
               {userLine(repo, QStringLiteral("add dark mode")),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("ai-title")},
                            {QStringLiteral("aiTitle"), QStringLiteral("Dark mode")}}});
    writeLines(dir + QStringLiteral("/untitled.jsonl"),
               {userLine(repo, QStringLiteral("<command-name>/clear</command-name>")),
                userLine(repo, QStringLiteral("\n  please tidy the README and the docs folder, "
                                              "then check every link still works across the "
                                              "whole site\nsecond line"))});
    writeLines(dir + QStringLiteral("/empty.jsonl"),
               {QJsonObject{{QStringLiteral("type"), QStringLiteral("mode")},
                            {QStringLiteral("cwd"), repo}}});
    writeLines(dir + QStringLiteral("/desktop.jsonl"),
               {userLine(repo, QStringLiteral("status update"))});
    writeLines(config + QStringLiteral("/projects/-elsewhere/desktop.jsonl"),
               {userLine(repo, QStringLiteral("status update")),
                assistantLine(QStringLiteral("a longer copy in the wrong folder"))});
    writeLines(config + QStringLiteral("/projects/-other/away.jsonl"),
               {userLine(home + QStringLiteral("/other"), QStringLiteral("other repo"))});
    QFile old(dir + QStringLiteral("/titled.jsonl"));
    old.open(QIODevice::ReadWrite);
    old.setFileTime(QDateTime::currentDateTime().addDays(-30),
                    QFileDevice::FileModificationTime);
    old.close();

    const QString desktop = ClaudeSessionScan::desktopConfigRoot() +
                            QStringLiteral("/claude-code-sessions/acct/org");
    QDir().mkpath(desktop);
    QFile record(desktop + QStringLiteral("/local_abc.json"));
    record.open(QIODevice::WriteOnly);
    record.write(QJsonDocument(QJsonObject{{QStringLiteral("cliSessionId"),
                                            QStringLiteral("desktop")},
                                           {QStringLiteral("title"),
                                            QStringLiteral("[done] Status update")}})
                     .toJson());
    record.close();

    const QList<ExternalClaudeSession> listed =
        ClaudeSessionScan::listAll({config, config}, repo, false);
    check(uuids(listed) == QStringList({QStringLiteral("desktop"), QStringLiteral("renamed"),
                                        QStringLiteral("titled"), QStringLiteral("untitled")}),
          "the import list has every session in the repo, old ones too, once each, "
          "without empty ones or other folders");
    const ExternalClaudeSession *renamed = find(listed, QStringLiteral("renamed"));
    check(renamed && renamed->title == QStringLiteral("My login work"),
          "a /rename title wins over the AI title");
    check(renamed && renamed->model == QStringLiteral("claude-opus-5-5"),
          "the model that answered is read from the transcript");
    check(renamed && renamed->firstPrompt == QStringLiteral("fix the login page"),
          "the first typed prompt is kept");
    const ExternalClaudeSession *titled = find(listed, QStringLiteral("titled"));
    check(titled && titled->title == QStringLiteral("Dark mode"), "the AI title names a session");
    const ExternalClaudeSession *untitled = find(listed, QStringLiteral("untitled"));
    check(untitled && untitled->title.startsWith(QStringLiteral("please tidy the README")) &&
              untitled->title.size() <= 80 && !untitled->title.contains(QLatin1Char('\n')),
          "an untitled session is named after its first real prompt, one short line");
    const ExternalClaudeSession *fromDesktop = find(listed, QStringLiteral("desktop"));
    check(fromDesktop && fromDesktop->title == QStringLiteral("[done] Status update"),
          "Claude Desktop's own name for a session wins");
    const QString agentTree = repo + QStringLiteral("/.worktrees/7-task");
    writeLines(config + QStringLiteral("/projects/") + ClaudeSessionScan::projectKey(agentTree) +
                   QStringLiteral("/agentrun.jsonl"),
               {userLine(agentTree, QStringLiteral("agent task"))});
    check(!uuids(ClaudeSessionScan::listAll({config}, repo, false,
                                            {repo + QStringLiteral("/.worktrees")}))
               .contains(QStringLiteral("agentrun")),
          "sessions in agent worktrees are skipped when asked");
    check(uuids(ClaudeSessionScan::listAll({config}, repo, false)).contains(QStringLiteral("agentrun")),
          "sessions in agent worktrees are listed when not skipped");

    const QList<ExternalClaudeSession> everywhere =
        ClaudeSessionScan::listAll({config}, repo, true);
    check(uuids(everywhere).contains(QStringLiteral("away")),
          "every folder lists sessions outside the repository too");
    check(uuids(everywhere).count(QStringLiteral("desktop")) == 1,
          "a session copied into two project folders is listed once");
    const ExternalClaudeSession *own = find(everywhere, QStringLiteral("desktop"));
    check(own && QFileInfo(own->path).dir().dirName() == key,
          "a session copied into two project folders is read from its own");

    const QString big(20000, QLatin1Char('x'));
    const QList<QJsonObject> lines{
        userLine(repo, QStringLiteral("please fix it")),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("user")},
                    {QStringLiteral("isSidechain"), true},
                    {QStringLiteral("message"),
                     QJsonObject{{QStringLiteral("content"), QStringLiteral("subagent")}}}},
        userLine(repo, QStringLiteral("<local-command-stdout>ok</local-command-stdout>")),
        assistantLine(QStringLiteral("Fixed.")),
        userLine(repo,
                 QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                        {QStringLiteral("tool_use_id"), QStringLiteral("t1")},
                                        {QStringLiteral("content"), big}}}),
        userLine(repo, QStringLiteral("<cross-session-message from=\"x\" from-name=\"ForkMesh "
                                      "(desktop)\">\nYour user typed this in the ForkMesh "
                                      "desktop app, as their next message.\n\nship it\n"
                                      "</cross-session-message>"))};
    const ClaudeImportBatch batch = ClaudeSessionImport::convertLines(lines, QStringLiteral("s1"));
    check(batch.events.size() == 4, "sidechains and command echoes are left out");
    check(batch.events.value(3).value(QStringLiteral("text")).toString() ==
              QStringLiteral("ship it"),
          "a message ForkMesh sent through the peer inbox shows as the user's own words");
    check(batch.events.value(0).value(QStringLiteral("type")).toString() ==
                  QStringLiteral("_local_user") &&
              batch.events.value(0).value(QStringLiteral("text")).toString() ==
                  QStringLiteral("please fix it"),
          "a typed prompt becomes the user's turn");
    check(batch.events.value(1).value(QStringLiteral("session_id")).toString() ==
              QStringLiteral("s1"),
          "assistant turns carry the session id ForkMesh resumes");
    const QString clipped = batch.events.value(2)
                                .value(QStringLiteral("message")).toObject()
                                .value(QStringLiteral("content")).toArray().first().toObject()
                                .value(QStringLiteral("content")).toString();
    check(clipped.size() < 17000 && clipped.contains(QStringLiteral("not imported")),
          "a huge tool result is clipped");
    check(batch.log.contains(QStringLiteral("please fix it")) &&
              batch.log.contains(QStringLiteral("Fixed.")),
          "the readable log keeps both sides of the conversation");
    check(batch.lastMs > 0, "the last activity time is read");

    const QString account = home + QStringLiteral("/account");
    const QString source = dir + QStringLiteral("/renamed.jsonl");
    QString why;
    check(ClaudeSessionImport::stage(source, account, repo, QStringLiteral("renamed"), &why),
          "an imported session is staged for another account");
    const QString staged = ClaudeSessionImport::stagedPath(account, repo, QStringLiteral("renamed"));
    QFile in(source);
    QFile out(staged);
    check(in.open(QIODevice::ReadOnly) && out.open(QIODevice::ReadOnly) &&
              in.readAll() == out.readAll(),
          "the staged transcript is a byte-for-byte copy where that account looks for it");
    check(ClaudeSessionImport::stage(source, config, repo, QStringLiteral("renamed"), &why),
          "staging into the folder the transcript already lives in is a no-op");
    check(!ClaudeSessionImport::stage(source, account, repo, QStringLiteral("../x"), &why),
          "a session id that is not a plain id is refused");
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

    importTests(home.path(), repo);

    if (failures == 0)
        std::cout << "claude session scan tests passed\n";
    return failures == 0 ? 0 : 1;
}
