#include "MachineAgentScan.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>

#if defined(Q_OS_LINUX)
#include <unistd.h>
#endif

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QByteArrayList args(std::initializer_list<const char *> list)
{
    QByteArrayList out;
    for (const char *arg : list)
        out.append(QByteArray(arg));
    return out;
}

static void touch(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write("{}\n");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace MachineAgentScan;

    check(agentKind(QStringLiteral("claude"), args({"claude"})) == QLatin1String("claude"),
          "a process named claude is Claude Code");
    check(agentKind(QStringLiteral("node"),
                    args({"node", "/usr/lib/node_modules/@openai/codex/bin/codex.js"}))
              == QLatin1String("codex"),
          "a node script named codex.js is Codex");
    check(agentKind(QStringLiteral("node"), args({"node", "--no-warnings", "/usr/bin/gemini"}))
              == QLatin1String("gemini"),
          "interpreter flags before the script are skipped");
    check(agentKind(QStringLiteral("python3"), args({"python3", "/home/u/.local/bin/aider"}))
              == QLatin1String("aider"),
          "a python entry point named aider is Aider");
    check(agentKind(QStringLiteral("node"), args({"node", "server.js"})).isEmpty(),
          "an ordinary node script is not an agent");
    check(scriptStub(QStringLiteral("/usr/bin/dash")),
          "a #!/bin/sh script named claude (a test fake) is not an agent");
    check(scriptStub(QStringLiteral("/bin/bash")), "bash scripts are stubs too");
    check(!scriptStub(QStringLiteral("/home/f/.local/share/claude/versions/2.1.300")),
          "the native Claude Code binary is an agent");
    check(!scriptStub(QStringLiteral("/usr/bin/node")), "a node-run agent is not a stub");
    check(!scriptStub(QString()), "an unreadable exe is not treated as a stub");
    check(agentKind(QStringLiteral("bash"), args({"bash", "-c", "claude"})).isEmpty(),
          "a shell that mentions claude is not an agent");

    check(argValue(args({"claude", "--model", "claude-opus-5-5"}), {"--model"})
              == QLatin1String("claude-opus-5-5"),
          "a spaced flag value is read");
    check(argValue(args({"claude", "--resume=abc"}), {"--resume", "-r"}) == QLatin1String("abc"),
          "an equals flag value is read");
    check(argValue(args({"claude", "--resume", "--verbose"}), {"--resume"}).isEmpty(),
          "a bare flag followed by another flag has no value");

    check(claudeProjectKey(QStringLiteral("/home/f/projects/forkmesh/src/.worktrees/312-way"))
              == QLatin1String("-home-f-projects-forkmesh-src--worktrees-312-way"),
          "the transcript folder replaces every non-alphanumeric character");

    QTemporaryDir temp;
    check(temp.isValid(), "temporary folder");
    const QString config = temp.path() + QStringLiteral("/config");
    const QString cwd = QStringLiteral("/work/repo");
    const QString dir = config + QStringLiteral("/projects/") + claudeProjectKey(cwd);
    QDir().mkpath(dir);
    touch(dir + QStringLiteral("/older.jsonl"));
    QThread::msleep(1100);
    touch(dir + QStringLiteral("/newer.jsonl"));

    check(claudeTranscript(config, cwd, QStringLiteral("older"), {})
              == dir + QStringLiteral("/older.jsonl"),
          "a resumed session id picks its own transcript");
    check(claudeTranscript(config, cwd, QString(), {}) == dir + QStringLiteral("/newer.jsonl"),
          "without an id the newest transcript in the folder is used");
    check(claudeTranscript(config, cwd, QString(), {dir + QStringLiteral("/newer.jsonl")})
              == dir + QStringLiteral("/older.jsonl"),
          "a transcript already claimed by another process is skipped");
    check(claudeTranscript(config, QStringLiteral("/elsewhere"), QString(), {}).isEmpty(),
          "a folder with no transcripts has none");

    QDir().mkpath(config + QStringLiteral("/sessions"));
    const auto record = [&config](const QString &name, const QByteArray &body) {
        QFile file(config + QStringLiteral("/sessions/") + name);
        file.open(QIODevice::WriteOnly);
        file.write(body);
    };
    record(QStringLiteral("4242.json"), "{\"pid\":4242,\"sessionId\":\"older\",\"cwd\":\"/work/repo\"}");
    record(QStringLiteral("4343.json"), "{\"pid\":9999,\"sessionId\":\"older\"}");
    record(QStringLiteral("4444.json"), "{\"pid\":4444,\"sessionId\":\"../../etc/passwd\"}");
    check(recordedClaudeSession(config, 4242) == QLatin1String("older"),
          "a fresh session's id is read from Claude's record for its process");
    check(recordedClaudeSession(config, 4343).isEmpty(), "a record naming another process is ignored");
    check(recordedClaudeSession(config, 4444).isEmpty(), "a session id that is not a plain name is ignored");
    check(recordedClaudeSession(config, 4545).isEmpty(), "a process without a record has none");

    SystemStats::ProcessTable table;
    table.entries.insert(10, {1, QStringLiteral("forkmesh"), false, 0});
    table.entries.insert(11, {10, QStringLiteral("claude"), false, 0});
    table.entries.insert(20, {1, QStringLiteral("gnome-terminal"), false, 0});
    table.entries.insert(21, {20, QStringLiteral("bash"), false, 0});
    table.entries.insert(22, {21, QStringLiteral("claude"), false, 0});
    table.entries.insert(23, {22, QStringLiteral("claude"), false, 0});
    table.entries.insert(30, {1, QStringLiteral("node"), false, 0});
    table.entries.insert(31, {30, QStringLiteral("codex"), false, 0});
    table.entries.insert(40, {1, QStringLiteral("claude"), false, 0});
    const QHash<qint64, QString> kinds = {
        {11, QStringLiteral("claude")}, {22, QStringLiteral("claude")},
        {23, QStringLiteral("claude")}, {30, QStringLiteral("codex")},
        {31, QStringLiteral("codex")},  {40, QStringLiteral("claude")},
    };
    check(outsideRoots(table, kinds, {10}) == QList<qint64>({22, 30, 40}),
          "agents under the desktop are its own, nested agents fold into their parent");
    check(outsideRoots(table, kinds, {10, 40}) == QList<qint64>({22, 30}),
          "a ForkMesh run's own process is not counted again");
    {
        SystemStats::ProcessTable phone = table;
        phone.entries.insert(50, {10, QStringLiteral("forkmesh"), false, 0});
        phone.entries.insert(51, {50, QStringLiteral("claude"), false, 0});
        phone.entries.insert(52, {51, QStringLiteral("claude"), false, 0});
        phone.entries.insert(60, {10, QStringLiteral("codex"), false, 0});
        QHash<qint64, QString> phoneKinds = kinds;
        phoneKinds.insert(51, QStringLiteral("claude"));
        phoneKinds.insert(52, QStringLiteral("claude"));
        phoneKinds.insert(60, QStringLiteral("codex"));
        check(outsideRoots(phone, phoneKinds, {10}) == QList<qint64>({22, 30, 40}),
              "a terminal the desktop started is its own without a foreign root");
        check(outsideRoots(phone, phoneKinds, {10}, {50, 60}) == QList<qint64>({22, 30, 40, 51, 60}),
              "an agent under a phone terminal is found, under the wrapper or as the terminal itself");
    }

    check(codexRollout(QStringLiteral("/h/.codex/sessions/2026/09/29/rollout-2026-09-29T10-00-00-abc.jsonl")),
          "a codex rollout file");
    check(!codexRollout(QStringLiteral("/h/.codex/history.jsonl")), "codex history is not a rollout");
#if defined(Q_OS_LINUX)
    check(processUid(QCoreApplication::applicationPid()) == qint64(::getuid()),
          "the desktop's own process belongs to its user");
    check(processUid(1) == 0, "init belongs to root");
    check(processUid(0) == -1, "a process that cannot be read has no user");
    check(userName(0) == QLatin1String("root"), "uid 0 is root");
    check(userName(4000000000LL) == QLatin1String("user 4000000000"),
          "an unknown uid is named by its number");
    if (::getuid() != 0) {
        SystemStats::ProcessTable others;
        others.entries.insert(1, {0, QStringLiteral("claude"), false, 0});
        const QList<MachineAgent> found = scan(others, {}, {});
        check(found.size() == 1 && found.first().otherUser == QLatin1String("root") &&
                  found.first().cwd.isEmpty() && found.first().transcript.isEmpty(),
              "another login's agent is listed with its user and nothing read from it");
    }
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath(QStringLiteral("sessions/2026/09/29"));
        QFile rollout(home.path() + QStringLiteral("/sessions/2026/09/29/rollout-x.jsonl"));
        rollout.open(QIODevice::WriteOnly);
        SystemStats::ProcessTable own;
        check(openCodexRollout(own, QCoreApplication::applicationPid()) ==
                  QFileInfo(rollout.fileName()).canonicalFilePath(),
              "the rollout a codex process holds open is its transcript");
        rollout.close();
        check(openCodexRollout(own, QCoreApplication::applicationPid()).isEmpty(),
              "no open rollout, no transcript");
    }
#endif

    check(label(QStringLiteral("claude")) == QLatin1String("Claude Code"), "claude label");
    check(provider(QStringLiteral("claude")) == QLatin1String("claude-code"),
          "claude maps to the claude-code provider");

    if (failures == 0)
        std::cout << "machine agent scan tests passed\n";
    return failures == 0 ? 0 : 1;
}
